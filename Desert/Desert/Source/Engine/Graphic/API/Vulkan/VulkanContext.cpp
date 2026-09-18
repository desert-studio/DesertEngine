#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/DeviceLost.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <string_view>
#include <vulkan/vulkan.h>

#if defined( DESERT_PLATFORM_WINDOWS )
#ifndef VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
#endif
#endif

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

    Common::ResultStr<VkResult> VulkanContext::CreateVKInstance()
    {
        LOG_TRACE( "VulkanRenderingContext::CreateVKInstance()" );
        DESERT_VERIFY( glfwVulkanSupported(), "GLFW must support Vulkan API" );
        VkApplicationInfo appInfo{};
        appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName   = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION( 1, 2, 0 );
        appInfo.pEngineName        = "No Engine";
        appInfo.engineVersion      = VK_MAKE_VERSION( 1, 3, 0 );
        appInfo.apiVersion         = VK_API_VERSION_1_3;

        std::vector<const char*> instanceExtensions = { VK_KHR_SURFACE_EXTENSION_NAME };
#if defined( DESERT_PLATFORM_WINDOWS )
        instanceExtensions.push_back( VK_KHR_WIN32_SURFACE_EXTENSION_NAME );
#elif defined( DESERT_PLATFORM_MACOS )
        // MoltenVK: the surface goes through Metal, and the implementation is a
        // portability driver, so it must be enumerated explicitly.
        instanceExtensions.push_back( "VK_EXT_metal_surface" );
        instanceExtensions.push_back( "VK_KHR_portability_enumeration" );
        instanceExtensions.push_back( VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME );
#endif
        if ( s_DebugValidation )
        {
            instanceExtensions.push_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
            instanceExtensions.push_back( VK_EXT_DEBUG_REPORT_EXTENSION_NAME );
#if !defined( DESERT_PLATFORM_MACOS ) // already added above
            instanceExtensions.push_back( VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME );
#endif
        }

        VkInstanceCreateInfo createInfo{};
        createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo        = &appInfo;
        createInfo.enabledExtensionCount   = (uint32_t)instanceExtensions.size();
        createInfo.ppEnabledExtensionNames = instanceExtensions.data();
        createInfo.enabledLayerCount       = 0;
#if defined( DESERT_PLATFORM_MACOS )
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

        if ( s_DebugValidation )
        {
            const char* validationLayers = "VK_LAYER_KHRONOS_validation";

            // ZERO-INITIALISED, and that is a fix rather than a tidy-up. The result of the call below is
            // one of the thirteen this tree still drops (see DeviceLostCensus), and a dropped result on a
            // count-then-fill enumeration is only harmless if the count is defined when the call fails —
            // this one was not, so a failed enumeration sized a vector from an uninitialised stack value.
            // The sibling counters in VulkanDevice and VulkanSwapChain were already written this way.
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties( &layerCount, nullptr );

            std::vector<VkLayerProperties> availableLayers( layerCount );
            vkEnumerateInstanceLayerProperties( &layerCount, availableLayers.data() );

            bool validationLayerPresent = false;
            LOG_INFO( "Vulkan Instance Layers:" );
            // The inner loop that used to be here walked `availableLayers` a SECOND time and never touched
            // its own loop variable: every iteration re-ran `strcmp( layer.layerName, validationLayers )`
            // on the OUTER layer, so the whole thing was one comparison performed N times. That unused
            // variable is what `-Wunused-variable` was pointing at. Behaviour is unchanged; the work is now
            // O(N) instead of O(N^2).
            for ( const VkLayerProperties& layer : availableLayers )
            {
                LOG_INFO( "  {0}", layer.layerName );
                if ( strcmp( layer.layerName, validationLayers ) == 0 )
                    validationLayerPresent = true;
            }

            if ( validationLayerPresent )
            {
                createInfo.ppEnabledLayerNames = &validationLayers;
                createInfo.enabledLayerCount   = 1;
            }
            else
            {
                LOG_ERROR( "Validation layer VK_LAYER_KHRONOS_validation not present, validation is disabled" );
            }
        }

        VK_RETURN_RESULT_IF_FALSE( vkCreateInstance( &createInfo, nullptr, &s_VulkanInstance ) );
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