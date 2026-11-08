#include <Common/Core/Core.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingScene.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    static_assert( sizeof( VkAccelerationStructureInstanceKHR ) == RayTracing::kInstanceRowBytes,
                   "EncodeInstanceRows writes VkAccelerationStructureInstanceKHR rows" );

    namespace
    {
        // The single INSTANCES geometry of a TLAS build; @p instances is the rows' device address (0 to size).
        VkAccelerationStructureGeometryKHR TlasGeometry( VkDeviceAddress instances )
        {
            VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.geometry.instances.sType =
                 VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            geometry.geometry.instances.arrayOfPointers    = VK_FALSE;
            geometry.geometry.instances.data.deviceAddress = instances;
            return geometry;
        }

        VkAccelerationStructureBuildGeometryInfoKHR
        TlasBuildInfo( const VkAccelerationStructureGeometryKHR& geometry )
        {
            VkAccelerationStructureBuildGeometryInfoKHR build{
                 VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            build.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            // ALLOW_UPDATE on every build and size query alike: an update's source must have been built with it.
            build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            build.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = 1;
            build.pGeometries   = &geometry;
            return build;
        }

        VkAccelerationStructureBuildSizesInfoKHR TlasSizes( const RayTracingDeviceFunctions& functions,
                                                            VkDevice device, uint32_t instanceCount )
        {
            const VkAccelerationStructureGeometryKHR          geometry = TlasGeometry( 0 );
            const VkAccelerationStructureBuildGeometryInfoKHR build    = TlasBuildInfo( geometry );
            VkAccelerationStructureBuildSizesInfoKHR          sizes{
                 VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                          &build, &instanceCount, &sizes );
            return sizes;
        }
    } // namespace

    Common::ResultStr<RayTracing::TlasBuildSizes> VulkanRayTracingScene::Prepare( uint32_t   instanceCount,
                                                                                  const bool update )
    {
        using Sizes        = RayTracing::TlasBuildSizes;
        const auto logical = VulkanLogicalDevice::Current();
        if ( !logical )
            return Common::MakeError<Sizes>( "TLAS: no Vulkan device" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( !functions.Complete() )
            return Common::MakeError<Sizes>(
                 "TLAS: the device has no acceleration-structure functions, but a resolved method traces rays" );
        const VkDevice device = logical->GetVulkanLogicalDevice();

        const VkAccelerationStructureBuildSizesInfoKHR frameSizes = TlasSizes( functions, device, instanceCount );
        if ( m_Handle == VK_NULL_HANDLE || instanceCount > m_Capacity ||
             frameSizes.accelerationStructureSize > m_StorageBytes )
        {
            Release();
            const uint32_t capacity = std::bit_ceil( std::max( instanceCount, 16u ) );
            const VkAccelerationStructureBuildSizesInfoKHR storageSizes = TlasSizes( functions, device, capacity );

            const auto& allocator =
                 SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size  = storageSizes.accelerationStructureSize;
            info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            const auto storage =
                 allocator->RT_AllocateBuffer( "TLAS", info, VMA_MEMORY_USAGE_GPU_ONLY, m_Storage );
            if ( !storage.IsSuccess() )
                return Common::MakeFormattedError<Sizes>( "TLAS storage: {}", storage.GetError() );
            m_StorageMemory = storage.GetValue();
            m_StorageBytes  = storageSizes.accelerationStructureSize;
            m_Ledger        = RayTracing::RecordAccelerationStructureMemory(
                 RayTracing::AccelerationStructureLevel::Top, VulkanAllocator::AllocationSize( m_StorageMemory ) );
            m_Destroy = functions.DestroyAccelerationStructure;

            VkAccelerationStructureCreateInfoKHR create{
                 VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
            create.buffer          = m_Storage;
            create.size            = m_StorageBytes;
            create.type            = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            const VkResult created = functions.CreateAccelerationStructure( device, &create, nullptr, &m_Handle );
            if ( created != VK_SUCCESS )
            {
                m_Handle = VK_NULL_HANDLE;
                Release();
                return Common::MakeFormattedError<Sizes>( "vkCreateAccelerationStructureKHR (TLAS) failed: {}",
                                                          static_cast<int>( created ) );
            }
            m_Capacity = capacity;

            // A new storage buffer starts untouched; each graph hands back the state it leaves the buffer in.
            m_External = RDG::ExternalBuffer( RDG::BufferDesc{ m_StorageBytes }, RDG::Access::None );
            m_External.Physical =
                 VulkanRdgBuffer::WrapAccelerationStructure( m_Storage, m_StorageBytes, m_Handle );
            m_External.RecordFinalState = [this]( const RDG::AccessState& state ) -> Common::BoolResultStr
            {
                m_External.State = state;
                // After EndGraph: the build or update this graph recorded is submitted, so it is the storage's.
                if ( m_RecordedInstances != kNoBuild )
                    m_BuiltInstances = m_RecordedInstances;
                m_RecordedInstances = kNoBuild;
                return BOOLSUCCESS;
            };
        }
        // A build recorded into a graph that never ended (abandoned: nothing submitted) is still pending: its
        // renderer counted it as done, so this frame builds instead of updating a source that build never wrote.
        if ( m_RecordedInstances != kNoBuild )
        {
            m_BuiltInstances    = kNoBuild;
            m_RecordedInstances = kNoBuild;
        }
        // Recreated storage resets m_BuiltInstances (Release): its first frame builds.
        const bool updates = update && m_BuiltInstances == instanceCount;
        return Common::MakeSuccess(
             Sizes{ .ScratchBytes = ( updates ? frameSizes.updateScratchSize : frameSizes.buildScratchSize ) +
                                    kMaxAccelerationStructureScratchAlignment,
                    .Update       = updates } );
    }

    Common::ResultStr<bool> VulkanRayTracingScene::EnsureRows( const RayTracing::SceneRowBuffer buffer,
                                                               const uint64_t                   bytes )
    {
        const auto index = static_cast<std::size_t>( buffer );
        if ( index >= kRowBufferCount )
            return Common::MakeFormattedError<bool>( "scene row buffer {} does not exist", index );
        RowBuffer& rows = m_Rows[index];
        if ( rows.Buffer != VK_NULL_HANDLE && bytes <= rows.Bytes )
            return Common::MakeSuccess( false );
        ReleaseRows( rows );
        // The same growth as the storage: a scene that adds one row a frame does not recreate every frame. 1 KiB
        // at least (sixteen TLAS rows); no row size needs the capacity to be its multiple.
        constexpr uint64_t kMinBytes = 1024u;
        const uint64_t     capacity  = std::bit_ceil( std::max( bytes, kMinBytes ) );
        // Copies write every buffer and ray-query passes read it; the build reads the instance rows by address.
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if ( buffer == RayTracing::SceneRowBuffer::Instances )
            usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        constexpr std::array<const char*, kRowBufferCount> kNames = {
             "TLAS instances", "RT primitive instances", "RT geometry records", "RT material records" };
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size        = capacity;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const auto memory =
             allocator->RT_AllocateBuffer( kNames[index], info, VMA_MEMORY_USAGE_GPU_ONLY, rows.Buffer );
        if ( !memory.IsSuccess() )
        {
            rows.Buffer = VK_NULL_HANDLE;
            return Common::MakeError<bool>( memory.GetError() );
        }
        rows.Memory = memory.GetValue();
        rows.Bytes  = capacity;
        rows.Ledger = ResourceOwnership::Take( Graphic::ResourceKind::StorageBuffer,
                                               VulkanAllocator::AllocationSize( rows.Memory ) );
        // A new buffer starts untouched; each graph hands back the state it leaves the buffer in.
        rows.External          = RDG::ExternalBuffer( RDG::BufferDesc{ capacity }, RDG::Access::None );
        rows.External.Physical = VulkanRdgBuffer::Wrap( rows.Buffer, capacity );
        rows.External.RecordFinalState = [this, index]( const RDG::AccessState& state ) -> Common::BoolResultStr
        {
            m_Rows[index].External.State = state;
            ++m_Rows[index].GraphsEnded; // after EndGraph: this graph's commands are submitted
            return BOOLSUCCESS;
        };
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRayTracingScene::RecordRowScatter(
         const RDG::PassContext& context, const RDG::BufferBinding& staging, const RDG::BufferBinding& rows,
         const std::span<const RayTracing::SceneRowRun> runs, const uint32_t rowBytes )
    {
        const auto command = VulkanRdgBackend::CommandBufferOf( context );
        if ( !command )
            return Common::MakeError( command.GetError() );
        const auto source = VulkanRdgBackend::BufferOf( staging );
        if ( !source )
            return Common::MakeError( source.GetError() );
        const auto destination = VulkanRdgBackend::BufferOf( rows );
        if ( !destination )
            return Common::MakeError( destination.GetError() );
        const VkDeviceSize        sourceBase      = source.GetValue()->GetOffset();
        const VkDeviceSize        destinationBase = destination.GetValue()->GetOffset();
        std::vector<VkBufferCopy> regions;
        regions.reserve( runs.size() );
        VkDeviceSize packed = 0;
        for ( const RayTracing::SceneRowRun& run : runs )
        {
            VkBufferCopy region{};
            region.size      = static_cast<VkDeviceSize>( run.RowCount ) * rowBytes;
            region.srcOffset = sourceBase + packed;
            region.dstOffset = destinationBase + static_cast<VkDeviceSize>( run.FirstRow ) * rowBytes;
            regions.push_back( region );
            packed += region.size;
        }
        if ( regions.empty() )
            return BOOLSUCCESS;
        vkCmdCopyBuffer( command.GetValue(), source.GetValue()->GetBuffer(), destination.GetValue()->GetBuffer(),
                         static_cast<uint32_t>( regions.size() ), regions.data() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanRayTracingScene::RecordBuild( const RDG::PassContext&   context,
                                                              const RDG::BufferBinding& instances,
                                                              const RDG::BufferBinding& scratch,
                                                              const uint32_t            instanceCount,
                                                              const bool                update )
    {
        const auto command = VulkanRdgBackend::CommandBufferOf( context );
        if ( !command )
            return Common::MakeError( command.GetError() );
        const auto instanceBuffer = VulkanRdgBackend::BufferOf( instances );
        if ( !instanceBuffer )
            return Common::MakeError( instanceBuffer.GetError() );
        const auto scratchBuffer = VulkanRdgBackend::BufferOf( scratch );
        if ( !scratchBuffer )
            return Common::MakeError( scratchBuffer.GetError() );
        const auto logical = VulkanLogicalDevice::Current();
        if ( !logical || m_Handle == VK_NULL_HANDLE )
            return Common::MakeError( "TLAS build recorded without a prepared acceleration structure" );
        if ( update && m_BuiltInstances != instanceCount )
            return Common::MakeError( std::format( "TLAS update of {} instances recorded over a build of {}",
                                                       instanceCount, m_BuiltInstances ) );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        const VkDevice                   device    = logical->GetVulkanLogicalDevice();

        const VkAccelerationStructureGeometryKHR geometry =
             TlasGeometry( ( functions.BufferAddress( device, instanceBuffer.GetValue()->GetBuffer() ) + instanceBuffer.GetValue()->GetOffset() ) );
        VkAccelerationStructureBuildGeometryInfoKHR build = TlasBuildInfo( geometry );
        build.dstAccelerationStructure                    = m_Handle;
        if ( update )
        {
            build.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            build.srcAccelerationStructure = m_Handle; // in place
        }
        build.scratchData.deviceAddress =
             AlignScratchAddress( ( functions.BufferAddress( device, scratchBuffer.GetValue()->GetBuffer() ) + scratchBuffer.GetValue()->GetOffset() ) );

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount                                   = instanceCount;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        functions.CmdBuildAccelerationStructures( command.GetValue(), 1, &build, &ranges );
        // Recorded, not yet submitted: m_BuiltInstances follows only when this graph ends (RecordFinalState).
        m_RecordedInstances = instanceCount;
        return BOOLSUCCESS;
    }

    void VulkanRayTracingScene::Release()
    {
        if ( m_Handle == VK_NULL_HANDLE && m_Storage == VK_NULL_HANDLE )
            return;
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        if ( m_Handle != VK_NULL_HANDLE )
            allocator->RT_DestroyAccelerationStructure( m_Handle, m_Destroy );
        if ( m_Storage != VK_NULL_HANDLE )
            allocator->RT_DestroyBuffer( m_Storage, m_StorageMemory );
        m_Handle        = VK_NULL_HANDLE;
        m_Storage       = VK_NULL_HANDLE;
        m_StorageMemory = nullptr;
        m_StorageBytes  = 0;
        m_Ledger        = ResourceOwnership{};
        m_Capacity      = 0;
        m_BuiltInstances    = kNoBuild;
        m_RecordedInstances = kNoBuild;
        m_External          = RDG::ExternalBuffer{};
    }

    void VulkanRayTracingScene::ReleaseRows( RowBuffer& rows )
    {
        if ( rows.Buffer == VK_NULL_HANDLE )
            return;
        // Queued, not destroyed: a frame still in flight may read the old rows (RT_DestroyBuffer's deletion
        // queue).
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        allocator->RT_DestroyBuffer( rows.Buffer, rows.Memory );
        rows.Buffer   = VK_NULL_HANDLE;
        rows.Memory   = nullptr;
        rows.Bytes    = 0;
        rows.Ledger   = ResourceOwnership{};
        rows.External = RDG::ExternalBuffer{};
    }

    VulkanRayTracingScene::~VulkanRayTracingScene()
    {
        Release();
        for ( RowBuffer& rows : m_Rows )
            ReleaseRows( rows );
    }
} // namespace Desert::Graphic::API::Vulkan

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<RayTracingSceneTarget> CreateRayTracingSceneTarget()
    {
        return std::make_unique<API::Vulkan::VulkanRayTracingScene>();
    }
} // namespace Desert::Graphic::RayTracing
