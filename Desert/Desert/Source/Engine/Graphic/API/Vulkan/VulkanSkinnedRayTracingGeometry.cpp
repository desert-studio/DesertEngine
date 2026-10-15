#include <Common/Core/Core.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSkinnedRayTracingGeometry.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        using Prepared = RayTracing::SkinnedBlasPrepared;

        // A deformed BLAS is rebuilt or refit every frame and traced for one: fast build over fast trace, and
        // ALLOW_UPDATE so the frames after the first refit instead of rebuilding (UE dynamic geometry flags).
        constexpr VkBuildAccelerationStructureFlagsKHR kSkinnedBlasFlags =
             VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
             VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;

        std::shared_ptr<VulkanLogicalDevice> LogicalDevice()
        {
            return SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        }
    } // namespace

    Common::ResultStr<Prepared>
    VulkanSkinnedRayTracingGeometry::Prepare( const RayTracing::RayTracingMeshInput& input )
    {
        const auto logical = LogicalDevice();
        if ( !logical )
            return Common::MakeError<Prepared>( "SkinCache: no Vulkan device" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( !functions.Complete() )
            return Common::MakeError<Prepared>(
                 "SkinCache: the device has no acceleration-structure functions, but "
                 "a resolved method traces rays" );

        const auto& vertices    = input.Vertices;
        const auto& indices     = input.Indices;
        const auto vertexBuffer = SP_CAST( VulkanVertexBuffer, vertices );
        const auto indexBuffer  = SP_CAST( VulkanIndexBuffer, indices );
        if ( !vertexBuffer || !indexBuffer || vertexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE ||
             indexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE )
            return Common::MakeError<Prepared>(
                 "SkinCache: the skinned mesh has no resident vertex/index buffer" );

        const uint32_t vertexCount = input.VertexCount;
        if ( m_Handle != VK_NULL_HANDLE && vertices == m_SourceVertices && indices == m_Indices &&
             input.Segments == m_Segments && vertexCount == m_VertexCount )
            return Common::MakeSuccess( Prepared{ ( m_Built ? m_UpdateScratch : m_BuildScratch ) +
                                                       kMaxAccelerationStructureScratchAlignment,
                                                  m_Built } );

        Release();
        if ( input.Segments.empty() || vertexCount == 0 )
            return Common::MakeFormattedError<Prepared>(
                 "SkinCache: the skinned mesh has no visible triangle ({} vertices, {} segments)", vertexCount,
                 input.Segments.size() );
        m_Segments = input.Segments;

        const VkDevice device = logical->GetVulkanLogicalDevice();
        const auto&    allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();

        // The cached positions: kDeformedPositionLayout (one float3 per vertex, the mesh's vertex order), written
        // by the skin pass as a storage buffer and read by the BLAS build by address.
        m_PositionsBytes = static_cast<VkDeviceSize>( vertexCount ) * RayTracing::kDeformedPositionLayout.Stride;
        const auto positions = allocator->RT_AllocateBuffer(
             "SkinCache_Positions",
             DeviceAddressBufferInfo( m_PositionsBytes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR ),
             VMA_MEMORY_USAGE_GPU_ONLY, m_Positions );
        if ( !positions.IsSuccess() )
        {
            m_Positions = VK_NULL_HANDLE;
            Release();
            return Common::MakeFormattedError<Prepared>( "SkinCache positions: {}", positions.GetError() );
        }
        m_PositionsMemory = positions.GetValue();
        m_PositionsLedger = ResourceOwnership::Take( ResourceKind::StorageBuffer,
                                                     VulkanAllocator::AllocationSize( m_PositionsMemory ) );

        m_IndexAddress = functions.BufferAddress( device, indexBuffer->GetVulkanBuffer() );
        const VulkanBlasBuildInput input =
             TranslateBlasGeometry( m_Segments, functions.BufferAddress( device, m_Positions ), m_IndexAddress );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( input );
        build.flags                                       = kSkinnedBlasFlags;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                      &build, input.MaxPrimitiveCounts.data(), &sizes );
        m_BuildScratch  = sizes.buildScratchSize;
        m_UpdateScratch = sizes.updateScratchSize;

        const auto storage = allocator->RT_AllocateBuffer(
             "SkinCache_BLAS",
             DeviceAddressBufferInfo( sizes.accelerationStructureSize,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR ),
             VMA_MEMORY_USAGE_GPU_ONLY, m_Storage );
        if ( !storage.IsSuccess() )
        {
            m_Storage = VK_NULL_HANDLE;
            Release();
            return Common::MakeFormattedError<Prepared>( "SkinCache BLAS storage: {}", storage.GetError() );
        }
        m_StorageMemory = storage.GetValue();
        m_StorageBytes  = sizes.accelerationStructureSize;
        m_StorageLedger = RayTracing::RecordAccelerationStructureMemory(
             RayTracing::AccelerationStructureLevel::Bottom, VulkanAllocator::AllocationSize( m_StorageMemory ) );
        m_Destroy = functions.DestroyAccelerationStructure;

        VkAccelerationStructureCreateInfoKHR create{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        create.buffer          = m_Storage;
        create.size            = m_StorageBytes;
        create.type            = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        const VkResult created = functions.CreateAccelerationStructure( device, &create, nullptr, &m_Handle );
        if ( created != VK_SUCCESS )
        {
            m_Handle = VK_NULL_HANDLE;
            Release();
            return Common::MakeFormattedError<Prepared>(
                 "vkCreateAccelerationStructureKHR (skinned BLAS) failed: {}", static_cast<int>( created ) );
        }
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addressInfo.accelerationStructure = m_Handle;
        m_DeviceAddress = functions.GetAccelerationStructureDeviceAddress( device, &addressInfo );
        if ( m_DeviceAddress == 0 )
        {
            Release();
            return Common::MakeError<Prepared>( "vkGetAccelerationStructureDeviceAddressKHR answered 0 for a "
                                                "skinned BLAS" );
        }

        m_SourceVertices  = vertices;
        m_Indices         = indices;
        m_VertexCount     = vertexCount;

        // Both start untouched; each graph hands back the state it leaves them in.
        m_PositionsExternal = RDG::ExternalBuffer( RDG::BufferDesc{ m_PositionsBytes }, RDG::Access::None );
        m_PositionsExternal.Physical         = VulkanRdgBuffer::Wrap( m_Positions, m_PositionsBytes );
        m_PositionsExternal.RecordFinalState = [this]( const RDG::AccessState& state ) -> Common::BoolResultStr
        {
            m_PositionsExternal.State = state;
            return BOOLSUCCESS;
        };
        m_StorageExternal = RDG::ExternalBuffer( RDG::BufferDesc{ m_StorageBytes }, RDG::Access::None );
        m_StorageExternal.Physical =
             VulkanRdgBuffer::WrapAccelerationStructure( m_Storage, m_StorageBytes, m_Handle );
        m_StorageExternal.RecordFinalState = [this]( const RDG::AccessState& ) -> Common::BoolResultStr
        {
            // The BLAS's last reader is not the TLAS build the graph saw but every ray query that traverses it
            // through the TLAS (readers declare the TLAS, never the BLAS). The next frame's refit writes this
            // storage in place, so it must wait for those reads: the state handed on is the ray-query read.
            m_StorageExternal.State = RDG::GetAccessState( RDG::Access::AccelStructRead );
            return BOOLSUCCESS;
        };
        return Common::MakeSuccess(
             Prepared{ m_BuildScratch + kMaxAccelerationStructureScratchAlignment, false } );
    }

    Common::BoolResultStr VulkanSkinnedRayTracingGeometry::RecordBuild( const RDG::PassContext&   context,
                                                                        const RDG::BufferBinding& positions,
                                                                        const RDG::BufferBinding* /*indices*/,
                                                                        const RDG::BufferBinding& scratch,
                                                                        const uint64_t            scratchOffset )
    {
        const auto command = VulkanRdgBackend::CommandBufferOf( context );
        if ( !command )
            return Common::MakeError( command.GetError() );
        const auto positionBuffer = VulkanRdgBackend::BufferOf( positions );
        if ( !positionBuffer )
            return Common::MakeError( positionBuffer.GetError() );
        const auto scratchBuffer = VulkanRdgBackend::BufferOf( scratch );
        if ( !scratchBuffer )
            return Common::MakeError( scratchBuffer.GetError() );
        const auto logical = LogicalDevice();
        if ( !logical || m_Handle == VK_NULL_HANDLE )
            return Common::MakeError( "skinned BLAS build recorded without a prepared acceleration structure" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        const VkDevice                   device    = logical->GetVulkanLogicalDevice();

        const VulkanBlasBuildInput input = TranslateBlasGeometry(
             m_Segments, functions.BufferAddress( device, positionBuffer.GetValue()->GetBuffer() ),
             m_IndexAddress );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( input );
        build.flags                                       = kSkinnedBlasFlags;
        build.mode                      = m_Built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                                  : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.srcAccelerationStructure  = m_Built ? m_Handle : VK_NULL_HANDLE;
        build.dstAccelerationStructure  = m_Handle;
        build.scratchData.deviceAddress = AlignScratchAddress(
             functions.BufferAddress( device, scratchBuffer.GetValue()->GetBuffer() ) + scratchOffset );

        const VkAccelerationStructureBuildRangeInfoKHR* ranges = input.Ranges.data();
        functions.CmdBuildAccelerationStructures( command.GetValue(), 1, &build, &ranges );
        m_Built = true;
        return BOOLSUCCESS;
    }

    void VulkanSkinnedRayTracingGeometry::Release()
    {
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        // All on the frame ring: a frame in flight may still trace this BLAS through a TLAS instance.
        if ( m_Handle != VK_NULL_HANDLE )
            allocator->RT_DestroyAccelerationStructure( m_Handle, m_Destroy );
        if ( m_Storage != VK_NULL_HANDLE )
            allocator->RT_DestroyBuffer( m_Storage, m_StorageMemory );
        if ( m_Positions != VK_NULL_HANDLE )
            allocator->RT_DestroyBuffer( m_Positions, m_PositionsMemory );
        m_Handle            = VK_NULL_HANDLE;
        m_Storage           = VK_NULL_HANDLE;
        m_StorageMemory     = nullptr;
        m_StorageBytes      = 0;
        m_DeviceAddress     = 0;
        m_StorageLedger     = ResourceOwnership{};
        m_StorageExternal   = RDG::ExternalBuffer{};
        m_Positions         = VK_NULL_HANDLE;
        m_PositionsMemory   = nullptr;
        m_PositionsBytes    = 0;
        m_PositionsLedger   = ResourceOwnership{};
        m_PositionsExternal = RDG::ExternalBuffer{};
        m_SourceVertices.reset();
        m_Indices.reset();
        m_Segments.clear();
        m_VertexCount     = 0;
        m_IndexAddress    = 0;
        m_BuildScratch    = 0;
        m_UpdateScratch   = 0;
        m_Built           = false;
    }

    VulkanSkinnedRayTracingGeometry::~VulkanSkinnedRayTracingGeometry()
    try
    {
        Release();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanSkinnedRayTracingGeometry" )
} // namespace Desert::Graphic::API::Vulkan

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<SkinnedRayTracingGeometry> CreateSkinnedRayTracingGeometry()
    {
        return std::make_unique<API::Vulkan::VulkanSkinnedRayTracingGeometry>();
    }
} // namespace Desert::Graphic::RayTracing
