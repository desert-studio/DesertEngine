#include <Engine/Assets/SoundAsset.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

// The `.desound` TEXT, apart from the asset: Parse and Write touch no registry and no AssetBase, so the
// SceneMigrator (which raises Clip paths to Sound GUIDs) compiles this file and not SoundAsset.cpp.
namespace Desert::Assets
{
    namespace
    {
        struct SoundDocument
        {
            Common::Content::TextAssetHeaderSerialized Header;
            std::string                                Source;
        };
    } // namespace

    Common::ResultStr<SoundAsset::Parsed> SoundAsset::Parse( std::string_view text )
    {
        const auto document = Common::Json::Read<SoundDocument>( text );
        if ( !document )
            return Common::MakeFormattedError<Parsed>( "not a .desound document: {}", document.GetError() );
        const SoundDocument& data = document.GetValue();
        if ( data.Header.Kind != kSoundKind )
            return Common::MakeFormattedError<Parsed>( "header states Kind '{}', a .desound states '{}'",
                                                       data.Header.Kind, kSoundKind );
        const auto guid = Common::Content::AssetGuidFromText( data.Header.Guid );
        if ( !guid )
            return Common::MakeFormattedError<Parsed>( "header GUID: {}", guid.GetError() );
        if ( guid.GetValue().IsNull() )
            return Common::MakeError<Parsed>( "header GUID is null; the null GUID names no sound" );
        if ( data.Source.empty() )
            return Common::MakeError<Parsed>( "Source is empty; a sound names the audio file it was imported from" );
        return Common::MakeSuccess( Parsed{ guid.GetValue(), data.Source } );
    }

    Common::ResultStr<std::string> SoundAsset::Write( const Common::Content::AssetGuid& guid, std::string_view source )
    {
        if ( guid.IsNull() )
            return Common::MakeError<std::string>( "a .desound needs a GUID; the null GUID names no sound" );
        if ( source.empty() )
            return Common::MakeError<std::string>( "a .desound names its source audio file; none was given" );
        SoundDocument data;
        data.Header = Common::Content::MakeTextHeader( Common::Content::ContentKind::Sound, guid, {} );
        data.Source = std::string( source );
        return Common::Content::CanonicalJsonTextOfWriterOutput( Common::Json::Write( data ) );
    }
} // namespace Desert::Assets
