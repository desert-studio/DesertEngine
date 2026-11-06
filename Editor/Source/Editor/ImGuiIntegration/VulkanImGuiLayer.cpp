#include <Editor/Core/Control/InputInjection.hpp>
#include <Engine/Core/RenderTask.hpp>
#include <Editor/Core/Control/PointerDrag.hpp>
#include <Editor/ImGuiIntegration/VulkanImGuiLayer.hpp>
#include <Editor/Widgets/UIHelper/UICacheTextureImGui.hpp>

#include <Common/Core/Events/MouseEvents.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFrameLoop.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>
#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <ImGui/backends/imgui_impl_glfw.h>
#include <ImGui/backends/imgui_impl_vulkan.h>

#include <Engine/Core/Glfw.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // HDR-OUTPUT: the UI composite the interface is drawn into (VulkanImGuiLayer.hpp, EnsureDisplayEncode).
    constexpr VkFormat                               kUICompositeVkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr Desert::Core::Formats::ImageFormat     kUICompositeFormat   = Desert::Core::Formats::ImageFormat::RGBA16F;

    // The slot names the display encode binds (SwapchainBlit / DisplayEncodeScRGB share them).
    struct DisplayEncodeShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, FinalColorTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
    };

    template <typename Callback>
    Callback CurrentGlfwCallback( GLFWwindow* window, Callback ( *install )( GLFWwindow*, Callback ) )
    {
        const Callback current = install( window, nullptr );
        install( window, current );
        return current;
    }

    void PlayThroughGlfw( const ::Desert::Editor::Control::InputFrame& frame )
    {
        using ::Desert::Editor::Control::InputAction;
        auto* window = static_cast<GLFWwindow*>( ::ImGui::GetMainViewport()->PlatformHandle );
        if ( window == nullptr )
            return;
        for ( const auto& step : frame )
        {
            switch ( step.Action )
            {
                case InputAction::Cursor:
                    if ( const auto enter = CurrentGlfwCallback( window, &glfwSetCursorEnterCallback ) )
                        enter( window, GLFW_TRUE );
                    if ( const auto move = CurrentGlfwCallback( window, &glfwSetCursorPosCallback ) )
                        move( window, step.X, step.Y );
                    break;
                case InputAction::ButtonDown:
                case InputAction::ButtonUp:
                    if ( const auto button = CurrentGlfwCallback( window, &glfwSetMouseButtonCallback ) )
                        button( window, step.Code,
                                step.Action == InputAction::ButtonDown ? GLFW_PRESS : GLFW_RELEASE, step.Mods );
                    break;
                case InputAction::KeyDown:
                case InputAction::KeyUp:
                    if ( const auto key = CurrentGlfwCallback( window, &glfwSetKeyCallback ) )
                        key( window, step.Code, glfwGetKeyScancode( step.Code ),
                             step.Action == InputAction::KeyDown ? GLFW_PRESS : GLFW_RELEASE, step.Mods );
                    break;
                case InputAction::Char:
                    if ( const auto typed = CurrentGlfwCallback( window, &glfwSetCharCallback ) )
                        typed( window, static_cast<unsigned int>( step.Code ) );
                    break;
                case InputAction::Scroll:
                    if ( const auto scroll = CurrentGlfwCallback( window, &glfwSetScrollCallback ) )
                        scroll( window, 0.0, static_cast<double>( step.Code ) );
                    break;
                case InputAction::Drop:
                    if ( const auto drop = CurrentGlfwCallback( window, &glfwSetDropCallback ) )
                    {
                        std::vector<const char*> paths;
                        paths.reserve( step.Paths.size() );
                        for ( const std::string& path : step.Paths )
                            paths.push_back( path.c_str() );
                        drop( window, static_cast<int>( paths.size() ), paths.data() );
                    }
                    break;
            }
        }
    }

    /// Presses/releases ImGui's modifier keys so the held set is @p wanted. Straight into io and not through
    /// the GLFW key callback: the backend's callbacks re-read the modifiers from the PHYSICAL keyboard
    /// (glfwGetKey), so a synthetic Shift sent there is overwritten by "not held" on the same event.
    void HoldDragModifiers( ImGuiIO& io, const int32_t wanted )
    {
        namespace Ctl       = ::Desert::Editor::Control;
        static int32_t held = 0;
        if ( held == wanted )
            return;
        constexpr std::pair<int32_t, ImGuiKey> kKeys[] = {
             { Ctl::kModShift, ImGuiKey_ModShift },
             { Ctl::kModControl, ImGuiKey_ModCtrl },
             { Ctl::kModAlt, ImGuiKey_ModAlt },
             { Ctl::kModSuper, ImGuiKey_ModSuper },
        };
        for ( const auto& [bit, key] : kKeys )
            if ( ( held & bit ) != ( wanted & bit ) )
                io.AddKeyEvent( key, ( wanted & bit ) != 0 );
        held = wanted;
    }
} // namespace

namespace Desert::Graphic::API::Vulkan
{
    // One of VulkanImGui's draw buffers (the header's kInterfaceDrawBuffers): a frame's ImGui draw data, copied
    // list for list. The lists are this buffer's own and keep their capacity from frame to frame, so a steady
    // interface allocates nothing. Filled and destroyed on the game thread; InUse is set there before the hand-off
    // and cleared by the render task once the interface graph has recorded from it.
    struct VulkanImGui::InterfaceDrawBuffer
    {
        ImDrawData               Data;
        std::vector<ImDrawList*> Lists; // owned; the first Data.CmdListsCount are this frame's
        std::atomic<bool>        InUse{ false };

        InterfaceDrawBuffer() = default;
        InterfaceDrawBuffer( const InterfaceDrawBuffer& )            = delete;
        InterfaceDrawBuffer& operator=( const InterfaceDrawBuffer& ) = delete;
        ~InterfaceDrawBuffer()
        {
            for ( ImDrawList* list : Lists )
            {
                list->~ImDrawList();
                ::ImGui::MemFree( list );
            }
        }

        // What ImDrawList::CloneOutput copies (commands, indices, vertices, flags), into lists that already exist.
        void Fill( const ImDrawData& source )
        {
            Data                = source;
            const size_t count  = source.CmdListsCount > 0 ? static_cast<size_t>( source.CmdListsCount ) : 0u;
            while ( Lists.size() < count )
            {
                // auto: the shared data pointer is const in this ImGui; ::ImGui, because Desert::ImGui hides it
                // inside IM_NEW / IM_DELETE.
                auto* const shared = source.CmdLists[Lists.size()]->_Data;
                void* const memory = ::ImGui::MemAlloc( sizeof( ImDrawList ) );
                Lists.push_back( new ( ImNewWrapper(), memory ) ImDrawList( shared ) );
            }
            for ( size_t i = 0; i < count; ++i )
            {
                const ImDrawList& from = *source.CmdLists[i];
                ImDrawList&       to   = *Lists[i];
                CopyInto( to.CmdBuffer, from.CmdBuffer );
                CopyInto( to.IdxBuffer, from.IdxBuffer );
                CopyInto( to.VtxBuffer, from.VtxBuffer );
                to.Flags = from.Flags;
            }
            Data.CmdLists = Lists.data();
        }

    private:
        template <typename T>
        static void CopyInto( ImVector<T>& to, const ImVector<T>& from )
        {
            to.resize( from.Size ); // grows only: the capacity of earlier frames is kept
            if ( from.Size > 0 )
                std::memcpy( to.Data, from.Data, static_cast<size_t>( from.Size ) * sizeof( T ) );
        }
    };

    // THE DETACHED WINDOWS (VK-SUBMIT-THREAD steps 7-9, REVIEW-6 GAP-2 / GAP-3 / RISK-3, REVIEW-7; UE: Slate
    // hands each viewport's element batch to the render thread by value, and a viewport resize or destroy
    // flushes the queued rendering before its swapchain changes - FlushRenderingCommands, a flush of the whole
    // queue, not of that viewport - then FVulkanViewport::RecreateSwapchain idles the device). A detached
    // window's frame is a copy of its draw data that an ordered entry of the submit thread owns
    // (VulkanFrameLoop::EnqueueOrdered): the backend records, submits and presents it there, after the frames
    // and uploads handed over before it, so a texture it shows is never sampled before its queued copy. Drawing
    // a frame does not wait. This thread waits where it touches a window: before it refills one of the window's
    // copies and before ImGui changes the size of its OS window (SetPlatformSize), the submit thread's FIFO up
    // to that entry; before the backend rebuilds or destroys it (the renderer callbacks below, put over the
    // backend's), the whole submit thread drained, then the queue idle under the queue lock, which stays held
    // until the backend is done (AfterOwnWork): a frame fence does not cover the present that waits on the
    // window's render-complete semaphore. A panel dragged by its edge pays it every frame it changes size.
    // A frame stops at its first failure (the fork's RenderWindow: no submit after a failed acquire, no
    // present of an image not acquired), and every failure comes back as a value: Render logs its cause once
    // and rebuilds the window here, after its own work, before its next frame; the submit thread never
    // rebuilds. Every backend call reports through CheckBackendResult: as a value to the call that captured it,
    // the cause logged once, and the device-lost latch noted.
    struct VulkanImGui::DetachedWindows
    {
        // One frame of one window as the submit thread draws it: the copy, and a viewport standing in for the
        // window whose DrawData is the copy. The backend reads only the viewport's renderer data, flags, size
        // and draw data, and the real viewport's are rewritten by the next ImGui frame.
        struct WindowFrame
        {
            InterfaceDrawBuffer Draw;
            ImGuiViewport       Proxy;
            uint64_t            Ticket = 0; // the entry that draws it (EnqueueOrdered); 0: none queued

            WindowFrame()                                = default;
            WindowFrame( const WindowFrame& )            = delete;
            WindowFrame& operator=( const WindowFrame& ) = delete;
            ~WindowFrame()
            {
                Proxy.RendererUserData = nullptr; // borrowed from the window: the backend frees it
            }
        };
        struct WindowState
        {
            std::array<std::unique_ptr<WindowFrame>, kInterfaceDrawBuffers> Frames;
            std::size_t                                                     Next       = 0;
            uint64_t                                                        LastTicket = 0; // the newest entry
            // The first failure an entry met (the submit thread writes it), taken by Render.
            std::atomic<VkResult> Failed{ VK_SUCCESS };
            // The window has a swapchain and frames: its last create or rebuild succeeded. Render draws only
            // such a window (a failed create or rebuild leaves the backend's window without frames).
            bool Usable = false;
            // Rebuilt before its next frame: an entry failed, or the last rebuild found the surface out of date.
            bool Stale = false;
        };

        static inline DetachedWindows* Active = nullptr; // the attached layer's (Install .. destruction)
        // THE BACKEND'S STATE IS ONE THREAD'S AT A TIME: its recording (the main interface on the render task,
        // a detached window on the submit thread) and its window calls (this thread) share buffers' alignment,
        // pipelines and shader modules. Taken after the queue lock wherever both are held.
        static inline std::mutex            RecordMutex;
        static inline thread_local bool     t_Capturing = false;
        static inline thread_local VkResult t_Captured  = VK_SUCCESS;
        static inline std::mutex            LoggedMutex;
        static inline std::vector<VkResult> Logged; // the causes already logged (ReportOnce)

        // The queue lock AfterOwnWork holds across a window call (QueuesHeld); null outside one.
        static inline thread_local const std::unique_lock<std::mutex>* t_HeldQueues = nullptr;

        VulkanLogicalDevice* Device = nullptr;
        // The backend's renderer half of the platform interface, kept when Install puts the layer's over it.
        void ( *BackendCreate )( ImGuiViewport* )         = nullptr;
        void ( *BackendDestroy )( ImGuiViewport* )        = nullptr;
        void ( *BackendResize )( ImGuiViewport*, ImVec2 ) = nullptr;
        void ( *BackendRender )( ImGuiViewport*, void* )  = nullptr;
        void ( *BackendPresent )( ImGuiViewport*, void* ) = nullptr;
        // The platform backend's (GLFW) size call, kept by Install; static so it outlives the layer's object.
        static inline void ( *PlatformSetSize )( ImGuiViewport*, ImVec2 ) = nullptr;
        std::unordered_map<const ImGuiViewport*, std::unique_ptr<WindowState>> Windows; // this thread's

        DetachedWindows()                                    = default;
        DetachedWindows( const DetachedWindows& )            = delete;
        DetachedWindows& operator=( const DetachedWindows& ) = delete;
        ~DetachedWindows()
        {
            Active = nullptr;
        }

        // After ImGui_ImplVulkan_Init: keeps the backend's callbacks and puts the layer's over them.
        void Install( VulkanLogicalDevice& device ) noexcept
        {
            Device                    = &device;
            ImGuiPlatformIO& io       = ::ImGui::GetPlatformIO();
            BackendCreate             = io.Renderer_CreateWindow;
            BackendDestroy            = io.Renderer_DestroyWindow;
            BackendResize             = io.Renderer_SetWindowSize;
            BackendRender             = io.Renderer_RenderWindow;
            BackendPresent            = io.Renderer_SwapBuffers;
            io.Renderer_CreateWindow  = &DetachedWindows::Create;
            io.Renderer_DestroyWindow = &DetachedWindows::Destroy;
            io.Renderer_SetWindowSize = &DetachedWindows::Resize;
            // ImGui changes the OS window's size (Platform_SetWindowSize) before the renderer's: the window's
            // queued frames drain first (SetPlatformSize), so none of them meets a surface its swapchain no
            // longer matches. Its OS window is destroyed after Renderer_DestroyWindow, which drains it already.
            if ( io.Platform_SetWindowSize != &DetachedWindows::SetPlatformSize )
                PlatformSetSize = io.Platform_SetWindowSize;
            io.Platform_SetWindowSize = &DetachedWindows::SetPlatformSize;
            // Rendered by Render below, never by ImGui::RenderPlatformWindowsDefault.
            io.Renderer_RenderWindow = nullptr;
            io.Renderer_SwapBuffers  = nullptr;
            Active                   = this;
        }

        // ImGui_ImplVulkan_InitInfo::CheckVkResultFn, on whichever thread the backend runs. A failure goes to the
        // call that captures it (Capture) as its value; outside one it is logged once per cause. A lost device is
        // latched either way. VK_SUBOPTIMAL_KHR (an acquire or present that worked on an image the surface no
        // longer matches) is a value too: the window is rebuilt. The other positive codes succeeded.
        static void CheckBackendResult( const VkResult result )
        {
            if ( result == VK_SUCCESS || ( result > VK_SUCCESS && result != VK_SUBOPTIMAL_KHR ) )
                return;
            // A lost device is latched and explained there (Graphic::DeviceLost); the value still goes on.
            const bool lost = NoteIfDeviceLost( result, "the ImGui Vulkan backend", __FILE__, __LINE__ );
            if ( t_Capturing )
            {
                // The first failure is the value; an error still replaces a SUBOPTIMAL met before it.
                if ( t_Captured == VK_SUCCESS || ( t_Captured > VK_SUCCESS && result < VK_SUCCESS ) )
                    t_Captured = result;
                return;
            }
            if ( !lost && result < VK_SUCCESS )
                ReportOnce( result, "an ImGui backend call outside a window or the interface" );
        }

        // Logs @p result once per cause (its code) for the session, from any thread. An image out of date (the
        // window is rebuilt) is a warning; anything else an error.
        static void ReportOnce( const VkResult result, const char* const what ) noexcept
        {
            {
                const std::scoped_lock lock( LoggedMutex );
                if ( std::find( Logged.begin(), Logged.end(), result ) != Logged.end() )
                    return;
                Logged.push_back( result );
            }
            if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR )
            {
                LOG_WARN( "[VulkanImGui] {}: {}, the window is rebuilt before its next frame (logged once)", what,
                          VkResultToString( result ) );
            }
            else
            {
                LOG_ERROR( "[VulkanImGui] {} failed: {} (each cause is logged once)", what,
                           VkResultToString( result ) );
            }
        }

        // Runs backend calls on this thread, under the backend's lock; the first failure they reported.
        template <typename Call>
        static VkResult Capture( Call&& call ) noexcept
        {
            const std::scoped_lock record( RecordMutex );
            t_Capturing = true;
            t_Captured  = VK_SUCCESS;
            call();
            t_Capturing = false;
            return std::exchange( t_Captured, VK_SUCCESS );
        }

        [[nodiscard]] WindowState* Find( const ImGuiViewport* const viewport ) const noexcept
        {
            const auto found = Windows.find( viewport );
            if ( found == Windows.end() )
                return nullptr;
            return found->second.get();
        }

        // THE SUBMIT THREAD FLUSHED AND PAUSED, THEN THE BACKEND (step 9; UE FVulkanViewport::RecreateSwapchain:
        // the RHI thread flushed, then the device idle, then the swapchain goes). For a window that exists:
        //   1. the submit thread is drained with no lock held (VulkanFrameLoop::DrainSubmitThread): no queue
        //      operation of this window, or of any other one (whose entry would need RecordMutex), is still
        //      recorded on the host;
        //   2. the queue lock is taken and kept across @p call: the submit thread puts nothing on the queue
        //      until the backend is done (lock order: the queue lock, then RecordMutex in Capture);
        //   3. inside @p call the backend waits only through WaitWindowIdle (the fork's InitInfo::
        //      WaitWindowIdleFn), which idles the queue under that lock before the old swapchain, its
        //      semaphores and frames go: a present's wait on RenderCompleteSemaphore is covered too, which no
        //      frame fence is (VUID-vkDestroySemaphore-semaphore-05149).
        // A new window (@p window null) has nothing queued and nothing to destroy: no flush, no lock, and the
        // fork does not wait for it. Nothing of a window is queued meanwhile: only this thread queues it.
        template <typename Call>
        VkResult AfterOwnWork( const WindowState* const window, Call&& call ) noexcept
        {
            std::unique_lock<std::mutex> queues;
            if ( window != nullptr )
            {
                VulkanFrameLoop::DrainSubmitThread();
                queues = Device->LockQueues();
            }
            const QueuesHeld held( queues );
            return Capture( std::forward<Call>( call ) );
        }

        // Publishes the queue lock AfterOwnWork holds to WaitWindowIdle, on this thread, for one backend call.
        struct QueuesHeld
        {
            explicit QueuesHeld( const std::unique_lock<std::mutex>& queues ) noexcept
            {
                t_HeldQueues = &queues;
            }
            ~QueuesHeld()
            {
                t_HeldQueues = nullptr;
            }
            QueuesHeld( const QueuesHeld& )            = delete;
            QueuesHeld& operator=( const QueuesHeld& ) = delete;
        };

        // ImGui_ImplVulkan_InitInfo::WaitWindowIdleFn, the backend's only wait before a window's objects go
        // (AfterOwnWork step 3): the queue idles under the lock AfterOwnWork took, after its flush. Refused (no
        // wait, the fork rebuilds nothing) outside AfterOwnWork or for a window it took no lock for.
        static VkResult WaitWindowIdle( VkQueue const queue ) noexcept
        {
            const DetachedWindows* const self = Active;
            if ( self == nullptr || t_HeldQueues == nullptr )
                return VK_ERROR_INITIALIZATION_FAILED;
            return self->Device->WaitQueueIdle( queue, *t_HeldQueues );
        }

        // ImGui's Platform_SetWindowSize, put over the platform backend's (Install): the window's queued frames
        // run before its OS window changes size, so none of them is drawn by a swapchain the surface no longer
        // matches. UpdatePlatformWindows calls it before Renderer_SetWindowSize (Resize).
        static void SetPlatformSize( ImGuiViewport* const viewport, const ImVec2 size )
        {
            if ( DetachedWindows* const self = Active; self != nullptr )
                if ( const WindowState* const window = self->Find( viewport ); window != nullptr )
                    (void)VulkanFrameLoop::WaitOrdered( window->LastTicket ); // false only on the submit thread
            if ( PlatformSetSize != nullptr )
                PlatformSetSize( viewport, size );
        }

        static void Create( ImGuiViewport* const viewport )
        {
            DetachedWindows* const self = Active;
            if ( self == nullptr )
                return;
            std::unique_ptr<WindowState>& window = self->Windows[viewport];
            window                               = std::make_unique<WindowState>();
            const VkResult created =
                 self->AfterOwnWork( nullptr, [self, viewport]() { self->BackendCreate( viewport ); } );
            // A window the backend could not make (surface, WSI support, swapchain) is not drawn (Render).
            window->Usable = created == VK_SUCCESS;
            if ( created != VK_SUCCESS )
                ReportOnce( created, "a detached window's creation" );
        }

        static void Destroy( ImGuiViewport* const viewport )
        {
            DetachedWindows* const self = Active;
            if ( self == nullptr )
                return;
            const VkResult destroyed = self->AfterOwnWork( self->Find( viewport ), [self, viewport]()
                                                           { self->BackendDestroy( viewport ); } );
            // Every entry that drew it has run (AfterOwnWork), so its copies go with it.
            self->Windows.erase( viewport );
            if ( destroyed != VK_SUCCESS && Graphic::DeviceLost::AllowWork() )
                ReportOnce( destroyed, "a detached window's destruction" );
        }

        // ImGui's Renderer_SetWindowSize, and Render's rebuild of a window an entry found out of date.
        static void Resize( ImGuiViewport* const viewport, const ImVec2 size )
        {
            DetachedWindows* const self = Active;
            if ( self == nullptr )
                return;
            WindowState* const window = self->Find( viewport );
            const VkResult     resized =
                 self->AfterOwnWork( window, [self, viewport, size]() { self->BackendResize( viewport, size ); } );
            if ( window != nullptr )
            {
                // A failed rebuild leaves the backend's window without frames: not drawn until one succeeds. A
                // surface still out of date (it has no drawable area yet) is tried again at the next frame.
                window->Usable = resized == VK_SUCCESS;
                window->Stale  = resized == VK_ERROR_OUT_OF_DATE_KHR;
            }
            if ( resized != VK_SUCCESS && resized != VK_ERROR_OUT_OF_DATE_KHR && Graphic::DeviceLost::AllowWork() )
                ReportOnce( resized, "a detached window's swapchain rebuild" );
        }

        // ON THE SUBMIT THREAD (inline when no loop runs): the backend's acquire, record, submit and present of
        // one copy. Its failure is the entry's value.
        VkResult Draw( WindowFrame& frame ) noexcept
        {
            if ( !Graphic::DeviceLost::AllowWork() )
                return VK_SUCCESS;
            const auto queues = Device->LockQueues();
            return Capture(
                 [this, &frame]()
                 {
                     BackendRender( &frame.Proxy, nullptr );
                     BackendPresent( &frame.Proxy, nullptr );
                 } );
        }

        // THIS THREAD, after UpdatePlatformWindows: every detached window's frame to the submit thread.
        void Render() noexcept
        {
            // A lost device draws nothing more, and no window is rebuilt on it.
            if ( !Graphic::DeviceLost::AllowWork() )
                return;
            const ImVector<ImGuiViewport*>& viewports = ::ImGui::GetPlatformIO().Viewports;
            for ( int i = 1; i < viewports.Size; ++i )
            {
                ImGuiViewport* const viewport = viewports[i];
                WindowState* const   window   = Find( viewport );
                if ( window == nullptr || viewport->DrawData == nullptr )
                    continue;
                // A minimised window has no drawable area: no frame, and no rebuild until it is restored (what
                // its entries met stays for then).
                if ( ( viewport->Flags & ImGuiViewportFlags_Minimized ) != 0 )
                    continue;
                // What the window's earlier entries met, as their value. Each stopped at its failure (nothing
                // submitted after a failed acquire, nothing presented that was not acquired), and the window is
                // rebuilt here, after its queued work, before its next frame: an image out of date, and any
                // other failure, which may have left an acquired image or its semaphore behind.
                const VkResult failed = window->Failed.exchange( VK_SUCCESS, std::memory_order_acq_rel );
                if ( failed != VK_SUCCESS )
                {
                    ReportOnce( failed, "a detached window's frame (acquire, submit or present)" );
                    window->Stale = true;
                }
                if ( window->Stale )
                    Resize( viewport, viewport->Size );
                if ( !window->Usable )
                    continue;

                std::unique_ptr<WindowFrame>& slot = window->Frames[window->Next];
                window->Next                       = ( window->Next + 1 ) % window->Frames.size();
                if ( !slot )
                    slot = std::make_unique<WindowFrame>();
                WindowFrame& frame = *slot;
                // The copy is refilled once the entry that drew it ran (the FIFO up to that entry).
                if ( frame.Ticket != 0 && !VulkanFrameLoop::WaitOrdered( frame.Ticket ) )
                    continue;
                frame.Draw.Fill( *viewport->DrawData );
                frame.Proxy.ID                = viewport->ID;
                frame.Proxy.Flags             = viewport->Flags;
                frame.Proxy.Pos               = viewport->Pos;
                frame.Proxy.Size              = viewport->Size;
                frame.Proxy.DpiScale          = viewport->DpiScale;
                frame.Proxy.RendererUserData  = viewport->RendererUserData;
                frame.Proxy.DrawData          = &frame.Draw.Data;
                frame.Draw.Data.OwnerViewport = &frame.Proxy;

                WindowFrame* const          drawn = &frame;
                const std::function<void()> draw  = [this, window, drawn]() noexcept
                {
                    DESERT_PROFILE_SCOPE( "Vulkan: detached window" );
                    const VkResult result = Draw( *drawn );
                    VkResult       none   = VK_SUCCESS;
                    if ( result != VK_SUCCESS )
                        (void)window->Failed.compare_exchange_strong( none, result, std::memory_order_acq_rel );
                };
                const std::optional<uint64_t> ticket = VulkanFrameLoop::EnqueueOrdered( draw );
                if ( !ticket )
                {
                    // No submit thread: everything handed over ran, so drawing here is still in queue order.
                    draw();
                    frame.Ticket = 0;
                    continue;
                }
                frame.Ticket       = *ticket;
                window->LastTicket = *ticket;
            }
        }
    };

    VulkanImGui::VulkanImGui()  = default;
    VulkanImGui::~VulkanImGui() = default;

    VulkanImGui::InterfaceDrawBuffer& VulkanImGui::AcquireDrawBuffer()
    {
        for ( int attempt = 0; attempt < 2; ++attempt )
        {
            for ( auto& buffer : m_DrawBuffers )
            {
                if ( !buffer )
                    buffer = std::make_unique<InterfaceDrawBuffer>();
                if ( !buffer->InUse.load( std::memory_order_acquire ) )
                    return *buffer;
            }
            // Every buffer is still being drawn (UE: the game thread waits for a free Slate draw buffer): the
            // render task is kInterfaceDrawBuffers interfaces behind, and only its join frees one.
            ::Desert::Engine::RenderTask::Get().Join();
        }
        // Drained, yet still marked: a job that took a buffer did not release it. Nothing reads it any more.
        LOG_ERROR( "[VulkanImGui] an interface draw buffer was not released by its render job; reused" );
        m_DrawBuffers.front()->InUse.store( false, std::memory_order_relaxed );
        return *m_DrawBuffers.front();
    }

    Common::BoolResultStr VulkanImGui::OnAttach()
    {
        if ( ::ImGui::GetCurrentContext() == nullptr )
        {
            ::ImGui::DebugCheckVersionAndDataLayout( IMGUI_VERSION, sizeof( ImGuiIO ), sizeof( ImGuiStyle ),
                                                     sizeof( ImVec2 ), sizeof( ImVec4 ), sizeof( ImDrawVert ),
                                                     sizeof( ImDrawIdx ) );
            ::ImGui::CreateContext();
        }

        ImGuiIO& io = ::ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

        ::ImGui::StyleColorsDark();

        ImGuiStyle& style = ::ImGui::GetStyle();
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            style.WindowRounding              = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        auto& engineContext = EngineContext::GetInstance();
        auto  window        = engineContext.GetWindow();
        auto  swapchain     = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );

        VkDevice device = SP_CAST( VulkanLogicalDevice, engineContext.GetDevice() )->GetVulkanLogicalDevice();

        // Create Descriptor Pool for ImGui
        VkDescriptorPoolSize       pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
                                                    { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };
        VkDescriptorPoolCreateInfo pool_info    = {};
        pool_info.sType                         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags                         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets                       = 1000 * IM_ARRAYSIZE( pool_sizes );
        pool_info.poolSizeCount                 = static_cast<uint32_t>( IM_ARRAYSIZE( pool_sizes ) );
        pool_info.pPoolSizes                    = pool_sizes;
        VK_CHECK_RESULT( vkCreateDescriptorPool( device, &pool_info, nullptr, &m_ImguiPool ) );
        // The UI texture cache frees its per-image sets back into this pool when their images die (AM3).
        ::Desert::Editor::UI::UICacheTextureImGui::Get().BindPool( m_ImguiPool );

        ImGui_ImplGlfw_InitForVulkan( static_cast<GLFWwindow*>( engineContext.GetNativeWindowHandle() ), true );

        // A closed lid with no external display leaves glfwGetMonitors empty, the GLFW backend then
        // publishes an empty Monitors list, and ImGui's first NewFrame asserts on it (it needs a monitor
        // rect to place viewport windows). The headless --shot path still has to run in that state, so
        // publish the main window's own rectangle as the one "monitor" — every viewport lands on the
        // window, which is where the offscreen frame is rendered anyway.
        ImGuiPlatformIO& platformIO = ::ImGui::GetPlatformIO();
        if ( platformIO.Monitors.Size == 0 )
        {
            LOG_ERROR( "No monitor is online (lid closed?): ImGui gets the window rect as its monitor" );
            int windowW = 0, windowH = 0;
            glfwGetWindowSize( static_cast<GLFWwindow*>( engineContext.GetNativeWindowHandle() ), &windowW,
                               &windowH );
            ImGuiPlatformMonitor synthetic;
            synthetic.MainPos = synthetic.WorkPos = ImVec2( 0.0f, 0.0f );
            synthetic.MainSize = synthetic.WorkSize = ImVec2( static_cast<float>( windowW > 0 ? windowW : 1280 ),
                                                              static_cast<float>( windowH > 0 ? windowH : 720 ) );
            platformIO.Monitors.push_back( synthetic );
        }

        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = SP_CAST( VulkanContext, engineContext.GetRendererContext() )->GetVulkanInstance();
        init_info.PhysicalDevice = SP_CAST( VulkanLogicalDevice, engineContext.GetDevice() )
                                        ->GetPhysicalDevice()
                                        ->GetVulkanPhysicalDevice();
        init_info.Device = device;
        init_info.QueueFamily =
             SP_CAST( VulkanLogicalDevice, engineContext.GetDevice() )->GetPhysicalDevice()->GetGraphicsFamily();
        init_info.Queue          = SP_CAST( VulkanLogicalDevice, engineContext.GetDevice() )->GetGraphicsQueue();
        init_info.PipelineCache  = VK_NULL_HANDLE;
        init_info.DescriptorPool = m_ImguiPool;
        init_info.Subpass        = 0;
        init_info.MinImageCount  = swapchain->GetBackBufferCount();
        init_info.ImageCount     = swapchain->GetBackBufferCount();
        init_info.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;
        init_info.Allocator      = nullptr;
        // The backend's own results, as values to the call that runs it and logged once per cause (GAP-3).
        init_info.CheckVkResultFn = &DetachedWindows::CheckBackendResult;
        // The backend's one wait before a window's swapchain, semaphores and frames go: the queue idles under the
        // lock the layer took after it flushed the submit thread (DetachedWindows::AfterOwnWork, step 9).
        init_info.WaitWindowIdleFn = &DetachedWindows::WaitWindowIdle;

        // The pipeline is built against the render graph's canonical pass for the back buffer's format: the
        // interface is drawn inside a graph node (End), and render-pass compatibility is formats and samples only.
        const Common::ResultStr<VkRenderPass> imguiPass =
             CreateRdgRenderPass( device, RdgCompatibleRenderPassKey( { kUICompositeVkFormat },
                                                                      VK_FORMAT_UNDEFINED, false, 1 ) );
        if ( !imguiPass )
            return Common::MakeFormattedError<bool>( "the ImGui render pass: {}", imguiPass.GetError() );
        m_ImguiRenderPass = imguiPass.GetValue();

        ImGui_ImplVulkan_Init( &init_info, m_ImguiRenderPass );
        m_Detached = std::make_unique<DetachedWindows>();
        m_Detached->Install( *SP_CAST( VulkanLogicalDevice, engineContext.GetDevice() ) );

        // Upload Fonts
        {
            const auto commandBuffer =
                 CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
            if ( !commandBuffer.IsSuccess() )
                return Common::MakeFormattedError<bool>( "the font atlas could not be uploaded: {}",
                                                         commandBuffer.GetError() );
            ImGui_ImplVulkan_CreateFontsTexture( commandBuffer.GetValue() );
            CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( commandBuffer.GetValue() );

            ImGui_ImplVulkan_DestroyFontUploadObjects();
            // The atlas lives on the device now; its CPU pixels (RGBA32 plus the Alpha8 it was expanded from,
            // ~80 MB for the editor's glyph ranges) are read by nothing after the upload. A later rebuild
            // rasterizes again from the fonts' config data, which this keeps.
            ::ImGui::GetIO().Fonts->ClearTexData();
        }

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImGui::OnDetach()
    {
        // The last interface the render task may still be recording, before its backend and its buffers go.
        ::Desert::Engine::RenderTask::Get().Join();
        for ( auto& buffer : m_DrawBuffers )
            buffer.reset();
        // Nothing is outstanding on a lost device, so the wait can only answer VK_ERROR_DEVICE_LOST; the
        // ImGui teardown below is destruction, which stays legal.
        if ( Graphic::DeviceLost::AllowWork() )
            EngineContext::GetInstance().GetDevice()->WaitIdle(); // under the queue lock (VK1)
        // THE WINDOWS BEFORE THE BACKEND'S SHARED OBJECTS (REVIEW-7 R7-3): ImGui_ImplVulkan_Shutdown destroys the
        // render buffers, pipeline and fonts every window draws with BEFORE it destroys the platform windows. So
        // they are destroyed here first, each through the layer's Destroy after its queued frames ran (on a lost
        // device too, where the wait above is skipped and a frame that started before the loss may still run).
        ::ImGui::DestroyPlatformWindows();
        ImGui_ImplVulkan_Shutdown();
        m_Detached.reset();
        ImGui_ImplGlfw_Shutdown();
        ::ImGui::DestroyContext();

        if ( m_ImguiPool != VK_NULL_HANDLE )
        {
            // Destroying the pool frees every set the texture cache still holds, so it forgets them rather
            // than freeing each. Through the allocator, not vkDestroyDescriptorPool: frees the cache queued
            // for this pool in the last frames are still in its ring, and the allocator drops a pool's
            // pending frees with the pool instead of running them against a dead handle afterwards.
            ::Desert::Editor::UI::UICacheTextureImGui::Get().ReleasePool();
            SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                 ->GetVulkanAllocator()
                 ->RT_DestroyDescriptorPool( m_ImguiPool );
            m_ImguiPool = VK_NULL_HANDLE;
        }
        if ( m_ImguiRenderPass != VK_NULL_HANDLE )
        {
            VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                   ->GetVulkanLogicalDevice();
            vkDestroyRenderPass( device, m_ImguiRenderPass, nullptr );
            m_ImguiRenderPass = VK_NULL_HANDLE;
        }

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImGui::OnUpdate( const Common::Timestep& /*ts*/ )
    {
        return BOOLSUCCESS;
    }

    void VulkanImGui::Begin()
    {
        // Before any panel asks for a texture id: the sets of images destroyed since last frame go back.
        (void)::Desert::Editor::UI::UICacheTextureImGui::Get().RetireReleased();
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        if ( auto frame = ::Desert::Editor::Control::InputInjection::NextFrame() )
            PlayThroughGlfw( *frame );
        // A control-channel drag: after the backend's own cursor event, so it is this frame's last word.
        const auto step = ::Desert::Editor::Control::PointerInjection::NextStep();
        // Before the pointer events, so the press is seen with them held; released the frame the drag ends.
        HoldDragModifiers( ::ImGui::GetIO(), step ? step->Mods : 0 );
        if ( step )
        {
            ImGuiIO& io = ::ImGui::GetIO();
            if ( ( io.BackendFlags & ImGuiBackendFlags_HasMouseHoveredViewport ) != 0 )
                io.AddMouseViewportEvent( ::Desert::Editor::Control::PointerInjection::ViewportId() );
            io.AddMousePosEvent( step->X, step->Y );
            io.AddMouseButtonEvent( ImGuiMouseButton_Left, step->Down );
            // Picking (entity, UI element, bone) listens to the press EVENT, which only the OS callback
            // emitted: a synthetic click reached the widgets and never selected anything. Sent before
            // NewFrame, as the OS event arrives during polling; the hover step put the cursor here a frame
            // earlier, so GetMousePos and the hover flags the listeners read are already this point.
            if ( step->Press )
            {
                Common::MouseButtonPressedEvent press( Common::MouseButton::Left );
                EngineContext::GetInstance().GetWindow()->Route( press );
            }
        }
        ::ImGui::NewFrame();
    }

    Common::BoolResultStr VulkanImGui::EnsureDisplayEncode()
    {
        auto window    = EngineContext::GetInstance().GetWindow();
        auto swapchain = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );
        const std::shared_ptr<Graphic::Framebuffer> framebuffer = swapchain->GetCompositeFramebuffer();
        const Common::Scalability::DisplayOutput     output      = Graphic::CurrentDisplayOutput().Output;
        if ( m_EncodePipeline && framebuffer == m_EncodeFramebuffer && output == m_EncodeOutput )
            return BOOLSUCCESS;

        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return Common::MakeError( "no shader service" );
        const char* shaderName =
             output == Common::Scalability::DisplayOutput::scRGB_Linear ? "DisplayEncodeScRGB" : "SwapchainBlit";
        auto shader = shaderService->GetByName( shaderName );
        if ( !shader )
            return Common::MakeError( std::format( "missing shader '{}'", shaderName ) );

        Graphic::GraphicsPipelineSpecification spec;
        spec.DebugName         = std::format( "DisplayEncode:{}", shaderName );
        spec.Shader            = shader;
        spec.Framebuffer       = framebuffer;
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.CullMode          = Graphic::CullMode::None;
        const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_EncodePipeline    = pipeline.GetValue();
        m_EncodeExecutor    = Graphic::MaterialExecutor::Create( shaderName, shader );
        m_EncodeFramebuffer = framebuffer;
        m_EncodeOutput      = output;
        LOG_INFO( "[Display] editor UI composite -> back buffer through {} ({})", shaderName,
                  Common::Scalability::DisplayOutputName( output ) );
        return BOOLSUCCESS;
    }

    void VulkanImGui::End()
    {
        // RECORDS FROM OUTSIDE VulkanRenderer.cpp: the interface graph below is executed into the frame's command
        // buffer from this file.
        //
        // Every vkCmd* in VulkanRenderer.cpp is covered by the gates of the only two writers of
        // m_CurrentCommandBuffer (BeginFrame and ExecuteGraph). This function also drives the ImGui backend's
        // own recording and its platform windows, so the device-lost census lists it as a gated row of its own.
        // Left ungated, a loss discovered during a layer's OnUpdate would still be followed by a full frame of
        // interface recording. Nothing would crash — recording does not touch the device and the buffer is never
        // submitted, because PresentFinalImage is gated too — but "no Vulkan call after the loss" would be false,
        // and a claim that is nearly true is the kind this project pays for later.
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        ImGuiIO& io     = ::ImGui::GetIO();
        auto     window = EngineContext::GetInstance().GetWindow();
        io.DisplaySize =
             ImVec2( static_cast<float>( window->GetWidth() ), static_cast<float>( window->GetHeight() ) );

        ::ImGui::Render();

        // THE INTERFACE IS DRAWN ON THE RENDER TASK (UE: Slate's draw elements are built on the game thread and
        // rendered on the render thread). Its graph samples the scene images the views record there, and the queue
        // keeps it behind them, so no fence is needed here: the frame's fence (Application, before the deferred
        // events and the present) waits for both. The draw data goes as a copy, the encode is made ready here.
        if ( const auto encode = EnsureDisplayEncode(); !encode )
        {
            LOG_ERROR( "[VulkanImGui] the display encode: {}", encode.GetError() );
            return;
        }
        if ( const ImDrawData* const source = ::ImGui::GetDrawData() )
        {
            InterfaceDrawBuffer& drawBuffer = AcquireDrawBuffer();
            drawBuffer.Fill( *source );
            drawBuffer.InUse.store( true, std::memory_order_relaxed ); // published by Enqueue's lock
            ::Desert::Engine::RenderTask::Get().Enqueue(
                 "Interface", [this, buffer = &drawBuffer]() -> Common::BoolResultStr
                 {
                     struct ReleaseOnExit
                     {
                         InterfaceDrawBuffer& Buffer;
                         ~ReleaseOnExit()
                         {
                             Buffer.InUse.store( false, std::memory_order_release );
                         }
                     } release{ *buffer };
                     return RecordInterface( buffer->Data );
                 } );
        }

        // THE BOUNDARY OF THE FRAME'S GRAPH. The detached platform windows below are OS windows the ImGui backend
        // draws, each with its own swapchain, render pass, command buffers, submit and present: they are not this
        // frame's output and not the engine's back buffer, so they are not graph nodes. UpdatePlatformWindows
        // creates, resizes and destroys them here (GLFW: this thread only) through the layer's renderer
        // callbacks; a rebuild or a destruction drains the submit thread and idles the queue under its lock first
        // (DetachedWindows::AfterOwnWork); their frames go to the submit thread as copies (DetachedWindows). A
        // frame with no window change waits for neither the render task nor the queue: the backend's state is
        // one thread's at a time (DetachedWindows::RecordMutex).
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            ::ImGui::UpdatePlatformWindows();
            if ( m_Detached )
                m_Detached->Render();
        }
    }

    Common::BoolResultStr VulkanImGui::RecordInterface( ImDrawData& interfaceData )
    {
        // ON THE RENDER TASK (End), behind the views of this frame; inline in serial mode.
        if ( !Graphic::DeviceLost::AllowWork() )
            return BOOLSUCCESS;
        auto& renderer = ::Desert::Graphic::Renderer::GetInstance();

        // THE INTERFACE IS A GRAPH NODE (UE: the viewport's back buffer is registered as an external texture each
        // frame and the last pass writes it). The node clears this frame's acquired image to the editor's
        // backdrop and records the main viewport's draw data into it on the pass's own command buffer; the graph
        // leaves the image in the present layout. It executes here, after every graph this frame recorded during
        // the panels' draw, because the UI samples their images (viewport, previews, thumbnails).
        Graphic::RDG::Builder         graph( "EditorImGui" );
        Graphic::RDG::ExternalTexture backBuffer;
        if ( const auto imported = renderer.ImportBackBuffer( backBuffer ); !imported )
            return Common::MakeFormattedError<bool>( "[VulkanImGui] the back buffer: {}", imported.GetError() );
        const Graphic::RDG::TextureRef target   = graph.RegisterExternal( backBuffer, "BackBuffer" );
        // The acquired image has no picture of its own: without its writer the frame has none (cleared to black).
        graph.SetFaultPolicy( target, Graphic::RDG::ExternalFaultPolicy::FrameFatal );
        Graphic::RDG::TextureDesc compositeDesc;
        compositeDesc.Size.Width  = backBuffer.Desc.Size.Width;
        compositeDesc.Size.Height = backBuffer.Desc.Size.Height;
        compositeDesc.Size.Depth  = 1;
        compositeDesc.Format      = kUICompositeFormat;
        const Graphic::RDG::TextureRef composite = graph.CreateTexture( compositeDesc, "EditorUIComposite" );
        ImDrawData* const              drawData  = &interfaceData;
        VkResult                       recorded  = VK_SUCCESS; // what the backend reported while recording it
        // NeverParallel (RDG::PassFlags): the exec calls the ImGui Vulkan backend, third-party code that is not
        // thread-safe; with every graphics segment cut into chunks (RDG-PARALLEL-1) it would record on a worker.
        graph.AddPass(
             "EditorImGui", Graphic::RDG::PassFlags::Raster | Graphic::RDG::PassFlags::NeverParallel,
             [&]( Graphic::RDG::PassBuilder& pass )
             { pass.ColorTarget( 0, composite, Graphic::RDG::LoadOp::ClearColor( 0.1f, 0.1f, 0.1f, 1.0f ) ); },
             [&]( Graphic::RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto commandBuffer = VulkanRdgBackend::CommandBufferOf( context );
                 if ( !commandBuffer )
                     return Common::MakeError( commandBuffer.GetError() );
                 // The backend's state is one thread's at a time (a detached window records on the submit thread,
                 // its windows are made on the game thread): Capture holds its lock, and returns what the backend
                 // reported as this pass's value and RecordInterface's.
                 recorded = DetachedWindows::Capture( [drawData, cb = commandBuffer.GetValue()]()
                                                      { ImGui_ImplVulkan_RenderDrawData( drawData, cb ); } );
                 if ( recorded != VK_SUCCESS )
                     return Common::MakeFormattedError<bool>( "the ImGui backend recorded the interface: {}",
                                                              VkResultToString( recorded ) );
                 // The backend bound its own vertex / index buffers: the renderer's bind-skip key is stale.
                 Graphic::Renderer::InvalidateGeometryBinding();
                 return BOOLSUCCESS;
             } );
        graph.AddPass(
             "DisplayEncode", Graphic::RDG::PassFlags::Raster,
             [&]( Graphic::RDG::PassBuilder& pass )
             {
                 pass.ColorTarget( 0, target, Graphic::RDG::LoadOp::DontCare() );
                 // RENDER-THREADS M1: the encode executor's view sets exist before the graph records.
                 pass.Prepares( *m_EncodeExecutor );
                 pass.Bindings( m_EncodeLayout.Get( m_EncodePipeline->GetSpecification().Shader ),
                                m_EncodeExecutor->GetRouteFill() )
                      .Texture( DisplayEncodeShaderParameters::FinalColorTexture, composite,
                                Graphic::RDG::Access::SampledGraphics, Graphic::RDG::SubresourceRange::All() )
                      .Sampler( DisplayEncodeShaderParameters::LinearClampSampler,
                                Graphic::RDG::SamplerDesc::LinearClamp() );
             },
             [&]( Graphic::RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const Graphic::RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return renderer.DrawFullscreen( bindings, *m_EncodePipeline, m_EncodeExecutor.get() );
             } );
        graph.Extract( target, backBuffer, Graphic::RDG::Access::Present );
        // Its faults are logged by the graph backend, its own failures by ExecuteGraph; a FrameFault presents
        // black, and the detached windows draw either way.
        (void)::Desert::Graphic::Renderer::ExecuteGraph( graph );
        // The backend's own failure is this call's value (REVIEW-7 R7-6), not a detached window's log line.
        if ( recorded != VK_SUCCESS )
            return Common::MakeFormattedError<bool>( "[VulkanImGui] the main interface's backend recording: {}",
                                                     VkResultToString( recorded ) );
        return BOOLSUCCESS;

    }

} // namespace Desert::Graphic::API::Vulkan
