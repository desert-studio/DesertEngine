#include <algorithm>
#include <array>
#include <utility>
#include <Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRecordContext.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuBatch.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgPassBindings.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/WriteDescriptorSetBuilder.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

namespace Desert::Graphic::API::Vulkan
{
    VulkanPipelineCompute::VulkanPipelineCompute( const ComputePipelineSpecification& spec ) 
        : m_Specification( spec )
    {
        // The unbuilt record: no handles yet, the backend the first Invalidate checks against the shader.
        auto unbuilt     = std::make_shared<BuiltCompute>();
        unbuilt->Backend = std::make_shared<VulkanMaterialBackend>( spec.Shader );
        Publish( std::move( unbuilt ), false );
    }

    VulkanPipelineCompute::~VulkanPipelineCompute()
    {
        // At once: the last owner of a pipeline has no frame recording with it (m_Owned dies with this object).
        m_Current.store( nullptr, std::memory_order_release );
    }

    VulkanPipelineCompute::BuiltCompute::~BuiltCompute()
    {
        if ( Pipeline == VK_NULL_HANDLE && Layout == VK_NULL_HANDLE && m_BatchPool == VK_NULL_HANDLE )
            return;
        VkDevice device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        if ( Pipeline != VK_NULL_HANDLE )
            vkDestroyPipeline( device, Pipeline, nullptr );
        if ( Layout != VK_NULL_HANDLE )
            vkDestroyPipelineLayout( device, Layout, nullptr );
        if ( m_BatchPool != VK_NULL_HANDLE )
            vkDestroyDescriptorPool( device, m_BatchPool, nullptr ); // frees every ring set allocated from it
        // Layouts and Backend are released after (member destruction follows this body): everything above
        // was built from the layouts.
    }

    void VulkanPipelineCompute::Publish( std::shared_ptr<const BuiltCompute> next, const bool deferred ) noexcept
    {
        std::shared_ptr<const BuiltCompute> replaced = std::move( m_Owned );
        m_Owned                                      = std::move( next );
        m_Current.store( m_Owned.get(), std::memory_order_release );
        if ( !replaced || !deferred )
            return; // nothing to retire, or no frame records with it: `replaced` dies here

        // THE OLD RECORD OUTLIVES EVERY FRAME THAT MAY STILL READ IT: a render worker of frame N-1 may be
        // binding its pipeline, layout or a ring set while this rebuild runs on the game thread of frame N
        // (UE: deferred deletion of the replaced PSO). At once only with no renderer.
        const auto ctx       = EngineContext::GetInstance().GetRendererContext();
        auto*      allocator = ctx ? SP_CAST( VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;
        if ( allocator != nullptr )
            allocator->RT_DestroyLater( [held = std::move( replaced )]() mutable { held.reset(); } );
    }

    void VulkanPipelineCompute::AddBoundKey( const uint32_t binding )
    {
        ++m_BoundKeysVersion;
        const auto at = std::lower_bound( m_BoundBindingList.begin(), m_BoundBindingList.end(), binding );
        if ( at == m_BoundBindingList.end() || *at != binding )
            m_BoundBindingList.insert( at, binding );
    }

    ComputePipeline& VulkanPipelineCompute::SetInput( uint32_t binding, Image* image, RDG::Access declared,
                                                      RDG::SubresourceRange range )
    {
        const InputBinding input{ image, dynamic_cast<IVulkanImage*>( image ), declared, range };
        if ( m_BoundInputs.insert_or_assign( binding, input ).second )
            AddBoundKey( binding );
        return *this;
    }

    ComputePipeline& VulkanPipelineCompute::SetSampler( uint32_t binding, const RDG::SamplerDesc& desc )
    {
        const auto [entry, inserted] = m_BoundSamplers.try_emplace( binding );
        if ( inserted )
            AddBoundKey( binding );
        else if ( entry->second.Sampler != VK_NULL_HANDLE && entry->second.Desc == desc )
            return *this; // the same state: the cache's sampler for it is already held
        entry->second.Desc                         = desc;
        const Common::ResultStr<VkSampler> sampler = EngineSamplerCache().Acquire( RdgSamplerCreateInfo( desc ) );
        entry->second.Sampler                      = VK_NULL_HANDLE;
        if ( sampler.IsSuccess() )
            entry->second.Sampler = sampler.GetValue();
        return *this;
    }

    ComputePipeline& VulkanPipelineCompute::SetOutput( uint32_t binding, Image* image, uint32_t mip )
    {
        const OutputBinding output{ image, dynamic_cast<IVulkanImage*>( image ), mip };
        if ( m_BoundOutputs.insert_or_assign( binding, output ).second )
            AddBoundKey( binding );
        return *this;
    }

    ComputePipeline& VulkanPipelineCompute::SetStorageBuffer( uint32_t                        binding,
                                                              ShaderResources::StorageBuffer* buffer )
    {
        const StorageBufferBinding bound{
             buffer, dynamic_cast<ShaderResources::API::Vulkan::VulkanStorageBuffer*>( buffer ) };
        if ( m_BoundStorageBuffers.insert_or_assign( binding, bound ).second )
            AddBoundKey( binding );
        return *this;
    }

    ComputePipeline& VulkanPipelineCompute::SetPushConstants( const void* data, uint32_t size )
    {
        const auto* bytes = reinterpret_cast<const std::byte*>( data );
        const bool  had   = !m_BoundPushConstants.empty();
        m_BoundPushConstants.assign( bytes, bytes + size );
        if ( had == m_BoundPushConstants.empty() )
            ++m_BoundKeysVersion;
        return *this;
    }

    Common::BoolResultStr VulkanPipelineCompute::WriteBoundResources( const BuiltCompute& built,
                                                                      VkDescriptorSet     descriptorSet )
    {
        VulkanMaterialBackend* backend = built.Backend.get();
        if ( backend == nullptr )
            return Common::MakeFormattedError( "ComputePipeline '{}': no material backend; dispatch skipped",
                                               m_Specification.DebugName );
        const size_t bound =
             m_BoundInputs.size() + m_BoundOutputs.size() + m_BoundSamplers.size() + m_BoundStorageBuffers.size();
        if ( bound > kMaxBoundWrites )
        {
            return Common::MakeFormattedError( "ComputePipeline '{}': {} bound bindings, more than the {} one "
                                               "dispatch writes; dispatch skipped",
                                               m_Specification.DebugName, bound, kMaxBoundWrites );
        }
        // THE WRITES ON THE STACK (UE FVulkanDescriptorSetWriter): one write per bound binding, pointing at its
        // info in the arrays beside it, which outlive vkUpdateDescriptorSets below. No allocation per dispatch.
        std::array<VkDescriptorImageInfo, kMaxBoundWrites>  infos;
        std::array<VkDescriptorBufferInfo, kMaxBoundWrites> bufferInfos;
        std::array<VkWriteDescriptorSet, kMaxBoundWrites>   writes;
        uint32_t                                            infoCount   = 0;
        uint32_t                                            bufferCount = 0;
        uint32_t                                            writeCount  = 0;

        // Outputs: bind the chosen mip view as a storage image (the caller has already put the image
        // into GENERAL — we never transition here, so every route that writes them shares this).
        for ( const auto& [binding, out] : m_BoundOutputs )
        {
            if ( out.Vulkan == nullptr )
                continue;
            VkDescriptorImageInfo& info = infos[infoCount++];
            info                 = { VK_NULL_HANDLE, out.Vulkan->GetMipView( out.Mip ), VK_IMAGE_LAYOUT_GENERAL };
            writes[writeCount++] = DescriptorSetBuilder::GetStorageWDS( backend, 0, 0, binding, 1, &info );
        }

        // Inputs: separate textures (SAMPLED_IMAGE), the view alone. Every input names the view it declared (one
        // mip, or all) in the layout its declared access guarantees: the graph's transition in a graph node, the
        // pipeline's own one (RecordTransitionedDispatch) outside it. No fallback is substituted: an input
        // without a view drops the dispatch with its binding named.
        const auto separateImageWrite =
             []( uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* info ) noexcept
        {
            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstBinding      = binding;
            write.descriptorCount = 1;
            write.descriptorType  = type;
            write.pImageInfo      = info;
            return write;
        };
        for ( const auto& [binding, input] : m_BoundInputs )
        {
            IVulkanImage* img = input.Vulkan;
            if ( !img )
            {
                return Common::MakeFormattedError(
                     "ComputePipeline '{}': input at binding {} is not a Vulkan image; "
                     "dispatch skipped",
                     m_Specification.DebugName, binding );
            }
            const auto& r = img->GetResource();

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only the Vulkan API makes graph
            // textures
            auto*      graphTexture = dynamic_cast<VulkanRdgTexture*>( img->GetGraphTexture().get() );
            const auto resolved     = SampledSubresourceView(
                 input.Range, r.ImageView, [img]( uint32_t mip ) { return img->GetMipView( mip ); },
                 graphTexture );
            if ( !resolved.IsSuccess() )
            {
                return Common::MakeFormattedError(
                     "ComputePipeline '{}': input at binding {} has no view of the subresource it declared: {}; "
                     "dispatch skipped",
                     m_Specification.DebugName, binding, resolved.GetError() );
            }
            const VkImageView      view           = resolved.GetValue();
            const RDG::ImageLayout declaredLayout = RDG::GetAccessState( input.Declared ).Layout;
            if ( declaredLayout != RDG::ImageLayout::ShaderReadOnly &&
                 declaredLayout != RDG::ImageLayout::General )
            {
                return Common::MakeFormattedError(
                     "ComputePipeline '{}': input at binding {} is declared as {}, which does not "
                     "leave the image sampleable; dispatch skipped",
                     m_Specification.DebugName, binding, RDG::GetAccessName( input.Declared ) );
            }
            if ( view == VK_NULL_HANDLE )
            {
                return Common::MakeFormattedError( "ComputePipeline '{}': input at binding {} has no image view; "
                                                   "dispatch skipped",
                                                   m_Specification.DebugName, binding );
            }
            VkDescriptorImageInfo& info = infos[infoCount++];
            info                        = { VK_NULL_HANDLE, view, RdgVulkanLayout( declaredLayout ) };
            writes[writeCount++]        = separateImageWrite( binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &info );
        }

        // Shared sampler states (VK_DESCRIPTOR_TYPE_SAMPLER): the engine sampler cache's, acquired by SetSampler.
        for ( const auto& [binding, sampler] : m_BoundSamplers )
        {
            if ( sampler.Sampler == VK_NULL_HANDLE )
            {
                // The cache refused the state when it was set; asked again only to name the reason.
                const Common::ResultStr<VkSampler> refused =
                     EngineSamplerCache().Acquire( RdgSamplerCreateInfo( sampler.Desc ) );
                if ( refused.IsSuccess() )
                {
                    return Common::MakeFormattedError( "ComputePipeline '{}': sampler at binding {} was refused "
                                                       "when it was set; dispatch skipped",
                                                       m_Specification.DebugName, binding );
                }
                return Common::MakeFormattedError( "ComputePipeline '{}': sampler at binding {}: {}; dispatch "
                                                   "skipped",
                                                   m_Specification.DebugName, binding, refused.GetError() );
            }
            VkDescriptorImageInfo& info = infos[infoCount++];
            info                        = { sampler.Sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            writes[writeCount++]        = separateImageWrite( binding, VK_DESCRIPTOR_TYPE_SAMPLER, &info );
        }

        // Storage buffers (read-write; e.g. a luminance histogram): the active view's copy for this frame
        // (or the one shared buffer when persistent). A buffer with no copy to bind drops the dispatch: a
        // compute set left pointing at whatever the ring slot held before is a silent wrong result, not a
        // fallback.
        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        for ( const auto& [binding, buffer] : m_BoundStorageBuffers )
        {
            if ( buffer.Vulkan == nullptr )
                continue;
            ShaderResources::API::Vulkan::ViewCopyBinding copy;
            const auto copyBound = buffer.Vulkan->BindActiveCopy( frameIndex, copy );
            if ( !copyBound.IsSuccess() )
            {
                return Common::MakeFormattedError(
                     "ComputePipeline '{}': storage buffer at binding {} has no copy to bind; dispatch "
                     "skipped -- {}",
                     m_Specification.DebugName, binding, copyBound.GetError() );
            }
            VkDescriptorBufferInfo& info = bufferInfos[bufferCount++];
            info                         = copy.Info;
            writes[writeCount++]         = DescriptorSetBuilder::GetStorageWDS( backend, 0, 0, binding, 1, &info );
        }

        // Retarget every write at the supplied set, then update it in one shot.
        for ( uint32_t i = 0; i < writeCount; ++i )
            writes[i].dstSet = descriptorSet;
        if ( writeCount != 0 )
        {
            VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                   ->GetVulkanLogicalDevice();
            vkUpdateDescriptorSets( device, writeCount, writes.data(), 0, nullptr );
        }
        return Common::MakeSuccess( true );
    }

    void VulkanPipelineCompute::RecordDescriptorsAndDispatch( const BuiltCompute& built, VkCommandBuffer cmd,
                                                              VkDescriptorSet descriptorSet, uint32_t groupsX,
                                                              uint32_t groupsY, uint32_t groupsZ )
    {
        if ( built.Pipeline == VK_NULL_HANDLE )
        {
            LOG_ERROR( "ComputePipeline '{}' is not built -- the dispatch is not recorded.",
                       m_Specification.DebugName );
            return;
        }
        if ( const Common::BoolResultStr written = WriteBoundResources( built, descriptorSet );
             !written.IsSuccess() )
        {
            LOG_ERROR( "{}", written.GetError() );
            return;
        }

        // A compute bind outside VulkanRendererAPI::BindComputePassState: its bind-skip state is stale.
        VulkanBoundCompute::ForeignBind();
        vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, built.Pipeline );
        vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, built.Layout, 0, 1, &descriptorSet, 0,
                                 nullptr );
        // The bindless table's set is the table's one live set, never one of this pipeline's (see
        // BuiltCompute::NextBatchSet).
        if ( const VulkanBindlessTextureTable* bindless = BindlessTableIn( built.Layouts ) )
        {
            const VkDescriptorSet table = bindless->GetDescriptorSet();
            vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, built.Layout, kBindlessSetIndex, 1,
                                     &table, 0, nullptr );
        }

        if ( !m_BoundPushConstants.empty() )
            vkCmdPushConstants( cmd, built.Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                static_cast<uint32_t>( m_BoundPushConstants.size() ),
                                m_BoundPushConstants.data() );

        vkCmdDispatch( cmd, groupsX, groupsY, groupsZ );
    }

    void VulkanPipelineCompute::Dispatch( uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ )
    {
        // ONE read: the pipeline, its layout and the backend's persistent set of one build.
        const BuiltCompute&    built   = GetBuilt();
        VulkanMaterialBackend* backend = built.Backend.get();
        if ( !backend )
            return;

        auto cmdResult = CommandBufferAllocator::GetInstance().RT_GetCommandBufferCompute( true );
        if ( !cmdResult.IsSuccess() )
            return;
        VkCommandBuffer cmd = cmdResult.GetValue();

        // Immediate path: transition outputs to GENERAL, dispatch, transition back to SHADER_READ so the
        // result can be sampled, then submit + wait. Reuses the persistent set 0 (safe — we wait below).
        RecordTransitionedDispatch( built, cmd, backend->GetDescriptorSet( 0, 0 ), groupsX, groupsY, groupsZ );

        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferCompute( cmd );
    }

    void VulkanPipelineCompute::Record( GpuBatch& batch, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ )
    {
        const BuiltCompute& built = GetBuilt();
        VkCommandBuffer     cmd   = RecordingBuffer( batch );
        if ( !built.Backend || cmd == VK_NULL_HANDLE )
        {
            LOG_ERROR( "ComputePipeline '{}': {} -- the dispatch is not recorded.", m_Specification.DebugName,
                       cmd == VK_NULL_HANDLE ? "the GPU batch was already submitted" : "no material backend" );
            return;
        }

        // A RING set, not the persistent one: every dispatch of a batch is still pending when the next is
        // recorded, and rewriting a set a recorded command references is undefined. The ring belongs to
        // this pipeline's record, so its only users are this build's own dispatches — the prefilter's mips
        // are the most one batch records, well under the ring's size.
        const VkDescriptorSet set = built.NextBatchSet();
        if ( set == VK_NULL_HANDLE )
        {
            LOG_ERROR( "ComputePipeline '{}': the build has no set 0 layout -- the dispatch is not recorded.",
                       m_Specification.DebugName );
            return;
        }

        RecordTransitionedDispatch( built, cmd, set, groupsX, groupsY, groupsZ );
    }

    void VulkanPipelineCompute::RecordTransitionedDispatch( const BuiltCompute& built, VkCommandBuffer cmd,
                                                            VkDescriptorSet descriptorSet, uint32_t groupsX,
                                                            uint32_t groupsY, uint32_t groupsZ )
    {
        // Explicit stages and access, because a batch puts the NEXT consumer (a blit, another dispatch,
        // a readback) in the same command buffer: the layout alone orders nothing there.
        // Outside the graph nothing else puts an input where its declared access says it is, so this does:
        // the descriptor names that layout (RecordDescriptorsAndDispatch), never the image's record.
        for ( const auto& [binding, in] : m_BoundInputs )
            if ( IVulkanImage* img = in.Vulkan )
                img->TransitionLayout( cmd, RdgVulkanLayout( RDG::GetAccessState( in.Declared ).Layout ),
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
        for ( const auto& [binding, out] : m_BoundOutputs )
            if ( IVulkanImage* img = out.Vulkan )
                img->TransitionLayout( cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );

        RecordDescriptorsAndDispatch( built, cmd, descriptorSet, groupsX, groupsY, groupsZ );

        for ( const auto& [binding, out] : m_BoundOutputs )
            if ( IVulkanImage* img = out.Vulkan )
                img->TransitionLayout( cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT );
    }

    VkDescriptorSet VulkanPipelineCompute::BuiltCompute::NextBatchSet() const noexcept
    {
        const std::scoped_lock lock( m_BatchLock );
        if ( m_BatchPool == VK_NULL_HANDLE )
        {
            // The ring is allocated from the layout THIS RECORD was built from, never from the shader's
            // current one. Re-reading the shader here is what produced the mismatch this arrangement exists
            // to prevent: a shader recompiled after the pipeline was built publishes a layout with a different
            // descriptor count, and a set allocated from it cannot be bound to a pipeline layout made from the
            // old one ("has 8 total descriptors, but ... has 9").
            VkDescriptorSetLayout layout0 = VK_NULL_HANDLE;
            if ( !Layouts.empty() && Layouts[0] )
                layout0 = Layouts[0]->Handle();
            if ( layout0 == VK_NULL_HANDLE )
                return VK_NULL_HANDLE;

            VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                   ->GetVulkanLogicalDevice();

            // Pool sized from the record's own layouts — the same contract the sets below are allocated
            // from. The bindless table's layout is left out: its set is the table's own
            // (RecordDescriptorsAndDispatch), and its update-after-bind array would size this pool at the
            // table's whole capacity.
            const VulkanBindlessTextureTable* bindless = BindlessTableIn( Layouts );
            std::unordered_map<int, uint32_t> countsByType;
            for ( const auto& layout : Layouts )
            {
                if ( !layout || ( bindless != nullptr && layout == bindless->GetLayout() ) )
                    continue;
                for ( const auto& binding : layout->Bindings() )
                    countsByType[static_cast<int>( binding.descriptorType )] += binding.descriptorCount;
            }

            std::vector<VkDescriptorPoolSize> poolSizes;
            poolSizes.reserve( countsByType.size() );
            for ( const auto& [type, count] : countsByType )
                poolSizes.push_back( { static_cast<VkDescriptorType>( type ), count * kBatchRingSize } );
            if ( poolSizes.empty() )
                poolSizes.push_back( { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kBatchRingSize } );

            const VkDescriptorPoolCreateInfo poolInfo{ .sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                       .maxSets = kBatchRingSize,
                                                       .poolSizeCount = static_cast<uint32_t>( poolSizes.size() ),
                                                       .pPoolSizes    = poolSizes.data() };
            VK_CHECK_RESULT( vkCreateDescriptorPool( device, &poolInfo, nullptr, &m_BatchPool ) );

            std::vector<VkDescriptorSetLayout> layouts( kBatchRingSize, layout0 );
            m_BatchRing.resize( kBatchRingSize );
            const VkDescriptorSetAllocateInfo allocInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                         .descriptorPool     = m_BatchPool,
                                                         .descriptorSetCount = kBatchRingSize,
                                                         .pSetLayouts        = layouts.data() };
            VK_CHECK_RESULT( vkAllocateDescriptorSets( device, &allocInfo, m_BatchRing.data() ) );
        }
        const VkDescriptorSet set = m_BatchRing[m_BatchCursor];
        m_BatchCursor             = ( m_BatchCursor + 1 ) % kBatchRingSize;
        return set;
    }

    std::shared_ptr<const RDG::OtherRouteFill> VulkanPipelineCompute::GetRouteFill() const
    {
        const auto* shader = dynamic_cast<const VulkanShader*>( m_Specification.Shader.get() );
        if ( shader == nullptr )
            return RDG::NoOtherRouteFill();
        const std::shared_ptr<const RdgSlotTable>& slots = shader->GetRdgSlots();
        const std::scoped_lock                     lock( m_RouteFillLock );
        if ( m_RouteFill.Fill && m_RouteFill.Slots == slots && m_RouteFill.BoundKeysVersion == m_BoundKeysVersion )
            return m_RouteFill.Fill;
        // The same set-0 keys DispatchCompute hands ResolveRdgPassBindings as the other route.
        RdgOtherRoute other;
        for ( const uint32_t binding : GetBoundBindings() )
            other.Filled.push_back( RdgSlotKey{ .Set = 0, .Binding = binding } );
        other.PushConstants = !m_BoundPushConstants.empty();
        // A shader that was never reflected has no table: the table of an empty reflection is the empty table.
        static const RdgSlotTable kUnreflected;
        const RdgSlotTable&       table = slots ? *slots : kUnreflected;
        m_RouteFill.Fill = std::make_shared<const RDG::OtherRouteFill>( MakeOtherRouteFill( table, other ) );
        m_RouteFill.Slots            = slots;
        m_RouteFill.BoundKeysVersion = m_BoundKeysVersion;
        return m_RouteFill.Fill;
    }

    void VulkanPipelineCompute::Invalidate()
    {
        RecordShaderCodeGeneration();

        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();
        const auto vulkanShader = sp_cast<VulkanShader>( m_Specification.Shader );
        // THE PROGRAM READ ONCE, held for the build: the stage, the layouts and the push range below come
        // from one compile even if a reload publishes the next one meanwhile.
        const std::shared_ptr<const VulkanShader::CompiledProgram> program = vulkanShader->ShareProgram();

        // THE RECORD THIS BUILD WRITES. Nothing a reader sees changes until Publish below: a render worker of
        // the previous frame keeps binding the published record while this runs on the game thread.
        auto next = std::make_shared<BuiltCompute>();
        // The backend of the published record carries over until the generation check below says otherwise.
        next->Backend = GetBuilt().Backend;

        // THE STAGE IS FOUND BY WHAT IT IS, NOT BY WHERE IT SITS. This used to be
        // `GetPipelineShaderStageCreateInfos()[0]` at the pipeline-info below, which reads a vector a
        // failed compile leaves empty (SIGSEGV, after 23 validation errors about a descriptor pool of
        // size zero) and, when the vector is full, happily hands vkCreateComputePipelines the VERTEX
        // stage of a graphics program reached by name. Both are refusals, so both are refused here and
        // nothing is built: the caller sees it because ComputePipeline::Create asks this object for the
        // handle it produced.
        const VkPipelineShaderStageCreateInfo* computeStage = nullptr;
        for ( const VkPipelineShaderStageCreateInfo& stage : program->StageInfos )
            if ( stage.stage == VK_SHADER_STAGE_COMPUTE_BIT )
                computeStage = &stage;
        if ( !computeStage )
        {
            LOG_ERROR( "ComputePipeline '{}': shader '{}' carries no compute stage ({} stage(s) in all) "
                       "— nothing is built and the dispatches using it are skipped.",
                       m_Specification.DebugName, vulkanShader->GetName(),
                       program->StageInfos.size() );
            Publish( std::move( next ), true ); // the unbuilt record: no handles, the backend kept
            return;
        }

        // Captured ONCE, here, and used for everything this record binds: the pipeline layout below,
        // the batch ring, and the pool that ring is allocated from. Holding the references is what
        // keeps the layouts alive if the shader recompiles under us, and using only these is what keeps
        // the set and the pipeline layout describing the same contract.
        next->Layouts          = program->SetLayouts;
        next->ShaderGeneration = vulkanShader->GetCodeGeneration();

        // The material backend allocated its descriptor sets against the shader of its own construction. If
        // the shader was recompiled between then and now, those sets belong to a different contract than
        // the pipeline layout about to be built, and every dispatch through the immediate path would
        // bind them — the exact "N total descriptors, but M total descriptors" the layer reports. Say
        // it with both numbers and give the record a new backend, rather than leaving it to be found on a
        // GPU. The old backend stays with the old record (its sets may be bound by a frame in flight).
        if ( next->Backend && next->Backend->GetShaderGeneration() != next->ShaderGeneration )
        {
            const auto&    backendLayouts = next->Backend->GetLayouts();
            const uint32_t was =
                 backendLayouts.empty() || !backendLayouts[0] ? 0 : backendLayouts[0]->DescriptorCount();
            const uint32_t now =
                 next->Layouts.empty() || !next->Layouts[0] ? 0 : next->Layouts[0]->DescriptorCount();

            LOG_ERROR( "ComputePipeline '{}': shader '{}' was recompiled between this pipeline's "
                       "material and its Invalidate (generation {} -> {}); its material's descriptor "
                       "sets describe {} descriptor(s) and the pipeline layout would describe {}. "
                       "Rebuilding the material against the new layout.",
                       m_Specification.DebugName, vulkanShader->GetName(), next->Backend->GetShaderGeneration(),
                       next->ShaderGeneration, was, now );

            next->Backend = std::make_shared<VulkanMaterialBackend>( m_Specification.Shader );
        }

        const std::vector<VkDescriptorSetLayout> descriptorSetLayouts = RawHandles( next->Layouts );

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
        pipelineLayoutCreateInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = static_cast<uint32_t>( descriptorSetLayouts.size() );
        pipelineLayoutCreateInfo.pSetLayouts    = descriptorSetLayouts.data();

        // Must outlive vkCreatePipelineLayout: pPushConstantRanges is read at the call below, so this
        // cannot live inside the if-block (a dangling stack pointer here reads garbage sizes in Release).
        VkPushConstantRange vulkanPushConstantRange{};

        const auto& pushConstantRange = program->Reflection.PushConstantRanges;
        if ( pushConstantRange )
        {
            vulkanPushConstantRange = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                        .offset     = pushConstantRange->Offset,
                                        .size       = pushConstantRange->Size };

            pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
            pipelineLayoutCreateInfo.pPushConstantRanges    = &vulkanPushConstantRange;
        }

        VK_CHECK_RESULT( vkCreatePipelineLayout( device, &pipelineLayoutCreateInfo, nullptr, &next->Layout ) );

        // Shared device-wide, disk-persisted pipeline cache — the old per-pipeline cache created here was
        // empty every time and cached nothing across (or within) runs.
        const VkPipelineCache pipelineCache =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetPipelineCache();

        // Subgroup control (ComputePipelineSpecification): the stage is copied so the shader's own create info is
        // never edited, and the required size is chained in front of whatever the stage already chains.
        VkPipelineShaderStageCreateInfo                     stage = *computeStage;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo requiredSize{
             .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };
        if ( m_Specification.RequireFullSubgroups )
            stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        if ( m_Specification.RequiredSubgroupSize != 0 )
        {
            requiredSize.pNext                = const_cast<void*>( stage.pNext );
            requiredSize.requiredSubgroupSize = m_Specification.RequiredSubgroupSize;
            stage.pNext                       = &requiredSize;
        }

        const VkComputePipelineCreateInfo pipelineInfo{
             .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = stage, .layout = next->Layout };

        {
            const Core::ScopedShaderPhase timer( Core::ShaderPhase::PipelineCreate );
            VK_CHECK_RESULT(
                 vkCreateComputePipelines( device, pipelineCache, 1, &pipelineInfo, nullptr, &next->Pipeline ) );
        }

        VKUtils::SetDebugUtilsObjectName( device, VK_OBJECT_TYPE_PIPELINE, m_Specification.Shader->GetName(),
                                          next->Pipeline );

        // ONE STORE makes the whole build visible; the replaced record is retired after the frames in flight.
        m_ActiveComputeCommandBuffer = VK_NULL_HANDLE;
        Publish( std::move( next ), true );
    }

    void VulkanPipelineCompute::Release()
    {
        ReleaseHandles( true );
    }

    void VulkanPipelineCompute::ReleaseHandles( const bool deferred )
    {
        // THE HANDLES OUTLIVE EVERY FRAME THAT MAY STILL USE THEM. A release on the game thread of frame N
        // while a render worker of frame N-1 may have read the pipeline, its layout or a ring set into a
        // command buffer: the published record goes through the allocator's deletion queue (UE: deferred
        // deletion of the replaced PSO) and its destructor frees them, with the set layouts they were built
        // from released last. At once only from the destructor or with no renderer. The material backend is
        // kept: GetVulkanMaterialBackend answers it until the next Invalidate, as before.
        auto unbuilt                 = std::make_shared<BuiltCompute>();
        unbuilt->Backend             = GetBuilt().Backend;
        m_ActiveComputeCommandBuffer = VK_NULL_HANDLE;
        Publish( std::move( unbuilt ), deferred );
    }

    void VulkanPipelineCompute::BindDescriptorSets( VkDescriptorSet /*descriptorSet*/, uint32_t frameIndex )
    {
        // This will be used by the executor. One read: the backend's sets and the layout of one build.
        const BuiltCompute& built = GetBuilt();
        if ( !built.Backend )
            return;
        VulkanBoundCompute::ForeignBind(); // compute sets bound outside BindComputePassState
        if ( !built.Backend->BindDescriptorSets( m_ActiveComputeCommandBuffer, built.Layout,
                                                 VK_PIPELINE_BIND_POINT_COMPUTE, frameIndex ) )
        {
            LOG_ERROR( "ComputePipeline '{}': its material's sets were not bound", m_Specification.DebugName );
        }
    }

} // namespace Desert::Graphic::API::Vulkan
