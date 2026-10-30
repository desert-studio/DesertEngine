#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/TextureStreamingDemand.hpp>

#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Common/Core/DevInstruments.hpp>
#include <Common/Utilities/ProcessMemory.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <array>
#include <mutex>
#include <string_view>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <format>
#include <string>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        static VmaAllocator s_VmaAllocator = VK_NULL_HANDLE;

#if DESERT_DEV_INSTRUMENTS
        // THE DETAILED MAP, SUMMED BY NAME (BISTRO-MEM-2 p4). Every allocation the engine makes is named at
        // creation (the ledger tag, "RDG <resource>", "RDG heap <graph>"), so VMA's own detailed map
        // (vmaBuildStatsString, detailedMap) is the census nothing can slip past: an allocation without a name
        // is one no creation path recorded, grouped by its suballocation type and Vulkan usage flags so the
        // group says what it is. Read at the census only (a footprint peak), never per frame.
        std::string DetailedMapByName( VmaAllocator allocator, std::size_t rows )
        {
            char* json = nullptr;
            vmaBuildStatsString( allocator, &json, VK_TRUE );
            if ( json == nullptr )
                return " (vmaBuildStatsString returned nothing)";
            const std::string_view map( json );
            struct Group
            {
                std::size_t Bytes = 0;
                std::size_t Count = 0;
            };
            std::unordered_map<std::string, Group> named;
            std::unordered_map<std::string, Group> unnamed;
            const auto stringAfter = []( std::string_view object, std::string_view key ) -> std::string_view
            {
                const std::size_t at = object.find( key );
                if ( at == std::string_view::npos )
                    return {};
                const std::size_t open  = object.find( '"', at + key.size() );
                const std::size_t close = open == std::string_view::npos ? open : object.find( '"', open + 1 );
                return close == std::string_view::npos ? std::string_view{}
                                                       : object.substr( open + 1, close - open - 1 );
            };
            const auto numberAfter = []( std::string_view object, std::string_view key ) -> uint64_t
            {
                const std::size_t at = object.find( key );
                if ( at == std::string_view::npos )
                    return 0;
                std::size_t digit = object.find_first_of( "0123456789", at + key.size() );
                uint64_t    value = 0;
                for ( ; digit < object.size() && object[digit] >= '0' && object[digit] <= '9'; ++digit )
                    value = value * 10 + static_cast<uint64_t>( object[digit] - '0' );
                return value;
            };
            // Each allocation is one flat JSON object: {"Offset"?, "Type", "Size", "Usage", "Name"?}.
            for ( std::size_t at = map.find( "\"Type\"" ); at != std::string_view::npos;
                  at             = map.find( "\"Type\"", at + 1 ) )
            {
                const std::size_t      begin  = map.rfind( '{', at );
                const std::size_t      end    = map.find( '}', at );
                const std::string_view object = map.substr( begin, end - begin );
                if ( object.find( '{', 1 ) != std::string_view::npos )
                    continue; // a block or pool object, not an allocation
                const std::string_view type = stringAfter( object, "\"Type\"" );
                if ( type == "FREE" )
                    continue;
                const uint64_t         size = numberAfter( object, "\"Size\"" );
                const std::string_view name = stringAfter( object, "\"Name\"" );
                Group&                 group =
                     name.empty()
                                          ? unnamed[std::format( "{} usage {:#x}", type, numberAfter( object, "\"Usage\"" ) )]
                                          : named[name.starts_with( "RDG heap " ) ? std::string( name )
                                                  : name.starts_with( "RDG " ) ? std::string( "RDG extracted (all)" )
                                                                               : std::string( "ledger tags (all)" )];
                group.Bytes += size;
                ++group.Count;
            }
            vmaFreeStatsString( allocator, json );
            const auto print = [&]( std::string_view title, const std::unordered_map<std::string, Group>& groups )
            {
                std::vector<std::pair<std::string, Group>> sorted( groups.begin(), groups.end() );
                std::ranges::sort( sorted, []( const auto& a, const auto& b )
                                   { return a.second.Bytes > b.second.Bytes; } );
                std::size_t total = 0;
                for ( const auto& [key, group] : sorted )
                    total += group.Bytes;
                std::string out = std::format( "\n  VMA detailed map, {}: {:.1f} MB", title,
                                               static_cast<double>( total ) / ( 1024.0 * 1024.0 ) );
                for ( std::size_t i = 0; i < sorted.size() && i < rows; ++i )
                    out += std::format( "\n    {:<56} x{:<6} {:9.1f} MB", sorted[i].first, sorted[i].second.Count,
                                        static_cast<double>( sorted[i].second.Bytes ) / ( 1024.0 * 1024.0 ) );
                return out;
            };
            return print( "named (ledger / RDG extracted / RDG heaps by graph)", named ) +
                   print( "UNNAMED (no creation path recorded it)", unnamed );
        }
#endif

        // The ledger keys a row by the allocation's address: VmaAllocation is an opaque pointer and its
        // value is the identity, never dereferenced through the integer.
        // RELEASE THE ROW BEFORE vmaDestroy*, everywhere. A staging copy is destroyed by whichever thread runs
        // CommandBufferAllocator::CollectFinished (the render thread or a loader thread), while other loader
        // threads allocate. VMA may hand the freed VmaAllocation to the next vmaCreate* at once; destroyed first,
        // a concurrent Record of the reused key could land before this Release and be erased by it, leaving a
        // live allocation out of the census. The ledger itself is locked (AllocationLedger::m_Mutex).
        uint64_t LedgerKey( VmaAllocation allocation )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            return reinterpret_cast<uint64_t>( allocation );
        }
    }

    void VulkanAllocator::Init( const std::shared_ptr<VulkanLogicalDevice>& device, VkInstance instance )
    {
        if ( s_VmaAllocator != VK_NULL_HANDLE )
        {
            vmaDestroyAllocator( s_VmaAllocator );
            s_VmaAllocator = VK_NULL_HANDLE;
        }

        VmaAllocatorCreateInfo allocatorInfo = {};
        // VMA's contract: never above the DEVICE's version (it calls core entry points of the version it is
        // told). min(device, 1.3) is DeviceCaps' used version — the same one routing planned against.
        allocatorInfo.vulkanApiVersion       = device->GetPhysicalDevice()->GetDeviceCaps().UsedApiVersion;
        allocatorInfo.physicalDevice         = device->GetPhysicalDevice()->GetVulkanPhysicalDevice();
        allocatorInfo.device                 = device->GetVulkanLogicalDevice();
        allocatorInfo.instance               = instance;
        m_DeviceCaps                         = device->GetPhysicalDevice()->GetDeviceCaps();
        m_Sharing                            = device->GetPhysicalDevice()->GetResourceSharing();
        // A buffer created with SHADER_DEVICE_ADDRESS (a BLAS build input, an acceleration structure, its
        // scratch) must be bound to memory allocated with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; VMA adds it
        // only when told. Only where the device enabled bufferDeviceAddress — the flag is invalid otherwise.
        if ( m_DeviceCaps.Has( Capability::BufferDeviceAddress ) )
            allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

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
            // The device is idle here (VulkanContext::Shutdown drained the queues first): the zero buffer no
            // longer has a reader.
            if ( m_ZeroBufferAllocation != VK_NULL_HANDLE )
            {
                m_Ledger.Release( LedgerKey( m_ZeroBufferAllocation ) );
                vmaDestroyBuffer( s_VmaAllocator, m_ZeroBuffer, m_ZeroBufferAllocation );
                m_ZeroBuffer           = VK_NULL_HANDLE;
                m_ZeroBufferAllocation = VK_NULL_HANDLE;
            }
            vmaDestroyAllocator( s_VmaAllocator );
            s_VmaAllocator = VK_NULL_HANDLE;
        }
    }

    Common::ResultStr<VkBuffer> VulkanAllocator::RT_ZeroBuffer()
    {
        const std::scoped_lock lock( m_ZeroBufferMutex );
        if ( m_ZeroBuffer != VK_NULL_HANDLE )
            return Common::MakeSuccess( m_ZeroBuffer );
        constexpr VkDeviceSize   kBytes = 65536;
        const VkBufferCreateInfo info{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size  = kBytes,
                                       .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        VkBuffer                 buffer = VK_NULL_HANDLE;
        const auto allocated = RT_AllocateBuffer( "ZeroBuffer", info, VMA_MEMORY_USAGE_CPU_TO_GPU, buffer );
        if ( !allocated.IsSuccess() )
            return Common::MakeError<VkBuffer>( allocated.GetError() );
        const VmaAllocation allocation = allocated.GetValue();
        {
            MappedMemory mapped = MapMemory( allocation );
            if ( const auto filled = mapped.Fill( 0, kBytes ); !filled.IsSuccess() )
            {
                m_Ledger.Release( LedgerKey( allocation ) );
                vmaDestroyBuffer( s_VmaAllocator, buffer, allocation );
                return Common::MakeFormattedError<VkBuffer>( "zeroing the zero buffer failed: {}",
                                                             filled.GetError() );
            }
        }
        if ( const VkResult flushed = vmaFlushAllocation( s_VmaAllocator, allocation, 0, VK_WHOLE_SIZE );
             flushed != VK_SUCCESS )
        {
            m_Ledger.Release( LedgerKey( allocation ) );
            vmaDestroyBuffer( s_VmaAllocator, buffer, allocation );
            return Common::MakeFormattedError<VkBuffer>( "flushing the zero buffer failed: {}",
                                                         VkResultToString( flushed ) );
        }
        m_ZeroBuffer           = buffer;
        m_ZeroBufferAllocation = allocation;
        return Common::MakeSuccess( m_ZeroBuffer );
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
        // THE ONE PLACE a buffer's queue-family sharing is decided, whatever the caller's create info said
        // (VulkanResourceSharing.hpp: CONCURRENT across the device's families, so a read on async compute that no
        // graph declared - an upload, a device address - is never a read on a family that does not own it).
        m_Sharing.ApplyTo( localInfo );

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

        if ( PoisonNewMemory() )
        {
            // Only memory the CPU can reach: a GPU-only buffer is written by a staging copy before any
            // shader sees it, and poisoning it would need a command buffer this primitive does not own.
            VkMemoryPropertyFlags props = 0;
            vmaGetAllocationMemoryProperties( s_VmaAllocator, allocation, &props );
            if ( ( props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) != 0 )
            {
                MappedMemory mapped = MapMemory( allocation );
                if ( auto filled = mapped.Fill( 0xCD, mapped.GetSize() ); !filled.IsSuccess() )
                {
                    LOG_WARN( "[VmaAllocator] poisoning '{}' was refused: {}", tag, filled.GetError() );
                }
                else if ( const VkResult flushed =
                               vmaFlushAllocation( s_VmaAllocator, allocation, 0, VK_WHOLE_SIZE );
                          flushed != VK_SUCCESS )
                {
                    LOG_WARN( "[VmaAllocator] flushing the poisoned '{}' failed: {}", tag,
                              VkResultToString( flushed ) );
                }
            }
        }

        // VMA keeps the tag on the allocation too: the census's detailed map (vmaBuildStatsString) then names
        // every allocation a ledger row covers, and an unnamed one is memory no ledger accounts for.
        vmaSetAllocationName( s_VmaAllocator, allocation, tag.c_str() );
        m_Ledger.Record( LedgerKey( allocation ), tag, AllocationSize( allocation ) );
        return Common::MakeSuccess( allocation );
    }

    bool VulkanAllocator::PoisonNewMemory()
    {
#if defined( DESERT_CONFIG_DEBUG )
        static const bool s_Poison = []
        {
            // Read once, from a function-local static, before any render thread exists; nothing sets the
            // environment afterwards.
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            const char* value  = std::getenv( "DESERT_POISON_NEW_MEMORY" );
            const bool  poison = value != nullptr && value[0] != '\0' && value[0] != '0';
            if ( poison )
                LOG_WARN( "[VmaAllocator] DESERT_POISON_NEW_MEMORY is set: new host-visible buffers are filled "
                          "with 0xCD and new 2D images without data are cleared to NaN / 0xCD" );
            return poison;
        }();
        return s_Poison;
#else
        return false;
#endif
    }

    Common::ResultStr<VmaAllocation> VulkanAllocator::RT_AllocateImage( const std::string& tag, const VkImageCreateInfo& imageCreateInfo,
                                                                         VmaMemoryUsage usage, VkImage& outImage )
    {
        if ( s_VmaAllocator == VK_NULL_HANDLE ) return Common::MakeError<VmaAllocation>( "VmaAllocator is null" );

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage                   = usage;

        // The image's queue-family sharing is decided here and only here, as for RT_AllocateBuffer: an uploaded
        // texture a bindless read on async compute reaches is CONCURRENT (VulkanResourceSharing.hpp).
        VkImageCreateInfo localInfo = imageCreateInfo;
        m_Sharing.ApplyTo( localInfo );

        VmaAllocation allocation = VK_NULL_HANDLE; // see RT_AllocateBuffer above
        VkResult res = vmaCreateImage( s_VmaAllocator, &localInfo, &allocInfo, &outImage, &allocation, nullptr );
        if ( res != VK_SUCCESS )
        {
            // Same reasoning as RT_AllocateBuffer above, and the same defect it fixes.
            LOG_ERROR( "[VmaAllocator] vmaCreateImage failed for tag '{}' with VkResult: {} ({})", tag, (int)res, VkResultToString(res) );
            return Common::MakeFormattedError<VmaAllocation>(
                 "vmaCreateImage failed for tag '{}' with VkResult {} ({})", tag, (int)res,
                 VkResultToString( res ) );
        }

        // VMA keeps the tag on the allocation too: the census's detailed map (vmaBuildStatsString) then names
        // every allocation a ledger row covers, and an unnamed one is memory no ledger accounts for.
        vmaSetAllocationName( s_VmaAllocator, allocation, tag.c_str() );
        m_Ledger.Record( LedgerKey( allocation ), tag, AllocationSize( allocation ) );
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
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_BufferDeletionQueue.push_back( { buffer, allocation, queuedFrame } );
    }

    void VulkanAllocator::RT_ReleaseStaging( VkBuffer buffer, VmaAllocation allocation,
                                             const Common::ResultStr<VkResult>& flushed )
    {
        if ( buffer == VK_NULL_HANDLE || allocation == VK_NULL_HANDLE )
            return;
        if ( !flushed.IsSuccess() || flushed.GetValue() != VK_SUCCESS || s_VmaAllocator == VK_NULL_HANDLE )
        {
            RT_DestroyBuffer( buffer, allocation );
            return;
        }
        m_Ledger.Release( LedgerKey( allocation ) );
        vmaDestroyBuffer( s_VmaAllocator, buffer, allocation );
    }

    void VulkanAllocator::LogCensusIfDue()
    {
        const auto now = std::chrono::steady_clock::now();
        if ( s_VmaAllocator == VK_NULL_HANDLE || now - m_LastCensus < std::chrono::seconds( 1 ) )
            return;
        m_LastCensus                               = now;
        constexpr double                        MB = 1024.0 * 1024.0;
        std::string                             heaps;
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties( s_VmaAllocator, &props );
        std::vector<VmaBudget> budgets( props != nullptr ? props->memoryHeapCount : 0 );
        if ( !budgets.empty() )
            vmaGetHeapBudgets( s_VmaAllocator, budgets.data() );
        for ( std::size_t heap = 0; heap < budgets.size(); ++heap )
            heaps += std::format( " heap{}: blocks {:.1f} MB, allocations {:.1f} MB, driver usage {:.1f} MB;",
                                  heap, static_cast<double>( budgets[heap].statistics.blockBytes ) / MB,
                                  static_cast<double>( budgets[heap].statistics.allocationBytes ) / MB,
                                  static_cast<double>( budgets[heap].usage ) / MB );
        std::string tags;
        const auto  byTag = m_Ledger.ByTag();
        for ( std::size_t i = 0; i < byTag.size() && i < 6; ++i )
            tags += std::format( " {} x{} {:.1f} MB;", byTag[i].Tag, byTag[i].Count,
                                 static_cast<double>( byTag[i].Bytes ) / MB );
        std::vector<std::pair<std::ptrdiff_t, const AllocationLedger::TagTotal*>> growth;
        std::unordered_map<std::string, std::size_t>                              bytesNow;
        for ( const auto& total : byTag )
        {
            bytesNow[total.Tag] = total.Bytes;
            const auto last     = m_LastCensusBytes.find( total.Tag );
            const auto before   = last != m_LastCensusBytes.end() ? last->second : std::size_t{ 0 };
            if ( total.Bytes > before )
                growth.emplace_back( static_cast<std::ptrdiff_t>( total.Bytes - before ), &total );
        }
        std::ranges::sort( growth, []( const auto& a, const auto& b ) { return a.first > b.first; } );
        std::string grew;
        for ( std::size_t i = 0; i < growth.size() && i < 6; ++i )
            grew += std::format( " {} +{:.1f} MB (x{} {:.1f} MB);", growth[i].second->Tag,
                                 static_cast<double>( growth[i].first ) / MB, growth[i].second->Count,
                                 static_cast<double>( growth[i].second->Bytes ) / MB );
        m_LastCensusBytes = std::move( bytesNow );

#if DESERT_DEV_INSTRUMENTS
        // THE CENSUS BY SUBSYSTEM, at every new resident peak (BISTRO-MEM-2). The line above names the six largest
        // tags; a view killed for its memory (Bistro Lit, 6386 MB) needs the whole account at the peak: the
        // ledger rolled up by the subsystem its tags name, the allocations no ledger row covers (the render
        // graph's transient heaps and resources, VulkanRdgTransient / VulkanRenderGraph), the owners
        // (ResourceLedger) and the process/device readout - all at one moment, so they subtract.
        constexpr uint64_t kCensusPeakStep = 128ull * 1024ull * 1024ull;
        // The process's own footprint, read here (one syscall a second): MemoryWatch samples only inside its
        // watch windows after a scene load, and the Bistro peak comes minutes later, at the hand-over. The
        // PHYSICAL footprint, not the resident set: on unified memory the resident set leaves out every Metal
        // allocation (1.7 GB resident at a 6.4 GB kill), and phys_footprint is the number the process is killed on.
        if ( const uint64_t peak = Common::Utils::ReadProcessFootprint().Physical;
             peak >= m_CensusPeakReported + kCensusPeakStep )
        {
            m_CensusPeakReported = peak;
            struct Subsystem
            {
                std::string_view Name;
                std::size_t      Bytes = 0;
                std::size_t      Count = 0;
            };
            std::array<Subsystem, 11> rollup{ { { "textures (.detex)" },
                                                { "mesh vertex/index buffers" },
                                                { "distance fields" },
                                                { "surface cache" },
                                                { "shadows" },
                                                { "swapchain" },
                                                { "view targets (attachments)" },
                                                { "uniform/storage buffers" },
                                                { "staging" },
                                                { "other images/buffers" },
                                                { "owed deletion (freed after its frame)" } } };
            const auto subsystemOf = []( std::string_view tag ) -> std::size_t
            {
                const auto has = [&]( std::string_view part ) { return tag.find( part ) != std::string_view::npos; };
                if ( has( ".detex" ) )
                    return 0;
                if ( tag == "VertexBuffer" || tag == "IndexBuffer" )
                    return 1;
                if ( has( "DistanceField" ) )
                    return 2;
                if ( has( "SurfaceCache" ) )
                    return 3;
                if ( has( "Shadow" ) || has( "Cascade" ) )
                    return 4;
                if ( has( "SwapChain" ) || has( "Swapchain" ) )
                    return 5;
                if ( has( "_attachment" ) || has( "GBuffer" ) )
                    return 6;
                if ( has( "UniformBuffer" ) || has( "StorageBuffer" ) )
                    return 7;
                if ( has( "Staging" ) )
                    return 8;
                return 9;
            };
            for ( const auto& total : byTag )
            {
                Subsystem& into = rollup[subsystemOf( total.Tag )];
                into.Bytes += total.Bytes;
                into.Count += total.Count;
            }
            {
                const std::lock_guard lock( m_DeletionMutex );
                for ( const BufferDeletionEntry& owed : m_BufferDeletionQueue )
                {
                    rollup[10].Bytes += AllocationSize( owed.Allocation );
                    ++rollup[10].Count;
                }
                for ( const ImageDeletionEntry& owed : m_ImageDeletionQueue )
                {
                    rollup[10].Bytes += AllocationSize( owed.Allocation );
                    ++rollup[10].Count;
                }
            }
            std::string bySubsystem;
            for ( const Subsystem& subsystem : rollup )
                bySubsystem += std::format( "\n  {:<40} x{:<6} {:9.1f} MB", subsystem.Name, subsystem.Count,
                                            static_cast<double>( subsystem.Bytes ) / MB );
            std::size_t allocated = 0;
            for ( const VmaBudget& budget : budgets )
                allocated += static_cast<std::size_t>( budget.statistics.allocationBytes );
            const std::size_t ledgered = m_Ledger.LiveBytes();
            // The render graph's transient heaps are VMA allocations outside the tag ledger: their own row, from
            // the ResourceLedger rows VulkanRdgTransientAllocator opens with each heap (retired heaps included).
            const ResourceCensus owners = ResourceLedger::Take();
            const auto transient = static_cast<std::size_t>( owners.BytesFor( ResourceOwner::Device, ResourceKind::TransientHeap ) );
            const std::size_t untagged = allocated > ledgered ? allocated - ledgered : 0;
            bySubsystem += std::format( "\n  {:<40} x{:<6} {:9.1f} MB", "render graph transient heaps (RDG)",
                                        owners.KnownFor( ResourceOwner::Device, ResourceKind::TransientHeap ),
                                        static_cast<double>( transient ) / MB );
            // The render graph's dedicated resources (extracted transients: histories, readbacks) - VMA
            // allocations outside the tag ledger too, in their own ledger by resource name (VulkanRdgPool).
            const AllocationLedger& dedicated      = RdgDedicatedLedger();
            const std::size_t       dedicatedBytes = dedicated.LiveBytes();
            bySubsystem += std::format( "\n  {:<40} x{:<6} {:9.1f} MB", "render graph extracted resources (RDG)",
                                        dedicated.LiveCount(), static_cast<double>( dedicatedBytes ) / MB );
            const std::size_t accounted = transient + dedicatedBytes;
            bySubsystem +=
                 std::format( "\n  {:<40} {:<7} {:9.1f} MB (VMA allocations {:.1f} MB - ledger {:.1f} MB - RDG "
                              "heaps - RDG extracted)",
                              "not in any ledger", "",
                              static_cast<double>( untagged > accounted ? untagged - accounted : 0 ) / MB,
                              static_cast<double>( allocated ) / MB, static_cast<double>( ledgered ) / MB );
            bySubsystem += DetailedMapByName( s_VmaAllocator, 16 );
            {
                const auto byName = dedicated.ByTag();
                for ( std::size_t i = 0; i < byName.size() && i < 12; ++i )
                    bySubsystem += std::format( "\n    RDG extracted {:<42} x{:<6} {:9.1f} MB", byName[i].Tag,
                                                byName[i].Count, static_cast<double>( byName[i].Bytes ) / MB );
            }
            // The texture streaming pool (TEX-STREAMING): what its textures hold against r.Streaming.PoolSize.
            {
                const auto& pool = TextureStreamingDemand::Get().Pool();
                bySubsystem += std::format(
                     "\n  {:<40} x{:<6} {:9.1f} MB resident / budget {:.1f} MB (wanted {:.1f} MB, full chains "
                     "{:.1f} MB, {} streaming)",
                     "texture streaming pool", pool.Textures.load( std::memory_order_relaxed ),
                     static_cast<double>( pool.ResidentBytes.load( std::memory_order_relaxed ) ) / MB,
                     static_cast<double>( pool.BudgetBytes.load( std::memory_order_relaxed ) ) / MB,
                     static_cast<double>( pool.WantedBytes.load( std::memory_order_relaxed ) ) / MB,
                     static_cast<double>( pool.FullChainBytes.load( std::memory_order_relaxed ) ) / MB,
                     pool.Streaming.load( std::memory_order_relaxed ) );
            }
            std::string largest;
            for ( std::size_t i = 0; i < byTag.size() && i < 25; ++i )
                largest += std::format( "\n  {:<56} x{:<6} {:9.1f} MB", byTag[i].Tag, byTag[i].Count,
                                        static_cast<double>( byTag[i].Bytes ) / MB );
            LOG_INFO( "[Memory] census at footprint peak {:.1f} MB, device memory by subsystem:{}\n largest tags:{}",
                      static_cast<double>( peak ) / MB, bySubsystem, largest );
            LOG_INFO( "[Memory] {}", MemoryReadout::Take().Report() );
            // The CPU side of the same peak: the device rows above are what the GPU allocator holds; the rest of
            // the footprint is named by the kind of region charging it.
            LOG_INFO( "[Memory] {}", Common::Utils::ReadFootprintBreakdown().Describe( peak ) );
            LOG_INFO( "[Resources] {}", ResourceLedger::Report() );
        }
#endif

        std::size_t owedCount   = 0;
        std::size_t owedBuffers = 0;
        std::size_t owedImages  = 0;
        {
            const std::lock_guard lock( m_DeletionMutex );
            owedCount   = QueuedCountLocked();
            owedBuffers = m_BufferDeletionQueue.size();
            owedImages  = m_ImageDeletionQueue.size();
        }
        LOG_DEBUG(
             "[VulkanAllocator] census: {} owed deletion(s) ({} buffer(s), {} image(s)); ledger {} object(s) "
             "{:.1f} MB;{} largest:{} grew:{}",
             owedCount, owedBuffers, owedImages, m_Ledger.LiveCount(),
             static_cast<double>( m_Ledger.LiveBytes() ) / MB, heaps, tags, grew );
    }

    void VulkanAllocator::RT_DestroyImage( VkImage image, VmaAllocation allocation, VkImageView imageView,
                                           const std::vector<VkImageView>& mipImageViews )
    {
        if ( !image || !allocation ) return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_ImageDeletionQueue.push_back( { image, allocation, imageView, mipImageViews, queuedFrame } );
    }

    void VulkanAllocator::RT_DestroyFramebuffer( VkFramebuffer framebuffer )
    {
        if ( !framebuffer ) return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_FramebufferDeletionQueue.push_back( { framebuffer, queuedFrame } );
    }

    void VulkanAllocator::RT_DestroyRenderPass( VkRenderPass renderPass )
    {
        if ( !renderPass ) return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_RenderPassDeletionQueue.push_back( { renderPass, queuedFrame } );
    }

    void VulkanAllocator::RT_DestroyQueryPool( VkQueryPool queryPool )
    {
        if ( queryPool == VK_NULL_HANDLE )
            return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_QueryPoolDeletionQueue.push_back( { queryPool, queuedFrame } );
    }

    void VulkanAllocator::RT_DestroyPipeline( VkPipeline pipeline, VkPipelineLayout layout )
    {
        if ( pipeline == VK_NULL_HANDLE && layout == VK_NULL_HANDLE )
            return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_PipelineDeletionQueue.push_back( { pipeline, layout, queuedFrame } );
    }

    void VulkanAllocator::RT_DestroyAccelerationStructure( VkAccelerationStructureKHR accelerationStructure,
                                                           PFN_vkDestroyAccelerationStructureKHR destroy )
    {
        if ( accelerationStructure == VK_NULL_HANDLE || destroy == nullptr )
            return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_AccelerationStructureDeletionQueue.push_back( { accelerationStructure, destroy, queuedFrame } );
    }

    void VulkanAllocator::RT_DestroyDescriptorPool( VkDescriptorPool descriptorPool )
    {
        if ( descriptorPool == VK_NULL_HANDLE )
            return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_DescriptorPoolDeletionQueue.push_back( { descriptorPool, queuedFrame, {} } );
    }

    void VulkanAllocator::RT_DestroyLater( std::function<void()> destroy )
    {
        if ( !destroy )
            return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_HostObjectDeletionQueue.push_back( { std::move( destroy ), queuedFrame } );
    }

    void VulkanAllocator::RT_FreeDescriptorSets( VkDescriptorPool pool, std::vector<VkDescriptorSet> sets )
    {
        if ( pool == VK_NULL_HANDLE || sets.empty() )
            return;
        const uint64_t        queuedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const std::lock_guard lock( m_DeletionMutex );
        m_DescriptorPoolDeletionQueue.push_back( { pool, queuedFrame, std::move( sets ) } );
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
        // The per-frame drain: the caller (VulkanRendererAPI::PresentFinalImage) has begun frame `current` and
        // waited its slot's fence, the fence of frame current - framesInFlight; every entry queued on that
        // frame or earlier, by any thread, is finished with (IsRetired).
        const Engine::FrameManager& frames         = Engine::FrameManager::GetInstance();
        const uint64_t              current        = frames.GetAbsoluteFrameCount();
        const uint32_t              framesInFlight = frames.GetMaxFramesInFlight();
        (void)DestroyQueued( [current, framesInFlight]( uint64_t queued )
                             { return IsRetired( queued, current, framesInFlight ); } );
#if DESERT_DEV_INSTRUMENTS
        SampleSceneOpen();
#endif
        LogCensusIfDue();
    }

    std::size_t VulkanAllocator::OwedBytes() const
    {
        const std::lock_guard lock( m_DeletionMutex );
        std::size_t owed = 0;
        for ( const BufferDeletionEntry& entry : m_BufferDeletionQueue )
            owed += AllocationSize( entry.Allocation );
        for ( const ImageDeletionEntry& entry : m_ImageDeletionQueue )
            owed += AllocationSize( entry.Allocation );
        return owed;
    }

    void VulkanAllocator::LogSceneOpenPeak( std::string_view when ) const
    {
        constexpr double MB = 1024.0 * 1024.0;
        LOG_INFO( "[Memory] scene open #{} '{}' peak {:.0f} MB, owed {:.1f} MB{}", m_SceneOpenIndex, m_SceneOpenName,
                  static_cast<double>( m_SceneOpenPeak ) / MB, static_cast<double>( m_SceneOpenOwedAtPeak ) / MB,
                  when );
    }

    void VulkanAllocator::BeginSceneOpen( std::string_view scene )
    {
        if ( m_SceneOpenIndex > 0 )
            LogSceneOpenPeak( " (window closed: the next open starts)" );
        const uint64_t now      = Common::Utils::ReadProcessFootprint().Physical;
        ++m_SceneOpenIndex;
        m_SceneOpenName       = std::string( scene );
        m_SceneOpenPeak       = now;
        m_SceneOpenOwedAtPeak = OwedBytes();
        m_SceneOpenPeakLogged = now;
        // The full subsystem census reports again from here: a second open below the run's first peak is
        // otherwise never written (FIELD-MEM p3).
        m_CensusPeakReported = now;
        LogSceneOpenPeak( " (window opened)" );
    }

    void VulkanAllocator::SampleSceneOpen()
    {
        if ( m_SceneOpenIndex == 0 || s_VmaAllocator == VK_NULL_HANDLE )
            return;
        const uint64_t now = Common::Utils::ReadProcessFootprint().Physical;
        if ( now <= m_SceneOpenPeak )
            return;
        m_SceneOpenPeak       = now;
        m_SceneOpenOwedAtPeak = OwedBytes();
        // One line per 16 MB of climb: the last line of a window is its peak to within one step, and the
        // closing line (next BeginSceneOpen) carries the exact value.
        constexpr uint64_t kStep = 16ull * 1024ull * 1024ull;
        if ( now >= m_SceneOpenPeakLogged + kStep )
        {
            m_SceneOpenPeakLogged = now;
            LogSceneOpenPeak( "" );
        }
    }

    std::size_t VulkanAllocator::DrainDeletionQueue()
    {
        // WHY AN UNCONDITIONAL DRAIN EXISTS AT ALL, in the numbers that produced it. ProcessDeletionQueue
        // above is reached from exactly one place — VulkanRendererAPI::PresentFinalImage — so it runs only while
        // frames run. Shutdown releases the whole content of the engine AFTER the last frame: Renderer::Shutdown
        // clears every resource service, the scene renderers drop their passes and the swapchain gives its
        // capture buffer back, and every one of those calls RT_Destroy*, which QUEUES. Nothing ever came
        // back to drain it, so all of it was still queued when vkDestroyDevice ran: 6 370 of the 6 599
        // objects the validation layer reported leaking on a normal close, VkBuffer alone being 5 716.
        //
        // The frame condition is dropped deliberately rather than approximated with "any frame": there is
        // no next frame to wait for, and the caller has already idled the device, so every entry here is
        // finished with by construction.
        // REPEATED UNTIL A PASS FINDS NOTHING: a host-object destroy may queue further objects, which the pass
        // that ran it no longer takes (DestroyQueued takes under its lock, then destroys).
        std::size_t total = 0;
        for ( std::size_t pass = DestroyQueued( []( uint64_t ) { return true; } ); pass > 0;
              pass             = DestroyQueued( []( uint64_t ) { return true; } ) )
            total += pass;
        return total;
    }

    std::size_t VulkanAllocator::DestroyQueued( const std::function<bool( uint64_t )>& takeFrame )
    {
        if ( s_VmaAllocator == VK_NULL_HANDLE )
            return 0;

        VkDevice device = m_Device;
        if ( device == VK_NULL_HANDLE )
            return 0;

        // TAKE UNDER THE LOCK, DESTROY OUTSIDE IT. Every entry @p takeFrame retires (IsRetired per frame) is
        // moved out of its queue while m_DeletionMutex is held; nothing is destroyed until it is released. A
        // loader thread queuing meanwhile lands in the queue for a later pass, and a host-object destroy that
        // queues again (below) re-enters RT_Destroy* without the lock held.
        const auto take = [&takeFrame]<typename Entry>( std::vector<Entry>& queue )
        {
            std::vector<Entry> taken;
            std::vector<Entry> kept;
            for ( Entry& entry : queue )
            {
                if ( takeFrame( entry.QueuedFrame ) )
                    taken.push_back( std::move( entry ) );
                else
                    kept.push_back( std::move( entry ) );
            }
            queue = std::move( kept );
            return taken;
        };

        std::vector<AccelerationStructureDeletionEntry> accelerationStructures;
        std::vector<BufferDeletionEntry>                buffers;
        std::vector<ImageDeletionEntry>                 images;
        std::vector<FramebufferDeletionEntry>           framebuffers;
        std::vector<RenderPassDeletionEntry>            renderPasses;
        std::vector<HostObjectDeletionEntry>            hostObjects;
        std::vector<QueryPoolDeletionEntry>             queryPools;
        std::vector<PipelineDeletionEntry>              pipelines;
        std::vector<DescriptorPoolDeletionEntry>        descriptorPools;
        {
            const std::lock_guard lock( m_DeletionMutex );
            accelerationStructures = take( m_AccelerationStructureDeletionQueue );
            buffers                = take( m_BufferDeletionQueue );
            images                 = take( m_ImageDeletionQueue );
            framebuffers           = take( m_FramebufferDeletionQueue );
            renderPasses           = take( m_RenderPassDeletionQueue );
            hostObjects            = take( m_HostObjectDeletionQueue );
            queryPools             = take( m_QueryPoolDeletionQueue );
            pipelines              = take( m_PipelineDeletionQueue );

            // Destroying a pool frees every set still in it. A free of that pool's sets queued AFTER the destroy
            // (taken now, later in queue order) or on a frame not yet retired (still queued)
            // would then name dead handles - and a pool handle the driver hands out again: both are dropped with
            // the pool here, under the lock, before any other thread can queue against a reused handle.
            const std::vector<DescriptorPoolDeletionEntry> takenPools = take( m_DescriptorPoolDeletionQueue );
            std::vector<VkDescriptorPool>                  destroyedPools;
            const auto destroyedAlready = [&destroyedPools]( VkDescriptorPool pool )
            { return std::find( destroyedPools.begin(), destroyedPools.end(), pool ) != destroyedPools.end(); };
            for ( const DescriptorPoolDeletionEntry& entry : takenPools )
            {
                if ( !entry.Sets.empty() && destroyedAlready( entry.DescriptorPool ) )
                    continue;
                if ( entry.Sets.empty() )
                    destroyedPools.push_back( entry.DescriptorPool );
                descriptorPools.push_back( entry );
            }
            std::erase_if( m_DescriptorPoolDeletionQueue,
                           [&destroyedAlready]( const DescriptorPoolDeletionEntry& entry )
                           { return !entry.Sets.empty() && destroyedAlready( entry.DescriptorPool ); } );
        }

        std::size_t destroyed = 0;
        // FIRST: an acceleration structure is destroyed before the buffer it lives in (queued on the same frame).
        for ( const AccelerationStructureDeletionEntry& entry : accelerationStructures )
        {
            entry.Destroy( device, entry.AccelerationStructure, nullptr );
            ++destroyed;
        }

        for ( const BufferDeletionEntry& entry : buffers )
        {
            m_Ledger.Release( LedgerKey( entry.Allocation ) );
            vmaDestroyBuffer( s_VmaAllocator, entry.Buffer, entry.Allocation );
            ++destroyed;
        }

        for ( const ImageDeletionEntry& entry : images )
        {
            if ( entry.ImageView != VK_NULL_HANDLE )
                vkDestroyImageView( device, entry.ImageView, nullptr );
            for ( VkImageView view : entry.MipImageViews )
                vkDestroyImageView( device, view, nullptr );

            m_Ledger.Release( LedgerKey( entry.Allocation ) );
            vmaDestroyImage( s_VmaAllocator, entry.Image, entry.Allocation );
            ++destroyed;
        }

        for ( const FramebufferDeletionEntry& entry : framebuffers )
        {
            vkDestroyFramebuffer( device, entry.Framebuffer, nullptr );
            ++destroyed;
        }

        for ( const RenderPassDeletionEntry& entry : renderPasses )
        {
            vkDestroyRenderPass( device, entry.RenderPass, nullptr );
            ++destroyed;
        }

        // The call may queue further destructions: they are tagged with the frame current now and are
        // retired framesInFlight frames later (DrainDeletionQueue repeats until nothing is left).
        for ( const HostObjectDeletionEntry& entry : hostObjects )
        {
            entry.Destroy();
            ++destroyed;
        }

        for ( const QueryPoolDeletionEntry& entry : queryPools )
        {
            vkDestroyQueryPool( device, entry.QueryPool, nullptr );
            ++destroyed;
        }

        for ( const PipelineDeletionEntry& entry : pipelines )
        {
            if ( entry.Pipeline != VK_NULL_HANDLE )
                vkDestroyPipeline( device, entry.Pipeline, nullptr );
            if ( entry.Layout != VK_NULL_HANDLE )
                vkDestroyPipelineLayout( device, entry.Layout, nullptr );
            ++destroyed;
        }

        for ( const DescriptorPoolDeletionEntry& entry : descriptorPools )
        {
            if ( !entry.Sets.empty() )
                vkFreeDescriptorSets( device, entry.DescriptorPool, static_cast<uint32_t>( entry.Sets.size() ),
                                      entry.Sets.data() );
            else
                vkDestroyDescriptorPool( device, entry.DescriptorPool, nullptr );
            ++destroyed;
        }

        return destroyed;
    }

    std::size_t VulkanAllocator::QueuedCount() const
    {
        const std::lock_guard lock( m_DeletionMutex );
        return QueuedCountLocked();
    }

    std::size_t VulkanAllocator::QueuedCountLocked() const
    {
        return m_BufferDeletionQueue.size() + m_ImageDeletionQueue.size() + m_FramebufferDeletionQueue.size() +
               m_RenderPassDeletionQueue.size() + m_QueryPoolDeletionQueue.size() +
               m_PipelineDeletionQueue.size() + m_AccelerationStructureDeletionQueue.size() +
               m_DescriptorPoolDeletionQueue.size() + m_HostObjectDeletionQueue.size();
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
