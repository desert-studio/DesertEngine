#include <Platform/Windows/WindowsWindow.hpp>

#include <Common/Core/Events/WindowEvents.hpp>
#include <Common/Core/Events/MouseEvents.hpp>
#include <Common/Core/Events/KeyEvents.hpp>
#include <Common/Core/Events/WindowEvents.hpp>

#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/ViewMemory.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Renderer.hpp>

namespace Desert::Platform::Windows
{

    static void GLFWErrorCallback( int error, const char* description )
    {
        LOG_ERROR( "GLFW Error: ({}: {})", error, description );
    }

    static bool s_GLFWInitialized = false;

    Common::ResultStr<bool> WindowsWindow::Init()
    {
        if ( !s_GLFWInitialized )
        {
            // TODO: glfwTerminate on system shutdown
            int success = glfwInit();
            if ( !success )
            {
                return Common::MakeError<bool>( "Could not intialize GLFW!" );
            }

            glfwSetErrorCallback( GLFWErrorCallback );
            s_GLFWInitialized = true;
        }

        if ( Graphic::RendererAPI::GetAPIType() == Graphic::RendererAPIType::Vulkan )
            glfwWindowHint( GLFW_CLIENT_API, GLFW_NO_API );

        auto width  = m_Data.Specification.Width;
        auto height = m_Data.Specification.Height;

        // NO MONITOR IS A REAL STATE ON THIS SIDE TOO, and this file did not say so. MacOSWindow::Init has
        // carried the guard and the explanation since a closed lid produced an empty display list there;
        // the same two calls stood here unguarded, so a Windows host with every display asleep or detached
        // (a headless CI runner, an RDP session that has dropped its console) dereferenced null in
        // glfwGetVideoMode before it reached a single frame. Found while У9 was reading both files to keep
        // them one shape; the fallback is the authored size, as on macOS, and it says so.
        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode    = monitor ? glfwGetVideoMode( monitor ) : nullptr;
        if ( !monitor )
            LOG_ERROR( "No monitor is online: window falls back to {}x{}, fullscreen ignored", width, height );

        int        posX = 0, posY = 0;
        bool       setPos       = false;
        const bool coverTaskbar = m_Data.Specification.Fullscreen && m_Data.Specification.FullscreenCoverTaskbar;

        // Covering the taskbar means the window has no frame whatever the specification says: there is
        // nowhere on the monitor to put one.
        const bool wantsFrame = m_Data.Specification.Decorated && !coverTaskbar;

        if ( m_Data.Specification.Fullscreen && monitor && mode )
        {
            if ( coverTaskbar )
            {
                width  = mode->width;
                height = mode->height;
                posX   = 0;
                posY   = 0;
                setPos = true;
            }
            else
            {
                // MAXIMIZED to the work area, leaving the taskbar visible. As on macOS, the size handed to
                // glfwCreateWindow is the RESTORE size — the hint zooms the window straight afterwards —
                // and the work area was the wrong answer for it: "restore down" then gave back a window
                // the size of the screen, which is not a restore. See the longer note in MacOSWindow::Init
                // for the measurement, including the 28 px overhang the old value produced there.
                //
                // ONLY A VISIBLE WINDOW IS ZOOMED AT CREATION. The hint makes GLFW call -zoom: (Cocoa) /
                // create with WS_MAXIMIZE (Win32) on a window the editor keeps hidden behind its splash,
                // and a zoom is a window-manager operation on a window that is not meant to be on screen
                // yet. A hidden window keeps the restore size and is maximized by Show().
                if ( m_Data.Specification.Visible )
                    glfwWindowHint( GLFW_MAXIMIZED, GLFW_TRUE );
                else
                    m_MaximizeOnShow = true;
                int wx, wy, ww, wh;
                glfwGetMonitorWorkarea( monitor, &wx, &wy, &ww, &wh );
                width  = (uint32_t)( ww * 4 / 5 );
                height = (uint32_t)( wh * 4 / 5 );
            }

            m_Data.Specification.Width  = width;
            m_Data.Specification.Height = height;
        }

        // Through the hint, so a hidden window is never on screen for even one frame; reset below with the
        // other sticky hint.
        glfwWindowHint( GLFW_VISIBLE, m_Data.Specification.Visible ? GLFW_TRUE : GLFW_FALSE );
        m_GLFWWindow =
             glfwCreateWindow( (int)width, (int)height, m_Data.Specification.Title.c_str(), nullptr, nullptr );

        // Applied after creation rather than through the hint, and the reason is written out in full over
        // the same call in MacOSWindow::Init — a borderless-at-creation NSWindow loses the resizable style
        // bit, which makes both Maximize and IsWindowMaximized answer wrongly there. Win32 does not have
        // that defect (getWindowStyle recomputes the same WS_POPUP either way), but the two files stay one
        // shape: a platform difference that exists in one of them and not the other is how the pair drifts.
        if ( !wantsFrame && m_GLFWWindow )
        {
            glfwSetWindowAttrib( m_GLFWWindow, GLFW_DECORATED, GLFW_FALSE );
            if ( m_Data.Specification.Fullscreen && !coverTaskbar && m_Data.Specification.Visible )
                glfwMaximizeWindow( m_GLFWWindow );
        }

        if ( setPos && m_GLFWWindow )
            glfwSetWindowPos( m_GLFWWindow, posX, posY );

        // The open size differs from the restore size whenever the window was maximized or undecorated
        // above -> sync the spec to the real client size so the swapchain/camera use the correct
        // dimensions.
        if ( m_GLFWWindow )
        {
            int fw = 0, fh = 0;
            glfwGetWindowSize( m_GLFWWindow, &fw, &fh );
            if ( fw > 0 && fh > 0 )
            {
                m_Data.Specification.Width  = (uint32_t)fw;
                m_Data.Specification.Height = (uint32_t)fh;
            }
        }

        glfwWindowHint( GLFW_MAXIMIZED, GLFW_FALSE ); // reset sticky hint
        glfwWindowHint( GLFW_VISIBLE, GLFW_TRUE );    // and this one

        // EngineContext::GetInstance().m_CurrentWindow = m_GLFWWindow;

        // WRITTEN BACK, because IsDecorated() answers out of this field and a caller asking "do I own the
        // frame?" must get what HAPPENED, not what was requested.
        m_Data.Specification.Decorated = wantsFrame;

        LOG_INFO( "The Window (windows) was created with: Title = {}, Width = {}, Height = {}, Frame = {}",
                  m_Data.Specification.Title.c_str(), m_Data.Specification.Width, m_Data.Specification.Height,
                  wantsFrame ? "system" : "drawn by the application" );

        glfwSetWindowUserPointer( m_GLFWWindow, &m_Data );

        glfwSetWindowCloseCallback( m_GLFWWindow,
                                    []( GLFWwindow* window )
                                    {
                                        auto& data = *(WindowData*)glfwGetWindowUserPointer( window );

                                        Common::EventWindowClose event;
                                        data.EventCallback( event );
                                    } );

        glfwSetWindowSizeCallback( m_GLFWWindow,
                                   []( GLFWwindow* window, int width, int height )
                                   {
                                       auto& data = *( (WindowData*)glfwGetWindowUserPointer( window ) );

                                       Common::EventWindowResize event( (uint32_t)width, (uint32_t)height );
                                       data.EventCallback( event );
                                       data.Specification.Width  = width;
                                       data.Specification.Height = height;
                                   } );

        glfwSetKeyCallback( m_GLFWWindow,
                            []( GLFWwindow* window, int key, int scancode, int action, int mods )
                            {
                                auto& data = *(WindowData*)glfwGetWindowUserPointer( window );

                                switch ( action )
                                {
                                    case GLFW_PRESS:
                                    {
                                        Common::KeyPressedEvent event( (Common::KeyCode)key, 0 );
                                        data.EventCallback( event );
                                        break;
                                    }
                                    case GLFW_REPEAT:
                                    {
                                        Common::KeyPressedEvent event( (Common::KeyCode)key, 1 );
                                        data.EventCallback( event );
                                        break;
                                    }
                                }
                            } );

        glfwSetDropCallback( m_GLFWWindow,
                             []( GLFWwindow* window, int count, const char** paths )
                             {
                                 auto& data = *( (WindowData*)glfwGetWindowUserPointer( window ) );

                                 std::vector<std::string> dropped;
                                 dropped.reserve( count );
                                 for ( int i = 0; i < count; ++i )
                                     dropped.emplace_back( paths[i] );

                                 Common::EventWindowFileDrop event( std::move( dropped ) );
                                 data.EventCallback( event );
                             } );

        glfwSetMouseButtonCallback( m_GLFWWindow,
                                    []( GLFWwindow* window, int button, int action, int mods )
                                    {
                                        auto& data = *( (WindowData*)glfwGetWindowUserPointer( window ) );

                                        switch ( action )
                                        {
                                            case GLFW_PRESS:
                                            {
                                                Common::MouseButtonPressedEvent event(
                                                     (Common::MouseButton)button );
                                                data.EventCallback( event );
                                                break;
                                            }
                                                /* case GLFW_RELEASE:
                                                 {
                                                     Common::MouseButtonReleasedEvent event( button );
                                                     data.EventCallback( event );
                                                     break;
                                                 }*/
                                        }
                                    } );

        m_SwapChain = Graphic::SwapChain::Create( m_GLFWWindow );
        // Hand the requested pacing over BEFORE CreateSwapChain picks a present mode. Without this the
        // swapchain always paced itself to the display and WindowSpecification::VSync did nothing at all.
        m_SwapChain->SetVSync( m_Data.Specification.VSync );

        return Common::MakeSuccess( true );
    }

    WindowsWindow::WindowsWindow( const WindowSpecification& specification )
    {
        m_Data.Specification = specification;
    }

    // The cache catching up, not a second owner of the size — the reason one frame of staleness matters is
    // written out over the same function in MacOSWindow.cpp.
    void WindowsWindow::RefreshCachedSize()
    {
        int w = 0, h = 0;
        glfwGetWindowSize( m_GLFWWindow, &w, &h );
        if ( w > 0 && h > 0 )
        {
            m_Data.Specification.Width  = (uint32_t)w;
            m_Data.Specification.Height = (uint32_t)h;
        }
    }

    void WindowsWindow::SetTitle( const std::string& title )
    {
        m_Data.Specification.Title = title;
        glfwSetWindowTitle( m_GLFWWindow, title.c_str() );
    }

    // THE OS IS TOLD, not just the cache. The version Г12 deleted assigned Width/Height and stopped there
    // — a setter that set nothing. The spec fields are a cache of the OS's answer; the resize callback
    // writes them when the move actually happens.
    void WindowsWindow::SetWindowSize( uint32_t width, uint32_t height )
    {
        glfwSetWindowSize( m_GLFWWindow, (int)width, (int)height );
    }

    void WindowsWindow::SetWindowPos( int x, int y )
    {
        glfwSetWindowPos( m_GLFWWindow, x, y );
    }

    void WindowsWindow::GetWindowPos( int& x, int& y ) const
    {
        glfwGetWindowPos( m_GLFWWindow, &x, &y );
    }

    // glfwMaximizeWindow, not glfwSetWindowMonitor. The deleted version made the window FULLSCREEN on the
    // primary monitor and called it "Maximize": it could not be undone by a restore button, it moved the
    // window to a monitor the user did not pick, and it made IsWindowMaximized (which asked
    // glfwGetWindowMonitor) report "maximized" for a window that was merely fullscreen — two wrong
    // answers that agreed with each other.
    void WindowsWindow::Maximize()
    {
        glfwMaximizeWindow( m_GLFWWindow );
        RefreshCachedSize();
    }

    // THE MAXIMIZE A HIDDEN WINDOW WAS OWED HAPPENS HERE, AFTER IT IS ON SCREEN. Issued on the hidden
    // window (as Init used to, right after taking the frame off) it is a zoom performed on a window the
    // splash is meant to be covering for, and the owner saw the editor's own title-bar buttons appear
    // next to the splash. Shown first, then zoomed: the zoom is a visible window's ordinary maximize, so
    // the OS records the creation size as the restore size exactly as for a click on the button.
    void WindowsWindow::Show()
    {
        glfwShowWindow( m_GLFWWindow );
        if ( m_MaximizeOnShow )
        {
            m_MaximizeOnShow = false;
            glfwMaximizeWindow( m_GLFWWindow );
            RefreshCachedSize();
        }
        glfwFocusWindow( m_GLFWWindow );
    }

    void WindowsWindow::Restore()
    {
        glfwRestoreWindow( m_GLFWWindow );
        RefreshCachedSize();
    }

    void WindowsWindow::Minimize()
    {
        glfwIconifyWindow( m_GLFWWindow );
    }

    bool WindowsWindow::IsWindowMaximized() const
    {
        return glfwGetWindowAttrib( m_GLFWWindow, GLFW_MAXIMIZED ) == GLFW_TRUE;
    }

    bool WindowsWindow::HasDrawableArea() const
    {
        // THE FRAMEBUFFER, NOT THE WINDOW, and not m_Data.Specification either. RefreshCachedSize keeps
        // the last non-degenerate size on purpose, so the cached width and height still read 1280x720 for
        // a window sitting in the taskbar; asking GLFW is what makes this answer the current truth.
        int width  = 0;
        int height = 0;
        glfwGetFramebufferSize( m_GLFWWindow, &width, &height );

        // Negative is not a size GLFW documents, but the cast below would turn one into four billion, and
        // the whole point of IsUsableViewExtent is that both ends of that range are refused.
        if ( width < 0 || height < 0 )
            return false;

        return Graphic::IsUsableViewExtent( static_cast<uint32_t>( width ), static_cast<uint32_t>( height ) );
    }

    uint32_t WindowsWindow::GetWidth() const
    {
        return m_Data.Specification.Width;
    }

    uint32_t WindowsWindow::GetHeight() const
    {
        return m_Data.Specification.Height;
    }

    const void* WindowsWindow::GetNativeWindow() const
    {
        return m_GLFWWindow;
    }

    void WindowsWindow::ProcessEvents()
    {
        glfwPollEvents();
    }

    Common::BoolResultStr WindowsWindow::PresentFinalImage() const
    {
        return Graphic::Renderer::GetInstance().PresentFinalImage();
    }

    Common::BoolResultStr WindowsWindow::PrepareNextFrame() const
    {
        return EngineContext::GetInstance().GetRendererContext()->BeginFrame();
    }

    void WindowsWindow::OnEvent( Common::Event& e )
    {
        Common::EventManager eventManager( e );
        eventManager.Notify<Common::EventWindowResize>( [this]( Common::EventWindowResize& e )
                                                        { return this->OnEventWindowResize( e ); } );
    }

    bool WindowsWindow::OnEventWindowResize( Common::EventWindowResize& e )
    {
        m_SwapChain->OnResize( e.width, e.height );

        return false;
    }

    Common::ResultStr<bool> WindowsWindow::SetupSwapChain()
    {
        const auto device = EngineContext::GetInstance().GetDevice();
        return m_SwapChain->CreateSwapChain( device, &m_Data.Specification.Width, &m_Data.Specification.Height );
    }

    WindowsWindow::~WindowsWindow()
    {
    }

} // namespace Desert::Platform::Windows