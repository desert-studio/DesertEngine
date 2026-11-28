#include <Engine/Assets/SoundAsset.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetGuid.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <Common/Json/ReadWrite.hpp>

// The `.desound` TEXT, apart from the asset: Parse and Write touch no registry and no AssetBase, so the
// SceneMigrator (which raises Clip paths to Sound GUIDs and SNDW generations) compiles this file and not
// SoundAsset.cpp.
namespace Desert::Assets
{
    namespace
    {
        // SNDW v2 (AUDIO-SOUNDCLASS): Source, Class, Loading. Class is the GUID text of the sound class asset,
        // stated again as the header's one Dependency; Loading is its enumerator's name
        // (Audio/SoundProperties.hpp), so a typo is refused, not defaulted.
        struct SoundDocument
        {
            Common::Content::TextAssetHeaderSerialized Header;
            std::string                                Source;
            std::string                                Class;
            std::string                                Loading;
        };

        // SNDW v1 (AUD-CORE): the same members, Class an enumerator name (Music / Effects / Voice) and no
        // Dependencies. Read by Upgrade alone.
        struct SoundDocumentV1
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

        // The class GUID a v2 Class states: well-formed and not null.
        Common::ResultStr<Audio::SoundClassId> ReadClassGuid( std::string_view text )
        {
            const auto guid = Common::Content::AssetGuidFromText( text );
            if ( !guid )
                return Common::MakeFormattedError<Audio::SoundClassId>(
                     "Class '{}' is not the GUID of a sound class asset: {}", text, guid.GetError() );
            if ( guid.GetValue().IsNull() )
                return Common::MakeError<Audio::SoundClassId>(
                     "Class is the null GUID; a sound plays in the class asset it names" );
            return Common::MakeSuccess( guid.GetValue() );
        }
    } // namespace

    Common::ResultStr<SoundAsset::Parsed> SoundAsset::Parse( std::string_view text )
    {
        const auto header = ReadSoundHeader( text );
        if ( !header )
            return Common::MakeError<Parsed>( header.GetError() );
        if ( const uint32_t stated = header.GetValue().second; stated < kSoundSchemaVersion )
            return Common::MakeFormattedError<Parsed>(
                 "SNDW v{} names its Class {}; this build reads SNDW v{} (Class = a sound class asset's GUID): "
                 "run SceneMigrator (scripts/Dev/migrate.sh --write) over it once",
                 stated, stated == 0 ? "nowhere" : "by an enumerator name", kSoundSchemaVersion );
        const auto document = Common::Json::Read<SoundDocument>( text );
        if ( !document )
            return Common::MakeFormattedError<Parsed>( "not a .desound document: {}", document.GetError() );
        const SoundDocument& data = document.GetValue();
        if ( auto checked = CheckStatedHeader( std::optional( data.Header ), Common::Content::ContentKind::Sound,
                                               kSoundSchemaTag, static_cast<int>( kSoundSchemaVersion ),
                                               SoundTextSubsystems() );
             !checked )
            return Common::MakeFormattedError<Parsed>( "sound {}", checked.GetError() );
        if ( data.Source.empty() )
            return Common::MakeError<Parsed>(
                 "Source is empty; a sound names the audio file it was imported from" );
        const auto soundClass = ReadClassGuid( data.Class );
        if ( !soundClass )
            return Common::MakeError<Parsed>( soundClass.GetError() );
        // The class asset is the sound's one reference: the header states it, and only it, so the content
        // graph (cook, redirectors, reference viewer) sees what every voice of it needs.
        if ( data.Header.Dependencies.size() != 1 )
            return Common::MakeFormattedError<Parsed>(
                 "the header states {} Dependencies; a sound states exactly one, its Class {}",
                 data.Header.Dependencies.size(), data.Class );
        const auto dependency = Common::Content::AssetGuidFromText( data.Header.Dependencies.front() );
        if ( !dependency || dependency.GetValue() != soundClass.GetValue() )
            return Common::MakeFormattedError<Parsed>(
                 "the header's Dependency '{}' is not the Class {}; a sound depends on its class asset alone",
                 data.Header.Dependencies.front(), data.Class );
        const auto loading = Audio::SoundLoadingNamed( data.Loading );
        if ( !loading )
            return Common::MakeFormattedError<Parsed>( "Loading '{}' names no loading (Resident, Streamed)",
                                                       data.Loading );
        const auto guid = Common::Content::AssetGuidFromText( data.Header.Guid );
        return Common::MakeSuccess( Parsed{ guid.GetValue(), data.Source, soundClass.GetValue(), *loading } );
    }

    Common::ResultStr<std::optional<std::string>> SoundAsset::Upgrade( std::string_view     text,
                                                                       const LegacyClasses& classes )
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
        if ( stated > kSoundSchemaVersion )
            return Common::MakeFormattedError<Result>(
                 "SNDW v{} is not a version this build knows (it writes v{})", stated, kSoundSchemaVersion );

        // v0 -> v1: v0 named a file and nothing else; v1 stated the class every voice of it had played in (the
        // migration's V0Name) and Resident, decoded whole on play.
        SoundDocumentV1 v1;
        if ( stated == 0 )
        {
            auto document = Common::Json::Read<SoundDocumentV0>( text );
            if ( !document )
                return Common::MakeFormattedError<Result>( "not a SNDW v0 .desound: {}", document.GetError() );
            if ( classes.V0Name.empty() )
                return Common::MakeError<Result>(
                     "SNDW v0 -> v1: the migration states no V0Name, the class a v0 sound played in" );
            SoundDocumentV0 v0 = document.ExtractValue();
            v1.Header          = std::move( v0.Header );
            v1.Source          = std::move( v0.Source );
            v1.Class           = std::string( classes.V0Name );
            v1.Loading         = std::string( Audio::SoundLoadingName( Audio::SoundLoading::Resident ) );
        }
        else
        {
            auto document = Common::Json::Read<SoundDocumentV1>( text );
            if ( !document )
                return Common::MakeFormattedError<Result>( "not a SNDW v1 .desound: {}", document.GetError() );
            v1 = document.ExtractValue();
        }

        // v1 -> v2: the enumerator name becomes the class asset the project states for it.
        if ( classes.ForName == nullptr )
            return Common::MakeError<Result>(
                 "SNDW v1 -> v2: the migration states no LegacyClasses::ForName to name the class assets" );
        const Audio::SoundClassId soundClass = classes.ForName( v1.Class, classes.User );
        if ( soundClass.IsNull() )
            return Common::MakeFormattedError<Result>(
                 "SNDW v1 -> v2: Class '{}' names no sound class asset of the project (its LegacyClassNames)",
                 v1.Class );
        const auto loading = Audio::SoundLoadingNamed( v1.Loading );
        if ( !loading )
            return Common::MakeFormattedError<Result>(
                 "SNDW v1 -> v2: Loading '{}' names no loading (Resident, Streamed)", v1.Loading );
        const auto guid   = Common::Content::AssetGuidFromText( v1.Header.Guid );
        auto       raised = Write( guid.GetValue(), v1.Source, soundClass, *loading );
        if ( !raised )
            return Common::MakeError<Result>( raised.GetError() );
        return Common::MakeSuccess( Result{ raised.ExtractValue() } );
    }

    Common::ResultStr<std::string> SoundAsset::Write( const Common::Content::AssetGuid& guid,
                                                      std::string_view                  source,
                                                      const Audio::SoundClassId&        soundClass,
                                                      Audio::SoundLoading               loading )
    {
        if ( guid.IsNull() )
            return Common::MakeError<std::string>( "a .desound needs a GUID; the null GUID names no sound" );
        if ( source.empty() )
            return Common::MakeError<std::string>( "a .desound names its source audio file; none was given" );
        if ( soundClass.IsNull() )
            return Common::MakeError<std::string>(
                 "a .desound names the sound class asset it plays in; the null GUID names none" );
        SoundDocument data;
        data.Header =
             Common::Content::MakeTextHeader( Common::Content::ContentKind::Sound, guid, SoundTextSubsystems() );
        data.Source              = std::string( source );
        data.Class               = Common::Content::AssetGuidToText( soundClass );
        data.Header.Dependencies = { data.Class };
        data.Loading             = std::string( Audio::SoundLoadingName( loading ) );
        return Common::Content::CanonicalJsonTextOfWriterOutput( Common::Json::Write( data ) );
    }
} // namespace Desert::Assets
