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

    Common::ResultStr<Common::Content::AssetGuid> EnsureImportRecord( const std::filesystem::path& source )
    {
        std::error_code ec;
        if ( std::filesystem::is_regular_file( Common::Content::ImportRecordPathFor( source ), ec ) )
            return ReadImportRecordGuid( source );
        ImportRecordData data;
        data.Source                        = source.filename().string(); // no header: the stamp mints the GUID
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        if ( auto written = Common::Content::WriteCanonicalJsonFileAtomic( record, WriteImportRecord( data ) );
             !written )
            return Common::MakeFormattedError<Common::Content::AssetGuid>( "'{}' could not be written: {}",
                                                                           record.string(), written.GetError() );
        return ReadImportRecordGuid( source );
    }
} // namespace Desert::Assets::Serialization
