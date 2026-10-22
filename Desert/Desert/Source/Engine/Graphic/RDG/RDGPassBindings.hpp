#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

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
// SLOTS ARE SHADER NAMES. An entry names the resource as the shader declares it ("BloomTexture",
// "DestinationMipTexture"); the consumer resolves the name against the pipeline's shader reflection to (set, binding,
// descriptor type). A name the shader does not declare, a kind that does not match the declared descriptor
// type (Sampled into a storage-image slot), and a resource slot of the shader that neither this block nor the
// material fills are errors returned by the consumer, never a silent fallback image. Binding indices never
// appear at a call site: they are the shader's business and drift with it.
//
// SAMPLERS. UE's shared-sampler form is the one form a pass has: an input is a separate texture (Texture) and
// the shader reads it through a sampler slot the pass fills with a SamplerDesc (Sampler), as UE's pass parameters
// carry TStaticSamplerState next to the textures. A pass never binds a combined sampler (sampler2D): one sampler
// descriptor per input put the deferred composite over maxPerStageDescriptorSamplers. There is no default and no
// implicit sampler on the graph route: the
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
        ShaderResourceKind Kind = ShaderResourceKind::Texture;
        TextureBinding     Texture;
        SubresourceRange   Range    = SubresourceRange::All();
        Access             Declared = Access::None;
        // Set for Sampler (the shared state), empty for the texture kinds.
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
        // Every slot is named by a parameter (RDGShaderParameters.hpp): a member of the pass's parameter struct or
        // a named engine constant of the parameter type, never a string - the member's kind admits only its own
        // call, and the name is spelled once. An element of an arrayed slot is parameter[i] ("<name>[<i>]").
        //
        // A separate texture slot (ShaderResourceKind::Texture): the image alone, sampled through a Sampler slot
        // of the same pass.
        BindingBlockBuilder& Texture( TextureParameter parameter, TextureRef texture, Access declared,
                                      SubresourceRange range );
        // A separate sampler slot (ShaderResourceKind::Sampler): the state, no resource. A SHARED state: every
        // Texture entry read through it names it, and the block keeps one entry per slot - naming it again with
        // the same state adds nothing, with another state it is a second entry that ValidatePassBindings refuses
        // ("bound twice"), so one slot never carries two states.
        BindingBlockBuilder& Sampler( SamplerParameter parameter, SamplerDesc sampler );
        BindingBlockBuilder& Storage( StorageTextureParameter parameter, TextureRef texture, Access declared,
                                      uint32_t mip = 0 );
        BindingBlockBuilder& Uniform( UniformBufferParameter parameter, BufferRef buffer );
        BindingBlockBuilder& Storage( StorageBufferParameter parameter, BufferRef buffer, Access declared );
        // A ray-queried scene: @p tlas is the buffer an acceleration-structure build wrote (RayTracingSceneFrame::
        // Tlas); declared AccelStructRead, bound as the acceleration structure it stores.
        BindingBlockBuilder& AccelerationStructure( AccelerationStructureParameter parameter, BufferRef tlas );
        // The size the exec's PassBindings::PushConstants will give; 0 = none.
        BindingBlockBuilder& PushConstantBytes( uint32_t bytes );

        [[nodiscard]] BindingBlockRef GetRef() const;

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
    //   "'<slot>' of shader '<shader>' is bound twice by the pass";
    //   "'<slot>' is a <kind> in shader '<shader>', bound as <kind>";
    //   "'<slot>' of shader '<shader>' is bound by the pass and by the material";
    //   "'<slot>' of shader '<shader>' is filled by neither the pass nor the material";
    //   "push constants: shader '<shader>' declares <n> bytes, the pass gives <m>" (both routes / neither / size).
    // These are exactly the checks ResolveRdgPassBindings makes at record time today; after FAULT1 that function
    // only places entries at their (set, binding) and its own refusals are late faults.
    Common::BoolResultStr ValidatePassBindings( const DeclaredBindingBlock& block );

    // The per-exec parameter block. Opened from the PassContext the exec lambda receives and the block its setup
    // declared (PassBuilder::Bindings); not copyable and not movable, so it cannot be returned out of the lambda
    // or stored in a renderer. Every slot name, access and range comes from that declaration - the exec names no
    // shader slot and only adds the push constants. Each declared entry is resolved through PassContext::
    // GetTexture / GetBuffer when the block is opened; a failure there (a block of another pass, a binding the
    // execution cannot give) is kept as the first failure (GetStatus) and the consumer refuses a block that has
    // one, so a half-bound dispatch is never recorded.
    class PassBindings
    {
    public:
        // RDG-FAULT1. Resolves every entry of the block @p block declared at setup through the context (the exec
        // adds only PushConstants). This is the only constructor: the name-taking exec route (a PassBindings built
        // from the context alone and filled with Sampled / Storage / Uniform by shader name) is gone, and the
        // RenderGraphCompile census TheNameTakingExecBindingApiStaysDeleted keeps it gone.
        PassBindings( const PassContext& context, BindingBlockRef block );

        PassBindings( const PassBindings& )            = delete;
        PassBindings& operator=( const PassBindings& ) = delete;
        PassBindings( PassBindings&& )                 = delete;
        PassBindings& operator=( PassBindings&& )      = delete;

        // The push-constant block of the draw / dispatch, copied. Its size is checked against the shader's
        // declared push-constant range by the consumer.
        PassBindings& PushConstants( const void* data, uint32_t size );

        // Success, or the first failed entry: "<pass>: '<shader name>' <- '<resource>': <why>".
        [[nodiscard]] Common::BoolResultStr GetStatus() const;

        [[nodiscard]] const PassContext&            GetContext() const;
        [[nodiscard]] std::span<const BoundTexture> GetTextures() const;
        [[nodiscard]] std::span<const BoundBuffer>  GetBuffers() const;
        [[nodiscard]] std::span<const std::byte>    GetPushConstants() const;

    private:
        // How the constructor resolves one declared entry. A sampled texture: @p range = Mip(m) is a view of that
        // mip alone (the shader samples it at lod 0), All the whole image. A storage image: one mip over every
        // layer.
        // A separate texture (ShaderResourceKind::Texture) resolves the same way with no @p sampler.
        void ResolveSampled( std::string_view shaderName, ShaderResourceKind kind, TextureRef texture,
                             Access declared, SubresourceRange range, std::optional<SamplerDesc> sampler );
        // A sampler slot: no resource, only the state.
        void ResolveSampler( std::string_view shaderName, SamplerDesc sampler );
        void ResolveStorageImage( std::string_view shaderName, TextureRef texture, Access declared, uint32_t mip );

        const PassContext&        m_Context;
        std::vector<BoundTexture> m_Textures;
        std::vector<BoundBuffer>  m_Buffers;
        std::vector<std::byte>    m_PushConstants;
        std::string               m_FirstError; // empty while every entry resolved
    };
} // namespace Desert::Graphic::RDG
