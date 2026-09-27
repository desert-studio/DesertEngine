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
        };

        // Per-subresource walk state while barriers are collected.
        struct RdgSubTrack
        {
            bool        Touched = false;
            AccessState GroupBefore; // state the current group was entered from
            AccessState Group;       // current (possibly merged) state
            uint32_t    GroupPosition = 0;
            int32_t     GroupRaw      = -1; // index of the group's transition in its pass's raw list
        };

        constexpr AccessState kRdgUntouchedState{ PipelineStage_None, MemoryAccess_None, ImageLayout::Undefined };

        uint64_t RdgAlignOffset( uint64_t value, uint64_t alignment )
        {
            return ( value + alignment - 1 ) / alignment * alignment;
        }

        bool RdgLifetimesIntersect( const ResourceLifetime& a, const ResourceLifetime& b )
        {
            return a.FirstPosition <= b.LastPosition && b.FirstPosition <= a.LastPosition;
        }

        bool RdgBytesIntersect( uint64_t aOffset, uint64_t aSize, uint64_t bOffset, uint64_t bSize )
        {
            return aOffset < bOffset + bSize && bOffset < aOffset + aSize;
        }

        // Merges one resource's per-subresource transitions into rectangles: runs of mips inside a layer,
        // then identical runs on consecutive layers. A bloom chain's "mip i-1 to read, mip i to write"
        // stays two barriers; a cascade map's four layers entering the same state become one.
        void RdgAppendMergedBarriers( std::vector<RdgRawTransition>& raws, const TextureDesc* desc,
                                      ResourceKind kind, std::vector<Barrier>& out )
        {
            if ( kind == ResourceKind::Buffer )
            {
                for ( const RdgRawTransition& raw : raws )
                    out.push_back( { raw.Resource, kind, SubresourceRange{ 0, 1, 0, 1 }, raw.Before, raw.After,
                                     raw.Discard } );
                return;
            }
            // Local subresource index is layer-major, so sorting by it walks layer by layer, mip by mip.
            std::sort( raws.begin(), raws.end(),
                       []( const RdgRawTransition& a, const RdgRawTransition& b ) { return a.Sub < b.Sub; } );
            const size_t first = out.size();
            for ( size_t i = 0; i < raws.size(); )
            {
                const uint32_t layer   = raws[i].Sub / desc->Mips;
                const uint32_t baseMip = raws[i].Sub % desc->Mips;
                size_t         end     = i + 1;
                while ( end < raws.size() && raws[end].Sub == raws[end - 1].Sub + 1 &&
                        raws[end].Sub / desc->Mips == layer && raws[end].Before == raws[i].Before &&
                        raws[end].After == raws[i].After && raws[end].Discard == raws[i].Discard )
                    ++end;
                const uint32_t mipCount = static_cast<uint32_t>( end - i );

                bool extended = false;
                for ( size_t b = first; b < out.size() && !extended; ++b )
                {
                    Barrier& open = out[b];
                    if ( open.Range.BaseMip == baseMip && open.Range.MipCount == mipCount &&
                         open.Range.BaseLayer + open.Range.LayerCount == layer && open.Before == raws[i].Before &&
                         open.After == raws[i].After && open.DiscardContents == raws[i].Discard )
                    {
                        ++open.Range.LayerCount;
                        extended = true;
                    }
                }
                if ( !extended )
                    out.push_back( { raws[i].Resource, kind, SubresourceRange{ baseMip, mipCount, layer, 1 },
                                     raws[i].Before, raws[i].After, raws[i].Discard } );
                i = end;
            }
        }
    } // namespace

    Common::ResultStr<CompileResult> Builder::Compile( const IMemoryRequirementsProvider& memory ) const
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
        // Roots: NeverCull passes and passes that write an external or extracted resource. A pass lives
        // if a living pass consumes a subresource it was the last to write before that consumer.
        std::vector<bool> alive( passCount, false );
        {
            std::vector<std::vector<uint32_t>> producers( passCount );
            std::vector<int32_t>               lastWriter( subCount, -1 );
            std::vector<uint32_t>              stack;
            for ( uint32_t p = 0; p < passCount; ++p )
            {
                bool root = HasFlag( m_Passes[p].Flags, PassFlags::NeverCull );
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
        for ( uint32_t p = 0; p < passCount; ++p )
        {
            if ( alive[p] )
                executed.push_back( p );
            else
                result.CulledPasses.push_back( p );
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
                    result.Lifetimes.push_back( { use.Resource, position, position } );
                    result.Usages.push_back( { use.Resource, 0 } );
                }
                const size_t index                   = static_cast<size_t>( lifetimeOf[use.Resource] );
                result.Lifetimes[index].LastPosition = position;
                result.Usages[index].AccessMask |= use.AccessMask;
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
                candidates.push_back( { l, { RdgAlignOffset( req.Size, req.Alignment ), req.Alignment,
                                             req.MemoryTypeBits,
                                             isTexture ? MemoryClass::Texture : MemoryClass::Buffer } } );
            }
            // First come, first placed; larger first on ties so big targets take the low offsets.
            std::sort( candidates.begin(), candidates.end(),
                       [&]( const Candidate& a, const Candidate& b )
                       {
                           const ResourceLifetime& la = result.Lifetimes[a.Lifetime];
                           const ResourceLifetime& lb = result.Lifetimes[b.Lifetime];
                           if ( la.FirstPosition != lb.FirstPosition )
                               return la.FirstPosition < lb.FirstPosition;
                           if ( a.Footprint.Size != b.Footprint.Size )
                               return a.Footprint.Size > b.Footprint.Size;
                           return la.Resource < lb.Resource;
                       } );

            std::vector<uint32_t> placedLifetime;
            for ( const Candidate& candidate : candidates )
            {
                const ResourceLifetime& lifetime = result.Lifetimes[candidate.Lifetime];
                const Footprint&        fp       = candidate.Footprint;

                // Bytes can be shared only by resources that could live in one memory type: a placed
                // allocation with a disjoint type set conflicts for its whole lifetime, as if always alive.
                std::vector<const Allocation*> conflicts;
                for ( size_t a = 0; a < result.Aliasing.Allocations.size(); ++a )
                {
                    const Allocation& placed = result.Aliasing.Allocations[a];
                    if ( placed.Class == fp.Class &&
                         ( ( placed.MemoryTypeBits & fp.MemoryTypeBits ) == 0 ||
                           RdgLifetimesIntersect( result.Lifetimes[placedLifetime[a]], lifetime ) ) )
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
                allocation.Resource  = lifetime.Resource;
                allocation.Class     = fp.Class;
                allocation.Offset    = offset;
                allocation.Size      = fp.Size;
                allocation.Alignment = fp.Alignment;
                allocation.MemoryTypeBits = fp.MemoryTypeBits;
                for ( size_t a = 0; a < result.Aliasing.Allocations.size(); ++a )
                {
                    const Allocation& placed = result.Aliasing.Allocations[a];
                    if ( placed.Class == fp.Class && ( placed.MemoryTypeBits & fp.MemoryTypeBits ) != 0 &&
                         result.Lifetimes[placedLifetime[a]].LastPosition < lifetime.FirstPosition &&
                         RdgBytesIntersect( offset, fp.Size, placed.Offset, placed.Size ) )
                        allocation.AliasPredecessors.push_back( placed.Resource );
                }
                const uint32_t classIndex = static_cast<uint32_t>( fp.Class );
                result.Aliasing.PeakBytes[classIndex] =
                     std::max( result.Aliasing.PeakBytes[classIndex], offset + fp.Size );
                result.Aliasing.UnaliasedBytes += fp.Size;
                result.Aliasing.Allocations.push_back( std::move( allocation ) );
                placedLifetime.push_back( candidate.Lifetime );
            }
            for ( uint64_t peak : result.Aliasing.PeakBytes )
                result.Aliasing.TotalPeakBytes += peak;
        }

        // ── 4+5. Barriers (merged read states) and load/store decisions ───────────────────────────────
        std::vector<RdgSubTrack>                   track( subCount );
        std::vector<std::vector<RdgRawTransition>> raws( executedCount );

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
                auto addRaw = [&]( uint32_t at, const AccessState& before, const AccessState& after, bool discard )
                {
                    raws[at].push_back( { use.Sub, use.Resource, before, after, discard } );
                    return static_cast<int32_t>( raws[at].size() - 1 );
                };

                if ( !sub.Touched )
                {
                    sub.Touched       = true;
                    sub.GroupBefore   = initialState( use.Resource, use.Sub - subBase[use.Resource] );
                    sub.Group         = use.State;
                    sub.GroupPosition = position;
                    sub.GroupRaw      = -1;
                    // A buffer has no layout to transition: with no earlier user of its bytes there is
                    // nothing to wait for either, so its first use needs no barrier at all.
                    const bool nothingToWaitFor =
                         record.Kind == ResourceKind::Buffer && sub.GroupBefore == kRdgUntouchedState;
                    if ( !nothingToWaitFor && !IsCoveredReadState( sub.GroupBefore, use.State ) )
                        sub.GroupRaw = addRaw( position, sub.GroupBefore, use.State, transient );
                }
                else if ( sub.Group.IsReadOnly() && use.State.IsReadOnly() &&
                          sub.Group.Layout == use.State.Layout )
                {
                    // Merged read state: the barrier that opened this group of reads is widened to cover
                    // this pass's stages too, so no barrier is needed between the readers.
                    sub.Group = MergeReadStates( sub.Group, use.State );
                    if ( sub.GroupRaw >= 0 )
                        raws[sub.GroupPosition][static_cast<size_t>( sub.GroupRaw )].After = sub.Group;
                    else if ( !IsCoveredReadState( sub.GroupBefore, sub.Group ) )
                        sub.GroupRaw = addRaw( sub.GroupPosition, sub.GroupBefore, sub.Group, false );
                }
                else
                {
                    // Every other change - read to write (WAR), write to read (RAW), write to write (WAW),
                    // or a read in another layout - is a barrier.
                    sub.GroupBefore   = sub.Group;
                    sub.Group         = use.State;
                    sub.GroupPosition = position;
                    sub.GroupRaw      = addRaw( position, sub.GroupBefore, use.State, false );
                }
            }

            // Load: a transient attachment nothing has written yet has nothing to load.
            const PassRecord& pass = m_Passes[p];
            CompiledPass      compiled;
            compiled.Pass  = p;
            compiled.Name  = pass.Name;
            compiled.Flags = pass.Flags;
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
                decision.Slot       = attachment.Slot;
                decision.IsDepth    = attachment.IsDepth;
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
                    decision.Store = needed ? StoreAction::Store : StoreAction::DontCare;
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
                if ( record.IsExtracted() )
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
        auto emit = [&]( std::vector<RdgRawTransition>& list, std::vector<Barrier>& out )
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
                RdgAppendMergedBarriers( group, &record.Texture, record.Kind, out );
                i = end;
            }
        };
        for ( uint32_t position = 0; position < executedCount; ++position )
            emit( raws[position], result.Passes[position].Barriers );
        emit( finalRaws, result.FinalBarriers );

        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Graphic::RDG
