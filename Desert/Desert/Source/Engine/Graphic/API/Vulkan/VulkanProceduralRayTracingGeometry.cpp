#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanProceduralRayTracingGeometry.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        using Prepared = RayTracing::ProceduralBlasPrepared;

        // Built once per upload and traced for many frames: trace speed over build speed (UE static geometry).
        constexpr VkBuildAccelerationStructureFlagsKHR kProceduralBlasFlags =
             VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

        std::shared_ptr<VulkanLogicalDevice> LogicalDevice()
        {
            return SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        }
    } // namespace

    Common::ResultStr<Prepared> VulkanProceduralRayTracingGeometry::Prepare( const Mesh&    mesh,
                                                                             const uint64_t hiddenSubmeshes )
    {
        const auto logical = LogicalDevice();
        if ( !logical )
            return Common::MakeError<Prepared>( "procedural BLAS: no Vulkan device" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( !functions.Complete() )
            return Common::MakeError<Prepared>(
                 "procedural BLAS: the device has no acceleration-structure functions, but a resolved method "
                 "traces rays" );

        const auto vertices     = mesh.GetVertexBuffer();
        const auto indices      = mesh.GetIndexBuffer();
        const auto vertexBuffer = SP_CAST( VulkanVertexBuffer, vertices );
        const auto indexBuffer  = SP_CAST( VulkanIndexBuffer, indices );
        if ( !vertexBuffer || !indexBuffer || vertexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE ||
             indexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE )
            return Common::MakeError<Prepared>( "procedural BLAS: the mesh has no resident vertex/index buffer" );

        if ( m_Handle != VK_NULL_HANDLE && vertices == m_SourceVertices && indices == m_Indices &&
             hiddenSubmeshes == m_HiddenSubmeshes )
            return Common::MakeSuccess(
                 Prepared{ m_BuildScratch + kMaxAccelerationStructureScratchAlignment, !m_Built } );

        Release();
        m_Segments = RayTracing::DescribeStaticMeshGeometry( mesh.GetSubmeshes(), hiddenSubmeshes );
        if ( m_Segments.empty() )
            return Common::MakeFormattedError<Prepared>(
                 "procedural BLAS: the mesh has no visible triangle (hidden mask {:#x})", hiddenSubmeshes );

        const VkDevice device = logical->GetVulkanLogicalDevice();
        const auto&    allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();

        m_IndexAddress                   = functions.BufferAddress( device, indexBuffer->GetVulkanBuffer() );
        m_VertexAddress = functions.BufferAddress( device, vertexBuffer->GetVulkanBuffer() );
        const VulkanBlasBuildInput input = TranslateBlasGeometry(
             m_Segments, functions.BufferAddress( device, vertexBuffer->GetVulkanBuffer() ), m_IndexAddress );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( input );
        build.flags                                       = kProceduralBlasFlags;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                      &build, input.MaxPrimitiveCounts.data(), &sizes );
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
            return Common::MakeFormattedError<Prepared>( "procedural BLAS storage: {}", storage.GetError() );
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
                 "vkCreateAccelerationStructureKHR (procedural BLAS) failed: {}", static_cast<int>( created ) );
        }
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addressInfo.accelerationStructure = m_Handle;
        m_DeviceAddress = functions.GetAccelerationStructureDeviceAddress( device, &addressInfo );
        if ( m_DeviceAddress == 0 )
        {
            Release();
            return Common::MakeError<Prepared>(
                 "vkGetAccelerationStructureDeviceAddressKHR answered 0 for a procedural BLAS" );
        }

        m_SourceVertices  = vertices;
        m_Indices         = indices;
        m_HiddenSubmeshes = hiddenSubmeshes;

        m_StorageExternal = RDG::ExternalBuffer( RDG::BufferDesc{ m_StorageBytes }, RDG::Access::None );
        m_StorageExternal.Physical =
             VulkanRdgBuffer::WrapAccelerationStructure( m_Storage, m_StorageBytes, m_Handle );
        m_StorageExternal.RecordFinalState = [this]( const RDG::AccessState& ) -> Common::BoolResultStr
        {
            // Its last readers are the ray queries traversing it through the TLAS (they declare the TLAS only).
            m_StorageExternal.State = RDG::GetAccessState( RDG::Access::AccelStructRead );
            return BOOLSUCCESS;
        };
        return Common::MakeSuccess( Prepared{ m_BuildScratch + kMaxAccelerationStructureScratchAlignment, true } );
    }

    Common::BoolResultStr VulkanProceduralRayTracingGeometry::RecordBuild( const RDG::PassContext&   context,
                                                                           const RDG::BufferBinding& vertices,
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
        const auto logical = LogicalDevice();
        if ( !logical || m_Handle == VK_NULL_HANDLE )
            return Common::MakeError( "procedural BLAS build recorded without a prepared acceleration structure" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        const VkDevice                   device    = logical->GetVulkanLogicalDevice();

        const VulkanBlasBuildInput input = TranslateBlasGeometry(
             m_Segments, functions.BufferAddress( device, vertexBuffer.GetValue()->GetBuffer() ), m_IndexAddress );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( input );
        build.flags                                       = kProceduralBlasFlags;
        build.mode                                        = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.dstAccelerationStructure                    = m_Handle;
        build.scratchData.deviceAddress                   = AlignScratchAddress(
             functions.BufferAddress( device, scratchBuffer.GetValue()->GetBuffer() ) + scratchOffset );

        const VkAccelerationStructureBuildRangeInfoKHR* ranges = input.Ranges.data();
        functions.CmdBuildAccelerationStructures( command.GetValue(), 1, &build, &ranges );
        m_Built = true;
        return BOOLSUCCESS;
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
        m_SourceVertices.reset();
        m_Indices.reset();
        m_Segments.clear();
        m_HiddenSubmeshes = 0;
        m_IndexAddress    = 0;
        m_VertexAddress   = 0;
        m_BuildScratch    = 0;
        m_Built           = false;
    }

    VulkanProceduralRayTracingGeometry::~VulkanProceduralRayTracingGeometry()
    try
    {
        Release();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanProceduralRayTracingGeometry" )
} // namespace Desert::Graphic::API::Vulkan
