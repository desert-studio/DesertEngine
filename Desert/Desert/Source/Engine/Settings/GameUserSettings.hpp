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
    // UE's Scalability groups (sg.*Quality): 0 = Low, 1 = Medium, 2 = High, 3 = Epic.
    inline constexpr uint8_t kMaxQualityLevel = 3;

    struct ScalabilityLevels
    {
        uint8_t ViewDistance = kMaxQualityLevel;
        uint8_t Shadows      = kMaxQualityLevel;
        uint8_t PostProcess  = kMaxQualityLevel;
        uint8_t Textures     = kMaxQualityLevel;
        uint8_t Effects      = kMaxQualityLevel;
        uint8_t Foliage      = kMaxQualityLevel;

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
    // Two files, one shape, read strictly (every field present):
    //   <project>/Config/DefaultGameUserSettings.json — the game's defaults, in the project (UE:
    //       DefaultGameUserSettings.ini);
    //   <GameUserDirectory(product)>/GameUserSettings.json — this player's, beside their logs and crash
    //       reports, never in the install or the repo (UE: Saved/Config/<Platform>/GameUserSettings.ini).
    // Load / Apply / Save are the three verbs, as in UE.
    struct GameUserSettings
    {
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

    // Every value inside its range: a resolution where the mode reads one, quality levels 0..3, volumes 0..1, a
    // positive sensitivity, a language this build knows. Refuses naming the field and the value.
    [[nodiscard]] Common::BoolResultStr ValidateGameUserSettings( const GameUserSettings& settings );

    // The player's file when there is one; else the project's defaults; else a refusal naming both paths. A file
    // that is there but does not parse or validate is refused with its path — it does NOT fall through to the
    // project's, because that would silently undo what the player chose.
    [[nodiscard]] Common::ResultStr<GameUserSettings>
    LoadGameUserSettings( const std::filesystem::path& projectDirectory,
                          const std::filesystem::path& userDirectory );

    // Writes the player's file (atomically, creating the directory). Refuses an invalid value without writing.
    [[nodiscard]] Common::BoolResultStr SaveGameUserSettings( const GameUserSettings&      settings,
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
    // Refuses an invalid value, or a quality the apply point refuses, before touching anything; a window that
    // cannot take the mode with the window's own reason.
    [[nodiscard]] Common::BoolResultStr ApplyGameUserSettings( const GameUserSettings& settings, Window& window,
                                                               Engine::FramePacer& pacer );
    // The look-input multiplier the last Apply set (1 until then), for the player controller to read.
    [[nodiscard]] float AppliedMouseSensitivity();
} // namespace Desert::Settings
