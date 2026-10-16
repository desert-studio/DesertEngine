#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

namespace Desert::Graphic::API::Vulkan
{

    Common::ResultStr<std::shared_ptr<VulkanRayTracingGeometry>>
    VulkanRayTracingGeometry::BuildStatic( const RayTracing::RayTracingMeshInput& input )
    {

        const auto logical = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        if ( !logical )
            return Common::MakeError<std::shared_ptr<VulkanRayTracingGeometry>>( "BLAS build: no Vulkan device" );
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( !functions.Complete() || input.Segments.empty() )
            return Common::MakeSuccess( std::shared_ptr<VulkanRayTracingGeometry>{} );

        const auto vertexBuffer = SP_CAST( VulkanVertexBuffer, input.Vertices );
        const auto indexBuffer  = SP_CAST( VulkanIndexBuffer, input.Indices );
        if ( !vertexBuffer || !indexBuffer || vertexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE ||
             indexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE )
            return Common::MakeSuccess( std::shared_ptr<VulkanRayTracingGeometry>{} );

        const std::vector<RayTracing::RayTracingGeometrySegment>& segments = input.Segments;

        const VkDevice             device = logical->GetVulkanLogicalDevice();
        const VulkanBlasBuildInput input =
             TranslateBlasGeometry( segments, functions.BufferAddress( device, vertexBuffer->GetVulkanBuffer() ),
                                    functions.BufferAddress( device, indexBuffer->GetVulkanBuffer() ) );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( input );

        VkAccelerationStructureBuildSizesInfoKHR sizes{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                      &build, input.MaxPrimitiveCounts.data(), &sizes );

        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();

        auto geometry       = std::make_shared<VulkanRayTracingGeometry>();
        geometry->m_Destroy = functions.DestroyAccelerationStructure;

        const auto storage = allocator->RT_AllocateBuffer(
             "BLAS",
             DeviceAddressBufferInfo( sizes.accelerationStructureSize,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR ),
             VMA_MEMORY_USAGE_GPU_ONLY, geometry->m_Storage );
        if ( !storage.IsSuccess() )
            return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>( "BLAS storage: {}",
                                                                                          storage.GetError() );
        geometry->m_StorageMemory = storage.GetValue();
        geometry->m_Ledger        = RayTracing::RecordAccelerationStructureMemory(
             RayTracing::AccelerationStructureLevel::Bottom,
             VulkanAllocator::AllocationSize( storage.GetValue() ) );

        VkAccelerationStructureCreateInfoKHR create{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        create.buffer = geometry->m_Storage;
        create.size   = sizes.accelerationStructureSize;
        create.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        const VkResult created =
             functions.CreateAccelerationStructure( device, &create, nullptr, &geometry->m_Handle );
        if ( created != VK_SUCCESS )
            return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>(
                 "vkCreateAccelerationStructureKHR (BLAS) failed: {}", static_cast<int>( created ) );

        VkBuffer   scratch       = VK_NULL_HANDLE;
        const auto scratchMemory = allocator->RT_AllocateBuffer(
             "BLAS_scratch",
             DeviceAddressBufferInfo( sizes.buildScratchSize + kMaxAccelerationStructureScratchAlignment,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT ),
             VMA_MEMORY_USAGE_GPU_ONLY, scratch );
        if ( !scratchMemory.IsSuccess() )
            return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>(
                 "BLAS scratch: {}", scratchMemory.GetError() );

        build.dstAccelerationStructure  = geometry->m_Handle;
        build.scratchData.deviceAddress = AlignScratchAddress( functions.BufferAddress( device, scratch ) );

        auto command = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !command.IsSuccess() )
        {
            allocator->RT_DestroyBuffer( scratch, scratchMemory.GetValue() );
            return Common::MakeError<std::shared_ptr<VulkanRayTracingGeometry>>( command.GetError() );
        }
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = input.Ranges.data();
        functions.CmdBuildAccelerationStructures( command.GetValue(), 1, &build, &ranges );
        const auto flushed =
             CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( command.GetValue() );
        // A waited fence frees the scratch now; a failed flush queues it on the frame ring (staging rule).
        allocator->RT_ReleaseStaging( scratch, scratchMemory.GetValue(), flushed );
        if ( !flushed.IsSuccess() )
            return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>( "BLAS build submit: {}",
                                                                                          flushed.GetError() );

        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addressInfo.accelerationStructure = geometry->m_Handle;
        geometry->m_DeviceAddress = functions.GetAccelerationStructureDeviceAddress( device, &addressInfo );
        if ( geometry->m_DeviceAddress == 0 )
            return Common::MakeError<std::shared_ptr<VulkanRayTracingGeometry>>(
                 "vkGetAccelerationStructureDeviceAddressKHR answered 0 for a built BLAS" );
        return Common::MakeSuccess( std::move( geometry ) );
    }

    VulkanRayTracingGeometry::~VulkanRayTracingGeometry()
    try
    {
        // Both on the frame ring: a frame in flight may still trace this BLAS through a TLAS instance.
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        allocator->RT_DestroyAccelerationStructure( m_Handle, m_Destroy );
        allocator->RT_DestroyBuffer( m_Storage, m_StorageMemory );
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanRayTracingGeometry" )
} // namespace Desert::Graphic::API::Vulkan
