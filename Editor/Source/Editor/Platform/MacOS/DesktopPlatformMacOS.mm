#include <Editor/Platform/DesktopPlatform.hpp>

#import <AppKit/AppKit.h>

namespace Desert::Editor
{
    std::filesystem::path DesktopPlatform::OpenFileDialog( const char* filter )
    {
        (void)filter;

        @autoreleasepool
        {
            NSOpenPanel* panel            = [NSOpenPanel openPanel];
            panel.canChooseFiles          = YES;
            panel.canChooseDirectories    = NO;
            panel.allowsMultipleSelection = NO;
            panel.resolvesAliases         = YES;

            if ( [panel runModal] == NSModalResponseOK )
            {
                NSURL* url = panel.URLs.firstObject;
                if ( url != nil )
                {
                    return std::filesystem::path( [url.path UTF8String] );
                }
            }
        }

        return std::filesystem::path();
    }

    std::filesystem::path DesktopPlatform::OpenFolderDialog( const char* initialFolder )
    {
        @autoreleasepool
        {
            NSOpenPanel* panel            = [NSOpenPanel openPanel];
            panel.canChooseFiles          = NO;
            panel.canChooseDirectories    = YES;
            panel.allowsMultipleSelection = NO;
            panel.resolvesAliases         = YES;

            if ( initialFolder != nullptr && initialFolder[0] != '\0' )
            {
                NSString* start    = [NSString stringWithUTF8String:initialFolder];
                panel.directoryURL = [NSURL fileURLWithPath:start isDirectory:YES];
            }

            if ( [panel runModal] == NSModalResponseOK )
            {
                NSURL* url = panel.URLs.firstObject;
                if ( url != nil )
                {
                    return std::filesystem::path( [url.path UTF8String] );
                }
            }
        }

        return std::filesystem::path();
    }

    std::filesystem::path DesktopPlatform::SaveFileDialog( const char* filter )
    {
        (void)filter;

        @autoreleasepool
        {
            NSSavePanel* panel         = [NSSavePanel savePanel];
            panel.canCreateDirectories = YES;

            if ( [panel runModal] == NSModalResponseOK )
            {
                NSURL* url = panel.URL;
                if ( url != nil )
                {
                    return std::filesystem::path( [url.path UTF8String] );
                }
            }
        }

        return std::filesystem::path();
    }
} // namespace Desert::Editor
