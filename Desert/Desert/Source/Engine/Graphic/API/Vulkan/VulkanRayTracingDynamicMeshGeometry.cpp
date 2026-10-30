// The Vulkan side of a dynamic BLAS that owns its build inputs (RayTracingDynamicMeshGeometryTarget,
// RayTracingDynamicGeometry.hpp): a persistent vertex buffer (Desert::Vertex) and index buffer (uint32) a
// producer's compute writes in the frame graph, and the BLAS over them - one triangle geometry per segment, OPAQUE
// or alpha-tested (VulkanRayTracingBuildInput.hpp maps the segments). Built with ALLOW_UPDATE so a change of the
// vertices alone refits in place; every object is freed on the frame ring and has its own ledger row. The build is
// recorded by RayTracingScene.DynamicGeometry like every other dynamic BLAS.
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <format>
#include <string>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        VkDeviceAddress BufferAddress( const RayTracingDeviceFunctions& functions, VkDevice device,
                                       VkBuffer buffer )
        {
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = buffer;
            return functions.GetBufferDeviceAddress( device, &info );
        }

        // The build of a dynamic mesh BLAS: traced every frame, refit when only its vertices moved.
        VkAccelerationStructureBuildGeometryInfoKHR DynamicMeshBuildInfo( const VulkanBlasBuildInput& input,
                                                                          const bool                  update )
        {
            VkAccelerationStructureBuildGeometryInfoKHR info = MakeBlasBuildInfo( input );
            info.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                               : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            return info;
        }

        struct OwnedBuffer
        {
            VkBuffer          Buffer = VK_NULL_HANDLE;
            VmaAllocation     Memory = nullptr;
            VkDeviceSize      Bytes  = 0;
            ResourceOwnership Ledger;
        };
    } // namespace

    class VulkanRayTracingDynamicMeshGeometry final : public RayTracing::RayTracingDynamicMeshGeometryTarget
    {
    public:
        explicit VulkanRayTracingDynamicMeshGeometry( std::string_view debugName ) : m_Name( debugName )
        {
        }
        ~VulkanRayTracingDynamicMeshGeometry() override
        try
        {
            Release();
        }
        DESERT_DESTRUCTOR_GUARD( "~VulkanRayTracingDynamicMeshGeometry" )
        VulkanRayTracingDynamicMeshGeometry( const VulkanRayTracingDynamicMeshGeometry& )            = delete;
        VulkanRayTracingDynamicMeshGeometry& operator=( const VulkanRayTracingDynamicMeshGeometry& ) = delete;

        [[nodiscard]] uint64_t GetDeviceAddress() const override
        {
            return m_DeviceAddress;
        }

        [[nodiscard]] Common::ResultStr<RayTracing::RayTracingDynamicGeometryPlan>
        Prepare( const RayTracing::RayTracingDynamicMeshLayout& layout, const bool update ) override
        {
            using Plan = RayTracing::RayTracingDynamicGeometryPlan;
            if ( layout.Segments.empty() || layout.VertexBytes == 0 || layout.IndexBytes == 0 )
                return Common::MakeFormattedError<Plan>( "{} BLAS: a dynamic mesh layout with no geometry",
                                                         m_Name );
            const auto logical = VulkanLogicalDevice::Current();
            if ( !logical )
                return Common::MakeFormattedError<Plan>( "{} BLAS: no Vulkan device", m_Name );
            const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
            if ( !functions.Complete() )
                return Common::MakeFormattedError<Plan>( "{} BLAS: the device has no acceleration-structure "
                                                         "functions, but a resolved method traces rays",
                                                         m_Name );
            const VkDevice device = logical->GetVulkanLogicalDevice();

            const VulkanBlasBuildInput input = TranslateBlasGeometry( layout.Segments, 0, 0 );
            const VkAccelerationStructureBuildGeometryInfoKHR build = DynamicMeshBuildInfo( input, false );
            VkAccelerationStructureBuildSizesInfoKHR          sizes{
                 VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                          &build, input.MaxPrimitiveCounts.data(), &sizes );

            const bool recreate = m_Handle == VK_NULL_HANDLE || !( layout == m_Layout );
            if ( recreate )
            {
                Release();
                const VkBufferUsageFlags inputUsage =
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                if ( auto made = Allocate( std::format( "{} RT vertices", m_Name ), layout.VertexBytes, inputUsage,
                                           ResourceKind::StorageBuffer, m_Vertices );
                     !made )
                    return Common::MakeError<Plan>( made.GetError() );
                if ( auto made = Allocate( std::format( "{} RT indices", m_Name ), layout.IndexBytes, inputUsage,
                                           ResourceKind::StorageBuffer, m_Indices );
                     !made )
                    return Common::MakeError<Plan>( made.GetError() );
                if ( auto made = Allocate( std::format( "{} BLAS", m_Name ), sizes.accelerationStructureSize,
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                           ResourceKind::BottomLevelAccelerationStructure, m_Storage );
                     !made )
                    return Common::MakeError<Plan>( made.GetError() );
                m_Destroy = functions.DestroyAccelerationStructure;

                VkAccelerationStructureCreateInfoKHR create{
                     VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
                create.buffer = m_Storage.Buffer;
                create.size   = sizes.accelerationStructureSize;
                create.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                if ( const VkResult created =
                          functions.CreateAccelerationStructure( device, &create, nullptr, &m_Handle );
                     created != VK_SUCCESS )
                {
                    m_Handle = VK_NULL_HANDLE;
                    Release();
                    return Common::MakeFormattedError<Plan>(
                         "vkCreateAccelerationStructureKHR ({} BLAS, {} geometries) failed: {}", m_Name,
                         layout.Segments.size(), static_cast<int>( created ) );
                }
                VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
                     VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
                addressInfo.accelerationStructure = m_Handle;
                m_DeviceAddress = functions.GetAccelerationStructureDeviceAddress( device, &addressInfo );
                if ( m_DeviceAddress == 0 )
                {
                    Release();
                    return Common::MakeFormattedError<Plan>(
                         "vkGetAccelerationStructureDeviceAddressKHR answered 0 for a {} BLAS", m_Name );
                }
                m_Layout        = layout;
                m_VertexAddress = BufferAddress( functions, device, m_Vertices.Buffer );
                m_IndexAddress  = BufferAddress( functions, device, m_Indices.Buffer );

                MakeExternal( m_VerticesExternal, m_Vertices, VK_NULL_HANDLE );
                MakeExternal( m_IndicesExternal, m_Indices, VK_NULL_HANDLE );
                MakeExternal( m_StorageExternal, m_Storage, m_Handle );
            }
            m_Update = update && !recreate;
            return Common::MakeSuccess(
                 Plan{ .ScratchBytes = ( m_Update ? sizes.updateScratchSize : sizes.buildScratchSize ) +
                                       kMaxAccelerationStructureScratchAlignment,
                       .Update = m_Update } );
        }

        [[nodiscard]] RDG::ExternalBuffer& GetVertices() override
        {
            return m_VerticesExternal;
        }
        [[nodiscard]] RDG::ExternalBuffer& GetIndices() override
        {
            return m_IndicesExternal;
        }
        [[nodiscard]] RDG::ExternalBuffer& GetStorage() override
        {
            return m_StorageExternal;
        }

        [[nodiscard]] Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                         const RDG::BufferBinding& vertices,
                                                         const RDG::BufferBinding* indices,
                                                         const RDG::BufferBinding& scratch,
                                                         const uint64_t            scratchOffset ) override
        {
            if ( indices == nullptr )
                return Common::MakeFormattedError<bool>(
                     "{} BLAS: the build request did not name the index buffer it owns", m_Name );
            const auto command = VulkanRdgBackend::CommandBufferOf( context );
            if ( !command )
                return Common::MakeError( command.GetError() );
            const auto vertexBuffer = VulkanRdgBackend::BufferOf( vertices );
            if ( !vertexBuffer )
                return Common::MakeError( vertexBuffer.GetError() );
            const auto indexBuffer = VulkanRdgBackend::BufferOf( *indices );
            if ( !indexBuffer )
                return Common::MakeError( indexBuffer.GetError() );
            const auto scratchBuffer = VulkanRdgBackend::BufferOf( scratch );
            if ( !scratchBuffer )
                return Common::MakeError( scratchBuffer.GetError() );
            const auto logical = VulkanLogicalDevice::Current();
            if ( !logical || m_Handle == VK_NULL_HANDLE )
                return Common::MakeFormattedError<bool>(
                     "{} BLAS build recorded without a prepared acceleration structure", m_Name );
            const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
            const VkDevice                   device    = logical->GetVulkanLogicalDevice();

            const VulkanBlasBuildInput input = TranslateBlasGeometry(
                 m_Layout.Segments, ( BufferAddress( functions, device, vertexBuffer.GetValue()->GetBuffer() ) + vertexBuffer.GetValue()->GetOffset() ),
                 ( BufferAddress( functions, device, indexBuffer.GetValue()->GetBuffer() ) + indexBuffer.GetValue()->GetOffset() ) );
            VkAccelerationStructureBuildGeometryInfoKHR build = DynamicMeshBuildInfo( input, m_Update );
            build.srcAccelerationStructure                    = m_Update ? m_Handle : VK_NULL_HANDLE;
            build.dstAccelerationStructure                    = m_Handle;
            build.scratchData.deviceAddress                   = AlignScratchAddress(
                 ( BufferAddress( functions, device, scratchBuffer.GetValue()->GetBuffer() ) + scratchBuffer.GetValue()->GetOffset() ) + scratchOffset );

            const VkAccelerationStructureBuildRangeInfoKHR* rangeList = input.Ranges.data();
            functions.CmdBuildAccelerationStructures( command.GetValue(), 1, &build, &rangeList );
            return BOOLSUCCESS;
        }

        // The tile's own Desert::Vertex buffer is both the BLAS input and the hit's attribute stream.
        [[nodiscard]] RayTracing::RayTracingGeometryLayout GetMetadataLayout() const override
        {
            return RayTracing::MeshAttributeLayout( m_VertexAddress, m_IndexAddress, m_Layout.Segments, false );
        }
        // Both read the buffers' ledger rows: the one statement of what the tile holds on the device.
        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_Storage.Ledger.GetBytes();
        }
        [[nodiscard]] uint64_t GetBuildInputBytes() const override
        {
            return m_Vertices.Ledger.GetBytes() + m_Indices.Ledger.GetBytes();
        }

    private:
        static Common::BoolResultStr Allocate( const std::string& name, VkDeviceSize bytes,
                                               VkBufferUsageFlags usage, ResourceKind kind, OwnedBuffer& out )
        {
            const auto& allocator =
                 SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size        = bytes;
            info.usage       = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            const auto memory =
                 allocator->RT_AllocateBuffer( name.c_str(), info, VMA_MEMORY_USAGE_GPU_ONLY, out.Buffer );
            if ( !memory.IsSuccess() )
                return Common::MakeFormattedError<bool>( "{} ({} bytes): {}", name, bytes, memory.GetError() );
            out.Memory = memory.GetValue();
            out.Bytes  = bytes;
            out.Ledger = kind == ResourceKind::BottomLevelAccelerationStructure
                              ? RayTracing::RecordAccelerationStructureMemory(
                                     RayTracing::AccelerationStructureLevel::Bottom,
                                     VulkanAllocator::AllocationSize( out.Memory ) )
                              : ResourceOwnership::Take( kind, VulkanAllocator::AllocationSize( out.Memory ) );
            return BOOLSUCCESS;
        }

        // A new buffer starts untouched; each graph hands back the state it leaves the buffer in.
        static void MakeExternal( RDG::ExternalBuffer& external, const OwnedBuffer& buffer,
                                  VkAccelerationStructureKHR accelerationStructure )
        {
            external                  = RDG::ExternalBuffer( RDG::BufferDesc{ buffer.Bytes }, RDG::Access::None );
            external.Physical         = accelerationStructure != VK_NULL_HANDLE
                                             ? VulkanRdgBuffer::WrapAccelerationStructure( buffer.Buffer, buffer.Bytes,
                                                                                           accelerationStructure )
                                             : VulkanRdgBuffer::Wrap( buffer.Buffer, buffer.Bytes );
            external.RecordFinalState = [&external]( const RDG::AccessState& state ) -> Common::BoolResultStr
            {
                external.State = state;
                return BOOLSUCCESS;
            };
        }

        void Release()
        {
            const auto& allocator =
                 SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
            if ( m_Handle != VK_NULL_HANDLE )
                allocator->RT_DestroyAccelerationStructure( m_Handle, m_Destroy );
            for ( OwnedBuffer* buffer : { &m_Vertices, &m_Indices, &m_Storage } )
            {
                if ( buffer->Buffer != VK_NULL_HANDLE )
                    allocator->RT_DestroyBuffer( buffer->Buffer, buffer->Memory );
                *buffer = OwnedBuffer{};
            }
            m_Handle           = VK_NULL_HANDLE;
            m_DeviceAddress    = 0;
            m_VertexAddress    = 0;
            m_IndexAddress     = 0;
            m_Update           = false;
            m_Layout           = {};
            m_VerticesExternal = RDG::ExternalBuffer{};
            m_IndicesExternal  = RDG::ExternalBuffer{};
            m_StorageExternal  = RDG::ExternalBuffer{};
        }

        std::string                           m_Name;
        VkAccelerationStructureKHR            m_Handle        = VK_NULL_HANDLE;
        PFN_vkDestroyAccelerationStructureKHR m_Destroy       = nullptr;
        uint64_t                              m_DeviceAddress = 0;
        uint64_t m_VertexAddress = 0;     // m_Vertices' device address (GetMetadataLayout)
        uint64_t m_IndexAddress  = 0;     // m_Indices' device address
        bool     m_Update        = false; // the last Prepare's plan
        RayTracing::RayTracingDynamicMeshLayout m_Layout;
        OwnedBuffer                             m_Vertices;
        OwnedBuffer                             m_Indices;
        OwnedBuffer                             m_Storage;
        RDG::ExternalBuffer                     m_VerticesExternal;
        RDG::ExternalBuffer                     m_IndicesExternal;
        RDG::ExternalBuffer                     m_StorageExternal;
    };
} // namespace Desert::Graphic::API::Vulkan

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<RayTracingDynamicMeshGeometryTarget>
    CreateRayTracingDynamicMeshGeometry( std::string_view debugName )
    {
        return std::make_unique<API::Vulkan::VulkanRayTracingDynamicMeshGeometry>( debugName );
    }
} // namespace Desert::Graphic::RayTracing
