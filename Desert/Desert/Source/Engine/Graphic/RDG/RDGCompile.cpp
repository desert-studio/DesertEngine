// Builder::Compile - the device-free half of the render graph (plan RDG0 §2.4). Ported from the shape of UE
// 5.8 FRDGBuilder::Compile (Runtime/RenderCore/Private/RenderGraphBuilder.cpp): one Builder::Compiler member
// function per phase - FlushCullStack, the fork/join pass, MergeRenderPasses, CollectPassBarriers,
// CompilePassOps, AllocateTransientResources, CreatePassBarriers, FinalizeResources - adapted to a graph
// whose passes are never reordered.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <set>
#include <tuple>

namespace Desert::Graphic::RDG
{
    namespace
    {
        // One pass's view of one subresource after its declarations are folded together.
        struct RdgSubUse
        {
            uint32_t    Sub      = 0; // global subresource index
            uint32_t    Resource = kInvalidResource;
            AccessState State;
            Access      FirstAccess = Access::None; // for messages
            uint32_t    AccessMask  = 0;
            bool        Writes      = false;
            // Needs the previous contents: every use except an attachment that clears or discards.
            bool Consumes = true;
        };

        // A transition on one subresource before one pass, before ranges are merged.
        struct RdgRawTransition
        {
            uint32_t    Sub      = 0;
            uint32_t    Resource = kInvalidResource;
            AccessState Before;
            AccessState After;
            bool        Discard = false;
            // Ownership transfers carry their pipes and the position of the other half (the acquire's
            // release position, the release's acquire position); every other transition keeps the defaults.
            BarrierKind Kind    = BarrierKind::Transition;
            Pipe        SrcPipe = Pipe::Graphics;
            Pipe        DstPipe = Pipe::Graphics;
            uint32_t    Partner = 0;

            [[nodiscard]] bool SameTransition( const RdgRawTransition& other ) const
            {
                return Before == other.Before && After == other.After && Discard == other.Discard &&
                       Kind == other.Kind && SrcPipe == other.SrcPipe && DstPipe == other.DstPipe &&
                       Partner == other.Partner;
            }
        };

        // Per-subresource walk state while barriers are collected.
        struct RdgSubTrack
        {
            bool        Touched = false;
            AccessState GroupBefore; // state the current group was entered from
            AccessState Group;       // current (possibly merged) state
            uint32_t    GroupPosition = 0;
            int32_t     GroupRaw      = -1;             // index of the group's transition in its pass's raw list
            Pipe        GroupPipe     = Pipe::Graphics; // the pipe that owns the subresource now
            uint32_t    LastTouch     = 0;              // last position that used it (on GroupPipe)
            // When the group was opened by an ownership acquire: its release half, widened together with it.
            int32_t  ReleaseRaw      = -1;
            uint32_t ReleasePosition = 0;
        };

        // A maximal run of consecutive executed passes on Pipe::AsyncCompute, with its fork and join.
        // RdgAsyncRun::ForkPosition of a run whose fork the graphics prologue segment signals (amendment B).
        constexpr int32_t kRdgForkAtGraphStart = -2;

        struct RdgAsyncRun
        {
            uint32_t First        = 0;
            uint32_t Last         = 0;
            int32_t  ForkPosition = -1; // graphics position that signals the fork; -1: no fork
            uint32_t JoinPosition = CrossPipeSync::kJoinAtGraphEnd;
            int32_t  ForkSync     = -1; // index into CompileResult::Syncs
            int32_t  JoinSync     = -1;
            bool     NeedsFork    = false; // an external it consumes was never touched by this graph before
        };

        Barrier RdgMakeBarrier( const RdgRawTransition& raw, ResourceKind kind, const SubresourceRange& range )
        {
            Barrier barrier;
            barrier.BarrierType     = raw.Kind;
            barrier.SrcPipe         = raw.SrcPipe;
            barrier.DstPipe         = raw.DstPipe;
            barrier.Resource        = raw.Resource;
            barrier.Kind            = kind;
            barrier.Range           = range;
            barrier.Before          = raw.Before;
            barrier.After           = raw.After;
            barrier.DiscardContents = raw.Discard;
            // The release half makes nothing visible on its own queue and the acquire half waits for nothing
            // on its own queue: the semaphore between them carries the dependency, the layouts match.
            if ( raw.Kind == BarrierKind::OwnershipRelease )
                barrier.After = AccessState{ PipelineStage_None, MemoryAccess_None, raw.After.Layout };
            if ( raw.Kind == BarrierKind::OwnershipAcquire )
                barrier.Before = AccessState{ PipelineStage_None, MemoryAccess_None, raw.Before.Layout };
            return barrier;
        }

        constexpr AccessState kRdgUntouchedState{ PipelineStage_None, MemoryAccess_None, ImageLayout::Undefined };

        // The stages a compute-only queue family executes (Vulkan's table of supported pipeline stages).
        // Graphics runs every stage. Acceleration-structure builds are left out: they need an extension.
        constexpr PipelineStageFlags kRdgComputeQueueStages =
             PipelineStage_DrawIndirect | PipelineStage_ComputeShader | PipelineStage_Copy | PipelineStage_Host;
        // What stands in for "everything this queue did before" when a barrier on the compute queue names
        // only another pipe's stages: those accesses are ordered by the semaphore wait, and this scope chains
        // the barrier to that wait (the graph's stage set has no ALL_COMMANDS).
        constexpr PipelineStageFlags kRdgComputeQueueOrderStages =
             PipelineStage_DrawIndirect | PipelineStage_ComputeShader | PipelineStage_Copy;

        // The memory accesses a set of stages can perform: an access whose stages were filtered away goes too.
        MemoryAccessFlags RdgAccessesOfStages( PipelineStageFlags stages )
        {
            MemoryAccessFlags memory = MemoryAccess_None;
            if ( ( stages & PipelineStage_DrawIndirect ) != 0 )
                memory |= MemoryAccess_IndirectCommandRead;
            if ( ( stages & PipelineStage_VertexInput ) != 0 )
                memory |= MemoryAccess_IndexRead | MemoryAccess_VertexAttributeRead;
            if ( ( stages & kAllShaderStages ) != 0 )
                memory |= MemoryAccess_UniformRead | MemoryAccess_ShaderSampledRead |
                          MemoryAccess_ShaderStorageRead | MemoryAccess_ShaderStorageWrite |
                          MemoryAccess_AccelStructRead;
            if ( ( stages & kDepthTestStages ) != 0 )
                memory |= MemoryAccess_DepthStencilRead | MemoryAccess_DepthStencilWrite;
            if ( ( stages & PipelineStage_ColorAttachmentOutput ) != 0 )
                memory |= MemoryAccess_ColorAttachmentRead | MemoryAccess_ColorAttachmentWrite;
            if ( ( stages & PipelineStage_Copy ) != 0 )
                memory |= MemoryAccess_TransferRead | MemoryAccess_TransferWrite;
            if ( ( stages & PipelineStage_Host ) != 0 )
                memory |= MemoryAccess_HostRead;
            if ( ( stages & PipelineStage_AccelStructBuild ) != 0 )
                memory |= MemoryAccess_AccelStructRead | MemoryAccess_AccelStructWrite;
            return memory;
        }

        // UE's per-pipeline stage filtering: a barrier recorded on @p pipe names only the stages that pipe's
        // queue executes. A declared access such as a sampled read carries vertex and fragment stages the
        // compute queue does not have; another pipe's accesses are ordered by the semaphore between the queues.
        AccessState RdgOnPipe( AccessState state, Pipe pipe )
        {
            if ( pipe == Pipe::Graphics )
                return state;
            const PipelineStageFlags declared = state.Stages;
            state.Stages &= kRdgComputeQueueStages;
            state.Memory &= RdgAccessesOfStages( state.Stages );
            if ( state.Stages == PipelineStage_None && declared != PipelineStage_None )
                state.Stages = kRdgComputeQueueOrderStages;
            return state;
        }

        uint64_t RdgAlignOffset( uint64_t value, uint64_t alignment )
        {
            return ( value + alignment - 1 ) / alignment * alignment;
        }

        // The aliasing plan's view of a lifetime: widened over the fork..join window of its async users.
        bool RdgLifetimesIntersect( const ResourceLifetime& a, const ResourceLifetime& b )
        {
            return a.AliasFirstPosition <= b.AliasLastPosition && b.AliasFirstPosition <= a.AliasLastPosition;
        }

        bool RdgBytesIntersect( uint64_t aOffset, uint64_t aSize, uint64_t bOffset, uint64_t bSize )
        {
            return aOffset < bOffset + bSize && bOffset < aOffset + aSize;
        }

        // Merges one resource's per-subresource transitions into rectangles: runs of mips inside a layer,
        // then identical runs on consecutive layers. A bloom chain's "mip i-1 to read, mip i to write"
        // stays two barriers; a cascade map's four layers entering the same state become one.
        void RdgAppendMergedBarriers( std::vector<RdgRawTransition>& raws, const TextureDesc* desc,
                                      ResourceKind kind, std::vector<Barrier>& out,
                                      std::vector<uint32_t>& partners )
        {
            if ( kind == ResourceKind::Buffer )
            {
                for ( const RdgRawTransition& raw : raws )
                {
                    out.push_back( RdgMakeBarrier( raw, kind, SubresourceRange{ 0, 1, 0, 1 } ) );
                    partners.push_back( raw.Partner );
                }
                return;
            }
            // Local subresource index is layer-major, so sorting by it walks layer by layer, mip by mip.
            std::sort( raws.begin(), raws.end(),
                       []( const RdgRawTransition& a, const RdgRawTransition& b ) { return a.Sub < b.Sub; } );
            const size_t first = out.size();
            // The raw each rectangle was made from: rectangles extend by the FULL transition, so the release
            // and the acquire half of one transfer merge into the same rectangles.
            std::vector<size_t> sourceOf;
            for ( size_t i = 0; i < raws.size(); )
            {
                const uint32_t layer   = raws[i].Sub / desc->Mips;
                const uint32_t baseMip = raws[i].Sub % desc->Mips;
                size_t         end     = i + 1;
                while ( end < raws.size() && raws[end].Sub == raws[end - 1].Sub + 1 &&
                        raws[end].Sub / desc->Mips == layer && raws[end].SameTransition( raws[i] ) )
                    ++end;
                const auto mipCount = static_cast<uint32_t>( end - i );

                bool extended = false;
                for ( size_t b = first; b < out.size() && !extended; ++b )
                {
                    Barrier& open = out[b];
                    if ( open.Range.BaseMip == baseMip && open.Range.MipCount == mipCount &&
                         open.Range.BaseLayer + open.Range.LayerCount == layer &&
                         raws[sourceOf[b - first]].SameTransition( raws[i] ) )
                    {
                        ++open.Range.LayerCount;
                        extended = true;
                    }
                }
                if ( !extended )
                {
                    out.push_back(
                         RdgMakeBarrier( raws[i], kind, SubresourceRange{ baseMip, mipCount, layer, 1 } ) );
                    partners.push_back( raws[i].Partner );
                    sourceOf.push_back( i );
                }
                i = end;
            }
        }

        // One transient's footprint in the aliasing plan.
        struct RdgAliasFootprint
        {
            uint64_t    Size;
            uint64_t    Alignment;
            uint32_t    MemoryTypeBits;
            MemoryClass Class;
        };

        struct RdgAliasCandidate
        {
            uint32_t          Lifetime;
            RdgAliasFootprint Footprint;
        };
    } // namespace

    // The compile's working state, one member function per phase - UE's FRDGBuilder::Compile and the
    // functions it calls (RenderGraphBuilder.cpp: FlushCullStack, the fork/join pass, the render-pass merge,
    // CollectPassBarriers, CompilePassOps, AllocateTransientResources, CreatePassBarriers). Builder::Compile
    // runs the phases in order; each reads what the earlier ones left in the members.
    class Builder::Compiler
    {
    public:
        Compiler( const Builder& builder, const IMemoryRequirementsProvider& memory,
                  const PipeCapabilities& pipes );

        [[nodiscard]] Common::BoolResultStr ValidateFaultDefaults() const;
        void                                FaultMalformedPasses();
        void                                FoldPassUses();
        void                                SubstituteLostInputs();
        void                                ApplyExternalFaultPolicies();
        void                                CullPasses();
        void                                CollectDependencyEdges();
        void                                ChoosePipes();
        void                                FindAsyncRuns();
        void                                CreateAsyncSyncs();
        void                                ComputeLifetimes();
        Common::BoolResultStr               PlanAliasing();
        void                                CollectPassBarriers();
        void                                DecideStoreActions();
        void                                MergeRenderPasses();
        void                                CollectFinalBarriers();
        void                                CreatePassBarriers();
        void                                FilterStagesPerPipe();
        void                                BuildPipeSegments();

        CompileResult TakeResult()
        {
            return std::move( m_Result );
        }

    private:
        [[nodiscard]] std::string DescribeSub( uint32_t sub ) const;
        void Fault( uint32_t p, PassFaultStage stage, std::string reason, std::optional<uint32_t> root );
        [[nodiscard]] bool        LoadsAttachment( uint32_t p, uint32_t resource ) const;
        void                  FoldUse( const PassRecord& pass, const ResourceUse& use, std::vector<int32_t>& slot,
                                       std::vector<RdgSubUse>& uses, std::string& error ) const;
        Common::BoolResultStr CollectAliasCandidates( std::vector<RdgAliasCandidate>& candidates ) const;
        void                  PlaceAliasCandidates( std::vector<RdgAliasCandidate>& candidates );
        [[nodiscard]] bool    IsAliased( uint32_t resource ) const;
        [[nodiscard]] AccessState InitialState( uint32_t resource, uint32_t local ) const;
        void                      TrackUse( uint32_t position, const RdgSubUse& use );
        void                      CompilePassOps( uint32_t position );
        void                      EmitBarriers( std::vector<RdgRawTransition>& list, std::vector<Barrier>& out,
                                                std::vector<uint32_t>& partners ) const;
        void AddTransfers( const std::vector<Barrier>& barriers, const std::vector<uint32_t>& partners,
                           uint32_t acquirePosition );

        const std::vector<PassRecord>&     m_Passes;
        const std::vector<ResourceRecord>& m_Resources;
        const std::string&                 m_Name;
        const bool                         m_PassCulling;
        const IMemoryRequirementsProvider& m_Memory;
        const PipeCapabilities&            m_Pipes;
        const FaultDefaults&               m_FaultDefaults;
        const Builder&                     m_Builder; // MakeFrameFaultExternal

        uint32_t m_PassCount     = 0;
        uint32_t m_ResourceCount = 0;
        // Global subresource numbering: resource r owns [m_SubBase[r], m_SubBase[r + 1]).
        std::vector<uint32_t>               m_SubBase;
        uint32_t                            m_SubCount = 0;
        std::vector<std::vector<RdgSubUse>> m_PassUses;

        // RDG-FAULT1. m_RootOf[p]: the pass faulted pass p's removal starts at (p itself for a root fault), -1
        // while p survives. m_ClearedAttachments: (pass, resource) whose Load from a transient that lost its
        // producer becomes the FaultDefault's clear.
        std::vector<int32_t>                       m_RootOf;
        std::vector<std::pair<uint32_t, uint32_t>> m_ClearedAttachments;

        CompileResult         m_Result;
        std::vector<uint32_t> m_Executed;
        uint32_t              m_ExecutedCount = 0;

        std::vector<Pipe>                  m_PipeAt;
        std::vector<uint32_t>              m_PositionOf;
        std::vector<RdgAsyncRun>           m_Runs;
        std::vector<int32_t>               m_RunAt;
        std::vector<std::vector<uint32_t>> m_WaitAt;
        std::vector<std::vector<uint32_t>> m_SignalAt;

        std::vector<int32_t> m_LifetimeOf;

        std::vector<RdgSubTrack>                   m_Track;
        std::vector<std::vector<RdgRawTransition>> m_Raws;
        // Ownership releases after each position; [m_ExecutedCount]: the prologue's (amendment B).
        std::vector<std::vector<RdgRawTransition>> m_EpilogueRaws;
        std::vector<bool>                          m_WrittenSoFar;
        std::vector<RdgRawTransition>              m_FinalRaws;
    };

    Builder::Compiler::Compiler( const Builder& builder, const IMemoryRequirementsProvider& memory,
                                 const PipeCapabilities& pipes )
         : m_Passes( builder.m_Passes ), m_Resources( builder.m_Resources ), m_Name( builder.m_Name ),
           m_PassCulling( builder.m_PassCulling ), m_Memory( memory ), m_Pipes( pipes ),
           m_FaultDefaults( builder.m_FaultDefaults ), m_Builder( builder )
    {
        m_PassCount     = static_cast<uint32_t>( m_Passes.size() );
        m_ResourceCount = static_cast<uint32_t>( m_Resources.size() );
        m_SubBase.assign( m_ResourceCount + 1, 0 );
        for ( uint32_t r = 0; r < m_ResourceCount; ++r )
            m_SubBase[r + 1] = m_SubBase[r] + m_Resources[r].SubresourceCount();
        m_SubCount = m_SubBase[m_ResourceCount];
        m_RootOf.assign( m_PassCount, -1 );
    }

    void Builder::Compiler::Fault( uint32_t p, PassFaultStage stage, std::string reason,
                                   std::optional<uint32_t> root )
    {
        m_RootOf[p] = static_cast<int32_t>( root.value_or( p ) );
        m_Result.Faults.push_back( { p, m_Passes[p].Name, stage, std::move( reason ), root } );
    }

    bool Builder::Compiler::LoadsAttachment( uint32_t p, uint32_t resource ) const
    {
        return std::any_of( m_Passes[p].Attachments.begin(), m_Passes[p].Attachments.end(),
                            [resource]( const AttachmentRecord& attachment ) {
                                return attachment.Resource == resource &&
                                       attachment.Load.Action == LoadAction::Load;
                            } );
    }

    // ── Graph check (RDG-FAULT1): a FaultDefault the graph cannot honour is a malformed GRAPH, not a pass fault ─
    Common::BoolResultStr Builder::Compiler::ValidateFaultDefaults() const
    {
        for ( const ResourceRecord& record : m_Resources )
        {
            if ( record.Default != FaultDefault::None &&
                 m_FaultDefaults.GetSource( record.Default ) == kInvalidResource )
            {
                return Common::MakeFormattedError( "graph '{}': texture '{}' declares a FaultDefault but the "
                                                   "graph registered no system textures "
                                                   "(RegisterSystemTextures)",
                                                   m_Name, record.Name );
            }
        }
        return Common::MakeSuccess( true );
    }

    // ── Fault set (RDG-FAULT1): a pass whose own declaration or binding blocks are malformed is removed as if it
    // had not been added (PassFaultStage::Declaration / Validation) ───────────────────────────────────────────
    void Builder::Compiler::FaultMalformedPasses()
    {
        for ( uint32_t p = 0; p < m_PassCount; ++p )
        {
            const PassRecord& pass = m_Passes[p];
            if ( !pass.DeclarationError.empty() )
            {
                Fault( p, PassFaultStage::Declaration, pass.DeclarationError, std::nullopt );
                continue;
            }
            for ( const DeclaredBindingBlock& block : pass.Blocks )
            {
                const Common::BoolResultStr valid = ValidatePassBindings( block );
                if ( !valid )
                {
                    Fault( p, PassFaultStage::Validation, valid.GetError(), std::nullopt );
                    break;
                }
            }
        }
    }

    std::string Builder::Compiler::DescribeSub( uint32_t sub ) const
    {
        uint32_t r = 0;
        while ( m_SubBase[r + 1] <= sub )
            ++r;
        const ResourceRecord& record = m_Resources[r];
        if ( record.Kind == ResourceKind::Buffer )
            return fmt::format( "buffer '{}'", record.Name );
        const uint32_t local = sub - m_SubBase[r];
        return fmt::format( "texture '{}' mip {} layer {}", record.Name, local % record.Texture.Mips,
                            local / record.Texture.Mips );
    }

    // ── 0. Fold each pass's declarations into one use per subresource ─────────────────────────────────
    void Builder::Compiler::FoldUse( const PassRecord& pass, const ResourceUse& use, std::vector<int32_t>& slot,
                                     std::vector<RdgSubUse>& uses, std::string& error ) const
    {
        const ResourceRecord& record   = m_Resources[use.Resource];
        const AccessState     state    = GetAccessState( use.Usage );
        bool                  consumes = true;
        if ( use.Attachment >= 0 )
        {
            const LoadAction load = pass.Attachments[static_cast<size_t>( use.Attachment )].Load.Action;
            consumes              = load == LoadAction::Load;
        }
        for ( uint32_t layer = use.Range.BaseLayer; layer < use.Range.BaseLayer + use.Range.LayerCount; ++layer )
        {
            for ( uint32_t mip = use.Range.BaseMip; mip < use.Range.BaseMip + use.Range.MipCount; ++mip )
            {
                const uint32_t local =
                     record.Kind == ResourceKind::Texture ? record.Texture.SubresourceIndex( mip, layer ) : 0;
                const uint32_t sub = m_SubBase[use.Resource] + local;
                if ( slot[sub] < 0 )
                {
                    slot[sub] = static_cast<int32_t>( uses.size() );
                    uses.push_back( { sub, use.Resource, state, use.Usage,
                                      1u << static_cast<uint32_t>( use.Usage ), IsWriteAccess( use.Usage ),
                                      consumes } );
                    continue;
                }
                RdgSubUse& existing = uses[static_cast<size_t>( slot[sub] )];
                if ( !existing.State.IsReadOnly() || !state.IsReadOnly() || existing.State.Layout != state.Layout )
                {
                    error = fmt::format( "graph '{}' pass '{}' declares {} as both {} and {}; one pass may hold a "
                                         "subresource in one state only",
                                         m_Name, pass.Name, DescribeSub( sub ),
                                         GetAccessName( existing.FirstAccess ), GetAccessName( use.Usage ) );
                    return;
                }
                existing.State = MergeReadStates( existing.State, state );
                existing.AccessMask |= 1u << static_cast<uint32_t>( use.Usage );
            }
        }
    }

    // A faulted pass folds nothing; one holding a subresource in two states is a Declaration fault (RDG-FAULT1).
    void Builder::Compiler::FoldPassUses()
    {
        m_PassUses.assign( m_PassCount, {} );
        std::vector<int32_t> slot( m_SubCount, -1 );
        for ( uint32_t p = 0; p < m_PassCount; ++p )
        {
            if ( m_RootOf[p] >= 0 )
                continue;
            std::vector<RdgSubUse>& uses = m_PassUses[p];
            std::string             conflict;
            for ( const ResourceUse& use : m_Passes[p].Uses )
            {
                FoldUse( m_Passes[p], use, slot, uses, conflict );
                if ( !conflict.empty() )
                    break;
            }
            for ( const RdgSubUse& use : uses )
                slot[use.Sub] = -1;
            if ( !conflict.empty() )
            {
                uses.clear();
                Fault( p, PassFaultStage::Declaration, std::move( conflict ), std::nullopt );
            }
        }
    }

    // ── 0b. Inputs without a surviving producer (RDG-FAULT1) ─────────────────────────────────────────────
    // In AddPass order, which is the fixed point: removing a pass only changes what LATER passes can read. A
    // transient subresource a pass consumes that no surviving pass wrote before it:
    //   * a removed pass wrote it and the resource has a FaultDefault: a sampled read reads the system texture
    //     instead (DefaultSubstitution); an attachment loaded from it is cleared to the default;
    //   * a removed pass wrote it otherwise: the reader is removed too (Dependency, root = that pass);
    //   * nothing ever wrote it: the reader reads garbage - a Declaration fault of the reader.
    void Builder::Compiler::SubstituteLostInputs()
    {
        constexpr uint32_t kSampledMask = ( 1u << static_cast<uint32_t>( Access::SampledGraphics ) ) |
                                          ( 1u << static_cast<uint32_t>( Access::SampledCompute ) );
        std::vector<bool>    written( m_SubCount, false );
        std::vector<int32_t> removedWriterRoot( m_SubCount, -1 );
        for ( uint32_t p = 0; p < m_PassCount; ++p )
        {
            std::vector<std::pair<uint32_t, FaultDefault>> substitute; // (lost resource, its default)
            std::vector<std::pair<uint32_t, FaultDefault>> clear;
            for ( const RdgSubUse& use : m_PassUses[p] )
            {
                if ( m_RootOf[p] >= 0 )
                    break;
                const ResourceRecord& record = m_Resources[use.Resource];
                if ( record.IsExternal() || !use.Consumes || written[use.Sub] )
                    continue;
                const int32_t root = removedWriterRoot[use.Sub];
                if ( root < 0 )
                {
                    if ( !use.Writes )
                    {
                        Fault( p, PassFaultStage::Declaration,
                               fmt::format( "graph '{}' pass '{}' reads {} as {} before any pass writes it",
                                            m_Name, m_Passes[p].Name, DescribeSub( use.Sub ),
                                            GetAccessName( use.FirstAccess ) ),
                               std::nullopt );
                    }
                    continue; // an attachment loaded before any write has nothing to load (DontCare)
                }
                const bool writesResource =
                     std::any_of( m_PassUses[p].begin(), m_PassUses[p].end(), [&]( const RdgSubUse& other )
                                  { return other.Resource == use.Resource && other.Writes; } );
                if ( record.Default != FaultDefault::None && LoadsAttachment( p, use.Resource ) )
                {
                    clear.emplace_back( use.Resource, record.Default );
                    continue;
                }
                if ( record.Default != FaultDefault::None && !writesResource &&
                     ( use.AccessMask & ~kSampledMask ) == 0 )
                {
                    substitute.emplace_back( use.Resource, record.Default );
                    continue;
                }
                Fault( p, PassFaultStage::Dependency,
                       fmt::format( "reads {} as {}, whose producer '{}' faulted", DescribeSub( use.Sub ),
                                    GetAccessName( use.FirstAccess ),
                                    m_Passes[static_cast<uint32_t>( root )].Name ),
                       static_cast<uint32_t>( root ) );
            }
            if ( m_RootOf[p] >= 0 )
            {
                // Removed: what it would have written is lost to every later reader.
                for ( const ResourceUse& use : m_Passes[p].Uses )
                {
                    if ( !IsWriteAccess( use.Usage ) )
                        continue;
                    for ( uint32_t sub = m_SubBase[use.Resource]; sub < m_SubBase[use.Resource + 1]; ++sub )
                    {
                        if ( !written[sub] )
                            removedWriterRoot[sub] = m_RootOf[p];
                    }
                }
                m_PassUses[p].clear();
                continue;
            }

            std::vector<RdgSubUse>& uses = m_PassUses[p];
            for ( const auto& [resource, value] : clear )
            {
                const std::pair<uint32_t, uint32_t> key{ p, resource };
                if ( std::find( m_ClearedAttachments.begin(), m_ClearedAttachments.end(), key ) !=
                     m_ClearedAttachments.end() )
                    continue;
                for ( RdgSubUse& use : uses )
                {
                    if ( use.Resource == resource )
                        use.Consumes = false;
                }
                m_ClearedAttachments.push_back( key );
                m_Result.Substitutions.push_back( { p, resource, kInvalidResource, value, true } );
            }
            for ( const auto& [resource, value] : substitute )
            {
                const auto lost =
                     std::find_if( uses.begin(), uses.end(), [resource = resource]( const RdgSubUse& use )
                                   { return use.Resource == resource; } );
                if ( lost == uses.end() )
                    continue; // already substituted (several lost subresources of one resource)
                const RdgSubUse read   = *lost;
                const uint32_t  source = m_FaultDefaults.GetSource( value );
                std::erase_if( uses, [resource = resource]( const RdgSubUse& use )
                               { return use.Resource == resource; } );
                for ( uint32_t sub = m_SubBase[source]; sub < m_SubBase[source + 1]; ++sub )
                {
                    const auto existing = std::find_if( uses.begin(), uses.end(),
                                                        [sub]( const RdgSubUse& use ) { return use.Sub == sub; } );
                    if ( existing != uses.end() )
                    {
                        existing->State = MergeReadStates( existing->State, read.State );
                        existing->AccessMask |= read.AccessMask;
                        continue;
                    }
                    uses.push_back( { sub, source, read.State, read.FirstAccess, read.AccessMask, false, true } );
                }
                m_Result.Substitutions.push_back( { p, resource, source, value, false } );
            }
            for ( const RdgSubUse& use : uses )
                written[use.Sub] = written[use.Sub] || use.Writes;
        }
    }

    // ── 0c. Externals that lost every writer (RDG-FAULT1): their owner's ExternalFaultPolicy decides ────────
    void Builder::Compiler::ApplyExternalFaultPolicies()
    {
        std::vector<bool>                  survivingWrite( m_ResourceCount, false );
        std::vector<std::vector<uint32_t>> removedRoots( m_ResourceCount );
        for ( uint32_t p = 0; p < m_PassCount; ++p )
        {
            for ( const ResourceUse& use : m_Passes[p].Uses )
            {
                if ( !IsWriteAccess( use.Usage ) || !m_Resources[use.Resource].IsExternal() )
                    continue;
                if ( m_RootOf[p] < 0 )
                {
                    survivingWrite[use.Resource] = true;
                }
                else
                {
                    removedRoots[use.Resource].push_back( static_cast<uint32_t>( m_RootOf[p] ) );
                }
            }
        }
        std::vector<uint32_t> fatal;
        std::vector<uint32_t> fatalRoots;
        for ( uint32_t r = 0; r < m_ResourceCount; ++r )
        {
            if ( survivingWrite[r] || removedRoots[r].empty() )
                continue;
            if ( m_Resources[r].Policy == ExternalFaultPolicy::InvalidateHistory )
                m_Result.InvalidatedExternals.push_back( r );
            if ( m_Resources[r].Policy != ExternalFaultPolicy::FrameFatal )
                continue;
            fatal.push_back( r );
            fatalRoots.insert( fatalRoots.end(), removedRoots[r].begin(), removedRoots[r].end() );
        }
        if ( fatal.empty() )
            return;
        std::sort( fatalRoots.begin(), fatalRoots.end() );
        fatalRoots.erase( std::unique( fatalRoots.begin(), fatalRoots.end() ), fatalRoots.end() );
        std::string externals;
        std::string roots;
        for ( const uint32_t r : fatal )
            externals += fmt::format( "{}'{}'", externals.empty() ? "" : ", ", m_Resources[r].Name );
        for ( const uint32_t p : fatalRoots )
            roots += fmt::format( "{}'{}'", roots.empty() ? "" : ", ", m_Passes[p].Name );
        m_Result.Frame = FrameFault{
             fmt::format( "graph '{}': {} lost every writer to the fault of {}", m_Name, externals, roots ),
             {},
             fatalRoots };
        for ( const uint32_t r : fatal )
            m_Result.Frame->Externals.push_back( m_Builder.MakeFrameFaultExternal( r ) );
    }

    // ── 1+2. Producer edges and culling (UE FlushCullStack) ───────────────────────────────────────────
    // Roots: NeverCull passes and passes that write an externally visible (external or extracted)
    // subresource; every pass when culling is off. A pass lives if a living pass consumes a subresource
    // it was the last to write before that consumer. The full definition is at Builder::Compile.
    void Builder::Compiler::CullPasses()
    {
        std::vector<bool>                  alive( m_PassCount, false );
        std::vector<std::vector<uint32_t>> producers( m_PassCount );
        std::vector<int32_t>               lastWriter( m_SubCount, -1 );
        std::vector<uint32_t>              stack;
        for ( uint32_t p = 0; p < m_PassCount; ++p )
        {
            if ( m_RootOf[p] >= 0 )
                continue; // removed by a fault: neither a root nor a producer
            bool root = !m_PassCulling || HasFlag( m_Passes[p].Flags, PassFlags::NeverCull );
            for ( const RdgSubUse& use : m_PassUses[p] )
            {
                if ( use.Consumes && lastWriter[use.Sub] >= 0 &&
                     static_cast<uint32_t>( lastWriter[use.Sub] ) != p )
                    producers[p].push_back( static_cast<uint32_t>( lastWriter[use.Sub] ) );
                const ResourceRecord& record = m_Resources[use.Resource];
                root = root || ( use.Writes && ( record.IsExternal() || record.IsExtracted() ) );
            }
            for ( const RdgSubUse& use : m_PassUses[p] )
            {
                if ( use.Writes )
                    lastWriter[use.Sub] = static_cast<int32_t>( p );
            }
            if ( root )
            {
                alive[p] = true;
                stack.push_back( p );
            }
        }
        while ( !stack.empty() )
        {
            const uint32_t p = stack.back();
            stack.pop_back();
            for ( const uint32_t producer : producers[p] )
            {
                if ( !alive[producer] )
                {
                    alive[producer] = true;
                    stack.push_back( producer );
                }
            }
        }

        m_Result.PassCulling = m_PassCulling;
        std::sort( m_Result.Faults.begin(), m_Result.Faults.end(),
                   []( const PassFault& a, const PassFault& b ) { return a.Pass < b.Pass; } );
        for ( uint32_t p = 0; p < m_PassCount; ++p )
        {
            if ( m_RootOf[p] >= 0 )
            {
                m_Result.FaultCulledPasses.push_back( p );
                continue;
            }
            if ( alive[p] )
            {
                m_Executed.push_back( p );
                continue;
            }
            m_Result.CulledPasses.push_back( p );
            m_Result.CulledPassNames.push_back( m_Passes[p].Name );
        }
        m_ExecutedCount = static_cast<uint32_t>( m_Executed.size() );
    }

    // ── 1. RAW / WAR / WAW edges between executed passes ──────────────────────────────────────────────
    void Builder::Compiler::CollectDependencyEdges()
    {
        std::set<std::tuple<uint32_t, uint32_t, uint8_t, uint32_t>> seen;
        std::vector<int32_t>                                        lastWriter( m_SubCount, -1 );
        std::vector<std::vector<uint32_t>>                          readers( m_SubCount );
        auto addEdge = [&]( uint32_t from, uint32_t to, DependencyKind kind, uint32_t resource )
        {
            if ( from != to && seen.insert( { from, to, static_cast<uint8_t>( kind ), resource } ).second )
                m_Result.Edges.push_back( { from, to, kind, resource } );
        };
        for ( const uint32_t p : m_Executed )
        {
            for ( const RdgSubUse& use : m_PassUses[p] )
            {
                if ( !use.Writes )
                {
                    if ( lastWriter[use.Sub] >= 0 )
                        addEdge( static_cast<uint32_t>( lastWriter[use.Sub] ), p, DependencyKind::ReadAfterWrite,
                                 use.Resource );
                    readers[use.Sub].push_back( p );
                    continue;
                }
                for ( const uint32_t reader : readers[use.Sub] )
                    addEdge( reader, p, DependencyKind::WriteAfterRead, use.Resource );
                if ( lastWriter[use.Sub] >= 0 )
                    addEdge( static_cast<uint32_t>( lastWriter[use.Sub] ), p, DependencyKind::WriteAfterWrite,
                             use.Resource );
                readers[use.Sub].clear();
                lastWriter[use.Sub] = static_cast<int32_t>( p );
            }
        }
    }

    // ── S1. Pipe choice. Passes are never reordered: the scheduler only picks a pipe per pass ─────────────
    void Builder::Compiler::ChoosePipes()
    {
        m_Result.Pipes = m_Pipes;
        m_PipeAt.assign( m_ExecutedCount, Pipe::Graphics );
        m_PositionOf.assign( m_PassCount, ~0u );
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            const uint32_t p = m_Executed[position];
            m_PositionOf[p]  = position;
            if ( !HasFlag( m_Passes[p].Flags, PassFlags::AsyncCompute ) )
                continue;
            if ( m_Pipes.SeparateComputeFamily )
                m_PipeAt[position] = Pipe::AsyncCompute;
            else
                m_Result.DemotedAsyncPasses.push_back( p );
        }
    }

    // ── S2. Async runs and their fork / join (UE's fork/join pass in Compile) ─────────────────────────────
    // A run is a maximal range of consecutive async positions. Its fork is the last graphics pass it
    // depends on (any RAW / WAR / WAW edge, or the release of contents it takes over); its join the first
    // graphics pass that depends on it or takes over its contents, or the end of the graph.
    void Builder::Compiler::FindAsyncRuns()
    {
        m_RunAt.assign( m_ExecutedCount, -1 );
        m_WaitAt.assign( m_ExecutedCount, {} );
        m_SignalAt.assign( m_ExecutedCount, {} );
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            if ( m_PipeAt[position] != Pipe::AsyncCompute )
                continue;
            if ( position == 0 || m_PipeAt[position - 1] != Pipe::AsyncCompute )
                m_Runs.push_back( { position, position } );
            m_Runs.back().Last = position;
            m_RunAt[position]  = static_cast<int32_t>( m_Runs.size() - 1 );
        }
        if ( m_Runs.empty() )
            return;
        for ( const DependencyEdge& edge : m_Result.Edges )
        {
            const uint32_t from = m_PositionOf[edge.From];
            const uint32_t to   = m_PositionOf[edge.To];
            if ( m_PipeAt[from] == m_PipeAt[to] )
                continue;
            if ( m_PipeAt[to] == Pipe::AsyncCompute )
            {
                RdgAsyncRun& run = m_Runs[static_cast<size_t>( m_RunAt[to] )];
                run.ForkPosition = std::max( run.ForkPosition, static_cast<int32_t>( from ) );
            }
            else
            {
                RdgAsyncRun& run = m_Runs[static_cast<size_t>( m_RunAt[from] )];
                run.JoinPosition = std::min( run.JoinPosition, to );
            }
        }
        // Contents that change owner - read after read included, which has no edge - release on the old
        // pipe before the sync and acquire on the new one after it. The same walk the barrier tracker
        // makes in CollectPassBarriers: externals start the graph owned by Graphics, transients by no pipe.
        std::vector<int32_t> lastTouch( m_SubCount, -1 );
        std::vector<Pipe>    owner( m_SubCount, Pipe::Graphics );
        std::vector<bool>    owned( m_SubCount, false );
        for ( uint32_t r = 0; r < m_ResourceCount; ++r )
        {
            for ( uint32_t sub = m_SubBase[r]; sub < m_SubBase[r + 1]; ++sub )
                owned[sub] = m_Resources[r].IsExternal();
        }
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            const Pipe pipe = m_PipeAt[position];
            for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
            {
                if ( owned[use.Sub] && owner[use.Sub] != pipe && use.Consumes )
                {
                    if ( pipe == Pipe::AsyncCompute )
                    {
                        RdgAsyncRun& run = m_Runs[static_cast<size_t>( m_RunAt[position] )];
                        if ( lastTouch[use.Sub] < 0 )
                            run.NeedsFork = true;
                        else
                            run.ForkPosition = std::max( run.ForkPosition, lastTouch[use.Sub] );
                    }
                    else
                    {
                        RdgAsyncRun& run =
                             m_Runs[static_cast<size_t>( m_RunAt[static_cast<size_t>( lastTouch[use.Sub] )] )];
                        run.JoinPosition = std::min( run.JoinPosition, position );
                    }
                }
                owned[use.Sub]     = true;
                owner[use.Sub]     = pipe;
                lastTouch[use.Sub] = static_cast<int32_t>( position );
            }
        }
    }

    // ── S2 (cont.). Each run's fork and join become CrossPipeSyncs ───────────────────────────────────────
    void Builder::Compiler::CreateAsyncSyncs()
    {
        for ( RdgAsyncRun& run : m_Runs )
        {
            // An external the run consumes untouched by this graph so far: the previous graph left it
            // owned by Graphics; the graphics pass just before the run releases it and forks.
            // At position 0 there is no such pass: the graphics prologue segment releases it and forks
            // (amendment B).
            if ( run.NeedsFork && run.ForkPosition < 0 )
                run.ForkPosition = run.First == 0 ? kRdgForkAtGraphStart : static_cast<int32_t>( run.First - 1 );

            std::vector<bool>  touched( m_ResourceCount, false );
            PipelineStageFlags asyncStages = PipelineStage_None;
            for ( uint32_t position = run.First; position <= run.Last; ++position )
            {
                for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
                {
                    asyncStages |= use.State.Stages;
                    touched[use.Resource] = true;
                }
            }
            if ( run.ForkPosition >= 0 || run.ForkPosition == kRdgForkAtGraphStart )
            {
                const bool     atStart = run.ForkPosition == kRdgForkAtGraphStart;
                const uint32_t fork =
                     atStart ? CrossPipeSync::kForkAtGraphStart : static_cast<uint32_t>( run.ForkPosition );
                run.ForkSync = static_cast<int32_t>( m_Result.Syncs.size() );
                m_Result.Syncs.push_back(
                     { Pipe::Graphics, fork, Pipe::AsyncCompute, run.First, asyncStages, true } );
                if ( !atStart ) // the prologue segment signals a fork from the graph start
                    m_SignalAt[fork].push_back( static_cast<uint32_t>( run.ForkSync ) );
                m_WaitAt[run.First].push_back( static_cast<uint32_t>( run.ForkSync ) );
            }
            // The semaphore wait blocks only the stages it names for everything after it on the graphics
            // queue: every graphics access to anything the run touched, from the join on. A join at the
            // graph end guards the final barriers, which take over the run's own last accesses.
            PipelineStageFlags joinStages = PipelineStage_None;
            if ( run.JoinPosition == CrossPipeSync::kJoinAtGraphEnd )
                joinStages = asyncStages;
            for ( uint32_t position = run.Last + 1; position < m_ExecutedCount; ++position )
            {
                if ( m_PipeAt[position] != Pipe::Graphics )
                    continue;
                for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
                {
                    if ( touched[use.Resource] )
                        joinStages |= use.State.Stages;
                }
            }
            run.JoinSync = static_cast<int32_t>( m_Result.Syncs.size() );
            m_Result.Syncs.push_back(
                 { Pipe::AsyncCompute, run.Last, Pipe::Graphics, run.JoinPosition, joinStages, false } );
            m_SignalAt[run.Last].push_back( static_cast<uint32_t>( run.JoinSync ) );
            if ( run.JoinPosition != CrossPipeSync::kJoinAtGraphEnd )
                m_WaitAt[run.JoinPosition].push_back( static_cast<uint32_t>( run.JoinSync ) );
        }
    }

    // ── 3. Lifetimes and derived usage of graph-owned resources ───────────────────────────────────────
    void Builder::Compiler::ComputeLifetimes()
    {
        m_LifetimeOf.assign( m_ResourceCount, -1 );
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
            {
                if ( m_Resources[use.Resource].IsExternal() )
                    continue;
                if ( m_LifetimeOf[use.Resource] < 0 )
                {
                    m_LifetimeOf[use.Resource] = static_cast<int32_t>( m_Result.Lifetimes.size() );
                    m_Result.Lifetimes.push_back(
                         { use.Resource, position, position, m_Executed[position], m_Executed[position] } );
                    m_Result.Usages.push_back( { use.Resource, 0 } );
                }
                const auto index                       = static_cast<size_t>( m_LifetimeOf[use.Resource] );
                m_Result.Lifetimes[index].LastPosition = position;
                m_Result.Lifetimes[index].LastPass     = m_Executed[position];
                m_Result.Usages[index].AccessMask |= use.AccessMask;
            }
        }
        // The aliasing plan's range: widened over the fork..join window of every async run that uses it
        // (UE extends async-compute lifetimes over the whole fork/join region).
        for ( ResourceLifetime& lifetime : m_Result.Lifetimes )
        {
            lifetime.AliasFirstPosition = lifetime.FirstPosition;
            lifetime.AliasLastPosition  = lifetime.LastPosition;
        }
        for ( const RdgAsyncRun& run : m_Runs )
        {
            const uint32_t windowFirst = run.ForkPosition < 0 ? 0u : static_cast<uint32_t>( run.ForkPosition );
            const uint32_t windowLast =
                 run.JoinPosition == CrossPipeSync::kJoinAtGraphEnd ? m_ExecutedCount - 1 : run.JoinPosition;
            for ( uint32_t position = run.First; position <= run.Last; ++position )
            {
                for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
                {
                    if ( m_LifetimeOf[use.Resource] < 0 )
                        continue;
                    ResourceLifetime& lifetime =
                         m_Result.Lifetimes[static_cast<size_t>( m_LifetimeOf[use.Resource] )];
                    lifetime.UsedOnAsyncCompute = true;
                    lifetime.AliasFirstPosition = std::min( lifetime.AliasFirstPosition, windowFirst );
                    lifetime.AliasLastPosition  = std::max( lifetime.AliasLastPosition, windowLast );
                }
            }
        }
        for ( DerivedUsage& usage : m_Result.Usages )
        {
            const ResourceRecord& record = m_Resources[usage.Resource];
            if ( record.IsExtracted() )
                usage.AccessMask |= 1u << static_cast<uint32_t>( record.FinalAccess );
        }
    }

    // ── 6. Aliasing plan: interval packing per memory class (UE AllocateTransientResources) ───────────
    // Extracted transients outlive the graph and so never share memory.
    Common::BoolResultStr
    Builder::Compiler::CollectAliasCandidates( std::vector<RdgAliasCandidate>& candidates ) const
    {
        for ( uint32_t l = 0; l < m_Result.Lifetimes.size(); ++l )
        {
            const ResourceRecord& record = m_Resources[m_Result.Lifetimes[l].Resource];
            if ( record.IsExtracted() )
                continue;
            const bool isTexture = record.Kind == ResourceKind::Texture;
            // Lifetimes and Usages are pushed together, so index l names the same resource in both.
            const uint32_t                              usage = m_Result.Usages[l].AccessMask;
            const Common::ResultStr<MemoryRequirements> requirements =
                 isTexture ? m_Memory.GetTextureRequirements( record.Texture, usage )
                           : m_Memory.GetBufferRequirements( record.Buffer, usage );
            if ( !requirements )
                return Common::MakeFormattedError( "graph '{}': no memory requirements for transient '{}': {}",
                                                   m_Name, record.Name, requirements.GetError() );
            const MemoryRequirements& req = requirements.GetValue();
            if ( req.Size == 0 || req.Alignment == 0 || ( req.Alignment & ( req.Alignment - 1 ) ) != 0 ||
                 req.MemoryTypeBits == 0 )
                return Common::MakeFormattedError(
                     "graph '{}': transient '{}' got unusable memory requirements (size {}, alignment {}, "
                     "memory types {:#x})",
                     m_Name, record.Name, req.Size, req.Alignment, req.MemoryTypeBits );
            candidates.push_back( { l,
                                    { RdgAlignOffset( req.Size, req.Alignment ), req.Alignment, req.MemoryTypeBits,
                                      isTexture ? MemoryClass::Texture : MemoryClass::Buffer } } );
        }
        return Common::MakeSuccess( true );
    }

    void Builder::Compiler::PlaceAliasCandidates( std::vector<RdgAliasCandidate>& candidates )
    {
        AliasingPlan& plan = m_Result.Aliasing;
        // First come, first placed; larger first on ties so big targets take the low offsets.
        std::sort( candidates.begin(), candidates.end(),
                   [&]( const RdgAliasCandidate& a, const RdgAliasCandidate& b )
                   {
                       const ResourceLifetime& la = m_Result.Lifetimes[a.Lifetime];
                       const ResourceLifetime& lb = m_Result.Lifetimes[b.Lifetime];
                       if ( la.AliasFirstPosition != lb.AliasFirstPosition )
                           return la.AliasFirstPosition < lb.AliasFirstPosition;
                       if ( a.Footprint.Size != b.Footprint.Size )
                           return a.Footprint.Size > b.Footprint.Size;
                       return la.Resource < lb.Resource;
                   } );

        std::vector<uint32_t> placedLifetime;
        for ( const RdgAliasCandidate& candidate : candidates )
        {
            const ResourceLifetime&  lifetime = m_Result.Lifetimes[candidate.Lifetime];
            const RdgAliasFootprint& fp       = candidate.Footprint;

            // Bytes can be shared only by resources that could live in one memory type: the first heap of
            // the class whose type set (the intersection of its allocations' sets) meets this one's takes
            // it; a disjoint set opens a heap of its own and never aliases the others.
            uint32_t heap = 0;
            while ( heap < plan.Heaps.size() && ( plan.Heaps[heap].Class != fp.Class ||
                                                  ( plan.Heaps[heap].MemoryTypeBits & fp.MemoryTypeBits ) == 0 ) )
                ++heap;
            if ( heap == plan.Heaps.size() )
                plan.Heaps.push_back( { fp.Class, fp.MemoryTypeBits, 0, 1 } );

            std::vector<const Allocation*> conflicts;
            for ( size_t a = 0; a < plan.Allocations.size(); ++a )
            {
                const Allocation& placed = plan.Allocations[a];
                if ( placed.Heap == heap &&
                     RdgLifetimesIntersect( m_Result.Lifetimes[placedLifetime[a]], lifetime ) )
                    conflicts.push_back( &placed );
            }
            uint64_t offset  = 0;
            bool     changed = true;
            while ( changed )
            {
                changed = false;
                for ( const Allocation* conflict : conflicts )
                {
                    if ( RdgBytesIntersect( offset, fp.Size, conflict->Offset, conflict->Size ) )
                    {
                        offset  = RdgAlignOffset( conflict->Offset + conflict->Size, fp.Alignment );
                        changed = true;
                    }
                }
            }

            Allocation allocation;
            allocation.Resource       = lifetime.Resource;
            allocation.Class          = fp.Class;
            allocation.Offset         = offset;
            allocation.Size           = fp.Size;
            allocation.Alignment      = fp.Alignment;
            allocation.MemoryTypeBits = fp.MemoryTypeBits;
            allocation.Heap           = heap;
            for ( size_t a = 0; a < plan.Allocations.size(); ++a )
            {
                const Allocation& placed = plan.Allocations[a];
                if ( placed.Heap == heap &&
                     m_Result.Lifetimes[placedLifetime[a]].AliasLastPosition < lifetime.AliasFirstPosition &&
                     RdgBytesIntersect( offset, fp.Size, placed.Offset, placed.Size ) )
                    allocation.AliasPredecessors.push_back( placed.Resource );
            }
            TransientHeapDesc& heapDesc = plan.Heaps[heap];
            heapDesc.MemoryTypeBits &= fp.MemoryTypeBits;
            heapDesc.Bytes     = std::max( heapDesc.Bytes, offset + fp.Size );
            heapDesc.Alignment = std::max( heapDesc.Alignment, fp.Alignment );
            plan.UnaliasedBytes += fp.Size;
            plan.Allocations.push_back( std::move( allocation ) );
            placedLifetime.push_back( candidate.Lifetime );
        }
        // A class's peak is the memory its heaps hold together.
        for ( const TransientHeapDesc& heapDesc : plan.Heaps )
            plan.PeakBytes[static_cast<uint32_t>( heapDesc.Class )] += heapDesc.Bytes;
        for ( const uint64_t peak : plan.PeakBytes )
            plan.TotalPeakBytes += peak;
    }

    Common::BoolResultStr Builder::Compiler::PlanAliasing()
    {
        std::vector<RdgAliasCandidate> candidates;
        Common::BoolResultStr          collected = CollectAliasCandidates( candidates );
        if ( !collected )
            return collected;
        PlaceAliasCandidates( candidates );
        return Common::MakeSuccess( true );
    }

    // ── 4+5. Barriers (merged read states) and load/store decisions (UE CollectPassBarriers) ──────────
    bool Builder::Compiler::IsAliased( uint32_t resource ) const
    {
        const Allocation* allocation = m_Result.FindAllocation( resource );
        return allocation != nullptr && !allocation->AliasPredecessors.empty();
    }

    // The state a subresource is in before the graph touches it. For transient memory that is
    // "undefined", entered after whatever aliased predecessor last used the same bytes.
    AccessState Builder::Compiler::InitialState( uint32_t resource, uint32_t local ) const
    {
        const ResourceRecord& record = m_Resources[resource];
        if ( record.ExternalTex != nullptr )
            return record.ExternalTex->SubresourceStates[local];
        if ( record.ExternalBuf != nullptr )
            return record.ExternalBuf->State;
        AccessState state = kRdgUntouchedState;
        if ( const Allocation* allocation = m_Result.FindAllocation( resource ) )
        {
            for ( const uint32_t predecessor : allocation->AliasPredecessors )
            {
                for ( uint32_t sub = m_SubBase[predecessor]; sub < m_SubBase[predecessor + 1]; ++sub )
                {
                    if ( m_Track[sub].Touched )
                    {
                        state.Stages |= m_Track[sub].Group.Stages;
                        state.Memory |= m_Track[sub].Group.Memory;
                    }
                }
            }
        }
        return state;
    }

    // One pass's use of one subresource against the walk state: opens, widens or closes a group of
    // accesses and records the transition (or ownership transfer) that enters it.
    void Builder::Compiler::TrackUse( uint32_t position, const RdgSubUse& use )
    {
        RdgSubTrack&          sub       = m_Track[use.Sub];
        const ResourceRecord& record    = m_Resources[use.Resource];
        const bool            transient = !record.IsExternal();
        const Pipe            pipe      = m_PipeAt[position];
        auto addRaw = [&]( uint32_t at, const AccessState& before, const AccessState& after, bool discard )
        {
            m_Raws[at].push_back( { use.Sub, use.Resource, before, after, discard } );
            return static_cast<int32_t>( m_Raws[at].size() - 1 );
        };
        // The subresource moves from pipe @p from to this pass's pipe. Contents the pass consumes are
        // transferred (release after the last use on @p from, acquire here); contents it discards
        // are not, and the transition starts from Undefined.
        auto crossPipes = [&]( const AccessState& before, uint32_t releasePosition, Pipe from )
        {
            sub.ReleaseRaw = -1;
            if ( !use.Consumes )
            {
                sub.GroupRaw = addRaw( position, kRdgUntouchedState, use.State, true );
                return;
            }
            m_Raws[position].push_back( { use.Sub, use.Resource, before, use.State, false,
                                          BarrierKind::OwnershipAcquire, from, pipe, releasePosition } );
            sub.GroupRaw = static_cast<int32_t>( m_Raws[position].size() - 1 );
            m_EpilogueRaws[releasePosition].push_back( { use.Sub, use.Resource, before, use.State, false,
                                                         BarrierKind::OwnershipRelease, from, pipe, position } );
            sub.ReleaseRaw      = static_cast<int32_t>( m_EpilogueRaws[releasePosition].size() - 1 );
            sub.ReleasePosition = releasePosition;
        };

        if ( !sub.Touched )
        {
            sub.Touched       = true;
            sub.GroupBefore   = InitialState( use.Resource, use.Sub - m_SubBase[use.Resource] );
            sub.Group         = use.State;
            sub.GroupPosition = position;
            sub.GroupRaw      = -1;
            sub.ReleaseRaw    = -1;
            if ( record.IsExternal() && pipe != Pipe::Graphics )
            {
                // Externals enter every graph owned by Graphics; the run's fork pass releases it.
                // A fork from the graph start releases it in the prologue (slot m_ExecutedCount).
                const int32_t fork = m_Runs[static_cast<size_t>( m_RunAt[position] )].ForkPosition;
                // The host is not a queue family: no ownership transfer may name the HOST stage. The
                // host's accesses to an external (a readback of an earlier frame) all happen before
                // this graph's submission, which orders them before every command in it, so the
                // transfer's source scope is the device part of the state alone.
                AccessState entered = sub.GroupBefore;
                entered.Stages &= ~static_cast<PipelineStageFlags>( PipelineStage_Host );
                entered.Memory &= ~static_cast<MemoryAccessFlags>( MemoryAccess_HostRead );
                crossPipes( entered,
                            fork == kRdgForkAtGraphStart ? m_ExecutedCount : static_cast<uint32_t>( fork ),
                            Pipe::Graphics );
            }
            else
            {
                // A buffer has no layout to transition: with no earlier user of its bytes there is
                // nothing to wait for either, so its first use needs no barrier at all.
                const bool nothingToWaitFor =
                     record.Kind == ResourceKind::Buffer && sub.GroupBefore == kRdgUntouchedState;
                if ( !nothingToWaitFor && !IsCoveredReadState( sub.GroupBefore, use.State ) )
                {
                    sub.GroupRaw = addRaw( position, sub.GroupBefore, use.State, transient );
                    // Bytes an aliased predecessor used: the acquire, from Undefined, with the union of
                    // the predecessors' last accesses (InitialState) as its source scope.
                    if ( transient && IsAliased( use.Resource ) )
                        m_Raws[position][static_cast<size_t>( sub.GroupRaw )].Kind = BarrierKind::AliasAcquire;
                }
            }
        }
        else if ( sub.GroupPipe == pipe && sub.Group.IsReadOnly() && use.State.IsReadOnly() &&
                  ( record.Kind == ResourceKind::Buffer || sub.Group.Layout == use.State.Layout ) )
        {
            // A buffer has no layout, so any two of its reads merge (an indirect-argument read joins the
            // storage read before it); an image's reads merge only within one layout.
            // Merged read state: the barrier that opened this group of reads is widened to cover
            // this pass's stages too, so no barrier is needed between the readers.
            sub.Group = MergeReadStates( sub.Group, use.State );
            if ( sub.GroupRaw >= 0 )
            {
                m_Raws[sub.GroupPosition][static_cast<size_t>( sub.GroupRaw )].After = sub.Group;
                // An acquire widens with its release half, so both halves stay one transfer.
                if ( sub.ReleaseRaw >= 0 )
                    m_EpilogueRaws[sub.ReleasePosition][static_cast<size_t>( sub.ReleaseRaw )].After = sub.Group;
            }
            else if ( !IsCoveredReadState( sub.GroupBefore, sub.Group ) )
                sub.GroupRaw = addRaw( sub.GroupPosition, sub.GroupBefore, sub.Group, false );
        }
        else
        {
            // Every other change - read to write (WAR), write to read (RAW), write to write (WAW),
            // a read in another layout, or any use on the other pipe - is a barrier.
            sub.GroupBefore   = sub.Group;
            sub.Group         = use.State;
            sub.GroupPosition = position;
            sub.ReleaseRaw    = -1;
            if ( sub.GroupPipe != pipe )
                crossPipes( sub.GroupBefore, sub.LastTouch, sub.GroupPipe );
            else
                sub.GroupRaw = addRaw( position, sub.GroupBefore, use.State, false );
        }
        sub.GroupPipe = pipe;
        sub.LastTouch = position;
    }

    // The pass's compiled record and its load decisions (UE CompilePassOps). Load: a transient attachment
    // nothing has written yet has nothing to load.
    void Builder::Compiler::CompilePassOps( uint32_t position )
    {
        const uint32_t    p    = m_Executed[position];
        const PassRecord& pass = m_Passes[p];
        CompiledPass      compiled;
        compiled.Pass        = p;
        compiled.Name        = pass.Name;
        compiled.Flags       = pass.Flags;
        compiled.OnPipe      = m_PipeAt[position];
        compiled.WaitSyncs   = m_WaitAt[position];
        compiled.SignalSyncs = m_SignalAt[position];
        for ( const AttachmentRecord& attachment : pass.Attachments )
        {
            const ResourceRecord& record     = m_Resources[attachment.Resource];
            bool                  anyWritten = false;
            for ( uint32_t layer = attachment.BaseLayer; layer < attachment.BaseLayer + attachment.LayerCount;
                  ++layer )
                anyWritten =
                     anyWritten || m_WrittenSoFar[m_SubBase[attachment.Resource] +
                                                  record.Texture.SubresourceIndex( attachment.Mip, layer )];
            AttachmentDecision decision;
            decision.Slot              = attachment.Slot;
            decision.IsDepth           = attachment.IsDepth;
            decision.IsResolve         = attachment.IsResolve;
            const auto attachmentIndex = static_cast<int32_t>( &attachment - pass.Attachments.data() );
            for ( const ResourceUse& use : pass.Uses )
            {
                if ( use.Attachment == attachmentIndex )
                    decision.Usage = use.Usage;
            }
            decision.Resource   = attachment.Resource;
            decision.Mip        = attachment.Mip;
            decision.BaseLayer  = attachment.BaseLayer;
            decision.LayerCount = attachment.LayerCount;
            decision.Load       = attachment.Load.Action;
            decision.Clear      = attachment.Load.Value;
            if ( decision.Load == LoadAction::Load &&
                 std::find( m_ClearedAttachments.begin(), m_ClearedAttachments.end(),
                            std::pair{ p, attachment.Resource } ) != m_ClearedAttachments.end() )
            {
                // RDG-FAULT1: loaded from a transient whose producer was removed - the default's clear.
                decision.Load  = LoadAction::Clear;
                decision.Clear = FaultDefaults::GetClear( record.Default );
            }
            if ( decision.Load == LoadAction::Load && !record.IsExternal() && !anyWritten )
                decision.Load = LoadAction::DontCare;
            compiled.Attachments.push_back( decision );
        }
        for ( const RdgSubUse& use : m_PassUses[p] )
            m_WrittenSoFar[use.Sub] = m_WrittenSoFar[use.Sub] || use.Writes;
        m_Result.Passes.push_back( std::move( compiled ) );
    }

    void Builder::Compiler::CollectPassBarriers()
    {
        m_Track.assign( m_SubCount, {} );
        m_Raws.assign( m_ExecutedCount, {} );
        m_EpilogueRaws.assign( m_ExecutedCount + 1, {} );
        m_WrittenSoFar.assign( m_SubCount, false );
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
                TrackUse( position, use );
            CompilePassOps( position );
        }
    }

    // Store: DontCare when nothing after the pass needs the contents and nothing outside the graph
    // will see them. Walked backwards so "needed later" is known when each pass is reached.
    void Builder::Compiler::DecideStoreActions()
    {
        std::vector<bool> neededLater( m_SubCount, false );
        for ( uint32_t position = m_ExecutedCount; position-- > 0; )
        {
            const uint32_t p = m_Executed[position];
            for ( AttachmentDecision& decision : m_Result.Passes[position].Attachments )
            {
                const ResourceRecord& record = m_Resources[decision.Resource];
                bool                  needed = record.IsExternal() || record.IsExtracted();
                for ( uint32_t layer = decision.BaseLayer; layer < decision.BaseLayer + decision.LayerCount;
                      ++layer )
                    needed = needed || neededLater[m_SubBase[decision.Resource] +
                                                   record.Texture.SubresourceIndex( decision.Mip, layer )];
                const AttachmentRecord& declared = m_Passes[p].Attachments[static_cast<size_t>(
                     &decision - m_Result.Passes[position].Attachments.data() )];
                decision.Store =
                     needed && declared.Store == StoreAction::Store ? StoreAction::Store : StoreAction::DontCare;
            }
            for ( const RdgSubUse& use : m_PassUses[p] )
            {
                if ( use.Consumes )
                    neededLater[use.Sub] = true;
                else if ( use.Writes )
                    neededLater[use.Sub] = false;
            }
        }
    }

    // ── Render-pass merging (UE MergeRenderPasses in Compile) ─────────────────────────────────────────────
    // A raster pass whose attachments are exactly those of the executed pass before it (same resources,
    // slots, subresources and attachment usage) and which LOADS every one of them continues the render
    // pass that pass opened: one begin/end for the run. Attachment writes inside one subpass are ordered
    // by rasterisation order, so the attachment-to-same-state transition planned between them is dropped;
    // any OTHER transition (a texture the pass samples, a layout change) cannot be recorded inside a
    // render pass and splits the run, as does a Clear or DontCare load.
    void Builder::Compiler::MergeRenderPasses()
    {
        auto sameAttachment = []( const AttachmentDecision& a, const AttachmentDecision& b )
        {
            return a.IsDepth == b.IsDepth && a.IsResolve == b.IsResolve && a.Slot == b.Slot &&
                   a.Usage == b.Usage && a.Resource == b.Resource && a.Mip == b.Mip &&
                   a.BaseLayer == b.BaseLayer && a.LayerCount == b.LayerCount;
        };
        auto sameAttachments = [&]( const CompiledPass& a, const CompiledPass& b )
        {
            if ( a.Attachments.size() != b.Attachments.size() )
                return false;
            for ( const AttachmentDecision& x : a.Attachments )
            {
                if ( std::none_of( b.Attachments.begin(), b.Attachments.end(),
                                   [&]( const AttachmentDecision& y ) { return sameAttachment( x, y ); } ) )
                    return false;
            }
            return true;
        };
        auto isOwnAttachment = [&]( const CompiledPass& pass, const RdgRawTransition& raw )
        {
            const TextureDesc& desc  = m_Resources[raw.Resource].Texture;
            const uint32_t     local = raw.Sub - m_SubBase[raw.Resource];
            const uint32_t     mip   = local % desc.Mips;
            const uint32_t     layer = local / desc.Mips;
            return std::ranges::any_of( pass.Attachments,
                                        [&]( const AttachmentDecision& attachment )
                                        {
                                            return attachment.Resource == raw.Resource && attachment.Mip == mip &&
                                                   layer >= attachment.BaseLayer &&
                                                   layer < attachment.BaseLayer + attachment.LayerCount;
                                        } );
        };
        uint32_t opener = 0;
        for ( uint32_t position = 1; position < m_ExecutedCount; ++position )
        {
            CompiledPass& previous = m_Result.Passes[position - 1];
            CompiledPass& current  = m_Result.Passes[position];
            bool          merge    = HasFlag( previous.Flags, PassFlags::Raster ) &&
                         HasFlag( current.Flags, PassFlags::Raster ) && !current.Attachments.empty() &&
                         sameAttachments( previous, current );
            // A resolve target is never loaded (the resolve at the end of the run overwrites it), so it
            // does not stop the run.
            for ( const AttachmentDecision& attachment : current.Attachments )
                merge = merge && ( attachment.IsResolve || attachment.Load == LoadAction::Load );
            for ( const RdgRawTransition& raw : m_Raws[position] )
                merge = merge && raw.Before == raw.After && isOwnAttachment( current, raw );
            // Never across a sync or a pipe: a pass that waits or signals neither continues nor keeps open
            // a render pass, and a release after the previous pass needs its render pass ended.
            merge = merge && previous.OnPipe == current.OnPipe && previous.WaitSyncs.empty() &&
                    previous.SignalSyncs.empty() && current.WaitSyncs.empty() && current.SignalSyncs.empty() &&
                    m_EpilogueRaws[position - 1].empty();
            if ( !merge )
            {
                opener = position;
                continue;
            }
            m_Raws[position].clear();
            previous.KeepsRenderPassOpen = true;
            current.ContinuesRenderPass  = true;
            // The render pass the opener began stores what the LAST pass of the run leaves.
            for ( AttachmentDecision& opened : m_Result.Passes[opener].Attachments )
            {
                for ( const AttachmentDecision& attachment : current.Attachments )
                {
                    if ( sameAttachment( opened, attachment ) )
                        opened.Store = attachment.Store;
                }
            }
        }
    }

    // Final transitions and the states handed back to externals / extraction targets (UE FinalizeResources).
    void Builder::Compiler::CollectFinalBarriers()
    {
        for ( uint32_t r = 0; r < m_ResourceCount; ++r )
        {
            const ResourceRecord& record = m_Resources[r];
            if ( !record.IsExternal() && !record.IsExtracted() )
                continue;
            ExternalFinalState final;
            final.Resource = r;
            final.Kind     = record.Kind;
            for ( uint32_t sub = m_SubBase[r]; sub < m_SubBase[r + 1]; ++sub )
            {
                const RdgSubTrack& track = m_Track[sub];
                AccessState        last  = track.Touched ? track.Group : InitialState( r, sub - m_SubBase[r] );
                if ( track.Touched && track.GroupPipe != Pipe::Graphics )
                {
                    // Every external and extraction target leaves the graph owned by Graphics: released after
                    // its last async use, acquired in the final barriers after the run's join.
                    const AccessState target = record.IsExtracted() ? GetAccessState( record.FinalAccess ) : last;
                    const uint32_t    at     = track.LastTouch;
                    // The host is not a queue family: no ownership transfer may name the HOST stage. A host
                    // readback gets its ownership back on Graphics in the state the run left it in, then a
                    // plain graphics barrier hands it to the host (which waits on the graph's completion).
                    const bool        host = ( target.Stages & PipelineStage_Host ) != 0;
                    const AccessState owned =
                         host ? AccessState{ last.Stages, last.Memory, target.Layout } : target;
                    m_FinalRaws.push_back( { sub, r, last, owned, false, BarrierKind::OwnershipAcquire,
                                             track.GroupPipe, Pipe::Graphics, at } );
                    m_EpilogueRaws[at].push_back( { sub, r, last, owned, false, BarrierKind::OwnershipRelease,
                                                    track.GroupPipe, Pipe::Graphics,
                                                    CrossPipeSync::kJoinAtGraphEnd } );
                    if ( host )
                        m_FinalRaws.push_back( { sub, r, owned, target, false } );
                    last = target;
                }
                else if ( record.IsExtracted() )
                {
                    const AccessState target = GetAccessState( record.FinalAccess );
                    if ( !IsCoveredReadState( last, target ) )
                        m_FinalRaws.push_back( { sub, r, last, target, false } );
                    last = target;
                }
                final.SubresourceStates.push_back( last );
            }
            m_Result.ExternalFinalStates.push_back( std::move( final ) );
        }
    }

    // Ranges: one barrier per rectangle of subresources sharing a transition, grouped by resource.
    // @p partners receives, per barrier, the position of the other half of an ownership transfer.
    void Builder::Compiler::EmitBarriers( std::vector<RdgRawTransition>& list, std::vector<Barrier>& out,
                                          std::vector<uint32_t>& partners ) const
    {
        std::stable_sort( list.begin(), list.end(), []( const RdgRawTransition& a, const RdgRawTransition& b )
                          { return a.Resource < b.Resource; } );
        for ( size_t i = 0; i < list.size(); )
        {
            size_t end = i;
            while ( end < list.size() && list[end].Resource == list[i].Resource )
                ++end;
            std::vector<RdgRawTransition> group( list.begin() + static_cast<std::ptrdiff_t>( i ),
                                                 list.begin() + static_cast<std::ptrdiff_t>( end ) );
            const ResourceRecord&         record = m_Resources[list[i].Resource];
            for ( RdgRawTransition& raw : group )
                raw.Sub -= m_SubBase[raw.Resource];
            RdgAppendMergedBarriers( group, &record.Texture, record.Kind, out, partners );
            i = end;
        }
    }

    // One QueueOwnershipTransfer per acquire rectangle; its release has the same rectangle (the two
    // halves carry the same full transition, so they merge alike).
    void Builder::Compiler::AddTransfers( const std::vector<Barrier>&  barriers,
                                          const std::vector<uint32_t>& partners, uint32_t acquirePosition )
    {
        for ( size_t b = 0; b < barriers.size(); ++b )
        {
            const Barrier& barrier = barriers[b];
            if ( barrier.BarrierType != BarrierKind::OwnershipAcquire )
                continue;
            QueueOwnershipTransfer transfer;
            transfer.Resource = barrier.Resource;
            transfer.Kind     = barrier.Kind;
            transfer.Range    = barrier.Range;
            transfer.From     = barrier.SrcPipe;
            transfer.To       = barrier.DstPipe;
            transfer.ReleasePosition =
                 partners[b] == m_ExecutedCount ? CrossPipeSync::kForkAtGraphStart : partners[b];
            transfer.AcquirePosition = acquirePosition;
            const RdgAsyncRun& run   = barrier.SrcPipe == Pipe::Graphics
                                            ? m_Runs[static_cast<size_t>( m_RunAt[acquirePosition] )]
                                            : m_Runs[static_cast<size_t>( m_RunAt[transfer.ReleasePosition] )];
            transfer.Sync =
                 static_cast<uint32_t>( barrier.SrcPipe == Pipe::Graphics ? run.ForkSync : run.JoinSync );
            m_Result.OwnershipTransfers.push_back( transfer );
        }
    }

    // UE CreatePassBarriers: the raw transitions of every position, the prologue and the end of the graph
    // become merged Barrier rectangles and QueueOwnershipTransfers.
    void Builder::Compiler::CreatePassBarriers()
    {
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            std::vector<uint32_t> partners;
            EmitBarriers( m_Raws[position], m_Result.Passes[position].Barriers, partners );
            AddTransfers( m_Result.Passes[position].Barriers, partners, position );
            std::vector<uint32_t> releasePartners;
            EmitBarriers( m_EpilogueRaws[position], m_Result.Passes[position].EpilogueBarriers, releasePartners );
        }
        {
            std::vector<uint32_t> releasePartners;
            EmitBarriers( m_EpilogueRaws[m_ExecutedCount], m_Result.PrologueBarriers, releasePartners );
        }
        std::vector<uint32_t> partners;
        EmitBarriers( m_FinalRaws, m_Result.FinalBarriers, partners );
        AddTransfers( m_Result.FinalBarriers, partners, CrossPipeSync::kJoinAtGraphEnd );
    }

    // ── S2b. Per-pipe stage filtering: a pass's barriers are recorded on its pipe (a release on the
    // source pipe, an acquire on the destination pipe), the prologue and the final barriers on Graphics,
    // and a semaphore wait blocks stages of the waiting pipe.
    void Builder::Compiler::FilterStagesPerPipe()
    {
        for ( CompiledPass& pass : m_Result.Passes )
        {
            for ( std::vector<Barrier>* list : { &pass.Barriers, &pass.EpilogueBarriers } )
            {
                for ( Barrier& barrier : *list )
                {
                    barrier.Before = RdgOnPipe( barrier.Before, pass.OnPipe );
                    barrier.After  = RdgOnPipe( barrier.After, pass.OnPipe );
                }
            }
        }
        for ( CrossPipeSync& sync : m_Result.Syncs )
        {
            const AccessState waited{ sync.WaitStages, MemoryAccess_None, ImageLayout::Undefined };
            sync.WaitStages = RdgOnPipe( waited, sync.WaitPipe ).Stages;
        }
    }

    // ── S3. Pipe segments: what the backend records into one command buffer and submits once ──────────────
    // An async segment is one run (fork wait at its first pass, join signal at its last); a graphics
    // segment is split before a pass that waits on a join and after a pass that signals a fork. In
    // position order, a segment comes after every segment it waits on.
    // Amendment B: the graphics prologue (no passes) signals every fork from the graph start, first.
    void Builder::Compiler::BuildPipeSegments()
    {
        PipeSegment prologue{
             Pipe::Graphics, CrossPipeSync::kForkAtGraphStart, CrossPipeSync::kForkAtGraphStart, {}, {} };
        for ( uint32_t sync = 0; sync < m_Result.Syncs.size(); ++sync )
        {
            if ( m_Result.Syncs[sync].SignalPosition == CrossPipeSync::kForkAtGraphStart )
                prologue.SignalSyncs.push_back( sync );
        }
        if ( !prologue.SignalSyncs.empty() )
            m_Result.Segments.push_back( std::move( prologue ) );
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            const CompiledPass& pass = m_Result.Passes[position];
            const bool          split =
                 position == 0 || pass.OnPipe != m_Result.Passes[position - 1].OnPipe ||
                 ( pass.OnPipe == Pipe::Graphics &&
                   ( !pass.WaitSyncs.empty() || !m_Result.Passes[position - 1].SignalSyncs.empty() ) );
            if ( split )
                m_Result.Segments.push_back( { pass.OnPipe, position, position, {}, {} } );
            PipeSegment& segment = m_Result.Segments.back();
            segment.LastPosition = position;
            segment.WaitSyncs.insert( segment.WaitSyncs.end(), pass.WaitSyncs.begin(), pass.WaitSyncs.end() );
            segment.SignalSyncs.insert( segment.SignalSyncs.end(), pass.SignalSyncs.begin(),
                                        pass.SignalSyncs.end() );
        }
    }

    Common::ResultStr<CompileResult> Builder::Compile( const IMemoryRequirementsProvider& memory ) const
    {
        return Compile( memory, PipeCapabilities{} );
    }

    // The phases in UE FRDGBuilder::Compile's order, adapted to a graph that is never reordered: the fault set
    // (RDG-FAULT1: malformed passes, folded declarations, lost inputs, externals' fault policies), culling,
    // dependency edges, pipe choice and fork/join, lifetimes, transient memory, barriers with load/store ops,
    // render-pass merging, final states, barrier rectangles, per-pipe stages, segments.
    Common::ResultStr<CompileResult> Builder::Compile( const IMemoryRequirementsProvider& memory,
                                                       const PipeCapabilities&            pipes ) const
    {
        if ( !m_DeclarationError.empty() )
            return Common::MakeError<CompileResult>( m_DeclarationError );

        Compiler compiler( *this, memory, pipes );
        if ( Common::BoolResultStr defaults = compiler.ValidateFaultDefaults(); !defaults )
            return Common::MakeError<CompileResult>( defaults.GetError() );
        compiler.FaultMalformedPasses();
        compiler.FoldPassUses();
        compiler.SubstituteLostInputs();
        compiler.ApplyExternalFaultPolicies();
        compiler.CullPasses();
        compiler.CollectDependencyEdges();
        compiler.ChoosePipes();
        compiler.FindAsyncRuns();
        compiler.CreateAsyncSyncs();
        compiler.ComputeLifetimes();
        if ( Common::BoolResultStr planned = compiler.PlanAliasing(); !planned )
            return Common::MakeError<CompileResult>( planned.GetError() );
        compiler.CollectPassBarriers();
        compiler.DecideStoreActions();
        compiler.MergeRenderPasses();
        compiler.CollectFinalBarriers();
        compiler.CreatePassBarriers();
        compiler.FilterStagesPerPipe();
        compiler.BuildPipeSegments();
        return Common::MakeSuccess( compiler.TakeResult() );
    }
} // namespace Desert::Graphic::RDG
