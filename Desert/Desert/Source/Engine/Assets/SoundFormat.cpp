#include <Engine/Assets/SoundAsset.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>

// The `.desound` TEXT, apart from the asset: Parse and Write touch no registry and no AssetBase, so the
// SceneMigrator (which raises Clip paths to Sound GUIDs) compiles this file and not SoundAsset.cpp.
namespace Desert::Assets
{
    namespace
    {
        // SNDW v1 (AUD-CORE): Source, Class, Loading. Class and Loading are written by their enumerator's name
        // (Audio/SoundProperties.hpp), so the file reads as what it means and a typo is refused, not defaulted.
        struct SoundDocument
        {
            Common::Content::TextAssetHeaderSerialized Header;
            std::string                                Source;
            std::string                                Class;
            std::string                                Loading;
        };

        // SNDW v0 (SOUND-ASSET): the header and Source only. Read by Upgrade alone.
        struct SoundDocumentV0
        {
            Common::Content::TextAssetHeaderSerialized Header;
            std::string                                Source;
        };

        std::span<const Common::Content::SubsystemVersion> SoundTextSubsystems()
        {
            static const std::array<Common::Content::SubsystemVersion, 1> versions = {
                 Common::Content::SubsystemVersion{ kSoundSchemaTag, kSoundSchemaVersion } };
            return versions;
        }

        // The header (kind, GUID) of any generation, and the SNDW version it states (0 = none, v0).
        Common::ResultStr<std::pair<Common::Content::TextAssetHeaderSerialized, uint32_t>>
        ReadSoundHeader( std::string_view text )
        {
            using Result       = std::pair<Common::Content::TextAssetHeaderSerialized, uint32_t>;
            const auto members = Common::Json::ObjectMembers( text );
            if ( !members )
                return Common::MakeFormattedError<Result>( "not a .desound document: {}", members.GetError() );
            // Json::Read refuses a key its struct does not declare, so the header is read on its own: which
            // generation's body follows is what it says.
            std::optional<Common::Content::TextAssetHeaderSerialized> stated;
            for ( const auto& [name, value] : members.GetValue() )
            {
                if ( name != Common::Content::kTextHeaderMember )
                    continue;
                auto read = Common::Json::Read<Common::Content::TextAssetHeaderSerialized>( value );
                if ( !read )
                    return Common::MakeFormattedError<Result>( "header: {}", read.GetError() );
                stated = read.ExtractValue();
            }
            if ( !stated )
                return Common::MakeError<Result>( "the document states no Header; a .desound states one" );
            const Common::Content::TextAssetHeaderSerialized& header = *stated;
            if ( header.Kind != kSoundKind )
                return Common::MakeFormattedError<Result>( "header states Kind '{}', a .desound states '{}'",
                                                           header.Kind, kSoundKind );
            const auto guid = Common::Content::AssetGuidFromText( header.Guid );
            if ( !guid )
                return Common::MakeFormattedError<Result>( "header GUID: {}", guid.GetError() );
            if ( guid.GetValue().IsNull() )
                return Common::MakeError<Result>( "header GUID is null; the null GUID names no sound" );
            return Common::MakeSuccess(
                 Result{ header, Common::Content::TextHeaderVersion( header, kSoundSchemaTag ).value_or( 0 ) } );
        }
    } // namespace

    Common::ResultStr<SoundAsset::Parsed> SoundAsset::Parse( std::string_view text )
    {
        const auto header = ReadSoundHeader( text );
        if ( !header )
            return Common::MakeError<Parsed>( header.GetError() );
        if ( header.GetValue().second == 0 )
            return Common::MakeFormattedError<Parsed>(
                 "SNDW v0 states no Class and no Loading; this build reads SNDW v{}: run SceneMigrator "
                 "(scripts/Dev/migrate.sh --write) over it once",
                 kSoundSchemaVersion );
        const auto document = Common::Json::Read<SoundDocument>( text );
        if ( !document )
            return Common::MakeFormattedError<Parsed>( "not a .desound document: {}", document.GetError() );
        const SoundDocument& data = document.GetValue();
        if ( auto checked = CheckStatedHeader( std::optional( data.Header ), Common::Content::ContentKind::Sound,
                                               kSoundSchemaTag, static_cast<int>( kSoundSchemaVersion ),
                                               SoundTextSubsystems() );
             !checked )
            return Common::MakeFormattedError<Parsed>( "sound {}", checked.GetError() );
        if ( !data.Header.Dependencies.empty() )
            return Common::MakeFormattedError<Parsed>(
                 "the header states {} Dependencies; a sound references no asset",
                 data.Header.Dependencies.size() );
        if ( data.Source.empty() )
            return Common::MakeError<Parsed>(
                 "Source is empty; a sound names the audio file it was imported from" );
        const auto soundClass = Audio::SoundClassNamed( data.Class );
        if ( !soundClass )
            return Common::MakeFormattedError<Parsed>( "Class '{}' names no sound class (Music, Effects, Voice)",
                                                       data.Class );
        const auto loading = Audio::SoundLoadingNamed( data.Loading );
        if ( !loading )
            return Common::MakeFormattedError<Parsed>( "Loading '{}' names no loading (Resident, Streamed)",
                                                       data.Loading );
        const auto guid = Common::Content::AssetGuidFromText( data.Header.Guid );
        return Common::MakeSuccess( Parsed{ guid.GetValue(), data.Source, *soundClass, *loading } );
    }

    Common::ResultStr<std::optional<std::string>> SoundAsset::Upgrade( std::string_view text )
    {
        using Result      = std::optional<std::string>;
        const auto header = ReadSoundHeader( text );
        if ( !header )
            return Common::MakeError<Result>( header.GetError() );
        const uint32_t stated = header.GetValue().second;
        if ( stated == kSoundSchemaVersion )
        {
            if ( const auto current = Parse( text ); !current )
                return Common::MakeError<Result>( current.GetError() );
            return Common::MakeSuccess( Result{} );
        }
        if ( stated != 0 )
            return Common::MakeFormattedError<Result>(
                 "SNDW v{} is not a version this build knows (it writes v{})", stated, kSoundSchemaVersion );
        const auto document = Common::Json::Read<SoundDocumentV0>( text );
        if ( !document )
            return Common::MakeFormattedError<Result>( "not a SNDW v0 .desound: {}", document.GetError() );
        const auto guid = Common::Content::AssetGuidFromText( document.GetValue().Header.Guid );
        // v0 named a file and nothing else: every voice of it played in Effects, decoded whole on play.
        auto raised = Write( guid.GetValue(), document.GetValue().Source, Audio::SoundClass::Effects,
                             Audio::SoundLoading::Resident );
        if ( !raised )
            return Common::MakeError<Result>( raised.GetError() );
        return Common::MakeSuccess( Result{ raised.ExtractValue() } );
    }

    Common::ResultStr<std::string> SoundAsset::Write( const Common::Content::AssetGuid& guid,
                                                      std::string_view source, Audio::SoundClass soundClass,
                                                      Audio::SoundLoading loading )
    {
        if ( guid.IsNull() )
            return Common::MakeError<std::string>( "a .desound needs a GUID; the null GUID names no sound" );
        if ( source.empty() )
            return Common::MakeError<std::string>( "a .desound names its source audio file; none was given" );
        SoundDocument data;
        data.Header =
             Common::Content::MakeTextHeader( Common::Content::ContentKind::Sound, guid, SoundTextSubsystems() );
        data.Source  = std::string( source );
        data.Class   = std::string( Audio::SoundClassName( soundClass ) );
        data.Loading = std::string( Audio::SoundLoadingName( loading ) );
        return Common::Content::CanonicalJsonTextOfWriterOutput( Common::Json::Write( data ) );
    }
} // namespace Desert::Assets
