// The per-OS half of the crash reporter's own title bar.
//
// WHY THIS IS A SEPARATE FILE. The bar itself - icon, title, minimise and close - is ordinary ImGui
// drawing in Main.cpp and is the same on every platform. What is NOT portable is telling the window
// manager "this strip of client area behaves like a caption": on Win32 that is WM_NCHITTEST, on macOS
// it is a titlebarAppearsTransparent NSWindow, on X11 it is _NET_WM_MOVERESIZE. Only that answer
// lives here, so a port adds one .cpp branch and touches no layout code.

#pragma once

#include <string>

struct GLFWwindow;

namespace CrashReporter::NativeFrame
{
    // Where the caption strip is, in CLIENT pixels, as the UI drew it this frame. Main.cpp owns these
    // numbers; this module only reports them to the window manager.
    struct CaptionLayout
    {
        // Height of the draggable strip measured from the top of the client area.
        float captionHeight = 0.0f;
        // Width, at the right end of that strip, that is NOT draggable because our own window buttons
        // are there. Without it the OS would eat the clicks before ImGui ever saw them.
        float buttonsWidth = 0.0f;
        // Thickness of the edge that starts a resize.
        float borderWidth = 0.0f;
    };

    // Turns inWindow into a borderless window that the OS still treats as a real one: caption drag,
    // Aero snap, double-click to maximise, edge resize, and rounded corners where the OS has them.
    //
    // Returns an empty string on success, otherwise one line per thing that could not be applied, so
    // the caller can show it. Nothing here is fatal: a window with an inert title bar is still a
    // window, and this tool is started by a process that has already died.
    std::string Install( GLFWwindow* inWindow );

    // Called every frame with the layout the UI just drew. Cheap; no OS call.
    void SetLayout( const CaptionLayout& inLayout );
} // namespace CrashReporter::NativeFrame
