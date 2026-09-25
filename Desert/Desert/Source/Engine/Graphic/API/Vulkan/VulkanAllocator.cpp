#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        static VmaAllocator s_VmaAllocator = VK_NULL_HANDLE;
    }

    void VulkanAllocator::Init( const std::shared_ptr<VulkanLogicalDevice>& device, VkInstance instance )
    {
        if ( s_VmaAllocator != VK_NULL_HANDLE )
        {
            vmaDestroyAllocator( s_VmaAllocator );
            s_VmaAllocator = VK_NULL_HANDLE;
        }

        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.vulkanApiVersion       = VK_API_VERSION_1_3;
        allocatorInfo.physicalDevice         = device->GetPhysicalDevice()->GetVulkanPhysicalDevice();
        allocatorInfo.device                 = device->GetVulkanLogicalDevice();
        allocatorInfo.instance               = instance;

        VK_CHECK_RESULT( vmaCreateAllocator( &allocatorInfo, &s_VmaAllocator ) );

        // HELD, NOT LOOKED UP. Every destroy below used to reach the VkDevice through
        // EngineContext::GetDevice(), which is a weak_ptr::lock() — and the one moment this class most
        // needs a device is INSIDE ~VulkanLogicalDevice, where that lock() answers null because the
        // shared_ptr is already expiring. The teardown drain therefore destroyed nothing and said nothing,
        // which is the silent-empty-success shape the contract forbids. The allocator was created for
        // exactly one device; holding its handle is the single source of truth for which one.
        m_Device = device->GetVulkanLogicalDevice();
    }

    void VulkanAllocator::Shutdown()
    {
        if ( s_VmaAllocator != VK_NULL_HANDLE )
        {
            vmaDestroyAllocator( s_VmaAllocator );
            s_VmaAllocator = VK_NULL_HANDLE;
        }
    }

    VmaAllocator& VulkanAllocator::GetVMAAllocator()
    {
        return s_VmaAllocator;
    }

    Common::ResultStr<VmaAllocation> VulkanAllocator::RT_AllocateBuffer( const std::string& tag, const VkBufferCreateInfo& bufferCreateInfo,
                                                                          VmaMemoryUsage usage, VkBuffer& outBuffer )
    {
        if ( s_VmaAllocator == VK_NULL_HANDLE ) return Common::MakeError<VmaAllocation>( "VmaAllocator is null" );

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage                   = usage;

        VkBufferCreateInfo localInfo = bufferCreateInfo;
        if ( localInfo.size == 0 ) localInfo.size = 1; // Vulkan requires size > 0

        // Initialised, because a caller that ignores the result below must get a NULL handle rather
        // than a stack value: null is refused by the validation layers at the point of use and names
        // the buffer, whereas garbage is undefined behaviour with no diagnostic anywhere.
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkResult res = vmaCreateBuffer( s_VmaAllocator, &localInfo, &allocInfo, &outBuffer, &allocation, nullptr );
        if ( res != VK_SUCCESS )
        {
            // ANSWER THE FAILURE THROUGH THE RESULT — the channel is already in the signature. This
            // used to LOG_ERROR, DESERT_VERIFY( false ) and then `return MakeSuccess( allocation )`,
            // which is worse than either half on its own: the verify aborted the process in EVERY
            // configuration (it has no NDEBUG guard), so a player hit by VK_ERROR_OUT_OF_DEVICE_MEMORY
            // - an external, recoverable condition - crashed outright; and on the paths that somehow
            // got past it the function reported SUCCESS carrying an uninitialised handle, which made
            // every `if ( !result )` above it dead code. ReadFileContent lost its own DESERT_VERIFY
            // for exactly this reason: a primitive cannot know whether its caller can survive the
            // failure, so it names the failure and lets the caller decide.
            LOG_ERROR( "[VmaAllocator] vmaCreateBuffer failed for tag '{}' (requested size: {}) with VkResult: {} ({})", tag, bufferCreateInfo.size, (int)res, VkResultToString(res) );
            return Common::MakeFormattedError<VmaAllocation>(
                 "vmaCreateBuffer failed for tag '{}' (requested size: {}) with VkResult {} ({})", tag,
                 bufferCreateInfo.size, (int)res, VkResultToString( res ) );
        }

        return Common::MakeSuccess( allocation );
    }

    Common::ResultStr<VmaAllocation> VulkanAllocator::RT_AllocateImage( const std::string& tag, const VkImageCreateInfo& imageCreateInfo,
                                                                         VmaMemoryUsage usage, VkImage& outImage )
    {
        if ( s_VmaAllocator == VK_NULL_HANDLE ) return Common::MakeError<VmaAllocation>( "VmaAllocator is null" );

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage                   = usage;

        VmaAllocation allocation = VK_NULL_HANDLE; // see RT_AllocateBuffer above
        VkResult res = vmaCreateImage( s_VmaAllocator, &imageCreateInfo, &allocInfo, &outImage, &allocation, nullptr );
        if ( res != VK_SUCCESS )
        {
            // Same reasoning as RT_AllocateBuffer above, and the same defect it fixes.
            LOG_ERROR( "[VmaAllocator] vmaCreateImage failed for tag '{}' with VkResult: {} ({})", tag, (int)res, VkResultToString(res) );
            return Common::MakeFormattedError<VmaAllocation>(
                 "vmaCreateImage failed for tag '{}' with VkResult {} ({})", tag, (int)res,
                 VkResultToString( res ) );
        }

        return Common::MakeSuccess( allocation );
    }

    std::size_t VulkanAllocator::AllocationSize( VmaAllocation allocation )
    {
        if ( s_VmaAllocator == VK_NULL_HANDLE || allocation == VK_NULL_HANDLE )
            return 0;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo( s_VmaAllocator, allocation, &info );
        return static_cast<std::size_t>( info.size );
    }

    void VulkanAllocator::RT_DestroyBuffer( VkBuffer buffer, VmaAllocation allocation )
    {
        if ( !buffer || !allocation ) return;
        uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        m_BufferDeletionQueue.push_back( { buffer, allocation, frameIndex } );
    }

    void VulkanAllocator::RT_DestroyImage( VkImage image, VmaAllocation allocation, VkImageView imageView,
                                           VkSampler sampler, const std::vector<VkImageView>& mipImageViews )
    {
        if ( !image || !allocation ) return;
        uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        m_ImageDeletionQueue.push_back( { image, allocation, imageView, sampler, mipImageViews, frameIndex } );
    }

    void VulkanAllocator::RT_DestroyFramebuffer( VkFramebuffer framebuffer )
    {
        if ( !framebuffer ) return;
        uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        m_FramebufferDeletionQueue.push_back( { framebuffer, frameIndex } );
    }

    void VulkanAllocator::RT_DestroyRenderPass( VkRenderPass renderPass )
    {
        if ( !renderPass ) return;
        uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        m_RenderPassDeletionQueue.push_back( { renderPass, frameIndex } );
    }

    void VulkanAllocator::RT_DestroyQueryPool( VkQueryPool queryPool )
    {
        if ( queryPool == VK_NULL_HANDLE )
            return;
        const uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        m_QueryPoolDeletionQueue.push_back( { queryPool, frameIndex } );
    }

    void VulkanAllocator::RT_DestroyDescriptorPool( VkDescriptorPool descriptorPool )
    {
        if ( descriptorPool == VK_NULL_HANDLE )
            return;
        const uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        m_DescriptorPoolDeletionQueue.push_back( { descriptorPool, frameIndex, {} } );
    }

    void VulkanAllocator::RT_FreeDescriptorSets( VkDescriptorPool pool, std::vector<VkDescriptorSet> sets )
    {
        if ( pool == VK_NULL_HANDLE || sets.empty() )
            return;
        const uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        m_DescriptorPoolDeletionQueue.push_back( { pool, frameIndex, std::move( sets ) } );
    }

    MappedMemory VulkanAllocator::MapMemory( VmaAllocation allocation )
    {
        if ( s_VmaAllocator == VK_NULL_HANDLE )
            return MappedMemory::Refused( "the VMA allocator is not initialised" );
        if ( allocation == VK_NULL_HANDLE )
            return MappedMemory::Refused( "there is no allocation to map (a null VmaAllocation)" );

        uint8_t*       mappedMemory = nullptr;
        const VkResult mapped       = vmaMapMemory( s_VmaAllocator, allocation, (void**)&mappedMemory );
        if ( mapped != VK_SUCCESS )
        {
            (void)NoteIfDeviceLost( mapped, "vmaMapMemory", __FILE__, __LINE__ );
            LOG_ERROR( "[Allocator] vmaMapMemory failed: {}; every write through this mapping is refused.",
                       VkResultToString( mapped ) );
            return MappedMemory::Refused( fmt::format( "vmaMapMemory failed: {}", VkResultToString( mapped ) ) );
        }

        // VK_SUCCESS WITH NO ADDRESS IS STILL A FAILURE, and it has to be UNMAPPED rather than merely
        // refused: VMA counts the map, so returning here without unmapping would leak a mapping that no
        // MappedMemory owns and nothing would ever release. MappedMemory::Live refuses this case on its
        // own account too — this branch exists for the unmap, not for the refusal.
        if ( mappedMemory == nullptr )
        {
            UnmapAllocation( allocation );
            LOG_ERROR( "[Allocator] vmaMapMemory reported success and handed back no address." );
            return MappedMemory::Refused( "vmaMapMemory reported success and handed back no address" );
        }

        // THE SIZE COMES FROM THE ALLOCATION, not from whatever the caller believes it asked for. That
        // is what lets MappedMemory refuse an overrun: every one of the old memcpy sites sized its copy
        // from a width, a height and a format computed several files away from the allocation it was
        // writing into, and nothing anywhere compared the two.
        VmaAllocationInfo info{};
        vmaGetAllocationInfo( s_VmaAllocator, allocation, &info );

        return MappedMemory::Live( allocation, mappedMemory, static_cast<std::size_t>( info.size ),
                                   &VulkanAllocator::UnmapAllocation );
    }

    void VulkanAllocator::UnmapAllocation( void* allocation )
    {
        if ( s_VmaAllocator != VK_NULL_HANDLE )
            vmaUnmapMemory( s_VmaAllocator, static_cast<VmaAllocation>( allocation ) );
    }

    void VulkanAllocator::ProcessDeletionQueue()
    {
        // The per-frame drain: an entry queued on frame f is destroyed the next time the ring comes back
        // round to f, which is the point at which the GPU has demonstrably finished with it.
        const uint32_t frameIndex = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
        (void)DestroyQueued( [frameIndex]( uint32_t queued ) { return queued == frameIndex; } );
    }

    std::size_t VulkanAllocator::DrainDeletionQueue()
    {
        // WHY AN UNCONDITIONAL DRAIN EXISTS AT ALL, in the numbers that produced it. ProcessDeletionQueue
        // above is reached from exactly one place — VulkanQueue::Present — so it runs only while frames
        // run. Shutdown releases the whole content of the engine AFTER the last frame: Renderer::Shutdown
        // clears every resource service, the scene renderers drop their passes and the swapchain gives its
        // capture buffer back, and every one of those calls RT_Destroy*, which QUEUES. Nothing ever came
        // back to drain it, so all of it was still queued when vkDestroyDevice ran: 6 370 of the 6 599
        // objects the validation layer reported leaking on a normal close, VkBuffer alone being 5 716.
        //
        // The frame condition is dropped deliberately rather than approximated with "any frame": there is
        // no next frame to wait for, and the caller has already idled the device, so every entry here is
        // finished with by construction.
        return DestroyQueued( []( uint32_t ) { return true; } );
    }

    std::size_t VulkanAllocator::DestroyQueued( const std::function<bool( uint32_t )>& takeFrame )
    {
        if ( s_VmaAllocator == VK_NULL_HANDLE )
            return 0;

        VkDevice device = m_Device;
        if ( device == VK_NULL_HANDLE )
            return 0;

        std::size_t destroyed = 0;

        for ( auto it = m_BufferDeletionQueue.begin(); it != m_BufferDeletionQueue.end(); )
        {
            if ( takeFrame( it->FrameIndex ) )
            {
                vmaDestroyBuffer( s_VmaAllocator, it->Buffer, it->Allocation );
                it = m_BufferDeletionQueue.erase( it );
                ++destroyed;
            }
            else ++it;
        }

        for ( auto it = m_ImageDeletionQueue.begin(); it != m_ImageDeletionQueue.end(); )
        {
            if ( takeFrame( it->FrameIndex ) )
            {
                if ( it->ImageView != VK_NULL_HANDLE ) vkDestroyImageView( device, it->ImageView, nullptr );
                if ( it->Sampler != VK_NULL_HANDLE )   vkDestroySampler( device, it->Sampler, nullptr );
                for ( auto view : it->MipImageViews )  vkDestroyImageView( device, view, nullptr );

                vmaDestroyImage( s_VmaAllocator, it->Image, it->Allocation );
                it = m_ImageDeletionQueue.erase( it );
                ++destroyed;
            }
            else ++it;
        }

        for ( auto it = m_FramebufferDeletionQueue.begin(); it != m_FramebufferDeletionQueue.end(); )
        {
            if ( takeFrame( it->FrameIndex ) )
            {
                vkDestroyFramebuffer( device, it->Framebuffer, nullptr );
                it = m_FramebufferDeletionQueue.erase( it );
                ++destroyed;
            }
            else ++it;
        }

        for ( auto it = m_RenderPassDeletionQueue.begin(); it != m_RenderPassDeletionQueue.end(); )
        {
            if ( takeFrame( it->FrameIndex ) )
            {
                vkDestroyRenderPass( device, it->RenderPass, nullptr );
                it = m_RenderPassDeletionQueue.erase( it );
                ++destroyed;
            }
            else ++it;
        }

        for ( auto it = m_QueryPoolDeletionQueue.begin(); it != m_QueryPoolDeletionQueue.end(); )
        {
            if ( takeFrame( it->FrameIndex ) )
            {
                vkDestroyQueryPool( device, it->QueryPool, nullptr );
                it = m_QueryPoolDeletionQueue.erase( it );
                ++destroyed;
            }
            else
                ++it;
        }

        for ( auto it = m_DescriptorPoolDeletionQueue.begin(); it != m_DescriptorPoolDeletionQueue.end(); )
        {
            if ( !takeFrame( it->FrameIndex ) )
            {
                ++it;
                continue;
            }
            if ( !it->Sets.empty() )
            {
                vkFreeDescriptorSets( device, it->DescriptorPool, static_cast<uint32_t>( it->Sets.size() ),
                                      it->Sets.data() );
                it = m_DescriptorPoolDeletionQueue.erase( it );
                ++destroyed;
                continue;
            }
            // Destroying the pool frees every set still in it. A free of this pool's sets queued on a
            // frame the ring has not come round to yet would then name dead handles: drop it with the pool.
            auto* const pool = it->DescriptorPool;
            vkDestroyDescriptorPool( device, pool, nullptr );
            auto next = static_cast<std::size_t>( m_DescriptorPoolDeletionQueue.erase( it ) -
                                                  m_DescriptorPoolDeletionQueue.begin() );
            ++destroyed;
            for ( std::size_t pending = 0; pending < m_DescriptorPoolDeletionQueue.size(); )
            {
                const auto& entry = m_DescriptorPoolDeletionQueue[pending];
                if ( entry.DescriptorPool == pool && !entry.Sets.empty() )
                {
                    m_DescriptorPoolDeletionQueue.erase( m_DescriptorPoolDeletionQueue.begin() +
                                                         static_cast<std::ptrdiff_t>( pending ) );
                    if ( pending < next )
                        --next;
                }
                else
                    ++pending;
            }
            it = m_DescriptorPoolDeletionQueue.begin() + static_cast<std::ptrdiff_t>( next );
        }

        return destroyed;
    }

    std::size_t VulkanAllocator::QueuedCount() const
    {
        return m_BufferDeletionQueue.size() + m_ImageDeletionQueue.size() + m_FramebufferDeletionQueue.size() +
               m_RenderPassDeletionQueue.size() + m_QueryPoolDeletionQueue.size() +
               m_DescriptorPoolDeletionQueue.size();
    }

    VulkanAllocator::~VulkanAllocator()
    try
    {
        // SAYS SO RATHER THAN DESTROYING. By the time this runs the VkDevice may already be gone — the
        // context is the last of Application's three members to die — so destroying anything here is the
        // very fault this file's drain exists to prevent. What a destructor CAN do is refuse to be silent:
        // a non-empty queue at this point means the teardown path in VulkanLogicalDevice::Destroy did not
        // run, and that is the state in which the validation layer reported 6 599 leaked objects with
        // nothing in the engine's own log to explain them.
        const std::size_t stranded = QueuedCount();
        if ( stranded > 0 )
        {
            LOG_ERROR( "[Allocator] {} deferred GPU object(s) were still queued when the allocator was "
                       "destroyed; VulkanLogicalDevice::Destroy did not drain them and the device has "
                       "already taken them with it.",
                       stranded );
        }
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanAllocator" )

} // namespace Desert::Graphic::API::Vulkan
