// The Windows splash. See SplashScreen.hpp for why it is a native window.
//
// THE WINDOW BELONGS TO THE SPLASH'S OWN THREAD. A Win32 window is serviced by the thread that created
// it, so a window made on the main thread would stop painting (and turn "Not Responding") for exactly as
// long as the start keeps that thread busy — the failure the splash exists to cover. The thread below
// creates the window, runs its message loop, and is the only thing that ever touches it; `SetStatus` and
// `Close` only post messages to it. This is the arrangement UE's FWindowsPlatformSplash uses.
//
// Drawn with plain GDI into a memory bitmap and blitted in one piece, so a repaint never flickers. GDI
// has no alpha for text, so "white at 62 %" is the grey that white at 62 % over black gives, and the soft
// shadow under the small text is the same text in near-black one pixel down.
//
// NOT RUN ON THE MACHINE IT WAS WRITTEN ON (macOS). What CI proves for this file is that it compiles.

#include <Editor/Splash/SplashImage.hpp>
#include <Editor/Splash/SplashLayout.hpp>
#include <Editor/Splash/SplashScreen.hpp>

#include <Common/Core/Logger.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Desert::Editor::Splash
{
    namespace
    {
        constexpr UINT     kStatusMessage = WM_APP + 1;
        constexpr wchar_t  kClassName[]   = L"DesertEngineSplash";
        constexpr COLORREF kBackground    = RGB( 20, 19, 18 );
        constexpr COLORREF kWhite         = RGB( 255, 255, 255 );
        constexpr COLORREF kDim           = RGB( 158, 158, 158 ); // white at kStageAlpha over black
        constexpr COLORREF kShadow        = RGB( 8, 8, 8 );
        constexpr COLORREF kTrack         = RGB( 46, 46, 46 ); // white at kBarTrackAlpha over black
        constexpr COLORREF kSand          = RGB( static_cast<int>( kBarFillRed * 255.0f ),
                                                 static_cast<int>( kBarFillGreen * 255.0f ),
                                                 static_cast<int>( kBarFillBlue * 255.0f ) );

        std::wstring Widen( const std::string& text )
        {
            if ( text.empty() )
                return {};
            const int length = MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
            std::wstring wide( static_cast<std::size_t>( length ), L'\0' );
            MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), wide.data(), length );
            return wide;
        }
    } // namespace

    class SplashScreenWindows final : public SplashScreen
    {
    public:
        explicit SplashScreenWindows( const SplashContent& content ) : m_Content( content )
        {
            m_Thread = std::thread( [this] { Run(); } );
            // Show() promises the window is on screen when it returns.
            std::unique_lock<std::mutex> lock( m_Mutex );
            m_Wake.wait( lock, [this] { return m_Started; } );
        }

        ~SplashScreenWindows() override
        {
            Close();
        }

        void SetStatus( const std::string& label, const std::size_t index, const std::size_t total ) override
        {
            HWND window = nullptr;
            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                m_Status = Status{ label, index, total };
                window   = m_Window;
            }
            if ( window )
                PostMessageW( window, kStatusMessage, 0, 0 );
        }

        void Close() override
        {
            if ( m_Closed )
                return;
            m_Closed = true;

            HWND window = nullptr;
            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                window = m_Window;
            }
            if ( window )
                PostMessageW( window, WM_CLOSE, 0, 0 );
            if ( m_Thread.joinable() )
                m_Thread.join();
        }

    private:
        struct Status
        {
            std::string Label;
            std::size_t Index = 0;
            std::size_t Total = 0;
        };

        static LRESULT CALLBACK WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
        {
            auto* self = reinterpret_cast<SplashScreenWindows*>( GetWindowLongPtrW( window, GWLP_USERDATA ) );
            switch ( message )
            {
                case kStatusMessage:
                    InvalidateRect( window, nullptr, FALSE );
                    return 0;
                case WM_ERASEBKGND:
                    return 1; // the whole client area is painted in WM_PAINT
                case WM_PAINT:
                    if ( self )
                        self->Paint( window );
                    return 0;
                case WM_CLOSE:
                    DestroyWindow( window );
                    return 0;
                case WM_DESTROY:
                    PostQuitMessage( 0 );
                    return 0;
                default:
                    return DefWindowProcW( window, message, wParam, lParam );
            }
        }

        void Run()
        {
            HINSTANCE instance = GetModuleHandleW( nullptr );

            WNDCLASSEXW windowClass   = {};
            windowClass.cbSize        = sizeof( windowClass );
            windowClass.lpfnWndProc   = &SplashScreenWindows::WindowProc;
            windowClass.hInstance     = instance;
            windowClass.hCursor       = LoadCursorW( nullptr, MAKEINTRESOURCEW( 32514 ) ); // IDC_APPSTARTING
            windowClass.lpszClassName = kClassName;
            RegisterClassExW( &windowClass );

            RECT work = {};
            SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
            const float workW = static_cast<float>( work.right - work.left );
            const float workH = static_cast<float>( work.bottom - work.top );
            m_Scale           = FitScale( workW, workH );
            const int w       = static_cast<int>( kWidth * m_Scale );
            const int h       = static_cast<int>( kHeight * m_Scale );
            const int x       = work.left + ( static_cast<int>( workW ) - w ) / 2;
            const int y       = work.top + ( static_cast<int>( workH ) - h ) / 2;

            HWND window = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Desert Engine", WS_POPUP,
                                           x, y, w, h, nullptr, nullptr, instance, nullptr );
            if ( window )
            {
                SetWindowLongPtrW( window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( this ) );
                ShowWindow( window, SW_SHOWNOACTIVATE );
                UpdateWindow( window );
            }
            else
            {
                LOG_WARN( "[Splash] the splash window could not be created (error {}); the start goes on "
                          "without it",
                          static_cast<unsigned long>( GetLastError() ) );
            }

            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                m_Window  = window;
                m_Started = true;
            }
            m_Wake.notify_all();
            if ( !window )
                return;

            // THE PICTURE, after the window is already up with its live text on the dark background.
            auto pixels = LoadSplashPixels( m_Content.CookedImage );
            if ( pixels.IsSuccess() )
            {
                SplashPixels image = pixels.ExtractValue();
                // GDI wants BGRA.
                for ( std::size_t i = 0; i + 3 < image.Rgba.size(); i += 4 )
                    std::swap( image.Rgba[i], image.Rgba[i + 2] );
                LOG_INFO( "[Splash] {}x{} picture decoded from '{}' in {:.1f} ms (BC7 on the CPU, off the main "
                          "thread)",
                          image.Width, image.Height, m_Content.CookedImage.string(), image.DecodeMs );
                {
                    std::lock_guard<std::mutex> lock( m_Mutex );
                    m_Picture = std::move( image );
                }
                InvalidateRect( window, nullptr, FALSE );
            }
            else
            {
                LOG_WARN( "[Splash] no picture, drawing on the plain background: {}", pixels.GetError() );
            }

            MSG message;
            while ( GetMessageW( &message, nullptr, 0, 0 ) > 0 )
            {
                TranslateMessage( &message );
                DispatchMessageW( &message );
            }

            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                m_Window = nullptr;
            }
            UnregisterClassW( kClassName, instance );
        }

        RECT Scaled( const Rect& designFromBottom ) const
        {
            const Rect top = FlipY( designFromBottom );
            RECT       r;
            r.left   = static_cast<LONG>( top.X * m_Scale );
            r.top    = static_cast<LONG>( top.Y * m_Scale );
            r.right  = static_cast<LONG>( ( top.X + top.W ) * m_Scale );
            r.bottom = static_cast<LONG>( ( top.Y + top.H ) * m_Scale );
            // A 2-point bar must stay at least one pixel tall on any screen.
            if ( r.bottom <= r.top )
                r.bottom = r.top + 1;
            return r;
        }

        void Text( HDC dc, HFONT font, const std::wstring& text, const Rect& box, COLORREF colour, UINT align ) const
        {
            if ( text.empty() )
                return;
            SelectObject( dc, font );
            const UINT flags = align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            RECT       shadow = Scaled( box );
            OffsetRect( &shadow, 0, 1 );
            SetTextColor( dc, kShadow );
            DrawTextW( dc, text.c_str(), static_cast<int>( text.size() ), &shadow, flags );
            RECT r = Scaled( box );
            SetTextColor( dc, colour );
            DrawTextW( dc, text.c_str(), static_cast<int>( text.size() ), &r, flags );
        }

        void Paint( HWND window )
        {
            PAINTSTRUCT paint;
            HDC         screen = BeginPaint( window, &paint );
            RECT        client;
            GetClientRect( window, &client );
            const int w = client.right - client.left;
            const int h = client.bottom - client.top;

            HDC     dc     = CreateCompatibleDC( screen );
            HBITMAP canvas = CreateCompatibleBitmap( screen, w, h );
            HGDIOBJ oldBmp = SelectObject( dc, canvas );

            Status status;
            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                status = m_Status;
                if ( m_Picture )
                {
                    BITMAPINFO info              = {};
                    info.bmiHeader.biSize        = sizeof( BITMAPINFOHEADER );
                    info.bmiHeader.biWidth       = static_cast<LONG>( m_Picture->Width );
                    info.bmiHeader.biHeight      = -static_cast<LONG>( m_Picture->Height ); // top row first
                    info.bmiHeader.biPlanes      = 1;
                    info.bmiHeader.biBitCount    = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    SetStretchBltMode( dc, HALFTONE );
                    StretchDIBits( dc, 0, 0, w, h, 0, 0, static_cast<int>( m_Picture->Width ),
                                   static_cast<int>( m_Picture->Height ), m_Picture->Rgba.data(), &info, DIB_RGB_COLORS,
                                   SRCCOPY );
                }
                else
                {
                    HBRUSH background = CreateSolidBrush( kBackground );
                    FillRect( dc, &client, background );
                    DeleteObject( background );
                }
            }

            const Layout layout = ComputeLayout( ProgressFraction( status.Index, status.Total ) );

            const auto makeFont = [this]( const float size, const int weight )
            {
                return CreateFontW( -static_cast<int>( size * m_Scale * 96.0f / 72.0f + 0.5f ), 0, 0, 0, weight, FALSE,
                                    FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI" );
            };
            HFONT projectFont = makeFont( kProjectFontSize, FW_SEMIBOLD );
            HFONT smallFont   = makeFont( kStageFontSize, FW_NORMAL );
            HGDIOBJ oldFont   = SelectObject( dc, smallFont );
            SetBkMode( dc, TRANSPARENT );

            Text( dc, projectFont, Widen( m_Content.ProjectName ), layout.Project, kWhite, DT_LEFT );
            Text( dc, smallFont, Widen( m_Content.Version ), layout.Version, kDim, DT_RIGHT );
            Text( dc, smallFont, Widen( status.Label ), layout.Stage, kDim, DT_LEFT );
            // Segoe UI's digits are tabular already, so the counter does not shift as it counts.
            Text( dc, smallFont, Widen( FormatProgress( status.Index, status.Total ) ), layout.Counter, kDim,
                  DT_RIGHT );

            HBRUSH track = CreateSolidBrush( kTrack );
            RECT   trackRect = Scaled( layout.BarTrack );
            FillRect( dc, &trackRect, track );
            DeleteObject( track );
            if ( layout.BarFill.W > 0.0f )
            {
                HBRUSH sand     = CreateSolidBrush( kSand );
                RECT   fillRect = Scaled( layout.BarFill );
                FillRect( dc, &fillRect, sand );
                DeleteObject( sand );
            }

            BitBlt( screen, 0, 0, w, h, dc, 0, 0, SRCCOPY );

            SelectObject( dc, oldFont );
            DeleteObject( projectFont );
            DeleteObject( smallFont );
            SelectObject( dc, oldBmp );
            DeleteObject( canvas );
            DeleteDC( dc );
            EndPaint( window, &paint );
        }

        SplashContent m_Content;
        float         m_Scale = 1.0f;

        std::thread                 m_Thread;
        mutable std::mutex          m_Mutex;
        std::condition_variable     m_Wake;
        HWND                        m_Window  = nullptr;
        bool                        m_Started = false;
        bool                        m_Closed  = false;
        Status                      m_Status;
        std::optional<SplashPixels> m_Picture;
    };

    std::unique_ptr<SplashScreen> SplashScreen::Show( const SplashContent& content )
    {
        return std::make_unique<SplashScreenWindows>( content );
    }
} // namespace Desert::Editor::Splash
