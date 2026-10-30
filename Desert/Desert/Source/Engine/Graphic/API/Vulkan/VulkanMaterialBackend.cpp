#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>

#include <bit>
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
#include <Engine/Graphic/API/Vulkan/VulkanViewDescriptorPools.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanBindlessTextureTable.hpp>

namespace Desert::Graphic::API::Vulkan
{
    VulkanMaterialBackend::VulkanMaterialBackend( const std::shared_ptr<Shader>& shader )
         : MaterialBackend( shader ), m_VulkanShader( SP_CAST( VulkanShader, shader ) ),
           m_Layouts( m_VulkanShader->GetAllDescriptorSetLayouts() ),
           m_ShaderGeneration( m_VulkanShader->GetCodeGeneration() )
    {
        // A SHADER WITH NO LAYOUTS HAS NOTHING TO ALLOCATE FOR, AND ASKING ANYWAY IS TWENTY VALIDATION
        // ERRORS. That is what a shader whose first compile failed carries: no stages, so no reflection,
        // so no set layouts — and allocating anyway once made a pool with `maxSets = 0` and ten
        // `vkAllocateDescriptorSets` with `descriptorSetCount = 0`, one per (frame x renderer slot).
        // Measured on the live editor with one broken shader: 20 of the run's 24 validation errors came
        // from here, drowning the two that said what had actually gone wrong. (Sets are made per view on
        // first use now, and never for such a shader.) The backend stays valid and
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

        // Every uniform/storage binding starts out pointing at the device's ONE zero buffer (WriteFallbacks):
        // defined zeros render a binding read before its real resource is bound as a stable black instead of
        // garbage. One per device, as UE's global null resources (GEmptyStructuredBuffer, GBlackTexture): it
        // used to be one 64 KB buffer PER MATERIAL BACKEND, which on Bistro was 7753 buffers, 485 MB that no
        // ledger saw (BISTRO-MEM-2 p4).
        auto zero = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                         ->GetVulkanAllocator()
                         ->RT_ZeroBuffer();
        if ( !zero.IsSuccess() )
        {
            LOG_ERROR( "[Material] shader '{}': the zero buffer for unbound bindings is missing: {}",
                       m_VulkanShader ? m_VulkanShader->GetName() : std::string( "<null>" ), zero.GetError() );
        }
        else
        {
            m_DummyBuffer = zero.GetValue();
        }
    }

    // The zero buffer is the device's (VulkanAllocator::RT_ZeroBuffer), alive until the device goes: the sets
    // this backend releases may still point at it from a command buffer in flight, and that stays valid.
    VulkanMaterialBackend::~VulkanMaterialBackend() = default;

    IViewDescriptorSetCopy* VulkanMaterialBackend::ActiveSets( const uint32_t frameIndex )
    {
        if ( m_Layouts.empty() )
            return nullptr;
        if ( IViewDescriptorSetCopy* existing = m_ViewSets.FindActive( frameIndex ) )
            return existing;

        IViewDescriptorSetCopy* sets = nullptr;
        const auto              made = m_ViewSets.Resolve(
             frameIndex,
             [this]( std::string_view /*viewName*/, uint32_t /*frame*/,
                     std::unique_ptr<IViewDescriptorSetCopy>& out )
             { return AllocateViewSets( m_Layouts, m_VulkanShader->GetName(), out ); },
             [this]( const uint32_t frame ) { WriteFallbacks( frame ); }, sets );
        if ( !made.IsSuccess() )
        {
            // Per draw otherwise; the draw itself is refused by BindDescriptorSets returning false.
            if ( !m_ResolveFailureReported )
                LOG_ERROR( "[MaterialBackend] {}", made.GetError() );
            m_ResolveFailureReported = true;
            return nullptr;
        }
        return sets;
    }

    Common::BoolResultStr VulkanMaterialBackend::PrepareViewResources( const MaterialExecutor& executor )
    {
        // The frame index the prepare's apply resolves with (ApplyUniformBuffer / ApplyStorageBuffer).
        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        if ( HasDescriptorSets() && ActiveSets( frameIndex ) == nullptr )
            return Common::MakeFormattedError( "shader '{}': its descriptor sets for frame {} were not made",
                                               m_VulkanShader->GetName(), frameIndex );

        // The block copies the prepare's descriptor writes bind (MaterialExecutor::PrepareForRecording applies
        // right after this). A copy that cannot be made is NOT this pass's fault: that apply meets the same
        // refusal at the bind, names it (m_BindRefusalReported) and keeps the set's fallback - so its result is
        // not turned into a pass fault here.
        ShaderResources::API::Vulkan::ViewCopyBinding copy;
        for ( const auto& [name, index] : executor.GetUniformBufferProperties() )
        {
            const auto property = executor.GetUniformBufferProperty( name );
            const auto buffer   = property ? sp_cast<ShaderResources::API::Vulkan::VulkanUniformBuffer>(
                                                  property->GetUniform() )
                                           : nullptr;
            if ( buffer )
                (void)buffer->BindActiveCopy( frameIndex, copy );
        }
        for ( const auto& [name, index] : executor.GetStorageBufferProperties() )
        {
            const auto property = executor.GetStorageBufferProperty( name );
            const auto buffer   = property ? sp_cast<ShaderResources::API::Vulkan::VulkanStorageBuffer>(
                                                  property->GetStorageBuffer() )
                                           : nullptr;
            if ( buffer )
                (void)buffer->BindActiveCopy( frameIndex, copy );
        }
        return Common::MakeSuccess( true );
    }

    VkDescriptorSet VulkanMaterialBackend::GetDescriptorSet( const uint32_t frameIndex, const uint32_t setIndex )
    {
        // The ACTIVE view resolves here and nowhere else, so a write and the bind that follows it can never
        // land on different copies.
        IViewDescriptorSetCopy* sets = ActiveSets( frameIndex );
        if ( sets == nullptr )
            return VK_NULL_HANDLE;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): AllocateViewSets makes only this type
        const auto& handles = static_cast<VulkanViewSets*>( sets )->GetSets();
        return setIndex < handles.size() ? handles[setIndex] : VK_NULL_HANDLE;
    }

    Common::ResultStr<VulkanMaterialBackend::WrittenSlots>
    VulkanMaterialBackend::GetWrittenSlots( const uint32_t frameIndex )
    {
        IViewDescriptorSetCopy* sets = ActiveSets( frameIndex );
        if ( sets == nullptr )
            return Common::MakeFormattedError<WrittenSlots>( "material of shader '{}' has no descriptor sets",
                                                             m_VulkanShader->GetName() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): AllocateViewSets makes only this type
        const auto& handles = static_cast<VulkanViewSets*>( sets )->GetSets();
        if ( handles.empty() )
            return Common::MakeFormattedError<WrittenSlots>( "material of shader '{}' has no set 0",
                                                             m_VulkanShader->GetName() );
        return Common::MakeSuccess( WrittenSlots{ handles[0], sets->BoundCopies.GetWrittenBindings() } );
    }

    void VulkanMaterialBackend::NoteDescriptorWrite( IViewDescriptorSetCopy& sets, const uint32_t binding,
                                                     const uint64_t handle )
    {
        auto&          record        = sets.Writes;
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( record.Frame != absoluteFrame )
        {
            record.Frame = absoluteFrame;
            record.Handles.clear();
        }
        record.Handles[binding] = handle;
    }

    void VulkanMaterialBackend::ReportSwallowedRebind( const IViewDescriptorSetCopy& sets, const uint32_t binding,
                                                       const uint64_t handle, const char* what )
    {
        const auto& record = sets.Writes;
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
        if ( uniformProp == nullptr )
            return;

        const uint32_t frameIndex    = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        auto vulkanBuffer =
             sp_cast<ShaderResources::API::Vulkan::VulkanUniformBuffer>( uniformProp->GetUniform() );
        if ( !vulkanBuffer )
            return;
        IViewDescriptorSetCopy* sets = ActiveSets( frameIndex );
        if ( sets == nullptr )
            return;

        const uint32_t                                binding = vulkanBuffer->GetBinding();
        ShaderResources::API::Vulkan::ViewCopyBinding copy;
        const auto                                    resolved = vulkanBuffer->BindActiveCopy( frameIndex, copy );
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

        auto& record = sets->BoundCopies;
        if ( !record.NeedsWrite( binding, copy.CopyId, uniformProp->GetVersion() ) )
            return;

        const auto handle = std::bit_cast<uint64_t>( copy.Info.buffer );

        // At most one descriptor flush per (view, frame in flight) per frame — the set is bound into the recording
        // command buffer right after the first draw's Apply, and rewriting it then is illegal. A LATER
        // Apply carrying a DIFFERENT resource is therefore a swallowed rebind: kept swallowed, but never
        // silent. The record is NOT updated, so the next frame rewrites it.
        if ( sets->FlushedFrame == absoluteFrame )
        {
            ReportSwallowedRebind( *sets, binding, handle, "uniform buffer" );
            return;
        }

        auto wds = DescriptorSetBuilder::GetUniformWDS( this, frameIndex, 0, binding, 1U, &copy.Info );

        UpdateDescriptorSets( { wds } );
        NoteDescriptorWrite( *sets, binding, handle );
        record.NoteWritten( binding, copy.CopyId, uniformProp->GetVersion() );
    }

    void VulkanMaterialBackend::ApplyStorageBuffer( MaterialProperty* prop )
    {
        // Called on EVERY Apply, dirty or not, for the reason ApplyUniformBuffer gives: a per-frame storage
        // buffer's copies are per view and made lazily, so a clean property can still own a set pointing at
        // a copy that was dropped (its view closed, or the buffer grew).
        auto storageProp = static_cast<StorageBufferProperty*>( prop );
        if ( storageProp == nullptr )
            return;
        // A storage buffer nothing ever wrote is not the material's slot (MaterialExecutor::GetRouteFill leaves it
        // to the pass, e.g. the Composite's uploaded light buffers): writing a lazy copy's descriptor here would
        // put the binding in GetWrittenSlots and the record would refuse it as filled by both routes.
        if ( !storageProp->IsWritten() )
            return;

        const uint32_t frameIndex    = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        auto vulkanBuffer =
             sp_cast<ShaderResources::API::Vulkan::VulkanStorageBuffer>( storageProp->GetStorageBuffer() );
        if ( !vulkanBuffer )
            return;
        IViewDescriptorSetCopy* sets = ActiveSets( frameIndex );
        if ( sets == nullptr )
            return;

        const uint32_t                                binding = storageProp->GetBinding();
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

        auto& record = sets->BoundCopies;
        if ( !record.NeedsWrite( binding, copy.CopyId, storageProp->GetVersion() ) )
            return;

        const auto handle = std::bit_cast<uint64_t>( copy.Info.buffer );

        // See ApplyUniformBuffer. This is the exact path that swallowed the particle system: one shared
        // billboard material, N emitters, and every SetBuffer after the first draw of the frame landed here
        // and vanished. The record is NOT updated, so the next frame rewrites it.
        if ( sets->FlushedFrame == absoluteFrame )
        {
            ReportSwallowedRebind( *sets, binding, handle, "storage buffer" );
            return;
        }

        auto wds = DescriptorSetBuilder::GetStorageWDS( this, frameIndex, 0, binding, 1U, &copy.Info );

        UpdateDescriptorSets( { wds } );
        NoteDescriptorWrite( *sets, binding, handle );
        record.NoteWritten( binding, copy.CopyId, storageProp->GetVersion() );
    }

    bool VulkanMaterialBackend::WriteTextureWithSampler( IViewDescriptorSetCopy& sets, uint32_t frameIndex,
                                                         uint32_t binding, std::optional<uint32_t> samplerBinding,
                                                         const VkDescriptorImageInfo& info, uint64_t handle,
                                                         uint64_t version )
    {
        // A bound texture property is a separate texture plus its own sampler state (`<name>Sampler`): the view at
        // its binding, the state at the paired one. Both are recorded as written, so a pass's set takes both.
        if ( !samplerBinding )
            return false;
        const VkDescriptorImageInfo texture{ VK_NULL_HANDLE, info.imageView, info.imageLayout };
        const VkDescriptorImageInfo sampler{ info.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        UpdateDescriptorSets(
             { DescriptorSetBuilder::GetSampledImageWDS( this, frameIndex, 0, binding, &texture ),
               DescriptorSetBuilder::GetSamplerStateWDS( this, frameIndex, 0, *samplerBinding, &sampler ) } );
        NoteDescriptorWrite( sets, binding, handle );
        NoteDescriptorWrite( sets, *samplerBinding, handle );
        sets.BoundCopies.NoteWritten( binding, handle, version );
        sets.BoundCopies.NoteWritten( *samplerBinding, handle, version );
        return true;
    }

    void VulkanMaterialBackend::ReportMissingSamplerState( uint32_t binding )
    {
        // Once per material lifetime per binding: the cause is the shader's text, not this frame.
        if ( !m_BindRefusalReported.insert( binding ).second )
            return;
        LOG_ERROR( "[Materials] shader '{}': the texture property at binding {} has no sampler state of its own "
                   "(`<texture>{}`); the texture is not bound",
                   m_VulkanShader->GetName(), binding, Core::Formats::kMaterialTextureSamplerSuffix );
    }

    void VulkanMaterialBackend::ApplyTexture2D( MaterialProperty* prop )
    {
        auto textureProp = static_cast<Texture2DProperty*>( prop );
        if ( textureProp == nullptr )
            return;

        auto vulkanImage =
             sp_cast<ShaderResources::API::Vulkan::VulkanUniformImage2D>( textureProp->GetUniform() );
        if ( !vulkanImage )
            return;

        const uint32_t frameIndex    = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t binding       = vulkanImage->GetBinding();

        IViewDescriptorSetCopy* sets = ActiveSets( frameIndex );
        if ( sets == nullptr )
            return;

        auto descriptorImageInfo = vulkanImage->GetDescriptorImageInfo();
        // A slot with a stated sampling state (clamp, mirror, nearest) samples through the shared state
        // sampler; the default state keeps the image's own, which the global filter setting recreates live.
        if ( textureProp->GetSamplerState() != Core::Formats::SamplerState{} )
            descriptorImageInfo.sampler = AcquireSlotSampler( textureProp->GetSamplerState() );
        if ( textureProp->SamplesEncoded() )
            if ( const auto* image = dynamic_cast<const VulkanImage2D*>( textureProp->GetImage() ) )
                descriptorImageInfo.imageView = image->GetEncodedView();
        // An image with no view yet (not resident) reads the 2D fallback until it is, as the combined write did.
        if ( descriptorImageInfo.imageView == VK_NULL_HANDLE || descriptorImageInfo.sampler == VK_NULL_HANDLE )
            if ( auto fallback = sp_cast<VulkanImage2D>(
                      FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ) ) )
                descriptorImageInfo = fallback->GetResource().GetDescriptorInfo();
        const auto handle = std::bit_cast<uint64_t>( descriptorImageInfo.imageView );

        // A set that already points at this image view with this version has nothing to learn; a new set
        // (a view that first records now, however late) has an empty record and takes the write.
        auto& record = sets->BoundCopies;
        if ( !record.NeedsWrite( binding, handle, textureProp->GetVersion() ) )
            return;

        // See ApplyUniformBuffer. For textures this is the "second terrain keeps the first
        // one's layer textures" path — the reason TerrainRenderer keys one material per texture set.
        if ( sets->FlushedFrame == absoluteFrame )
        {
            ReportSwallowedRebind( *sets, binding, handle, "2D texture" );
            return;
        }

        if ( !WriteTextureWithSampler( *sets, frameIndex, binding, vulkanImage->GetSamplerBinding(),
                                       descriptorImageInfo, handle, textureProp->GetVersion() ) )
            ReportMissingSamplerState( binding );
    }

    void VulkanMaterialBackend::ApplyTextureCube( MaterialProperty* prop )
    {
        auto textureProp = static_cast<TextureCubeProperty*>( prop );
        if ( textureProp == nullptr )
            return;

        auto vulkanImage =
             sp_cast<ShaderResources::API::Vulkan::VulkanUniformImageCube>( textureProp->GetUniform() );
        if ( !vulkanImage )
            return;

        const uint32_t frameIndex    = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint64_t absoluteFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t binding       = vulkanImage->GetBinding();

        IViewDescriptorSetCopy* sets = ActiveSets( frameIndex );
        if ( sets == nullptr )
            return;

        auto       descriptorImageInfo = vulkanImage->GetDescriptorImageInfo();
        const auto handle              = std::bit_cast<uint64_t>( descriptorImageInfo.imageView );

        // A set that already points at this image view with this version has nothing to learn; a new set
        // (a view that first records now, however late) has an empty record and takes the write.
        auto& record = sets->BoundCopies;
        if ( !record.NeedsWrite( binding, handle, textureProp->GetVersion() ) )
            return;

        // See ApplyUniformBuffer.
        if ( sets->FlushedFrame == absoluteFrame )
        {
            ReportSwallowedRebind( *sets, binding, handle, "cube texture" );
            return;
        }

        // A cube property with no image (TextureCubeProperty::RestoreDefault clears it) reads the fallback cube,
        // the documented "no cubemap set" state of a skybox or preview material.
        if ( descriptorImageInfo.imageView == VK_NULL_HANDLE || descriptorImageInfo.sampler == VK_NULL_HANDLE )
            if ( auto fallback = sp_cast<VulkanImageCube>(
                      FallbackTextures::Get().GetFallbackTextureCube( Core::Formats::ImageFormat::RGBA8F ) ) )
                descriptorImageInfo = fallback->GetResource().GetDescriptorInfo();
        if ( !WriteTextureWithSampler( *sets, frameIndex, binding, vulkanImage->GetSamplerBinding(),
                                       descriptorImageInfo, handle, textureProp->GetVersion() ) )
            ReportMissingSamplerState( binding );
    }

    void VulkanMaterialBackend::ReportShapeDriftOnce()
    {
        if ( m_ShapeDriftReported || !m_VulkanShader )
            return;
        if ( m_VulkanShader->GetCodeGeneration() == m_ShaderGeneration )
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

        m_ShaderGeneration   = m_VulkanShader->GetCodeGeneration();
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

    bool VulkanMaterialBackend::BindDescriptorSets( VkCommandBuffer cmdBuffer, VkPipelineLayout layout,
                                                    VkPipelineBindPoint bindPoint, uint32_t frameIndex )
    {
        ReportShapeDriftOnce();

        IViewDescriptorSetCopy* sets = ActiveSets( frameIndex );
        if ( sets == nullptr )
            return false; // named once by ActiveSets
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): AllocateViewSets makes only this type
        std::vector<VkDescriptorSet> handles = static_cast<VulkanViewSets*>( sets )->GetSets();
        // Material textures are read through the bindless table (BindlessTextures.glslh): its set is the one live
        // update-after-bind set of the device, taken at bind time because growth replaces it (EndFrame).
        if ( const VulkanBindlessTextureTable* table = BindlessTableIn( m_Layouts ) )
            handles[kBindlessSetIndex] = table->GetDescriptorSet();

        vkCmdBindDescriptorSets( cmdBuffer, bindPoint, layout, 0, static_cast<uint32_t>( handles.size() ),
                                 handles.data(), 0, nullptr );
        return true;
    }

    bool VulkanMaterialBackend::HasDescriptorSets() const
    {
        return !m_Layouts.empty();
    }

    void VulkanMaterialBackend::FlushUpdates()
    {
        // A shader that declares no descriptor resources (`UIMatError` draws from push constants alone) has
        // no sets, so there is nothing to mark.
        if ( m_Layouts.empty() )
            return;

        // Marks the ACTIVE view's sets only: marking another view's would tell it its descriptors are
        // current when nobody has written them.
        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        if ( IViewDescriptorSetCopy* sets = ActiveSets( frameIndex ) )
            sets->FlushedFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
    }

    void VulkanMaterialBackend::WriteFallbacks( const uint32_t frame )
    {
        auto& descriptorSets = m_VulkanShader->GetShaderDescriptorSets();

        for ( const auto& [setIndex, descriptorSet] : descriptorSets )
        {
            std::vector<VkWriteDescriptorSet> writes;

            // Track infos to keep them alive until vkUpdateDescriptorSets
            // Every write below stores a POINTER into this vector, so it must not reallocate:
            // the reserve has to count every image binding that follows, 3D included.
            std::vector<VkDescriptorImageInfo> imageInfos;
            imageInfos.reserve( descriptorSet.StorageImage2DSamplers.size() +
                                descriptorSet.StorageImage3DSamplers.size() );

            std::vector<VkDescriptorBufferInfo> bufferInfos;
            bufferInfos.reserve( descriptorSet.UniformBuffers.size() + descriptorSet.StorageBuffers.size() );

            // UNIFORM BUFFERS
            for ( const auto& [binding, size] : descriptorSet.UniformBuffers )
            {
                const VkDescriptorBufferInfo info = {
                     .buffer = m_DummyBuffer, .offset = 0, .range = VK_WHOLE_SIZE };
                bufferInfos.push_back( info );
                writes.push_back( DescriptorSetBuilder::GetUniformWDS( this, frame, setIndex, binding, 1,
                                                                       &bufferInfos.back() ) );
            }

            // STORAGE BUFFERS
            for ( const auto& [binding, size] : descriptorSet.StorageBuffers )
            {
                const VkDescriptorBufferInfo info = {
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
                            writes.push_back( DescriptorSetBuilder::GetStorageWDS( this, frame, setIndex, binding,
                                                                                   1, &imageInfos.back() ) );
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
                            writes.push_back( DescriptorSetBuilder::GetStorageWDS( this, frame, setIndex, binding,
                                                                                   1, &imageInfos.back() ) );
                        }
                        break;
                    }
                    default:
                        // Named rather than left to fall off the end: the value arrives from
                        // reflection rather than from a closed enum this switch could be made
                        // exhaustive over, so `-Wswitch` cannot reach it and the log line is the
                        // only thing that would. (This comment used to say "the workspace compiles
                        // with warnings off"; it does not, as of the commit that turned them on.)
                        LOG_ERROR( "WriteFallbacks: image binding {} has no fallback for "
                                   "kind {}; its descriptor is left UNWRITTEN",
                                   binding, static_cast<int>( kind ) );
                        break;
                }
            }

            UpdateDescriptorSets( writes );
        }
    }

} // namespace Desert::Graphic::API::Vulkan
