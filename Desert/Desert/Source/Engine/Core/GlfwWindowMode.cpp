#include "GlfwWindowMode.hpp"

#include <Engine/Core/Glfw.hpp>

#include <format>

namespace Desert
{
    Common::BoolResultStr ApplyGlfwWindowMode( GLFWwindow* window, WindowMode mode, uint32_t width, uint32_t height,
                                               bool requestedFrame )
    {
        if ( window == nullptr )
            return Common::MakeError<bool>( "the window mode was not changed: the window does not exist" );
        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* video   = monitor ? glfwGetVideoMode( monitor ) : nullptr;
        if ( monitor == nullptr || video == nullptr )
            return Common::MakeError<bool>( "the window mode was not changed: no monitor is online (lid closed?)" );
        if ( ( mode == WindowMode::Windowed || mode == WindowMode::Fullscreen ) && ( width == 0 || height == 0 ) )
            return Common::MakeError<bool>(
                 std::format( "the window mode was not changed: {}x{} is not a size", width, height ) );

        int monitorX = 0, monitorY = 0;
        glfwGetMonitorPos( monitor, &monitorX, &monitorY );
        int workX = 0, workY = 0, workW = 0, workH = 0;
        glfwGetMonitorWorkarea( monitor, &workX, &workY, &workW, &workH );

        // Every mode but exclusive Fullscreen starts from a window that owns no monitor and is not zoomed —
        // a maximized window ignores a resize on both platforms until it is restored.
        if ( glfwGetWindowAttrib( window, GLFW_MAXIMIZED ) == GLFW_TRUE )
            glfwRestoreWindow( window );

        const bool frame = WindowModeHasFrame( mode, requestedFrame );
        switch ( mode )
        {
            case WindowMode::Fullscreen:
                glfwSetWindowMonitor( window, monitor, 0, 0, (int)width, (int)height, GLFW_DONT_CARE );
                break;
            case WindowMode::WindowedFullscreen:
                glfwSetWindowMonitor( window, nullptr, monitorX, monitorY, video->width, video->height,
                                      GLFW_DONT_CARE );
                glfwSetWindowAttrib( window, GLFW_DECORATED, GLFW_FALSE );
                // Dropping the frame keeps the FRAME rect on Cocoa and grows the content rect by the bar's
                // height (see MacOSWindow::Init): the size and the place are set again after it.
                glfwSetWindowSize( window, video->width, video->height );
                glfwSetWindowPos( window, monitorX, monitorY );
                break;
            case WindowMode::Windowed:
            {
                const int w = (int)width, h = (int)height;
                const int x = workX + ( workW > w ? ( workW - w ) / 2 : 0 );
                const int y = workY + ( workH > h ? ( workH - h ) / 2 : 0 );
                glfwSetWindowMonitor( window, nullptr, x, y, w, h, GLFW_DONT_CARE );
                glfwSetWindowAttrib( window, GLFW_DECORATED, frame ? GLFW_TRUE : GLFW_FALSE );
                glfwSetWindowSize( window, w, h );
                glfwSetWindowPos( window, x, y );
                break;
            }
            case WindowMode::Maximized:
                glfwSetWindowMonitor( window, nullptr, workX, workY, workW * 4 / 5, workH * 4 / 5, GLFW_DONT_CARE );
                glfwSetWindowAttrib( window, GLFW_DECORATED, frame ? GLFW_TRUE : GLFW_FALSE );
                glfwMaximizeWindow( window );
                break;
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert
