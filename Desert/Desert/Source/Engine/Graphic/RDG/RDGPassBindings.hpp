#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
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
    // RDG-PARALLEL-9 (RDG-REC-2): ShaderName views the declared entry's name (DeclaredBindingEntry::ShaderName
    // of the block the pass declared at setup), which the graph keeps unchanged until the exec returns: no
    // string is copied per entry per exec.
    struct BoundTexture
    {
        std::string_view   ShaderName;
        ShaderResourceKind Kind = ShaderResourceKind::Texture;
        TextureBinding     Texture;
        SubresourceRange   Range    = SubresourceRange::All();
        Access             Declared = Access::None;
        // Set for Sampler (the shared state), empty for the texture kinds.
        std::optional<SamplerDesc> Sampler;
        // FPS-ARCH-BINDINGS: the declared entry's slot index in the block's layout and its name hash
        // (DeclaredBindingEntry::Slot / NameHash): what the backend places the entry by, never the name.
        uint32_t                   Slot     = kNoShaderSlot;
        uint64_t                   NameHash = 0;
    };

    struct BoundBuffer
    {
        std::string_view   ShaderName;
        ShaderResourceKind Kind = ShaderResourceKind::StorageBuffer;
        BufferBinding      Buffer;
        Access             Declared = Access::None;
        uint32_t           Slot     = kNoShaderSlot; // as BoundTexture::Slot
        uint64_t           NameHash = 0;
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
        // One layer of one mip of an arrayed texture: the pass declares (and RDG tracks, barriers and culls) that
        // subresource alone, so passes writing distinct layers of one array never overwrite each other's. The
        // view stays arrayed (a Tex2DArray's view is 2D_ARRAY of one layer): the shader addresses its layer 0.
        BindingBlockBuilder& StorageLayer( StorageTextureParameter parameter, TextureRef texture, Access declared,
                                           uint32_t mip, uint32_t layer );
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
        // RDG-PARALLEL-9 (RDG-REC-2): hands the entry storage back to this thread's free lists, so the next block
        // opened on it allocates nothing once they have grown.
        ~PassBindings();

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
        // The declared block this one was opened from. Within one exec of one pass, two PassBindings of the same
        // block resolve to the same entries (the constructor reads only the block and the context), so a backend
        // may key what it builds from them by it (VulkanRdgBackend's pass set memo).
        [[nodiscard]] BindingBlockRef GetBlock() const;
        // FPS-ARCH-BINDINGS: the layout the block was declared and validated against (every entry's Slot indexes
        // its Slots); null only for a block that failed to open (GetStatus is then an error).
        [[nodiscard]] const ShaderBindingLayout* GetLayout() const noexcept
        {
            return m_Layout;
        }

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
        BindingBlockRef           m_Block;
        const ShaderBindingLayout* m_Layout = nullptr;
        std::vector<BoundTexture> m_Textures;
        std::vector<BoundBuffer>  m_Buffers;
        std::vector<std::byte>    m_PushConstants;
        std::string               m_FirstError; // empty while every entry resolved
    };

    // RDG-PARALLEL-9 (RDG-REC-2; UE: pass parameters live in the graph's allocator, never the heap per draw list),
    // shared by every exec that opens one block per material (MeshDrawList::Record, TerrainRenderer::RecordDraws)
    // since CPU-FRAME-ARENA 3: the blocks @p first .. @p first + @p count - 1 of the pass, opened in slots this
    // thread keeps. A record leases the next slots for its blocks and closes them when it ends (a nested record on
    // the thread leases the ones after), so a steady frame opens its blocks with no allocation: the slot is kept,
    // and the block's entry storage comes back from PassBindings' own per-thread lists. Each slot is a heap object
    // of its own, so growing the list moves no open block. A block is never kept past its record.
    class OpenBindingBlocks
    {
    public:
        OpenBindingBlocks( const PassContext& context, const uint32_t first, const size_t count )
             : m_Base( Slots().Used ), m_Count( count )
        {
            ThreadSlots& open = Slots();
            while ( open.Slots.size() < m_Base + m_Count )
                open.Slots.push_back( std::make_unique<std::optional<PassBindings>>() );
            open.Used = m_Base + m_Count;
            for ( size_t index = 0; index < m_Count; ++index )
                open.Slots[m_Base + index]->emplace(
                     context, context.GetBindingBlock( first + static_cast<uint32_t>( index ) ) );
        }
        ~OpenBindingBlocks()
        {
            ThreadSlots& open = Slots();
            for ( size_t index = 0; index < m_Count; ++index )
                open.Slots[m_Base + index]->reset();
            open.Used = m_Base;
        }
        OpenBindingBlocks( const OpenBindingBlocks& )            = delete;
        OpenBindingBlocks& operator=( const OpenBindingBlocks& ) = delete;
        OpenBindingBlocks( OpenBindingBlocks&& )                 = delete;
        OpenBindingBlocks& operator=( OpenBindingBlocks&& )      = delete;

        [[nodiscard]] size_t Count() const noexcept
        {
            return m_Count;
        }
        // Block @p index of the lease (pass block first + index).
        [[nodiscard]] const PassBindings& At( const size_t index ) const
        {
            return **Slots().Slots[m_Base + index];
        }

    private:
        struct ThreadSlots
        {
            std::vector<std::unique_ptr<std::optional<PassBindings>>> Slots;
            size_t                                                     Used = 0;
        };
        [[nodiscard]] static ThreadSlots& Slots() noexcept
        {
            thread_local ThreadSlots t_Slots;
            return t_Slots;
        }

        size_t m_Base  = 0;
        size_t m_Count = 0;
    };
} // namespace Desert::Graphic::RDG
