// Builder::Compile - the device-free half of the render graph (plan RDG0 §2.4). Ported from the shape of UE
// 5.8 FRDGBuilder::Compile (Runtime/RenderCore/Private/RenderGraphBuilder.cpp): one Builder::Compiler member
// function per phase - FlushCullStack, the fork/join pass, MergeRenderPasses, CollectPassBarriers,
// CompilePassOps, AllocateTransientResources, CreatePassBarriers, FinalizeResources - adapted to a graph
// whose passes keep their declaration order except where HoistQueuedUploads moves a queued upload to the graph
// head and ScheduleIntoAsyncWindows / SinkIntoAsyncWindows move a graphics pass, along its dependency edges,
// into an async run's overlap window.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Common/Core/Profiler.hpp>

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
            // A HELD read group (FindAsyncRuns): its opening transition is in m_EpilogueRaws[GroupPosition] - on
            // Graphics, after the subresource's last use there - not in the barrier batch of GroupPosition. A read
            // group HELD ON ASYNC: in the epilogue of its last async use before the group (the writer).
            bool GroupRawInEpilogue = false;
            // The current group is HELD ON ASYNC (FindAsyncRuns): AsyncCompute keeps it (GroupPipe, LastTouch: the
            // last async reader) while Graphics reads it too; a graphics reader widens its opening transition.
            bool HeldOnAsync = false;
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
            // A later piece of one run of consecutive async positions, cut where a graphics consumer joins
            // (FindAsyncRuns, UE bAsyncComputeEnd): submitted right after the previous piece on the same queue,
            // it is ordered by that piece's fork and shares it (CreateAsyncSyncs).
            bool ContinuesRun = false;
            // What sets JoinPosition: the async position whose consumer it is and the resource that made it one
            // (m_ConsumerCause); kJoinAtGraphEnd / kInvalidResource for a join at the graph end.
            uint32_t JoinCausePosition = CrossPipeSync::kJoinAtGraphEnd;
            uint32_t JoinCauseResource = kInvalidResource;
            // A piece cut only because the next piece forks later (CutRunsAtConsumers): it joins where the next
            // piece does - the queue runs them in order - instead of at a graphics position of its own.
            bool JoinsWithNext = false;
            // What sets ForkPosition: the async position whose graphics producer it is and the resource
            // (m_ForkCause); kJoinAtGraphEnd / kInvalidResource for a fork no producer sets.
            uint32_t ForkCausePosition = CrossPipeSync::kJoinAtGraphEnd;
            uint32_t ForkCauseResource = kInvalidResource;
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
        void                                HoistQueuedUploads();
        void                                ScheduleIntoAsyncWindows();
        void                                SinkIntoAsyncWindows();
        void                                FindAsyncRuns();
        void                                CutRunsAtConsumers();
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

        // Set by AddTransfers when a transfer's acquire would record before the join ordering it.
        const std::string& SyncError() const
        {
            return m_SyncError;
        }

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
        void TrackUse( uint32_t position, const RdgSubUse& use, bool sharedRead, bool opensHeldGroup,
                       bool opensAsyncHeldGroup );
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
        // Positions [0, m_UploadHead) are the queued uploads HoistQueuedUploads moved to the graph head.
        uint32_t                           m_UploadHead = 0;
        std::vector<RdgAsyncRun>           m_Runs;
        std::vector<int32_t>               m_RunAt;
        // By async position (UE FRDGPass::CrossPipelineConsumer): the earliest graphics position that may not
        // start before the pass at that position completed; kJoinAtGraphEnd: only the graph end needs it.
        std::vector<uint32_t>              m_ConsumerAt;
        // By async position: the resource whose use set m_ConsumerAt there (the join's cause in the log).
        std::vector<uint32_t> m_ConsumerCause;
        // By async position (UE FRDGPass::CrossPipelineProducer): the latest graphics position the pass may not
        // start before (-1: none), whether it consumes an external this graph had not touched, and the resource
        // that set the producer (the fork's cause in the log). CutRunsAtConsumers forks a piece of a run anew
        // where a pass's producer is later than the fork of the passes before it (UE bAsyncComputeBegin).
        std::vector<int32_t>  m_ForkAt;
        std::vector<bool>     m_NeedsForkAt;
        std::vector<uint32_t> m_ForkCause;
        // A transfer whose acquire would record before the join that orders it (AddTransfers): Compile fails.
        std::string                        m_SyncError;
        std::vector<std::vector<uint32_t>> m_WaitAt;
        std::vector<std::vector<uint32_t>> m_SignalAt;
        // SHARED READ GROUPS (FindAsyncRuns). m_GroupOfUse[position][i]: the read group use i of the pass at
        // that position belongs to, -1 for a use that writes. By group: whether it is shared across the pipes,
        // and the position and pipe of its first use (the one whose barrier opened it).
        std::vector<std::vector<int32_t>> m_GroupOfUse;
        std::vector<bool>                 m_GroupShared;
        std::vector<uint32_t>             m_GroupOpener;
        std::vector<Pipe>                 m_GroupOpenerPipe;
        // By group: an AsyncCompute opener with a Graphics reader after it in the group; and HELD - such a shared
        // group over contents Graphics holds, opened on Graphics before the fork (FindAsyncRuns, TrackUse).
        std::vector<bool> m_GroupGraphicsReader;
        std::vector<bool> m_GroupHeld;
        // By group: HELD ON ASYNC - such a shared group over contents AsyncCompute holds (an async pass wrote or
        // read them last), opened on AsyncCompute right after that last use; its graphics readers join that use.
        std::vector<bool> m_GroupHeldOnAsync;
        // By held group: the position of the last use before it (its transition records after it) - on Graphics
        // for a HELD group, on AsyncCompute for one HELD ON ASYNC; -1: an external the graph had not touched (a
        // HELD group's transition records after the run's fork).
        std::vector<int32_t> m_GroupHeldAfter;

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
        const ResourceRecord& record = m_Resources[use.Resource];
        // A texture an earlier graph created and extracted takes only the accesses it was created for (its usage
        // flags): any other faults the pass here, before a command the image cannot take is recorded.
        if ( record.ExternalTex != nullptr && record.ExternalTex->CreatedAccessMask != 0 &&
             ( record.ExternalTex->CreatedAccessMask & ( 1u << static_cast<uint32_t>( use.Usage ) ) ) == 0 )
        {
            error = fmt::format( "graph '{}' pass '{}' declares texture '{}' as {}, an access the graph that "
                                 "created it did not extract it for (Builder::Extract @p later)",
                                 m_Name, pass.Name, record.Name, GetAccessName( use.Usage ) );
            return;
        }
        AccessState state    = GetAccessState( use.Usage );
        bool        consumes = true;
        if ( use.Attachment >= 0 )
        {
            const AttachmentRecord& attachment = pass.Attachments[static_cast<size_t>( use.Attachment )];
            consumes                           = attachment.Load.Action == LoadAction::Load;
            // The render pass's store op writes the attachment, a read-only depth one too (AttachmentAccessState):
            // the barrier after this pass must make that write available.
            state = AttachmentAccessState( use.Usage, attachment.IsDepth );
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
                RdgSubUse&     existing = uses[static_cast<size_t>( slot[sub] )];
                const uint32_t mask     = existing.AccessMask | ( 1u << static_cast<uint32_t>( use.Usage ) );
                if ( IsDepthReadSampledPair( mask ) )
                {
                    existing.State        = MergeReadStates( existing.State, state );
                    existing.State.Layout = ImageLayout::DepthStencilReadOnly;
                    existing.AccessMask   = mask;
                    continue;
                }
                // The rule is the subresource's state, not the entry count: one shader-resource access stated by
                // several binding blocks of the pass (the particle nodes' one block per emitter over the scene's
                // pool) is one use. An attachment is one slot of the render pass and is never stated twice.
                if ( use.Attachment < 0 && existing.FirstAccess == use.Usage && existing.State == state )
                    continue;
                // Two READS are one read state, except an image's in two layouts. A buffer has no layout (UE: one
                // ERHIAccess such as SRVGraphics | IndirectArgs), so a storage read and an indirect-argument read
                // of one buffer in one pass are one use, as the barrier phase already merges them
                // (CollectPassBarriers); the Layout an AccessState carries for a buffer access is not one.
                const bool layoutsDiffer =
                     record.Kind == ResourceKind::Texture && existing.State.Layout != state.Layout;
                if ( !existing.State.IsReadOnly() || !state.IsReadOnly() || layoutsDiffer )
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
        // CPU-FRAME-5: every edge found while walking pass p ends AT p, and p is walked once, so an edge can only
        // repeat inside p's own run at the back of Edges (one attempt per subresource of a resource): that run is
        // searched instead of a set of every edge of the graph - the same edges in the same order. The readers
        // since each subresource's last write are one list per subresource in a shared pool, in reader order.
        std::vector<int32_t>                      lastWriter( m_SubCount, -1 );
        std::vector<int32_t>                      readersHead( m_SubCount, -1 );
        std::vector<int32_t>                      readersTail( m_SubCount, -1 );
        std::vector<std::pair<uint32_t, int32_t>> readerPool; // (reader pass, next node or -1)
        size_t                                    passEdges = 0; // the first edge that ends at the walked pass
        auto addEdge = [&]( uint32_t from, uint32_t to, DependencyKind kind, uint32_t resource )
        {
            if ( from == to )
                return;
            for ( size_t e = m_Result.Edges.size(); e-- > passEdges; )
            {
                const DependencyEdge& edge = m_Result.Edges[e];
                if ( edge.From == from && edge.Kind == kind && edge.Resource == resource )
                    return;
            }
            m_Result.Edges.push_back( { from, to, kind, resource } );
        };
        for ( const uint32_t p : m_Executed )
        {
            passEdges = m_Result.Edges.size();
            for ( const RdgSubUse& use : m_PassUses[p] )
            {
                if ( !use.Writes )
                {
                    if ( lastWriter[use.Sub] >= 0 )
                        addEdge( static_cast<uint32_t>( lastWriter[use.Sub] ), p, DependencyKind::ReadAfterWrite,
                                 use.Resource );
                    const auto node = static_cast<int32_t>( readerPool.size() );
                    readerPool.emplace_back( p, -1 );
                    if ( readersTail[use.Sub] >= 0 )
                        readerPool[static_cast<size_t>( readersTail[use.Sub] )].second = node;
                    else
                        readersHead[use.Sub] = node;
                    readersTail[use.Sub] = node;
                    continue;
                }
                for ( int32_t node = readersHead[use.Sub]; node >= 0;
                      node         = readerPool[static_cast<size_t>( node )].second )
                    addEdge( readerPool[static_cast<size_t>( node )].first, p, DependencyKind::WriteAfterRead,
                             use.Resource );
                if ( lastWriter[use.Sub] >= 0 )
                    addEdge( static_cast<uint32_t>( lastWriter[use.Sub] ), p, DependencyKind::WriteAfterWrite,
                             use.Resource );
                readersHead[use.Sub] = -1;
                readersTail[use.Sub] = -1;
                lastWriter[use.Sub]  = static_cast<int32_t>( p );
            }
        }
    }

    // ── S1. Pipe choice. A pipe per pass (reordering: HoistQueuedUploads, ScheduleIntoAsyncWindows) ──────
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

    // ── S1 (cont.). Queued uploads at the graph head (RDG-ASYNC-2; UE FRDGBuilder::SubmitBufferUploads) ──────
    // QueueBufferUpload adds its copy where the renderer calls it, right before the reader. An async run that
    // reads an upload then forks after it, mid-frame, and a fork is a graphics submission that signals only once
    // ALL earlier graphics work completed: the queue drains there (FPS-FIELD-BARN 1_static: "Upload:
    // Lights.ShadowViews" drained the movable shadow cascades, 0.91 ms; the screen-probe CompactAllocator uploads
    // 0.28 + 0.31 ms). UE uploads every queued buffer before the first pass. Here: an upload with no incoming
    // dependency edge (no earlier pass touched its buffer, so nothing it writes can be seen earlier) moves, in its
    // order, to the head. Every edge keeps its direction - an upload has no predecessor, and its readers stay
    // after it - so the graph's result is the same. An upload's exec reads only its own copied bytes, so it
    // crosses a NeverParallel or ExternalQueue pass freely. An upload with an earlier user of its buffer stays in
    // place.
    void Builder::Compiler::HoistQueuedUploads()
    {
        std::vector<bool> hasPredecessor( m_ExecutedCount, false );
        for ( const DependencyEdge& edge : m_Result.Edges )
            hasPredecessor[m_PositionOf[edge.To]] = true;
        std::vector<bool> hoisted( m_ExecutedCount, false );
        uint32_t          count = 0;
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            hoisted[position] = m_Passes[m_Executed[position]].QueuedUpload && !hasPredecessor[position] &&
                                m_PipeAt[position] == Pipe::Graphics;
            if ( hoisted[position] )
                ++count;
        }
        m_UploadHead = count;
        if ( count == 0 )
            return;
        std::vector<uint32_t> order; // new position -> old position
        order.reserve( m_ExecutedCount );
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            if ( hoisted[position] )
                order.push_back( position );
        }
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            if ( !hoisted[position] )
                order.push_back( position );
        }
        std::vector<uint32_t> executed( m_ExecutedCount );
        std::vector<Pipe>     pipes( m_ExecutedCount );
        for ( uint32_t at = 0; at < m_ExecutedCount; ++at )
        {
            executed[at]               = m_Executed[order[at]];
            pipes[at]                  = m_PipeAt[order[at]];
            m_PositionOf[executed[at]] = at;
        }
        std::copy( executed.begin(), executed.end(), m_Executed.begin() );
        m_PipeAt = std::move( pipes );
    }

    // ── S2 (pre). Graphics work into the async windows (RDG-ASYNC-1) ─────────────────────────────────────
    // The graphics queue waits at a run's join when the graphics passes between the run's fork and its first
    // consumer take less time than the run (FPS-FIELD-BARN: 1.5-1.9 ms of graphics against a 2.41 ms chain).
    // A graphics pass after that consumer and before the next run that depends on no async pass, and on no
    // pass from the insertion point on that is not moved itself, is moved - in its order - to just before the
    // consumer: every dependency edge keeps its direction, so the graph's result is the same and the queue
    // fills the window. UE leaves this to the renderer's declaration order; the graph has the edges, so the
    // compile decides it, with no hand-placed order in the renderer.
    // Order no edge records is kept: a NeverParallel pass (its exec writes renderer state later passes read
    // while recording) and an ExternalQueue pass are never moved, nothing moves across them, and a consumer
    // that is one of them takes nothing in front of it. The insertion point steps back over a raster chain the
    // consumer continues (a raster predecessor right before a raster consumer), so a render-pass merge stays.
    void Builder::Compiler::ScheduleIntoAsyncWindows()
    {
        if ( std::find( m_PipeAt.begin(), m_PipeAt.end(), Pipe::AsyncCompute ) == m_PipeAt.end() )
            return;
        // Direct predecessors of each position, from the dependency edges (CSR).
        std::vector<uint32_t> predBegin( m_ExecutedCount + 1, 0 );
        for ( const DependencyEdge& edge : m_Result.Edges )
            ++predBegin[m_PositionOf[edge.To] + 1];
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
            predBegin[position + 1] += predBegin[position];
        std::vector<uint32_t> preds( m_Result.Edges.size() );
        {
            std::vector<uint32_t> fill( predBegin.begin(), predBegin.end() - 1 );
            for ( const DependencyEdge& edge : m_Result.Edges )
                preds[fill[m_PositionOf[edge.To]]++] = m_PositionOf[edge.From];
        }
        const auto keepsOrder = [&]( uint32_t position )
        {
            const PassRecord& pass = m_Passes[m_Executed[position]];
            return HasFlag( pass.Flags, PassFlags::NeverParallel ) || pass.External.has_value();
        };
        const auto isRaster = [&]( uint32_t position )
        { return HasFlag( m_Passes[m_Executed[position]].Flags, PassFlags::Raster ); };
        const auto dependsOn = [&]( uint32_t position, uint32_t on )
        {
            return std::find( preds.begin() + predBegin[position], preds.begin() + predBegin[position + 1], on ) !=
                   preds.begin() + predBegin[position + 1];
        };

        std::vector<bool>     moved( m_ExecutedCount, false );
        std::vector<uint32_t> order; // new position -> old position
        uint32_t              emitted  = 0;
        uint32_t              position = 0;
        while ( position < m_ExecutedCount )
        {
            if ( m_PipeAt[position] != Pipe::AsyncCompute )
            {
                ++position;
                continue;
            }
            const uint32_t first = position;
            while ( position < m_ExecutedCount && m_PipeAt[position] == Pipe::AsyncCompute )
                ++position;
            const uint32_t last      = position - 1;
            uint32_t       nextFirst = position;
            while ( nextFirst < m_ExecutedCount && m_PipeAt[nextFirst] != Pipe::AsyncCompute )
                ++nextFirst;
            // The run's first consumer before the next run: the first graphics pass with an edge from it.
            uint32_t join = nextFirst;
            for ( uint32_t q = last + 1; q < nextFirst && join == nextFirst; ++q )
            {
                for ( uint32_t k = predBegin[q]; k < predBegin[q + 1]; ++k )
                {
                    if ( preds[k] >= first && preds[k] <= last )
                        join = q;
                }
            }
            if ( join == nextFirst || keepsOrder( join ) )
                continue;
            uint32_t insertAt = join;
            while ( insertAt - 1 > last && !keepsOrder( insertAt - 1 ) && isRaster( insertAt ) &&
                    isRaster( insertAt - 1 ) && dependsOn( insertAt, insertAt - 1 ) )
                --insertAt;
            bool any = false;
            for ( uint32_t q = join + 1; q < nextFirst; ++q )
            {
                if ( keepsOrder( q ) )
                    break;
                bool free = true;
                for ( uint32_t k = predBegin[q]; k < predBegin[q + 1] && free; ++k )
                {
                    const uint32_t before = preds[k];
                    free = m_PipeAt[before] != Pipe::AsyncCompute && ( before < insertAt || moved[before] );
                }
                moved[q] = free;
                any      = any || free;
            }
            if ( !any )
                continue;
            order.reserve( m_ExecutedCount );
            for ( ; emitted < insertAt; ++emitted )
                order.push_back( emitted );
            for ( uint32_t q = insertAt; q < nextFirst; ++q )
            {
                if ( moved[q] )
                    order.push_back( q );
            }
            for ( uint32_t q = insertAt; q < nextFirst; ++q )
            {
                if ( !moved[q] )
                    order.push_back( q );
            }
            emitted = nextFirst;
        }
        if ( order.empty() )
            return;
        for ( ; emitted < m_ExecutedCount; ++emitted )
            order.push_back( emitted );
        std::vector<uint32_t> executed( m_ExecutedCount );
        std::vector<Pipe>     pipes( m_ExecutedCount );
        for ( uint32_t at = 0; at < m_ExecutedCount; ++at )
        {
            executed[at]               = m_Executed[order[at]];
            pipes[at]                  = m_PipeAt[order[at]];
            m_PositionOf[executed[at]] = at;
        }
        std::copy( executed.begin(), executed.end(), m_Executed.begin() );
        m_PipeAt = std::move( pipes );
    }

    // ── S2 (pre, cont.). Independent graphics work sunk into the async windows (GPU-TURN-1) ──────────────
    // The pull above fills a window only with graphics work declared AFTER the run's first consumer. On
    // FPS-FIELD-BARN-TURN that is all there is and the graphics queue still idles 0.46 ms ("RDG Wait: graphics
    // queue on async compute before 'Deferred: Composite'"): the screen-probe piece outlasts the graphics work
    // between the run's fork ('HZB: Mip 0' / 'HZB: Mip 10') and that consumer. The work it lacks is declared
    // BEFORE the fork - the shadow depths, which only the lighting after the join reads. UE renders them after
    // the base pass for exactly this overlap (RenderShadowDepthMaps after the async Lumen dispatch, beside
    // RenderLights; raster depth work beside compute tracing), by a hand-placed call order. Here the graph has
    // the edges, so the compile decides it: a graphics pass before a run's fork that no pass before the run's
    // first consumer depends on - a pass sunk with it aside - is moved, in its order, to just before that
    // consumer (the pull's insertion point). It runs on Graphics while the run runs on AsyncCompute, and the
    // fork, no longer queued behind it, signals earlier. Every dependency edge keeps its direction, so the
    // graph's result is the same. The pull's order rules hold: a NeverParallel or ExternalQueue pass is never
    // moved and nothing moves across one, and a queued upload stays at the head HoistQueuedUploads put it at
    // (the cascade copies "MeshShadowCascade{c}.FromCache" are Copy passes and do sink). A run whose first
    // consumer is beyond the next run shares that run's window; positions before a window's insertion point
    // are settled and never move again.
    void Builder::Compiler::SinkIntoAsyncWindows()
    {
        if ( std::find( m_PipeAt.begin(), m_PipeAt.end(), Pipe::AsyncCompute ) == m_PipeAt.end() )
            return;
        // Direct predecessors and successors of each position (CSR), rebuilt after each move.
        std::vector<uint32_t> predBegin;
        std::vector<uint32_t> preds( m_Result.Edges.size() );
        std::vector<uint32_t> succBegin;
        std::vector<uint32_t> succs( m_Result.Edges.size() );
        const auto buildAdjacency = [&]()
        {
            predBegin.assign( m_ExecutedCount + 1, 0 );
            succBegin.assign( m_ExecutedCount + 1, 0 );
            for ( const DependencyEdge& edge : m_Result.Edges )
            {
                ++predBegin[m_PositionOf[edge.To] + 1];
                ++succBegin[m_PositionOf[edge.From] + 1];
            }
            for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
            {
                predBegin[position + 1] += predBegin[position];
                succBegin[position + 1] += succBegin[position];
            }
            std::vector<uint32_t> predFill( predBegin.begin(), predBegin.end() - 1 );
            std::vector<uint32_t> succFill( succBegin.begin(), succBegin.end() - 1 );
            for ( const DependencyEdge& edge : m_Result.Edges )
            {
                const uint32_t from     = m_PositionOf[edge.From];
                const uint32_t to       = m_PositionOf[edge.To];
                preds[predFill[to]++]   = from;
                succs[succFill[from]++] = to;
            }
        };
        const auto keepsOrder = [&]( uint32_t position )
        {
            const PassRecord& pass = m_Passes[m_Executed[position]];
            return HasFlag( pass.Flags, PassFlags::NeverParallel ) || pass.External.has_value();
        };
        const auto isRaster = [&]( uint32_t position )
        { return HasFlag( m_Passes[m_Executed[position]].Flags, PassFlags::Raster ); };
        const auto dependsOn = [&]( uint32_t position, uint32_t on )
        {
            return std::find( preds.begin() + predBegin[position], preds.begin() + predBegin[position + 1], on ) !=
                   preds.begin() + predBegin[position + 1];
        };

        buildAdjacency();
        std::vector<bool>     sunk( m_ExecutedCount, false );
        std::vector<uint32_t> order;
        uint32_t              settled  = 0;
        uint32_t              position = 0;
        while ( position < m_ExecutedCount )
        {
            if ( m_PipeAt[position] != Pipe::AsyncCompute )
            {
                ++position;
                continue;
            }
            const uint32_t first = position;
            while ( position < m_ExecutedCount && m_PipeAt[position] == Pipe::AsyncCompute )
                ++position;
            const uint32_t last      = position - 1;
            uint32_t       nextFirst = position;
            while ( nextFirst < m_ExecutedCount && m_PipeAt[nextFirst] != Pipe::AsyncCompute )
                ++nextFirst;
            // The run's first consumer before the next run, and its fork: the latest graphics position a pass
            // of the run depends on (every graphics position the run depends on is before `first`).
            uint32_t join = nextFirst;
            for ( uint32_t q = last + 1; q < nextFirst && join == nextFirst; ++q )
            {
                for ( uint32_t k = predBegin[q]; k < predBegin[q + 1]; ++k )
                {
                    if ( preds[k] >= first && preds[k] <= last )
                        join = q;
                }
            }
            uint32_t forkEnd = 0; // one past the fork; 0: the run depends on no graphics pass
            for ( uint32_t a = first; a <= last; ++a )
            {
                for ( uint32_t k = predBegin[a]; k < predBegin[a + 1]; ++k )
                {
                    if ( m_PipeAt[preds[k]] != Pipe::AsyncCompute )
                        forkEnd = std::max( forkEnd, preds[k] + 1 );
                }
            }
            if ( join == nextFirst || keepsOrder( join ) )
                continue;
            uint32_t insertAt = join;
            while ( insertAt - 1 > last && !keepsOrder( insertAt - 1 ) && isRaster( insertAt ) &&
                    isRaster( insertAt - 1 ) && dependsOn( insertAt, insertAt - 1 ) )
                --insertAt;
            // Nothing moves across an order-keeping pass, nor out of an earlier window's settled positions.
            uint32_t scanStart = settled;
            for ( uint32_t q = settled; q < insertAt; ++q )
            {
                if ( keepsOrder( q ) )
                    scanStart = q + 1;
            }
            settled = insertAt;
            if ( forkEnd <= scanStart )
                continue;
            // From the fork back: a pass sinks when each of its successors is at or after the insertion point
            // or sinks itself (positions from the fork on never sink, so a successor there keeps it).
            std::fill( sunk.begin(), sunk.end(), false );
            bool any = false;
            for ( uint32_t q = forkEnd; q-- > scanStart; )
            {
                if ( m_PipeAt[q] != Pipe::Graphics || m_Passes[m_Executed[q]].QueuedUpload )
                    continue;
                bool free = true;
                for ( uint32_t k = succBegin[q]; k < succBegin[q + 1] && free; ++k )
                    free = succs[k] >= insertAt || sunk[succs[k]];
                sunk[q] = free;
                any     = any || free;
            }
            if ( !any )
                continue;
            order.clear();
            order.reserve( m_ExecutedCount );
            for ( uint32_t q = 0; q < scanStart; ++q )
                order.push_back( q );
            for ( uint32_t q = scanStart; q < insertAt; ++q )
            {
                if ( !sunk[q] )
                    order.push_back( q );
            }
            for ( uint32_t q = scanStart; q < insertAt; ++q )
            {
                if ( sunk[q] )
                    order.push_back( q );
            }
            for ( uint32_t q = insertAt; q < m_ExecutedCount; ++q )
                order.push_back( q );
            std::vector<uint32_t> executed( m_ExecutedCount );
            std::vector<Pipe>     pipes( m_ExecutedCount );
            for ( uint32_t at = 0; at < m_ExecutedCount; ++at )
            {
                executed[at]               = m_Executed[order[at]];
                pipes[at]                  = m_PipeAt[order[at]];
                m_PositionOf[executed[at]] = at;
            }
            std::copy( executed.begin(), executed.end(), m_Executed.begin() );
            m_PipeAt = std::move( pipes );
            // The run moved down by the sunk passes before it; positions from insertAt on did not move.
            buildAdjacency();
            position = nextFirst;
        }
    }

    // ── S2. Async runs and their fork / join (UE's fork/join pass in Compile) ─────────────────────────────
    // A run is a maximal range of consecutive async positions. Its fork is the last graphics pass it
    // depends on (any RAW / WAR / WAW edge, or the release of contents it takes over). Each of its passes has
    // a cross-pipeline consumer: the first graphics pass that depends on it or takes over its contents, or the
    // end of the graph. CutRunsAtConsumers then cuts the run where a consumer joins.
    void Builder::Compiler::FindAsyncRuns()
    {
        m_RunAt.assign( m_ExecutedCount, -1 );
        m_ConsumerAt.assign( m_ExecutedCount, CrossPipeSync::kJoinAtGraphEnd );
        m_ConsumerCause.assign( m_ExecutedCount, kInvalidResource );
        m_ForkAt.assign( m_ExecutedCount, -1 );
        m_NeedsForkAt.assign( m_ExecutedCount, false );
        m_ForkCause.assign( m_ExecutedCount, kInvalidResource );
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
        // The graphics position @p by may not start before the async position @p of completed, because of
        // @p resource: the earliest such position is the consumer, and its resource the join's cause.
        const auto consumedBy = [&]( uint32_t of, uint32_t by, uint32_t resource )
        {
            if ( by >= m_ConsumerAt[of] )
                return;
            m_ConsumerAt[of]    = by;
            m_ConsumerCause[of] = resource;
        };
        // The async position @p at may not start before the graphics position @p from completed, because of
        // @p resource: the latest such position is its producer, and its resource the fork's cause.
        const auto producedBy = [&]( uint32_t at, int32_t from, uint32_t resource )
        {
            if ( from <= m_ForkAt[at] )
                return;
            m_ForkAt[at]    = from;
            m_ForkCause[at] = resource;
        };
        for ( const DependencyEdge& edge : m_Result.Edges )
        {
            const uint32_t from = m_PositionOf[edge.From];
            const uint32_t to   = m_PositionOf[edge.To];
            if ( m_PipeAt[from] == m_PipeAt[to] )
                continue;
            if ( m_PipeAt[to] == Pipe::AsyncCompute )
                producedBy( to, static_cast<int32_t>( from ), edge.Resource );
            else
                consumedBy( from, to, edge.Resource );
        }
        // SHARED READ GROUPS (UE: a read-only state merges across pipelines, ERHIPipeline::All). A group is a
        // maximal run of one subresource's uses that all read it in one layout (a buffer: any reads) - the run
        // TrackUse merges into one barrier. A graph that crosses queues has every resource CONCURRENT across the
        // two families (VulkanResourceSharing.hpp; Execute refuses it otherwise), so both queues may read one
        // group at once: what needs ordering is the group's opening barrier before every reader, and every
        // reader before the use that ends the group. A group ended on Graphics, or by the graph end (where every
        // run has joined), is SHARED: a read on the other pipe takes no ownership hand-over, so a graphics reader
        // waits only for the run that opened the group, never for the async readers after it (FPS-FIELD-BARN:
        // "Deferred: Reflections" waited 2.1 ms for the screen-probe run, which only READ the radiance cache and
        // the distance fields it reads too). A group ended on AsyncCompute keeps the hand-over at every crossing:
        // its end would have to order the graphics readers' release before it.
        // A HELD group (UE: the transition into the read state of ERHIPipeline::All is begun on the pipe that
        // last used the resource, before the fork) is a shared group AsyncCompute opens over contents Graphics
        // holds - a graphics pass wrote or read them last, or an external enters the graph there - that a graphics
        // pass reads too. Its opening transition records on Graphics after that last use (TrackUse), the fork
        // orders the async readers after it, and a graphics reader joins nothing: "Deferred: Reflections" joined
        // the whole screen-probe piece to "ScreenProbes: Disocclusion" because that async pass opened the read of
        // the velocity and the G-buffer Graphics had written (FPS-FIELD-BARN-TURN, RDG-ASYNC-JOIN-1).
        // A group HELD ON ASYNC (UE: the same transition begun on the pipe that last used the resource, here
        // AsyncCompute) is a shared group AsyncCompute opens over contents AsyncCompute holds - an async pass
        // wrote or read them last - that a graphics pass reads too. Its opening transition records on AsyncCompute
        // after that last use (TrackUse), and a graphics reader joins that use's piece, never the opener's: the
        // radiance cache 'RadianceCache: Filter' writes is opened by 'ScreenProbes: Trace Near Field' and read by
        // "Deferred: Reflections", which waited for the whole screen-probe piece up to Near Field
        // (RDG-ASYNC-JOIN-2).
        m_GroupOfUse.assign( m_ExecutedCount, {} );
        {
            std::vector<int32_t>     open( m_SubCount, -1 ); // the group of each subresource's last use
            std::vector<AccessState> openState( m_SubCount );
            for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
            {
                const std::vector<RdgSubUse>& uses = m_PassUses[m_Executed[position]];
                m_GroupOfUse[position].assign( uses.size(), -1 );
                for ( size_t i = 0; i < uses.size(); ++i )
                {
                    const RdgSubUse& use    = uses[i];
                    int32_t&         group  = open[use.Sub];
                    const bool       member = group >= 0 && use.State.IsReadOnly() &&
                                        ( m_Resources[use.Resource].Kind == ResourceKind::Buffer ||
                                          openState[use.Sub].Layout == use.State.Layout );
                    if ( !member )
                    {
                        if ( group >= 0 )
                            m_GroupShared[static_cast<size_t>( group )] = m_PipeAt[position] == Pipe::Graphics;
                        group = -1;
                        if ( use.State.IsReadOnly() )
                        {
                            group = static_cast<int32_t>( m_GroupShared.size() );
                            m_GroupShared.push_back( true ); // unless a use on AsyncCompute ends it
                            m_GroupOpener.push_back( position );
                            m_GroupOpenerPipe.push_back( m_PipeAt[position] );
                            m_GroupGraphicsReader.push_back( false );
                            m_GroupHeld.push_back( false );
                            m_GroupHeldOnAsync.push_back( false );
                            m_GroupHeldAfter.push_back( -1 );
                            openState[use.Sub] = use.State;
                        }
                    }
                    else if ( m_PipeAt[position] == Pipe::Graphics &&
                              m_GroupOpenerPipe[static_cast<size_t>( group )] == Pipe::AsyncCompute )
                        m_GroupGraphicsReader[static_cast<size_t>( group )] = true;
                    m_GroupOfUse[position][i] = group;
                }
            }
        }

        // Contents that change owner - read after read included, which has no edge - release on the old
        // pipe before the sync and acquire on the new one after it. The same walk the barrier tracker
        // makes in CollectPassBarriers: externals start the graph owned by Graphics, transients by no pipe.
        // A shared group's reads on the other pipe follow TrackUse's shared branch instead (see above).
        std::vector<int32_t> lastTouch( m_SubCount, -1 );
        std::vector<Pipe>    owner( m_SubCount, Pipe::Graphics );
        std::vector<bool>    owned( m_SubCount, false );
        // The last AsyncCompute reader of the shared group each subresource is in (-1: none): the use that ends
        // the group (on Graphics, by the group's definition) joins its run, and with it every earlier run.
        std::vector<int32_t> sharedAsync( m_SubCount, -1 );
        for ( uint32_t r = 0; r < m_ResourceCount; ++r )
        {
            for ( uint32_t sub = m_SubBase[r]; sub < m_SubBase[r + 1]; ++sub )
                owned[sub] = m_Resources[r].IsExternal();
        }
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            const Pipe                    pipe = m_PipeAt[position];
            const std::vector<RdgSubUse>& uses = m_PassUses[m_Executed[position]];
            for ( size_t i = 0; i < uses.size(); ++i )
            {
                const RdgSubUse& use    = uses[i];
                const int32_t    group  = m_GroupOfUse[position][i];
                const bool       shared = group >= 0 && m_GroupShared[static_cast<size_t>( group )];
                const bool       opens  = group < 0 || m_GroupOpener[static_cast<size_t>( group )] == position;
                if ( opens && sharedAsync[use.Sub] >= 0 )
                {
                    const uint32_t reader = static_cast<uint32_t>( sharedAsync[use.Sub] );
                    consumedBy( reader, position, use.Resource );
                    sharedAsync[use.Sub] = -1;
                }
                if ( shared && pipe == Pipe::AsyncCompute )
                    sharedAsync[use.Sub] = static_cast<int32_t>( position );
                if ( shared && opens && pipe == Pipe::AsyncCompute && use.Consumes && owned[use.Sub] &&
                     m_GroupGraphicsReader[static_cast<size_t>( group )] )
                {
                    // Graphics holds the contents: HELD (the transition records on Graphics before the fork).
                    // AsyncCompute holds them: HELD ON ASYNC (the transition records after the last async use).
                    if ( owner[use.Sub] == Pipe::Graphics )
                        m_GroupHeld[static_cast<size_t>( group )] = true;
                    else
                        m_GroupHeldOnAsync[static_cast<size_t>( group )] = true;
                    m_GroupHeldAfter[static_cast<size_t>( group )] = lastTouch[use.Sub];
                }
                if ( shared && m_GroupHeld[static_cast<size_t>( group )] && pipe == Pipe::AsyncCompute )
                {
                    // A HELD group's async reader (the opener included) reads after the opening transition
                    // Graphics records after the last use before the group: the pass forks there at the latest (an
                    // external this graph has not touched: at the run's fork, CreateAsyncSyncs). Owner and
                    // lastTouch stay on Graphics, so its graphics readers consume nothing of this run; the use
                    // that ends the group joins its last async reader (sharedAsync, above).
                    const int32_t after = m_GroupHeldAfter[static_cast<size_t>( group )];
                    if ( after < 0 )
                        m_NeedsForkAt[position] = true;
                    else
                        producedBy( position, after, use.Resource );
                    continue;
                }
                if ( shared && !opens && m_GroupHeldOnAsync[static_cast<size_t>( group )] &&
                     pipe == Pipe::Graphics )
                {
                    // A graphics reader of a group HELD ON ASYNC (UE: the transition into the read state of
                    // ERHIPipeline::All right after the async writer, the graphics reader joins the writer): it
                    // waits for the piece of the last async use before the group - not for the async pass that
                    // opened the group, nor its readers ("Deferred: Reflections" joined 'ScreenProbes: Trace Near
                    // Field', the first async reader of the radiance cache 'RadianceCache: Filter' wrote,
                    // RDG-ASYNC-JOIN-2). Owner and lastTouch stay on AsyncCompute: the use that ends the group
                    // joins its last async reader (sharedAsync, above; TrackUse records that end's transition on
                    // Graphics, no hand-over, since the graphics readers are not ordered before an async release).
                    consumedBy( static_cast<uint32_t>( m_GroupHeldAfter[static_cast<size_t>( group )] ), position,
                                use.Resource );
                    continue;
                }
                if ( shared && !opens && owned[use.Sub] && owner[use.Sub] != pipe )
                {
                    const uint32_t opener = m_GroupOpener[static_cast<size_t>( group )];
                    if ( pipe == Pipe::AsyncCompute )
                    {
                        // After the opening barrier Graphics recorded; owner and lastTouch stay on Graphics.
                        if ( m_GroupOpenerPipe[static_cast<size_t>( group )] == Pipe::Graphics )
                            producedBy( position, static_cast<int32_t>( opener ), use.Resource );
                        continue;
                    }
                    // Graphics takes the group from the run that opened it (TrackUse releases it there).
                    consumedBy( opener, position, use.Resource );
                    owner[use.Sub]       = pipe;
                    lastTouch[use.Sub]   = static_cast<int32_t>( position );
                    continue;
                }
                if ( owned[use.Sub] && owner[use.Sub] != pipe && use.Consumes )
                {
                    if ( pipe == Pipe::AsyncCompute )
                    {
                        if ( lastTouch[use.Sub] < 0 )
                            m_NeedsForkAt[position] = true;
                        else
                            producedBy( position, lastTouch[use.Sub], use.Resource );
                    }
                    else
                    {
                        const uint32_t released = static_cast<uint32_t>( lastTouch[use.Sub] );
                        consumedBy( released, position, use.Resource );
                    }
                }
                owned[use.Sub]     = true;
                owner[use.Sub]     = pipe;
                lastTouch[use.Sub] = static_cast<int32_t>( position );
            }
        }
        CutRunsAtConsumers();
    }

    // CROSS-PIPELINE CONSUMERS (UE FRDGBuilder::Compile: CrossPipelineConsumer, bAsyncComputeEnd). A run's
    // single join after its last pass made the first graphics consumer wait for the whole run even when it
    // depends on its first passes only (FPS-FIELD-BARN: "Deferred: Reflections" waited 0.3-0.9 ms for the end
    // of the run "far field .. Temporal", whose last passes only the composite reads). A run is cut after every
    // pass whose consumers come before any consumer of the passes after it: the piece ends its submission
    // there and signals its own join, waited by its earliest consumer. One queue runs the pieces in order, so
    // a later piece's join implies every earlier one: a cut whose consumer a later pass's earlier consumer
    // already waits for is not made, and the pieces' joins strictly increase - except a piece cut only for the
    // next one's fork (below), which joins with it.
    void Builder::Compiler::CutRunsAtConsumers()
    {
        // laterJoin[p]: the earliest consumer of the run's passes from p to its end (slot Last + 1: none).
        std::vector<uint32_t> laterJoin( m_ExecutedCount + 1, CrossPipeSync::kJoinAtGraphEnd );
        // LATER PRODUCERS (UE FRDGBuilder::Compile: CrossPipelineProducer, bAsyncComputeBegin / bGraphicsFork). A
        // run's single fork at the latest graphics producer of any of its passes held its first passes back until
        // the producer of its last ones: the radiance cache, whose inputs are the G-buffer, forked after the HZB
        // the screen probes after it in the same run trace against (RDG-ASYNC-JOIN-2). A pass whose producer is
        // later than the fork of the passes before it starts a piece with a fork of its own, waited by that piece
        // on the compute queue; the passes before it fork at their own producers. The first passes of a run with
        // no producer of their own take no fork of their own either: they fork with the first producer after them
        // (an untouched external's release, CreateAsyncSyncs, never earlier than a producer inside the run) - a
        // piece with no fork would start on the compute queue ordered after no graphics work of this frame at all
        // (UE forks every async pass at the prologue pass at the earliest).
        // By the first position of each fork's pieces: the fork, NeedsFork, the async position that set the fork.
        std::vector<int32_t>     forkOf( m_ExecutedCount, -1 );
        std::vector<bool>        needsOf( m_ExecutedCount, false );
        std::vector<int32_t>     forkCauseOf( m_ExecutedCount, -1 );
        std::vector<bool>        forksAt( m_ExecutedCount, false );
        std::vector<RdgAsyncRun> pieces;
        pieces.reserve( m_Runs.size() );
        for ( const RdgAsyncRun& run : m_Runs )
        {
            laterJoin[run.Last + 1] = CrossPipeSync::kJoinAtGraphEnd;
            for ( uint32_t position = run.Last + 1; position-- > run.First; )
                laterJoin[position] = std::min( laterJoin[position + 1], m_ConsumerAt[position] );
            uint32_t   forkFirst = run.First;
            int32_t    fork      = -1;
            bool       needs     = false;
            int32_t    cause     = -1;
            const auto closeFork = [&]()
            {
                forkOf[forkFirst]      = fork;
                needsOf[forkFirst]     = needs;
                forkCauseOf[forkFirst] = cause;
            };
            for ( uint32_t position = run.First; position <= run.Last; ++position )
            {
                if ( position != forkFirst && fork >= 0 && m_ForkAt[position] > fork )
                {
                    closeFork();
                    forkFirst         = position;
                    forksAt[position] = true;
                    fork              = -1;
                    needs             = false;
                    cause             = -1;
                }
                if ( m_ForkAt[position] > fork )
                {
                    fork  = m_ForkAt[position];
                    cause = static_cast<int32_t>( position );
                }
                needs = needs || m_NeedsForkAt[position];
            }
            closeFork();

            // A piece from @p first on: a later piece of the same fork (ContinuesRun), or the first of a fork.
            const auto startPiece = [&]( RdgAsyncRun& piece, uint32_t first, bool continues )
            {
                piece.First             = first;
                piece.JoinPosition      = CrossPipeSync::kJoinAtGraphEnd;
                piece.JoinCausePosition = CrossPipeSync::kJoinAtGraphEnd;
                piece.JoinCauseResource = kInvalidResource;
                piece.JoinsWithNext     = false;
                piece.ContinuesRun      = continues;
                if ( continues )
                    return; // CreateAsyncSyncs gives it the fork of the piece before it
                const int32_t setBy     = forkCauseOf[first];
                piece.ForkPosition      = forkOf[first];
                piece.NeedsFork         = needsOf[first];
                piece.ForkCausePosition = CrossPipeSync::kJoinAtGraphEnd;
                piece.ForkCauseResource = kInvalidResource;
                if ( setBy >= 0 )
                {
                    piece.ForkCausePosition = static_cast<uint32_t>( setBy );
                    piece.ForkCauseResource = m_ForkCause[static_cast<size_t>( setBy )];
                }
            };
            RdgAsyncRun piece = run;
            startPiece( piece, run.First, false );
            for ( uint32_t position = run.First; position <= run.Last; ++position )
            {
                if ( m_ConsumerAt[position] < piece.JoinPosition )
                {
                    piece.JoinPosition      = m_ConsumerAt[position];
                    piece.JoinCausePosition = position;
                    piece.JoinCauseResource = m_ConsumerCause[position];
                }
                const bool nextForks = position != run.Last && forksAt[position + 1];
                if ( position != run.Last && !nextForks && piece.JoinPosition >= laterJoin[position + 1] )
                    continue;
                if ( nextForks && piece.JoinPosition >= laterJoin[position + 1] )
                {
                    // Cut for the next piece's fork alone: no consumer of its own comes before the next piece's,
                    // so it joins where that one does - one graphics boundary for both, the queue completes it
                    // first - and the pieces' joins still never decrease.
                    piece.JoinPosition      = laterJoin[position + 1];
                    piece.JoinCausePosition = CrossPipeSync::kJoinAtGraphEnd;
                    piece.JoinCauseResource = kInvalidResource;
                    piece.JoinsWithNext     = piece.JoinPosition != CrossPipeSync::kJoinAtGraphEnd;
                }
                piece.Last = position;
                pieces.push_back( piece );
                if ( position != run.Last )
                    startPiece( piece, position + 1, !nextForks );
            }
        }
        m_Runs = std::move( pieces );
        for ( size_t r = 0; r < m_Runs.size(); ++r )
        {
            for ( uint32_t position = m_Runs[r].First; position <= m_Runs[r].Last; ++position )
                m_RunAt[position] = static_cast<int32_t>( r );
        }
    }

    // ── S2 (cont.). Each run's fork and join become CrossPipeSyncs ───────────────────────────────────────
    void Builder::Compiler::CreateAsyncSyncs()
    {
        std::vector<bool> touched( m_ResourceCount, false );
        // THE NEWEST FORK (RDG-ASYNC-2; UE: a run forks only where a graphics producer of it is, and the async
        // pipe's later runs follow the earlier ones). The compute queue runs the runs in order, and a semaphore
        // wait's second scope holds every later command on that queue: a run whose graphics producers all come
        // at or before the newest fork's position is already ordered after them by that fork. It takes no fork of
        // its own - another fork would cut the graphics submission and drain the queue there for nothing - and
        // the newest fork's wait stages grow by its stages. kRdgForkAtGraphStart orders before every position.
        int32_t coverSync     = -1;
        int32_t coverPosition = kRdgForkAtGraphStart;
        for ( size_t r = 0; r < m_Runs.size(); ++r )
        {
            RdgAsyncRun& run = m_Runs[r];
            // An external the run consumes untouched by this graph so far: the previous graph left it
            // owned by Graphics; a graphics position before the run releases it and forks: the newest fork's,
            // else the end of the upload head (HoistQueuedUploads), else the pass just before the run.
            // At position 0 there is no such pass: the graphics prologue segment releases it and forks
            // (amendment B).
            if ( !run.ContinuesRun && run.NeedsFork && run.ForkPosition < 0 )
            {
                if ( coverSync >= 0 )
                    run.ForkPosition = coverPosition;
                else if ( m_UploadHead > 0 )
                    run.ForkPosition = static_cast<int32_t>( m_UploadHead - 1 );
                else if ( run.First == 0 )
                    run.ForkPosition = kRdgForkAtGraphStart;
                else
                    run.ForkPosition = static_cast<int32_t>( run.First - 1 );
            }
            // A run whose graphics producers are all queued uploads forks once at the end of the upload head:
            // every such run then shares one fork there, before any graphics work is in flight.
            if ( !run.ContinuesRun && run.ForkPosition >= 0 &&
                 static_cast<uint32_t>( run.ForkPosition ) < m_UploadHead )
                run.ForkPosition = static_cast<int32_t>( m_UploadHead - 1 );
            // A later piece of a run waits on no fork of its own: the first piece's wait orders it (a semaphore
            // wait's second scope includes every later command on its queue, for the stages it names - the
            // stages of the whole run, below). Its transfers from Graphics and its aliasing window use that fork.
            if ( run.ContinuesRun )
            {
                run.ForkPosition = m_Runs[r - 1].ForkPosition;
                run.ForkSync     = m_Runs[r - 1].ForkSync;
            }

            std::fill( touched.begin(), touched.end(), false );
            PipelineStageFlags asyncStages = PipelineStage_None;
            for ( uint32_t position = run.First; position <= run.Last; ++position )
            {
                for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
                {
                    asyncStages |= use.State.Stages;
                    touched[use.Resource] = true;
                }
            }
            if ( !run.ContinuesRun && ( run.ForkPosition >= 0 || run.ForkPosition == kRdgForkAtGraphStart ) )
            {
                // The fork gates every piece of the run: its wait stages are those of all of them.
                PipelineStageFlags forkStages = asyncStages;
                for ( size_t later = r + 1; later < m_Runs.size() && m_Runs[later].ContinuesRun; ++later )
                {
                    for ( uint32_t position = m_Runs[later].First; position <= m_Runs[later].Last; ++position )
                    {
                        for ( const RdgSubUse& use : m_PassUses[m_Executed[position]] )
                            forkStages |= use.State.Stages;
                    }
                }
                if ( coverSync >= 0 && run.ForkPosition <= coverPosition )
                {
                    // Ordered by the newest fork (above): its transfers from Graphics release before that fork
                    // signals, and its aliasing window opens there - the run cannot start earlier.
                    run.ForkPosition = coverPosition;
                    run.ForkSync     = coverSync;
                    m_Result.Syncs[static_cast<size_t>( coverSync )].WaitStages |= forkStages;
                }
                else
                {
                    const bool     atStart = run.ForkPosition == kRdgForkAtGraphStart;
                    const uint32_t fork =
                         atStart ? CrossPipeSync::kForkAtGraphStart : static_cast<uint32_t>( run.ForkPosition );
                    run.ForkSync = static_cast<int32_t>( m_Result.Syncs.size() );
                    m_Result.Syncs.push_back(
                         { Pipe::Graphics, fork, Pipe::AsyncCompute, run.First, forkStages, true } );
                    // What made it fork there: the resource and the async pass of the run whose producer it is.
                    if ( run.ForkCausePosition != CrossPipeSync::kJoinAtGraphEnd &&
                         run.ForkCauseResource < m_ResourceCount )
                    {
                        m_Result.Syncs.back().Cause =
                             fmt::format( "'{}' of '{}'", m_Resources[run.ForkCauseResource].Name,
                                          m_Passes[m_Executed[run.ForkCausePosition]].Name );
                    }
                    else if ( run.NeedsFork )
                        m_Result.Syncs.back().Cause = "an external no earlier pass touched";
                    if ( !atStart ) // the prologue segment signals a fork from the graph start
                        m_SignalAt[fork].push_back( static_cast<uint32_t>( run.ForkSync ) );
                    m_WaitAt[run.First].push_back( static_cast<uint32_t>( run.ForkSync ) );
                    coverSync     = run.ForkSync;
                    coverPosition = run.ForkPosition;
                }
            }
            // The semaphore wait blocks only the stages it names for everything after it on the graphics
            // queue: every graphics access to anything the piece touched, from the join on. A join at the
            // graph end guards the final barriers, which take over the run's own last accesses. Each piece
            // signals its own join at its end; the next piece starts a new submission (BuildPipeSegments).
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
            if ( run.JoinPosition != CrossPipeSync::kJoinAtGraphEnd &&
                 run.JoinCausePosition != CrossPipeSync::kJoinAtGraphEnd )
            {
                const std::string_view pass = m_Passes[m_Executed[run.JoinCausePosition]].Name;
                std::string            cause;
                if ( run.JoinCauseResource < m_ResourceCount )
                    cause = fmt::format( "'{}' of '{}'", m_Resources[run.JoinCauseResource].Name, pass );
                else
                    cause = fmt::format( "'{}'", pass );
                m_Result.Syncs.back().Cause = std::move( cause );
            }
            else if ( run.JoinsWithNext )
                m_Result.Syncs.back().Cause = "the next piece (cut for its later fork)";
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
                usage.AccessMask |= ( 1u << static_cast<uint32_t>( record.FinalAccess ) ) | record.LaterAccessMask;
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
        plan.AllocationOf.assign( m_ResourceCount, -1 );
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

        std::vector<uint32_t>          placedLifetime;
        std::vector<const Allocation*> conflicts; // of the candidate being placed; one buffer for all
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

            conflicts.clear();
            for ( size_t a = 0; a < plan.Allocations.size(); ++a )
            {
                const Allocation& placed = plan.Allocations[a];
                if ( placed.Heap == heap &&
                     RdgLifetimesIntersect( m_Result.Lifetimes[placedLifetime[a]], lifetime ) )
                    conflicts.push_back( &placed );
            }
            // The lowest multiple of the alignment clear of every conflict (CPU-FRAME-5: one sweep in offset
            // order instead of bumping until nothing intersects - each bump there was forced, so that fixpoint is
            // this same least feasible offset). In offset order, a conflict starting past the placement's end
            // means every later one does too.
            std::sort( conflicts.begin(), conflicts.end(),
                       []( const Allocation* x, const Allocation* y ) { return x->Offset < y->Offset; } );
            uint64_t offset = 0;
            for ( const Allocation* conflict : conflicts )
            {
                if ( offset + fp.Size <= conflict->Offset )
                    break;
                if ( RdgBytesIntersect( offset, fp.Size, conflict->Offset, conflict->Size ) )
                    offset = RdgAlignOffset( conflict->Offset + conflict->Size, fp.Alignment );
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
            plan.AllocationOf[lifetime.Resource] = static_cast<int32_t>( plan.Allocations.size() );
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
    void Builder::Compiler::TrackUse( uint32_t position, const RdgSubUse& use, const bool sharedRead,
                                      const bool opensHeldGroup, const bool opensAsyncHeldGroup )
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

        // The barrier list the group's transition is in (a held group's: the epilogue of its position).
        const auto groupRaws = [&]() -> std::vector<RdgRawTransition>&
        { return sub.GroupRawInEpilogue ? m_EpilogueRaws[sub.GroupPosition] : m_Raws[sub.GroupPosition]; };

        if ( opensHeldGroup )
        {
            // A HELD READ GROUP (FindAsyncRuns) opened by this async read: Graphics holds the contents, so the
            // transition into the shared read state records on Graphics after their last use there - or, for an
            // external this graph had not touched, after the run's fork (the prologue for a fork at the graph
            // start) - before the fork signals. The fork orders the async readers after it; Graphics keeps the
            // group (GroupPipe, LastTouch) and widens the transition for its own readers of the group.
            if ( sub.Touched && sub.GroupPipe != Pipe::Graphics && m_SyncError.empty() )
            {
                m_SyncError = fmt::format( "graph '{}': the held read group of '{}' opened at pass '{}' is not "
                                           "held by Graphics in the barrier walk",
                                           m_Name, record.Name, m_Passes[m_Executed[position]].Name );
            }
            uint32_t at = sub.LastTouch;
            if ( !sub.Touched )
            {
                const int32_t fork = m_Runs[static_cast<size_t>( m_RunAt[position] )].ForkPosition;
                at                 = fork == kRdgForkAtGraphStart ? m_ExecutedCount : static_cast<uint32_t>( fork );
                sub.Touched        = true;
                sub.GroupBefore    = InitialState( use.Resource, use.Sub - m_SubBase[use.Resource] );
                sub.GroupPipe      = Pipe::Graphics;
                sub.LastTouch      = at == m_ExecutedCount ? 0u : at; // a graphics reader of the group follows
            }
            else
                sub.GroupBefore = sub.Group;
            sub.Group              = use.State;
            sub.GroupPosition      = at;
            sub.HeldOnAsync        = false;
            sub.GroupRawInEpilogue = true;
            sub.GroupRaw           = -1;
            sub.ReleaseRaw         = -1;
            const bool nothingToWaitFor =
                 record.Kind == ResourceKind::Buffer && sub.GroupBefore == kRdgUntouchedState;
            if ( !nothingToWaitFor && !IsCoveredReadState( sub.GroupBefore, use.State ) )
            {
                m_EpilogueRaws[at].push_back( { use.Sub, use.Resource, sub.GroupBefore, use.State, false } );
                sub.GroupRaw = static_cast<int32_t>( m_EpilogueRaws[at].size() - 1 );
            }
            return;
        }
        if ( !sub.Touched )
        {
            sub.Touched            = true;
            sub.GroupRawInEpilogue = false;
            sub.HeldOnAsync = false;
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
                groupRaws()[static_cast<size_t>( sub.GroupRaw )].After = sub.Group;
                // An acquire widens with its release half, so both halves stay one transfer.
                if ( sub.ReleaseRaw >= 0 )
                    m_EpilogueRaws[sub.ReleasePosition][static_cast<size_t>( sub.ReleaseRaw )].After = sub.Group;
            }
            else if ( !IsCoveredReadState( sub.GroupBefore, sub.Group ) )
            {
                groupRaws().push_back( { use.Sub, use.Resource, sub.GroupBefore, sub.Group, false } );
                sub.GroupRaw = static_cast<int32_t>( groupRaws().size() - 1 );
            }
        }
        else if ( sharedRead && sub.Group.IsReadOnly() && use.State.IsReadOnly() &&
                  ( record.Kind == ResourceKind::Buffer || sub.Group.Layout == use.State.Layout ) )
        {
            // A SHARED READ GROUP read on the other pipe (FindAsyncRuns): the resource is CONCURRENT across the
            // families, so two queues read one layout at once without handing it over.
            // AsyncCompute reads what Graphics holds as it is - ordered after the group's opening barrier by the
            // run's fork, before the group's end by its join. Graphics keeps the group, its barrier, LastTouch.
            if ( pipe == Pipe::AsyncCompute )
                return;
            if ( sub.HeldOnAsync )
            {
                // A group HELD ON ASYNC (FindAsyncRuns): the graphics reader widens the opening transition in the
                // async writer's epilogue (FilterStagesPerPipe keeps its compute stages there; the join's wait
                // stages carry the graphics ones) and waits for the writer's piece. No hand-over: AsyncCompute
                // keeps the group; the use that ends it on Graphics transitions it there after the last async
                // reader's join (below), the graph end through CollectFinalBarriers with the layout kept on
                // AsyncCompute.
                sub.Group = MergeReadStates( sub.Group, use.State );
                if ( sub.GroupRaw >= 0 )
                    groupRaws()[static_cast<size_t>( sub.GroupRaw )].After = sub.Group;
                else if ( !IsCoveredReadState( sub.GroupBefore, sub.Group ) )
                {
                    groupRaws().push_back( { use.Sub, use.Resource, sub.GroupBefore, sub.Group, false } );
                    sub.GroupRaw = static_cast<int32_t>( groupRaws().size() - 1 );
                }
                return;
            }
            // Graphics reads a group AsyncCompute opened: the acquire here, its release at the group's OPENER, not
            // at the last async reader - the join waits for the run whose barrier opened the group, and the async
            // readers after it keep reading while Graphics does (the group's end joins them: FindAsyncRuns).
            const uint32_t opener = sub.GroupPosition;
            sub.GroupBefore       = sub.Group;
            sub.Group             = use.State;
            sub.GroupPosition      = position;
            sub.GroupRawInEpilogue = false;
            sub.ReleaseRaw         = -1;
            crossPipes( sub.GroupBefore, opener, sub.GroupPipe );
        }
        else
        {
            // Every other change - read to write (WAR), write to read (RAW), write to write (WAW),
            // a read in another layout, or any use on the other pipe - is a barrier.
            sub.GroupBefore   = sub.Group;
            sub.Group         = use.State;
            sub.GroupPosition      = position;
            sub.GroupRawInEpilogue = false;
            sub.ReleaseRaw         = -1;
            const bool graphicsReadHeld = sub.HeldOnAsync && pipe == Pipe::Graphics;
            sub.HeldOnAsync             = false;
            if ( opensAsyncHeldGroup && sub.GroupPipe != pipe && m_SyncError.empty() )
            {
                m_SyncError = fmt::format( "graph '{}': the read group of '{}' held on async at pass '{}' is not "
                                           "held by AsyncCompute in the barrier walk",
                                           m_Name, record.Name, m_Passes[m_Executed[position]].Name );
            }
            if ( graphicsReadHeld )
            {
                // A group HELD ON ASYNC that Graphics read too, ended on Graphics: the transition records HERE, on
                // Graphics, from the group's merged reads - as a HELD group's end does - never as a release after
                // the last async reader. A release is the whole transition on its source queue (VulkanRdgBackend
                // RecordBarriers), and nothing orders the graphics readers before an async epilogue: a layout
                // change there would run under "Deferred: Reflections" still sampling the group. The graphics
                // readers are before it on this queue; the async ones are ordered by the join of the last async
                // reader (FindAsyncRuns: sharedAsync). Contents it discards enter from Undefined.
                sub.GroupRaw = addRaw( position, sub.GroupBefore, use.State, !use.Consumes );
            }
            else if ( sub.GroupPipe != pipe )
                crossPipes( sub.GroupBefore, sub.LastTouch, sub.GroupPipe );
            else if ( opensAsyncHeldGroup )
            {
                // HELD ON ASYNC (FindAsyncRuns): the transition into the shared read state records after the last
                // async use before the group (the writer), so a graphics reader joins that use's piece.
                sub.GroupPosition      = sub.LastTouch;
                sub.GroupRawInEpilogue = true;
                sub.HeldOnAsync        = true;
                sub.GroupRaw           = -1;
                if ( !IsCoveredReadState( sub.GroupBefore, use.State ) )
                {
                    m_EpilogueRaws[sub.LastTouch].push_back(
                         { use.Sub, use.Resource, sub.GroupBefore, use.State, false } );
                    sub.GroupRaw = static_cast<int32_t>( m_EpilogueRaws[sub.LastTouch].size() - 1 );
                }
            }
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
        compiled.AreaWidth   = pass.AreaWidth;
        compiled.AreaHeight  = pass.AreaHeight;
        compiled.External    = pass.External;
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
            const std::vector<RdgSubUse>& uses = m_PassUses[m_Executed[position]];
            for ( size_t i = 0; i < uses.size(); ++i )
            {
                // A single-pipe compile finds no runs and groups nothing (FindAsyncRuns returns before them).
                const int32_t group  = position < m_GroupOfUse.size() ? m_GroupOfUse[position][i] : -1;
                const bool    shared = group >= 0 && m_GroupShared[static_cast<size_t>( group )];
                const bool    held   = shared && m_GroupHeld[static_cast<size_t>( group )] &&
                                  m_GroupOpener[static_cast<size_t>( group )] == position;
                const bool heldOnAsync = shared && m_GroupHeldOnAsync[static_cast<size_t>( group )] &&
                                         m_GroupOpener[static_cast<size_t>( group )] == position;
                TrackUse( position, uses[i], shared, held, heldOnAsync );
            }
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
                    AccessState       owned = target;
                    if ( host )
                        owned = AccessState{ last.Stages, last.Memory, target.Layout };
                    // A group HELD ON ASYNC that Graphics read too (TrackUse): the release is the whole transition
                    // on AsyncCompute, where nothing orders the graphics readers before it, so it keeps the
                    // group's layout; the final barriers on Graphics, after them on that queue, change it.
                    if ( track.HeldOnAsync )
                        owned = last;
                    m_FinalRaws.push_back( { sub, r, last, owned, false, BarrierKind::OwnershipAcquire,
                                             track.GroupPipe, Pipe::Graphics, at } );
                    m_EpilogueRaws[at].push_back( { sub, r, last, owned, false, BarrierKind::OwnershipRelease,
                                                    track.GroupPipe, Pipe::Graphics,
                                                    CrossPipeSync::kJoinAtGraphEnd } );
                    if ( host || ( track.HeldOnAsync && owned != target ) )
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
            // A run is cut at its consumers (CutRunsAtConsumers): the piece that releases must have joined by
            // the acquire. FindAsyncRuns' ownership walk mirrors this tracker; if they ever name different
            // release positions, the acquire would record before the semaphore that orders it - refused.
            if ( barrier.SrcPipe != Pipe::Graphics && acquirePosition != CrossPipeSync::kJoinAtGraphEnd &&
                 run.JoinPosition > acquirePosition && m_SyncError.empty() )
            {
                std::string waiter = "the graph end";
                if ( run.JoinPosition != CrossPipeSync::kJoinAtGraphEnd )
                    waiter = "pass '" + std::string( m_Passes[m_Executed[run.JoinPosition]].Name ) + "'";
                m_SyncError =
                     fmt::format( "graph '{}': the ownership acquire at pass '{}' is ordered by the join of "
                                  "the async piece ending at pass '{}', which only {} waits for",
                                  m_Name, m_Passes[m_Executed[acquirePosition]].Name,
                                  m_Passes[m_Executed[run.Last]].Name, waiter );
            }
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
        // RDG-EXTERNAL: Wait points of ExternalQueue passes the next segment of each pipe has to wait.
        std::array<std::vector<ExternalTimelinePoint>, kPipeCount> pendingExternalWaits;
        for ( uint32_t position = 0; position < m_ExecutedCount; ++position )
        {
            const CompiledPass& pass = m_Result.Passes[position];
            // RDG-EXTERNAL: a segment ends with an ExternalQueue pass (it signals the pass's Signal point), and
            // the FIRST segment of EACH pipe begun after it waits the pass's Wait point - an async run forked
            // after the pass may read what the other API wrote, and the fork's binary semaphore is signalled
            // before that work ran. A timeline wait consumes nothing, so both pipes wait the same value.
            const bool afterExternal = position > 0 && m_Result.Passes[position - 1].External.has_value();
            // A signal ends a submission on either pipe: a graphics fork, or the join of an async run's piece
            // (CutRunsAtConsumers) - the next piece of the run is the next AsyncCompute submission.
            const bool split = position == 0 || afterExternal ||
                               pass.OnPipe != m_Result.Passes[position - 1].OnPipe ||
                               !m_Result.Passes[position - 1].SignalSyncs.empty() ||
                               ( pass.OnPipe == Pipe::Graphics && !pass.WaitSyncs.empty() );
            if ( split )
            {
                m_Result.Segments.push_back( { pass.OnPipe, position, position, {}, {} } );
                auto& pending                         = pendingExternalWaits[static_cast<uint32_t>( pass.OnPipe )];
                m_Result.Segments.back().WaitExternal = std::exchange( pending, {} );
            }
            PipeSegment& segment = m_Result.Segments.back();
            if ( pass.External.has_value() )
            {
                segment.SignalExternal = pass.External->Signal;
                for ( auto& pending : pendingExternalWaits )
                    pending.push_back( pass.External->Wait );
            }
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

    // The phases in UE FRDGBuilder::Compile's order, adapted to a graph reordered only by queued uploads to its head
    // and into async windows: the fault set (RDG-FAULT1: malformed passes, folded declarations, lost inputs,
    // externals' fault policies), culling, dependency edges, pipe choice, upload head and fork/join, lifetimes,
    // transient memory, barriers with load/store ops, render-pass merging, final states, barrier rectangles,
    // per-pipe stages, segments.
    Common::ResultStr<CompileResult> Builder::Compile( const IMemoryRequirementsProvider& memory,
                                                       const PipeCapabilities&            pipes ) const
    {
        if ( !m_DeclarationError.empty() )
            return Common::MakeError<CompileResult>( m_DeclarationError );

        // CPU-FRAME-5: one row per stage under "RDG: Compile" (PhaseTimer), so the compile's cost reads per stage.
        DESERT_PROFILE_PHASES( stages );
        DESERT_PROFILE_PHASE( stages, "RDG Compile: setup" );
        Compiler compiler( *this, memory, pipes );
        if ( const Common::BoolResultStr defaults = compiler.ValidateFaultDefaults(); !defaults )
            return Common::MakeError<CompileResult>( defaults.GetError() );
        DESERT_PROFILE_PHASE( stages, "RDG Compile: validate blocks" );
        compiler.FaultMalformedPasses();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: fold uses and lost inputs" );
        compiler.FoldPassUses();
        compiler.SubstituteLostInputs();
        compiler.ApplyExternalFaultPolicies();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: cull" );
        compiler.CullPasses();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: dependency edges" );
        compiler.CollectDependencyEdges();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: pipes and async runs" );
        compiler.ChoosePipes();
        compiler.HoistQueuedUploads();
        compiler.ScheduleIntoAsyncWindows();
        compiler.SinkIntoAsyncWindows();
        compiler.FindAsyncRuns();
        compiler.CreateAsyncSyncs();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: lifetimes" );
        compiler.ComputeLifetimes();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: aliasing" );
        if ( Common::BoolResultStr planned = compiler.PlanAliasing(); !planned )
            return Common::MakeError<CompileResult>( planned.GetError() );
        DESERT_PROFILE_PHASE( stages, "RDG Compile: pass barriers" );
        compiler.CollectPassBarriers();
        compiler.DecideStoreActions();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: render-pass merge" );
        compiler.MergeRenderPasses();
        DESERT_PROFILE_PHASE( stages, "RDG Compile: final and merged barriers" );
        compiler.CollectFinalBarriers();
        compiler.CreatePassBarriers();
        if ( !compiler.SyncError().empty() )
            return Common::MakeError<CompileResult>( compiler.SyncError() );
        DESERT_PROFILE_PHASE( stages, "RDG Compile: pipe segments" );
        compiler.FilterStagesPerPipe();
        compiler.BuildPipeSegments();
        DESERT_PROFILE_PHASE_END( stages );
        return Common::MakeSuccess( compiler.TakeResult() );
    }
} // namespace Desert::Graphic::RDG
