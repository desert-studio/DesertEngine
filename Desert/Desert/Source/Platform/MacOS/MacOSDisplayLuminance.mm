#include <Engine/Graphic/DisplayOutput.hpp>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>

// macOS EDR: the OS reports headroom RATIOS over SDR white, never nits (NSScreen,
// maximumExtendedDynamicRangeColorComponentValue & co.). The window's screen answers; a window not yet on a
// screen asks the main screen.
namespace Desert::Graphic
{
    Common::ResultStr<DisplayLuminance> QueryDisplayLuminance( void* nativeWindow )
    {
        @autoreleasepool
        {
            NSWindow* window = nativeWindow ? glfwGetCocoaWindow( static_cast<GLFWwindow*>( nativeWindow ) ) : nil;
            NSScreen* screen = window && window.screen ? window.screen : [NSScreen mainScreen];
            if ( screen == nil )
                return Common::MakeError<DisplayLuminance>( "QueryDisplayLuminance: no NSScreen (no display online)" );

            DisplayLuminance luminance;
            luminance.EdrHeadroom       = static_cast<float>( screen.maximumExtendedDynamicRangeColorComponentValue );
            luminance.PotentialHeadroom =
                 static_cast<float>( screen.maximumPotentialExtendedDynamicRangeColorComponentValue );
            if ( @available( macOS 10.15, * ) )
                luminance.ReferenceHeadroom =
                     static_cast<float>( screen.maximumReferenceExtendedDynamicRangeColorComponentValue );
            luminance.Source = std::string( "NSScreen \"" ) + [screen.localizedName UTF8String] + "\"";
            return Common::MakeSuccess( luminance );
        }
    }
} // namespace Desert::Graphic
