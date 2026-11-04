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
#include <Engine/Graphic/DeviceLost.hpp>

#include <ImGui/backends/imgui_impl_glfw.h>
#include <ImGui/backends/imgui_impl_vulkan.h>

#include <Engine/Core/Glfw.hpp>

#include <atomic>
#include <cstring>
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

        // The pipeline is built against the render graph's canonical pass for the back buffer's format: the
        // interface is drawn inside a graph node (End), and render-pass compatibility is formats and samples only.
        const Common::ResultStr<VkRenderPass> imguiPass =
             CreateRdgRenderPass( device, RdgCompatibleRenderPassKey( { kUICompositeVkFormat },
                                                                      VK_FORMAT_UNDEFINED, false, 1 ) );
        if ( !imguiPass )
            return Common::MakeFormattedError<bool>( "the ImGui render pass: {}", imguiPass.GetError() );
        m_ImguiRenderPass = imguiPass.GetValue();

        ImGui_ImplVulkan_Init( &init_info, m_ImguiRenderPass );

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
        ImGui_ImplVulkan_Shutdown();
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

        // THE DETACHED WINDOWS JOIN FIRST. They are GLFW windows (main thread only) drawn by the same ImGui
        // backend, which is not thread-safe, and today's order records the main interface before them. With only
        // the main viewport there is nothing to draw and UpdatePlatformWindows touches no backend state: no join.
        // THE BOUNDARY OF THE FRAME'S GRAPH. The detached platform windows below are OS windows the ImGui backend
        // owns, each with its own swapchain, render pass, command buffers, submit and present — they are not this
        // frame's output and not the engine's back buffer, so they are not graph nodes.
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            // The backend's own vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle for detached windows run
            // on this thread but on the queue uploads flush to from others: hold the device's queue lock (VK1).
            if ( ::ImGui::GetPlatformIO().Viewports.Size > 1 )
                ::Desert::Engine::RenderTask::Get().Join();
            const auto queues =
                 SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->LockQueues();
            ::ImGui::UpdatePlatformWindows();
            ::ImGui::RenderPlatformWindowsDefault();
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
                 ImGui_ImplVulkan_RenderDrawData( drawData, commandBuffer.GetValue() );
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
        return BOOLSUCCESS;

    }

} // namespace Desert::Graphic::API::Vulkan
