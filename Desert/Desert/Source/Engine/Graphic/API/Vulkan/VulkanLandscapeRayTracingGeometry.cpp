// The Vulkan side of one landscape tile's ray-tracing geometry (LandscapeRayTracing.hpp): a persistent vertex
// buffer (Desert::Vertex, the LOD grid plus the NaN vertex), a persistent index buffer (the opaque range, then the
// masked range) and the tile's BLAS of two triangle geometries over them - OPAQUE and non-opaque (the alpha test
// reads the visibility layer). Built with ALLOW_UPDATE so a LOD-only change refits in place; every object is freed
// on the frame ring and has its own ledger row.
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/LandscapeRayTracing.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        std::shared_ptr<VulkanLogicalDevice> LogicalDevice()
        {
            return SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        }

        VkDeviceAddress BufferAddress( const RayTracingDeviceFunctions& functions, VkDevice device,
                                       VkBuffer buffer )
        {
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = buffer;
            return functions.GetBufferDeviceAddress( device, &info );
        }

        // Geometry 0 = opaque, 1 = masked; both index the same vertices, each over its own index range.
        std::array<VkAccelerationStructureGeometryKHR, 2>
        TileGeometries( VkDeviceAddress vertices, VkDeviceAddress indices,
                        const RayTracing::LandscapeRayTracingGrid& grid )
        {
            std::array<VkAccelerationStructureGeometryKHR, 2> geometries{};
            for ( uint32_t g = 0; g < 2; ++g )
            {
                VkAccelerationStructureGeometryKHR& geometry = geometries[g];
                geometry.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                // The masked geometry's candidate hits go to the alpha test; each hole is tested once.
                geometry.flags =
                     g == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
                VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
                triangles.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                triangles.vertexData.deviceAddress = vertices == 0 ? 0 : vertices + offsetof( Vertex, Position );
                triangles.vertexStride             = sizeof( Vertex );
                triangles.maxVertex                = grid.GridVertices; // the NaN vertex is the last one
                triangles.indexType                = VK_INDEX_TYPE_UINT32;
                triangles.indexData.deviceAddress =
                     indices == 0 ? 0 : indices + VkDeviceAddress( g ) * grid.Triangles * 3u * sizeof( uint32_t );
            }
            return geometries;
        }

        VkAccelerationStructureBuildGeometryInfoKHR
        TileBuildInfo( const std::array<VkAccelerationStructureGeometryKHR, 2>& geometries, bool update )
        {
            VkAccelerationStructureBuildGeometryInfoKHR build{
                 VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            build.type  = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            build.mode          = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                         : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = static_cast<uint32_t>( geometries.size() );
            build.pGeometries   = geometries.data();
            return build;
        }

        struct OwnedBuffer
        {
            VkBuffer          Buffer = VK_NULL_HANDLE;
            VmaAllocation     Memory = nullptr;
            VkDeviceSize      Bytes  = 0;
            ResourceOwnership Ledger;
        };
    } // namespace

    class VulkanLandscapeRayTracingGeometry final : public RayTracing::LandscapeRayTracingGeometryTarget
    {
    public:
        VulkanLandscapeRayTracingGeometry() = default;
        ~VulkanLandscapeRayTracingGeometry() override
        try
        {
            Release();
        }
        DESERT_DESTRUCTOR_GUARD( "~VulkanLandscapeRayTracingGeometry" )
        VulkanLandscapeRayTracingGeometry( const VulkanLandscapeRayTracingGeometry& )            = delete;
        VulkanLandscapeRayTracingGeometry& operator=( const VulkanLandscapeRayTracingGeometry& ) = delete;

        [[nodiscard]] uint64_t GetDeviceAddress() const override
        {
            return m_DeviceAddress;
        }

        [[nodiscard]] Common::ResultStr<RayTracing::LandscapeBlasPlan>
        Prepare( const RayTracing::LandscapeRayTracingGrid& grid, bool update ) override
        {
            using Plan         = RayTracing::LandscapeBlasPlan;
            const auto logical = LogicalDevice();
            if ( !logical )
                return Common::MakeError<Plan>( "landscape BLAS: no Vulkan device" );
            const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
            if ( !functions.Complete() )
                return Common::MakeError<Plan>(
                     "landscape BLAS: the device has no acceleration-structure functions, but a resolved method "
                     "traces rays" );
            const VkDevice device = logical->GetVulkanLogicalDevice();

            const auto                               geometries = TileGeometries( 0, 0, grid );
            const auto                               build      = TileBuildInfo( geometries, false );
            const std::array<uint32_t, 2>            counts{ grid.Triangles, grid.Triangles };
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                 VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                          &build, counts.data(), &sizes );

            const bool recreate = m_Handle == VK_NULL_HANDLE || !( grid == m_Grid );
            if ( recreate )
            {
                Release();
                const VkBufferUsageFlags input =
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                if ( auto made = Allocate( "Landscape RT vertices", ( grid.GridVertices + 1u ) * sizeof( Vertex ),
                                           input, ResourceKind::StorageBuffer, m_Vertices );
                     !made )
                    return Common::MakeError<Plan>( made.GetError() );
                if ( auto made = Allocate( "Landscape RT indices", 2u * grid.Triangles * 3u * sizeof( uint32_t ),
                                           input, ResourceKind::StorageBuffer, m_Indices );
                     !made )
                    return Common::MakeError<Plan>( made.GetError() );
                if ( auto made = Allocate( "Landscape BLAS", sizes.accelerationStructureSize,
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
                         "vkCreateAccelerationStructureKHR (landscape BLAS, {} triangles x 2) failed: {}",
                         grid.Triangles, static_cast<int>( created ) );
                }
                VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
                     VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
                addressInfo.accelerationStructure = m_Handle;
                m_DeviceAddress = functions.GetAccelerationStructureDeviceAddress( device, &addressInfo );
                if ( m_DeviceAddress == 0 )
                {
                    Release();
                    return Common::MakeError<Plan>(
                         "vkGetAccelerationStructureDeviceAddressKHR answered 0 for a landscape BLAS" );
                }
                m_Grid = grid;

                MakeExternal( m_VerticesExternal, m_Vertices, VK_NULL_HANDLE );
                MakeExternal( m_IndicesExternal, m_Indices, VK_NULL_HANDLE );
                MakeExternal( m_StorageExternal, m_Storage, m_Handle );
            }
            const bool refit = update && !recreate;
            return Common::MakeSuccess(
                 Plan{ .ScratchBytes = ( refit ? sizes.updateScratchSize : sizes.buildScratchSize ) +
                                       kMaxAccelerationStructureScratchAlignment,
                       .Update = refit } );
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
                                                         const RDG::BufferBinding& indices,
                                                         const RDG::BufferBinding& scratch, uint64_t scratchOffset,
                                                         bool update ) override
        {
            const auto command = VulkanRdgBackend::CommandBufferOf( context );
            if ( !command )
                return Common::MakeError( command.GetError() );
            const auto vertexBuffer = VulkanRdgBackend::BufferOf( vertices );
            if ( !vertexBuffer )
                return Common::MakeError( vertexBuffer.GetError() );
            const auto indexBuffer = VulkanRdgBackend::BufferOf( indices );
            if ( !indexBuffer )
                return Common::MakeError( indexBuffer.GetError() );
            const auto scratchBuffer = VulkanRdgBackend::BufferOf( scratch );
            if ( !scratchBuffer )
                return Common::MakeError( scratchBuffer.GetError() );
            const auto logical = LogicalDevice();
            if ( !logical || m_Handle == VK_NULL_HANDLE )
                return Common::MakeError(
                     "landscape BLAS build recorded without a prepared acceleration structure" );
            const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
            const VkDevice                   device    = logical->GetVulkanLogicalDevice();

            const auto geometries =
                 TileGeometries( BufferAddress( functions, device, vertexBuffer.GetValue()->GetBuffer() ),
                                 BufferAddress( functions, device, indexBuffer.GetValue()->GetBuffer() ), m_Grid );
            VkAccelerationStructureBuildGeometryInfoKHR build = TileBuildInfo( geometries, update );
            build.srcAccelerationStructure                    = update ? m_Handle : VK_NULL_HANDLE;
            build.dstAccelerationStructure                    = m_Handle;
            build.scratchData.deviceAddress                   = AlignScratchAddress(
                 BufferAddress( functions, device, scratchBuffer.GetValue()->GetBuffer() ) + scratchOffset );

            std::array<VkAccelerationStructureBuildRangeInfoKHR, 2> ranges{};
            for ( auto& range : ranges )
                range.primitiveCount = m_Grid.Triangles;
            const VkAccelerationStructureBuildRangeInfoKHR* rangeList = ranges.data();
            functions.CmdBuildAccelerationStructures( command.GetValue(), 1, &build, &rangeList );
            return BOOLSUCCESS;
        }

        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_Vertices.Bytes + m_Indices.Bytes + m_Storage.Bytes;
        }

    private:
        static Common::BoolResultStr Allocate( const char* name, VkDeviceSize bytes, VkBufferUsageFlags usage,
                                               ResourceKind kind, OwnedBuffer& out )
        {
            const auto& allocator =
                 SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size         = bytes;
            info.usage        = usage;
            info.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
            const auto memory = allocator->RT_AllocateBuffer( name, info, VMA_MEMORY_USAGE_GPU_ONLY, out.Buffer );
            if ( !memory.IsSuccess() )
                return Common::MakeFormattedError<bool>( "{} ({} bytes): {}", name, bytes, memory.GetError() );
            out.Memory = memory.GetValue();
            out.Bytes  = bytes;
            out.Ledger = ResourceOwnership::Take( kind, VulkanAllocator::AllocationSize( out.Memory ) );
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
            m_Grid             = {};
            m_VerticesExternal = RDG::ExternalBuffer{};
            m_IndicesExternal  = RDG::ExternalBuffer{};
            m_StorageExternal  = RDG::ExternalBuffer{};
        }

        VkAccelerationStructureKHR            m_Handle        = VK_NULL_HANDLE;
        PFN_vkDestroyAccelerationStructureKHR m_Destroy       = nullptr;
        uint64_t                              m_DeviceAddress = 0;
        RayTracing::LandscapeRayTracingGrid   m_Grid;
        OwnedBuffer                           m_Vertices;
        OwnedBuffer                           m_Indices;
        OwnedBuffer                           m_Storage;
        RDG::ExternalBuffer                   m_VerticesExternal;
        RDG::ExternalBuffer                   m_IndicesExternal;
        RDG::ExternalBuffer                   m_StorageExternal;
    };
} // namespace Desert::Graphic::API::Vulkan

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<LandscapeRayTracingGeometryTarget> CreateLandscapeRayTracingGeometryTarget()
    {
        return std::make_unique<API::Vulkan::VulkanLandscapeRayTracingGeometry>();
    }
} // namespace Desert::Graphic::RayTracing
