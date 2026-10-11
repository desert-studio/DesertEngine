#pragma once

#include <Engine/Core/Window.hpp>

#include <cstdint>

namespace Desert
{
    // HOW THE APPLICATION'S WINDOW IS SHOWN AND PACED — the display part of UE's UGameUserSettings
    // (FullscreenMode, ResolutionSizeX/Y, bUseVSync, FrameRateLimit). One struct for both of its readers: the
    // packaged game takes it from its GameUserSettings (Engine/Settings/GameUserSettings.hpp), the editor
    // states its own; Engine::ApplicationInfo carries it to the window at creation, and
    // Settings::ApplyGameUserSettings moves a live window to a changed one.
    struct DisplaySettings
    {
        WindowMode Mode = WindowMode::Windowed;
        // The client size for Windowed and the monitor's video mode for Fullscreen; the two monitor-sized
        // modes (Maximized, WindowedFullscreen) take the monitor's size and do not read them.
        uint32_t ResolutionX = 0;
        uint32_t ResolutionY = 0;
        bool     VSync       = true;
        // Frames per second the main loop is held to (Engine::FramePacer); 0 = not limited.
        uint32_t FrameRateLimit = 0;

        [[nodiscard]] bool UsesResolution() const
        {
            return Mode == WindowMode::Windowed || Mode == WindowMode::Fullscreen;
        }

        bool operator==( const DisplaySettings& ) const = default;
    };
} // namespace Desert
