#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSkinnedRayTracingGeometry.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
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

    Common::ResultStr<bool> VulkanSkinCacheStreams::Prepare( const uint32_t vertexCount )
    {
        if ( m_Buffer != VK_NULL_HANDLE && vertexCount == m_VertexCount )
            return Common::MakeSuccess( false );
        if ( vertexCount == 0 )
            return Common::MakeError<bool>( "SkinCache: the skinned mesh has no vertex" );
        const auto logical = LogicalDevice();
        if ( !logical )
            return Common::MakeError<bool>( "SkinCache: no Vulkan device" );
        if ( !logical->GetPhysicalDevice()->GetDeviceCaps().Has( Capability::BufferDeviceAddress ) )
            return Common::MakeError<bool>(
                 "SkinCache: the device has no buffer device addresses (the view passes fetch the streams by "
                 "address)" );

        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( functions.GetBufferDeviceAddress == nullptr )
            return Common::MakeError<bool>( "SkinCache: vkGetBufferDeviceAddress was not resolved on this device" );

        Release();
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        m_Bytes = static_cast<VkDeviceSize>( vertexCount ) * RayTracing::kSkinCacheFloatsPerVertex * sizeof( float );
        // The BLAS-input usage only where the device enabled VK_KHR_acceleration_structure (a usage bit of an
        // extension not enabled is invalid); the raster reads the streams as a storage buffer either way.
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if ( logical->GetPhysicalDevice()->GetDeviceCaps().Has( Capability::AccelerationStructure ) )
            usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        const auto allocated = allocator->RT_AllocateBuffer(
             "SkinCache_Streams",
             DeviceAddressBufferInfo( m_Bytes, usage ),
             VMA_MEMORY_USAGE_GPU_ONLY, m_Buffer );
        if ( !allocated.IsSuccess() )
        {
            m_Buffer = VK_NULL_HANDLE;
            Release();
            return Common::MakeFormattedError<bool>( "SkinCache streams ({} bytes): {}", m_Bytes, allocated.GetError() );
        }
        m_Memory = allocated.GetValue();
        m_Ledger = ResourceOwnership::Take( ResourceKind::StorageBuffer, VulkanAllocator::AllocationSize( m_Memory ) );

        // The device's loaded table (VulkanRayTracingFunctions.hpp), as every other address in the engine; it
        // holds the buffer-address entry on a device without acceleration structures too.
        m_Address = functions.BufferAddress( logical->GetVulkanLogicalDevice(), m_Buffer );
        if ( m_Address == 0 )
        {
            Release();
            return Common::MakeError<bool>( "SkinCache: vkGetBufferDeviceAddress answered 0 for the streams" );
        }
        m_VertexCount = vertexCount;

        // Starts untouched; each graph hands back the state it leaves it in.
        m_External                  = RDG::ExternalBuffer( RDG::BufferDesc{ m_Bytes }, RDG::Access::None );
        m_External.Physical         = VulkanRdgBuffer::Wrap( m_Buffer, m_Bytes );
        m_External.RecordFinalState = [this]( const RDG::AccessState& state ) -> Common::BoolResultStr
        {
            m_External.State = state;
            return BOOLSUCCESS;
        };
        return Common::MakeSuccess( true );
    }

    void VulkanSkinCacheStreams::Release()
    {
        // On the frame ring: a frame in flight may still draw from or trace these positions.
        if ( m_Buffer != VK_NULL_HANDLE )
            SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                 ->GetVulkanAllocator()
                 ->RT_DestroyBuffer( m_Buffer, m_Memory );
        m_Buffer      = VK_NULL_HANDLE;
        m_Memory      = nullptr;
        m_Bytes       = 0;
        m_Address     = 0;
        m_VertexCount = 0;
        m_Ledger      = ResourceOwnership{};
        m_External    = RDG::ExternalBuffer{};
    }

    VulkanSkinCacheStreams::~VulkanSkinCacheStreams()
    try
    {
        Release();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanSkinCacheStreams" )

    Common::ResultStr<Prepared> VulkanSkinnedRayTracingGeometry::Prepare( const SkinnedMesh& mesh,
                                                                          const uint64_t     hiddenSubmeshes )
    {
        const auto logical = LogicalDevice();
        if ( !logical )
            return Common::MakeError<Prepared>( "SkinCache: no Vulkan device" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( !functions.Complete() )
            return Common::MakeError<Prepared>(
                 "SkinCache: the device has no acceleration-structure functions, but "
                 "a resolved method traces rays" );

        const auto vertices     = mesh.GetVertexBuffer();
        const auto indices      = mesh.GetIndexBuffer();
        const auto vertexBuffer = SP_CAST( VulkanVertexBuffer, vertices );
        const auto indexBuffer  = SP_CAST( VulkanIndexBuffer, indices );
        if ( !vertexBuffer || !indexBuffer || vertexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE ||
             indexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE )
            return Common::MakeError<Prepared>(
                 "SkinCache: the skinned mesh has no resident vertex/index buffer" );

        const uint32_t vertexCount = mesh.GetVertexCount();
        if ( m_Handle != VK_NULL_HANDLE && vertices == m_SourceVertices && indices == m_Indices &&
             hiddenSubmeshes == m_HiddenSubmeshes && vertexCount == m_VertexCount )
            return Common::MakeSuccess( Prepared{ ( m_Built ? m_UpdateScratch : m_BuildScratch ) +
                                                       kMaxAccelerationStructureScratchAlignment,
                                                  m_Built } );

        Release();
        m_Segments = RayTracing::DescribeDeformedMeshGeometry( mesh.GetSubmeshes(), hiddenSubmeshes );
        if ( m_Segments.empty() || vertexCount == 0 )
            return Common::MakeFormattedError<Prepared>(
                 "SkinCache: the skinned mesh has no visible triangle ({} vertices, hidden mask {:#x})",
                 vertexCount, hiddenSubmeshes );

        const VkDevice device = logical->GetVulkanLogicalDevice();
        const auto&    allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();

        m_IndexAddress = functions.BufferAddress( device, indexBuffer->GetVulkanBuffer() );
        const VulkanBlasBuildInput input =
             TranslateBlasGeometry( m_Segments, 0, m_IndexAddress ); // sizes ignore addresses; RecordBuild passes the streams'
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
        m_HiddenSubmeshes = hiddenSubmeshes;
        m_VertexCount     = vertexCount;

        // Starts untouched; each graph hands back the state it leaves it in.
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
        m_Handle            = VK_NULL_HANDLE;
        m_Storage           = VK_NULL_HANDLE;
        m_StorageMemory     = nullptr;
        m_StorageBytes      = 0;
        m_DeviceAddress     = 0;
        m_StorageLedger     = ResourceOwnership{};
        m_StorageExternal   = RDG::ExternalBuffer{};
        m_SourceVertices.reset();
        m_Indices.reset();
        m_Segments.clear();
        m_HiddenSubmeshes = 0;
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
