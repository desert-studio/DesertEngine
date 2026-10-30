#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/DisplaySettings.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Desert
{
    class Window;
    namespace Engine
    {
        class FramePacer;
    }
} // namespace Desert

namespace Desert::Settings
{
    // UE's Scalability levels (sg.*Quality): 0 = Low, 1 = Medium, 2 = High, 3 = Epic, 4 = Cinematic — the same
    // order as Common::Scalability::Level, so the player's file holds any level the machine can select.
    inline constexpr uint8_t kEpicQualityLevel = 3;
    inline constexpr uint8_t kMaxQualityLevel  = 4;

    // EVERY quality group the engine has (Common::Scalability::Group, one member each, same names) — UE's
    // FQualityLevels inside UGameUserSettings::ScalabilityQuality: the player's choice lives here whole, and
    // machine.json mirrors it (QualityState's Saver) instead of holding a part of it. A group added to the engine
    // without a member here does not compile (GameUserSettings.cpp kPlayerGroups).
    struct ScalabilityLevels
    {
        uint8_t ViewDistance       = kEpicQualityLevel;
        uint8_t Shadows            = kEpicQualityLevel;
        uint8_t PostProcess        = kEpicQualityLevel;
        uint8_t Textures           = kEpicQualityLevel;
        uint8_t Effects            = kEpicQualityLevel;
        uint8_t Foliage            = kEpicQualityLevel;
        // Since GameUserSettings version 1 (a version-0 file has only the six above).
        uint8_t Filtering          = kEpicQualityLevel;
        uint8_t GlobalIllumination = kEpicQualityLevel;
        uint8_t Reflections        = kEpicQualityLevel;
        uint8_t AntiAliasing       = kEpicQualityLevel;
        uint8_t ResolutionScale    = kEpicQualityLevel;
        uint8_t Shading            = kEpicQualityLevel;
        uint8_t PathTracing        = kEpicQualityLevel;

        bool operator==( const ScalabilityLevels& ) const = default;
    };

    // 0 = silent, 1 = as authored. Audio::AudioMix is where they land.
    struct AudioVolumes
    {
        float Master  = 1.0f;
        float Music   = 1.0f;
        float Effects = 1.0f;
        float Voice   = 1.0f;

        bool operator==( const AudioVolumes& ) const = default;
    };

    // THE PLAYER'S SETTINGS — UE's UGameUserSettings, the one home of everything an options menu changes.
    // Two files, one shape, read by Common::Json's rules (an unknown key refused, a missing field its default) and
    // versioned (kGameUserSettingsVersion):
    //   <project>/Config/DefaultGameUserSettings.json — the game's defaults, in the project (UE:
    //       DefaultGameUserSettings.ini);
    //   <GameUserDirectory(product)>/GameUserSettings.json — this player's, beside their logs and crash
    //       reports, never in the install or the repo (UE: Saved/Config/<Platform>/GameUserSettings.ini).
    // Load / Apply / Save are the three verbs, as in UE.
    // THE FORMAT'S VERSION (UE: UE_GAMEUSERSETTINGS_VERSION / UGameUserSettings::Version). A missing field reads as
    // its default, so a version is what tells a file written before a field existed from one that chose it:
    //   0 — no "Version" key: the six groups ViewDistance, Shadows, PostProcess, Textures, Effects, Foliage;
    //   1 — "Version": 1 and every engine quality group (ScalabilityLevels).
    // A file older than this build's is read, then UpgradeGameUserSettings brings it here and the game saves it; a
    // newer one (a later build's) is refused, never guessed at.
    inline constexpr uint32_t kGameUserSettingsVersion = 1;

    struct GameUserSettings
    {
        uint32_t          Version = kGameUserSettingsVersion; // the file's format; 0 = written before versions
        DisplaySettings   Display;
        ScalabilityLevels Scalability;
        AudioVolumes      Audio;
        float             MouseSensitivity = 1.0f; // multiplier on the look input; > 0
        std::string       Language;                // BCP-47 tag a Localization::LocaleRow answers for ("en")

        bool operator==( const GameUserSettings& ) const = default;
    };

    [[nodiscard]] std::filesystem::path
    ProjectGameUserSettingsFile( const std::filesystem::path& projectDirectory );
    [[nodiscard]] std::filesystem::path UserGameUserSettingsFile( const std::filesystem::path& userDirectory );

    // Every value inside its range: a resolution where the mode reads one, quality levels 0..4, volumes 0..1, a
    // positive sensitivity, a language this build knows, a Version no newer than this build's. Refuses naming the
    // field and the value. (An OLDER Version is valid here — it is what LoadGameUserSettings hands to
    // UpgradeGameUserSettings; Apply and Save take only the current one.)
    [[nodiscard]] Common::BoolResultStr ValidateGameUserSettings( const GameUserSettings& settings );

    // Which file the settings came from: the player's own, or — on a first launch, before any was written — the
    // game's defaults (UE: no Saved/Config GameUserSettings.ini yet, the one moment RunHardwareBenchmark runs).
    enum class GameUserSettingsSource
    {
        PlayerFile,
        ProjectDefaults
    };
    struct LoadedGameUserSettings
    {
        GameUserSettings       Settings;
        GameUserSettingsSource Source = GameUserSettingsSource::PlayerFile;
    };

    // The player's file when there is one; else the project's defaults; else a refusal naming both paths. A file
    // that is there but does not parse or validate is refused with its path — it does NOT fall through to the
    // project's, because that would silently undo what the player chose. Either file may be of an OLDER version
    // (Settings.Version says which; a newer one is refused): the player's goes through UpgradeGameUserSettings once
    // QualityState is up and is saved; the project's, read only on a first launch, through AdoptMachineQuality,
    // which sets every group and so the version.
    [[nodiscard]] Common::ResultStr<LoadedGameUserSettings>
    LoadGameUserSettings( const std::filesystem::path& projectDirectory,
                          const std::filesystem::path& userDirectory );

    // Brings settings read from an OLDER file (Version < kGameUserSettingsVersion) to this build's format, in memory
    // — UE UGameUserSettings::LoadSettings on a stale Version, except that nothing the player chose is reset:
    //   0 -> 1: the six groups the file had keep the player's levels; the seven it had no field for (Filtering,
    //           GlobalIllumination, Reflections, AntiAliasing, ResolutionScale, Shading, PathTracing) take the level
    //           the machine runs them at (QualityState::Selection — machine.json's, else the recommendation), so the
    //           upgrade changes nothing on screen.
    // The caller saves the result (SaveGameUserSettings) so the next start reads the new version and does not
    // upgrade again. Settings already at the current version are left as they are. Needs QualityState
    // initialized (QualityBoot::Start); refused before it, and on a version newer than this build's, with
    // `settings` untouched.
    [[nodiscard]] Common::BoolResultStr UpgradeGameUserSettings( GameUserSettings& settings );

    // Writes the player's file (atomically, creating the directory). Refuses an invalid value, or settings not at
    // kGameUserSettingsVersion (UpgradeGameUserSettings first), without writing.
    [[nodiscard]] Common::BoolResultStr SaveGameUserSettings( const GameUserSettings&      settings,
                                                              const std::filesystem::path& userDirectory );

    // A FIRST LAUNCH (Source == ProjectDefaults) — UE ApplyHardwareBenchmarkResults: every quality group takes the
    // level the machine started on (Graphic::QualityBoot::Start — machine.json's saved selection, else the GPU
    // benchmark's recommendation for this device, else High), never the project's defaults (Epic), and the
    // result is written as the player's file; from then on that file is the only source of these levels and the
    // recommendation never overwrites it, at kGameUserSettingsVersion whatever version the project's defaults had.
    // The rest of `settings` (display, audio, language) stays the project's.
    // Needs QualityState initialized (QualityBoot::Start); refuses a save that fails, with SaveGameUserSettings'
    // reason, and leaves `settings` untouched on any refusal.
    [[nodiscard]] Common::BoolResultStr AdoptMachineQuality( GameUserSettings&            settings,
                                                             const std::filesystem::path& userDirectory );

    // Hands every value to its consumer (UE: UGameUserSettings::ApplySettings) — the game calls it ONCE at start
    // and again after every change its options menu makes:
    //   * the quality groups -> Common::Scalability::QualityState::SetGroupLevels, THE apply point every host
    //     shares (UE: Scalability::SetQualityLevels), in one resolve; the renderer reads only ResolvedQuality,
    //     never these settings. A group whose level did not change keeps its overrides (UE without bForce);
    //   * the window (mode, resolution — moved only when it differs from what the window already is — and
    //     VSync, which rebuilds the swapchain), the frame pacer, Audio::AudioMix, Localization (the language),
    //     and the sensitivity below.
    // Needs QualityState initialized (Graphic::QualityBoot::Start, after the device): before it, refused.
    // Refuses an invalid value, settings not at kGameUserSettingsVersion (UpgradeGameUserSettings first), or a
    // quality the apply point refuses, before touching anything; a window that
    // cannot take the mode with the window's own reason.
    [[nodiscard]] Common::BoolResultStr ApplyGameUserSettings( const GameUserSettings& settings, Window& window,
                                                               Engine::FramePacer& pacer );
    // The look-input multiplier the last Apply set (1 until then), for the player controller to read.
    [[nodiscard]] float AppliedMouseSensitivity();
} // namespace Desert::Settings
