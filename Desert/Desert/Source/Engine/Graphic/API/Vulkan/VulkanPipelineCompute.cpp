#include <Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/WriteDescriptorSetBuilder.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

namespace Desert::Graphic::API::Vulkan
{
    VulkanPipelineCompute::VulkanPipelineCompute( const ComputePipelineSpecification& spec ) 
        : m_Specification( spec )
    {
        m_VulkanMaterialBackend = std::make_unique<VulkanMaterialBackend>( spec.Shader );
    }

    VulkanPipelineCompute::~VulkanPipelineCompute()
    {
        Release();
    }

    ComputePipeline& VulkanPipelineCompute::SetInput( uint32_t binding, Image* image )
    {
        m_BoundInputs[binding] = image;
        return *this;
    }

    ComputePipeline& VulkanPipelineCompute::SetOutput( uint32_t binding, Image* image, uint32_t mip )
    {
        m_BoundOutputs[binding] = { image, mip };
        return *this;
    }

    ComputePipeline& VulkanPipelineCompute::SetStorageBuffer( uint32_t                        binding,
                                                              ShaderResources::StorageBuffer* buffer )
    {
        m_BoundStorageBuffers[binding] = buffer;
        return *this;
    }

    ComputePipeline& VulkanPipelineCompute::SetPushConstants( const void* data, uint32_t size )
    {
        const auto* bytes = reinterpret_cast<const std::byte*>( data );
        m_BoundPushConstants.assign( bytes, bytes + size );
        return *this;
    }

    namespace
    {
        // A sampled input is bound with the descriptor write that matches its DIMENSIONALITY. This is not
        // cosmetic: GetSampler2DWDS substitutes a 2D fallback texture whenever the supplied image info is
        // incomplete, and a 2D view landing in a `sampler3D` (or a `samplerCube`) binding does not fail —
        // it quietly samples the wrong thing, which is the most expensive failure mode there is.
        enum class SampledImageKind
        {
            Texture2D,
            Cube,
            Volume
        };

        SampledImageKind ClassifySampledImage( Image* image )
        {
            if ( dynamic_cast<Image3D*>( image ) )
                return SampledImageKind::Volume;
            if ( dynamic_cast<ImageCube*>( image ) )
                return SampledImageKind::Cube;
            return SampledImageKind::Texture2D;
        }
    } // namespace

    void VulkanPipelineCompute::RecordDescriptorsAndDispatch( VkCommandBuffer cmd, VkDescriptorSet descriptorSet,
                                                              uint32_t groupsX, uint32_t groupsY,
                                                              uint32_t groupsZ )
    {
        // The image infos must outlive UpdateDescriptorSet (each write stores a pointer into here),
        // so reserve up front to avoid a reallocation invalidating those pointers.
        std::vector<VkDescriptorImageInfo> infos;
        infos.reserve( m_BoundInputs.size() + m_BoundOutputs.size() );
        std::vector<VkWriteDescriptorSet> writes;

        // Outputs: bind the chosen mip view as a storage image (the caller has already put the image
        // into GENERAL — we never transition here so this works both immediate and in-frame).
        for ( const auto& [binding, out] : m_BoundOutputs )
        {
            auto* img = dynamic_cast<IVulkanImage*>( out.Image );
            if ( !img )
                continue;
            infos.push_back( { VK_NULL_HANDLE, img->GetMipView( out.Mip ), VK_IMAGE_LAYOUT_GENERAL } );
            writes.push_back( DescriptorSetBuilder::GetStorageWDS( m_VulkanMaterialBackend.get(), 0, 0,
                                                                  binding, 1, &infos.back() ) );
        }

        // Inputs: sampled images (2D, cube and volume share the same VkDescriptorImageInfo shape, but not
        // the same write builder — see ClassifySampledImage). Use the image's actual tracked layout: a
        // sampled input may be SHADER_READ_ONLY (a separate source image), GENERAL (a mip of an image
        // this same chain is writing) or, for the scene depth, whatever ComputeImageBeginRead left.
        for ( const auto& [binding, image] : m_BoundInputs )
        {
            auto* img = dynamic_cast<IVulkanImage*>( image );
            if ( !img )
                continue;
            const auto&            r    = img->GetResource();
            const SampledImageKind kind = ClassifySampledImage( image );

            if ( kind == SampledImageKind::Volume &&
                 ( r.ImageView == VK_NULL_HANDLE || r.Sampler == VK_NULL_HANDLE ||
                   r.Layout == VK_IMAGE_LAYOUT_UNDEFINED ) )
            {
                // No fallback exists for a volume, and dispatching with a stale descriptor would read
                // whatever the previous user of this ring slot bound. Say exactly what is missing and
                // drop the dispatch instead.
                LOG_ERROR( "ComputePipeline '{}': volume input at binding {} is not sampleable "
                           "(view={}, sampler={}, layout={}); dispatch skipped",
                           m_Specification.DebugName, binding, r.ImageView != VK_NULL_HANDLE,
                           r.Sampler != VK_NULL_HANDLE, static_cast<int>( r.Layout ) );
                return;
            }

            infos.push_back( { r.Sampler, r.ImageView, r.Layout } );

            switch ( kind )
            {
                case SampledImageKind::Volume:
                    writes.push_back( DescriptorSetBuilder::GetSampler3DWDS( m_VulkanMaterialBackend.get(), 0, 0,
                                                                             binding, 1, &infos.back() ) );
                    break;
                case SampledImageKind::Cube:
                    writes.push_back( DescriptorSetBuilder::GetSamplerCubeWDS( m_VulkanMaterialBackend.get(), 0, 0,
                                                                               binding, 1, &infos.back() ) );
                    break;
                case SampledImageKind::Texture2D:
                    writes.push_back( DescriptorSetBuilder::GetSampler2DWDS( m_VulkanMaterialBackend.get(), 0, 0,
                                                                             binding, 1, &infos.back() ) );
                    break;
            }
        }

        // Storage buffers (read-write; e.g. a luminance histogram). The descriptor buffer info lives in
        // the buffer object (stable across this call), so we can point the write straight at it.
        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        for ( const auto& [binding, buffer] : m_BoundStorageBuffers )
        {
            auto* vkBuffer = dynamic_cast<ShaderResources::API::Vulkan::VulkanStorageBuffer*>( buffer );
            if ( !vkBuffer )
                continue;
            writes.push_back( DescriptorSetBuilder::GetStorageWDS( m_VulkanMaterialBackend.get(), 0, 0, binding,
                                                                  1, &vkBuffer->GetDescriptorBufferInfo( frameIndex ) ) );
        }

        // Retarget every write at the supplied set, then update it in one shot.
        UpdateDescriptorSet( 0, writes, descriptorSet );

        vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComputePipeline );
        vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComputePipelineLayout, 0, 1,
                                 &descriptorSet, 0, nullptr );

        if ( !m_BoundPushConstants.empty() )
            vkCmdPushConstants( cmd, m_ComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                static_cast<uint32_t>( m_BoundPushConstants.size() ),
                                m_BoundPushConstants.data() );

        vkCmdDispatch( cmd, groupsX, groupsY, groupsZ );
    }

    void VulkanPipelineCompute::Dispatch( uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ )
    {
        auto* backend = GetVulkanMaterialBackend();
        if ( !backend )
            return;

        auto cmdResult = CommandBufferAllocator::GetInstance().RT_GetCommandBufferCompute( true );
        if ( !cmdResult.IsSuccess() )
            return;
        VkCommandBuffer cmd = cmdResult.GetValue();

        // Immediate path: transition outputs to GENERAL, dispatch, transition back to SHADER_READ so the
        // result can be sampled, then submit + wait. Reuses the persistent set 0 (safe — we wait below).
        for ( const auto& [binding, out] : m_BoundOutputs )
            if ( auto* img = dynamic_cast<IVulkanImage*>( out.Image ) )
                img->TransitionLayout( cmd, VK_IMAGE_LAYOUT_GENERAL );

        RecordDescriptorsAndDispatch( cmd, backend->GetDescriptorSet( 0, 0 ), groupsX, groupsY, groupsZ );

        for ( const auto& [binding, out] : m_BoundOutputs )
            if ( auto* img = dynamic_cast<IVulkanImage*>( out.Image ) )
                img->TransitionLayout( cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );

        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferCompute( cmd );
    }

    void VulkanPipelineCompute::EnsureInFrameRing()
    {
        if ( m_InFramePool != VK_NULL_HANDLE )
            return;

        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();

        // Pool sized from the CAPTURED layout's own bindings — the same contract the sets below are
        // allocated from — rather than from the shader's current reflection, which a recompile may have
        // moved on from since this pipeline was built.
        std::unordered_map<int, uint32_t> countsByType;
        for ( const auto& layout : m_Layouts )
        {
            if ( !layout )
                continue;
            for ( const auto& binding : layout->Bindings() )
                countsByType[static_cast<int>( binding.descriptorType )] += binding.descriptorCount;
        }

        std::vector<VkDescriptorPoolSize> poolSizes;
        for ( const auto& [type, count] : countsByType )
            poolSizes.push_back( { static_cast<VkDescriptorType>( type ), count * kInFrameRingSize } );
        if ( poolSizes.empty() )
            poolSizes.push_back( { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kInFrameRingSize } );

        VkDescriptorPoolCreateInfo poolInfo{ .sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .maxSets = kInFrameRingSize,
                                             .poolSizeCount = static_cast<uint32_t>( poolSizes.size() ),
                                             .pPoolSizes    = poolSizes.data() };
        VK_CHECK_RESULT( vkCreateDescriptorPool( device, &poolInfo, nullptr, &m_InFramePool ) );

        // The ring is allocated from the layout THIS PIPELINE captured at Invalidate, never from the
        // shader's current one. Re-reading the shader here is what produced the mismatch this whole
        // arrangement exists to prevent: a shader recompiled after the pipeline was built publishes a
        // layout with a different descriptor count, and a set allocated from it cannot be bound to a
        // pipeline layout made from the old one ("has 8 total descriptors, but ... has 9").
        VkDescriptorSetLayout layout0 =
             m_Layouts.empty() || !m_Layouts[0] ? VK_NULL_HANDLE : m_Layouts[0]->Handle();

        std::vector<VkDescriptorSetLayout> layouts( kInFrameRingSize, layout0 );
        m_InFrameRing.resize( kInFrameRingSize );
        VkDescriptorSetAllocateInfo allocInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool     = m_InFramePool,
                                               .descriptorSetCount = kInFrameRingSize,
                                               .pSetLayouts        = layouts.data() };
        VK_CHECK_RESULT( vkAllocateDescriptorSets( device, &allocInfo, m_InFrameRing.data() ) );
    }

    void VulkanPipelineCompute::RecordInFrame( VkCommandBuffer cmd, uint32_t groupsX, uint32_t groupsY,
                                               uint32_t groupsZ )
    {
        if ( !m_VulkanMaterialBackend || !cmd )
            return;

        EnsureInFrameRing();

        VkDescriptorSet set = m_InFrameRing[m_InFrameCursor];
        m_InFrameCursor     = ( m_InFrameCursor + 1 ) % kInFrameRingSize;

        RecordDescriptorsAndDispatch( cmd, set, groupsX, groupsY, groupsZ );
    }

    void VulkanPipelineCompute::Invalidate()
    {
        Release();

        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();
        const auto vulkanShader = sp_cast<VulkanShader>( m_Specification.Shader );

        // THE STAGE IS FOUND BY WHAT IT IS, NOT BY WHERE IT SITS. This used to be
        // `GetPipelineShaderStageCreateInfos()[0]` at the pipeline-info below, which reads a vector a
        // failed compile leaves empty (SIGSEGV, after 23 validation errors about a descriptor pool of
        // size zero) and, when the vector is full, happily hands vkCreateComputePipelines the VERTEX
        // stage of a graphics program reached by name. Both are refusals, so both are refused here and
        // nothing is built: the caller sees it because ComputePipeline::Create asks this object for the
        // handle it produced.
        const VkPipelineShaderStageCreateInfo* computeStage = vulkanShader->GetComputeStage();
        if ( !computeStage )
        {
            LOG_ERROR( "ComputePipeline '{}': shader '{}' carries no compute stage ({} stage(s) in all) "
                       "— nothing is built and the dispatches using it are skipped.",
                       m_Specification.DebugName, vulkanShader->GetName(),
                       vulkanShader->GetPipelineShaderStageCreateInfos().size() );
            return;
        }

        // Captured ONCE, here, and used for everything this pipeline binds: the pipeline layout below,
        // the in-frame ring, and the pool that ring is allocated from. Holding the references is what
        // keeps the layouts alive if the shader recompiles under us, and using only these is what keeps
        // the set and the pipeline layout describing the same contract.
        m_Layouts          = vulkanShader->GetAllDescriptorSetLayouts();
        m_ShaderGeneration = vulkanShader->GetReloadGeneration();

        // The material backend allocated its descriptor sets in this pipeline's constructor. If the
        // shader was recompiled between then and now, those sets belong to a different contract than
        // the pipeline layout about to be built, and every dispatch through the immediate path would
        // bind them — the exact "N total descriptors, but M total descriptors" the layer reports. Say
        // it with both numbers and rebuild the backend, rather than leaving it to be found on a GPU.
        if ( m_VulkanMaterialBackend && m_VulkanMaterialBackend->GetShaderGeneration() != m_ShaderGeneration )
        {
            const auto&    backendLayouts = m_VulkanMaterialBackend->GetLayouts();
            const uint32_t was =
                 backendLayouts.empty() || !backendLayouts[0] ? 0 : backendLayouts[0]->DescriptorCount();
            const uint32_t now = m_Layouts.empty() || !m_Layouts[0] ? 0 : m_Layouts[0]->DescriptorCount();

            LOG_ERROR( "ComputePipeline '{}': shader '{}' was recompiled between this pipeline's "
                       "construction and its Invalidate (generation {} -> {}); its material's descriptor "
                       "sets describe {} descriptor(s) and the pipeline layout would describe {}. "
                       "Rebuilding the material against the new layout.",
                       m_Specification.DebugName, vulkanShader->GetName(),
                       m_VulkanMaterialBackend->GetShaderGeneration(), m_ShaderGeneration, was, now );

            m_VulkanMaterialBackend = std::make_unique<VulkanMaterialBackend>( m_Specification.Shader );
        }

        const std::vector<VkDescriptorSetLayout> descriptorSetLayouts = RawHandles( m_Layouts );

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
        pipelineLayoutCreateInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>( descriptorSetLayouts.size() );
        pipelineLayoutCreateInfo.pSetLayouts    = descriptorSetLayouts.data();

        // Must outlive vkCreatePipelineLayout: pPushConstantRanges is read at the call below, so this
        // cannot live inside the if-block (a dangling stack pointer here reads garbage sizes in Release).
        VkPushConstantRange vulkanPushConstantRange{};

        const auto& pushConstantRange = vulkanShader->GetShaderPushConstant();
        if ( pushConstantRange )
        {
            vulkanPushConstantRange = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                        .offset     = pushConstantRange->Offset,
                                        .size       = pushConstantRange->Size };

            pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
            pipelineLayoutCreateInfo.pPushConstantRanges    = &vulkanPushConstantRange;
        }

        VK_CHECK_RESULT( vkCreatePipelineLayout( device, &pipelineLayoutCreateInfo, nullptr, &m_ComputePipelineLayout ) );

        // Shared device-wide, disk-persisted pipeline cache — the old per-pipeline cache created here was
        // empty every time and cached nothing across (or within) runs.
        const VkPipelineCache pipelineCache =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetPipelineCache();

        const VkComputePipelineCreateInfo pipelineInfo{ .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                        .stage  = *computeStage,
                                                        .layout = m_ComputePipelineLayout };

        VK_CHECK_RESULT(
             vkCreateComputePipelines( device, pipelineCache, 1, &pipelineInfo, nullptr, &m_ComputePipeline ) );

        VKUtils::SetDebugUtilsObjectName( device, VK_OBJECT_TYPE_PIPELINE, m_Specification.Shader->GetName(), m_ComputePipeline );

        m_VulkanMaterialBackend->InitializeDefaults();
    }

    void VulkanPipelineCompute::Release()
    {
        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();
        if ( m_ComputePipeline != VK_NULL_HANDLE )
        {
            vkDestroyPipeline( device, m_ComputePipeline, nullptr );
            m_ComputePipeline = VK_NULL_HANDLE;
        }

        if ( m_ComputePipelineLayout != VK_NULL_HANDLE )
        {
            vkDestroyPipelineLayout( device, m_ComputePipelineLayout, nullptr );
            m_ComputePipelineLayout = VK_NULL_HANDLE;
        }

        if ( m_InFramePool != VK_NULL_HANDLE )
        {
            // Frees every ring set allocated from it.
            vkDestroyDescriptorPool( device, m_InFramePool, nullptr );
            m_InFramePool = VK_NULL_HANDLE;
            m_InFrameRing.clear();
            m_InFrameCursor = 0;
        }

        // Released LAST: everything above was built from these, so they may only be let go once nothing
        // is standing on them. This is the release order the ownership is there to make obvious.
        m_Layouts.clear();

        m_ActiveComputeCommandBuffer = VK_NULL_HANDLE;
    }

    void VulkanPipelineCompute::BindDescriptorSets( VkDescriptorSet /*descriptorSet*/, uint32_t frameIndex )
    {
        // This will be used by the executor
        m_VulkanMaterialBackend->BindDescriptorSets( m_ActiveComputeCommandBuffer, m_ComputePipelineLayout,
                                                     VK_PIPELINE_BIND_POINT_COMPUTE, frameIndex );
    }

    void VulkanPipelineCompute::UpdateDescriptorSet( uint32_t /*frameIndex*/,
                                                     const std::vector<VkWriteDescriptorSet>& writes,
                                                     VkDescriptorSet                          descriptorSet,
                                                     uint32_t /*setIndex*/ /*= 0 */ )
    {
        std::vector<VkWriteDescriptorSet> modifiedWrites = writes;
        for ( auto& write : modifiedWrites )
        {
            write.dstSet = descriptorSet;
        }
        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();        vkUpdateDescriptorSets( device, static_cast<uint32_t>( modifiedWrites.size() ), modifiedWrites.data(), 0,
                                nullptr );
    }

} // namespace Desert::Graphic::API::Vulkan
