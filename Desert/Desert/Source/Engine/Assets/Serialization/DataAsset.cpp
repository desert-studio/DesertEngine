#include <Engine/Assets/Serialization/DataAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/ReadWrite.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

namespace Desert::Assets::Serialization
{
    Reflection::AssetResolver DataAssetReferenceResolver()
    {
        // All four members assigned, as the AssetResolver invariant asks (a resolver that exists is whole).
        Reflection::AssetResolver r;
        r.ToPath = []( uint64_t handle, const std::string& /*assetType*/ ) -> std::string
        { return ContentRegistry::KeyForHandle( handle ); };
        r.ToGuid = []( uint64_t handle, const std::string& /*assetType*/ ) -> std::string
        {
            const auto guid = ContentRegistry::GuidForHandle( handle );
            return guid ? Common::Content::AssetGuidToText( *guid ) : std::string{};
        };
        // ResolveGuidRef hands over HandleForGuid of the stated GUID: known when the registry has a row for it.
        r.FromGuid = []( uint64_t expected, const std::string& /*assetType*/ ) -> uint64_t
        { return ContentRegistry::GuidForHandle( expected ).has_value() ? expected : 0; };
        r.FromPath = []( const std::string& key, const std::string& /*assetType*/ ) -> uint64_t
        { return ContentRegistry::HandleForKey( key ); };
        return r;
    }

    std::vector<std::string> DataAssetReferencedGuids( const Reflection::TypeInfo& type, const void* payload,
                                                       std::vector<uint64_t>& unnamed )
    {
        std::vector<std::string> guids;
        if ( payload == nullptr )
            return guids;
        auto take = [&]( const uint64_t handle )
        {
            if ( handle == 0 )
                return;
            if ( const auto guid = ContentRegistry::GuidForHandle( handle ) )
                guids.push_back( Common::Content::AssetGuidToText( *guid ) );
            else
                unnamed.push_back( handle );
        };

        // The same explicit stack SerializeReflected walks nested structs with, so a reference two structs
        // deep is a dependency exactly when it is written.
        struct Frame
        {
            const Reflection::TypeInfo* Type;
            const std::byte*            Base;
        };
        std::vector<Frame> stack{ { &type, static_cast<const std::byte*>( payload ) } };
        while ( !stack.empty() )
        {
            const Frame top = stack.back();
            stack.pop_back();
            for ( const Reflection::FieldInfo& field : top.Type->Fields )
            {
                const void* p = top.Base + field.Offset;
                if ( field.IsContainer )
                {
                    if ( field.ContainerHandles )
                        for ( const uint64_t handle : field.ContainerHandles( p ) )
                            take( handle );
                    // A vector of structs (a weapon's fire modes, each naming its muzzle flash): every element
                    // walked as a nested struct, so a reference inside a row is a dependency as it is written.
                    if ( field.ElementStruct != nullptr && field.ContainerSize && field.ContainerElementAt )
                        for ( std::size_t i = 0, n = field.ContainerSize( p ); i < n; ++i )
                            if ( const void* element = field.ContainerElementAt( p, i ) )
                                stack.push_back( { field.ElementStruct, static_cast<const std::byte*>( element ) } );
                    continue;
                }
                if ( field.Type == Reflection::FieldType::Struct && field.StructType != nullptr )
                    stack.push_back( { field.StructType, static_cast<const std::byte*>( p ) } );
                else if ( field.Type == Reflection::FieldType::AssetHandle )
                    take( *static_cast<const uint64_t*>( p ) );
            }
        }
        std::sort( guids.begin(), guids.end() );
        guids.erase( std::unique( guids.begin(), guids.end() ), guids.end() );
        return guids;
    }

    Common::ResultStr<DataAssetContents> ParseDataAsset( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<DataAssetContents>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kDataAssetVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<DataAssetContents>( "data asset {}", headed.GetError() );

        auto parsed = Common::Json::Read<DataAssetFileData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<DataAssetContents>( "{}", parsed.GetError() );
        DataAssetFileData file = parsed.ExtractValue();

        if ( auto header = Assets::CheckStatedHeader( file.Header, Common::Content::ContentKind::DataAsset,
                                                      Assets::kDataAssetSchemaTag, kDataAssetVersion,
                                                      DataAssetTextSubsystems() );
             !header )
            return Common::MakeFormattedError<DataAssetContents>( "data asset {}", header.GetError() );

        if ( file.Type.empty() )
            return Common::MakeFormattedError<DataAssetContents>( "the data asset states no Type" );
        const DataAssetType* type = DataAssetTypeRegistry::Get().Find( file.Type );
        if ( type == nullptr )
            return Common::MakeFormattedError<DataAssetContents>(
                 "the data asset holds a '{}', which no loaded module registers "
                 "(DataAssetTypeRegistry::Register<T>( \"{}\" ) at the module's init)",
                 file.Type, file.Type );
        auto reflected = DataAssetTypeRegistry::ReflectedTypeOf( *type );
        if ( !reflected )
            return Common::MakeFormattedError<DataAssetContents>( "{}", reflected.GetError() );

        DataAssetContents contents;
        // THE PAYLOAD'S SCHEMA (UE: a UObject's custom version, upgraded in Serialize): a newer file than this
        // build's struct is refused (its meaning is unknown here); an older one is walked up one step at a time.
        const uint32_t stated = file.TypeVersion.value_or( 1u );
        if ( stated > type->Version )
            return Common::MakeFormattedError<DataAssetContents>(
                 "the '{}' data asset was written at schema version {}, newer than this build's {}", file.Type,
                 stated, type->Version );
        if ( stated == 0 )
            return Common::MakeFormattedError<DataAssetContents>( "the '{}' data asset states TypeVersion 0",
                                                                  file.Type );
        for ( uint32_t from = stated; from < type->Version; ++from )
        {
            if ( type->Migrate == nullptr )
                return Common::MakeFormattedError<DataAssetContents>(
                     "the '{}' data asset is schema version {}, this build's is {}, and the type registers no "
                     "migrator",
                     file.Type, stated, type->Version );
            if ( auto step = type->Migrate( from, file.Data ); !step )
                return Common::MakeFormattedError<DataAssetContents>( "migrating the '{}' data asset from {} to {}: {}",
                                                                      file.Type, from, from + 1, step.GetError() );
        }

        contents.Header  = std::move( file.Header );
        contents.Type    = type;
        contents.Payload = type->Make();

        const Common::Json::Value data( std::move( file.Data ) );
        Common::Json::Issues            issues;
        const Reflection::AssetResolver resolver = DataAssetReferenceResolver();
        Reflection::DeserializeReflected( *reflected.GetValue(), contents.Payload.get(),
                                          Common::Json::Root( data ), issues, &resolver );
        if ( !issues.empty() )
            return Common::MakeFormattedError<DataAssetContents>( "'{}' Data: {}", file.Type,
                                                                  Common::Json::ReportIssues( issues, text ) );
        return Common::MakeSuccess( std::move( contents ) );
    }

    Common::ResultStr<std::string>
    WriteDataAsset( const std::optional<Common::Content::TextAssetHeaderSerialized>& header,
                    const DataAssetType& type, const void* payload )
    {
        if ( payload == nullptr )
            return Common::MakeFormattedError<std::string>( "a '{}' data asset with no payload", type.Name );
        auto reflected = DataAssetTypeRegistry::ReflectedTypeOf( type );
        if ( !reflected )
            return Common::MakeFormattedError<std::string>( "{}", reflected.GetError() );

        DataAssetFileData out;
        out.Header =
             Assets::StampTextHeader( header, Common::Content::ContentKind::DataAsset, DataAssetTextSubsystems() );
        // EVERY OUTGOING REFERENCE IN THE HEADER, as a material states its textures (StampMaterialHeader): the
        // registry's Closure and the chunk plan read these edges, so a packaged game carries the mesh a
        // WeaponDef names. A set handle the registry cannot name would be written empty - refused instead, so
        // the reference is never lost silently.
        std::vector<uint64_t> unnamed;
        out.Header->Dependencies = DataAssetReferencedGuids( *reflected.GetValue(), payload, unnamed );
        if ( !unnamed.empty() )
            return Common::MakeFormattedError<std::string>(
                 "the '{}' payload references {} asset(s) the content registry does not name (first handle {}); "
                 "saving would write the reference empty - restart the editor so the registry gathers the file",
                 type.Name, unnamed.size(), unnamed.front() );
        out.Type        = type.Name;
        out.TypeVersion = type.Version;
        const Reflection::AssetResolver resolver = DataAssetReferenceResolver();
        out.Data = Reflection::SerializeReflected( *reflected.GetValue(), payload, &resolver );
        return Common::MakeSuccess( Common::Json::Write( out ) );
    }

    Common::BoolResultStr
    SaveDataAssetFile( const std::filesystem::path&                                     path,
                       const std::optional<Common::Content::TextAssetHeaderSerialized>& header,
                       const DataAssetType& type, const void* payload )
    {
        auto text = WriteDataAsset( header, type, payload );
        if ( !text )
            return Common::MakeFormattedError<bool>( "refusing to write data asset '{}': {}", path.string(),
                                                     text.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for a data asset: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, text.GetValue() );
    }
} // namespace Desert::Assets::Serialization
