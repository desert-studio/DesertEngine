#pragma once

#include <string>

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/EventRegistry.hpp>

#include <Engine/Graphic/SwapChain.hpp>

namespace Desert::Graphic
{
    class SwapChain;
}

namespace Desert
{
    struct WindowSpecification
    {
        std::string Title  = "Sandbox";
        uint32_t    Width  = 1600;
        uint32_t    Height = 900;
        // FALSE = THE APPLICATION DRAWS ITS OWN TITLE BAR and the OS draws no frame at all. The editor
        // asks for false (it has drawn a menu bar carrying the project, the level and the window's
        // commands for a long time, with the system bar still stacked above it — two title bars, one
        // window); the packaged Runtime leaves it true, because a game has nothing to draw there.
        //
        // Г12 deleted this field because it had ZERO readers — the only `glfwWindowHint( GLFW_DECORATED )`
        // calls in the tree live inside each platform's fullscreen branch and answer a different question
        // (borderless OVER the monitor). У9 gave it the reader: both platforms' Init() applies it, and
        // both apply it AFTER glfwCreateWindow rather than through the hint. That is not a style choice
        // and the reason is measured — see the block above the call in MacOSWindow::Init.
        bool        Decorated  = true;
        bool        Fullscreen = false;
        // When Fullscreen (borderless): cover the whole monitor (over the taskbar) if true, else fit the
        // monitor work area (taskbar stays visible).
        bool        FullscreenCoverTaskbar = false;
        bool        VSync                  = true;
        // FALSE = CREATED HIDDEN, and nothing but `Window::Show` puts it on screen. The editor asks for it:
        // a full-size window that is blank and not answering for the seconds its start takes is what its
        // own splash exists to replace (Editor/Splash/SplashScreen.hpp), and the swapchain renders into a
        // hidden window exactly as into a visible one. The packaged Runtime leaves it true.
        bool Visible = true;
    };

    class Window : public Common::EventHandler
    {
    public:
        virtual ~Window()                   = default;
        virtual Common::ResultStr<bool> Init() = 0;

        virtual void ProcessEvents() = 0;

        using EventCallbackFn = std::function<void( Common::Event& )>;

        // ===== The window's own frame, for an application that draws its title bar itself ==========
        //
        // Г12 deleted six methods from here (GetTitle, SetTitle, SetWindowSize, Maximize,
        // IsWindowMaximized, IsWindowMinimized): they were implemented on both platforms and called from
        // nowhere — the platform half of ONE unfinished feature whose other half, the bar, nobody had
        // written. У9 wrote the bar, so the half comes back; and because a pure virtual with no caller is
        // an instruction to every future implementer to write a body nothing runs, EVERY declaration below
        // names the caller that makes it live. If you remove that caller, remove the method with it.
        //
        // FIVE of the six are back, not six. `IsWindowMinimized` is not, and its absence is a decision:
        // У9 found no caller for it. The two candidates were a frame the run loop skips while the window
        // is iconified — which would stop the control channel answering, since a reply is released on a
        // PRESENTED frame — and a restore button, which cannot be clicked on a window you cannot see.
        // Its two implementations also both returned a literal `false`, so what was restored would have
        // been a stub as well as a dead end.

        // The OS-visible title. With the system frame gone this is the only place the OS shows the
        // window's name — the Dock, Mission Control, the taskbar and the window switcher all read it —
        // so the editor composes "engine — project — level" and pushes it whenever the level changes.
        // GetTitle is what makes THIS the single owner of that string: the pusher compares against it
        // rather than keeping a second copy of what it last wrote (EditorLayer::SyncWindowTitle).
        [[nodiscard]] virtual const std::string& GetTitle() const                     = 0;
        virtual void                             SetTitle( const std::string& title ) = 0;

        // Where the window is and how big it is, in screen coordinates. Called by the title bar's drag and
        // by the resize borders the editor draws in place of the frame the OS no longer provides
        // (Editor::UI::WindowChrome).
        virtual void SetWindowSize( uint32_t width, uint32_t height ) = 0;
        virtual void SetWindowPos( int x, int y )                     = 0;
        virtual void GetWindowPos( int& x, int& y ) const             = 0;

        // The title bar's three buttons, and its double-click.
        virtual void Maximize() = 0;
        virtual void Restore()  = 0;
        virtual void Minimize() = 0;

        // ASKED OF THE OS EVERY TIME, never remembered. "Is this window maximized" is a fact with two
        // possible owners — a flag of ours and the window manager — and the pair goes out of step the
        // first time anything else maximizes the window. The implementations are one glfwGetWindowAttrib
        // call each for that reason. Read by the bar to pick between the maximize and restore icons, and
        // by the double-click to decide which way to toggle.
        [[nodiscard]] virtual bool IsWindowMaximized() const = 0;

        // Whether the OS draws this window's frame. Read by the editor: the chrome is drawn only when the
        // application owns the frame, so one build serves both answers and neither is a second code path
        // nobody exercises.
        [[nodiscard]] virtual bool IsDecorated() const = 0;

        // Puts a window created with `Visible = false` on screen and gives it the focus. Called by the
        // editor when its start is over — every stage run and the scene's content settled — immediately
        // before the first frame a person is meant to see (EditorLayer::RevealWhenReady).
        virtual void Show() = 0;

        virtual void                      SetVSync( bool enabled ) = 0;
        [[nodiscard]] virtual uint32_t    GetWidth() const         = 0;
        [[nodiscard]] virtual uint32_t    GetHeight() const        = 0;
        [[nodiscard]] virtual const void* GetNativeWindow() const  = 0;

        // The frame's own result, passed through rather than swallowed. See Renderer.cpp: this link
        // declared them void, which is one of the three places a failed present used to vanish.
        [[nodiscard]] virtual Common::BoolResultStr PrepareNextFrame() const  = 0;
        [[nodiscard]] virtual Common::BoolResultStr PresentFinalImage() const = 0;

        virtual std::shared_ptr<Graphic::SwapChain> GetWindowSwapChain() = 0;

        virtual void SetEventCallback( const EventCallbackFn& e ) = 0;

        virtual Common::ResultStr<bool>
        SetupSwapChain( ) = 0;

        static std::shared_ptr<Window> Create( const WindowSpecification& specification );
    };
} // namespace Desert