#pragma once

#include <filesystem>

// THE SYSTEM FILE DIALOGS LIVE HERE, NOT IN COMMON'S FileSystem. UE's split: IPlatformFile is plain I/O in
// Core, IDesktopPlatform::OpenFileDialog is the DesktopPlatform module that only UI-bearing programs load.
// Common's FileSystem is read by tools and test suites with no window (DShaderTool reads .shadingmodel files
// through it); while it also carried NSOpenPanel, every one of them had to link AppKit and the ObjC runtime.
// One implementation per platform: Platform/MacOS (AppKit, Objective-C++), Platform/Windows (comdlg32/shell32).
// Every call is modal and returns an empty path when the user cancels.
namespace Desert::Editor
{
    class DesktopPlatform
    {
    public:
        // filter is the Windows-style "Description\0*.ext\0" string; the macOS panel ignores it.
        static std::filesystem::path OpenFileDialog( const char* filter = "All\0*.*\0" );

        // initialFolder is where the macOS panel starts when non-empty; the Windows folder browser takes none.
        static std::filesystem::path OpenFolderDialog( const char* initialFolder = "" );

        // filter as for OpenFileDialog.
        static std::filesystem::path SaveFileDialog( const char* filter = "All\0*.*\0" );
    };
} // namespace Desert::Editor
