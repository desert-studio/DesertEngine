#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGBackend.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGCompileResult.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <functional>
#include <optional>
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
    // RDGPassBindings.hpp: the setup-time parameter block of one draw / dispatch (RDG-FAULT1).
    class BindingBlockBuilder;
    struct ShaderBindingLayout;
    struct OtherRouteFill;

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
        [[nodiscard]] Common::ResultStr<TextureBinding>
        GetTexture( TextureRef texture, Access access, SubresourceRange range = SubresourceRange::All() ) const;
        [[nodiscard]] Common::ResultStr<BufferBinding> GetBuffer( BufferRef buffer, Access access ) const;
        // The description of any texture of this graph (UE: FRDGTexture::Desc) - Builder::GetTextureDesc seen
        // from a pass body. A description is not contents, so no declared access is needed to ask for it.
        [[nodiscard]] Common::ResultStr<TextureDesc> GetTextureDesc( TextureRef texture ) const;

        [[nodiscard]] std::string_view GetPassName() const;
        // RDG-FAULT1. The @p index-th binding block this pass's setup declared (PassBuilder::Bindings, in
        // declaration order; RenderPassDeclaration::Bindings returns that index). The exec opens it with
        // PassBindings( context, context.GetBindingBlock( index ) ); an index the setup never declared is refused
        // there, naming the pass.
        [[nodiscard]] BindingBlockRef GetBindingBlock( uint32_t index ) const;
        // RDG-CONTRACTS B(1). The pipe this pass records on (CompiledPass::OnPipe). For labels and profiling rows
        // only: an exec lambda records the same work on either pipe.
        [[nodiscard]] Pipe GetPipe() const;

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
        [[nodiscard]] IBackend& GetBackend() const
        {
            return m_Backend;
        }

    private:
        friend class Builder;
        friend class PassBindings; // PassBindings( context, block ) reads the block this pass declared
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

        // RDG-FAULT1. Declares the parameter block of one draw / dispatch this pass records, against the shader it
        // records with (UE: the pass parameter struct handed to AddPass, validated before the pass runs). Every
        // entry of the block IS the declaration of its access - no separate Read / Write for it - so the binding
        // and the barrier cannot disagree. Compile validates the block with ValidatePassBindings BEFORE anything
        // of the graph is recorded: a name the shader does not declare, a kind other than the declared descriptor
        // type, a slot neither the block nor @p other fills, or a push-constant size other than the shader's
        // faults THIS pass (PassFaultStage::Validation) and the graph goes on without it. The exec builds
        // PassBindings( context, block.GetRef() ) and names no shader slot itself. A pass recording several draws
        // with different shaders declares one block per shader.
        // @p layout is the kept layout (ShaderBindingLayoutCache / RDG::LayoutCache): the block shares it, no
        // copy.
        BindingBlockBuilder Bindings( const std::shared_ptr<const ShaderBindingLayout>& layout,
                                      OtherRouteFill                                    other );
        // A layout made for this declaration only (tests, a draw list built per frame): the graph takes it over.
        BindingBlockBuilder Bindings( ShaderBindingLayout layout, OtherRouteFill other );

    private:
        friend class Builder;
        friend class BindingBlockBuilder;
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

        // The description a created or registered texture carries (UE: FRDGTexture::Desc): a reader derives
        // what depends on its shape (mip count, extent, format) from the texture itself, never from a value
        // its producer publishes beside the ref. Refused for a handle that is not a texture of this graph.
        [[nodiscard]] Common::ResultStr<TextureDesc> GetTextureDesc( TextureRef texture ) const;

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

        // RDG-FAULT1 C3b (UE: FRDGBuilder::QueueBufferUpload). CPU data for a buffer of this graph, the only way
        // a graph buffer gets contents from the host. @p bytes are COPIED now (the caller's storage may die
        // right after the call), and a Copy pass "Upload: <buffer name>" is added HERE that writes them
        // (Access::CopyDst), so every pass added after this call that reads the buffer is ordered after the
        // upload by the graph, with its barrier; a reader added before it reads a buffer nothing wrote and is
        // refused ("... before any pass writes it"). The upload is culled with its buffer when nobody reads it.
        // Refused by name - a Declaration fault of the upload pass, whose readers then fault as its dependants -
        // for a handle that is not a buffer of this graph, an empty payload, a payload larger than the buffer,
        // or a size that is not a multiple of 4 bytes (the transfer granularity every GPU API shares).
        void QueueBufferUpload( BufferRef buffer, std::span<const std::byte> bytes );

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
        //
        // FAULT ISOLATION (RDG-FAULT1, RDGFault.hpp). Before culling, every pass is checked on its own: its
        // declaration errors (a malformed Read / Write / attachment is recorded against ITS pass, not the graph)
        // and every binding block it declared (ValidatePassBindings). A pass that fails either is a PassFault and
        // is removed as if it had not been added; then, to a fixed point, a pass that consumes a subresource whose
        // only producer was removed is substituted (a texture with a FaultDefault, DefaultSubstitution) or removed
        // too (PassFaultStage::Dependency, RootPass = the pass the chain starts at). Culling and everything after
        // it run on what is left. The returned error is reserved for a malformed GRAPH (a resource declared wrong
        // outside any pass, a FaultDefault in a graph whose FaultDefaults have no sources); a faulted pass is
        // never an error.
        [[nodiscard]] Common::ResultStr<CompileResult> Compile( const IMemoryRequirementsProvider& memory ) const;

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
        [[nodiscard]] Common::ResultStr<CompileResult> Compile( const IMemoryRequirementsProvider& memory,
                                                                const PipeCapabilities&            pipes ) const;

        // Pass culling is on by default. Off, every pass is live and CulledPasses stays empty: the debug switch
        // DebugViewState::DisablePassCulling, so that a picture which changes with it names a pass whose effect
        // is not declared to the graph.
        void SetPassCulling( bool enabled )
        {
            m_PassCulling = enabled;
        }
        [[nodiscard]] bool IsPassCullingEnabled() const
        {
            return m_PassCulling;
        }

        // RDG-FAULT1 (RDGFault.hpp). What a surviving reader reads when every pass that defined @p texture's
        // contents was removed by a fault. Declared by the creator of the transient (only the producer knows what
        // "nothing" is for its output); None, the default, culls the readers instead. Refused (a graph-level
        // declaration error) for a handle that is not a transient texture of this graph.
        void SetFaultDefault( TextureRef texture, FaultDefault value );
        // What losing every writer of a registered external means: KeepsContents unless its owner says otherwise.
        // The swapchain image and an editor viewport's presented image are FrameFatal; a history the next frame
        // reads is InvalidateHistory. Refused for a handle that is not an external of this graph.
        void SetFaultPolicy( TextureRef external, ExternalFaultPolicy policy );
        void SetFaultPolicy( BufferRef external, ExternalFaultPolicy policy );
        // What the FaultDefault values mean in this graph (their system-texture sources and clears). Its sources
        // are set by RegisterSystemTextures, so every graph that registers its system textures can honour a
        // FaultDefault without a second call site.
        FaultDefaults&                     GetFaultDefaults();
        [[nodiscard]] const FaultDefaults& GetFaultDefaults() const;

        // Compiles against the backend's memory requirements, has the backend acquire physical resources,
        // then for every executed pass in order: label/timestamp, its one barrier batch, begin render pass
        // (raster with attachments), the exec lambda, end rendering. Finishes with the final barriers and
        // writes the final states (and an extracted transient's image) back into every external and
        // extraction target. Runs once per builder.
        //
        // RDG-FAULT1. A faulted pass does not fail the graph. Faults found by Compile are already out of the plan.
        // A LATE fault - an exec lambda returning an error after its pass began recording - cannot be un-recorded:
        // Execute closes the render pass the pass had open (a following pass compiled with ContinuesRenderPass
        // opens its own instead; merged passes load every attachment, so the decisions stay valid), records the
        // pass's EpilogueBarriers and EndPass, and goes on. Every later pass that reads what it wrote is then
        // treated like a Compile-time dependant: a texture with a FaultDefault is read through PassContext as the
        // system texture (system textures are permanently in a sampled state and always bound once a FaultDefault
        // exists), anything else skips that pass's render pass and exec - its barrier batch is still recorded so
        // the layout chain the plan computed stays true - and the skip propagates. After the last pass, the
        // execute's faults go to backend.GetPassFaultReporter() (the only place they are logged) and are kept in
        // GetExecuteReport(). The return is an error exactly when the frame has no defined picture (FrameFault):
        // the caller clears FrameFault::Externals to black and presents; it never logs the error itself.
        Common::BoolResultStr Execute( IBackend& backend );

        // The faults of the last Execute (empty before it); what an editor panel or a test asks.
        [[nodiscard]] const ExecuteReport& GetExecuteReport() const;
        // RDG-FAULT1. The external texture registered as resource @p resource (null for a transient, a buffer or
        // an index out of range): how the caller reaches the images of FrameFault::Externals to clear them.
        [[nodiscard]] ExternalTexture* FindExternalTexture( uint32_t resource ) const;

        [[nodiscard]] const std::string& GetName() const
        {
            return m_Name;
        }

    private:
        friend class PassBuilder;
        friend class PassContext;
        friend class BindingBlockBuilder; // appends the entries of a declared block (RDGPassBindings.cpp)
        friend class PassBindings;        // resolves a declared block in the exec
        // Compile's working state and its phases (RDGCompile.cpp; UE FRDGBuilder::Compile).
        class Compiler;

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
            uint32_t    Slot      = 0;
            bool        IsDepth   = false;
            bool        IsResolve = false;
            uint32_t    Resource  = kInvalidResource;
            LoadOp      Load;
            uint32_t    Mip        = 0;
            uint32_t    BaseLayer  = 0;
            uint32_t    LayerCount = 1;
            StoreAction Store      = StoreAction::Store; // declared; Compile may still discard
        };

        struct PassRecord
        {
            std::string                   Name;
            PassFlags                     Flags = PassFlags::None;
            std::vector<ResourceUse>      Uses;
            std::vector<AttachmentRecord> Attachments;
            ExecFunction                  Exec;
            // RDG-FAULT1: the binding blocks its setup declared (PassBuilder::Bindings) and the first malformed
            // declaration of THIS pass. A pass with a declaration error is a Declaration fault, not a graph error.
            std::vector<DeclaredBindingBlock> Blocks;
            std::string                       DeclarationError;
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
            FaultDefault        Default        = FaultDefault::None; // SetFaultDefault (transients)
            ExternalFaultPolicy Policy         = ExternalFaultPolicy::KeepsContents; // SetFaultPolicy (externals)

            [[nodiscard]] bool IsExternal() const
            {
                return ExternalTex != nullptr || ExternalBuf != nullptr;
            }
            [[nodiscard]] bool IsExtracted() const
            {
                return HasFinalAccess;
            }
            [[nodiscard]] uint32_t SubresourceCount() const
            {
                return Kind == ResourceKind::Texture ? Texture.SubresourceCount() : 1;
            }
        };

        PassBuilder BeginPass( std::string_view name, PassFlags flags );
        // Keeps the FIRST declaration error: later ones are usually its consequences.
        void                                RecordError( std::string message );
        // RDG-FAULT1: a malformed declaration inside pass @p pass faults that pass only (first one kept).
        void                                RecordPassError( uint32_t pass, std::string message );
        [[nodiscard]] const ResourceRecord* FindResource( uint32_t index, ResourceKind kind ) const;
        // RDG-FAULT1: FrameFatal external @p resource as a FrameFault lists it (with its Extract's final access).
        [[nodiscard]] FrameFaultExternal MakeFrameFaultExternal( uint32_t resource ) const;

        std::string                 m_Name;
        std::vector<ResourceRecord> m_Resources;
        std::vector<PassRecord>     m_Passes;
        std::string                 m_DeclarationError;
        bool                        m_Executed    = false;
        bool                        m_PassCulling = true;
        // RDG-FAULT1. What a FaultDefault names and clears to (FaultDefaults), the faults of the last
        // Execute, and the substitutions Execute made for LATE faults (PassContext::GetTexture honours them
        // together with CompileResult::Substitutions).
        FaultDefaults                    m_FaultDefaults;
        ExecuteReport                    m_Report;
        std::vector<DefaultSubstitution> m_LateSubstitutions;
    };
} // namespace Desert::Graphic::RDG
