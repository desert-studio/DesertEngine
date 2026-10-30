#include <Common/Core/Core.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanProceduralRayTracingGeometry.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGeometryAddress.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // Built once per upload and traced for many frames: trace speed over build speed (UE static geometry).
        constexpr VkBuildAccelerationStructureFlagsKHR kProceduralBlasFlags =
             VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    } // namespace

    Common::ResultStr<RayTracing::ProceduralBlasPrepared>
    VulkanProceduralRayTracingGeometry::Prepare( const RayTracing::RayTracingMeshInput& input )
    {
        const auto logical = VulkanLogicalDevice::Current();
        if ( !logical )
            return Common::MakeError<RayTracing::ProceduralBlasPrepared>( "procedural BLAS: no Vulkan device" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( !functions.Complete() )
            return Common::MakeError<RayTracing::ProceduralBlasPrepared>(
                 "procedural BLAS: the device has no acceleration-structure functions, but a resolved method "
                 "traces rays" );

        const auto& vertices     = input.Vertices;
        const auto& indices      = input.Indices;
        const auto  vertexBuffer = SP_CAST( VulkanVertexBuffer, vertices );
        const auto  indexBuffer  = SP_CAST( VulkanIndexBuffer, indices );
        if ( !vertexBuffer || !indexBuffer || vertexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE ||
             indexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE )
            return Common::MakeError<RayTracing::ProceduralBlasPrepared>(
                 "procedural BLAS: the mesh has no resident vertex/index buffer" );

        const VertexBufferRange vertexRange{ vertices, input.VertexByteOffset };
        const IndexBufferRange  indexRange{ indices, input.IndexByteOffset };
        if ( m_Handle != VK_NULL_HANDLE && vertexRange == m_Vertices && indexRange == m_Indices &&
             input.Segments == m_Segments )
            return Common::MakeSuccess( RayTracing::ProceduralBlasPrepared{
                 m_BuildScratch + kMaxAccelerationStructureScratchAlignment, !m_Built } );

        Release();
        if ( input.Segments.empty() )
            return Common::MakeError<RayTracing::ProceduralBlasPrepared>(
                 "procedural BLAS: the mesh has no visible triangle" );
        m_Segments = input.Segments;

        const VkDevice device = logical->GetVulkanLogicalDevice();
        const auto&    allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();

        // The sizes do not depend on the addresses; the build reads them again when it records.
        const VulkanBlasBuildInput buildInput = TranslateBlasGeometry(
             m_Segments, CurrentGeometryAddress( vertexRange ), CurrentGeometryAddress( indexRange ) );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( buildInput );
        build.flags                                       = kProceduralBlasFlags;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                      &build, buildInput.MaxPrimitiveCounts.data(), &sizes );
        m_BuildScratch = sizes.buildScratchSize;

        const auto storage = allocator->RT_AllocateBuffer(
             "Procedural_BLAS",
             DeviceAddressBufferInfo( sizes.accelerationStructureSize,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR ),
             VMA_MEMORY_USAGE_GPU_ONLY, m_Storage );
        if ( !storage.IsSuccess() )
        {
            m_Storage = VK_NULL_HANDLE;
            Release();
            return Common::MakeFormattedError<RayTracing::ProceduralBlasPrepared>( "procedural BLAS storage: {}",
                                                                                   storage.GetError() );
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
            return Common::MakeFormattedError<RayTracing::ProceduralBlasPrepared>(
                 "vkCreateAccelerationStructureKHR (procedural BLAS) failed: {}", static_cast<int>( created ) );
        }
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addressInfo.accelerationStructure = m_Handle;
        m_DeviceAddress = functions.GetAccelerationStructureDeviceAddress( device, &addressInfo );
        if ( m_DeviceAddress == 0 )
        {
            Release();
            return Common::MakeError<RayTracing::ProceduralBlasPrepared>(
                 "vkGetAccelerationStructureDeviceAddressKHR answered 0 for a procedural BLAS" );
        }

        m_Vertices = vertexRange;
        m_Indices  = indexRange;

        m_StorageExternal = RDG::ExternalBuffer( RDG::BufferDesc{ m_StorageBytes }, RDG::Access::None );
        m_StorageExternal.Physical =
             VulkanRdgBuffer::WrapAccelerationStructure( m_Storage, m_StorageBytes, m_Handle );
        m_StorageExternal.RecordFinalState = [this]( const RDG::AccessState& ) -> Common::BoolResultStr
        {
            // Its last readers are the ray queries traversing it through the TLAS (they declare the TLAS only).
            m_StorageExternal.State = RDG::GetAccessState( RDG::Access::AccelStructRead );
            return BOOLSUCCESS;
        };
        return Common::MakeSuccess( RayTracing::ProceduralBlasPrepared{
             m_BuildScratch + kMaxAccelerationStructureScratchAlignment, true } );
    }

    Common::BoolResultStr VulkanProceduralRayTracingGeometry::RecordBuild( const RDG::PassContext&   context,
                                                                           const RDG::BufferBinding& vertices,
                                                                           const RDG::BufferBinding* /*indices*/,
                                                                           const RDG::BufferBinding& scratch,
                                                                           const uint64_t scratchOffset )
    {
        const auto command = VulkanRdgBackend::CommandBufferOf( context );
        if ( !command )
            return Common::MakeError( command.GetError() );
        const auto vertexBuffer = VulkanRdgBackend::BufferOf( vertices );
        if ( !vertexBuffer )
            return Common::MakeError( vertexBuffer.GetError() );
        const auto scratchBuffer = VulkanRdgBackend::BufferOf( scratch );
        if ( !scratchBuffer )
            return Common::MakeError( scratchBuffer.GetError() );
        const auto logical = VulkanLogicalDevice::Current();
        if ( !logical || m_Handle == VK_NULL_HANDLE )
            return Common::MakeError( "procedural BLAS build recorded without a prepared acceleration structure" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        const VkDevice                   device    = logical->GetVulkanLogicalDevice();

        const VulkanBlasBuildInput input = TranslateBlasGeometry(
             m_Segments,
             functions.BufferAddress( device, vertexBuffer.GetValue()->GetBuffer() ) +
                  vertexBuffer.GetValue()->GetOffset(),
             CurrentGeometryAddress( m_Indices ) );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( input );
        build.flags                                       = kProceduralBlasFlags;
        build.mode                                        = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.dstAccelerationStructure                    = m_Handle;
        build.scratchData.deviceAddress                   = AlignScratchAddress(
             ( functions.BufferAddress( device, scratchBuffer.GetValue()->GetBuffer() ) + scratchBuffer.GetValue()->GetOffset() ) + scratchOffset );

        const VkAccelerationStructureBuildRangeInfoKHR* ranges = input.Ranges.data();
        functions.CmdBuildAccelerationStructures( command.GetValue(), 1, &build, &ranges );
        m_Built = true;
        return BOOLSUCCESS;
    }

    RayTracing::RayTracingGeometryLayout VulkanProceduralRayTracingGeometry::GetMetadataLayout() const
    {
        return RayTracing::MeshAttributeLayout( CurrentGeometryAddress( m_Vertices ),
                                                CurrentGeometryAddress( m_Indices ), m_Segments, false );
    }

    void VulkanProceduralRayTracingGeometry::Release()
    {
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        // On the frame ring: a frame in flight may still trace this BLAS through a TLAS instance.
        if ( m_Handle != VK_NULL_HANDLE )
            allocator->RT_DestroyAccelerationStructure( m_Handle, m_Destroy );
        if ( m_Storage != VK_NULL_HANDLE )
            allocator->RT_DestroyBuffer( m_Storage, m_StorageMemory );
        m_Handle          = VK_NULL_HANDLE;
        m_Storage         = VK_NULL_HANDLE;
        m_StorageMemory   = nullptr;
        m_StorageBytes    = 0;
        m_DeviceAddress   = 0;
        m_StorageLedger   = ResourceOwnership{};
        m_StorageExternal = RDG::ExternalBuffer{};
        m_Vertices = VertexBufferRange{};
        m_Indices  = IndexBufferRange{};
        m_Segments.clear();
        m_BuildScratch = 0;
        m_Built         = false;
    }

    VulkanProceduralRayTracingGeometry::~VulkanProceduralRayTracingGeometry()
    try
    {
        Release();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanProceduralRayTracingGeometry" )
} // namespace Desert::Graphic::API::Vulkan

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<ProceduralRayTracingGeometry> CreateProceduralRayTracingGeometry()
    {
        return std::make_unique<API::Vulkan::VulkanProceduralRayTracingGeometry>();
    }
} // namespace Desert::Graphic::RayTracing
