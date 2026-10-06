#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RDG-A2 - how a renderer binds graph resources to a pipeline (UE: the pass parameter struct of FRDGTexture*
// that the shader parameters are set from inside the pass lambda, and that nothing keeps after it).
//
// A PassBindings is the ONE parameter block of one draw or dispatch recorded by an exec lambda. It is built
// inside the exec from the PassContext, filled with TextureRef / BufferRef the pass declared in its setup,
// handed to Renderer::DispatchCompute / Renderer::DrawFullscreen, and destroyed when the exec returns. The
// backend writes a descriptor set for it in THAT exec (Vulkan: VulkanRdgBackend::DescriptorsOf) and forgets it.
//
// Every image or buffer that is a resource of the graph - a transient (Builder::CreateTexture/CreateBuffer) OR
// an external (FrameTextures::Import, a history image, a framebuffer attachment) - is bound through a
// PassBindings when a pass records with it. There is no second route: ComputePipeline::SetInput/SetOutput/
// SetStorageBuffer and Texture2DProperty::SetImage remain only for work recorded OUTSIDE a graph (bakes on a
// GpuBatch, immediate Dispatch) and for asset textures that are not graph resources (a lens-dirt texture, a
// LUT loaded from disk). A pass that binds a graph resource through a Material property or a pipeline setter
// is a defect, and so is a slot bound by both routes (the draw is refused, see Renderer::DrawFullscreen).
//
// SLOTS ARE SHADER NAMES. An entry names the resource as the shader declares it ("u_BloomTexture",
// "u_Output"); the consumer resolves the name against the pipeline's shader reflection to (set, binding,
// descriptor type). A name the shader does not declare, a kind that does not match the declared descriptor
// type (Sampled into a storage-image slot), and a resource slot of the shader that neither this block nor the
// material fills are errors returned by the consumer, never a silent fallback image. Binding indices never
// appear at a call site: they are the shader's business and drift with it.
//
// SAMPLERS. A sampled entry names the sampler it is read with (SamplerDesc), as UE's pass parameters carry a
// TStaticSamplerState next to the texture. There is no default and no implicit sampler on the graph route: the
// call site passes one, usually a named constant (SamplerDesc::LinearClamp()). The backend turns the description
// into a device object it caches for the device's lifetime (Vulkan: VulkanRdgPassDescriptors::GetSampler), so
// equal descriptions share one object and nothing is created per frame. Asset textures bound through a material
// keep the sampler their image owns; that is the other route and is unchanged.
namespace Desert::Graphic::RDG
{
    // One resolved texture entry: the binding PassContext::GetTexture returned for THIS execution of the pass.
    // Valid only until the exec lambda returns (see PassContext A(3)).
    struct BoundTexture
    {
        std::string        ShaderName;
        ShaderResourceKind Kind = ShaderResourceKind::SampledTexture;
        TextureBinding     Texture;
        SubresourceRange   Range    = SubresourceRange::All();
        Access             Declared = Access::None;
        // Set for SampledTexture (the entry's sampler), empty for StorageTexture.
        std::optional<SamplerDesc> Sampler;
    };

    struct BoundBuffer
    {
        std::string        ShaderName;
        ShaderResourceKind Kind = ShaderResourceKind::StorageBuffer;
        BufferBinding      Buffer;
        Access             Declared = Access::None;
    };

    // Fills one DeclaredBindingBlock from a setup lambda (PassBuilder::Bindings). Each call declares the access on
    // the pass exactly as PassBuilder::Read / Write would, so it is subject to the same declaration checks.
    class BindingBlockBuilder
    {
    public:
        BindingBlockBuilder& Sampled( std::string_view shaderName, TextureRef texture, Access declared,
                                      SubresourceRange range, SamplerDesc sampler );
        BindingBlockBuilder& Storage( std::string_view shaderName, TextureRef texture, Access declared,
                                      uint32_t mip = 0 );
        BindingBlockBuilder& Uniform( std::string_view shaderName, BufferRef buffer );
        BindingBlockBuilder& Storage( std::string_view shaderName, BufferRef buffer, Access declared );
        // The size the exec's PassBindings::PushConstants will give; 0 = none.
        BindingBlockBuilder& PushConstantBytes( uint32_t bytes );

        BindingBlockRef GetRef() const;

    private:
        friend class PassBuilder;
        BindingBlockBuilder( PassBuilder& pass, BindingBlockRef ref ) : m_Pass( pass ), m_Ref( ref )
        {
        }

        PassBuilder&    m_Pass;
        BindingBlockRef m_Ref;
    };

    // RDG-FAULT1. The pre-execution validation of one block - pure, no device, called by Builder::Compile for
    // every block of every pass. Success, or the FIRST mismatch as a stable reason (the reporter keys on it):
    //   "'<slot>' is not a resource of shader '<shader>'"            - the live 2026-10-05 glass case;
    //   "'<slot>' is a <kind> in shader '<shader>', bound as <kind>";
    //   "'<slot>' of shader '<shader>' is bound by the pass and by the material";
    //   "'<slot>' of shader '<shader>' is filled by neither the pass nor the material";
    //   "push constants: shader '<shader>' declares <n> bytes, the pass gives <m>" (both routes / neither / size).
    // These are exactly the checks ResolveRdgPassBindings makes at record time today; after FAULT1 that function
    // only places entries at their (set, binding) and its own refusals are late faults.
    Common::BoolResultStr ValidatePassBindings( const DeclaredBindingBlock& block );

    // The per-exec parameter block. Built from the PassContext the exec lambda receives; not copyable and
    // not movable, so it cannot be returned out of the lambda or stored in a renderer. Each Add* resolves its
    // ref through PassContext::GetTexture / GetBuffer immediately, so an undeclared resource, an access other
    // than the declared one, or a ref from another graph fails HERE, naming the pass, the resource and the
    // shader slot. The first failure is kept (GetStatus) and the consumer refuses a block that has one: a
    // renderer does not have to check every call, and a half-bound dispatch is never recorded.
    class PassBindings
    {
    public:
        explicit PassBindings( const PassContext& context );
        // RDG-FAULT1. Resolves every entry of the block @p block declared at setup through the context (the exec
        // adds only PushConstants). Replaces the name-taking Sampled / Storage / Uniform below, which go when
        // every renderer has moved its names into its setup (REMAINDER-FAULT1-C0, step C3).
        PassBindings( const PassContext& context, BindingBlockRef block );

        PassBindings( const PassBindings& )            = delete;
        PassBindings& operator=( const PassBindings& ) = delete;
        PassBindings( PassBindings&& )                 = delete;
        PassBindings& operator=( PassBindings&& )      = delete;

        // A texture read through @p sampler. @p declared is the access the setup declared for @p texture over
        // @p range (SampledCompute / SampledGraphics). @p range = Mip(m) gives a view of that mip alone (the
        // shader samples it at lod 0), All the whole image.
        PassBindings& Sampled( std::string_view shaderName, TextureRef texture, Access declared,
                               SubresourceRange range, SamplerDesc sampler );
        // A storage image, one mip over every layer. @p declared is StorageWrite or StorageRead as declared
        // for Mip(@p mip).
        PassBindings& Storage( std::string_view shaderName, TextureRef texture, Access declared,
                               uint32_t mip = 0 );
        // A graph buffer bound as a uniform or storage buffer, the whole buffer.
        PassBindings& Uniform( std::string_view shaderName, BufferRef buffer );
        PassBindings& Storage( std::string_view shaderName, BufferRef buffer, Access declared );
        // The push-constant block of the draw / dispatch, copied. Its size is checked against the shader's
        // declared push-constant range by the consumer.
        PassBindings& PushConstants( const void* data, uint32_t size );

        // Success, or the first failed entry: "<pass>: '<shader name>' <- '<resource>': <why>".
        Common::BoolResultStr GetStatus() const;

        const PassContext&            GetContext() const;
        std::span<const BoundTexture> GetTextures() const;
        std::span<const BoundBuffer>  GetBuffers() const;
        std::span<const std::byte>    GetPushConstants() const;

    private:
        const PassContext&        m_Context;
        std::vector<BoundTexture> m_Textures;
        std::vector<BoundBuffer>  m_Buffers;
        std::vector<std::byte>    m_PushConstants;
        std::string               m_FirstError; // empty while every entry resolved
    };
} // namespace Desert::Graphic::RDG
