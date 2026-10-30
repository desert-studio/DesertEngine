#include "GameUserSettings.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Audio/AudioMix.hpp>
#include <Engine/Core/FramePacer.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Graphic/SwapChain.hpp>
#include <Engine/Localization/LocaleFormat.hpp>
#include <Engine/Localization/LocalizationService.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <system_error>

namespace Desert::Settings
{
    namespace
    {
        float s_AppliedMouseSensitivity = 1.0f;

        Common::BoolResultStr Refuse( std::string reason )
        {
            return Common::MakeError<bool>( std::move( reason ) );
        }

        /// One shape for every refusal of ApplyGameUserSettings: what was refused, then why.
        Common::BoolResultStr NotApplied( const std::string_view why )
        {
            return Refuse( std::format( "the game user settings were not applied: {}", why ) );
        }

        Common::BoolResultStr CheckLevel( const char* group, uint8_t level )
        {
            if ( level > kMaxQualityLevel )
                return Refuse( std::format( "Scalability.{} is {}, a quality level is 0..{}", group, level,
                                            kMaxQualityLevel ) );
            return Common::MakeSuccess( true );
        }

        // THE ONE MAP between the player's file and the engine's groups: the JSON member, the engine group it sets,
        // and the GameUserSettings version that added it. Every group exactly once (checked below), so a group the
        // engine adds without a member here does not compile.
        struct PlayerGroup
        {
            const char*                  Name;
            uint8_t ScalabilityLevels::* Member;
            Common::Scalability::Group   EngineGroup;
            uint32_t                     Since; // the first GameUserSettings version whose file holds it
        };
        using G                               = Common::Scalability::Group;
        using SL                              = ScalabilityLevels;
        constexpr PlayerGroup kPlayerGroups[] = {
             { "ViewDistance", &SL::ViewDistance, G::ViewDistance, 0 },
             { "Shadows", &SL::Shadows, G::Shadows, 0 },
             { "PostProcess", &SL::PostProcess, G::PostProcess, 0 },
             { "Textures", &SL::Textures, G::Textures, 0 },
             { "Effects", &SL::Effects, G::Effects, 0 },
             { "Foliage", &SL::Foliage, G::Foliage, 0 },
             { "Filtering", &SL::Filtering, G::Filtering, 1 },
             { "GlobalIllumination", &SL::GlobalIllumination, G::GlobalIllumination, 1 },
             { "Reflections", &SL::Reflections, G::Reflections, 1 },
             { "AntiAliasing", &SL::AntiAliasing, G::AntiAliasing, 1 },
             { "ResolutionScale", &SL::ResolutionScale, G::ResolutionScale, 1 },
             { "Shading", &SL::Shading, G::Shading, 1 },
             { "PathTracing", &SL::PathTracing, G::PathTracing, 1 },
        };
        static_assert( std::size( kPlayerGroups ) == Common::Scalability::kGroupCount,
                       "every engine quality group has its member in ScalabilityLevels and its row here" );
        consteval bool EveryGroupOnceAtAKnownVersion()
        {
            std::array<int, Common::Scalability::kGroupCount> seen{};
            for ( const PlayerGroup& g : kPlayerGroups )
                if ( ++seen[static_cast<std::size_t>( g.EngineGroup )] != 1 || g.Since > kGameUserSettingsVersion )
                    return false;
            return true;
        }
        static_assert( EveryGroupOnceAtAKnownVersion(),
                       "kPlayerGroups names each engine group once, at a version this build has" );

        // A player's level 0..kMaxQualityLevel is Level::Low..Cinematic, the same order.
        static_assert( static_cast<int>( Common::Scalability::Level::Epic ) == kEpicQualityLevel &&
                            static_cast<int>( Common::Scalability::Level::Cinematic ) == kMaxQualityLevel,
                       "a player's quality level 0..kMaxQualityLevel is Level::Low..Cinematic" );

        using EngineLevels =
             std::array<Common::Scalability::QualityState::GroupLevel, Common::Scalability::kGroupCount>;

        // The player's groups as the engine's, all of them: the player's file is the whole selection.
        EngineLevels EngineGroupLevels( const ScalabilityLevels& q )
        {
            EngineLevels levels{};
            for ( std::size_t i = 0; i < std::size( kPlayerGroups ); ++i )
                levels[i] = { kPlayerGroups[i].EngineGroup,
                              static_cast<Common::Scalability::Level>( q.*kPlayerGroups[i].Member ) };
            return levels;
        }

        // The level the machine runs one engine group at, as a player's level.
        uint8_t MachineLevel( const Common::Scalability::QualitySelection& selection,
                              const Common::Scalability::Group             group )
        {
            return static_cast<uint8_t>( selection.Levels[static_cast<std::size_t>( group )] );
        }

        Common::BoolResultStr CheckCurrentVersion( const GameUserSettings& settings )
        {
            if ( settings.Version != kGameUserSettingsVersion )
                return Refuse( std::format( "Version is {}, this build takes {} (UpgradeGameUserSettings first)",
                                            settings.Version, kGameUserSettingsVersion ) );
            return Common::MakeSuccess( true );
        }

        // "Name level, ..." of the groups whose Since is in [firstSince, lastSince], for the log.
        std::string DescribeLevels( const ScalabilityLevels& q, const uint32_t firstSince, const uint32_t lastSince )
        {
            std::string text;
            for ( const PlayerGroup& g : kPlayerGroups )
                if ( g.Since >= firstSince && g.Since <= lastSince )
                    text += std::format( "{}{} {}", text.empty() ? "" : ", ", g.Name, q.*g.Member );
            return text;
        }

        Common::BoolResultStr CheckVolume( const char* name, float volume )
        {
            if ( !( volume >= 0.0f && volume <= 1.0f ) )
                return Refuse( std::format( "Audio.{} is {}, a volume is 0..1", name, volume ) );
            return Common::MakeSuccess( true );
        }

        // A file with no "Version" key is version 0, written before versions: Common::Json reads a missing key as
        // the struct's default — the CURRENT version — so without this an old file would pass for a new one.
        Common::ResultStr<LoadedGameUserSettings> ReadValid( const std::filesystem::path& file,
                                                             const GameUserSettingsSource source )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !text )
                return Common::MakeError<LoadedGameUserSettings>(
                     std::format( "{}: could not be read: {}", file.string(), text.GetError() ) );
            auto parsed = Common::Json::Read<GameUserSettings>( text.GetValue() );
            if ( !parsed )
                return Common::MakeError<LoadedGameUserSettings>(
                     std::format( "{}: {}", file.string(), parsed.GetError() ) );
            const auto members = Common::Json::ObjectMembers( text.GetValue() );
            if ( !members )
                return Common::MakeError<LoadedGameUserSettings>(
                     std::format( "{}: {}", file.string(), members.GetError() ) );
            GameUserSettings settings = parsed.GetValue();
            const bool       stated   = std::ranges::any_of( members.GetValue(), []( const auto& member )
                                                             { return member.first == "Version"; } );
            if ( !stated )
                settings.Version = 0;
            if ( const auto valid = ValidateGameUserSettings( settings ); !valid )
                return Common::MakeError<LoadedGameUserSettings>(
                     std::format( "{}: {}", file.string(), valid.GetError() ) );
            return Common::MakeSuccess( LoadedGameUserSettings{ std::move( settings ), source } );
        }

        // The engine's levels back as the player's (the inverse of EngineGroupLevels), every group.
        ScalabilityLevels PlayerLevels( const Common::Scalability::QualitySelection& selection )
        {
            ScalabilityLevels q;
            for ( const PlayerGroup& g : kPlayerGroups )
                q.*g.Member = MachineLevel( selection, g.EngineGroup );
            return q;
        }
    } // namespace

    std::filesystem::path ProjectGameUserSettingsFile( const std::filesystem::path& projectDirectory )
    {
        return projectDirectory / "Config" / "DefaultGameUserSettings.json";
    }

    std::filesystem::path UserGameUserSettingsFile( const std::filesystem::path& userDirectory )
    {
        return userDirectory / "GameUserSettings.json";
    }

    Common::BoolResultStr ValidateGameUserSettings( const GameUserSettings& settings )
    {
        const DisplaySettings& display = settings.Display;
        if ( display.UsesResolution() && ( display.ResolutionX == 0 || display.ResolutionY == 0 ) )
            return Refuse( std::format( "Display.Resolution is {}x{}, the mode needs a size", display.ResolutionX,
                                        display.ResolutionY ) );

        if ( settings.Version > kGameUserSettingsVersion )
            return Refuse( std::format( "Version is {}, newer than this build's {} (written by a later build)",
                                        settings.Version, kGameUserSettingsVersion ) );
        for ( const PlayerGroup& g : kPlayerGroups )
            if ( const auto checked = CheckLevel( g.Name, settings.Scalability.*g.Member ); !checked )
                return checked;
        for ( const auto& checked :
              { CheckVolume( "Master", settings.Audio.Master ), CheckVolume( "Music", settings.Audio.Music ),
                CheckVolume( "Effects", settings.Audio.Effects ), CheckVolume( "Voice", settings.Audio.Voice ) } )
            if ( !checked )
                return checked;

        if ( !( settings.MouseSensitivity > 0.0f ) )
            return Refuse(
                 std::format( "MouseSensitivity is {}, it must be above 0", settings.MouseSensitivity ) );
        if ( Localization::FindLocale( settings.Language ) == nullptr )
            return Refuse( std::format( "Language '{}' is not a language this build knows", settings.Language ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<LoadedGameUserSettings> LoadGameUserSettings( const std::filesystem::path& projectDirectory,
                                                              const std::filesystem::path& userDirectory )
    {
        const std::filesystem::path user    = UserGameUserSettingsFile( userDirectory );
        const std::filesystem::path project = ProjectGameUserSettingsFile( projectDirectory );
        std::error_code             ec;
        if ( std::filesystem::exists( user, ec ) )
            return ReadValid( user, GameUserSettingsSource::PlayerFile );
        // The project's defaults are CONTENT (UE: Config/Default*.ini ship inside the build): asked through the
        // same file layer as every asset, so a packaged game finds them in its archive (PackagedContentTrees
        // stages the project Config under the "Config" key) and the dev tree finds them on disk.
        if ( Common::Utils::FileSystem::Exists( project ) )
            return ReadValid( project, GameUserSettingsSource::ProjectDefaults );
        return Common::MakeError<LoadedGameUserSettings>(
             std::format( "no game user settings: neither the player's {} nor the project's {} exists",
                          user.string(), project.string() ) );
    }

    Common::BoolResultStr SaveGameUserSettings( const GameUserSettings&      settings,
                                                const std::filesystem::path& userDirectory )
    {
        const std::filesystem::path file = UserGameUserSettingsFile( userDirectory );
        if ( const auto valid = ValidateGameUserSettings( settings ); !valid )
            return Refuse( std::format( "{} was not saved: {}", file.string(), valid.GetError() ) );
        if ( const auto current = CheckCurrentVersion( settings ); !current )
            return Refuse( std::format( "{} was not saved: {}", file.string(), current.GetError() ) );
        std::error_code ec;
        std::filesystem::create_directories( userDirectory, ec );
        if ( ec )
            return Refuse(
                 std::format( "{} was not saved: cannot create its folder: {}", file.string(), ec.message() ) );
        if ( const auto written = Common::Json::WriteFileAtomic( file, settings ); !written )
            return Refuse( std::format( "{} was not saved: {}", file.string(), written.GetError() ) );
        LOG_INFO( "[Settings] saved the game user settings -> {}", file.string() );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AdoptMachineQuality( GameUserSettings& settings, const std::filesystem::path& userDirectory )
    {
        if ( !Common::Scalability::QualityState::Initialized() )
            return Refuse( "the machine's quality was not adopted: QualityState is not initialized "
                           "(Graphic::QualityBoot::Start did not run or refused)" );
        GameUserSettings adopted = settings;
        adopted.Scalability      = PlayerLevels( Common::Scalability::QualityState::Selection() );
        // Every group is the machine's now, so defaults of an older version are this build's from here on.
        adopted.Version = kGameUserSettingsVersion;
        if ( const auto saved = SaveGameUserSettings( adopted, userDirectory ); !saved )
            return saved;
        LOG_INFO( "[Settings] first launch: the player's quality is the machine's - {} (0 = Low .. 4 = Cinematic)",
                  DescribeLevels( adopted.Scalability, 0, kGameUserSettingsVersion ) );
        settings = std::move( adopted );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr UpgradeGameUserSettings( GameUserSettings& settings )
    {
        if ( settings.Version > kGameUserSettingsVersion )
            return Refuse( std::format( "the game user settings were not upgraded: Version {} is newer than this "
                                        "build's {}",
                                        settings.Version, kGameUserSettingsVersion ) );
        if ( settings.Version == kGameUserSettingsVersion )
            return Common::MakeSuccess( true );
        if ( !Common::Scalability::QualityState::Initialized() )
            return Refuse( "the game user settings were not upgraded: QualityState is not initialized "
                           "(Graphic::QualityBoot::Start did not run or refused)" );
        const uint32_t                               from      = settings.Version;
        const Common::Scalability::QualitySelection& selection = Common::Scalability::QualityState::Selection();
        GameUserSettings                             upgraded  = settings;
        // Every step from `from` on. 0 -> 1: a group the old file had no field for runs at the machine's level.
        for ( const PlayerGroup& g : kPlayerGroups )
            if ( g.Since > from )
                upgraded.Scalability.*g.Member = MachineLevel( selection, g.EngineGroup );
        upgraded.Version = kGameUserSettingsVersion;
        if ( const auto valid = ValidateGameUserSettings( upgraded ); !valid )
            return Refuse( std::format( "the game user settings were not upgraded: {}", valid.GetError() ) );
        LOG_INFO( "[Settings] the game user settings upgraded from version {} to {}: the player's {}; the machine's {}",
                  from, kGameUserSettingsVersion, DescribeLevels( upgraded.Scalability, 0, from ),
                  DescribeLevels( upgraded.Scalability, from + 1, kGameUserSettingsVersion ) );
        settings = std::move( upgraded );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ApplyGameUserSettings( const GameUserSettings& settings, Window& window,
                                                 Engine::FramePacer& pacer )
    {
        if ( const auto valid = ValidateGameUserSettings( settings ); !valid )
            return NotApplied( valid.GetError() );
        if ( const auto current = CheckCurrentVersion( settings ); !current )
            return NotApplied( current.GetError() );

        // The quality first: it is the one step that can refuse on state (QualityState not initialized), and a
        // refusal must leave the window, the mix and the language as they were.
        const auto groupLevels = EngineGroupLevels( settings.Scalability );
        if ( const auto quality = Common::Scalability::QualityState::SetGroupLevels( groupLevels );
             !quality.Refused.empty() )
            return NotApplied( std::format( "the quality levels: {}", quality.Refused ) );

        // The window: moved only when it is not already there — the packaged game was CREATED from these same
        // DisplaySettings, and re-entering a mode it is in would restore and re-zoom it for nothing.
        const DisplaySettings& display = settings.Display;
        const bool sizeDiffers         = display.UsesResolution() && ( window.GetWidth() != display.ResolutionX ||
                                                               window.GetHeight() != display.ResolutionY );
        if ( window.GetWindowMode() != display.Mode || sizeDiffers )
            if ( const auto moved = window.SetWindowMode( display.Mode, display.ResolutionX, display.ResolutionY );
                 !moved )
                return NotApplied( moved.GetError() );
        // VSync goes through the swapchain's present pacing (Window::SetDisplay rebuilds it only on a change);
        // the rest of that pacing is kept as it is.
        const std::shared_ptr<Graphic::SwapChain> swapChain = window.GetWindowSwapChain();
        Common::Scalability::DisplaySettings      pacing =
             swapChain ? swapChain->Display() : Common::Scalability::DisplaySettings{};
        pacing.VSync = display.VSync;
        window.SetDisplay( pacing );
        pacer.SetLimit( display.FrameRateLimit );

        Audio::AudioMix& mix = Audio::AudioMix::Get();
        mix.SetMasterVolume( settings.Audio.Master );
        mix.SetClassVolume( Audio::SoundClass::Music, settings.Audio.Music );
        mix.SetClassVolume( Audio::SoundClass::Effects, settings.Audio.Effects );
        mix.SetClassVolume( Audio::SoundClass::Voice, settings.Audio.Voice );

        if ( const auto language = Localization::Localization::Get().SetLanguage( settings.Language ); !language )
            return NotApplied( language.GetError() );

        s_AppliedMouseSensitivity = settings.MouseSensitivity;
        return Common::MakeSuccess( true );
    }

    float AppliedMouseSensitivity()
    {
        return s_AppliedMouseSensitivity;
    }
} // namespace Desert::Settings
