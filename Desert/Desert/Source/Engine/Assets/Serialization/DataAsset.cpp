#include <Engine/Assets/Serialization/DataAsset.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/ReadWrite.hpp>

#include <string>
#include <system_error>

namespace Desert::Assets::Serialization
{
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
        contents.Header  = std::move( file.Header );
        contents.Type    = type;
        contents.Payload = type->Make();

        const Common::Json::Value data( std::move( file.Data ) );
        Common::Json::Issues      issues;
        Reflection::DeserializeReflected( *reflected.GetValue(), contents.Payload.get(),
                                          Common::Json::Root( data ), issues );
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
        out.Type = type.Name;
        out.Data = Reflection::SerializeReflected( *reflected.GetValue(), payload );
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
