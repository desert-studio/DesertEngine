// The macOS splash. See SplashScreen.hpp for why it is a native window and why this arrangement.
//
// THE ONE RULE THIS FILE LIVES BY: after `Show` returns, nothing here needs the main thread's run loop.
// The window is created and ordered front on the main thread (AppKit's requirement), and every later
// change — the picture, the status text, the bar — is a Core Animation layer property set on the
// splash's own thread inside an explicit `[CATransaction begin] ... commit` followed by `flush`. That
// pushes the change to WindowServer, which composites it whether or not the application ever returns to
// its event loop. An IMPLICIT transaction (a property set with no begin/commit) is committed only at the
// end of a run-loop turn, and the splash's thread has no run loop: measured with the probe described in
// SplashScreen.hpp, the implicit form never reached the screen at all.
//
// Manual retain/release: this target is not built with -fobjc-arc (neither is Common's MacOS code).

#include <Editor/Splash/SplashControls.hpp>
#include <Editor/Splash/SplashImage.hpp>
#include <Editor/Splash/SplashLayout.hpp>
#include <Editor/Splash/SplashScreen.hpp>

#include <Common/Core/Logger.hpp>

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>

#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>

// A patch of the splash a mouse-down cannot drag the window by: the two buttons. Everything else of the
// splash moves the window (movableByWindowBackground), the way the editor's title bar does.
@interface DesertSplashButtonArea : NSView
@end
@implementation DesertSplashButtonArea
- (BOOL)mouseDownCanMoveWindow
{
    return NO;
}
@end

namespace Desert::Editor::Splash
{
    namespace
    {
        CGColorRef MakeColour( const CGFloat r, const CGFloat g, const CGFloat b, const CGFloat a )
        {
            return CGColorCreateSRGB( r, g, b, a );
        }

        CGRect ToCG( const Rect& rect )
        {
            return CGRectMake( rect.X, rect.Y, rect.W, rect.H );
        }

        // A text layer for one line of the live text, with the soft dark shadow under it that keeps small
        // white type legible over the brightest part of the sky.
        CATextLayer* MakeTextLayer( CALayer* parent, NSFont* font, const CGFloat alpha, NSString* alignment,
                                    const CGFloat scale )
        {
            CATextLayer* layer    = [[CATextLayer alloc] init];
            layer.font            = (__bridge CFTypeRef)font;
            layer.fontSize        = font.pointSize;
            CGColorRef colour     = MakeColour( 1, 1, 1, alpha );
            layer.foregroundColor = colour;
            CGColorRelease( colour );
            layer.alignmentMode  = alignment;
            layer.truncationMode = kCATruncationEnd;
            layer.contentsScale  = scale;
            CGColorRef shadow    = MakeColour( 0, 0, 0, 1 );
            layer.shadowColor    = shadow;
            CGColorRelease( shadow );
            layer.shadowOpacity = 0.55f;
            layer.shadowRadius  = 3.0;
            layer.shadowOffset  = CGSizeMake( 0, -1 );
            [parent addSublayer:layer];
            return layer; // +1, released in the destructor
        }

        NSString* ToNS( const std::string& text )
        {
            NSString* string = [NSString stringWithUTF8String:text.c_str()];
            return ( string != nullptr ) ? string : @"";
        }

        // The editor's icon font, loaded from its file (it is not installed in the system). Null, and said
        // once, when the file is missing: the buttons then draw the system font's closest characters.
        CTFontRef LoadIconFont( const CGFloat size )
        {
            NSURL*     url         = [NSURL fileURLWithPath:ToNS( UI::kIconFontFile.string() )];
            CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromURL( (__bridge CFURLRef)url );
            CTFontRef  font        = nullptr;
            if ( descriptors != nullptr && CFArrayGetCount( descriptors ) > 0 )
                font = CTFontCreateWithFontDescriptor(
                     (CTFontDescriptorRef)CFArrayGetValueAtIndex( descriptors, 0 ), size, nullptr );
            if ( descriptors != nullptr )
                CFRelease( descriptors );
            if ( font == nullptr )
                LOG_WARN( "[Splash] icon font '{}' could not be read; the window buttons draw system characters",
                          UI::kIconFontFile.string() );
            return font; // +1 or null
        }

        void SetFill( CALayer* layer, const UI::ButtonColour& colour )
        {
            CGColorRef fill       = MakeColour( colour.R, colour.G, colour.B, colour.A );
            layer.backgroundColor = fill;
            CGColorRelease( fill );
        }

        // Where the pointer is and whether its left button is down, asked of the window server rather than
        // of the event stream: the event stream is the main thread's, and the main thread is loading.
        struct Pointer
        {
            CGPoint Location{};
            bool    Down = false;
        };
        Pointer ReadPointer()
        {
            Pointer    pointer;
            CGEventRef event = CGEventCreate( nullptr );
            if ( event != nullptr )
            {
                pointer.Location = CGEventGetLocation( event );
                CFRelease( event );
            }
            pointer.Down = CGEventSourceButtonState( kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft );
            return pointer;
        }

        // The window's frame as the window server has it now — top-origin, points — including every move a
        // drag made since. Nothing when it is not on screen (minimized).
        std::optional<CGRect> OnScreenBounds( const CGWindowID window )
        {
            const void*    ids[] = { reinterpret_cast<const void*>( static_cast<uintptr_t>( window ) ) };
            CFArrayRef     list  = CFArrayCreate( nullptr, ids, 1, nullptr );
            CFArrayRef     info  = CGWindowListCreateDescriptionFromArray( list );
            std::optional<CGRect> bounds;
            if ( info != nullptr && CFArrayGetCount( info ) > 0 )
            {
                auto      entry    = (CFDictionaryRef)CFArrayGetValueAtIndex( info, 0 );
                auto      onScreen = (CFBooleanRef)CFDictionaryGetValue( entry, kCGWindowIsOnscreen );
                auto      rect     = (CFDictionaryRef)CFDictionaryGetValue( entry, kCGWindowBounds );
                CGRect    frame{};
                if ( onScreen != nullptr && CFBooleanGetValue( onScreen ) && rect != nullptr &&
                     CGRectMakeWithDictionaryRepresentation( rect, &frame ) )
                    bounds = frame;
            }
            if ( info != nullptr )
                CFRelease( info );
            CFRelease( list );
            return bounds;
        }
    } // namespace

    class SplashScreenMacOS final : public SplashScreen
    {
    public:
        explicit SplashScreenMacOS( const SplashContent& content ) : m_CookedImage( content.CookedImage )
        {
            [NSApplication sharedApplication];
            // The same policy GLFW sets a moment later. Without it a process started from a terminal is a
            // background app, and its first window is ordered in behind the terminal.
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            // A bare executable has no bundle, so the Dock would show the generic "exec" icon. A missing
            // file keeps that generic icon and says so once; it is not a reason to stop the start.
            if ( NSImage* icon = [[NSImage alloc] initWithContentsOfFile:ToNS( kAppIcon.string() )] )
                [NSApp setApplicationIconImage:icon];
            else
                LOG_WARN( "[Splash] application icon '{}' not found; the Dock keeps the generic icon",
                          kAppIcon.string() );

            NSScreen*     screen  = [NSScreen mainScreen];
            const NSRect  visible =
                 ( screen != nullptr ) ? screen.visibleFrame : NSMakeRect( 0, 0, kWidth, kHeight );
            const CGFloat backing = ( screen != nullptr ) ? screen.backingScaleFactor : 2.0;
            const CGFloat fit     = FitScale( (float)visible.size.width, (float)visible.size.height );
            const CGFloat w       = kWidth * fit;
            const CGFloat h       = kHeight * fit;
            const NSRect  frame   = NSMakeRect( visible.origin.x + ( visible.size.width - w ) * 0.5,
                                                visible.origin.y + ( visible.size.height - h ) * 0.5, w, h );

            m_Window                    = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:NSWindowStyleMaskBorderless |
                                                             NSWindowStyleMaskMiniaturizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
            m_Window.releasedWhenClosed = NO;
            m_Window.opaque             = NO;
            m_Window.backgroundColor    = [NSColor clearColor];
            m_Window.hasShadow          = YES;
            // Above the editor's own window while that one is still hidden, and above the terminal that
            // started it — the place every engine's splash sits.
            m_Window.level = NSFloatingWindowLevel;
            m_Window.title = @"Desert Engine";
            // Moved by its background, as the editor's title bar moves the editor. The drag is the window
            // server's once AppKit has handed it the draggable region, so it does not wait on the loading
            // main thread either.
            m_Window.movableByWindowBackground = YES;
            m_Fit                              = fit;

            NSView* view    = m_Window.contentView;
            view.wantsLayer = YES;

            [CATransaction begin];
            [CATransaction setDisableActions:YES];

            // The design is laid out once, in its own 1200x675 points, inside a container that is scaled
            // as one picture to the window: FitScale is the only place the screen size enters.
            m_Root                 = [[CALayer alloc] init];
            m_Root.anchorPoint     = CGPointZero;
            m_Root.bounds          = CGRectMake( 0, 0, kWidth, kHeight );
            m_Root.position        = CGPointZero;
            m_Root.affineTransform = CGAffineTransformMakeScale( fit, fit );
            m_Root.masksToBounds   = YES;
            m_Root.cornerRadius    = 10.0;
            // The plain dark background is what shows when there is no cooked picture yet (first run on a
            // fresh clone), and under the picture's first frame while it decodes.
            CGColorRef dark        = MakeColour( 0.08, 0.075, 0.07, 1 );
            m_Root.backgroundColor = dark;
            CGColorRelease( dark );
            [view.layer addSublayer:m_Root];

            m_Image                 = [[CALayer alloc] init];
            m_Image.frame           = CGRectMake( 0, 0, kWidth, kHeight );
            m_Image.contentsGravity = kCAGravityResizeAspectFill;
            [m_Root addSublayer:m_Image];

            const CGFloat scale = backing * fit;
            m_Project =
                 MakeTextLayer( m_Root, [NSFont systemFontOfSize:kProjectFontSize weight:NSFontWeightSemibold],
                                1.0, kCAAlignmentLeft, scale );
            m_Version =
                 MakeTextLayer( m_Root, [NSFont systemFontOfSize:kVersionFontSize weight:NSFontWeightRegular],
                                kStageAlpha, kCAAlignmentRight, scale );
            m_Stage   = MakeTextLayer( m_Root, [NSFont systemFontOfSize:kStageFontSize weight:NSFontWeightRegular],
                                       kStageAlpha, kCAAlignmentLeft, scale );
            m_Percent = MakeTextLayer( m_Root,
                                       [NSFont monospacedDigitSystemFontOfSize:kPercentFontSize
                                                                        weight:NSFontWeightRegular],
                                       kStageAlpha, kCAAlignmentRight, scale );
            m_Item    = MakeTextLayer( m_Root, [NSFont systemFontOfSize:kItemFontSize weight:NSFontWeightRegular],
                                       kItemAlpha, kCAAlignmentLeft, scale );

            m_Track                 = [[CALayer alloc] init];
            CGColorRef track        = MakeColour( 1, 1, 1, kBarTrackAlpha );
            m_Track.backgroundColor = track;
            CGColorRelease( track );
            [m_Root addSublayer:m_Track];

            m_Fill                 = [[CALayer alloc] init];
            CGColorRef sand        = MakeColour( kBarFillRed, kBarFillGreen, kBarFillBlue, 1 );
            m_Fill.backgroundColor = sand;
            CGColorRelease( sand );
            [m_Root addSublayer:m_Fill];

            // THE WINDOW BUTTONS: the editor frame's minimize and close, same squares, colours and glyphs
            // (WindowButtonStyle.hpp), in the splash's top-right corner.
            CTFontRef iconFont = LoadIconFont( kButtonGlyphSize );
            for ( const SplashButton button : { SplashButton::Minimize, SplashButton::Close } )
            {
                const Rect rect         = ButtonRect( button );
                CALayer*   square       = [[CALayer alloc] init];
                square.frame            = ToCG( rect );
                CATextLayer* glyph      = [[CATextLayer alloc] init];
                const bool   close      = button == SplashButton::Close;
                glyph.font              = iconFont != nullptr ? (CFTypeRef)iconFont
                                                              : (__bridge CFTypeRef)[NSFont systemFontOfSize:kButtonGlyphSize];
                glyph.fontSize          = kButtonGlyphSize;
                glyph.string            = iconFont != nullptr ? ToNS( close ? UI::kCloseGlyph : UI::kMinimizeGlyph )
                                                              : ( close ? @"\u2715" : @"\u2013" );
                glyph.alignmentMode     = kCAAlignmentCenter;
                glyph.contentsScale     = scale;
                CGColorRef white        = MakeColour( 1, 1, 1, 0.9 );
                glyph.foregroundColor   = white;
                CGColorRelease( white );
                const CGFloat line      = LineHeight( kButtonGlyphSize );
                glyph.frame             = CGRectMake( 0, ( rect.H - line ) * 0.5, rect.W, line );
                [square addSublayer:glyph];
                [m_Root addSublayer:square];
                m_ButtonSquares[close ? 1 : 0] = square;
                m_ButtonGlyphs[close ? 1 : 0]  = glyph;

                NSView* area = [[DesertSplashButtonArea alloc]
                     initWithFrame:NSMakeRect( rect.X * fit, rect.Y * fit, rect.W * fit, rect.H * fit )];
                [view addSubview:area];
                [area release];
            }
            if ( iconFont != nullptr )
                CFRelease( iconFont );

            m_Project.string = ToNS( content.ProjectName );
            m_Version.string = ToNS( content.Version );
            ApplyStatus( ProgressSnapshot{} );

            // The fade is handed to Core Animation whole: its render server runs it on its own clock.
            CABasicAnimation* fadeIn = [CABasicAnimation animationWithKeyPath:@"opacity"];
            fadeIn.fromValue         = @0.0;
            fadeIn.toValue           = @1.0;
            fadeIn.duration          = kFadeInSeconds;
            [m_Root addAnimation:fadeIn forKey:@"fade-in"];

            [CATransaction commit];

            [m_Window orderFrontRegardless];
            [CATransaction flush];

            // HOW EARLY, measured from the kernel's own record of when this process began — the one clock
            // that includes the loader and the static initialisers, which no timer started in main() sees.
            kinfo_proc info{};
            size_t     size  = sizeof( info );
            int        mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
            struct timeval now
            {
            };
            gettimeofday( &now, nullptr );
            if ( sysctl( mib, 4, &info, &size, nullptr, 0 ) == 0 )
            {
                const struct timeval start = info.kp_proc.p_starttime;
                const double         ms =
                     ( now.tv_sec - start.tv_sec ) * 1000.0 + ( now.tv_usec - start.tv_usec ) / 1000.0;
                LOG_INFO( "[Splash] on screen {:.0f} ms after the process started", ms );
            }

            m_WindowId = static_cast<CGWindowID>( m_Window.windowNumber );
            m_Thread   = std::thread( [this] { Run(); } );
        }

        ~SplashScreenMacOS() override
        {
            Close();
        }

        void SetProgress( const ProgressSnapshot& progress ) override
        {
            // On the main thread, which is where AppKit wants a window minimized: a minimize clicked while
            // the main thread was inside a long call is carried out at its next progress report.
            MinimizeIfAsked( m_Window, m_MinimizeAsked );
            {
                const std::lock_guard<std::mutex> lock( m_Mutex );
                m_Pending = progress;
            }
            m_Wake.notify_one();
        }

        void Close() override
        {
            if ( m_Closed )
                return;
            m_Closed = true;

            {
                const std::lock_guard<std::mutex> lock( m_Mutex );
                m_Stop = true;
            }
            m_Wake.notify_one();
            if ( m_Thread.joinable() )
                m_Thread.join();

            // THE CROSSFADE: the editor's window is already shown underneath, so fading the splash's layers
            // out reveals it. The window itself is ordered out once the fade is over, by the main run loop
            // that the editor pumps every frame (glfwPollEvents) — not by sleeping here, which would
            // freeze the editor that just appeared for the length of the fade.
            [CATransaction begin];
            CABasicAnimation* fadeOut = [CABasicAnimation animationWithKeyPath:@"opacity"];
            fadeOut.fromValue         = @1.0;
            fadeOut.toValue           = @0.0;
            fadeOut.duration          = kFadeOutSeconds;
            m_Root.opacity            = 0.0f;
            [m_Root addAnimation:fadeOut forKey:@"fade-out"];
            [CATransaction commit];
            [CATransaction flush];
            // The delayed perform retains the window until it has run, so the release below is safe.
            [m_Window performSelector:@selector( orderOut: ) withObject:nil afterDelay:kFadeOutSeconds];

            for ( CALayer* layer :
                  { (CALayer*)m_Project, (CALayer*)m_Version, (CALayer*)m_Stage, (CALayer*)m_Percent,
                    (CALayer*)m_Item, m_Track, m_Fill, m_Image, (CALayer*)m_ButtonGlyphs[0],
                    (CALayer*)m_ButtonGlyphs[1], m_ButtonSquares[0], m_ButtonSquares[1], m_Root } )
                [layer release];
            [m_Window release];
            m_Window = nil;
        }

    private:
        static void MinimizeIfAsked( NSWindow* window, const std::shared_ptr<std::atomic<bool>>& asked )
        {
            if ( window != nil && asked->exchange( false ) )
                [window miniaturize:nil];
        }

        // One look at the pointer, on the splash's thread: hover and pressed colours, and a click on
        // either button. Returns whether anything visible changed.
        bool TrackPointer()
        {
            const Pointer pointer = ReadPointer();
            SplashButton  under   = SplashButton::None;
            if ( const auto bounds = OnScreenBounds( m_WindowId ) )
            {
                const DesignPoint at =
                     ScreenToDesign( (float)pointer.Location.x, (float)pointer.Location.y,
                                     (float)bounds->origin.x, (float)bounds->origin.y, (float)m_Fit );
                under = HitTestButtons( at.X, at.Y );
            }
            const SplashButton before[2] = { m_Buttons.Hovered(), m_Buttons.Pressed() };
            const SplashButton clicked   = m_Buttons.Update( under, pointer.Down );
            if ( clicked == SplashButton::Close && !CloseRequested() )
            {
                LOG_INFO( "[Splash] close clicked; the editor stops loading and exits" );
                NoteCloseClicked();
            }
            else if ( clicked == SplashButton::Minimize )
            {
                m_MinimizeAsked->store( true );
                // And through the main queue too, which the main run loop drains whenever the editor
                // pumps events between stages; whichever comes first minimizes, the other finds nothing.
                NSWindow* window = m_Window;
                auto      asked  = m_MinimizeAsked;
                [window retain];
                dispatch_async( dispatch_get_main_queue(), ^{
                  MinimizeIfAsked( window, asked );
                  [window release];
                } );
            }
            return clicked == SplashButton::Close || before[0] != m_Buttons.Hovered() ||
                   before[1] != m_Buttons.Pressed();
        }

        // Called inside a transaction the caller owns.
        void ApplyButtons()
        {
            SetFill( m_ButtonSquares[0], ButtonFill( SplashButton::Minimize, m_Buttons ) );
            SetFill( m_ButtonSquares[1], ButtonFill( SplashButton::Close, m_Buttons ) );
        }

        // Called inside a transaction the caller owns.
        void ApplyStatus( const ProgressSnapshot& status )
        {
            const Layout layout = ComputeLayout( status.Fraction );
            m_Project.frame     = ToCG( layout.Project );
            m_Version.frame     = ToCG( layout.Version );
            m_Stage.frame       = ToCG( layout.Stage );
            m_Percent.frame     = ToCG( layout.Percent );
            m_Item.frame        = ToCG( layout.Item );
            m_Track.frame       = ToCG( layout.BarTrack );
            m_Fill.frame        = ToCG( layout.BarFill );
            // Once close is clicked the load is being abandoned, and the stage the main thread last
            // reported is no longer what is happening.
            m_Stage.string      = ToNS( CloseRequested() ? std::string( "Closing..." ) : status.Stage );
            m_Item.string       = ToNS( CloseRequested() ? std::string{} : status.Item );
            // No stage yet means no plan yet: a "0%" there would be a number that says nothing.
            m_Percent.string = ToNS( status.Stage.empty() ? std::string{} : FormatPercent( status.Fraction ) );
        }

        void Run()
        {
            // THE PICTURE FIRST, on this thread: the window is already up with its live text on the dark
            // background, so the decode delays nothing a person is waiting for.
            auto pixels = LoadSplashPixels( m_CookedImage );
            if ( pixels.IsSuccess() )
            {
                const SplashPixels& image = pixels.GetValue();
                CFDataRef           data  = CFDataCreate( nullptr, image.Rgba.data(), (CFIndex)image.Rgba.size() );
                CGDataProviderRef   provider = CGDataProviderCreateWithCFData( data );
                CGColorSpaceRef     space    = CGColorSpaceCreateWithName( kCGColorSpaceSRGB );
                CGImageRef          picture =
                     CGImageCreate( image.Width, image.Height, 8, 32, static_cast<size_t>( image.Width ) * 4u,
                                    space, (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault,
                                    provider, nullptr, false, kCGRenderingIntentDefault );
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                m_Image.contents = (__bridge id)picture;
                [CATransaction commit];
                [CATransaction flush];
                CGImageRelease( picture );
                CGColorSpaceRelease( space );
                CGDataProviderRelease( provider );
                CFRelease( data );
                LOG_INFO( "[Splash] {}x{} picture decoded from '{}' in {:.1f} ms (BC7 on the CPU, off the main "
                          "thread)",
                          image.Width, image.Height, m_CookedImage.string(), image.DecodeMs );
            }
            else
            {
                LOG_WARN( "[Splash] no picture, drawing on the plain background: {}", pixels.GetError() );
            }

            // Woken by a new status, and otherwise every kPointerPeriod to look at the pointer: the buttons
            // answer from this thread, because the main thread that owns the event stream is loading.
            constexpr auto   kPointerPeriod = std::chrono::milliseconds( 16 );
            ProgressSnapshot status;
            for ( ;; )
            {
                bool statusChanged = false;
                {
                    std::unique_lock<std::mutex> lock( m_Mutex );
                    m_Wake.wait_for( lock, kPointerPeriod, [this] { return m_Stop || m_Pending.has_value(); } );
                    if ( m_Stop )
                        return;
                    if ( m_Pending )
                    {
                        status = std::move( *m_Pending );
                        m_Pending.reset();
                        statusChanged = true;
                    }
                }
                const bool buttonsChanged = TrackPointer();
                if ( !statusChanged && !buttonsChanged )
                    continue;
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                ApplyStatus( status );
                ApplyButtons();
                [CATransaction commit];
                [CATransaction flush];
            }
        }

        std::filesystem::path m_CookedImage;

        NSWindow*    m_Window  = nil;
        CALayer*     m_Root    = nil;
        CALayer*     m_Image   = nil;
        CATextLayer* m_Project = nil;
        CATextLayer* m_Version = nil;
        CATextLayer* m_Stage   = nil;
        CATextLayer* m_Percent = nil;
        CATextLayer* m_Item    = nil;
        CALayer*     m_Track   = nil;
        CALayer*     m_Fill    = nil;
        // [0] minimize, [1] close.
        CALayer*     m_ButtonSquares[2] = { nil, nil };
        CATextLayer* m_ButtonGlyphs[2]  = { nil, nil };
        CGWindowID   m_WindowId         = 0;
        CGFloat      m_Fit              = 1.0;
        // Touched by the splash's thread only.
        ButtonTracker m_Buttons;
        // Set by the splash's thread, carried out on the main thread (AppKit's rule for window changes).
        std::shared_ptr<std::atomic<bool>> m_MinimizeAsked = std::make_shared<std::atomic<bool>>( false );

        std::thread             m_Thread;
        std::mutex              m_Mutex;
        std::condition_variable m_Wake;
        std::optional<ProgressSnapshot> m_Pending;
        bool                    m_Stop   = false;
        bool                    m_Closed = false;
    };

    std::unique_ptr<SplashScreen> SplashScreen::Show( const SplashContent& content )
    {
        return std::make_unique<SplashScreenMacOS>( content );
    }
} // namespace Desert::Editor::Splash
