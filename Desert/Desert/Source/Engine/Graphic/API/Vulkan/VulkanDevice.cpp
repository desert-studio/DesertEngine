#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp> // GetImageVulkanFormat — engine format -> VkFormat
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Engine/Core/EngineContext.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/VFS.hpp>

#include <algorithm> // std::max — largest device-local heap
#include <filesystem>
#include <fstream>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        std::filesystem::path PipelineCacheDiskPath()
        {
            return Common::Constants::Path::COOKED_PATH / "PipelineCache.bin";
        }
    } // namespace

    VulkanPhysicalDevice::VulkanPhysicalDevice()
    {
        CreateDevice();
    }

    Common::ResultStr<bool> VulkanPhysicalDevice::CreateDevice()
    {
        auto& instance =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanInstance();
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices( instance, &deviceCount, nullptr );
        DESERT_VERIFY( deviceCount, "Failed to find GPUs with Vulkan support!" );
        std::vector<VkPhysicalDevice> devices( deviceCount );
        vkEnumeratePhysicalDevices( instance, &deviceCount, devices.data() );

        VkPhysicalDevice           selectedPhysicalDevice = nullptr;
        VkPhysicalDeviceProperties deviceProperties{};
        VkPhysicalDeviceFeatures   deviceFeatures{};

        // Pick the BEST available GPU by type: discrete > integrated > virtual > anything. An
        // integrated GPU is the NORMAL case on Apple Silicon (there is no discrete one), not a
        // fallback — so this is a ranking, not a requirement, and it never warns. Geometry-shader
        // support is NOT required: MoltenVK has none and nothing in the renderer uses them.
        auto deviceScore = []( VkPhysicalDevice device )
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties( device, &props );
            switch ( props.deviceType )
            {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
                default:                                     return 0;
            }
        };

        int bestScore = -1;
        for ( const auto& device : devices )
        {
            const int score = deviceScore( device );
            if ( score > bestScore )
            {
                bestScore              = score;
                selectedPhysicalDevice = device;
            }
        }

        if ( selectedPhysicalDevice )
        {
            vkGetPhysicalDeviceProperties( selectedPhysicalDevice, &deviceProperties );
            vkGetPhysicalDeviceFeatures( selectedPhysicalDevice, &deviceFeatures );

            m_Capabilities.MaxStorageBufferSize   = deviceProperties.limits.maxStorageBufferRange;
            m_Capabilities.StorageBufferAlignment = deviceProperties.limits.minStorageBufferOffsetAlignment;
            m_Capabilities.SupportsWideLines      = deviceFeatures.wideLines == VK_TRUE;
            m_Capabilities.MaxLineWidth           = deviceProperties.limits.lineWidthRange[1];
            m_Capabilities.SupportsAnisotropy     = deviceFeatures.samplerAnisotropy == VK_TRUE;
            m_Capabilities.MaxAnisotropy          = deviceProperties.limits.maxSamplerAnisotropy;
            m_Capabilities.SupportsNonSolidFill   = deviceFeatures.fillModeNonSolid == VK_TRUE;

            // --- Identity ---
            m_Capabilities.Name = deviceProperties.deviceName;
            switch ( deviceProperties.deviceType )
            {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    m_Capabilities.Type = Engine::DeviceType::Discrete;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    m_Capabilities.Type = Engine::DeviceType::Integrated;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    m_Capabilities.Type = Engine::DeviceType::Virtual;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    m_Capabilities.Type = Engine::DeviceType::CPU;
                    break;
                default:
                    m_Capabilities.Type = Engine::DeviceType::Unknown;
                    break;
            }
            // PCI-SIG vendor IDs. Apple reports its own rather than a PCI one on Apple Silicon.
            switch ( deviceProperties.vendorID )
            {
                case 0x10DE: m_Capabilities.VendorName = "NVIDIA"; break;
                case 0x1002:
                case 0x1022: m_Capabilities.VendorName = "AMD"; break;
                case 0x8086: m_Capabilities.VendorName = "Intel"; break;
                case 0x106B: m_Capabilities.VendorName = "Apple"; break;
                case 0x13B5: m_Capabilities.VendorName = "ARM"; break;
                case 0x5143: m_Capabilities.VendorName = "Qualcomm"; break;
                default:     m_Capabilities.VendorName = "Unknown"; break;
            }

            // --- Limits the renderer actually branches on ---
            m_Capabilities.MaxPushConstantSize    = deviceProperties.limits.maxPushConstantsSize;
            m_Capabilities.MaxTexture2DSize       = deviceProperties.limits.maxImageDimension2D;
            m_Capabilities.MaxTextureArrayLayers  = deviceProperties.limits.maxImageArrayLayers;
            m_Capabilities.MaxColorAttachments    = deviceProperties.limits.maxColorAttachments;
            m_Capabilities.SupportsGeometryShaders   = deviceFeatures.geometryShader == VK_TRUE;
            m_Capabilities.SupportsTessellation      = deviceFeatures.tessellationShader == VK_TRUE;
            m_Capabilities.SupportsMultiDrawIndirect = deviceFeatures.multiDrawIndirect == VK_TRUE;
            m_Capabilities.SupportsTimestampQueries  = deviceProperties.limits.timestampComputeAndGraphics == VK_TRUE;
            m_Capabilities.TimestampPeriodNs         = deviceProperties.limits.timestampPeriod;

            // MSAA counts usable for BOTH colour and depth — a count only one of them supports is useless
            // to a framebuffer that has each.
            const VkSampleCountFlags sampleCounts = deviceProperties.limits.framebufferColorSampleCounts &
                                                    deviceProperties.limits.framebufferDepthSampleCounts;
            m_Capabilities.MSAASampleMask = static_cast<uint32_t>( sampleCounts );

            // Float render targets: RGBA32F must be usable as a colour attachment AND blendable, which is
            // what every accumulating screen-space pass (SSR trace/resolve, GI resolve, bloom) relies on.
            {
                VkFormatProperties fmt{};
                vkGetPhysicalDeviceFormatProperties( selectedPhysicalDevice, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                     &fmt );
                constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
                m_Capabilities.SupportsFloatRenderTargets =
                     ( fmt.optimalTilingFeatures & kNeeded ) == kNeeded;
            }

            // Device-local heap size — the budget the screen-space passes are weighed against.
            {
                VkPhysicalDeviceMemoryProperties memProps{};
                vkGetPhysicalDeviceMemoryProperties( selectedPhysicalDevice, &memProps );
                for ( uint32_t i = 0; i < memProps.memoryHeapCount; ++i )
                    if ( memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT )
                        m_Capabilities.VideoMemory =
                             std::max<uint64_t>( m_Capabilities.VideoMemory, memProps.memoryHeaps[i].size );
            }

            const char* typeName = m_Capabilities.Type == Engine::DeviceType::Discrete     ? "discrete"
                                   : m_Capabilities.Type == Engine::DeviceType::Integrated ? "integrated"
                                                                                           : "other";
            LOG_INFO( "[Vulkan] GPU: {} ({}, {}, {} MB VRAM)", m_Capabilities.Name, m_Capabilities.VendorName,
                      typeName, m_Capabilities.VideoMemory / ( 1024ull * 1024ull ) );
            LOG_INFO( "[Vulkan] Caps: maxMSAA {}x, maxTex2D {}, colorAttachments {}, float RTs {}, "
                      "timestamps {} (period {} ns/tick)",
                      m_Capabilities.MaxMSAASamples(), m_Capabilities.MaxTexture2DSize,
                      m_Capabilities.MaxColorAttachments, m_Capabilities.SupportsFloatRenderTargets ? "yes" : "NO",
                      m_Capabilities.SupportsTimestampQueries ? "yes" : "no", m_Capabilities.TimestampPeriodNs );
        }

        // Publish anisotropy support to the low-level sampler-creation path (0 = unsupported -> no aniso).
        Graphic::RenderConfig::MaxAnisotropy =
             m_Capabilities.SupportsAnisotropy ? m_Capabilities.MaxAnisotropy : 0.0f;
        Graphic::RenderConfig::WideLines = m_Capabilities.SupportsWideLines; // clamp debug-line width if false

        // Publish the device's MSAA ceiling. Derived from the capability computed above rather than
        // re-querying the driver — one source of truth, so the value the renderer clamps to and the value
        // GetCapabilities() reports can never disagree.
        Graphic::RenderConfig::MaxMSAASamples = static_cast<int>( m_Capabilities.MaxMSAASamples() );

        DESERT_VERIFY( selectedPhysicalDevice, "Could not find any physical devices!" );

        m_PhysicalDevice = selectedPhysicalDevice;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties( m_PhysicalDevice, &queueFamilyCount, nullptr );

        m_QueueFamilyProperties.resize( queueFamilyCount );
        vkGetPhysicalDeviceQueueFamilyProperties( m_PhysicalDevice, &queueFamilyCount,
                                                  m_QueueFamilyProperties.data() );

        int requestedQueueTypes = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        m_QueueFamilyIndices    = GetQueueFamilyIndices( requestedQueueTypes );

        // REFUSE HERE, ONCE, BY NAME — this is what makes every reader downstream able to take a plain
        // index. The three lines below used to read `.value_or( -1 )`, which handed Vulkan a queue family
        // index of 0xFFFFFFFF and let device creation carry on; the five readers further out unwrapped the
        // same optionals with `.value()` and `*`, so a driver reporting no graphics family produced either
        // an uncaught std::bad_optional_access inside a constructor or undefined behaviour, depending on
        // which of them ran first. GetQueueFamilyIndices() already falls the compute and transfer families
        // back to the graphics one, so all three are present exactly when the graphics family is.
        if ( !m_QueueFamilyIndices.GraphicsFamily || !m_QueueFamilyIndices.ComputeFamily ||
             !m_QueueFamilyIndices.TransferFamily )
        {
            return Common::MakeFormattedError<bool>(
                 "the selected physical device reports no usable queue families for the work this engine "
                 "submits — graphics: {}, compute: {}, transfer: {} (of {} families the driver listed)",
                 m_QueueFamilyIndices.GraphicsFamily ? std::to_string( *m_QueueFamilyIndices.GraphicsFamily )
                                                     : "none",
                 m_QueueFamilyIndices.ComputeFamily ? std::to_string( *m_QueueFamilyIndices.ComputeFamily )
                                                    : "none",
                 m_QueueFamilyIndices.TransferFamily ? std::to_string( *m_QueueFamilyIndices.TransferFamily )
                                                     : "none",
                 m_QueueFamilyProperties.size() );
        }

        m_ResolvedQueueFamilies.Graphics = *m_QueueFamilyIndices.GraphicsFamily;
        m_ResolvedQueueFamilies.Compute  = *m_QueueFamilyIndices.ComputeFamily;
        m_ResolvedQueueFamilies.Transfer = *m_QueueFamilyIndices.TransferFamily;
        m_QueueFamiliesResolved          = true;

        static constexpr float queuePriority = 1.0f;

        if ( requestedQueueTypes & VK_QUEUE_GRAPHICS_BIT )
        {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = m_ResolvedQueueFamilies.Graphics;
            queueCreateInfo.queueCount       = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            m_QueueCreateInfos.push_back( queueCreateInfo );
        }

        if ( (requestedQueueTypes & VK_QUEUE_COMPUTE_BIT) && 
             (m_QueueFamilyIndices.ComputeFamily != m_QueueFamilyIndices.GraphicsFamily) )
        {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = m_ResolvedQueueFamilies.Compute;
            queueCreateInfo.queueCount       = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            m_QueueCreateInfos.push_back( queueCreateInfo );
        }

        if ( (requestedQueueTypes & VK_QUEUE_TRANSFER_BIT) &&
             (m_QueueFamilyIndices.TransferFamily != m_QueueFamilyIndices.GraphicsFamily &&
              m_QueueFamilyIndices.TransferFamily != m_QueueFamilyIndices.ComputeFamily) )
        {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = m_ResolvedQueueFamilies.Transfer;
            queueCreateInfo.queueCount       = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            m_QueueCreateInfos.push_back( queueCreateInfo );
        }

        // Zero-initialised: the enumeration's result is dropped, so a failure must still leave a
        // defined count. See VulkanContext::CreateVKInstance.
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties( m_PhysicalDevice, nullptr, &extensionCount, nullptr );

        if ( extensionCount )
        {
            std::vector<VkExtensionProperties> availableExtensions( extensionCount );
            vkEnumerateDeviceExtensionProperties( m_PhysicalDevice, nullptr, &extensionCount,
                                                  availableExtensions.data() );

            for ( const auto& ext : availableExtensions )
            {
                m_SupportedExtensions.emplace( ext.extensionName );
            }
        }

        m_DepthFormat = FindDepthFormat();

        return Common::MakeSuccess( true );
    }

    std::shared_ptr<VulkanPhysicalDevice> VulkanPhysicalDevice::Create()
    {
        return std::make_shared<VulkanPhysicalDevice>();
    }

    VulkanLogicalDevice::VulkanLogicalDevice()
    {
        m_PhysicalDevice = std::make_shared<VulkanPhysicalDevice>();
        CreateDevice();

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties( m_PhysicalDevice->GetVulkanPhysicalDevice(), &props );
        m_DeviceName = props.deviceName;
    }

    VulkanLogicalDevice::~VulkanLogicalDevice()
    try
    {
        Destroy();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanLogicalDevice" )

    const Engine::DeviceCapabilities& VulkanLogicalDevice::GetCapabilities() const
    {
        return m_PhysicalDevice->GetCapabilities();
    }

    void VulkanLogicalDevice::WaitIdle() const
    {
        // Nothing can be outstanding on a lost device, so this can only report the loss again. Skipping it
        // is what keeps the teardown path free of calls that add nothing but log noise.
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        const VkResult idle = vkDeviceWaitIdle( m_LogicalDevice );
        if ( idle != VK_SUCCESS && !NoteIfDeviceLost( idle, "vkDeviceWaitIdle", __FILE__, __LINE__ ) )
            LOG_ERROR( "[Device] vkDeviceWaitIdle failed: {}", VkResultToString( idle ) );
    }

    Engine::DeviceMemoryReport VulkanLogicalDevice::QueryMemory() const
    {
        Engine::DeviceMemoryReport report;

        // THE QUERY IS MADE HERE, EVERY CALL, INTO A LOCAL. There is no member holding the last answer,
        // deliberately: the spec's words for these numbers are "a rough estimate" that is "not
        // invariant", so a stored copy would be a reading of an instant that has passed, presented with
        // the authority of a fact. Whatever this costs, it costs less than a wrong budget.
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        VkPhysicalDeviceMemoryProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        // Chained ONLY when the extension was enabled on this device. Chaining it otherwise is undefined
        // behaviour whose observed shape is a struct nobody wrote to, i.e. zeros — and zero usage on a
        // loaded device is the one wrong answer this whole readout exists to stop us believing.
        properties.pNext = m_MemoryBudgetEnabled ? static_cast<void*>( &budget ) : nullptr;

        vkGetPhysicalDeviceMemoryProperties2( m_PhysicalDevice->GetVulkanPhysicalDevice(), &properties );

        report.BudgetKnown = m_MemoryBudgetEnabled;
        const uint32_t heaps =
             std::min( properties.memoryProperties.memoryHeapCount, uint32_t{ VK_MAX_MEMORY_HEAPS } );
        report.Heaps.reserve( heaps );
        for ( uint32_t heap = 0; heap < heaps; ++heap )
        {
            Engine::DeviceMemoryHeap row;
            row.Size = properties.memoryProperties.memoryHeaps[heap].size;
            row.DeviceLocal =
                 ( properties.memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) != 0;
            if ( m_MemoryBudgetEnabled )
            {
                row.Budget = budget.heapBudget[heap];
                row.Usage  = budget.heapUsage[heap];
            }
            report.Heaps.push_back( row );
        }

        return report;
    }

    std::string VulkanLogicalDevice::GetName() const
    {
        return m_DeviceName;
    }

    bool VulkanLogicalDevice::IsFormatSupported( ::Desert::Core::Formats::ImageFormat format,
                                                 Engine::FormatUsage                  usage ) const
    {
        const VkFormat vkFormat = GetImageVulkanFormat( format );
        if ( vkFormat == VK_FORMAT_UNDEFINED )
            return false;

        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties( m_PhysicalDevice->GetVulkanPhysicalDevice(), vkFormat, &props );

        // Optimal tiling only: every image the engine creates is VK_IMAGE_TILING_OPTIMAL. Asking about
        // linear tiling would answer a question nothing here can act on.
        const VkFormatFeatureFlags features = props.optimalTilingFeatures;

        VkFormatFeatureFlags required = 0;
        if ( usage & Engine::FormatUsage_Sampled )
            required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ( usage & Engine::FormatUsage_ColorAttachment )
            required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if ( usage & Engine::FormatUsage_DepthAttachment )
            required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ( usage & Engine::FormatUsage_Blendable )
            required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if ( usage & Engine::FormatUsage_Storage )
            required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ( usage & Engine::FormatUsage_LinearFilter )
            required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

        return ( features & required ) == required;
    }

    void VulkanLogicalDevice::Destroy()
    {
        if ( m_LogicalDevice != VK_NULL_HANDLE )
        {
            // Same reason as WaitIdle above; vkDestroyPipelineCache and vkDestroyDevice below stay legal on
            // a lost device, which is what makes an orderly close possible at all.
            if ( Graphic::DeviceLost::AllowWork() )
                vkDeviceWaitIdle( m_LogicalDevice );
            if ( m_PipelineCache != VK_NULL_HANDLE )
            {
                SavePipelineCache(); // persist the driver's accumulated pipeline binaries for next run
                vkDestroyPipelineCache( m_LogicalDevice, m_PipelineCache, nullptr );
                m_PipelineCache = VK_NULL_HANDLE;
            }
            vkDestroyDevice( m_LogicalDevice, nullptr );
            m_LogicalDevice = VK_NULL_HANDLE;
        }
    }

    Common::ResultStr<bool> VulkanLogicalDevice::CreateDevice()
    {
        VkDeviceCreateInfo       createInfo{};
        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.tessellationShader = VK_TRUE; // required for the terrain tessellation pipeline
        if ( m_PhysicalDevice->m_Capabilities.SupportsWideLines )
        {
            deviceFeatures.wideLines = VK_TRUE;
        }
        // Independent of wideLines: MoltenVK offers non-solid fill (wireframe) but no wide lines.
        if ( m_PhysicalDevice->m_Capabilities.SupportsNonSolidFill )
        {
            deviceFeatures.fillModeNonSolid = VK_TRUE;
        }
        if ( m_PhysicalDevice->m_Capabilities.SupportsAnisotropy )
        {
            deviceFeatures.samplerAnisotropy = VK_TRUE;
        }
        createInfo.sType                 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos     = m_PhysicalDevice->m_QueueCreateInfos.data();
        createInfo.queueCreateInfoCount  = (uint32_t)m_PhysicalDevice->m_QueueCreateInfos.size();
        createInfo.pEnabledFeatures      = &deviceFeatures;

        std::vector<const char*> deviceExtensions;
        DESERT_VERIFY( m_PhysicalDevice->IsExtensionSupported( VK_KHR_SWAPCHAIN_EXTENSION_NAME ) );
        deviceExtensions.push_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
#if defined( DESERT_PLATFORM_MACOS )
        // The spec requires VK_KHR_portability_subset to be enabled when the
        // implementation (MoltenVK) advertises it.
        if ( m_PhysicalDevice->IsExtensionSupported( "VK_KHR_portability_subset" ) )
        {
            deviceExtensions.push_back( "VK_KHR_portability_subset" );
        }
#endif

        // VK_EXT_memory_budget — THE ONLY WAY TO ASK THE DRIVER WHAT WE ACTUALLY HOLD.
        //
        // Optional, because it is an extension and a device may not have it, and the reading reports
        // `BudgetKnown = false` in that case rather than zeros. Present on MoltenVK 1.1.357 / Apple M1
        // Pro (probed on this machine: 130 device extensions, this among them, one 16 GiB heap whose
        // budget reads 11.84 GiB) and on every Windows driver we target. The instance side it needs,
        // VK_KHR_get_physical_device_properties2, is already enabled unconditionally in
        // VulkanContext.cpp — so the only thing missing was this line.
        if ( m_PhysicalDevice->IsExtensionSupported( VK_EXT_MEMORY_BUDGET_EXTENSION_NAME ) )
        {
            deviceExtensions.push_back( VK_EXT_MEMORY_BUDGET_EXTENSION_NAME );
            m_MemoryBudgetEnabled = true;
        }
        else
        {
            LOG_WARN( "[Device] VK_EXT_memory_budget is absent; device-memory usage will report as "
                      "unknown rather than as zero." );
        }

        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        createInfo.enabledExtensionCount   = (uint32_t)deviceExtensions.size();

        VK_CHECK_RESULT( vkCreateDevice( m_PhysicalDevice->GetVulkanPhysicalDevice(), &createInfo, nullptr,
                                         &m_LogicalDevice ) );

        vkGetDeviceQueue( m_LogicalDevice, m_PhysicalDevice->GetGraphicsFamily(), 0, &m_GraphicsQueue );
        vkGetDeviceQueue( m_LogicalDevice, m_PhysicalDevice->GetComputeFamily(), 0, &m_ComputeQueue );
        vkGetDeviceQueue( m_LogicalDevice, m_PhysicalDevice->GetTransferFamily(), 0, &m_TransferQueue );

        CreatePipelineCache();

        return Common::MakeSuccess( true );
    }

    void VulkanLogicalDevice::CreatePipelineCache()
    {
        // Seed the cache from the previous run's blob if present. The driver checks the header
        // (vendorID/deviceID/pipelineCacheUUID) and ignores it if it doesn't match this GPU/driver, so a
        // stale or cross-machine file is harmless — it just starts the cache empty again.
        std::vector<char> initial;
        {
            std::error_code ec;
            const auto      path = PipelineCacheDiskPath();
            const auto      size = std::filesystem::file_size( path, ec );
            if ( !ec && size > 0 )
            {
                std::ifstream in( path, std::ios::binary );
                if ( in )
                {
                    initial.resize( static_cast<size_t>( size ) );
                    in.read( initial.data(), static_cast<std::streamsize>( size ) );
                    if ( !in )
                        initial.clear();
                }
            }
            // No loose blob: a packaged game ships the packaging machine's one inside Content.dpak
            // (the census packs the whole Cooked/ tree, and the pak is mounted before the device
            // exists). Same-GPU installs seed from it; the driver's header check discards it anywhere
            // else — exactly the harmlessness contract above.
            if ( initial.empty() )
                if ( auto packed = Common::Utils::VFS::ReadFile( path ) )
                    initial.assign( packed->begin(), packed->end() );
        }

        VkPipelineCacheCreateInfo info{ .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                        .initialDataSize = initial.size(),
                                        .pInitialData    = initial.empty() ? nullptr : initial.data() };
        if ( vkCreatePipelineCache( m_LogicalDevice, &info, nullptr, &m_PipelineCache ) != VK_SUCCESS )
            m_PipelineCache = VK_NULL_HANDLE; // non-fatal: pipeline creation just falls back to no cache
    }

    void VulkanLogicalDevice::SavePipelineCache() const
    {
        if ( m_PipelineCache == VK_NULL_HANDLE )
            return;

        // A lost device has nothing to hand back, and asking it is one more call after the point where the
        // engine promised to stop. Losing one run's accumulated pipeline binaries costs a slower next
        // start; it costs nothing that matters.
        if ( !Graphic::DeviceLost::AllowWork() )
            return;

        size_t size = 0;
        if ( vkGetPipelineCacheData( m_LogicalDevice, m_PipelineCache, &size, nullptr ) != VK_SUCCESS ||
             size == 0 )
            return;
        std::vector<char> data( size );
        if ( vkGetPipelineCacheData( m_LogicalDevice, m_PipelineCache, &size, data.data() ) != VK_SUCCESS )
            return;

        const auto      path = PipelineCacheDiskPath();
        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        if ( out ) // read-only install (e.g. inside an .app bundle) — cache is best-effort
            out.write( data.data(), static_cast<std::streamsize>( size ) );
    }

    VulkanPhysicalDevice::QueueFamilyIndices VulkanPhysicalDevice::GetQueueFamilyIndices( int flags )
    {
        QueueFamilyIndices indices;

        if ( flags & VK_QUEUE_COMPUTE_BIT )
        {
            for ( uint32_t i = 0; i < (uint32_t)m_QueueFamilyProperties.size(); i++ )
            {
                auto& queueFamilyProperties = m_QueueFamilyProperties[i];
                if ( ( queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT ) &&
                     ( ( queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT ) == 0 ) )
                {
                    indices.ComputeFamily = i;
                    break;
                }
            }
        }

        if ( ( flags & VK_QUEUE_TRANSFER_BIT ) )
        {
            for ( uint32_t i = 0; i < (uint32_t)m_QueueFamilyProperties.size(); i++ )
            {
                auto& queueFamilyProperties = m_QueueFamilyProperties[i];
                if ( ( queueFamilyProperties.queueFlags & VK_QUEUE_TRANSFER_BIT ) &&
                     ( ( queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT ) == 0 ) &&
                     ( ( queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT ) == 0 ) )
                {
                    indices.TransferFamily = i;
                    break;
                }
            }
        }

        for ( uint32_t i = 0; i < (uint32_t)m_QueueFamilyProperties.size(); i++ )
        {
            if ( ( flags & VK_QUEUE_COMPUTE_BIT ) && !indices.ComputeFamily )
            {
                if ( m_QueueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT )
                    indices.ComputeFamily = i;
            }

            if ( ( flags & VK_QUEUE_TRANSFER_BIT ) && !indices.TransferFamily )
            {
                if ( m_QueueFamilyProperties[i].queueFlags & VK_QUEUE_TRANSFER_BIT )
                    indices.TransferFamily = i;
            }

            if ( ( flags & VK_QUEUE_GRAPHICS_BIT ) && !indices.GraphicsFamily )
            {
                if ( m_QueueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT )
                    indices.GraphicsFamily = i;
            }
        }

        // Implementations without dedicated compute/transfer families (e.g. MoltenVK
        // on Apple Silicon) still fully support those operations on the graphics
        // family — fall back to it instead of leaving the index empty.
        if ( ( flags & VK_QUEUE_COMPUTE_BIT ) && !indices.ComputeFamily )
            indices.ComputeFamily = indices.GraphicsFamily;
        if ( ( flags & VK_QUEUE_TRANSFER_BIT ) && !indices.TransferFamily )
            indices.TransferFamily = indices.GraphicsFamily;

        return indices;
    }

    VkFormat VulkanPhysicalDevice::FindDepthFormat() const
    {
        std::array<VkFormat, 5> depthFormats = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT,
                                                 VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT,
                                                 VK_FORMAT_D16_UNORM };

        for ( auto& format : depthFormats )
        {
            VkFormatProperties formatProps;
            vkGetPhysicalDeviceFormatProperties( m_PhysicalDevice, format, &formatProps );
            if ( formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT )
                return format;
        }
        return VK_FORMAT_UNDEFINED;
    }

} // namespace Desert::Graphic::API::Vulkan
