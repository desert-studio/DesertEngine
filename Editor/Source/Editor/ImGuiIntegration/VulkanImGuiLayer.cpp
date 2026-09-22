#include <Editor/ImGuiIntegration/VulkanImGuiLayer.hpp>

#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <ImGui/backends/imgui_impl_glfw.h>
#include <ImGui/backends/imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

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
        pool_info.poolSizeCount                 = (uint32_t)IM_ARRAYSIZE( pool_sizes );
        pool_info.pPoolSizes                    = pool_sizes;
        VK_CHECK_RESULT( vkCreateDescriptorPool( device, &pool_info, nullptr, &m_ImguiPool ) );

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

        ImGui_ImplVulkan_Init( &init_info, swapchain->GetRenderPass() );

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
        auto device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        // Nothing is outstanding on a lost device, so the wait can only answer VK_ERROR_DEVICE_LOST; the
        // ImGui teardown below is destruction, which stays legal.
        if ( Graphic::DeviceLost::AllowWork() )
            vkDeviceWaitIdle( device );
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ::ImGui::DestroyContext();

        if ( m_ImguiPool != VK_NULL_HANDLE )
        {
            vkDestroyDescriptorPool( device, m_ImguiPool, nullptr );
            m_ImguiPool = VK_NULL_HANDLE;
        }

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImGui::OnUpdate( const Common::Timestep& /*ts*/ )
    {
        return BOOLSUCCESS;
    }

    void VulkanImGui::OnEvent( Common::Event& /*event*/ )
    {
    }

    void VulkanImGui::Begin()
    {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ::ImGui::NewFrame();
    }

    void VulkanImGui::End()
    {
        // THE ONE PLACE THAT RECORDS WITHOUT GOING THROUGH VulkanRendererAPI::m_CurrentCommandBuffer.
        //
        // Every vkCmd* in VulkanRenderer.cpp is covered by one gate, because BeginFrame is the only writer
        // of that field and BeginFrame is gated. This function is the exception: it reaches past the
        // renderer to `queue->GetDrawCommandBuffer()` directly, so no amount of guarding over there
        // reaches it. Left ungated, a loss discovered during a layer's OnUpdate would still be followed by
        // a full frame of interface recording. Nothing would crash — recording does not touch the device
        // and the buffer is never submitted, because PresentFinalImage is gated too — but "no Vulkan call
        // after the loss" would be false, and a claim that is nearly true is the kind this project pays
        // for later.
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        ImGuiIO& io     = ::ImGui::GetIO();
        auto     window = EngineContext::GetInstance().GetWindow();
        io.DisplaySize  = ImVec2( (float)window->GetWidth(), (float)window->GetHeight() );

        ::ImGui::Render();

        auto  swapChain = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() );
        auto& renderer =
             static_cast<VulkanRendererAPI&>( *::Desert::Graphic::Renderer::GetInstance().GetRendererAPI() );

        // ImGui must be rendered within a render pass that target the swapchain
        renderer.BeginSwapChainRenderPass();

        ImGui_ImplVulkan_RenderDrawData( ::ImGui::GetDrawData(),
                                         swapChain->GetVulkanQueue()->GetDrawCommandBuffer() );

        // Reported rather than returned: this helper is void and its caller is ImGui's own render path.
        // A pass that will not close leaves the command buffer inside a render pass, and everything
        // recorded after it is rejected by the driver rather than by us — so the log entry naming this
        // line is the only thing that points at the cause.
        const auto passEnded = renderer.EndRenderPass();
        if ( !passEnded.IsSuccess() )
            LOG_ERROR( "[VulkanImGui] EndRenderPass failed: {}", passEnded.GetError() );

        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            ::ImGui::UpdatePlatformWindows();
            ::ImGui::RenderPlatformWindowsDefault();
        }
    }

} // namespace Desert::Graphic::API::Vulkan
