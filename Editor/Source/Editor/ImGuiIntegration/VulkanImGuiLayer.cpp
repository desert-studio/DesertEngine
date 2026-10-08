#include <Editor/Core/Control/InputInjection.hpp>
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
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <ImGui/backends/imgui_impl_glfw.h>
#include <ImGui/backends/imgui_impl_vulkan.h>

#include <Engine/Core/Glfw.hpp>

namespace
{
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
} // namespace

namespace Desert::Graphic::API::Vulkan
{
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
             CreateRdgRenderPass( device, RdgCompatibleRenderPassKey( { swapchain->GetColorFormat() },
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
        }

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImGui::OnDetach()
    {
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
        if ( const auto step = ::Desert::Editor::Control::PointerInjection::NextStep() )
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

        auto& renderer = ::Desert::Graphic::Renderer::GetInstance();

        // THE INTERFACE IS A GRAPH NODE (UE: the viewport's back buffer is registered as an external texture each
        // frame and the last pass writes it). The node clears this frame's acquired image to the editor's
        // backdrop and records the main viewport's draw data into it on the pass's own command buffer; the graph
        // leaves the image in the present layout. It executes here, after every graph this frame recorded during
        // the panels' draw, because the UI samples their images (viewport, previews, thumbnails).
        Graphic::RDG::Builder         graph( "EditorImGui" );
        Graphic::RDG::ExternalTexture backBuffer;
        if ( const auto imported = renderer.ImportBackBuffer( backBuffer ); !imported )
        {
            // Reported rather than returned: this function is void and its caller is the layer stack's UI pass.
            LOG_ERROR( "[VulkanImGui] the back buffer: {}", imported.GetError() );
            return;
        }
        const Graphic::RDG::TextureRef target   = graph.RegisterExternal( backBuffer, "BackBuffer" );
        // The acquired image has no picture of its own: without its writer the frame has none (cleared to black).
        graph.SetFaultPolicy( target, Graphic::RDG::ExternalFaultPolicy::FrameFatal );
        ImDrawData*                    drawData = ::ImGui::GetDrawData();
        graph.AddPass(
             "EditorImGui", Graphic::RDG::PassFlags::Raster, [&]( Graphic::RDG::PassBuilder& pass )
             { pass.ColorTarget( 0, target, Graphic::RDG::LoadOp::ClearColor( 0.1f, 0.1f, 0.1f, 1.0f ) ); },
             [&]( Graphic::RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto commandBuffer = VulkanRdgBackend::CommandBufferOf( context );
                 if ( !commandBuffer )
                     return Common::MakeError( commandBuffer.GetError() );
                 ImGui_ImplVulkan_RenderDrawData( drawData, commandBuffer.GetValue() );
                 return BOOLSUCCESS;
             } );
        graph.Extract( target, backBuffer, Graphic::RDG::Access::Present );
        // Its faults are logged by the graph backend, its own failures by ExecuteGraph; a FrameFault presents
        // black, and the detached windows below draw either way.
        (void)::Desert::Graphic::Renderer::ExecuteGraph( graph );

        // THE BOUNDARY OF THE FRAME'S GRAPH. The detached platform windows below are OS windows the ImGui
        // backend owns, each with its own swapchain, render pass, command buffers, submit and present — they are
        // not this frame's output and not the engine's back buffer, so they are not graph nodes.
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            // The backend's own vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle for detached windows run
            // on this thread but on the queue uploads flush to from others: hold the device's queue lock (VK1).
            const auto queues =
                 SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->LockQueues();
            ::ImGui::UpdatePlatformWindows();
            ::ImGui::RenderPlatformWindowsDefault();
        }
    }

} // namespace Desert::Graphic::API::Vulkan
