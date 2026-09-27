#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>
#include <vk-bootstrap/VkBootstrap.h>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Graphic/ViewResources.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <string_view>
#include <vulkan/vulkan.h>

namespace Desert::Graphic::API::Vulkan
{
    static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugReportCallback( VkDebugReportFlagsEXT      flags,
                                                                     VkDebugReportObjectTypeEXT objectType,
                                                                     uint64_t object, size_t location,
                                                                     int32_t messageCode, const char* pLayerPrefix,
                                                                     const char* pMessage, void* pUserData )
    {
        (void)flags;
        (void)object;
        (void)location;
        (void)messageCode;
        (void)pUserData;
        (void)pLayerPrefix; // Unused arguments

        // THE DRIVER SAYS IT HERE FIRST, AND EARLIER THAN ANY RETURN CODE DOES. A submit executes
        // asynchronously, so MoltenVK learns the device is gone while the engine is between calls and has
        // nothing to check; the way it tells us is this callback:
        //   "VK_ERROR_OUT_OF_DEVICE_MEMORY: Lost VkDevice after MTLCommandBuffer ... execution failed"
        // Latching on it moves the discovery to the very first line of the incident instead of the second.
        //
        // MATCHING ON DRIVER TEXT IS FRAGILE, AND THAT IS ACCEPTABLE HERE ONLY BECAUSE IT IS ADDITIVE:
        // every return-code path latches too, so if a driver update reworded this the behaviour degrades
        // to what the return codes already give — one call later, not broken. It also only exists in a
        // build with validation on; Release has no callback installed at all.
        if ( pMessage != nullptr )
        {
            const std::string_view message( pMessage );
            if ( message.find( "Lost VkDevice" ) != std::string_view::npos ||
                 message.find( "VK_ERROR_DEVICE_LOST" ) != std::string_view::npos )
            {
                (void)Graphic::DeviceLost::Report( "the Vulkan driver's own debug callback", message );
                return VK_FALSE;
            }
        }

        // EVERY MESSAGE AFTER THE LATCH IS A CONSEQUENCE, and printing it at WARN alongside the real cause
        // is exactly how "pFences[0] is in use" came to look like a synchronisation defect of ours. It is
        // still printed — nothing is hidden — but marked as what it is and dropped to TRACE, so the one
        // explanation stays the loudest thing in the log.
        if ( Graphic::DeviceLost::IsLost() )
        {
            LOG_TRACE( "VulkanDebugCallback (consequence of the lost device, not a defect):\n  Message: {0}",
                       pMessage != nullptr ? pMessage : "" );
            return VK_FALSE;
        }

        LOG_WARN( "VulkanDebugCallback:\n  Object Type: {0}\n  Message: {1}", (int)objectType, pMessage );
        return VK_FALSE;
    }

#ifdef DESERT_CONFIG_DEBUG
    static bool s_DebugValidation = true;
#else
    static bool s_DebugValidation = false;
#endif

    VulkanContext::VulkanContext( const std::shared_ptr<Window>& window ) : m_Window( window )
    {
        // A constructor cannot return a result, and there is no partially-working VulkanContext: with no
        // instance every device, surface and swapchain below dereferences null, so the process dies a few
        // frames later somewhere that says nothing about the cause. DESERT_VERIFY names the cause and
        // aborts in both configurations — the same instrument the line below already uses for the
        // adjacent precondition, and the one this file's own `glfwVulkanSupported()` check picked.
        const auto instance = CreateVKInstance();
        DESERT_VERIFY( instance.IsSuccess(), "Vulkan instance could not be created: {}", instance.GetError() );
    }

    namespace
    {
        // Held for the process: device selection (DeviceCapsProbe) is built from it, and it is what
        // VulkanContext::GetVulkanInstance() hands out.
        vkb::Instance s_BootstrapInstance;
    } // namespace

    const vkb::Instance& VulkanContext::GetBootstrapInstance()
    {
        return s_BootstrapInstance;
    }

    Common::ResultStr<VkResult> VulkanContext::CreateVKInstance()
    {
        LOG_TRACE( "VulkanRenderingContext::CreateVKInstance()" );
        DESERT_VERIFY( glfwVulkanSupported(), "GLFW must support Vulkan API" );

        // vk-bootstrap adds what every windowed instance needs: VK_KHR_surface and the platform surface
        // (win32 / metal), VK_KHR_portability_enumeration plus its create flag where the loader offers it
        // (MoltenVK). The loader is linked, so its vkGetInstanceProcAddr is handed over instead of letting
        // vk-bootstrap dlopen a second copy.
        //
        // API VERSION: 1.3 is what the engine is written against and what the instance advertises; the
        // instance itself only has to be 1.1 (kMinimumDeviceApiVersion). A device older than 1.3 is still
        // driven at its own version — VMA and routing use min(device, 1.3), see DeviceCaps.
        vkb::InstanceBuilder builder( vkGetInstanceProcAddr );
        builder.set_app_name( "Desert Engine" )
             .set_engine_name( "Desert Engine" )
             .require_api_version( kMaximumApiVersion )
             .set_minimum_instance_version( kMinimumDeviceApiVersion );

        if ( s_DebugValidation )
        {
            const auto system = vkb::SystemInfo::get_system_info( vkGetInstanceProcAddr );
            if ( system && system.value().validation_layers_available )
                builder.request_validation_layers( true );
            else
                LOG_ERROR( "Validation layer VK_LAYER_KHRONOS_validation not present, validation is disabled" );
            builder.enable_extension( VK_EXT_DEBUG_UTILS_EXTENSION_NAME )
                 .enable_extension( VK_EXT_DEBUG_REPORT_EXTENSION_NAME );
        }

        auto built = builder.build();
        if ( !built )
            return Common::MakeFormattedError<VkResult>(
                 "vk-bootstrap could not create the Vulkan instance: {} ({})", built.error().message(),
                 static_cast<int>( built.vk_result() ) );
        s_BootstrapInstance = built.value();
        s_VulkanInstance    = s_BootstrapInstance.instance;
        LOG_INFO( "[Vulkan] instance created through vk-bootstrap (engine targets Vulkan {}, minimum device {})",
                  FormatApiVersion( kMaximumApiVersion ), FormatApiVersion( kMinimumDeviceApiVersion ) );

        if ( s_DebugValidation )
        {
            auto vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(
                 s_VulkanInstance, "vkCreateDebugReportCallbackEXT" );
            DESERT_VERIFY( vkCreateDebugReportCallbackEXT != NULL, "" );
            VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
            debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
            debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                                    VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
            debug_report_ci.pfnCallback = VulkanDebugReportCallback;
            debug_report_ci.pUserData   = NULL;
            VK_CHECK_RESULT( vkCreateDebugReportCallbackEXT( s_VulkanInstance, &debug_report_ci, nullptr,
                                                             &m_DebugReportCallback ) );
        }
        VulkanLoadDebugUtilsExtensions( s_VulkanInstance );
        return Common::MakeSuccess( VK_SUCCESS );
    }

    Common::BoolResultStr VulkanContext::BeginFrame() const
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; no image is acquired for this frame." );

        const auto window = m_Window.lock();
        if ( !window )
        {
            DESERT_VERIFY( false );
        }

        const auto& vulkanQueue = SP_CAST( VulkanSwapChain, window->GetWindowSwapChain() )->GetVulkanQueue();
        vulkanQueue->PrepareFrame();

        // Asked AFTER the acquire too. A device that died during the PREVIOUS frame's submit is discovered
        // here — the submit itself returned VK_SUCCESS and the failure arrived asynchronously — and saying
        // so in this frame's result is what lets the loop stop now instead of one frame of illegal calls
        // later.
        if ( Graphic::DeviceLost::IsLost() )
            return Common::MakeError( "the device was lost while preparing this frame." );

        return BOOLSUCCESS;
    }

    void VulkanContext::Shutdown()
    {
        // WHAT INIT CREATED, IN REVERSE, AND IT IS REACHED NOW. `RendererContext::Shutdown` is pure
        // virtual and, until this commit, was called from NOWHERE: this body existed, destroyed the VMA
        // allocator, and never ran. VulkanLogicalDevice::Destroy calls it — the last moment at which the
        // VkDevice is still alive — because every object released below is a child of that device.
        CommandBufferAllocator::GetInstance().Destroy();

        if ( m_VulkanAllocator )
        {
            // The deferred-deletion queue is drained BEFORE the allocator is destroyed, for the obvious
            // reason: vmaDestroyAllocator takes the allocations with it without ever touching the VkBuffer
            // and VkImage handles built on them.
            // The frame context outlives every view (it is a process static), so its copies are handed to
            // the queue here, where the drain below still collects them; left to static destruction they
            // would be released into an allocator that no longer exists.
            ViewResourceRegistry::FrameContext().Clear();

            const std::size_t drained = m_VulkanAllocator->DrainDeletionQueue();
            if ( drained > 0 )
            {
                LOG_INFO( "[Allocator] drained {} deferred GPU object destruction(s) at teardown; these had "
                          "no frame left to be collected on.",
                          drained );
            }
            m_VulkanAllocator->Shutdown();
        }
    }

    void VulkanContext::Init()
    {
        const auto logicalDeivce = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );

        m_VulkanAllocator = std::make_unique<VulkanAllocator>();
        m_VulkanAllocator->Init( logicalDeivce, s_VulkanInstance );

        CommandBufferAllocator::CreateInstance( logicalDeivce );

        const auto window = m_Window.lock();
        if ( !window )
        {
            DESERT_VERIFY( false );
        }
    }

} // namespace Desert::Graphic::API::Vulkan