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
        // Reaching here on a device without acceleration structures is a caller's bug (every caller reads the
        // catalog first: MeshGpuUploader, RayTracingSceneRequired): an error, never a silent null BLAS.
        if ( !functions.Complete() )
            return Common::MakeError<std::shared_ptr<VulkanRayTracingGeometry>>(
                 "BLAS build: the device has no acceleration structure functions (ray tracing off or hidden)" );
        if ( input.Segments.empty() )
            return Common::MakeSuccess( std::shared_ptr<VulkanRayTracingGeometry>{} );

        const auto vertexBuffer = SP_CAST( VulkanVertexBuffer, input.Vertices );
        const auto indexBuffer  = SP_CAST( VulkanIndexBuffer, input.Indices );
        if ( !vertexBuffer || !indexBuffer || vertexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE ||
             indexBuffer->GetVulkanBuffer() == VK_NULL_HANDLE )
            return Common::MakeSuccess( std::shared_ptr<VulkanRayTracingGeometry>{} );

        const std::vector<RayTracing::RayTracingGeometrySegment>& segments = input.Segments;

        const VkDevice device = logical->GetVulkanLogicalDevice();
        const auto&    allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        auto geometry = std::make_shared<VulkanRayTracingGeometry>();

        // THE OPACITY MICROMAPS FIRST (RT-OMM-SER): each baked one becomes a VkMicromapEXT whose handle the
        // segment's geometry names; their builds are recorded below, before the BLAS build, behind a
        // MICROMAP_BUILD -> ACCELERATION_STRUCTURE_BUILD barrier. A micromap on a device without the functions is
        // a caller's bug (the catalog gates the bake): an error, never a BLAS silently built without it.
        if ( !input.Micromaps.empty() && !functions.MicromapsComplete() )
            return Common::MakeError<std::shared_ptr<VulkanRayTracingGeometry>>(
                 "BLAS build: opacity micromaps given on a device without VK_EXT_opacity_micromap" );
        std::vector<VulkanSegmentMicromap>  segmentMicromaps( input.Micromaps.empty() ? 0u : segments.size() );
        std::vector<VkMicromapUsageEXT>     micromapUsages;
        std::vector<VkMicromapBuildInfoEXT> micromapBuilds;
        std::vector<VkDeviceSize>           micromapScratchOffsets;
        micromapUsages.reserve( input.Micromaps.size() ); // never reallocated: the builds point into it
        micromapBuilds.reserve( input.Micromaps.size() );
        VkDeviceSize micromapScratchBytes = 0;
        geometry->m_DestroyMicromap       = functions.DestroyMicromap;
        for ( const RayTracing::RayTracingSegmentOpacityMicromap& source : input.Micromaps )
        {
            if ( source.Segment >= segments.size() || segments[source.Segment].Opaque ||
                 source.TriangleCount != segments[source.Segment].PrimitiveCount ||
                 segmentMicromaps[source.Segment].Micromap != VK_NULL_HANDLE )
                return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>(
                     "BLAS build: the micromap of segment {} ({} triangles) is not one of a non-opaque segment of {}",
                     source.Segment, source.TriangleCount, segments.size() );
            VkMicromapBuildInfoEXT& build = micromapBuilds.emplace_back();
            build.sType                   = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
            build.type                    = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
            build.flags                   = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
            build.mode                    = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
            build.usageCountsCount        = 1;
            build.pUsageCounts =
                 &micromapUsages.emplace_back( OpacityMicromapUsage( source.TriangleCount, kOpacityMicromapLevel ) );
            build.data.deviceAddress          = source.DataAddress;
            build.triangleArray.deviceAddress = source.TrianglesAddress;
            build.triangleArrayStride         = sizeof( VkMicromapTriangleEXT );
            VkMicromapBuildSizesInfoEXT sizes{ VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
            functions.GetMicromapBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &sizes );

            Micromap&  micromap = geometry->m_Micromaps.emplace_back();
            const auto storage  = allocator->RT_AllocateBuffer(
                 "BLAS_micromap",
                 DeviceAddressBufferInfo( sizes.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT ),
                 VMA_MEMORY_USAGE_GPU_ONLY, micromap.Storage );
            if ( !storage.IsSuccess() )
                return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>(
                     "BLAS micromap storage: {}", storage.GetError() );
            micromap.StorageMemory = storage.GetValue();
            micromap.Ledger        = RayTracing::RecordAccelerationStructureMemory(
                 RayTracing::AccelerationStructureLevel::Bottom, VulkanAllocator::AllocationSize( storage.GetValue() ) );
            VkMicromapCreateInfoEXT create{ VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
            create.buffer = micromap.Storage;
            create.size   = sizes.micromapSize;
            create.type   = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
            if ( const VkResult created = functions.CreateMicromap( device, &create, nullptr, &micromap.Handle );
                 created != VK_SUCCESS )
                return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>(
                     "vkCreateMicromapEXT failed: {}", static_cast<int>( created ) );
            build.dstMicromap = micromap.Handle;
            // One scratch buffer for all of them: each build its own aligned slice (addresses set once it exists).
            micromapScratchOffsets.push_back( micromapScratchBytes );
            micromapScratchBytes += AlignScratchAddress( sizes.buildScratchSize );
            segmentMicromaps[source.Segment] = VulkanSegmentMicromap{ .Micromap     = micromap.Handle,
                                                                      .IndexAddress = source.IndexAddress };
        }

        const VulkanBlasBuildInput buildInput =
             TranslateBlasGeometry( segments,
                                    functions.BufferAddress( device, vertexBuffer->GetVulkanBuffer() ) +
                                         input.VertexByteOffset,
                                    functions.BufferAddress( device, indexBuffer->GetVulkanBuffer() ) +
                                         input.IndexByteOffset,
                                    segmentMicromaps );
        VkAccelerationStructureBuildGeometryInfoKHR build = MakeBlasBuildInfo( buildInput );

        VkAccelerationStructureBuildSizesInfoKHR sizes{
             VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        functions.GetAccelerationStructureBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                      &build, buildInput.MaxPrimitiveCounts.data(), &sizes );

        geometry->m_Destroy       = functions.DestroyAccelerationStructure;
        geometry->m_Segments      = segments;
        geometry->m_Vertices      = VertexBufferRange{ input.Vertices, input.VertexByteOffset };
        geometry->m_Indices       = IndexBufferRange{ input.Indices, input.IndexByteOffset };

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

        VkBuffer                       micromapScratch = VK_NULL_HANDLE;
        Common::ResultStr<VmaAllocation> micromapScratchMemory = Common::MakeSuccess( VmaAllocation{} );
        if ( !micromapBuilds.empty() )
        {
            micromapScratchMemory = allocator->RT_AllocateBuffer(
                 "BLAS_micromap_scratch",
                 DeviceAddressBufferInfo( micromapScratchBytes + kMaxAccelerationStructureScratchAlignment,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT ),
                 VMA_MEMORY_USAGE_GPU_ONLY, micromapScratch );
            if ( !micromapScratchMemory.IsSuccess() )
            {
                allocator->RT_DestroyBuffer( scratch, scratchMemory.GetValue() );
                return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>(
                     "BLAS micromap scratch: {}", micromapScratchMemory.GetError() );
            }
            const VkDeviceAddress base = AlignScratchAddress( functions.BufferAddress( device, micromapScratch ) );
            for ( std::size_t i = 0; i < micromapBuilds.size(); ++i )
                micromapBuilds[i].scratchData.deviceAddress = base + micromapScratchOffsets[i];
        }

        build.dstAccelerationStructure  = geometry->m_Handle;
        build.scratchData.deviceAddress = AlignScratchAddress( functions.BufferAddress( device, scratch ) );

        auto command = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !command.IsSuccess() )
        {
            allocator->RT_DestroyBuffer( scratch, scratchMemory.GetValue() );
            if ( micromapScratch != VK_NULL_HANDLE )
                allocator->RT_DestroyBuffer( micromapScratch, micromapScratchMemory.GetValue() );
            return Common::MakeError<std::shared_ptr<VulkanRayTracingGeometry>>( command.GetError() );
        }
        // SUBMITTED, NOT WAITED FOR (FPS-CPU-2; UE builds a BLAS on the frame's command list and never sleeps
        // the CPU on it). A BLAS variant (RayTracingGeometryVariants::Resolve) is built from inside the frame's
        // graph setup, and the blocking flush made that frame wait for the whole GPU queue ahead of it. The two
        // barriers are the whole dependency, both on this one queue:
        //  * before: every earlier submission's writes (the geometry's own upload) are visible to the build's
        //    reads of the vertex and index buffers;
        //  * after: the built structure is visible to every later submission - the frame graph's TLAS build
        //    and every ray query, all submitted after this on the same queue.
        // The scratch buffer is released once the fence is seen signalled (CollectFinished, every frame).
        const VkCommandBuffer cmd = command.GetValue();
        const VkMemoryBarrier inputsReady{ .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR };
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &inputsReady, 0,
                              nullptr, 0, nullptr );
        // The micromaps, then the BLAS that names them (sync2: MICROMAP_BUILD has no legacy stage bit). The bake
        // that wrote their inputs was submitted before this command buffer; inputsReady above covers its writes
        // for the acceleration-structure stage, the barrier here covers them for the micromap build.
        if ( !micromapBuilds.empty() )
        {
            const VkMemoryBarrier2 bakeWritten{ .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                                .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                .dstStageMask  = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
                                                .dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT |
                                                                 VK_ACCESS_2_SHADER_READ_BIT };
            const VkDependencyInfo bakeDependency{ .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                   .memoryBarrierCount = 1,
                                                   .pMemoryBarriers    = &bakeWritten };
            vkCmdPipelineBarrier2( cmd, &bakeDependency );
            functions.CmdBuildMicromaps( cmd, static_cast<uint32_t>( micromapBuilds.size() ), micromapBuilds.data() );
            const VkMemoryBarrier2 micromapsBuilt{
                 .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                 .srcStageMask  = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
                 .srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
                 .dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 .dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT };
            const VkDependencyInfo builtDependency{ .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                    .memoryBarrierCount = 1,
                                                    .pMemoryBarriers    = &micromapsBuilt };
            vkCmdPipelineBarrier2( cmd, &builtDependency );
        }
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = buildInput.Ranges.data();
        functions.CmdBuildAccelerationStructures( cmd, 1, &build, &ranges );
        const VkMemoryBarrier built{ .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                     .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                                      VK_ACCESS_SHADER_READ_BIT };
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &built, 0, nullptr, 0, nullptr );
        const VmaAllocation scratchAllocation = scratchMemory.GetValue();
        const auto          submitted         = CommandBufferAllocator::GetInstance().RT_SubmitGraphicThenRelease(
             cmd,
             [alloc = allocator.get(), scratch, scratchAllocation, micromapScratch,
              micromapScratchAllocation = micromapScratchMemory.GetValue(), sources = input.Micromaps]
             {
                 // sources: the bake's buffers, held until the micromap build that read them has finished.
                 alloc->RT_ReleaseStaging( scratch, scratchAllocation, Common::MakeSuccess( VK_SUCCESS ) );
                 if ( micromapScratch != VK_NULL_HANDLE )
                     alloc->RT_ReleaseStaging( micromapScratch, micromapScratchAllocation,
                                               Common::MakeSuccess( VK_SUCCESS ) );
             } );
        if ( !submitted.IsSuccess() )
            return Common::MakeFormattedError<std::shared_ptr<VulkanRayTracingGeometry>>( "BLAS build submit: {}",
                                                                                          submitted.GetError() );

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
    {
        // Both on the frame ring: a frame in flight may still trace this BLAS through a TLAS instance.
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        allocator->RT_DestroyAccelerationStructure( m_Handle, m_Destroy );
        allocator->RT_DestroyBuffer( m_Storage, m_StorageMemory );
        // After the BLAS on the same ring (queued later, drained no earlier): a micromap outlives every trace of
        // the BLAS that names it.
        for ( Micromap& micromap : m_Micromaps )
            allocator->RT_DestroyLater(
                 [alloc = allocator.get(), destroy = m_DestroyMicromap, handle = micromap.Handle,
                  storage = micromap.Storage, memory = micromap.StorageMemory,
                  device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                ->GetVulkanLogicalDevice()]
                 {
                     if ( handle != VK_NULL_HANDLE && destroy != nullptr )
                         destroy( device, handle, nullptr );
                     alloc->RT_DestroyBuffer( storage, memory );
                 } );
    }
} // namespace Desert::Graphic::API::Vulkan
