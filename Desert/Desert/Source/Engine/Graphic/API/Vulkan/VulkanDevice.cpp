#include <Common/Utilities/FileSystem.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Graphic/PipelineCacheFile.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <vk-bootstrap/VkBootstrap.h>

#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp> // GetImageVulkanFormat — engine format -> VkFormat
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Engine/Core/EngineContext.hpp>

#include <Common/Core/Constants.hpp>

#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp> // ReadShaderPhaseTimes — pipelines built so far

#include <algorithm> // std::max — largest device-local heap
#include <array>
#include <chrono>
#include <format>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // The blob is only valid for the GPU and driver that wrote it (the driver discards anything else),
        // so they name the file: two GPUs in one machine, or a driver update, get files of their own.
        PipelineCacheFile::DeviceIdentity PipelineIdentity( VkPhysicalDevice gpu )
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties( gpu, &props );
            PipelineCacheFile::DeviceIdentity identity{ props.vendorID, props.deviceID, props.driverVersion, {} };
            static_assert( VK_UUID_SIZE == std::tuple_size_v<decltype( identity.CacheUuid )> );
            std::copy( std::begin( props.pipelineCacheUUID ), std::end( props.pipelineCacheUUID ),
                       identity.CacheUuid.begin() );
            return identity;
        }

        // A kill -9, a crash or a lost device never reaches Destroy, and the pipelines built in that run
        // were then rebuilt from scratch on the next start. So the cache is written while the app runs,
        // as soon as the driver has built new pipelines, and at most this often while it keeps building.
    } // namespace

    VulkanPhysicalDevice::VulkanPhysicalDevice( ProbedDevice probed )
         : m_Bootstrap( std::move( probed.Physical ) ), m_DeviceCaps( std::move( probed.Caps ) )
    {
        m_PhysicalDevice                                   = m_Bootstrap->physical_device;
        const VkPhysicalDeviceProperties& deviceProperties = m_Bootstrap->properties;

        m_Capabilities.MaxStorageBufferSize         = deviceProperties.limits.maxStorageBufferRange;
        m_Capabilities.StorageBufferAlignment       = deviceProperties.limits.minStorageBufferOffsetAlignment;
        m_Capabilities.SupportsWideLines            = m_DeviceCaps.Has( Capability::WideLines );
        m_Capabilities.MaxLineWidth                 = deviceProperties.limits.lineWidthRange[1];
        m_Capabilities.SupportsAnisotropy           = m_DeviceCaps.Has( Capability::SamplerAnisotropy );
        m_Capabilities.MaxAnisotropy                = deviceProperties.limits.maxSamplerAnisotropy;
        m_Capabilities.SupportsNonSolidFill         = m_DeviceCaps.Has( Capability::FillModeNonSolid );
        m_Capabilities.SupportsTextureCompressionBC = m_DeviceCaps.Has( Capability::TextureCompressionBC );

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
            m_Capabilities.SupportsTessellation      = m_DeviceCaps.Has( Capability::TessellationShader );
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
                vkGetPhysicalDeviceFormatProperties( m_PhysicalDevice, VK_FORMAT_R32G32B32A32_SFLOAT, &fmt );
                constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
                m_Capabilities.SupportsFloatRenderTargets =
                     ( fmt.optimalTilingFeatures & kNeeded ) == kNeeded;
            }

            // Device-local heap size — the budget the screen-space passes are weighed against.
            {
                const VkPhysicalDeviceMemoryProperties& memProps = m_Bootstrap->memory_properties;
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
            LOG_INFO( "[Vulkan] Caps: textureCompressionBC (BC1-BC7) {}",
                      m_Capabilities.SupportsTextureCompressionBC ? "supported -> enabled on the device"
                                                                  : "NOT supported -- no BC textures" );

            // Publish anisotropy support to the low-level sampler-creation path (0 = unsupported -> no aniso).
            Graphic::RenderConfig::MaxAnisotropy =
                 m_Capabilities.SupportsAnisotropy ? m_Capabilities.MaxAnisotropy : 0.0f;
            Graphic::RenderConfig::WideLines = m_Capabilities.SupportsWideLines; // clamp debug-line width if false

            // Publish the device's MSAA ceiling. Derived from the capability computed above rather than
            // re-querying the driver — one source of truth, so the value the renderer clamps to and the value
            // GetCapabilities() reports can never disagree.
            Graphic::RenderConfig::MaxMSAASamples = static_cast<int>( m_Capabilities.MaxMSAASamples() );

            m_QueueFamilyProperties = m_Bootstrap->get_queue_families();
            m_DepthFormat           = FindDepthFormat();
    }

    Common::ResultStr<std::shared_ptr<VulkanPhysicalDevice>> VulkanPhysicalDevice::Create( ProbedDevice probed )
    {
        auto device = std::make_shared<VulkanPhysicalDevice>( std::move( probed ) );

        int requestedQueueTypes = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        device->m_QueueFamilyIndices = device->GetQueueFamilyIndices( requestedQueueTypes );

        // REFUSE HERE, ONCE, BY NAME — this is what makes every reader downstream able to take a plain
        // index. The three lines below used to read `.value_or( -1 )`, which handed Vulkan a queue family
        // index of 0xFFFFFFFF and let device creation carry on; the five readers further out unwrapped the
        // same optionals with `.value()` and `*`, so a driver reporting no graphics family produced either
        // an uncaught std::bad_optional_access inside a constructor or undefined behaviour, depending on
        // which of them ran first. GetQueueFamilyIndices() already falls the compute and transfer families
        // back to the graphics one, so all three are present exactly when the graphics family is.
        if ( !device->m_QueueFamilyIndices.GraphicsFamily || !device->m_QueueFamilyIndices.ComputeFamily ||
             !device->m_QueueFamilyIndices.TransferFamily )
        {
            return Common::MakeFormattedError<std::shared_ptr<VulkanPhysicalDevice>>(
                 "the selected physical device reports no usable queue families for the work this engine "
                 "submits — graphics: {}, compute: {}, transfer: {} (of {} families the driver listed)",
                 device->m_QueueFamilyIndices.GraphicsFamily
                      ? std::to_string( *device->m_QueueFamilyIndices.GraphicsFamily )
                      : "none",
                 device->m_QueueFamilyIndices.ComputeFamily
                      ? std::to_string( *device->m_QueueFamilyIndices.ComputeFamily )
                      : "none",
                 device->m_QueueFamilyIndices.TransferFamily
                      ? std::to_string( *device->m_QueueFamilyIndices.TransferFamily )
                      : "none",
                 device->m_QueueFamilyProperties.size() );
        }

        device->m_ResolvedQueueFamilies.Graphics = *device->m_QueueFamilyIndices.GraphicsFamily;
        device->m_ResolvedQueueFamilies.Compute  = *device->m_QueueFamilyIndices.ComputeFamily;
        device->m_ResolvedQueueFamilies.Transfer = *device->m_QueueFamilyIndices.TransferFamily;
        device->m_QueueFamiliesResolved          = true;

        return Common::MakeSuccess( std::move( device ) );
    }

    Common::ResultStr<std::shared_ptr<VulkanLogicalDevice>> VulkanLogicalDevice::Create()
    {
        auto probed = SelectDevice( VulkanContext::GetBootstrapInstance() );
        if ( !probed )
            return Common::MakeError<std::shared_ptr<VulkanLogicalDevice>>( probed.GetError() );
        // THE START-UP TABLE, ONE LINE: which device, which driver, which version, and every row of
        // DeviceCaps as core / ext / no ("!" marks the required set).
        LOG_INFO( "[Vulkan] {}", FormatCapsTable( probed.GetValue().Caps ) );

        auto physical = VulkanPhysicalDevice::Create( probed.ExtractValue() );
        if ( !physical )
            return Common::MakeError<std::shared_ptr<VulkanLogicalDevice>>( physical.GetError() );

        auto device = std::make_shared<VulkanLogicalDevice>( physical.ExtractValue() );
        if ( const auto created = device->CreateDevice(); !created )
            return Common::MakeError<std::shared_ptr<VulkanLogicalDevice>>( created.GetError() );
        return Common::MakeSuccess( std::move( device ) );
    }

    VulkanLogicalDevice::VulkanLogicalDevice( std::shared_ptr<VulkanPhysicalDevice> physicalDevice )
         : m_PhysicalDevice( std::move( physicalDevice ) ),
           m_DeviceName( m_PhysicalDevice->GetDeviceCaps().DeviceName )
    {
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

        const std::scoped_lock queues( m_QueueMutex );
        const VkResult         idle = vkDeviceWaitIdle( m_LogicalDevice );
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

    VkResult VulkanLogicalDevice::SubmitToQueue( VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits,
                                                 VkFence fence ) const
    {
        const std::scoped_lock queues( m_QueueMutex );
        return vkQueueSubmit( queue, submitCount, submits, fence );
    }

    VkResult VulkanLogicalDevice::PresentToQueue( VkQueue queue, const VkPresentInfoKHR& present ) const
    {
        const std::scoped_lock queues( m_QueueMutex );
        return vkQueuePresentKHR( queue, &present );
    }

    std::unique_lock<std::mutex> VulkanLogicalDevice::LockQueues() const
    {
        return std::unique_lock<std::mutex>( m_QueueMutex );
    }

    void VulkanLogicalDevice::Destroy()
    {
        if ( m_LogicalDevice != VK_NULL_HANDLE )
        {
            // Same reason as WaitIdle above; vkDestroyPipelineCache and vkDestroyDevice below stay legal on
            // a lost device, which is what makes an orderly close possible at all.
            if ( Graphic::DeviceLost::AllowWork() )
            {
                const std::scoped_lock queues( m_QueueMutex );
                vkDeviceWaitIdle( m_LogicalDevice );
            }

            // THE LAST MOMENT AT WHICH THIS DEVICE IS STILL ALIVE, and therefore the only place from which
            // every one of its children can be released. The order below this line was already correct and
            // the leak was never in it: `vkDestroyDevice(): VkDevice has 6599 leaked objects` on a normal
            // close, of which 6 370 were sitting in the renderer context's deferred-deletion queue (drained
            // only from VulkanQueue::Present, and shutdown releases the engine's whole content AFTER the
            // last frame) and 220 were the command pools and their one-off buffers, which nothing had ever
            // been written to destroy.
            //
            // It is called from HERE rather than from ~VulkanContext because Application's members die
            // window -> device -> context: by the time the context is destroyed this device is already
            // gone, so a release there would free device children through a destroyed device. Every owner
            // that queues work — the swapchain included — has already run by the time we get here.
            if ( const auto context = EngineContext::GetInstance().GetRendererContext() )
                context->Shutdown();

            if ( m_PipelineCache != VK_NULL_HANDLE )
            {
                // The pipelines built since the last in-run write (PersistPipelineCache is throttled).
                if ( const auto written = WritePipelineCache(); !written )
                    LOG_WARN( "[PipelineCache] not written at exit: {}", written.GetError() );
                vkDestroyPipelineCache( m_LogicalDevice, m_PipelineCache, nullptr );
                m_PipelineCache = VK_NULL_HANDLE;
            }
            vkDestroyDevice( m_LogicalDevice, nullptr );
            m_LogicalDevice = VK_NULL_HANDLE;
        }
    }

    Common::ResultStr<bool> VulkanLogicalDevice::CreateDevice()
    {
        // THE ENABLED SET IS THE PROBED SET. Every feature and extension this device gets was enabled on
        // m_Bootstrap by DeviceCapsProbe, on the route DeviceCaps planned and only where the driver said
        // yes — required rows, optional ones (wideLines, anisotropy, textureCompressionBC, memory budget,
        // portability_subset where MoltenVK advertises it, ray tracing) and the 1.2/1.3 ones VKF2 will
        // build on. Nothing is added here, so DeviceCaps::Has() is the truth about this VkDevice.
        //
        // textureCompressionBC in particular: the spec allows BC images only when the feature is ENABLED,
        // and on MoltenVK its absence is undetectable (measured 2026-09-23: a BC7 image creates, uploads
        // and samples bit-exact with the feature off, validation 1.4.350.1 silent) — so the enable is
        // the table row, not something a run on this machine could catch.
        m_MemoryBudgetEnabled = m_PhysicalDevice->GetDeviceCaps().Has( Capability::MemoryBudget );
        if ( !m_MemoryBudgetEnabled )
        {
            LOG_WARN( "[Device] VK_EXT_memory_budget is absent; device-memory usage will report as "
                      "unknown rather than as zero." );
        }

        // Same queues as ever: graphics, plus compute and transfer when they live in other families.
        const uint32_t                           graphics = m_PhysicalDevice->GetGraphicsFamily();
        const uint32_t                           compute  = m_PhysicalDevice->GetComputeFamily();
        const uint32_t                           transfer = m_PhysicalDevice->GetTransferFamily();
        std::vector<vkb::CustomQueueDescription> queues;
        queues.emplace_back( graphics, std::vector<float>{ 1.0f } );
        if ( compute != graphics )
            queues.emplace_back( compute, std::vector<float>{ 1.0f } );
        if ( transfer != graphics && transfer != compute )
            queues.emplace_back( transfer, std::vector<float>{ 1.0f } );

        vkb::DeviceBuilder builder( m_PhysicalDevice->GetBootstrapDevice() );
        builder.custom_queue_setup( queues );
        auto built = builder.build();
        if ( !built )
            return Common::MakeFormattedError<bool>(
                 "vk-bootstrap could not create the logical device on '{}': {} ({})", m_DeviceName,
                 built.error().message(), static_cast<int>( built.vk_result() ) );
        m_LogicalDevice = built.value().device;

        vkGetDeviceQueue( m_LogicalDevice, m_PhysicalDevice->GetGraphicsFamily(), 0, &m_GraphicsQueue );
        vkGetDeviceQueue( m_LogicalDevice, m_PhysicalDevice->GetComputeFamily(), 0, &m_ComputeQueue );
        vkGetDeviceQueue( m_LogicalDevice, m_PhysicalDevice->GetTransferFamily(), 0, &m_TransferQueue );

        CreatePipelineCache();

        return Common::MakeSuccess( true );
    }

    void VulkanLogicalDevice::CreatePipelineCache()
    {
        // Seed the cache from this user's blob for this device. It lives in the USER's directory, never in
        // the install (a protected install cannot be written, and before PSO1 a package wrote its blob into
        // its own folder). Our header refuses another device's or a torn file with a reason; the driver's
        // own header check stays behind it.
        m_PipelineIdentity = PipelineIdentity( m_PhysicalDevice->GetVulkanPhysicalDevice() );
        std::string                                  initial;
        const std::optional<PipelineCacheFile::Host> host = PipelineCacheFile::DeclaredHost();
        if ( !Project::ProjectContext::HasProject() )
        {
            LOG_ERROR( "[PipelineCache] no project is open, so there is no per-user cache path; pipelines "
                       "build uncached and nothing is persisted this run" );
        }
        else if ( !host )
        {
            LOG_ERROR( "[PipelineCache] the host never declared itself editor or game "
                       "(PipelineCacheFile::DeclareHost), so there is no per-user cache path; pipelines build "
                       "uncached and nothing is persisted this run" );
        }
        else
        {
            const std::string&          name = Project::ProjectContext::Current().Name;
            const std::filesystem::path userDir =
                 *host == PipelineCacheFile::Host::Game
                      ? Common::Settings::GameUserDirectory( name )
                      : std::filesystem::path( Project::ProjectContext::ConfigDirectory() );
            const std::filesystem::path dir = PipelineCacheFile::Directory( *host, userDir, name );
            std::error_code             ec;
            std::filesystem::create_directories( dir, ec );
            if ( ec )
            {
                LOG_ERROR( "[PipelineCache] cannot create '{}': {}; nothing is persisted this run", dir.string(),
                           ec.message() );
            }
            else
            {
                m_PipelineCacheFile = dir / PipelineCacheFile::FileName( m_PipelineIdentity );
            }
        }
        if ( !m_PipelineCacheFile.empty() )
        {
            std::error_code ec;
            if ( !std::filesystem::is_regular_file( m_PipelineCacheFile, ec ) )
            {
                LOG_INFO( "[PipelineCache] '{}': no file, created empty", m_PipelineCacheFile.string() );
            }
            else if ( auto read = Common::Utils::FileSystem::ReadFileContent( m_PipelineCacheFile ); !read )
            {
                LOG_ERROR( "[PipelineCache] '{}' unreadable ({}); created empty", m_PipelineCacheFile.string(),
                           read.GetError() );
            }
            else if ( auto decoded = PipelineCacheFile::Decode( m_PipelineIdentity, read.GetValue() );
                      !decoded.Blob )
            {
                LOG_WARN( "[PipelineCache] '{}' refused: {}; created empty", m_PipelineCacheFile.string(),
                          decoded.Refusal );
            }
            else
            {
                initial = std::move( *decoded.Blob );
                LOG_INFO( "[PipelineCache] '{}': seeded from {} bytes", m_PipelineCacheFile.string(),
                          initial.size() );
            }
        }
        m_PersistedPipelineHash = initial.empty() ? 0 : PipelineCacheFile::HashBytes( initial );

        const VkPipelineCacheCreateInfo info{ .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                              .pNext           = nullptr,
                                              .flags           = 0,
                                              .initialDataSize = initial.size(),
                                              .pInitialData    = initial.empty() ? nullptr : initial.data() };
        if ( const VkResult r = vkCreatePipelineCache( m_LogicalDevice, &info, nullptr, &m_PipelineCache );
             r != VK_SUCCESS )
        {
            // Non-fatal: pipelines are still built, each one from scratch, and nothing is persisted this run.
            LOG_WARN( "[PipelineCache] vkCreatePipelineCache ({} seed bytes) = {}; pipelines build uncached",
                      initial.size(), static_cast<int>( r ) );
            m_PipelineCache = VK_NULL_HANDLE;
        }
    }

    Common::BoolResultStr VulkanLogicalDevice::PersistPipelineCache()
    {
        const uint64_t built =
             Core::ReadShaderPhaseTimes().Calls[static_cast<size_t>( Core::ShaderPhase::PipelineCreate )];
        if ( !m_PersistSchedule.Due( built, std::chrono::steady_clock::now() ) )
            return Common::MakeSuccess( true );
        return WritePipelineCache();
    }

    Common::BoolResultStr VulkanLogicalDevice::WritePipelineCache()
    {
        if ( m_PipelineCache == VK_NULL_HANDLE || m_PipelineCacheFile.empty() )
            return Common::MakeSuccess( true );

        // A lost device has nothing to hand back, and asking it is one more call after the point where the
        // engine promised to stop. Losing one run's accumulated pipeline binaries costs a slower next
        // start; it costs nothing that matters.
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeSuccess( true );

        size_t size = 0;
        if ( const VkResult r = vkGetPipelineCacheData( m_LogicalDevice, m_PipelineCache, &size, nullptr );
             r != VK_SUCCESS )
            return Common::MakeError<bool>(
                 std::format( "pipeline cache: vkGetPipelineCacheData (size) = {}", static_cast<int>( r ) ) );
        std::string data( size, '\0' );
        // VK_INCOMPLETE: another thread built a pipeline between the two calls, and the driver returned what
        // fit (the spec's partial read, header first). Written as is; the driver re-validates it on load, and
        // the count that moved schedules the next write.
        if ( const VkResult r = vkGetPipelineCacheData( m_LogicalDevice, m_PipelineCache, &size, data.data() );
             r != VK_SUCCESS && r != VK_INCOMPLETE )
            return Common::MakeError<bool>( std::format( "pipeline cache: vkGetPipelineCacheData ({} bytes) = {}",
                                                         size, static_cast<int>( r ) ) );
        data.resize( size );

        // The same bytes as the entry already on disk: nothing was learned, and nothing is rewritten.
        const uint64_t hash = PipelineCacheFile::HashBytes( data );
        if ( hash == m_PersistedPipelineHash )
            return Common::MakeSuccess( true );
        // Written to a temporary and renamed over the file: a kill in the middle leaves the previous blob or
        // the new one, never a torn file (and a torn one would be refused by the header anyway).
        auto stored = Common::Utils::FileSystem::WriteContentToFileAtomic(
             m_PipelineCacheFile, PipelineCacheFile::Encode( m_PipelineIdentity, data ) );
        if ( !stored )
            return Common::MakeError<bool>(
                 std::format( "pipeline cache: '{}': {}", m_PipelineCacheFile.string(), stored.GetError() ) );
        m_PersistedPipelineHash = hash;
        return stored;
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
