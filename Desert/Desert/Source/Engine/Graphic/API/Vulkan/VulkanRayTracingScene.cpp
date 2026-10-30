#include <Common/Core/Core.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingScene.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>

#include <algorithm>
#include <bit>
#include <format>

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
                return BOOLSUCCESS;
            };
        }
        // Recreated storage resets m_BuiltInstances (Release): its first frame builds.
        const bool updates = update && m_BuiltInstances == instanceCount;
        return Common::MakeSuccess(
             Sizes{ .ScratchBytes = ( updates ? frameSizes.updateScratchSize : frameSizes.buildScratchSize ) +
                                    kMaxAccelerationStructureScratchAlignment,
                    .Update       = updates } );
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
        m_BuiltInstances = instanceCount;
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
        m_BuiltInstances = kNoBuild;
        m_External      = RDG::ExternalBuffer{};
    }

    VulkanRayTracingScene::~VulkanRayTracingScene()
    try
    {
        Release();
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanRayTracingScene" )
} // namespace Desert::Graphic::API::Vulkan

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<RayTracingSceneTarget> CreateRayTracingSceneTarget()
    {
        return std::make_unique<API::Vulkan::VulkanRayTracingScene>();
    }
} // namespace Desert::Graphic::RayTracing
