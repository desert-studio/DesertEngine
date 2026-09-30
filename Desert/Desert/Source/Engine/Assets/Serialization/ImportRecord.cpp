#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets::Serialization
{
    Common::ResultStr<ImportRecordData> ParseImportRecord( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<ImportRecordData>( "the file is empty" );
        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kImportRecordVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<ImportRecordData>( "import record {}", headed.GetError() );
        const auto parsed = Common::Json::Read<ImportRecordData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<ImportRecordData>( "{}", parsed.GetError() );
        ImportRecordData data = parsed.GetValue();
        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::StaticMesh,
                                                      Assets::kImportRecordSchemaTag, kImportRecordVersion,
                                                      ImportRecordTextSubsystems() );
             !header )
            return Common::MakeFormattedError<ImportRecordData>( "import record {}", header.GetError() );
        if ( data.Source.empty() )
            return Common::MakeFormattedError<ImportRecordData>( "import record names no Source" );
        if ( !data.Bounds )
            return Common::MakeFormattedError<ImportRecordData>( "import record states no Bounds" );
        if ( data.Thumbnail )
        {
            if ( data.Thumbnail->empty() )
                return Common::MakeFormattedError<ImportRecordData>(
                     "import record states an empty Thumbnail: no orbit for any mesh is written as no key" );
            for ( const auto& [mesh, stated] : *data.Thumbnail )
            {
                const ThumbnailOrbit orbit = Resolve( stated );
                if ( !IsValidThumbnailOrbit( orbit ) )
                    return Common::MakeFormattedError<ImportRecordData>(
                         "import record: the Thumbnail orbit of '{}' is not finite or zooms to -1 or in", mesh );
                if ( orbit == ThumbnailOrbit{} )
                    return Common::MakeFormattedError<ImportRecordData>(
                         "import record states the default Thumbnail orbit for '{}': the default is written as no "
                         "entry",
                         mesh );
            }
        }
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteImportRecord( const ImportRecordData& data )
    {
        ImportRecordData out = data;
        out.Header           = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::StaticMesh,
                                                        ImportRecordTextSubsystems() );
        return Common::Json::Write( out );
    }

    Common::ResultStr<Common::Content::AssetGuid> ReadImportRecordGuid( const std::filesystem::path& source )
    {
        using Common::Content::AssetGuid;
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        std::error_code             ec;
        if ( !std::filesystem::is_regular_file( record, ec ) )
            return Common::MakeFormattedError<AssetGuid>(
                 "'{}' has no import record '{}': its mesh has no identity until it is imported (the import "
                 "writes the record)",
                 source.string(), record.string() );
        const auto text = Common::Utils::FileSystem::ReadFileContent( record );
        if ( !text )
            return Common::MakeFormattedError<AssetGuid>( "'{}': {}", record.string(), text.GetError() );
        const auto data = ParseImportRecord( text.GetValue() );
        if ( !data )
            return Common::MakeFormattedError<AssetGuid>( "'{}': {}", record.string(), data.GetError() );
        if ( data.GetValue().Source != source.filename().string() )
            return Common::MakeFormattedError<AssetGuid>( "'{}' is the record of '{}', not of '{}'",
                                                          record.string(), data.GetValue().Source,
                                                          source.filename().string() );
        const auto guid = Common::Content::AssetGuidFromText( data.GetValue().Header->Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeFormattedError<AssetGuid>( "'{}' states no usable GUID", record.string() );
        return guid;
    }

    Common::ResultStr<bool> ReadImportRecordCombineMeshes( const std::filesystem::path& source )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        std::error_code             ec;
        if ( !std::filesystem::is_regular_file( record, ec ) )
            return Common::MakeSuccess( false ); // the first import: UE's default
        const auto text = Common::Utils::FileSystem::ReadFileContent( record );
        if ( !text )
            return Common::MakeFormattedError<bool>( "'{}': {}", record.string(), text.GetError() );
        const auto data = ParseImportRecord( text.GetValue() );
        if ( !data )
            return Common::MakeFormattedError<bool>( "'{}': {}", record.string(), data.GetError() );
        return Common::MakeSuccess( data.GetValue().CombineMeshes.value_or( false ) );
    }

    Common::ResultStr<Common::Content::AssetGuid> EnsureImportRecord( const std::filesystem::path& source,
                                                                      const Common::Math::AABB&    bounds )
    {
        using Common::Content::AssetGuid;
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        const ImportRecordData::Box box{ { bounds.Min.x, bounds.Min.y, bounds.Min.z },
                                         { bounds.Max.x, bounds.Max.y, bounds.Max.z } };
        ImportRecordData            data;
        std::error_code             ec;
        if ( std::filesystem::is_regular_file( record, ec ) )
        {
            // The identity is read through the one reader, so a record of another generation or another
            // source is refused here exactly as everywhere else.
            if ( auto guid = ReadImportRecordGuid( source ); !guid )
                return guid;
            const auto text = Common::Utils::FileSystem::ReadFileContent( record );
            if ( !text )
                return Common::MakeFormattedError<AssetGuid>( "'{}': {}", record.string(), text.GetError() );
            auto parsed = ParseImportRecord( text.GetValue() );
            if ( !parsed )
                return Common::MakeFormattedError<AssetGuid>( "'{}': {}", record.string(), parsed.GetError() );
            data = parsed.ExtractValue();
            if ( data.Bounds && data.Bounds->Min == box.Min && data.Bounds->Max == box.Max )
                return ReadImportRecordGuid( source );
        }
        else
            data.Source = source.filename().string(); // no header: the stamp mints the GUID
        data.Bounds = box;
        if ( auto written = Common::Content::WriteCanonicalJsonFileAtomic( record, WriteImportRecord( data ) );
             !written )
            return Common::MakeFormattedError<AssetGuid>( "'{}' could not be written: {}", record.string(),
                                                          written.GetError() );
        return ReadImportRecordGuid( source );
    }

    Common::ResultStr<std::optional<ImportRecordData>> ReadImportRecord( const std::filesystem::path& source )
    {
        using Result                       = std::optional<ImportRecordData>;
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        std::error_code             ec;
        if ( !std::filesystem::is_regular_file( record, ec ) )
            return Common::MakeSuccess( Result{} );
        const auto text = Common::Utils::FileSystem::ReadFileContent( record );
        if ( !text )
            return Common::MakeFormattedError<Result>( "'{}': {}", record.string(), text.GetError() );
        auto parsed = ParseImportRecord( text.GetValue() );
        if ( !parsed )
            return Common::MakeFormattedError<Result>( "'{}': {}", record.string(), parsed.GetError() );
        return Common::MakeSuccess( Result{ parsed.ExtractValue() } );
    }

    Common::ResultStr<ThumbnailOrbit> ReadImportRecordThumbnail( const std::filesystem::path& source,
                                                                 const std::string&           meshFile )
    {
        const auto data = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<ThumbnailOrbit>( data.GetError() );
        if ( !data.GetValue() )
            return Common::MakeFormattedError<ThumbnailOrbit>(
                 "'{}' has no import record, so the orbit of '{}' has no home", source.string(), meshFile );
        const auto& thumbnail = data.GetValue()->Thumbnail;
        if ( !thumbnail )
            return Common::MakeSuccess( ThumbnailOrbit{} );
        const auto it = thumbnail->find( meshFile );
        return Common::MakeSuccess( it == thumbnail->end() ? ThumbnailOrbit{} : Resolve( it->second ) );
    }

    Common::BoolResultStr SetImportRecordNodes( const std::filesystem::path&                   source,
                                                const std::optional<std::vector<std::string>>& nodes )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        auto                        data   = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        if ( !data.GetValue() )
            return Common::MakeFormattedError<bool>( "'{}' does not exist, so its node meshes cannot be recorded",
                                                     record.string() );
        ImportRecordData out = *data.GetValue();
        if ( out.Nodes == nodes )
            return BOOLSUCCESS;
        out.Nodes = nodes;
        if ( auto written = Common::Content::WriteCanonicalJsonFileAtomic( record, WriteImportRecord( out ) );
             !written )
            return Common::MakeFormattedError<bool>( "'{}' could not be written: {}", record.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }
    Common::BoolResultStr SetImportRecordThumbnail( const std::filesystem::path& source,
                                                    const std::string& meshFile, const ThumbnailOrbit& orbit )
    {
        if ( !IsValidThumbnailOrbit( orbit ) )
            return Common::MakeFormattedError<bool>(
                 "the Thumbnail orbit for '{}' is not finite or zooms to -1 or in", meshFile );
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        auto                        data   = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        if ( !data.GetValue() )
            return Common::MakeFormattedError<bool>( "'{}' does not exist, so the orbit of '{}' has no home",
                                                     record.string(), meshFile );
        ImportRecordData                            out = *data.GetValue();
        std::map<std::string, ThumbnailOrbitRecord> entries =
             out.Thumbnail.value_or( std::map<std::string, ThumbnailOrbitRecord>{} );
        if ( orbit == ThumbnailOrbit{} )
            entries.erase( meshFile );
        else
            entries[meshFile] = ToRecord( orbit );
        std::optional<std::map<std::string, ThumbnailOrbitRecord>> next;
        if ( !entries.empty() )
            next = std::move( entries );
        if ( out.Thumbnail == next )
            return BOOLSUCCESS;
        out.Thumbnail = std::move( next );
        if ( auto written = Common::Content::WriteCanonicalJsonFileAtomic( record, WriteImportRecord( out ) );
             !written )
            return Common::MakeFormattedError<bool>( "'{}' could not be written: {}", record.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets::Serialization
