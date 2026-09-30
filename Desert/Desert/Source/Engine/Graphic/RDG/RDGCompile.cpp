// Builder::Compile - the device-free half of the render graph (plan RDG0 §2.4). Follows the shape of UE
// 5.8 FRDGBuilder::Compile (Runtime/RenderCore/Private/RenderGraphBuilder.cpp: FlushCullStack,
// CollectPassBarriers, AllocateTransientResources) as described in the plan; UE was not available on
// this machine, so this is written against the plan's description rather than ported line by line.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

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

            bool SameTransition( const RdgRawTransition& other ) const
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
            int32_t     GroupRaw      = -1; // index of the group's transition in its pass's raw list
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
                const uint32_t mipCount = static_cast<uint32_t>( end - i );

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
    } // namespace

    Common::ResultStr<CompileResult> Builder::Compile( const IMemoryRequirementsProvider& memory ) const
    {
        return Compile( memory, PipeCapabilities{} );
    }

    Common::ResultStr<CompileResult> Builder::Compile( const IMemoryRequirementsProvider& memory,
                                                       const PipeCapabilities&            pipes ) const
    {
        if ( !m_DeclarationError.empty() )
            return Common::MakeError<CompileResult>( m_DeclarationError );

        const uint32_t passCount     = static_cast<uint32_t>( m_Passes.size() );
        const uint32_t resourceCount = static_cast<uint32_t>( m_Resources.size() );

        // Global subresource numbering: resource r owns [subBase[r], subBase[r + 1]).
        std::vector<uint32_t> subBase( resourceCount + 1, 0 );
        for ( uint32_t r = 0; r < resourceCount; ++r )
            subBase[r + 1] = subBase[r] + m_Resources[r].SubresourceCount();
        const uint32_t subCount = subBase[resourceCount];

        auto describeSub = [&]( uint32_t sub )
        {
            uint32_t r = 0;
            while ( subBase[r + 1] <= sub )
                ++r;
            const ResourceRecord& record = m_Resources[r];
            if ( record.Kind == ResourceKind::Buffer )
                return fmt::format( "buffer '{}'", record.Name );
            const uint32_t local = sub - subBase[r];
            return fmt::format( "texture '{}' mip {} layer {}", record.Name, local % record.Texture.Mips,
                                local / record.Texture.Mips );
        };

        // ── 0. Fold each pass's declarations into one use per subresource ─────────────────────────────
        std::vector<std::vector<RdgSubUse>> passUses( passCount );
        {
            std::vector<int32_t> slot( subCount, -1 );
            for ( uint32_t p = 0; p < passCount; ++p )
            {
                const PassRecord&       pass = m_Passes[p];
                std::vector<RdgSubUse>& uses = passUses[p];
                for ( const ResourceUse& use : pass.Uses )
                {
                    const ResourceRecord& record   = m_Resources[use.Resource];
                    const AccessState     state    = GetAccessState( use.Usage );
                    bool                  consumes = true;
                    if ( use.Attachment >= 0 )
                    {
                        const LoadAction load =
                             pass.Attachments[static_cast<size_t>( use.Attachment )].Load.Action;
                        consumes = load == LoadAction::Load;
                    }
                    for ( uint32_t layer = use.Range.BaseLayer; layer < use.Range.BaseLayer + use.Range.LayerCount;
                          ++layer )
                    {
                        for ( uint32_t mip = use.Range.BaseMip; mip < use.Range.BaseMip + use.Range.MipCount;
                              ++mip )
                        {
                            const uint32_t local = record.Kind == ResourceKind::Texture
                                                        ? record.Texture.SubresourceIndex( mip, layer )
                                                        : 0;
                            const uint32_t sub   = subBase[use.Resource] + local;
                            if ( slot[sub] < 0 )
                            {
                                slot[sub] = static_cast<int32_t>( uses.size() );
                                uses.push_back( { sub, use.Resource, state, use.Usage,
                                                  1u << static_cast<uint32_t>( use.Usage ),
                                                  IsWriteAccess( use.Usage ), consumes } );
                                continue;
                            }
                            RdgSubUse& existing = uses[static_cast<size_t>( slot[sub] )];
                            if ( !existing.State.IsReadOnly() || !state.IsReadOnly() ||
                                 existing.State.Layout != state.Layout )
                                return Common::MakeFormattedError<CompileResult>(
                                     "graph '{}' pass '{}' declares {} as both {} and {}; one pass may hold a "
                                     "subresource in one state only",
                                     m_Name, pass.Name, describeSub( sub ), GetAccessName( existing.FirstAccess ),
                                     GetAccessName( use.Usage ) );
                            existing.State = MergeReadStates( existing.State, state );
                            existing.AccessMask |= 1u << static_cast<uint32_t>( use.Usage );
                        }
                    }
                }
                for ( const RdgSubUse& use : uses )
                    slot[use.Sub] = -1;
            }
        }

        // ── 0b. A transient read before anything wrote it reads garbage: refuse, naming both ──────────
        {
            std::vector<bool> written( subCount, false );
            for ( uint32_t p = 0; p < passCount; ++p )
            {
                for ( const RdgSubUse& use : passUses[p] )
                {
                    if ( !m_Resources[use.Resource].IsExternal() && !use.Writes && !written[use.Sub] )
                        return Common::MakeFormattedError<CompileResult>(
                             "graph '{}' pass '{}' reads {} as {} before any pass writes it", m_Name,
                             m_Passes[p].Name, describeSub( use.Sub ), GetAccessName( use.FirstAccess ) );
                }
                for ( const RdgSubUse& use : passUses[p] )
                    written[use.Sub] = written[use.Sub] || use.Writes;
            }
        }

        // ── 1+2. Producer edges and culling (FlushCullStack) ──────────────────────────────────────────
        // Roots: NeverCull passes and passes that write an externally visible (external or extracted)
        // subresource; every pass when culling is off. A pass lives if a living pass consumes a subresource
        // it was the last to write before that consumer. The full definition is at Builder::Compile.
        std::vector<bool> alive( passCount, false );
        {
            std::vector<std::vector<uint32_t>> producers( passCount );
            std::vector<int32_t>               lastWriter( subCount, -1 );
            std::vector<uint32_t>              stack;
            for ( uint32_t p = 0; p < passCount; ++p )
            {
                bool root = !m_PassCulling || HasFlag( m_Passes[p].Flags, PassFlags::NeverCull );
                for ( const RdgSubUse& use : passUses[p] )
                {
                    if ( use.Consumes && lastWriter[use.Sub] >= 0 &&
                         static_cast<uint32_t>( lastWriter[use.Sub] ) != p )
                        producers[p].push_back( static_cast<uint32_t>( lastWriter[use.Sub] ) );
                    const ResourceRecord& record = m_Resources[use.Resource];
                    root = root || ( use.Writes && ( record.IsExternal() || record.IsExtracted() ) );
                }
                for ( const RdgSubUse& use : passUses[p] )
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
                for ( uint32_t producer : producers[p] )
                {
                    if ( !alive[producer] )
                    {
                        alive[producer] = true;
                        stack.push_back( producer );
                    }
                }
            }
        }

        CompileResult         result;
        std::vector<uint32_t> executed;
        result.PassCulling = m_PassCulling;
        for ( uint32_t p = 0; p < passCount; ++p )
        {
            if ( alive[p] )
            {
                executed.push_back( p );
                continue;
            }
            result.CulledPasses.push_back( p );
            result.CulledPassNames.push_back( m_Passes[p].Name );
        }
        const uint32_t executedCount = static_cast<uint32_t>( executed.size() );

        // ── 1. RAW / WAR / WAW edges between executed passes ──────────────────────────────────────────
        {
            std::set<std::tuple<uint32_t, uint32_t, uint8_t, uint32_t>> seen;
            std::vector<int32_t>                                        lastWriter( subCount, -1 );
            std::vector<std::vector<uint32_t>>                          readers( subCount );
            auto addEdge = [&]( uint32_t from, uint32_t to, DependencyKind kind, uint32_t resource )
            {
                if ( from != to && seen.insert( { from, to, static_cast<uint8_t>( kind ), resource } ).second )
                    result.Edges.push_back( { from, to, kind, resource } );
            };
            for ( uint32_t p : executed )
            {
                for ( const RdgSubUse& use : passUses[p] )
                {
                    if ( !use.Writes )
                    {
                        if ( lastWriter[use.Sub] >= 0 )
                            addEdge( static_cast<uint32_t>( lastWriter[use.Sub] ), p,
                                     DependencyKind::ReadAfterWrite, use.Resource );
                        readers[use.Sub].push_back( p );
                        continue;
                    }
                    for ( uint32_t reader : readers[use.Sub] )
                        addEdge( reader, p, DependencyKind::WriteAfterRead, use.Resource );
                    if ( lastWriter[use.Sub] >= 0 )
                        addEdge( static_cast<uint32_t>( lastWriter[use.Sub] ), p, DependencyKind::WriteAfterWrite,
                                 use.Resource );
                    readers[use.Sub].clear();
                    lastWriter[use.Sub] = static_cast<int32_t>( p );
                }
            }
        }

        // ── S1. Pipe choice. Passes are never reordered: the scheduler only picks a pipe per pass ─────────
        result.Pipes = pipes;
        std::vector<Pipe>     pipeAt( executedCount, Pipe::Graphics );
        std::vector<uint32_t> positionOf( passCount, ~0u );
        for ( uint32_t position = 0; position < executedCount; ++position )
        {
            const uint32_t p = executed[position];
            positionOf[p]    = position;
            if ( !HasFlag( m_Passes[p].Flags, PassFlags::AsyncCompute ) )
                continue;
            if ( pipes.SeparateComputeFamily )
                pipeAt[position] = Pipe::AsyncCompute;
            else
                result.DemotedAsyncPasses.push_back( p );
        }

        // ── S2. Async runs and their fork / join (CrossPipeSync) ──────────────────────────────────────────
        // A run is a maximal range of consecutive async positions. Its fork is the last graphics pass it
        // depends on (any RAW / WAR / WAW edge, or the release of contents it takes over); its join the first
        // graphics pass that depends on it or takes over its contents, or the end of the graph.
        std::vector<RdgAsyncRun>           runs;
        std::vector<int32_t>               runAt( executedCount, -1 );
        std::vector<std::vector<uint32_t>> waitAt( executedCount );
        std::vector<std::vector<uint32_t>> signalAt( executedCount );
        for ( uint32_t position = 0; position < executedCount; ++position )
        {
            if ( pipeAt[position] != Pipe::AsyncCompute )
                continue;
            if ( position == 0 || pipeAt[position - 1] != Pipe::AsyncCompute )
                runs.push_back( { position, position } );
            runs.back().Last = position;
            runAt[position]  = static_cast<int32_t>( runs.size() - 1 );
        }
        if ( !runs.empty() )
        {
            for ( const DependencyEdge& edge : result.Edges )
            {
                const uint32_t from = positionOf[edge.From];
                const uint32_t to   = positionOf[edge.To];
                if ( pipeAt[from] == pipeAt[to] )
                    continue;
                if ( pipeAt[to] == Pipe::AsyncCompute )
                {
                    RdgAsyncRun& run = runs[static_cast<size_t>( runAt[to] )];
                    run.ForkPosition = std::max( run.ForkPosition, static_cast<int32_t>( from ) );
                }
                else
                {
                    RdgAsyncRun& run = runs[static_cast<size_t>( runAt[from] )];
                    run.JoinPosition = std::min( run.JoinPosition, to );
                }
            }
            // Contents that change owner - read after read included, which has no edge - release on the old
            // pipe before the sync and acquire on the new one after it. The same walk the barrier tracker
            // makes below: externals start the graph owned by Graphics, transients by no pipe.
            std::vector<int32_t> lastTouch( subCount, -1 );
            std::vector<Pipe>    owner( subCount, Pipe::Graphics );
            std::vector<bool>    owned( subCount, false );
            for ( uint32_t r = 0; r < resourceCount; ++r )
            {
                for ( uint32_t sub = subBase[r]; sub < subBase[r + 1]; ++sub )
                    owned[sub] = m_Resources[r].IsExternal();
            }
            for ( uint32_t position = 0; position < executedCount; ++position )
            {
                const Pipe pipe = pipeAt[position];
                for ( const RdgSubUse& use : passUses[executed[position]] )
                {
                    if ( owned[use.Sub] && owner[use.Sub] != pipe && use.Consumes )
                    {
                        if ( pipe == Pipe::AsyncCompute )
                        {
                            RdgAsyncRun& run = runs[static_cast<size_t>( runAt[position] )];
                            if ( lastTouch[use.Sub] < 0 )
                                run.NeedsFork = true;
                            else
                                run.ForkPosition = std::max( run.ForkPosition, lastTouch[use.Sub] );
                        }
                        else
                        {
                            RdgAsyncRun& run =
                                 runs[static_cast<size_t>( runAt[static_cast<size_t>( lastTouch[use.Sub] )] )];
                            run.JoinPosition = std::min( run.JoinPosition, position );
                        }
                    }
                    owned[use.Sub]     = true;
                    owner[use.Sub]     = pipe;
                    lastTouch[use.Sub] = static_cast<int32_t>( position );
                }
            }

            for ( RdgAsyncRun& run : runs )
            {
                // An external the run consumes untouched by this graph so far: the previous graph left it
                // owned by Graphics; the graphics pass just before the run releases it and forks.
                // At position 0 there is no such pass: the graphics prologue segment releases it and forks
                // (amendment B).
                if ( run.NeedsFork && run.ForkPosition < 0 )
                    run.ForkPosition =
                         run.First == 0 ? kRdgForkAtGraphStart : static_cast<int32_t>( run.First - 1 );

                std::vector<bool>  touched( resourceCount, false );
                PipelineStageFlags asyncStages = PipelineStage_None;
                for ( uint32_t position = run.First; position <= run.Last; ++position )
                {
                    for ( const RdgSubUse& use : passUses[executed[position]] )
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
                    run.ForkSync = static_cast<int32_t>( result.Syncs.size() );
                    result.Syncs.push_back(
                         { Pipe::Graphics, fork, Pipe::AsyncCompute, run.First, asyncStages, true } );
                    if ( !atStart ) // the prologue segment signals a fork from the graph start
                        signalAt[fork].push_back( static_cast<uint32_t>( run.ForkSync ) );
                    waitAt[run.First].push_back( static_cast<uint32_t>( run.ForkSync ) );
                }
                // The semaphore wait blocks only the stages it names for everything after it on the graphics
                // queue: every graphics access to anything the run touched, from the join on. A join at the
                // graph end guards the final barriers, which take over the run's own last accesses.
                PipelineStageFlags joinStages = PipelineStage_None;
                if ( run.JoinPosition == CrossPipeSync::kJoinAtGraphEnd )
                    joinStages = asyncStages;
                for ( uint32_t position = run.Last + 1; position < executedCount; ++position )
                {
                    if ( pipeAt[position] != Pipe::Graphics )
                        continue;
                    for ( const RdgSubUse& use : passUses[executed[position]] )
                    {
                        if ( touched[use.Resource] )
                            joinStages |= use.State.Stages;
                    }
                }
                run.JoinSync = static_cast<int32_t>( result.Syncs.size() );
                result.Syncs.push_back(
                     { Pipe::AsyncCompute, run.Last, Pipe::Graphics, run.JoinPosition, joinStages, false } );
                signalAt[run.Last].push_back( static_cast<uint32_t>( run.JoinSync ) );
                if ( run.JoinPosition != CrossPipeSync::kJoinAtGraphEnd )
                    waitAt[run.JoinPosition].push_back( static_cast<uint32_t>( run.JoinSync ) );
            }
        }

        // ── 3. Lifetimes and derived usage of graph-owned resources ───────────────────────────────────
        std::vector<int32_t> lifetimeOf( resourceCount, -1 );
        for ( uint32_t position = 0; position < executedCount; ++position )
        {
            for ( const RdgSubUse& use : passUses[executed[position]] )
            {
                if ( m_Resources[use.Resource].IsExternal() )
                    continue;
                if ( lifetimeOf[use.Resource] < 0 )
                {
                    lifetimeOf[use.Resource] = static_cast<int32_t>( result.Lifetimes.size() );
                    result.Lifetimes.push_back(
                         { use.Resource, position, position, executed[position], executed[position] } );
                    result.Usages.push_back( { use.Resource, 0 } );
                }
                const size_t index                   = static_cast<size_t>( lifetimeOf[use.Resource] );
                result.Lifetimes[index].LastPosition = position;
                result.Lifetimes[index].LastPass     = executed[position];
                result.Usages[index].AccessMask |= use.AccessMask;
            }
        }
        // The aliasing plan's range: widened over the fork..join window of every async run that uses it.
        for ( ResourceLifetime& lifetime : result.Lifetimes )
        {
            lifetime.AliasFirstPosition = lifetime.FirstPosition;
            lifetime.AliasLastPosition  = lifetime.LastPosition;
        }
        for ( const RdgAsyncRun& run : runs )
        {
            const uint32_t windowFirst = run.ForkPosition < 0 ? 0u : static_cast<uint32_t>( run.ForkPosition );
            const uint32_t windowLast =
                 run.JoinPosition == CrossPipeSync::kJoinAtGraphEnd ? executedCount - 1 : run.JoinPosition;
            for ( uint32_t position = run.First; position <= run.Last; ++position )
            {
                for ( const RdgSubUse& use : passUses[executed[position]] )
                {
                    if ( lifetimeOf[use.Resource] < 0 )
                        continue;
                    ResourceLifetime& lifetime = result.Lifetimes[static_cast<size_t>( lifetimeOf[use.Resource] )];
                    lifetime.UsedOnAsyncCompute = true;
                    lifetime.AliasFirstPosition = std::min( lifetime.AliasFirstPosition, windowFirst );
                    lifetime.AliasLastPosition  = std::max( lifetime.AliasLastPosition, windowLast );
                }
            }
        }
        for ( DerivedUsage& usage : result.Usages )
        {
            const ResourceRecord& record = m_Resources[usage.Resource];
            if ( record.IsExtracted() )
                usage.AccessMask |= 1u << static_cast<uint32_t>( record.FinalAccess );
        }

        // ── 6. Aliasing plan: interval packing per memory class ───────────────────────────────────────
        // Extracted transients outlive the graph and so never share memory.
        {
            struct Footprint
            {
                uint64_t    Size;
                uint64_t    Alignment;
                uint32_t    MemoryTypeBits;
                MemoryClass Class;
            };
            struct Candidate
            {
                uint32_t  Lifetime;
                Footprint Footprint;
            };
            std::vector<Candidate> candidates;
            for ( uint32_t l = 0; l < result.Lifetimes.size(); ++l )
            {
                const ResourceRecord& record = m_Resources[result.Lifetimes[l].Resource];
                if ( record.IsExtracted() )
                    continue;
                const bool isTexture = record.Kind == ResourceKind::Texture;
                // Lifetimes and Usages are pushed together, so index l names the same resource in both.
                const uint32_t                        usage = result.Usages[l].AccessMask;
                Common::ResultStr<MemoryRequirements> requirements =
                     isTexture ? memory.GetTextureRequirements( record.Texture, usage )
                               : memory.GetBufferRequirements( record.Buffer, usage );
                if ( !requirements )
                    return Common::MakeFormattedError<CompileResult>(
                         "graph '{}': no memory requirements for transient '{}': {}", m_Name, record.Name,
                         requirements.GetError() );
                const MemoryRequirements& req = requirements.GetValue();
                if ( req.Size == 0 || req.Alignment == 0 || ( req.Alignment & ( req.Alignment - 1 ) ) != 0 ||
                     req.MemoryTypeBits == 0 )
                    return Common::MakeFormattedError<CompileResult>(
                         "graph '{}': transient '{}' got unusable memory requirements (size {}, alignment {}, "
                         "memory types {:#x})",
                         m_Name, record.Name, req.Size, req.Alignment, req.MemoryTypeBits );
                candidates.push_back(
                     { l,
                       { RdgAlignOffset( req.Size, req.Alignment ), req.Alignment, req.MemoryTypeBits,
                         isTexture ? MemoryClass::Texture : MemoryClass::Buffer } } );
            }
            // First come, first placed; larger first on ties so big targets take the low offsets.
            std::sort( candidates.begin(), candidates.end(),
                       [&]( const Candidate& a, const Candidate& b )
                       {
                           const ResourceLifetime& la = result.Lifetimes[a.Lifetime];
                           const ResourceLifetime& lb = result.Lifetimes[b.Lifetime];
                           if ( la.AliasFirstPosition != lb.AliasFirstPosition )
                               return la.AliasFirstPosition < lb.AliasFirstPosition;
                           if ( a.Footprint.Size != b.Footprint.Size )
                               return a.Footprint.Size > b.Footprint.Size;
                           return la.Resource < lb.Resource;
                       } );

            std::vector<uint32_t> placedLifetime;
            for ( const Candidate& candidate : candidates )
            {
                const ResourceLifetime& lifetime = result.Lifetimes[candidate.Lifetime];
                const Footprint&        fp       = candidate.Footprint;

                // Bytes can be shared only by resources that could live in one memory type: the first heap of
                // the class whose type set (the intersection of its allocations' sets) meets this one's takes
                // it; a disjoint set opens a heap of its own and never aliases the others.
                uint32_t heap = 0;
                while ( heap < result.Aliasing.Heaps.size() &&
                        ( result.Aliasing.Heaps[heap].Class != fp.Class ||
                          ( result.Aliasing.Heaps[heap].MemoryTypeBits & fp.MemoryTypeBits ) == 0 ) )
                    ++heap;
                if ( heap == result.Aliasing.Heaps.size() )
                    result.Aliasing.Heaps.push_back( { fp.Class, fp.MemoryTypeBits, 0, 1 } );

                std::vector<const Allocation*> conflicts;
                for ( size_t a = 0; a < result.Aliasing.Allocations.size(); ++a )
                {
                    const Allocation& placed = result.Aliasing.Allocations[a];
                    if ( placed.Heap == heap &&
                         RdgLifetimesIntersect( result.Lifetimes[placedLifetime[a]], lifetime ) )
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
                for ( size_t a = 0; a < result.Aliasing.Allocations.size(); ++a )
                {
                    const Allocation& placed = result.Aliasing.Allocations[a];
                    if ( placed.Heap == heap &&
                         result.Lifetimes[placedLifetime[a]].AliasLastPosition < lifetime.AliasFirstPosition &&
                         RdgBytesIntersect( offset, fp.Size, placed.Offset, placed.Size ) )
                        allocation.AliasPredecessors.push_back( placed.Resource );
                }
                TransientHeapDesc& heapDesc = result.Aliasing.Heaps[heap];
                heapDesc.MemoryTypeBits &= fp.MemoryTypeBits;
                heapDesc.Bytes     = std::max( heapDesc.Bytes, offset + fp.Size );
                heapDesc.Alignment = std::max( heapDesc.Alignment, fp.Alignment );
                result.Aliasing.UnaliasedBytes += fp.Size;
                result.Aliasing.Allocations.push_back( std::move( allocation ) );
                placedLifetime.push_back( candidate.Lifetime );
            }
            // A class's peak is the memory its heaps hold together.
            for ( const TransientHeapDesc& heapDesc : result.Aliasing.Heaps )
                result.Aliasing.PeakBytes[static_cast<uint32_t>( heapDesc.Class )] += heapDesc.Bytes;
            for ( uint64_t peak : result.Aliasing.PeakBytes )
                result.Aliasing.TotalPeakBytes += peak;
        }

        // ── 4+5. Barriers (merged read states) and load/store decisions ───────────────────────────────
        std::vector<RdgSubTrack>                   track( subCount );
        std::vector<std::vector<RdgRawTransition>> raws( executedCount );
        std::vector<std::vector<RdgRawTransition>> epilogueRaws(
             executedCount + 1 ); // ownership releases; [executedCount]: the prologue's (amendment B)
        auto                                       isAliased = [&]( uint32_t resource )
        {
            const Allocation* allocation = result.FindAllocation( resource );
            return allocation && !allocation->AliasPredecessors.empty();
        };

        // The state a subresource is in before the graph touches it. For transient memory that is
        // "undefined", entered after whatever aliased predecessor last used the same bytes.
        auto initialState = [&]( uint32_t resource, uint32_t local )
        {
            const ResourceRecord& record = m_Resources[resource];
            if ( record.ExternalTex )
                return record.ExternalTex->SubresourceStates[local];
            if ( record.ExternalBuf )
                return record.ExternalBuf->State;
            AccessState state = kRdgUntouchedState;
            if ( const Allocation* allocation = result.FindAllocation( resource ) )
            {
                for ( uint32_t predecessor : allocation->AliasPredecessors )
                {
                    for ( uint32_t sub = subBase[predecessor]; sub < subBase[predecessor + 1]; ++sub )
                    {
                        if ( track[sub].Touched )
                        {
                            state.Stages |= track[sub].Group.Stages;
                            state.Memory |= track[sub].Group.Memory;
                        }
                    }
                }
            }
            return state;
        };

        std::vector<bool> writtenSoFar( subCount, false );
        for ( uint32_t position = 0; position < executedCount; ++position )
        {
            const uint32_t p = executed[position];
            for ( const RdgSubUse& use : passUses[p] )
            {
                RdgSubTrack&          sub       = track[use.Sub];
                const ResourceRecord& record    = m_Resources[use.Resource];
                const bool            transient = !record.IsExternal();
                const Pipe            pipe      = pipeAt[position];
                auto addRaw = [&]( uint32_t at, const AccessState& before, const AccessState& after, bool discard )
                {
                    raws[at].push_back( { use.Sub, use.Resource, before, after, discard } );
                    return static_cast<int32_t>( raws[at].size() - 1 );
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
                    raws[position].push_back( { use.Sub, use.Resource, before, use.State, false,
                                                BarrierKind::OwnershipAcquire, from, pipe, releasePosition } );
                    sub.GroupRaw = static_cast<int32_t>( raws[position].size() - 1 );
                    epilogueRaws[releasePosition].push_back( { use.Sub, use.Resource, before, use.State, false,
                                                               BarrierKind::OwnershipRelease, from, pipe,
                                                               position } );
                    sub.ReleaseRaw      = static_cast<int32_t>( epilogueRaws[releasePosition].size() - 1 );
                    sub.ReleasePosition = releasePosition;
                };

                if ( !sub.Touched )
                {
                    sub.Touched       = true;
                    sub.GroupBefore   = initialState( use.Resource, use.Sub - subBase[use.Resource] );
                    sub.Group         = use.State;
                    sub.GroupPosition = position;
                    sub.GroupRaw      = -1;
                    sub.ReleaseRaw    = -1;
                    if ( record.IsExternal() && pipe != Pipe::Graphics )
                    {
                        // Externals enter every graph owned by Graphics; the run's fork pass releases it.
                        // A fork from the graph start releases it in the prologue (slot executedCount).
                        const int32_t fork = runs[static_cast<size_t>( runAt[position] )].ForkPosition;
                        // The host is not a queue family: no ownership transfer may name the HOST stage. The
                        // host's accesses to an external (a readback of an earlier frame) all happen before
                        // this graph's submission, which orders them before every command in it, so the
                        // transfer's source scope is the device part of the state alone.
                        AccessState entered = sub.GroupBefore;
                        entered.Stages &= ~static_cast<PipelineStageFlags>( PipelineStage_Host );
                        entered.Memory &= ~static_cast<MemoryAccessFlags>( MemoryAccess_HostRead );
                        crossPipes( entered,
                                    fork == kRdgForkAtGraphStart ? executedCount : static_cast<uint32_t>( fork ),
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
                            // the predecessors' last accesses (initialState) as its source scope.
                            if ( transient && isAliased( use.Resource ) )
                                raws[position][static_cast<size_t>( sub.GroupRaw )].Kind =
                                     BarrierKind::AliasAcquire;
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
                        raws[sub.GroupPosition][static_cast<size_t>( sub.GroupRaw )].After = sub.Group;
                        // An acquire widens with its release half, so both halves stay one transfer.
                        if ( sub.ReleaseRaw >= 0 )
                            epilogueRaws[sub.ReleasePosition][static_cast<size_t>( sub.ReleaseRaw )].After =
                                 sub.Group;
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

            // Load: a transient attachment nothing has written yet has nothing to load.
            const PassRecord& pass = m_Passes[p];
            CompiledPass      compiled;
            compiled.Pass  = p;
            compiled.Name  = pass.Name;
            compiled.Flags       = pass.Flags;
            compiled.OnPipe      = pipeAt[position];
            compiled.WaitSyncs   = waitAt[position];
            compiled.SignalSyncs = signalAt[position];
            for ( const AttachmentRecord& attachment : pass.Attachments )
            {
                const ResourceRecord& record     = m_Resources[attachment.Resource];
                bool                  anyWritten = false;
                for ( uint32_t layer = attachment.BaseLayer; layer < attachment.BaseLayer + attachment.LayerCount;
                      ++layer )
                    anyWritten =
                         anyWritten || writtenSoFar[subBase[attachment.Resource] +
                                                    record.Texture.SubresourceIndex( attachment.Mip, layer )];
                AttachmentDecision decision;
                decision.Slot                 = attachment.Slot;
                decision.IsDepth              = attachment.IsDepth;
                decision.IsResolve            = attachment.IsResolve;
                const int32_t attachmentIndex = static_cast<int32_t>( &attachment - pass.Attachments.data() );
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
                if ( decision.Load == LoadAction::Load && !record.IsExternal() && !anyWritten )
                    decision.Load = LoadAction::DontCare;
                compiled.Attachments.push_back( decision );
            }
            for ( const RdgSubUse& use : passUses[p] )
                writtenSoFar[use.Sub] = writtenSoFar[use.Sub] || use.Writes;
            result.Passes.push_back( std::move( compiled ) );
        }

        // Store: DontCare when nothing after the pass needs the contents and nothing outside the graph
        // will see them. Walked backwards so "needed later" is known when each pass is reached.
        {
            std::vector<bool> neededLater( subCount, false );
            for ( uint32_t position = executedCount; position-- > 0; )
            {
                const uint32_t p = executed[position];
                for ( AttachmentDecision& decision : result.Passes[position].Attachments )
                {
                    const ResourceRecord& record = m_Resources[decision.Resource];
                    bool                  needed = record.IsExternal() || record.IsExtracted();
                    for ( uint32_t layer = decision.BaseLayer; layer < decision.BaseLayer + decision.LayerCount;
                          ++layer )
                        needed = needed || neededLater[subBase[decision.Resource] +
                                                       record.Texture.SubresourceIndex( decision.Mip, layer )];
                    const AttachmentRecord& declared = m_Passes[p].Attachments[static_cast<size_t>(
                         &decision - result.Passes[position].Attachments.data() )];
                    decision.Store = needed && declared.Store == StoreAction::Store ? StoreAction::Store
                                                                                    : StoreAction::DontCare;
                }
                for ( const RdgSubUse& use : passUses[p] )
                {
                    if ( use.Consumes )
                        neededLater[use.Sub] = true;
                    else if ( use.Writes )
                        neededLater[use.Sub] = false;
                }
            }
        }

        // ── Render-pass merging ─────────────────────────────────────────────────────────────────────────
        // A raster pass whose attachments are exactly those of the executed pass before it (same resources,
        // slots, subresources and attachment usage) and which LOADS every one of them continues the render
        // pass that pass opened: one begin/end for the run. Attachment writes inside one subpass are ordered
        // by rasterisation order, so the attachment-to-same-state transition planned between them is dropped;
        // any OTHER transition (a texture the pass samples, a layout change) cannot be recorded inside a
        // render pass and splits the run, as does a Clear or DontCare load.
        {
            auto sameAttachment = []( const AttachmentDecision& a, const AttachmentDecision& b )
            {
                return a.IsDepth == b.IsDepth && a.IsResolve == b.IsResolve && a.Slot == b.Slot &&
                       a.Usage == b.Usage &&
                       a.Resource == b.Resource && a.Mip == b.Mip && a.BaseLayer == b.BaseLayer &&
                       a.LayerCount == b.LayerCount;
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
                const uint32_t     local = raw.Sub - subBase[raw.Resource];
                const uint32_t     mip   = local % desc.Mips;
                const uint32_t     layer = local / desc.Mips;
                for ( const AttachmentDecision& attachment : pass.Attachments )
                {
                    if ( attachment.Resource == raw.Resource && attachment.Mip == mip &&
                         layer >= attachment.BaseLayer && layer < attachment.BaseLayer + attachment.LayerCount )
                        return true;
                }
                return false;
            };
            uint32_t opener = 0;
            for ( uint32_t position = 1; position < executedCount; ++position )
            {
                CompiledPass& previous = result.Passes[position - 1];
                CompiledPass& current  = result.Passes[position];
                bool          merge    = HasFlag( previous.Flags, PassFlags::Raster ) &&
                             HasFlag( current.Flags, PassFlags::Raster ) && !current.Attachments.empty() &&
                             sameAttachments( previous, current );
                // A resolve target is never loaded (the resolve at the end of the run overwrites it), so it
                // does not stop the run.
                for ( const AttachmentDecision& attachment : current.Attachments )
                    merge = merge && ( attachment.IsResolve || attachment.Load == LoadAction::Load );
                for ( const RdgRawTransition& raw : raws[position] )
                    merge = merge && raw.Before == raw.After && isOwnAttachment( current, raw );
                // Never across a sync or a pipe: a pass that waits or signals neither continues nor keeps open
                // a render pass, and a release after the previous pass needs its render pass ended.
                merge = merge && previous.OnPipe == current.OnPipe && previous.WaitSyncs.empty() &&
                        previous.SignalSyncs.empty() && current.WaitSyncs.empty() && current.SignalSyncs.empty() &&
                        epilogueRaws[position - 1].empty();
                if ( !merge )
                {
                    opener = position;
                    continue;
                }
                raws[position].clear();
                previous.KeepsRenderPassOpen = true;
                current.ContinuesRenderPass  = true;
                // The render pass the opener began stores what the LAST pass of the run leaves.
                for ( AttachmentDecision& opened : result.Passes[opener].Attachments )
                {
                    for ( const AttachmentDecision& attachment : current.Attachments )
                    {
                        if ( sameAttachment( opened, attachment ) )
                            opened.Store = attachment.Store;
                    }
                }
            }
        }

        // Final transitions and the states handed back to externals / extraction targets.
        std::vector<RdgRawTransition> finalRaws;
        for ( uint32_t r = 0; r < resourceCount; ++r )
        {
            const ResourceRecord& record = m_Resources[r];
            if ( !record.IsExternal() && !record.IsExtracted() )
                continue;
            ExternalFinalState final;
            final.Resource = r;
            final.Kind     = record.Kind;
            for ( uint32_t sub = subBase[r]; sub < subBase[r + 1]; ++sub )
            {
                AccessState last = track[sub].Touched ? track[sub].Group : initialState( r, sub - subBase[r] );
                if ( track[sub].Touched && track[sub].GroupPipe != Pipe::Graphics )
                {
                    // Every external and extraction target leaves the graph owned by Graphics: released after
                    // its last async use, acquired in the final barriers after the run's join.
                    const AccessState target = record.IsExtracted() ? GetAccessState( record.FinalAccess ) : last;
                    const uint32_t    at     = track[sub].LastTouch;
                    // The host is not a queue family: no ownership transfer may name the HOST stage. A host
                    // readback gets its ownership back on Graphics in the state the run left it in, then a
                    // plain graphics barrier hands it to the host (which waits on the graph's completion).
                    const bool        host = ( target.Stages & PipelineStage_Host ) != 0;
                    const AccessState owned =
                         host ? AccessState{ last.Stages, last.Memory, target.Layout } : target;
                    finalRaws.push_back( { sub, r, last, owned, false, BarrierKind::OwnershipAcquire,
                                           track[sub].GroupPipe, Pipe::Graphics, at } );
                    epilogueRaws[at].push_back( { sub, r, last, owned, false, BarrierKind::OwnershipRelease,
                                                  track[sub].GroupPipe, Pipe::Graphics,
                                                  CrossPipeSync::kJoinAtGraphEnd } );
                    if ( host )
                        finalRaws.push_back( { sub, r, owned, target, false } );
                    last = target;
                }
                else if ( record.IsExtracted() )
                {
                    const AccessState target = GetAccessState( record.FinalAccess );
                    if ( !IsCoveredReadState( last, target ) )
                        finalRaws.push_back( { sub, r, last, target, false } );
                    last = target;
                }
                final.SubresourceStates.push_back( last );
            }
            result.ExternalFinalStates.push_back( std::move( final ) );
        }

        // Ranges: one barrier per rectangle of subresources sharing a transition, grouped by resource.
        // @p partners receives, per barrier, the position of the other half of an ownership transfer.
        auto emit =
             [&]( std::vector<RdgRawTransition>& list, std::vector<Barrier>& out, std::vector<uint32_t>& partners )
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
                    raw.Sub -= subBase[raw.Resource];
                RdgAppendMergedBarriers( group, &record.Texture, record.Kind, out, partners );
                i = end;
            }
        };
        // One QueueOwnershipTransfer per acquire rectangle; its release has the same rectangle (the two
        // halves carry the same full transition, so they merge alike).
        auto addTransfers = [&]( const std::vector<Barrier>& barriers, const std::vector<uint32_t>& partners,
                                 uint32_t acquirePosition )
        {
            for ( size_t b = 0; b < barriers.size(); ++b )
            {
                const Barrier& barrier = barriers[b];
                if ( barrier.BarrierType != BarrierKind::OwnershipAcquire )
                    continue;
                QueueOwnershipTransfer transfer;
                transfer.Resource        = barrier.Resource;
                transfer.Kind            = barrier.Kind;
                transfer.Range           = barrier.Range;
                transfer.From            = barrier.SrcPipe;
                transfer.To              = barrier.DstPipe;
                transfer.ReleasePosition =
                     partners[b] == executedCount ? CrossPipeSync::kForkAtGraphStart : partners[b];
                transfer.AcquirePosition = acquirePosition;
                const RdgAsyncRun& run   = barrier.SrcPipe == Pipe::Graphics
                                                ? runs[static_cast<size_t>( runAt[acquirePosition] )]
                                                : runs[static_cast<size_t>( runAt[transfer.ReleasePosition] )];
                transfer.Sync =
                     static_cast<uint32_t>( barrier.SrcPipe == Pipe::Graphics ? run.ForkSync : run.JoinSync );
                result.OwnershipTransfers.push_back( transfer );
            }
        };
        for ( uint32_t position = 0; position < executedCount; ++position )
        {
            std::vector<uint32_t> partners;
            emit( raws[position], result.Passes[position].Barriers, partners );
            addTransfers( result.Passes[position].Barriers, partners, position );
            std::vector<uint32_t> releasePartners;
            emit( epilogueRaws[position], result.Passes[position].EpilogueBarriers, releasePartners );
        }
        {
            std::vector<uint32_t> releasePartners;
            emit( epilogueRaws[executedCount], result.PrologueBarriers, releasePartners );
        }
        {
            std::vector<uint32_t> partners;
            emit( finalRaws, result.FinalBarriers, partners );
            addTransfers( result.FinalBarriers, partners, CrossPipeSync::kJoinAtGraphEnd );
        }

        // ── S2b. Per-pipe stage filtering: a pass's barriers are recorded on its pipe (a release on the
        // source pipe, an acquire on the destination pipe), the prologue and the final barriers on Graphics,
        // and a semaphore wait blocks stages of the waiting pipe.
        for ( CompiledPass& pass : result.Passes )
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
        for ( CrossPipeSync& sync : result.Syncs )
        {
            const AccessState waited{ sync.WaitStages, MemoryAccess_None, ImageLayout::Undefined };
            sync.WaitStages = RdgOnPipe( waited, sync.WaitPipe ).Stages;
        }

        // ── S3. Pipe segments: what the backend records into one command buffer and submits once ──────────
        // An async segment is one run (fork wait at its first pass, join signal at its last); a graphics
        // segment is split before a pass that waits on a join and after a pass that signals a fork. In
        // position order, a segment comes after every segment it waits on.
        // Amendment B: the graphics prologue (no passes) signals every fork from the graph start, first.
        {
            PipeSegment prologue{ Pipe::Graphics, CrossPipeSync::kForkAtGraphStart,
                                  CrossPipeSync::kForkAtGraphStart };
            for ( uint32_t sync = 0; sync < result.Syncs.size(); ++sync )
            {
                if ( result.Syncs[sync].SignalPosition == CrossPipeSync::kForkAtGraphStart )
                    prologue.SignalSyncs.push_back( sync );
            }
            if ( !prologue.SignalSyncs.empty() )
                result.Segments.push_back( std::move( prologue ) );
        }
        for ( uint32_t position = 0; position < executedCount; ++position )
        {
            const CompiledPass& pass  = result.Passes[position];
            const bool          split = position == 0 || pass.OnPipe != result.Passes[position - 1].OnPipe ||
                               ( pass.OnPipe == Pipe::Graphics &&
                                 ( !pass.WaitSyncs.empty() || !result.Passes[position - 1].SignalSyncs.empty() ) );
            if ( split )
                result.Segments.push_back( { pass.OnPipe, position, position } );
            PipeSegment& segment = result.Segments.back();
            segment.LastPosition = position;
            segment.WaitSyncs.insert( segment.WaitSyncs.end(), pass.WaitSyncs.begin(), pass.WaitSyncs.end() );
            segment.SignalSyncs.insert( segment.SignalSyncs.end(), pass.SignalSyncs.begin(),
                                        pass.SignalSyncs.end() );
        }

        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Graphic::RDG
