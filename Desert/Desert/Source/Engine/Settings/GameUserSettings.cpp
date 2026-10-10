#include "GameUserSettings.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Json.hpp>
#include <Engine/Audio/AudioMix.hpp>
#include <Engine/Core/FramePacer.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Graphic/SwapChain.hpp>
#include <Engine/Localization/LocaleFormat.hpp>
#include <Engine/Localization/LocalizationService.hpp>

#include <format>
#include <system_error>

namespace Desert::Settings
{
    namespace
    {
        ScalabilityLevels s_AppliedScalability;
        float             s_AppliedMouseSensitivity = 1.0f;

        Common::BoolResultStr Refuse( std::string reason )
        {
            return Common::MakeError<bool>( std::move( reason ) );
        }

        Common::BoolResultStr CheckLevel( const char* group, uint8_t level )
        {
            if ( level > kMaxQualityLevel )
                return Refuse( std::format( "Scalability.{} is {}, a quality level is 0..{}", group, level,
                                            kMaxQualityLevel ) );
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr CheckVolume( const char* name, float volume )
        {
            if ( !( volume >= 0.0f && volume <= 1.0f ) )
                return Refuse( std::format( "Audio.{} is {}, a volume is 0..1", name, volume ) );
            return Common::MakeSuccess( true );
        }

        Common::ResultStr<GameUserSettings> ReadValid( const std::filesystem::path& file )
        {
            auto parsed = Common::Json::ReadFile<GameUserSettings>( file );
            if ( !parsed )
                return Common::MakeError<GameUserSettings>( parsed.GetError() );
            if ( const auto valid = ValidateGameUserSettings( parsed.GetValue() ); !valid )
                return Common::MakeError<GameUserSettings>( std::format( "{}: {}", file.string(), valid.GetError() ) );
            return parsed;
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

        const ScalabilityLevels& q = settings.Scalability;
        for ( const auto& checked : { CheckLevel( "ViewDistance", q.ViewDistance ), CheckLevel( "Shadows", q.Shadows ),
                                      CheckLevel( "PostProcess", q.PostProcess ), CheckLevel( "Textures", q.Textures ),
                                      CheckLevel( "Effects", q.Effects ), CheckLevel( "Foliage", q.Foliage ),
                                      CheckVolume( "Master", settings.Audio.Master ),
                                      CheckVolume( "Music", settings.Audio.Music ),
                                      CheckVolume( "Effects", settings.Audio.Effects ),
                                      CheckVolume( "Voice", settings.Audio.Voice ) } )
            if ( !checked )
                return checked;

        if ( !( settings.MouseSensitivity > 0.0f ) )
            return Refuse( std::format( "MouseSensitivity is {}, it must be above 0", settings.MouseSensitivity ) );
        if ( Localization::FindLocale( settings.Language ) == nullptr )
            return Refuse( std::format( "Language '{}' is not a language this build knows", settings.Language ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<GameUserSettings> LoadGameUserSettings( const std::filesystem::path& projectDirectory,
                                                              const std::filesystem::path& userDirectory )
    {
        const std::filesystem::path user    = UserGameUserSettingsFile( userDirectory );
        const std::filesystem::path project = ProjectGameUserSettingsFile( projectDirectory );
        std::error_code             ec;
        if ( std::filesystem::exists( user, ec ) )
            return ReadValid( user );
        if ( std::filesystem::exists( project, ec ) )
            return ReadValid( project );
        return Common::MakeError<GameUserSettings>(
             std::format( "no game user settings: neither the player's {} nor the project's {} exists", user.string(),
                          project.string() ) );
    }

    Common::BoolResultStr SaveGameUserSettings( const GameUserSettings& settings, const std::filesystem::path& userDirectory )
    {
        const std::filesystem::path file = UserGameUserSettingsFile( userDirectory );
        if ( const auto valid = ValidateGameUserSettings( settings ); !valid )
            return Refuse( std::format( "{} was not saved: {}", file.string(), valid.GetError() ) );
        std::error_code ec;
        std::filesystem::create_directories( userDirectory, ec );
        if ( ec )
            return Refuse( std::format( "{} was not saved: cannot create its folder: {}", file.string(), ec.message() ) );
        if ( const auto written = Common::Json::WriteFileAtomic( file, settings ); !written )
            return Refuse( std::format( "{} was not saved: {}", file.string(), written.GetError() ) );
        LOG_INFO( "[Settings] saved the game user settings -> {}", file.string() );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ApplyGameUserSettings( const GameUserSettings& settings, Window& window,
                                                 Engine::FramePacer& pacer )
    {
        if ( const auto valid = ValidateGameUserSettings( settings ); !valid )
            return Refuse( "the game user settings were not applied: " + valid.GetError() );

        // The window: moved only when it is not already there — the packaged game was CREATED from these same
        // DisplaySettings, and re-entering a mode it is in would restore and re-zoom it for nothing.
        const DisplaySettings& display = settings.Display;
        const bool             sizeDiffers =
             display.UsesResolution() &&
             ( window.GetWidth() != display.ResolutionX || window.GetHeight() != display.ResolutionY );
        if ( window.GetWindowMode() != display.Mode || sizeDiffers )
            if ( const auto moved = window.SetWindowMode( display.Mode, display.ResolutionX, display.ResolutionY );
                 !moved )
                return Refuse( "the game user settings were not applied: " + moved.GetError() );
        // VSync goes through the swapchain's present pacing (Window::SetDisplay rebuilds it only on a change);
        // the rest of that pacing is kept as it is.
        if ( const std::shared_ptr<Graphic::SwapChain> swapChain = window.GetWindowSwapChain() )
        {
            Common::Scalability::DisplaySettings pacing = swapChain->Display();
            pacing.VSync                                = display.VSync;
            window.SetDisplay( pacing );
        }
        pacer.SetLimit( display.FrameRateLimit );

        Audio::AudioMix& mix = Audio::AudioMix::Get();
        mix.SetMasterVolume( settings.Audio.Master );
        mix.SetClassVolume( Audio::SoundClass::Music, settings.Audio.Music );
        mix.SetClassVolume( Audio::SoundClass::Effects, settings.Audio.Effects );
        mix.SetClassVolume( Audio::SoundClass::Voice, settings.Audio.Voice );

        if ( const auto language = Localization::Localization::Get().SetLanguage( settings.Language ); !language )
            return Refuse( "the game user settings were not applied: " + language.GetError() );

        s_AppliedScalability      = settings.Scalability;
        s_AppliedMouseSensitivity = settings.MouseSensitivity;
        return Common::MakeSuccess( true );
    }

    const ScalabilityLevels& AppliedScalability()
    {
        return s_AppliedScalability;
    }

    float AppliedMouseSensitivity()
    {
        return s_AppliedMouseSensitivity;
    }
} // namespace Desert::Settings
