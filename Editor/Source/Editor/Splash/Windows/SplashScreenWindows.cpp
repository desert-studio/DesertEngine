// The Windows splash. See SplashScreen.hpp for why it is a native window.
//
// THE WINDOW BELONGS TO THE SPLASH'S OWN THREAD. A Win32 window is serviced by the thread that created
// it, so a window made on the main thread would stop painting (and turn "Not Responding") for exactly as
// long as the start keeps that thread busy — the failure the splash exists to cover. The thread below
// creates the window, runs its message loop, and is the only thing that ever touches it; `Close` only posts
// a message to it. This is the arrangement UE's FWindowsPlatformSplash uses.
//
// THE TEXT IS PULLED, NOT PUSHED (STARTUP-PROGRESS, R6). Every kFrameMs the thread samples the start's
// progress (`Core::StartupProgress::Sample`) and repaints, so the phase, the running items with their ages,
// the time left and the bar all move while the main thread is blocked inside one long call. The thread is
// the progress's only sampler, as `Sample` requires.
//
// THE MOTION (SplashLayout.hpp: fade in, crossfade out) is driven by the same timer. The fades are the
// layered window's own alpha.
//
// Drawn with plain GDI into a memory bitmap and blitted in one piece, so a repaint never flickers. GDI
// has no alpha for text, so "white at 62 %" is the grey that white at 62 % over black gives, and the soft
// shadow under the small text is the same text in near-black one pixel down. The picture is scaled to the
// window ONCE into a bitmap of its own: a 30 Hz HALFTONE stretch of the 2400x1350 source would cost the
// splash thread most of a core.

#include <Common/Core/EngineThread.hpp>
#include <Editor/Core/StartupRecord.hpp>
#include <Editor/Splash/SplashControls.hpp>
#include <Editor/Splash/SplashImage.hpp>
#include <Editor/Splash/SplashLayout.hpp>
#include <Editor/Splash/SplashScreen.hpp>

#include <Common/Core/Logger.hpp>

#include <stb_image/stb_image_write.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
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
        constexpr UINT_PTR kFrameTimer      = 1;
        constexpr UINT     kFrameMs         = 33;
        constexpr int64_t  kCapturePeriodMs = 1000;
        constexpr wchar_t  kClassName[]     = L"DesertEngineSplash";
        constexpr COLORREF kBackground      = RGB( 20, 19, 18 );
        constexpr COLORREF kWhite           = RGB( 255, 255, 255 );
        constexpr COLORREF kDim             = RGB( 158, 158, 158 ); // white at kStageAlpha over black
        constexpr COLORREF kFaint           = RGB( 115, 115, 115 ); // white at kItemAlpha / kLogAlpha over black
        constexpr COLORREF kShadow          = RGB( 8, 8, 8 );
        constexpr COLORREF kTrack           = RGB( 46, 46, 46 ); // white at kBarTrackAlpha over black
        constexpr COLORREF kSand =
             RGB( static_cast<int>( kBarFillRed * 255.0f ), static_cast<int>( kBarFillGreen * 255.0f ),
                  static_cast<int>( kBarFillBlue * 255.0f ) );

        std::wstring Widen( const std::string& text )
        {
            if ( text.empty() )
                return {};
            const int length =
                 MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
            std::wstring wide( static_cast<std::size_t>( length ), L'\0' );
            MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), wide.data(), length );
            return wide;
        }

        // Whether pixel (@p x, @p y) of a @p w x @p h box lies inside its rounded rectangle of corner @p r.
        bool InsideRounded( const int x, const int y, const int w, const int h, const float r )
        {
            const float px = static_cast<float>( x ) + 0.5f;
            const float py = static_cast<float>( y ) + 0.5f;
            const float cx = px < r ? r : ( px > static_cast<float>( w ) - r ? static_cast<float>( w ) - r : px );
            const float cy = py < r ? r : ( py > static_cast<float>( h ) - r ? static_cast<float>( h ) - r : py );
            const float dx = px - cx;
            const float dy = py - cy;
            return dx * dx + dy * dy <= r * r;
        }
    } // namespace

    class SplashScreenWindows final : public SplashScreen
    {
    public:
        SplashScreenWindows( const SplashContent& content, std::shared_ptr<Core::StartupProgress> progress )
             : m_Content( content ), m_Progress( std::move( progress ) )
        {
            OpenCapture();
            m_Thread = Common::StartEngineThread( "Splash Screen", Common::ThreadPriority::Normal,
                                                  [this]() noexcept { Run(); } );
            // Show() promises the window is on screen when it returns.
            std::unique_lock<std::mutex> lock( m_Mutex );
            m_Wake.wait( lock, [this] { return m_Started; } );
        }

        ~SplashScreenWindows() override
        {
            Close();
            if ( m_Thread.joinable() )
                m_Thread.join();
        }

        // The main-thread half of the window: none on Windows, where the splash's thread owns all of it.
        void PumpMainThread() override
        {
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
            // Starts the fade out and returns: the splash thread destroys the window when the fade is
            // over, and the destructor joins it.
            if ( window )
                PostMessageW( window, WM_CLOSE, 0, 0 );
        }

    private:
        static LRESULT CALLBACK WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
        {
            auto* self = reinterpret_cast<SplashScreenWindows*>( GetWindowLongPtrW( window, GWLP_USERDATA ) );
            switch ( message )
            {
                case WM_NCHITTEST:
                {
                    // THE PICTURE MOVES THE WINDOW: everything but the two buttons answers as a caption, so
                    // the system drags the splash on this thread, as the editor's title bar drags the editor.
                    const LRESULT hit = DefWindowProcW( window, message, wParam, lParam );
                    if ( hit != HTCLIENT || !self )
                        return hit;
                    POINT at{ static_cast<short>( LOWORD( lParam ) ), static_cast<short>( HIWORD( lParam ) ) };
                    ScreenToClient( window, &at );
                    return self->ButtonAt( at.x, at.y ) == SplashButton::None ? HTCAPTION : HTCLIENT;
                }
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                    if ( self )
                        self->Pointer( window, static_cast<short>( LOWORD( lParam ) ),
                                       static_cast<short>( HIWORD( lParam ) ), message );
                    return 0;
                case WM_MOUSELEAVE:
                    if ( self )
                        self->Pointer( window, -1, -1, message );
                    return 0;
                case WM_ERASEBKGND:
                    return 1; // the whole client area is painted in WM_PAINT
                case WM_PAINT:
                    if ( self )
                        self->Paint( window );
                    return 0;
                case WM_TIMER:
                    if ( self )
                        self->Tick( window );
                    return 0;
                case WM_CLOSE:
                    // Not destroyed here: the fade out runs first, and Tick destroys it at the end.
                    if ( self && !self->m_Closing )
                    {
                        self->m_Closing   = true;
                        self->m_ClosingAt = std::chrono::steady_clock::now();
                    }
                    return 0;
                case WM_DESTROY:
                    if ( self )
                        self->ReleaseBackground();
                    PostQuitMessage( 0 );
                    return 0;
                default:
                    return DefWindowProcW( window, message, wParam, lParam );
            }
        }

        // --splash-capture: the folder and the CSV are made before the window, so a refusal is said once and
        // the start goes on without the capture.
        void OpenCapture()
        {
            if ( m_Content.CaptureDir.empty() )
                return;
            std::error_code error;
            std::filesystem::create_directories( m_Content.CaptureDir, error );
            if ( error )
            {
                LOG_WARN( "[Splash] capture folder '{}' could not be made ({}); the splash is not captured",
                          m_Content.CaptureDir.string(), error.message() );
                return;
            }
            const std::filesystem::path csv = m_Content.CaptureDir / "progress.csv";
            m_Csv.open( csv, std::ios::out | std::ios::trunc );
            if ( !m_Csv )
            {
                LOG_WARN( "[Splash] '{}' could not be opened for writing; the splash is not captured",
                          csv.string() );
                return;
            }
            m_Csv << kCaptureCsvHeader << '\n';
            m_Capturing = true;
            LOG_INFO( "[Splash] capturing into '{}' (splash-<ms>.png every {} ms, progress.csv per repaint)",
                      m_Content.CaptureDir.string(), kCapturePeriodMs );
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

            // The first sample before the first paint, so the window never shows an empty frame.
            m_View = m_Progress->Sample( Core::StartupClock::now() );

            HWND window =
                 // An APP window with a minimize box, not a tool window: minimized, it has a taskbar button to
                 // come back from.
                 CreateWindowExW( WS_EX_APPWINDOW | WS_EX_LAYERED, kClassName, L"Desert Engine",
                                  WS_POPUP | WS_MINIMIZEBOX, x, y, w, h, nullptr, nullptr, instance, nullptr );
            const std::wstring iconFont = UI::IconFontFile().wstring();
            m_IconFontLoaded            = AddFontResourceExW( iconFont.c_str(), FR_PRIVATE, nullptr ) > 0;
            if ( !m_IconFontLoaded )
                LOG_WARN( "[Splash] icon font '{}' could not be loaded; the window buttons draw system characters",
                          UI::IconFontFile().string() );
            if ( window )
            {
                SetWindowLongPtrW( window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( this ) );
                // Fully transparent at first: the fade in is the first thing the timer does.
                SetLayeredWindowAttributes( window, 0, 0, LWA_ALPHA );
                m_ShownAt = std::chrono::steady_clock::now();
                SetTimer( window, kFrameTimer, kFrameMs, nullptr );
                // In front ONCE, as it appears; not TOPMOST — the owner switches to other windows while the
                // editor loads, and a splash that stays above them is in the way.
                ShowWindow( window, SW_SHOW );
                SetForegroundWindow( window );
                UpdateWindow( window );

                // How early, from the kernel's record of when this process began (Editor/Core/StartupRecord.hpp,
                // the one copy of that clock).
                if ( const auto started = ProcessStartTime() )
                    LOG_INFO( "[Splash] on screen {:.0f} ms after the process started",
                              std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() -
                                                                         started.GetValue() )
                                   .count() );
                else
                    LOG_WARN( "[Splash] on screen; the process start could not be read ({})", started.GetError() );
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

            // THE PICTURE, after the window is already up with its live text on the dark background — and
            // on a thread of its own, because this one has to keep pumping: the fade in is a timer
            // message, and a decode run here (1.2 s in Debug) would hold the window invisible. The next
            // timer tick repaints with it.
            std::thread decoder = Common::StartEngineThread(
                 "Splash Decoder", Common::ThreadPriority::Normal,
                 [this]() noexcept
                 {
                     auto pixels = LoadSplashPixels( m_Content.CookedImage );
                     if ( !pixels.IsSuccess() )
                     {
                         LOG_WARN( "[Splash] no picture, drawing on the plain background: {}", pixels.GetError() );
                         return;
                     }
                     SplashPixels image = pixels.ExtractValue();
                     // GDI wants BGRA.
                     for ( std::size_t i = 0; i + 3 < image.Rgba.size(); i += 4 )
                         std::swap( image.Rgba[i], image.Rgba[i + 2] );
                     LOG_INFO(
                          "[Splash] {}x{} picture decoded from '{}' in {:.1f} ms (BC7 on the CPU, off the main "
                          "thread)",
                          image.Width, image.Height, m_Content.CookedImage.string(), image.DecodeMs );
                     std::lock_guard<std::mutex> lock( m_Mutex );
                     m_Picture = std::move( image );
                 } );

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
            decoder.join();
            UnregisterClassW( kClassName, instance );
        }

        static float SecondsSince( const std::chrono::steady_clock::time_point then )
        {
            return std::chrono::duration<float>( std::chrono::steady_clock::now() - then ).count();
        }

        // Every kFrameMs: the fade, then one sample of the progress and a repaint with it.
        void Tick( HWND window )
        {
            const float opacity = m_Closing ? SplashOpacity( SecondsSince( m_ClosingAt ), true )
                                            : SplashOpacity( SecondsSince( m_ShownAt ), false );
            SetLayeredWindowAttributes( window, 0, static_cast<BYTE>( opacity * 255.0f + 0.5f ), LWA_ALPHA );
            if ( m_Closing && opacity <= 0.0f )
            {
                KillTimer( window, kFrameTimer );
                DestroyWindow( window );
                return;
            }

            m_View = m_Progress->Sample( Core::StartupClock::now() );
            if ( m_Capturing )
                AppendCsvRow();
            InvalidateRect( window, nullptr, FALSE );
        }

        // One row per sample: what N1..N5 of the acceptance are read from.
        void AppendCsvRow()
        {
            m_Csv << CaptureCsvRow( m_View ) << '\n';
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

        void Text( HDC dc, HFONT font, const std::wstring& text, const Rect& box, COLORREF colour,
                   UINT align ) const
        {
            if ( text.empty() )
                return;
            SelectObject( dc, font );
            const UINT flags  = align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            RECT       shadow = Scaled( box );
            OffsetRect( &shadow, 0, 1 );
            SetTextColor( dc, kShadow );
            DrawTextW( dc, text.c_str(), static_cast<int>( text.size() ), &shadow, flags );
            RECT r = Scaled( box );
            SetTextColor( dc, colour );
            DrawTextW( dc, text.c_str(), static_cast<int>( text.size() ), &r, flags );
        }

        // The picture scaled to the window, made once when it has arrived; the plain background until then.
        void DrawBackground( HDC dc, HDC screen, const RECT& client, const int w, const int h )
        {
            if ( !m_Background )
            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                if ( m_Picture )
                {
                    m_Background   = CreateCompatibleBitmap( screen, w, h );
                    HDC     bg     = CreateCompatibleDC( screen );
                    HGDIOBJ oldBmp = SelectObject( bg, m_Background );

                    BITMAPINFO info              = {};
                    info.bmiHeader.biSize        = sizeof( BITMAPINFOHEADER );
                    info.bmiHeader.biWidth       = static_cast<LONG>( m_Picture->Width );
                    info.bmiHeader.biHeight      = -static_cast<LONG>( m_Picture->Height ); // top row first
                    info.bmiHeader.biPlanes      = 1;
                    info.bmiHeader.biBitCount    = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    SetStretchBltMode( bg, HALFTONE );
                    SetBrushOrgEx( bg, 0, 0, nullptr );
                    StretchDIBits( bg, 0, 0, w, h, 0, 0, static_cast<int>( m_Picture->Width ),
                                   static_cast<int>( m_Picture->Height ), m_Picture->Rgba.data(), &info,
                                   DIB_RGB_COLORS, SRCCOPY );
                    SelectObject( bg, oldBmp );
                    DeleteDC( bg );
                    // The source is not needed again: the window's size does not change.
                    m_Picture.reset();
                }
            }
            if ( m_Background )
            {
                HDC     bg     = CreateCompatibleDC( screen );
                HGDIOBJ oldBmp = SelectObject( bg, m_Background );
                BitBlt( dc, 0, 0, w, h, bg, 0, 0, SRCCOPY );
                SelectObject( bg, oldBmp );
                DeleteDC( bg );
                return;
            }
            HBRUSH background = CreateSolidBrush( kBackground );
            FillRect( dc, &client, background );
            DeleteObject( background );
        }

        void ReleaseBackground()
        {
            if ( m_Background )
                DeleteObject( m_Background );
            m_Background = nullptr;
        }

        // The detail log's backing: black at LogPanelAlpha with rounded corners, as one premultiplied
        // per-pixel-alpha blend (GDI's only way to blend a shape that is not a rectangle).
        void PaintLogPanel( HDC dc, const Layout& layout ) const
        {
            const RECT box = Scaled( layout.LogPanel );
            const int  w   = box.right - box.left;
            const int  h   = box.bottom - box.top;
            if ( w <= 0 || h <= 0 )
                return;
            BITMAPINFO info              = {};
            info.bmiHeader.biSize        = sizeof( BITMAPINFOHEADER );
            info.bmiHeader.biWidth       = w;
            info.bmiHeader.biHeight      = -h;
            info.bmiHeader.biPlanes      = 1;
            info.bmiHeader.biBitCount    = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void*   bits                 = nullptr;
            HBITMAP panel                = CreateDIBSection( dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0 );
            if ( !panel || !bits )
                return;
            auto*       pixels = static_cast<uint32_t*>( bits );
            const auto  alpha  = static_cast<uint32_t>( layout.LogPanelAlpha * 255.0f + 0.5f );
            const float radius = kLogPanelRadius * m_Scale;
            for ( int y = 0; y < h; ++y )
                for ( int x = 0; x < w; ++x )
                    // Black premultiplied by its alpha is black: only the alpha byte is set.
                    pixels[static_cast<std::size_t>( y ) * static_cast<std::size_t>( w ) +
                           static_cast<std::size_t>( x )] =
                         InsideRounded( x, y, w, h, radius ) ? ( alpha << 24 ) : 0u;
            HDC           panelDc = CreateCompatibleDC( dc );
            HGDIOBJ       oldBmp  = SelectObject( panelDc, panel );
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            GdiAlphaBlend( dc, box.left, box.top, w, h, panelDc, 0, 0, w, h, blend );
            SelectObject( panelDc, oldBmp );
            DeleteObject( panel );
            DeleteDC( panelDc );
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

            DrawBackground( dc, screen, client, w, h );

            const Core::StartupView& view   = m_View;
            const Layout             layout = ComputeLayout( view.Fraction );

            const auto makeFont = [this]( const float size, const int weight )
            {
                return CreateFontW( -static_cast<int>( size * m_Scale * 96.0f / 72.0f + 0.5f ), 0, 0, 0, weight,
                                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI" );
            };
            HFONT   projectFont = makeFont( kProjectFontSize, FW_SEMIBOLD );
            HFONT   smallFont   = makeFont( kStageFontSize, FW_NORMAL );
            HFONT   itemFont    = makeFont( kItemFontSize, FW_NORMAL );
            HFONT   logFont     = makeFont( kLogFontSize, FW_NORMAL );
            HGDIOBJ oldFont     = SelectObject( dc, smallFont );
            SetBkMode( dc, TRANSPARENT );

            Text( dc, projectFont, Widen( m_Content.ProjectName ), layout.Project, kWhite, DT_LEFT );
            Text( dc, smallFont, Widen( m_Content.Version ), layout.Version, kDim, DT_RIGHT );
            Text( dc, smallFont, Widen( StageText( view ) ), layout.Stage, kDim, DT_LEFT );
            // Segoe UI's digits are tabular already, so the percentage and the times do not shift as they
            // count.
            Text( dc, smallFont, Widen( PercentText( view ) ), layout.Percent, kDim, DT_RIGHT );
            Text( dc, itemFont, Widen( ItemText( view ) ), layout.Item, kFaint, DT_LEFT );
            Text( dc, itemFont, Widen( TimeText( view ) ), layout.Time, kFaint, DT_RIGHT );

            if ( !view.Log.empty() )
            {
                PaintLogPanel( dc, layout );
                for ( std::size_t row = 0; row < kLogLines; ++row )
                    Text( dc, logFont, Widen( LogRowText( view, row ) ), layout.LogLines[row], kFaint, DT_LEFT );
            }

            HBRUSH track     = CreateSolidBrush( kTrack );
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

            PaintButtons( dc );
            BitBlt( screen, 0, 0, w, h, dc, 0, 0, SRCCOPY );

            SelectObject( dc, oldFont );
            DeleteObject( projectFont );
            DeleteObject( smallFont );
            DeleteObject( itemFont );
            DeleteObject( logFont );
            SelectObject( dc, oldBmp );
            // GetDIBits wants the bitmap out of every device context, so the capture reads it here.
            CaptureIfDue( dc, canvas, w, h );
            DeleteObject( canvas );
            DeleteDC( dc );
            EndPaint( window, &paint );
        }

        // --splash-capture: the painted canvas (window pixels, the design times m_Scale) as
        // splash-<ms>.png once per kCapturePeriodMs, <ms> since the process started.
        void CaptureIfDue( HDC dc, HBITMAP canvas, const int w, const int h )
        {
            if ( !m_Capturing || w <= 0 || h <= 0 )
                return;
            const auto atMs = static_cast<int64_t>( m_View.ElapsedSeconds * 1000.0 );
            if ( m_LastCaptureMs >= 0 && atMs - m_LastCaptureMs < kCapturePeriodMs )
                return;
            m_LastCaptureMs = atMs;

            BITMAPINFO info              = {};
            info.bmiHeader.biSize        = sizeof( BITMAPINFOHEADER );
            info.bmiHeader.biWidth       = w;
            info.bmiHeader.biHeight      = -h; // top row first, as PNG wants
            info.bmiHeader.biPlanes      = 1;
            info.bmiHeader.biBitCount    = 32;
            info.bmiHeader.biCompression = BI_RGB;
            std::vector<unsigned char> pixels( static_cast<std::size_t>( w ) * static_cast<std::size_t>( h ) *
                                               4u );
            if ( GetDIBits( dc, canvas, 0, static_cast<UINT>( h ), pixels.data(), &info, DIB_RGB_COLORS ) != h )
            {
                LOG_WARN( "[Splash] capture at {} ms: the canvas could not be read (error {})", atMs,
                          static_cast<unsigned long>( GetLastError() ) );
                return;
            }
            for ( std::size_t i = 0; i + 3 < pixels.size(); i += 4 )
            {
                std::swap( pixels[i], pixels[i + 2] ); // BGRA -> RGBA
                pixels[i + 3] = 255;                   // GDI leaves the alpha byte undefined
            }
            const std::filesystem::path file =
                 m_Content.CaptureDir / ( "splash-" + std::to_string( atMs ) + ".png" );
            if ( stbi_write_png( file.string().c_str(), w, h, 4, pixels.data(), w * 4 ) == 0 )
                LOG_WARN( "[Splash] capture '{}' could not be written", file.string() );
            m_Csv.flush();
        }

        RECT ToPixels( const Rect& rect ) const
        {
            const Rect flipped = FlipY( rect );
            return { static_cast<LONG>( flipped.X * m_Scale ), static_cast<LONG>( flipped.Y * m_Scale ),
                     static_cast<LONG>( ( flipped.X + flipped.W ) * m_Scale ),
                     static_cast<LONG>( ( flipped.Y + flipped.H ) * m_Scale ) };
        }

        SplashButton ButtonAt( const int x, const int y ) const
        {
            return HitTestButtons( static_cast<float>( x ) / m_Scale,
                                   kHeight - static_cast<float>( y ) / m_Scale );
        }

        // Every pointer message, on the splash's thread (which owns the window): hover, pressed, click.
        void Pointer( HWND window, const int x, const int y, const UINT message )
        {
            if ( message == WM_MOUSEMOVE && !m_TrackingLeave )
            {
                TRACKMOUSEEVENT track{ sizeof( TRACKMOUSEEVENT ), TME_LEAVE, window, 0 };
                m_TrackingLeave = TrackMouseEvent( &track ) != FALSE;
            }
            if ( message == WM_MOUSELEAVE )
                m_TrackingLeave = false;
            if ( message == WM_LBUTTONDOWN )
                SetCapture( window );
            if ( message == WM_LBUTTONUP )
                ReleaseCapture();

            const SplashButton under = message == WM_MOUSELEAVE ? SplashButton::None : ButtonAt( x, y );
            const bool         down  = ( message == WM_LBUTTONDOWN ) ||
                              ( message == WM_MOUSEMOVE && ( GetKeyState( VK_LBUTTON ) & 0x8000 ) != 0 );
            const SplashButton clicked = m_Buttons.Update( under, down );
            if ( clicked == SplashButton::Close && !m_Progress->StopRequested() )
            {
                // The start's one stop flag: the long calls ask it between their items.
                LOG_INFO( "[Splash] close clicked; the editor stops loading and exits" );
                m_Progress->RequestStop();
            }
            else if ( clicked == SplashButton::Minimize )
                ShowWindow( window, SW_MINIMIZE );
            InvalidateRect( window, nullptr, FALSE );
        }

        // The editor frame's two buttons (WindowButtonStyle.hpp): the square blended over the picture at
        // the shared colour's alpha, the glyph from the icon font.
        void PaintButtons( HDC dc )
        {
            const int glyphPx = -static_cast<int>( kButtonGlyphSize * m_Scale * 96.0f / 72.0f + 0.5f );
            HFONT     font    = CreateFontW( glyphPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                      m_IconFontLoaded ? L"Material Design Icons" : L"Segoe UI" );
            HGDIOBJ   old     = SelectObject( dc, font );
            SetBkMode( dc, TRANSPARENT );
            SetTextColor( dc, RGB( 230, 230, 230 ) );
            for ( const SplashButton button : { SplashButton::Minimize, SplashButton::Close } )
            {
                RECT                   box    = ToPixels( ButtonRect( button ) );
                const UI::ButtonColour colour = ButtonFill( button, m_Buttons );
                if ( colour.A > 0.0f )
                {
                    // One pixel of the colour, stretched and blended: GDI's only per-call alpha fill.
                    HDC     pixelDc = CreateCompatibleDC( dc );
                    HBITMAP pixel   = CreateCompatibleBitmap( dc, 1, 1 );
                    HGDIOBJ oldBmp  = SelectObject( pixelDc, pixel );
                    SetPixel( pixelDc, 0, 0,
                              RGB( static_cast<BYTE>( colour.R * 255.0f ), static_cast<BYTE>( colour.G * 255.0f ),
                                   static_cast<BYTE>( colour.B * 255.0f ) ) );
                    BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>( colour.A * 255.0f ), 0 };
                    GdiAlphaBlend( dc, box.left, box.top, box.right - box.left, box.bottom - box.top, pixelDc, 0,
                                   0, 1, 1, blend );
                    SelectObject( pixelDc, oldBmp );
                    DeleteObject( pixel );
                    DeleteDC( pixelDc );
                }
                const bool   close = button == SplashButton::Close;
                std::wstring glyph = L"\u2013";
                if ( m_IconFontLoaded )
                {
                    const char* utf8 = close ? UI::kCloseGlyph : UI::kMinimizeGlyph;
                    wchar_t     wide[4]{};
                    const int   n = MultiByteToWideChar( CP_UTF8, 0, utf8, -1, wide, 4 );
                    glyph         = std::wstring( wide, n > 0 ? static_cast<size_t>( n - 1 ) : 0 );
                }
                else if ( close )
                    glyph = L"\u2715";
                DrawTextW( dc, glyph.c_str(), static_cast<int>( glyph.size() ), &box,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
            }
            SelectObject( dc, old );
            DeleteObject( font );
        }

        // Touched by the splash's thread only.
        ButtonTracker m_Buttons;
        bool          m_TrackingLeave  = false;
        bool          m_IconFontLoaded = false;

        SplashContent                          m_Content;
        std::shared_ptr<Core::StartupProgress> m_Progress; // sampled by the splash's thread only
        float                                  m_Scale = 1.0f;

        std::thread             m_Thread;
        mutable std::mutex      m_Mutex;
        std::condition_variable m_Wake;
        HWND                    m_Window  = nullptr;
        bool                    m_Started = false;
        bool                    m_Closed  = false;
        // Under m_Mutex: written by the decoder thread, taken by the splash thread.
        std::optional<SplashPixels> m_Picture;
        // Splash thread only.
        std::chrono::steady_clock::time_point m_ShownAt;
        std::chrono::steady_clock::time_point m_ClosingAt;
        bool                                  m_Closing = false;
        Core::StartupView                     m_View;
        HBITMAP                               m_Background = nullptr;
        // --splash-capture (opened on the main thread before the splash's thread starts, then its only).
        std::ofstream m_Csv;
        bool          m_Capturing     = false;
        int64_t       m_LastCaptureMs = -1;
    };

    std::unique_ptr<SplashScreen> SplashScreen::Show( const SplashContent&                   content,
                                                      std::shared_ptr<Core::StartupProgress> progress )
    {
        return std::make_unique<SplashScreenWindows>( content, std::move( progress ) );
    }
} // namespace Desert::Editor::Splash
