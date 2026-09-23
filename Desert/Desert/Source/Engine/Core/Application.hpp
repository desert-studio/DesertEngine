#pragma once

#include <memory>

#include <string>
#include <optional>
#include <cstdint>
#include <unordered_set>

#include <Common/Core/Singleton.hpp>
#include <Common/Core/LayerStack.hpp>
#include <Common/Core/Events/WindowEvents.hpp>
#include <Common/Core/Core.hpp>

#include "EngineStats.hpp"
#include <Engine/Core/Window.hpp>
#include <Engine/Core/Device.hpp>

#include <Engine/Graphic/RendererContext.hpp>

// THE ENGINE CORE KNOWS NOTHING ABOUT ANY INTERFACE TOOLKIT. `<Engine/imgui/ImGuiLayer.hpp>` was
// included here, which is how Dear ImGui reached every consumer of the engine — the packaged Runtime
// included: Application.hpp is the header every layer host opens. The per-frame UI hook it drives is
// `Common::Layer::OnUIRender`, and what a layer draws in it is the layer's business (the Editor records
// ImGui; the Runtime records its own Render2D batches).

namespace Desert::Engine
{
    struct ApplicationInfo
    {
        std::string Title;
        // Window size. std::nullopt (the default) = start fullscreen at the monitor's native resolution;
        // set a concrete value for a windowed size.
        std::optional<uint32_t> Width;
        std::optional<uint32_t> Height;
        // Only when starting fullscreen (Width/Height = nullopt): true = cover the taskbar, false = leave
        // the taskbar visible (fit the monitor work area).
        bool FullscreenCoverTaskbar = false;
        // false = THIS APPLICATION DRAWS ITS OWN TITLE BAR, and the OS is asked for no frame at all. The
        // editor sets it (its menu bar already carries the project, the level and the window's commands,
        // so the system bar above it was a second title bar over the same window); the packaged Runtime
        // leaves it true, because a game has no menu bar to put there and would lose the close button.
        bool Decorated = true;
        bool VSync     = true;
        // false = the window is created hidden and the application shows it itself (Window::Show). The
        // editor does, from behind its splash; see WindowSpecification::Visible.
        bool Visible = true;
    };

    class Application
    {
    public:
        Application( const ApplicationInfo& appInfo );
        // Virtual because main owns the concrete app through a std::unique_ptr<Application> (see
        // EntryPoint.hpp): a non-virtual destructor there would destroy the base and leak the derived part.
        virtual ~Application();

        const auto& GetEngineStats() const
        {
            return m_EngineStats;
        }

        virtual void OnCreate()  = 0;
        virtual void OnDestroy() = 0;

        /// Takes ownership of @p layer and attaches it. `PopLayer` is gone: it was called from nowhere,
        /// it deleted nothing, and under ownership it would have been a silent destroy (see LayerStack).
        void PushLayer( std::unique_ptr<Common::Layer> layer );

        const auto& GetWindow() const
        {
            return m_Window;
        }

        void Run();

        /// Process exit status when the run ended because the GPU device was lost. Distinct from 1 (a
        /// layer or a frame failed for a reason inside this program) so that a script, or a person reading
        /// a CI log, can tell "someone else's GPU reset took us with it" from "we are broken". 133 — what
        /// the abort inside VK_CHECK_RESULT used to produce — meant neither of those things.
        static constexpr int kExitDeviceLost = 3;

    public:
        // Ends the run loop after the current frame. Used by the editor's screenshot mode, which renders a
        // fixed number of frames and leaves.
        //
        // @p exitCode becomes the process exit status (see EntryPoint.hpp). It exists because a capture
        // that could not write its PNG used to log the error and then exit 0: a caller reading the exit
        // code was told the shot succeeded. A false success is worse than a false failure — it is the one
        // a script cannot notice.
        void Close( int exitCode = 0 )
        {
            m_IsRunningApplication = false;
            m_ExitCode             = exitCode;
        }

        NO_DISCARD int ExitCode() const
        {
            return m_ExitCode;
        }

    private:
        void Init();
        void Destroy();

        // Reports a layer's failed result once per distinct (stage, layer, message). See the definition
        // for why the deduplication is not an optimisation.
        void ReportLayerFailure( const char* stage, Common::Layer* layer, const std::string& error );

        /// TRUE when the device is lost, having asked the run to end with kExitDeviceLost. Called at each
        /// of the three points in the loop where a frame can fail, so that the loop stops at whichever one
        /// discovers the loss rather than carrying on to the next.
        NO_DISCARD bool EndRunOnDeviceLoss( const char* stage );

    private:
        NO_DISCARD bool OnClose( Common::EventWindowClose& /*e*/ )
        {
            m_IsRunningApplication = false;
            return true;
        }
        void ProcessEvents( Common::Event& e );

        // MEMBER ORDER IS LOAD-BEARING. Members die in REVERSE declaration order, and the window owns the
        // swapchain, its framebuffers and their images — device-owned objects that must be released while
        // the device and the context's VMA allocator are still alive. Declared in the order below they are
        // destroyed window -> device -> context, which is the only order that holds.
        //
        // It used to be the exact opposite -- window declared first, context last -- and that is where the
        // exit-time SEGFAULT came from: VulkanImage2D::Release() dereferenced an already-expired renderer
        // context to reach the allocator. VulkanFramebuffer::Release() carries an `if (allocator)` guard
        // written to survive the same window; that guard is still load-bearing for the editor's
        // process-lifetime thumbnail caches, which are not released deterministically yet.
    private:
        ApplicationInfo m_ApplicationInfo;

        bool m_IsRunningApplication = true;
        int  m_ExitCode             = 0;

        // Failures already reported by ReportLayerFailure, keyed on stage + layer + message. Not a
        // counter: a counter cannot tell a message that is still recurring from a new one.
        std::unordered_set<std::string> m_ReportedLayerFailures;

        std::shared_ptr<Graphic::RendererContext> m_RendererContext;
        std::shared_ptr<Device>                   m_Device;

        Common::LayerStack m_LayerStack;
        EngineStats        m_EngineStats;

    protected:
        std::shared_ptr<Window> m_Window;
    };

    Application* CreateApplicaton( int argc, char** argv );
} // namespace Desert::Engine
