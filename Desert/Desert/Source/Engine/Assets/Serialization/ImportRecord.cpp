#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cmath>

namespace Desert::Assets::Serialization
{
    SourceImportSettingsText ImportSettingsToText( const Assets::SourceImportSettings& settings )
    {
        return { settings.CombineMeshes, settings.Mesh.UniformScale,
                 std::string( Assets::MeshSourceUpAxisName( settings.Mesh.UpAxis ) ),
                 std::string( Assets::MeshLodPolicyName( settings.Mesh.LodPolicy ) ) };
    }

    Common::ResultStr<Assets::SourceImportSettings> ImportSettingsFromText( const SourceImportSettingsText& text )
    {
        using Result    = Assets::SourceImportSettings;
        const auto axis = Assets::MeshSourceUpAxisFromName( text.UpAxis );
        const auto lods = Assets::MeshLodPolicyFromName( text.LodPolicy );
        if ( !axis || !lods )
            return Common::MakeFormattedError<Result>( "import settings name up axis '{}' and LOD policy '{}'; "
                                                       "one is unknown to this build",
                                                       text.UpAxis, text.LodPolicy );
        if ( !std::isfinite( text.UniformScale ) || text.UniformScale <= 0.0f )
            return Common::MakeFormattedError<Result>(
                 "import settings state scale {}, not a finite positive number", text.UniformScale );
        Result out;
        out.CombineMeshes     = text.CombineMeshes;
        out.Mesh.UniformScale = text.UniformScale;
        out.Mesh.UpAxis       = *axis;
        out.Mesh.LodPolicy    = *lods;
        return Common::MakeSuccess( out );
    }

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

    Common::ResultStr<Assets::SourceImportSettings> ReadImportRecordSettings( const std::filesystem::path& source )
    {
        using Result = Assets::SourceImportSettings;
        auto data    = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<Result>( data.GetError() );
        if ( !data.GetValue() || !data.GetValue()->Settings )
            return Common::MakeSuccess(
                 Result{} ); // the first import, or a record from before THM1l: UE's defaults
        auto settings = ImportSettingsFromText( *data.GetValue()->Settings );
        if ( !settings )
            return Common::MakeFormattedError<Result>(
                 "'{}': {}", Common::Content::ImportRecordPathFor( source ).string(), settings.GetError() );
        return settings;
    }

    Common::ResultStr<Common::Content::AssetGuid>
    EnsureImportRecord( const std::filesystem::path& source, const std::optional<Common::Math::AABB>& bounds,
                        const Assets::SourceImportSettings& settings )
    {
        using Common::Content::AssetGuid;
        const std::filesystem::path          record = Common::Content::ImportRecordPathFor( source );
        std::optional<ImportRecordData::Box> box;
        if ( bounds )
            box = ImportRecordData::Box{ { bounds->Min.x, bounds->Min.y, bounds->Min.z },
                                         { bounds->Max.x, bounds->Max.y, bounds->Max.z } };
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
            bool sameSettings = false;
            if ( data.Settings )
                if ( const auto stated = ImportSettingsFromText( *data.Settings ) )
                    sameSettings = stated.GetValue() == settings;
            // No box (a file with no mesh: a skeleton and its clips) leaves the stated box as it is.
            const bool sameBox =
                 !box || ( data.Bounds && data.Bounds->Min == box->Min && data.Bounds->Max == box->Max );
            if ( sameBox && sameSettings )
                return ReadImportRecordGuid( source );
        }
        else
            data.Source = source.filename().string(); // no header: the stamp mints the GUID
        if ( box )
            data.Bounds = box;
        data.Settings = ImportSettingsToText( settings );
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
} // namespace Desert::Assets::Serialization
