#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>

#include <limits>
#include <cstring>
#include <Engine/ShaderResources/API/Vulkan/VulkanUniformBuffer.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanUniformImage2D.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanUniformImageCube.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/WriteDescriptorSetBuilder.hpp>

namespace Desert::Graphic::API::Vulkan
{
    VulkanMaterialBackend::VulkanMaterialBackend( const std::shared_ptr<Shader>& shader )
         : MaterialBackend( shader ), m_VulkanShader( SP_CAST( VulkanShader, shader ) )
    {
        // Captured ONCE, and kept. Every set this backend allocates below belongs to these layouts for
        // as long as the backend lives, so a recompile of the shader cannot leave the sets pointing at
        // a contract that no longer exists — see VulkanDescriptorSetLayout.hpp.
        m_Layouts          = m_VulkanShader->GetAllDescriptorSetLayouts();
        m_ShaderGeneration = m_VulkanShader->GetReloadGeneration();

        // A SHADER WITH NO LAYOUTS HAS NOTHING TO ALLOCATE FOR, AND ASKING ANYWAY IS TWENTY VALIDATION
        // ERRORS. That is what a shader whose first compile failed carries: no stages, so no reflection,
        // so no set layouts — and the two calls below then made a pool with `maxSets = 0` and ten
        // `vkAllocateDescriptorSets` with `descriptorSetCount = 0`, one per (frame x renderer slot).
        // Measured on the live editor with one broken shader: 20 of the run's 24 validation errors came
        // from here, drowning the two that said what had actually gone wrong. The backend stays valid and
        // EMPTY — HasDescriptorSets() already answers false for it, which is the state every consumer
        // reads as "nothing to bind". Same family as the crash Г21 is about: a container derived from a
        // shader that carries nothing, used as though it did.
        //
        // THE MESSAGE USED TO ASSERT A CAUSE IT HAD NOT CHECKED. It read "(it has no compiled stages)"
        // off `m_Layouts.empty()` alone, and the two are not the same fact: a program that compiled
        // perfectly well and simply declares no uniform, no storage block and no sampler publishes no
        // layouts either, and is entirely legal Vulkan. Ю11's `UIMatError` is the first such program in
        // the tree — a procedural fill with no parameters — and it was reported here as a broken shader.
        // The stage list is what actually answers the question, so it is what is asked.
        if ( m_Layouts.empty() )
        {
            if ( m_VulkanShader && m_VulkanShader->IsCompiled() )
            {
                LOG_INFO( "[Material] shader '{}' declares no descriptor resources; it draws from its "
                          "push constants alone.",
                          m_VulkanShader->GetName() );
            }
            else
            {
                LOG_ERROR( "[Material] shader '{}' publishes no descriptor set layouts because it has no "
                           "compiled stages — no descriptor pool is created and nothing binds through "
                           "this material.",
                           m_VulkanShader ? m_VulkanShader->GetName() : std::string( "<null>" ) );
            }
        }
        else
        {
            CreateDescriptorPool();
            AllocateDescriptorSets();
        }

        // Create a dummy buffer to initialize unused bindings
        VmaAllocator allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                                      ->GetVulkanAllocator()
                                      ->GetVMAAllocator();

        VkBufferCreateInfo      bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                               .size  = 65536,
                                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        // Host-visible + zero-initialized. Every uniform/storage binding starts out pointing at this
        // dummy buffer (see InitializeWithFallbacks). If it held uninitialized GPU memory, any binding
        // sampled before its real resource is bound — or one that is never bound — would read garbage,
        // which the lighting blows up into white/NaN (or near-zero black). Defined zeros make those
        // cases render as a stable black instead of flickering garbage.
        VmaAllocationCreateInfo allocInfo  = { .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                               .usage = VMA_MEMORY_USAGE_CPU_TO_GPU };

        VmaAllocationInfo dummyAllocInfo{};
        VK_CHECK_RESULT( vmaCreateBuffer( allocator, &bufferInfo, &allocInfo, &m_DummyBuffer, &m_DummyAllocation, &dummyAllocInfo ) );

        if ( dummyAllocInfo.pMappedData )
        {
            std::memset( dummyAllocInfo.pMappedData, 0, bufferInfo.size );
        }
    }

    VulkanMaterialBackend::~VulkanMaterialBackend()
    {
        // DEFERRED, like every other GPU object a mid-session destruction releases: a material dropped
        // while its sets are still in a command buffer in flight would otherwise free them under the GPU.
        // The dummy buffer goes the same way: the sets being released still point at it.
        const auto& allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator();
        allocator->RT_DestroyDescriptorPool( m_DescriptorPool );
        allocator->RT_DestroyBuffer( m_DummyBuffer, m_DummyAllocation );
    }

    void VulkanMaterialBackend::CreateDescriptorPool()
    {
        const uint32_t framesInFlight = EngineContext::GetInstance().GetMaxFramesInFlight();
        const uint32_t setCount       = static_cast<uint32_t>( m_Layouts.size() );

        auto& descriptorSets = m_VulkanShader->GetShaderDescriptorSets();

        uint32_t uniformBufferCount        = 0;
        uint32_t combinedImageSamplerCount = 0;
        uint32_t storageImageCount         = 0;
        uint32_t storageBufferCount        = 0;

        for ( const auto& [setIndex, descriptorSet] : descriptorSets )
        {
            uniformBufferCount += (uint32_t)descriptorSet.UniformBuffers.size();
            combinedImageSamplerCount += (uint32_t)descriptorSet.Image2DSamplers.size();
            combinedImageSamplerCount += (uint32_t)descriptorSet.Image3DSamplers.size();
            combinedImageSamplerCount += (uint32_t)descriptorSet.ImageCubeSamplers.size();
            storageImageCount += (uint32_t)descriptorSet.StorageImage2DSamplers.size();
            storageImageCount += (uint32_t)descriptorSet.StorageImage3DSamplers.size();
            storageBufferCount += (uint32_t)descriptorSet.StorageBuffers.size();
        }

        // One set per (frame in flight x RENDERER SLOT): each view records with its own descriptors, so
        // the pool has to hold that many. Slots are a small fixed number (EngineContext::kMaxRendererSlots)
        // and a set is a handful of descriptors, so this is a few kilobytes, not a real cost.
        const uint32_t slots = EngineContext::kMaxRendererSlots;

        uniformBufferCount *= framesInFlight * slots;
        combinedImageSamplerCount *= framesInFlight * slots;
        storageImageCount *= framesInFlight * slots;
        storageBufferCount *= framesInFlight * slots;

        std::vector<VkDescriptorPoolSize> poolSizes;
        if ( uniformBufferCount > 0 ) poolSizes.push_back( { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBufferCount } );
        if ( combinedImageSamplerCount > 0 ) poolSizes.push_back( { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, combinedImageSamplerCount } );
        if ( storageImageCount > 0 ) poolSizes.push_back( { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageImageCount } );
        if ( storageBufferCount > 0 ) poolSizes.push_back( { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageBufferCount } );

        if ( poolSizes.empty() ) poolSizes.push_back( { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } );

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>( poolSizes.size() );
        poolInfo.pPoolSizes    = poolSizes.data();
        poolInfo.maxSets       = framesInFlight * slots * setCount;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        VK_CHECK_RESULT( vkCreateDescriptorPool( SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                          ->GetVulkanLogicalDevice(), &poolInfo, nullptr, &m_DescriptorPool ) );
    }

    void VulkanMaterialBackend::AllocateDescriptorSets()
    {
        const uint32_t framesInFlight = EngineContext::GetInstance().GetMaxFramesInFlight();
        const uint32_t setCount       = static_cast<uint32_t>( m_Layouts.size() );

        const uint32_t slots = EngineContext::kMaxRendererSlots;

        m_DescriptorSets.assign( framesInFlight, std::vector<std::vector<VkDescriptorSet>>( slots ) );
        m_DescriptorSetsUpdateFrame.assign(
             framesInFlight,
             std::vector<std::vector<uint64_t>>(
                  slots, std::vector<uint64_t>( setCount, std::numeric_limits<uint64_t>::max() ) ) );
        m_FrameWrites.assign( framesInFlight, std::vector<FrameWriteRecord>( slots ) );
        // Fresh sets point at nothing, so every binding is owed a write.
        m_BoundCopies.assign( framesInFlight, std::vector<ShaderResources::DescriptorCopyRecord>( slots ) );

        const std::vector<VkDescriptorSetLayout> layouts = RawHandles( m_Layouts );

        for ( uint32_t frame = 0; frame < framesInFlight; ++frame )
        {
            for ( uint32_t slot = 0; slot < slots; ++slot )
            {
                m_DescriptorSets[frame][slot].resize( setCount );

                VkDescriptorSetAllocateInfo allocInfo{};
                allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocInfo.descriptorPool     = m_DescriptorPool;
                allocInfo.descriptorSetCount = setCount;
                allocInfo.pSetLayouts        = layouts.data();

                VK_CHECK_RESULT( vkAllocateDescriptorSets(
                     SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                          ->GetVulkanLogicalDevice(),
                     &allocInfo, m_DescriptorSets[frame][slot].data() ) );
            }
        }
    }

    VkDescriptorSet VulkanMaterialBackend::GetDescriptorSet( uint32_t frameIndex, uint32_t setIndex ) const
    {
        // The slot of the renderer that is RECORDING resolves here and nowhere else, so a write and the
        // bind that follows it can never land on different copies.
        const uint32_t slot = EngineContext::GetInstance().GetActiveRendererSlot();
        if ( frameIndex < m_DescriptorSets.size() && slot < m_DescriptorSets[frameIndex].size() &&
             setIndex < m_DescriptorSets[frameIndex][slot].size() )
        {
            return m_DescriptorSets[frameIndex][slot][setIndex];
        }
        return VK_NULL_HANDLE;
    }

    void VulkanMaterialBackend::NoteDescriptorWrite( uint32_t frameIndex, uint32_t binding, uint64_t handle )
    {
        const uint32_t slot = EngineContext::GetInstance().GetActiveRendererSlot();
        if ( frameIndex >= m_FrameWrites.size() || slot >= m_FrameWrites[frameIndex].size() )
            return;

        auto&          record        = m_FrameWrites[frameIndex][slot];
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( record.Frame != absoluteFrame )
        {
            record.Frame = absoluteFrame;
            record.Handles.clear();
        }
        record.Handles[binding] = handle;
    }

    void VulkanMaterialBackend::ReportSwallowedRebind( uint32_t frameIndex, uint32_t binding, uint64_t handle,
                                                       const char* what )
    {
        const uint32_t slot = EngineContext::GetInstance().GetActiveRendererSlot();
        if ( frameIndex >= m_FrameWrites.size() || slot >= m_FrameWrites[frameIndex].size() )
            return;

        const auto& record = m_FrameWrites[frameIndex][slot];
        if ( record.Frame != Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() )
            return; // nothing was written this frame; the stamp came from another mechanism
        const auto written = record.Handles.find( binding );
        if ( written == record.Handles.end() || written->second == handle )
            return; // same resource re-applied inside its dirty window — the guard doing its job

        if ( !m_SwallowReported.insert( binding ).second )
            return; // already named once for this material; a per-draw defect fires every draw

        LOG_ERROR( "Material on shader '{}': {} at binding {} was rebound to a different resource "
                   "({:#x} -> {:#x}) after this frame's descriptor flush. The write is SWALLOWED — the "
                   "set was already bound in the recording command buffer, so every draw of this "
                   "material keeps the first resource. A resource that varies per draw needs its own "
                   "material (one per emitter / per texture set, as ParticleRenderer and "
                   "TerrainRenderer do) or a buffer row named by push constant.",
                   m_VulkanShader->GetName(), what, binding, written->second, handle );
    }

    // NO `force` PARAMETER. It was `bool force = false` and this body never read it — there is no
    // conditional path here for a caller to force past, only an unconditional `vkUpdateDescriptorSets`.
    // One of the five call sites passed `true`, which is what a flag like this costs: a reader at that
    // line believes something different happens there, and nothing does.
    void VulkanMaterialBackend::UpdateDescriptorSets( const std::vector<VkWriteDescriptorSet>& writes )
    {
        if ( writes.empty() ) return;
        
        vkUpdateDescriptorSets( SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                                     ->GetVulkanLogicalDevice(),
                                static_cast<uint32_t>( writes.size() ),
                                writes.data(), 0, nullptr );
    }

    void VulkanMaterialBackend::ApplyUniformBuffer( MaterialProperty* prop )
    {
        // Called on EVERY Apply, dirty or not (UniformBufferProperty::Apply): whether the set must be
        // rewritten is no longer "is the value dirty" alone but also "is the set still pointing at the copy
        // this view binds now" — see DescriptorCopyRecord in ShaderResources/ViewCopiedBlock.hpp.
        auto uniformProp = static_cast<UniformBufferProperty*>( prop );
        if ( !uniformProp )
            return;

        const uint32_t frameIndex    = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t slot          = EngineContext::GetInstance().GetActiveRendererSlot();
        const uint32_t setIndex      = 0; // Simplified

        auto vulkanBuffer =
             sp_cast<ShaderResources::API::Vulkan::VulkanUniformBuffer>( uniformProp->GetUniform() );
        if ( !vulkanBuffer || frameIndex >= m_BoundCopies.size() || slot >= m_BoundCopies[frameIndex].size() )
            return;

        const uint32_t                                                     binding = vulkanBuffer->GetBinding();
        ShaderResources::API::Vulkan::ViewCopyBinding                      copy;
        const auto resolved = vulkanBuffer->BindActiveCopy( frameIndex, copy );
        if ( !resolved.IsSuccess() )
        {
            // Nothing valid to point the set at, so the write is skipped (the set keeps its fallback or its
            // last LIVE copy). Named once per binding: this runs per draw.
            if ( m_BindRefusalReported.insert( binding ).second )
                LOG_ERROR( "[MaterialBackend] '{}' binding {}: no uniform-buffer copy to bind -- {}",
                           m_VulkanShader ? m_VulkanShader->GetName() : std::string( "<no shader>" ), binding,
                           resolved.GetError() );
            return;
        }

        auto& record = m_BoundCopies[frameIndex][slot];
        if ( !record.NeedsWrite( binding, copy.CopyId, uniformProp->IsDirty() ) )
            return;

        const uint64_t handle = reinterpret_cast<uint64_t>( copy.Info.buffer );

        // At most one descriptor flush per (frame, slot) per frame — the set is bound into the recording
        // command buffer right after the first draw's Apply, and rewriting it then is illegal. A LATER
        // Apply carrying a DIFFERENT resource is therefore a swallowed rebind: kept swallowed, but never
        // silent. The record is NOT updated, so the next frame rewrites it.
        if ( m_DescriptorSetsUpdateFrame[frameIndex][slot][setIndex] == absoluteFrame )
        {
            ReportSwallowedRebind( frameIndex, binding, handle, "uniform buffer" );
            return;
        }

        auto wds = DescriptorSetBuilder::GetUniformWDS( this, frameIndex, 0, binding, 1U, &copy.Info );

        UpdateDescriptorSets( { wds } );
        NoteDescriptorWrite( frameIndex, binding, handle );
        record.NoteWritten( binding, copy.CopyId );
        uniformProp->MarkClean();
    }

    void VulkanMaterialBackend::ApplyStorageBuffer( MaterialProperty* prop )
    {
        // Called on EVERY Apply, dirty or not, for the reason ApplyUniformBuffer gives: a per-frame storage
        // buffer's copies are per view and made lazily, so a clean property can still own a set pointing at
        // a copy that was dropped (its view closed, or the buffer grew).
        auto storageProp = static_cast<StorageBufferProperty*>( prop );
        if ( !storageProp )
            return;

        const uint32_t frameIndex    = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t slot          = EngineContext::GetInstance().GetActiveRendererSlot();
        const uint32_t setIndex      = 0; // Simplified

        auto vulkanBuffer =
             sp_cast<ShaderResources::API::Vulkan::VulkanStorageBuffer>( storageProp->GetStorageBuffer() );
        if ( !vulkanBuffer || frameIndex >= m_BoundCopies.size() || slot >= m_BoundCopies[frameIndex].size() )
            return;

        const uint32_t                                binding = vulkanBuffer->GetBinding();
        ShaderResources::API::Vulkan::ViewCopyBinding copy;
        const auto                                    resolved = vulkanBuffer->BindActiveCopy( frameIndex, copy );
        if ( !resolved.IsSuccess() )
        {
            if ( m_BindRefusalReported.insert( binding ).second )
                LOG_ERROR( "[MaterialBackend] '{}' binding {}: no storage-buffer copy to bind -- {}",
                           m_VulkanShader ? m_VulkanShader->GetName() : std::string( "<no shader>" ), binding,
                           resolved.GetError() );
            return;
        }

        auto& record = m_BoundCopies[frameIndex][slot];
        if ( !record.NeedsWrite( binding, copy.CopyId, storageProp->IsDirty() ) )
            return;

        const uint64_t handle = reinterpret_cast<uint64_t>( copy.Info.buffer );

        // See ApplyUniformBuffer. This is the exact path that swallowed the particle system: one shared
        // billboard material, N emitters, and every SetBuffer after the first draw of the frame landed here
        // and vanished. The record is NOT updated, so the next frame rewrites it.
        if ( m_DescriptorSetsUpdateFrame[frameIndex][slot][setIndex] == absoluteFrame )
        {
            ReportSwallowedRebind( frameIndex, binding, handle, "storage buffer" );
            return;
        }

        auto wds = DescriptorSetBuilder::GetStorageWDS( this, frameIndex, 0, binding, 1U, &copy.Info );

        UpdateDescriptorSets( { wds } );
        NoteDescriptorWrite( frameIndex, binding, handle );
        record.NoteWritten( binding, copy.CopyId );
        storageProp->MarkClean();
    }

    void VulkanMaterialBackend::ApplyTexture2D( MaterialProperty* prop )
    {
        auto textureProp = static_cast<Texture2DProperty*>( prop );
        if ( !textureProp || !textureProp->IsDirty() )
            return;

        const uint32_t frameIndex   = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t setIndex = 0; // Simplified

        if ( auto imageUniform = textureProp->GetUniform() )
        {
            if ( auto vulkanImage = sp_cast<ShaderResources::API::Vulkan::VulkanUniformImage2D>( imageUniform ) )
            {
                auto descriptorImageInfo = vulkanImage->GetDescriptorImageInfo();
                const uint64_t handle              = reinterpret_cast<uint64_t>( descriptorImageInfo.imageView );

                // See ApplyUniformBuffer. For textures this is the "second terrain keeps the first
                // one's splat" path — the reason TerrainRenderer keys one material per texture set.
                if ( m_DescriptorSetsUpdateFrame[frameIndex][EngineContext::GetInstance().GetActiveRendererSlot()]
                                                [setIndex] == absoluteFrame )
                {
                    ReportSwallowedRebind( frameIndex, vulkanImage->GetBinding(), handle, "2D texture" );
                    return;
                }

                auto wds = DescriptorSetBuilder::GetSampler2DWDS( this, frameIndex, 0,
                                                                  vulkanImage->GetBinding(), 1U, &descriptorImageInfo );

                UpdateDescriptorSets( { wds } );
                NoteDescriptorWrite( frameIndex, vulkanImage->GetBinding(), handle );
                textureProp->MarkClean();
            }
        }
    }

    void VulkanMaterialBackend::ApplyTextureCube( MaterialProperty* prop )
    {
        auto textureProp = static_cast<TextureCubeProperty*>( prop );
        if ( !textureProp || !textureProp->IsDirty() )
            return;

        const uint32_t frameIndex   = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t setIndex = 0; // Simplified

        if ( auto imageUniform = textureProp->GetUniform() )
        {
            if ( auto vulkanImage = sp_cast<ShaderResources::API::Vulkan::VulkanUniformImageCube>( imageUniform ) )
            {
                auto descriptorImageInfo = vulkanImage->GetDescriptorImageInfo();
                const uint64_t handle              = reinterpret_cast<uint64_t>( descriptorImageInfo.imageView );

                // See ApplyUniformBuffer.
                if ( m_DescriptorSetsUpdateFrame[frameIndex][EngineContext::GetInstance().GetActiveRendererSlot()]
                                                [setIndex] == absoluteFrame )
                {
                    ReportSwallowedRebind( frameIndex, vulkanImage->GetBinding(), handle, "cube texture" );
                    return;
                }

                auto wds = DescriptorSetBuilder::GetSamplerCubeWDS( this, frameIndex, 0,
                                                                    vulkanImage->GetBinding(), 1U, &descriptorImageInfo );

                UpdateDescriptorSets( { wds } );
                NoteDescriptorWrite( frameIndex, vulkanImage->GetBinding(), handle );
                textureProp->MarkClean();
            }
        }
    }

    void VulkanMaterialBackend::ReportShapeDriftOnce()
    {
        if ( m_ShapeDriftReported || !m_VulkanShader )
            return;
        if ( m_VulkanShader->GetReloadGeneration() == m_ShaderGeneration )
            return;

        // The shader was recompiled after these sets were allocated. That is HARMLESS as long as the new
        // layout has the same shape: Vulkan compares descriptor set layouts by content, not by handle,
        // so a recompile that only changed the code leaves the sets perfectly bindable. What is NOT
        // harmless is a recompile that added or removed a binding — the pipeline built from the new
        // layout and these sets then describe different shaders, which is the
        // "has N total descriptors, but ... has M total descriptors" the validation layer reports on
        // every bind. Say which it is, once, naming both counts.
        const auto&    current = m_VulkanShader->GetAllDescriptorSetLayouts();
        const uint32_t was     = m_Layouts.empty() || !m_Layouts[0] ? 0u : m_Layouts[0]->DescriptorCount();
        const uint32_t now     = current.empty() || !current[0] ? 0u : current[0]->DescriptorCount();

        m_ShaderGeneration   = m_VulkanShader->GetReloadGeneration();
        m_ShapeDriftReported = was != now;

        if ( was != now )
        {
            LOG_ERROR( "Material on shader '{}': its descriptor sets were allocated against a layout of "
                       "{} descriptor(s) and the shader now declares {}. The sets stay valid — they own "
                       "their layout — but any pipeline rebuilt from the new one cannot be bound to "
                       "them. Restart the editor to pick the change up.",
                       m_VulkanShader->GetName(), was, now );
        }
    }

    void VulkanMaterialBackend::BindDescriptorSets( VkCommandBuffer cmdBuffer, VkPipelineLayout layout,
                                                    VkPipelineBindPoint bindPoint, uint32_t frameIndex )
    {
        ReportShapeDriftOnce();

        const uint32_t slot = EngineContext::GetInstance().GetActiveRendererSlot();
        if ( frameIndex >= m_DescriptorSets.size() || slot >= m_DescriptorSets[frameIndex].size() ||
             m_DescriptorSets[frameIndex][slot].empty() )
            return;

        std::vector<VkDescriptorSet> setsToBind;
        setsToBind.reserve( m_DescriptorSets[frameIndex][slot].size() );

        for ( VkDescriptorSet descriptorSet : m_DescriptorSets[frameIndex][slot] )
        {
            if ( descriptorSet != VK_NULL_HANDLE )
            {
                setsToBind.push_back( descriptorSet );
            }
            else
            {
                // Instead of falling back or just warning, we log a critical error and skip
                // binding this entire set to prevent vkCmdBindDescriptorSets from crashing.
                LOG_ERROR( "VulkanMaterialBackend: Attempting to bind NULL descriptor set! This indicates a failure in descriptor set initialization or asset loading." );
                return;
            }
        }

        vkCmdBindDescriptorSets( cmdBuffer, bindPoint, layout, 0, static_cast<uint32_t>( setsToBind.size() ),
                                 setsToBind.data(), 0, nullptr );
    }

    void VulkanMaterialBackend::ResetFrameUpdateState( uint32_t /*frameIndex*/ )
    {
    }

    bool VulkanMaterialBackend::HasDescriptorSets() const
    {
        return !m_DescriptorSets.empty() && !m_DescriptorSets[0].empty() && !m_DescriptorSets[0][0].empty();
    }

    void VulkanMaterialBackend::FlushUpdates()
    {
        // NOTHING WAS ALLOCATED, SO THERE IS NOTHING TO MARK — and until Ю11 this line could not be
        // reached, so it was not written. A shader that declares no descriptor resources allocates no
        // sets, which leaves `m_DescriptorSetsUpdateFrame` EMPTY, and the index below then addressed
        // element 0 of a null buffer: a hard crash, not a wrong picture. Such a shader is legal Vulkan
        // (`UIMatError` draws from push constants alone), and it now flushes nothing instead of dying.
        if ( m_DescriptorSetsUpdateFrame.empty() )
            return;

        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        // Mark this RENDERER's sets as updated for this absolute frame. Marking every slot would tell the
        // next view its descriptors are current when nobody has written them.
        const uint32_t slot = EngineContext::GetInstance().GetActiveRendererSlot();
        if ( slot < m_DescriptorSetsUpdateFrame[frameIndex].size() )
            for ( auto& setFrame : m_DescriptorSetsUpdateFrame[frameIndex][slot] )
                setFrame = absoluteFrame;
    }

    void VulkanMaterialBackend::InitializeWithFallbacks()
    {
        const uint32_t framesInFlight = EngineContext::GetInstance().GetMaxFramesInFlight();
        auto&          descriptorSets = m_VulkanShader->GetShaderDescriptorSets();

        // EVERY slot gets the fallbacks, not just the active one: a set that is bound before anything
        // wrote it reads undefined descriptors, and a view that opens later would bind exactly that. The
        // writes address a slot through GetDescriptorSet, so the active slot is moved across them and put
        // back — this runs once, at material creation, on the one thread that records.
        const uint32_t restoreSlot = EngineContext::GetInstance().GetActiveRendererSlot();

        for ( uint32_t slot = 0; slot < EngineContext::kMaxRendererSlots; ++slot )
        {
            EngineContext::GetInstance().SetActiveRendererSlot( slot );
            for ( uint32_t frame = 0; frame < framesInFlight; ++frame )
            {
                for ( const auto& [setIndex, descriptorSet] : descriptorSets )
                {
                    std::vector<VkWriteDescriptorSet> writes;

                    // Track infos to keep them alive until vkUpdateDescriptorSets
                    // Every write below stores a POINTER into this vector, so it must not reallocate:
                    // the reserve has to count every image binding that follows, 3D included.
                    std::vector<VkDescriptorImageInfo> imageInfos;
                    imageInfos.reserve(
                         descriptorSet.Image2DSamplers.size() + descriptorSet.ImageCubeSamplers.size() +
                         descriptorSet.StorageImage2DSamplers.size() + descriptorSet.Image3DSamplers.size() +
                         descriptorSet.StorageImage3DSamplers.size() );

                    std::vector<VkDescriptorBufferInfo> bufferInfos;
                    bufferInfos.reserve( descriptorSet.UniformBuffers.size() +
                                         descriptorSet.StorageBuffers.size() );

                    // UNIFORM BUFFERS
                    for ( const auto& [binding, size] : descriptorSet.UniformBuffers )
                    {
                        VkDescriptorBufferInfo info = {
                             .buffer = m_DummyBuffer, .offset = 0, .range = VK_WHOLE_SIZE };
                        bufferInfos.push_back( info );
                        writes.push_back( DescriptorSetBuilder::GetUniformWDS( this, frame, setIndex, binding, 1,
                                                                               &bufferInfos.back() ) );
                    }

                    // STORAGE BUFFERS
                    for ( const auto& [binding, size] : descriptorSet.StorageBuffers )
                    {
                        VkDescriptorBufferInfo info = {
                             .buffer = m_DummyBuffer, .offset = 0, .range = VK_WHOLE_SIZE };
                        bufferInfos.push_back( info );
                        writes.push_back( DescriptorSetBuilder::GetStorageWDS( this, frame, setIndex, binding, 1,
                                                                               &bufferInfos.back() ) );
                    }

                    // IMAGES — one loop over every declared image binding, each tagged with the fallback
                    // its VIEW TYPE requires. Driving off CollectImageBindings rather than walking the
                    // buckets here is what makes "every declared binding gets a descriptor" true by
                    // construction: a reflection bucket that gains a kind cannot be quietly skipped,
                    // because the kind arrives in this switch. A binding left out is not a visual
                    // glitch — it is an undefined descriptor the shader samples anyway.
                    for ( const auto& [binding, kind] : ShaderResource::CollectImageBindings( descriptorSet ) )
                    {
                        switch ( kind )
                        {
                            case ShaderResource::FallbackImageKind::Sampled2D:
                            {
                                auto fallback = FallbackTextures::Get().GetFallbackTexture2D(
                                     Core::Formats::ImageFormat::RGBA32F );
                                if ( auto vulkanImage = sp_cast<VulkanImage2D>( fallback ) )
                                {
                                    imageInfos.push_back( vulkanImage->GetResource().GetDescriptorInfo() );
                                    writes.push_back( DescriptorSetBuilder::GetSampler2DWDS(
                                         this, frame, setIndex, binding, 1, &imageInfos.back() ) );
                                }
                                break;
                            }
                            case ShaderResource::FallbackImageKind::SampledCube:
                            {
                                auto fallback = FallbackTextures::Get().GetFallbackTextureCube(
                                     Core::Formats::ImageFormat::RGBA8F );
                                if ( auto vulkanImage = sp_cast<VulkanImageCube>( fallback ) )
                                {
                                    imageInfos.push_back( vulkanImage->GetResource().GetDescriptorInfo() );
                                    writes.push_back( DescriptorSetBuilder::GetSamplerCubeWDS(
                                         this, frame, setIndex, binding, 1, &imageInfos.back() ) );
                                }
                                break;
                            }
                            case ShaderResource::FallbackImageKind::Sampled3D:
                            {
                                // A volume needs a VOLUME fallback: a 2D view in a `sampler3D` does not
                                // fail, it samples the wrong thing.
                                auto fallback = FallbackTextures::Get().GetFallbackTexture3D(
                                     Core::Formats::ImageFormat::RGBA8F );
                                if ( auto vulkanImage = sp_cast<VulkanImage3D>( fallback ) )
                                {
                                    imageInfos.push_back( vulkanImage->GetResource().GetDescriptorInfo() );
                                    writes.push_back( DescriptorSetBuilder::GetSampler3DWDS(
                                         this, frame, setIndex, binding, 1, &imageInfos.back() ) );
                                }
                                break;
                            }
                            case ShaderResource::FallbackImageKind::Storage2D:
                            {
                                // Dedicated storage fallback — it carries VK_IMAGE_USAGE_STORAGE_BIT,
                                // which the sampled one does not.
                                auto fallback = FallbackTextures::Get().GetFallbackStorageImage2D(
                                     Core::Formats::ImageFormat::RGBA32F );
                                if ( auto vulkanImage = sp_cast<VulkanImage2D>( fallback ) )
                                {
                                    imageInfos.push_back( { VK_NULL_HANDLE, vulkanImage->GetResource().ImageView,
                                                            VK_IMAGE_LAYOUT_GENERAL } );
                                    writes.push_back( DescriptorSetBuilder::GetStorageWDS(
                                         this, frame, setIndex, binding, 1, &imageInfos.back() ) );
                                }
                                break;
                            }
                            case ShaderResource::FallbackImageKind::Storage3D:
                            {
                                auto fallback = FallbackTextures::Get().GetFallbackStorageImage3D(
                                     Core::Formats::ImageFormat::RGBA8F );
                                if ( auto vulkanImage = sp_cast<VulkanImage3D>( fallback ) )
                                {
                                    imageInfos.push_back( { VK_NULL_HANDLE, vulkanImage->GetResource().ImageView,
                                                            VK_IMAGE_LAYOUT_GENERAL } );
                                    writes.push_back( DescriptorSetBuilder::GetStorageWDS(
                                         this, frame, setIndex, binding, 1, &imageInfos.back() ) );
                                }
                                break;
                            }
                            default:
                                // Named rather than left to fall off the end: the value arrives from
                                // reflection rather than from a closed enum this switch could be made
                                // exhaustive over, so `-Wswitch` cannot reach it and the log line is the
                                // only thing that would. (This comment used to say "the workspace compiles
                                // with warnings off"; it does not, as of the commit that turned them on.)
                                LOG_ERROR( "InitializeWithFallbacks: image binding {} has no fallback for "
                                           "kind {}; its descriptor is left UNWRITTEN",
                                           binding, static_cast<int>( kind ) );
                                break;
                        }
                    }

                    UpdateDescriptorSets( writes );
                }
            }
        }

        EngineContext::GetInstance().SetActiveRendererSlot( restoreSlot );
    }

    void VulkanMaterialBackend::InitializeDefaults()
    {
        InitializeWithFallbacks();
    }

} // namespace Desert::Graphic::API::Vulkan
