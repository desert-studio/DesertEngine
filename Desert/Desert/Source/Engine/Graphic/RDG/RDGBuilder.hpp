#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGBackend.hpp>
#include <Engine/Graphic/RDG/RDGCompileResult.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The render graph core (plan RDG0 §2.2/§2.4). A graph is built every frame for every view: passes are
// added in the order they must run, each declares its resources in a setup lambda, and Compile turns the
// declarations into data - what runs, what is culled, the barrier batch before every pass, load/store
// decisions, transient lifetimes and an aliasing plan with the memory peak. Nothing here touches a
// device: memory requirements and every recorded command go through the interfaces in RDGBackend.hpp.
namespace Desert::Graphic::RDG
{
    class Builder;

    struct TextureBinding
    {
        uint32_t           Resource = kInvalidResource;
        std::string_view   Name;
        const TextureDesc* Desc     = nullptr;
        ExternalTexture*   External = nullptr; // null for a transient
        const Allocation*  Memory   = nullptr; // null for an external or extracted texture
        IPhysicalTexture*  Physical = nullptr; // the backend's image
    };

    struct BufferBinding
    {
        uint32_t          Resource = kInvalidResource;
        std::string_view  Name;
        const BufferDesc* Desc     = nullptr;
        ExternalBuffer*   External = nullptr;
        const Allocation* Memory   = nullptr;
        IPhysicalBuffer*  Physical = nullptr;
    };

    // What an exec lambda sees. Get* is the ONLY way from a handle to a resource inside a pass, which is
    // what lets it refuse a resource the pass never declared: an undeclared read would get no barrier and
    // an undeclared write would be invisible to culling, and neither shows up as anything but a wrong
    // picture. The check costs a scan of the pass's declarations, so it is compiled out of Shipping
    // (DESERT_DEV_INSTRUMENTS), like every other development-only check.
    class PassContext
    {
    public:
        Common::ResultStr<TextureBinding> GetTexture( TextureRef texture, Access access,
                                                      SubresourceRange range = SubresourceRange::All() ) const;
        Common::ResultStr<BufferBinding>  GetBuffer( BufferRef buffer, Access access ) const;

        std::string_view GetPassName() const;
        // RDG-CONTRACTS B(1). The pipe this pass records on (CompiledPass::OnPipe). For labels and profiling rows
        // only: an exec lambda records the same work on either pipe.
        Pipe GetPipe() const;

        // RDG-CONTRACTS A(3) - the renderer-facing contract for a graph transient (UE: FRDGTexture accessed
        // through the pass parameters, never stored). A renderer moving an owned intermediate into the graph:
        //   * declares it EVERY frame in the graph it builds: Builder::CreateTexture / CreateBuffer with a desc
        //     computed from this frame's view (size, format, mips) - no member Image2D, no Resize();
        //   * keeps only the TextureRef / BufferRef, and only for the graph being built (a local, or a field
        //     of a per-frame struct such as FrameTextures). A ref from an earlier graph is meaningless;
        //   * inside exec, gets the binding with GetTexture / GetBuffer and the backend object through the
        //     backend's helper (VulkanRdgBackend::TextureOf -> VulkanRdgTexture::GetView);
        //   * binds it through a descriptor set written in THIS exec (VulkanRdgBackend::DescriptorsOf), never
        //     through a Material property or a descriptor set that outlives the frame.
        // Never: cache a TextureBinding, IPhysicalTexture, VkImage, VkImageView or descriptor set written from
        // one across frames or across graphs (the allocator may place a different image at the same handle
        // value, or the same image for a different resource); read a transient's contents from a previous frame
        // (a history buffer is an external, or an extracted transient registered back next frame).
        // The backend recording this graph; a backend-specific helper turns it into its command buffer.
        IBackend& GetBackend() const
        {
            return m_Backend;
        }

    private:
        friend class Builder;
        PassContext( const Builder& builder, const CompileResult& result, IBackend& backend, uint32_t pass )
             : m_Builder( builder ), m_Result( result ), m_Backend( backend ), m_Pass( pass )
        {
        }

        const Builder&       m_Builder;
        const CompileResult& m_Result;
        IBackend&            m_Backend;
        uint32_t             m_Pass;
    };

    // Declarations of one pass, handed to its setup lambda. A malformed declaration (wrong resource kind,
    // range outside the texture, an attachment in a compute pass...) is recorded with the pass and
    // resource names and returned by Compile; the setup lambda does not have to check every call.
    class PassBuilder
    {
    public:
        void Read( TextureRef texture, Access access, SubresourceRange range = SubresourceRange::All() );
        void Write( TextureRef texture, Access access, SubresourceRange range = SubresourceRange::All() );
        void Read( BufferRef buffer, Access access );
        void Write( BufferRef buffer, Access access );

        // @p layer = kAllRemaining binds every layer (layered rendering); a single layer renders one
        // cascade of a layered shadow map.
        void ColorTarget( uint32_t slot, TextureRef texture, const LoadOp& load, uint32_t mip = 0,
                          uint32_t layer = kAllRemaining, StoreAction store = StoreAction::Store );
        // @p store = DontCare discards the contents after the render pass (Compile also discards what nothing
        // after the pass reads); Store keeps them when anything later or outside the graph needs them.
        void DepthTarget( TextureRef texture, const LoadOp& load, bool write = true,
                          uint32_t layer = kAllRemaining, StoreAction store = StoreAction::Store );
        // The single-sample image the multisampled ColorTarget of @p slot resolves into at the end of the render
        // pass. The ColorTarget is declared first; both have the same format and size. The resolve overwrites
        // every pixel, so the image's previous contents are not loaded.
        void ResolveTarget( uint32_t slot, TextureRef texture );

    private:
        friend class Builder;
        PassBuilder( Builder& builder, uint32_t pass ) : m_Builder( builder ), m_Pass( pass )
        {
        }

        void DeclareTexture( TextureRef texture, Access access, SubresourceRange range, bool asWrite,
                             std::string_view call );
        void DeclareBuffer( BufferRef buffer, Access access, bool asWrite, std::string_view call );
        void DeclareAttachment( uint32_t slot, bool isDepth, TextureRef texture, Access access, const LoadOp& load,
                                uint32_t mip, uint32_t layer, StoreAction store, bool isResolve = false );

        Builder& m_Builder;
        uint32_t m_Pass;
    };

    class Builder
    {
    public:
        using ExecFunction = std::function<Common::BoolResultStr( PassContext& )>;

        explicit Builder( std::string name ) : m_Name( std::move( name ) )
        {
        }
        Builder( const Builder& )            = delete;
        Builder& operator=( const Builder& ) = delete;

        TextureRef CreateTexture( const TextureDesc& desc, std::string_view name );
        BufferRef  CreateBuffer( const BufferDesc& desc, std::string_view name );

        // The resource's current state is the one it carries (ExternalTexture::SubresourceStates); the
        // graph does not take a second copy of it as an argument.
        TextureRef RegisterExternal( ExternalTexture& texture, std::string_view name );
        BufferRef  RegisterExternal( ExternalBuffer& buffer, std::string_view name );

        // Registers the attachments of an engine framebuffer, each carrying the state the engine recorded
        // for it (colour i as "<name>.Color<i>", depth as "<name>.Depth"). @p depth may be null. Execute
        // writes the final states back through each texture's RecordStates (after every barrier on it and at the
        // end).
        // @p resolves (by colour slot, multisampled framebuffers only) are registered as "<name>.Resolve<i>".
        ImportedFramebuffer ImportFramebuffer( std::span<ExternalTexture* const> colors, ExternalTexture* depth,
                                               std::string_view                  name,
                                               std::span<ExternalTexture* const> resolves = {} );

        // A transient extracted into @p into survives the graph: it is a culling root, is never aliased,
        // and @p into receives its description and final states on Execute. For an external resource
        // @p into must be the object it was registered from, and the call only sets the final access
        // (e.g. Present for a swapchain image).
        void Extract( TextureRef texture, ExternalTexture& into, Access final );
        void Extract( BufferRef buffer, ExternalBuffer& into, Access final );

        template <class Setup, class Exec>
        void AddPass( std::string_view name, PassFlags flags, Setup&& setup, Exec&& exec )
        {
            static_assert( std::is_invocable_v<Setup, PassBuilder&>, "setup lambda takes PassBuilder&" );
            static_assert( std::is_invocable_r_v<Common::BoolResultStr, Exec, PassContext&>,
                           "exec lambda takes PassContext& and returns Common::BoolResultStr" );
            PassBuilder passBuilder = BeginPass( name, flags );
            std::forward<Setup>( setup )( passBuilder );
            m_Passes.back().Exec = ExecFunction( std::forward<Exec>( exec ) );
        }

        // Pure: reads the declarations, touches nothing, may be called any number of times. @p memory
        // answers the size / alignment / memory types of each transient for the aliasing plan.
        //
        // PASS CULLING (UE's FRDGBuilder::Compile). A pass executes only if it is live:
        //   * it is NeverCull - its effect is one the graph cannot see (a readback, a query, a clock it advances);
        //   * or it writes an EXTERNALLY VISIBLE subresource. A resource is externally visible when its contents
        //     leave the graph through a registered hand-back: RegisterExternal / ImportFramebuffer (the image
        //     outlives the graph, and Execute writes every external's final state back into its
        //     SubresourceStates and through RecordFinalStates / RecordFinalState when set) or Extract (a transient
        //     that survives the graph, or an external's final access such as Present). A plain transient is not;
        //   * or it produced a subresource (one mip/layer of a texture, or a buffer) that a live pass consumes: it
        //     was the last pass before that consumer to write it. A read consumes; an attachment that is cleared
        //     or not loaded does not, so the pass that wrote it before is not kept alive by it.
        // A culled pass records nothing, gets no barrier and opens no lifetime; CompileResult::CulledPasses and
        // CulledPassNames list it.
        Common::ResultStr<CompileResult> Compile( const IMemoryRequirementsProvider& memory ) const;

        // RDG-CONTRACTS B(2). The same compile, scheduled for @p pipes. Compile(memory) above is this overload
        // with PipeCapabilities{} (no separate compute family). Scheduling runs after culling and before the
        // aliasing plan (which uses ResourceLifetime::AliasFirst/LastPosition):
        //   1. pipe choice: a live pass with AsyncCompute runs on Pipe::AsyncCompute if
        //   pipes.SeparateComputeFamily,
        //      otherwise on Graphics and is listed in DemotedAsyncPasses;
        //   2. segments: consecutive async passes form one AsyncCompute segment; the fork is the last graphics
        //      writer of anything the segment reads, the join the first graphics pass depending on anything the
        //      segment touched (CrossPipeSync). A graphics pass depending on an async pass WITHOUT a sync is a
        //      compile error, never a silently serialised pass;
        //   3. ownership: one QueueOwnershipTransfer per (resource, range) whose contents cross pipes;
        //   4. lifetimes: AliasFirst/LastPosition widened over the fork..join window.
        // The single-pipe result equals Compile(memory) on the same graph, except DemotedAsyncPasses.
        Common::ResultStr<CompileResult> Compile( const IMemoryRequirementsProvider& memory,
                                                  const PipeCapabilities&            pipes ) const;

        // Pass culling is on by default. Off, every pass is live and CulledPasses stays empty: the debug switch
        // DebugViewState::DisablePassCulling, so that a picture which changes with it names a pass whose effect
        // is not declared to the graph.
        void SetPassCulling( bool enabled )
        {
            m_PassCulling = enabled;
        }
        bool IsPassCullingEnabled() const
        {
            return m_PassCulling;
        }

        // Compiles against the backend's memory requirements, has the backend acquire physical resources,
        // then for every executed pass in order: label/timestamp, its one barrier batch, begin render pass
        // (raster with attachments), the exec lambda, end rendering. Finishes with the final barriers and
        // writes the final states (and an extracted transient's image) back into every external and
        // extraction target. Runs once per builder.
        Common::BoolResultStr Execute( IBackend& backend );

        const std::string& GetName() const
        {
            return m_Name;
        }

    private:
        friend class PassBuilder;
        friend class PassContext;

        Common::BoolResultStr RecordExternalStates( std::span<const Barrier> barriers );

        struct ResourceUse
        {
            uint32_t         Resource = kInvalidResource;
            Access           Usage    = Access::None;
            SubresourceRange Range;           // resolved: no kAllRemaining
            int32_t          Attachment = -1; // index into PassRecord::Attachments, -1 for a plain use
        };

        struct AttachmentRecord
        {
            uint32_t Slot     = 0;
            bool     IsDepth   = false;
            bool     IsResolve = false;
            uint32_t Resource  = kInvalidResource;
            LoadOp   Load;
            uint32_t Mip        = 0;
            uint32_t BaseLayer  = 0;
            uint32_t LayerCount = 1;
            StoreAction Store      = StoreAction::Store; // declared; Compile may still discard
        };

        struct PassRecord
        {
            std::string                   Name;
            PassFlags                     Flags = PassFlags::None;
            std::vector<ResourceUse>      Uses;
            std::vector<AttachmentRecord> Attachments;
            ExecFunction                  Exec;
        };

        struct ResourceRecord
        {
            std::string      Name;
            ResourceKind     Kind = ResourceKind::Texture;
            TextureDesc      Texture;
            BufferDesc       Buffer;
            ExternalTexture* ExternalTex    = nullptr;
            ExternalBuffer*  ExternalBuf    = nullptr;
            ExternalTexture* ExtractTex     = nullptr;
            ExternalBuffer*  ExtractBuf     = nullptr;
            bool             HasFinalAccess = false;
            Access           FinalAccess    = Access::None;

            bool IsExternal() const
            {
                return ExternalTex != nullptr || ExternalBuf != nullptr;
            }
            bool IsExtracted() const
            {
                return HasFinalAccess;
            }
            uint32_t SubresourceCount() const
            {
                return Kind == ResourceKind::Texture ? Texture.SubresourceCount() : 1;
            }
        };

        PassBuilder BeginPass( std::string_view name, PassFlags flags );
        // Keeps the FIRST declaration error: later ones are usually its consequences.
        void                  RecordError( std::string message );
        const ResourceRecord* FindResource( uint32_t index, ResourceKind kind ) const;

        std::string                 m_Name;
        std::vector<ResourceRecord> m_Resources;
        std::vector<PassRecord>     m_Passes;
        std::string                 m_DeclarationError;
        bool                        m_Executed    = false;
        bool                        m_PassCulling = true;
    };
} // namespace Desert::Graphic::RDG
