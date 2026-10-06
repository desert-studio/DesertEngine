#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Window.hpp>

#include <cstdint>

struct GLFWwindow;

namespace Desert
{
    // THE ONE PLACE A LIVE GLFW WINDOW CHANGES ITS WindowMode — both platform windows (MacOSWindow,
    // WindowsWindow) call it from SetWindowMode, and from Init for the one mode glfwCreateWindow is not asked
    // for directly (exclusive Fullscreen). @p requestedFrame is the Decorated the APPLICATION asked for: the
    // two windowed modes give it back, the two monitor-covering modes have no frame whatever was asked.
    // Refuses when no monitor is online — a fullscreen request cannot be honoured without one, and the window
    // is then left exactly as it was.
    [[nodiscard]] Common::BoolResultStr ApplyGlfwWindowMode( GLFWwindow* window, WindowMode mode, uint32_t width,
                                                             uint32_t height, bool requestedFrame );

    // The frame the window has in @p mode when the application asked for @p requestedFrame.
    [[nodiscard]] constexpr bool WindowModeHasFrame( WindowMode mode, bool requestedFrame )
    {
        return requestedFrame && ( mode == WindowMode::Windowed || mode == WindowMode::Maximized );
    }
} // namespace Desert
