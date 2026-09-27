#include "NativeFrame.hpp"

#if defined( _WIN32 )

#define WIN32_LEAN_AND_MEAN
#if !defined( NOMINMAX )
#define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <dwmapi.h>

#include <cstdio>

namespace
{
    // The Windows 11 rounding attribute. Spelled out because the Windows SDK this project builds
    // against may predate it; the values are fixed by the OS ABI.
    constexpr DWORD kDwmWindowCornerPreference = 33;
    constexpr DWORD kDwmCornerRound            = 2;

    // ONE window, ONE state. The crash reporter shows exactly one window and then exits, so a map
    // keyed by HWND would be ceremony around a single row. The wndproc asserts the identity below
    // rather than assuming it.
    struct FrameState
    {
        HWND                                      window   = nullptr;
        WNDPROC                                   previous = nullptr;
        CrashReporter::NativeFrame::CaptionLayout layout{};
    };

    FrameState gFrame;

    bool IsWindowMaximized( HWND inWindow )
    {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof( placement );
        if ( ::GetWindowPlacement( inWindow, &placement ) == FALSE )
        {
            return false;
        }
        return placement.showCmd == SW_SHOWMAXIMIZED;
    }

    LRESULT HitTest( HWND inWindow, LPARAM inParam )
    {
        POINT point = { GET_X_LPARAM( inParam ), GET_Y_LPARAM( inParam ) };
        ::ScreenToClient( inWindow, &point );

        RECT client{};
        ::GetClientRect( inWindow, &client );
        const LONG width  = client.right - client.left;
        const LONG height = client.bottom - client.top;

        const LONG border = static_cast<LONG>( gFrame.layout.borderWidth );
        // A maximised window has no edges to drag; offering them would resize it off the work area.
        if ( border > 0 && !IsWindowMaximized( inWindow ) )
        {
            const bool onLeft   = point.x < border;
            const bool onRight  = point.x >= width - border;
            const bool onTop    = point.y < border;
            const bool onBottom = point.y >= height - border;

            if ( onTop && onLeft )
            {
                return HTTOPLEFT;
            }
            if ( onTop && onRight )
            {
                return HTTOPRIGHT;
            }
            if ( onBottom && onLeft )
            {
                return HTBOTTOMLEFT;
            }
            if ( onBottom && onRight )
            {
                return HTBOTTOMRIGHT;
            }
            if ( onLeft )
            {
                return HTLEFT;
            }
            if ( onRight )
            {
                return HTRIGHT;
            }
            if ( onTop )
            {
                return HTTOP;
            }
            if ( onBottom )
            {
                return HTBOTTOM;
            }
        }

        // The caption strip minus our own buttons. HTCAPTION is what buys drag, Aero snap and
        // double-click-to-maximise for free; the button strip must stay HTCLIENT or the OS consumes
        // the click and ImGui never sees it.
        const LONG captionHeight = static_cast<LONG>( gFrame.layout.captionHeight );
        const LONG buttonsWidth  = static_cast<LONG>( gFrame.layout.buttonsWidth );
        if ( point.y >= 0 && point.y < captionHeight && point.x < width - buttonsWidth )
        {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    LRESULT CALLBACK FrameWindowProc( HWND inWindow, UINT inMessage, WPARAM inWParam, LPARAM inLParam )
    {
        if ( inWindow == gFrame.window && gFrame.previous != nullptr )
        {
            switch ( inMessage )
            {
                case WM_NCCALCSIZE:
                    // The whole point of the exercise: the window keeps a real frame (so the OS still
                    // gives it snap, resize, shadow and rounded corners) but that frame reserves no
                    // non-client area, so our ImGui bar is drawn all the way to the top edge.
                    if ( inWParam == TRUE )
                    {
                        return 0;
                    }
                    break;

                case WM_NCHITTEST:
                    return HitTest( inWindow, inLParam );

                default:
                    break;
            }
        }
        // EVERY other message goes on to GLFW's own proc. GLFW tracks focus, size, cursor, keyboard
        // and the close request in there; swallowing anything breaks input in ways that only show up
        // later.
        return ::CallWindowProcW( gFrame.previous, inWindow, inMessage, inWParam, inLParam );
    }
} // namespace

namespace CrashReporter::NativeFrame
{
    std::string Install( GLFWwindow* inWindow )
    {
        if ( inWindow == nullptr )
        {
            return "NativeFrame::Install was given no window\n";
        }

        HWND handle = ::glfwGetWin32Window( inWindow );
        if ( handle == nullptr )
        {
            return "glfwGetWin32Window returned no HWND; the title bar stays inert\n";
        }

        std::string problems;

        // GLFW creates an undecorated window as WS_POPUP, which has no sizing frame and no maximise
        // box - so no edge resize and no Aero snap however we answer WM_NCHITTEST. Putting the frame
        // styles back and then zeroing the non-client area in WM_NCCALCSIZE is what gives a
        // borderless window the full native window behaviour.
        const LONG_PTR style = ::GetWindowLongPtrW( handle, GWL_STYLE );
        ::SetLastError( 0 );
        if ( ::SetWindowLongPtrW( handle, GWL_STYLE,
                                  style | WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX |
                                       WS_SYSMENU ) == 0 &&
             ::GetLastError() != 0 )
        {
            problems += "SetWindowLongPtrW(GWL_STYLE) failed with Win32 error " +
                        std::to_string( ::GetLastError() ) + "; the window cannot be resized\n";
        }

        gFrame.window = handle;
        ::SetLastError( 0 );
        gFrame.previous = reinterpret_cast<WNDPROC>(
             ::SetWindowLongPtrW( handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>( &FrameWindowProc ) ) );
        if ( gFrame.previous == nullptr )
        {
            gFrame.window = nullptr;
            problems += "SetWindowLongPtrW(GWLP_WNDPROC) failed with Win32 error " +
                        std::to_string( ::GetLastError() ) + "; the title bar cannot drag or resize\n";
        }

        // SWP_FRAMECHANGED is what makes the style change take effect; without it the first
        // WM_NCCALCSIZE never arrives and the window keeps the old frame until it is moved.
        ::SetWindowPos( handle, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );

        const DWORD   corner = kDwmCornerRound;
        const HRESULT result =
             ::DwmSetWindowAttribute( handle, kDwmWindowCornerPreference, &corner, sizeof( corner ) );
        // E_INVALIDARG is exactly what every Windows build before 11 returns for an attribute it does
        // not know. Square corners there are the OS's own look, not a defect of this tool, so that one
        // code is expected; anything else is reported.
        if ( FAILED( result ) && result != E_INVALIDARG )
        {
            char text[96] = {};
            std::snprintf( text, sizeof( text ),
                           "DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE) failed, HRESULT 0x%08lX\n",
                           static_cast<unsigned long>( result ) );
            problems += text;
        }

        return problems;
    }

    void SetLayout( const CaptionLayout& inLayout )
    {
        gFrame.layout = inLayout;
    }
} // namespace CrashReporter::NativeFrame

#else

namespace CrashReporter::NativeFrame
{
    std::string Install( GLFWwindow* )
    {
        // Named rather than silently ignored: on a platform without this half, the bar draws but does
        // not drag, and the window must say so instead of looking broken for no stated reason.
        return "a borderless window with a draggable caption is implemented for Windows only\n";
    }

    void SetLayout( const CaptionLayout& )
    {
    }
} // namespace CrashReporter::NativeFrame

#endif
