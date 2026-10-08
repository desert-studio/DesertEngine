#pragma once

#include "RenderPhase.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace Desert::Graphic
{
    // ------------------------------------------------------------------------------------------------
    // The render graph's ordering rules, as pure functions of integers.
    //
    // They live OUTSIDE RenderGraphBuilder deliberately. The builder caches Vulkan RenderPass objects
    // while it sorts, so it cannot be constructed without a device, and "which pass draws first" was
    // therefore only ever observable by looking at a picture. That is how the order inside a phase
    // stayed undefined for so long: nothing about it could be asserted. Everything below is decided
    // here, on plain numbers, and is covered by Tests/Engine/RenderGraphSort.
    //
    // Two levels of order, and both are total:
    //   1. Between phases — a topological sort of the phase dependency graph, ties broken by the
    //      registry's declaration order.
    //   2. Inside one phase — the registration index (who called AddPass first). There is no numeric
    //      placement: an order a pass depends on is the order of the calls in SceneRenderer's frame-build
    //      functions (AddFrameTranslucency draws the fog, then the cloud composite, then the phase).
    // ------------------------------------------------------------------------------------------------

    // Everything the sort needs to know about one registered pass.
    struct RenderPassSortKey
    {
        RenderPhaseID Phase             = RenderPhase::None;
        uint64_t      RegistrationIndex = 0;
    };

    // phase -> the phases that must finish before it. Ordered containers, not hashed ones: the walk
    // order of an unordered_map is an artefact of the hash and of the insertion history, and feeding it
    // into a tie-break makes the frame's draw order depend on how many systems happen to be registered.
    using RenderPhaseDependencies = std::map<RenderPhaseID, std::set<RenderPhaseID>>;

    // Kahn's algorithm over the phase graph.
    //
    // `presentPhases` are the phases that actually own passes; phases that appear only as an endpoint of
    // a dependency are included too, so an edge onto an empty phase still constrains the phases around
    // it. Among the phases that are ready at the same moment the earliest in `declarationOrder` wins;
    // a phase missing from `declarationOrder` (an unregistered raw ID) sorts after every declared one,
    // by ascending ID, so it is placed rather than silently dropped.
    //
    // A phase caught in a dependency cycle cannot become ready and is therefore absent from the result.
    // Callers reject cycles before sorting (RenderGraphBuilder::ValidateDependencies).
    inline std::vector<RenderPhaseID> OrderRenderPhases( const std::set<RenderPhaseID>&    presentPhases,
                                                         const RenderPhaseDependencies&    dependencies,
                                                         const std::vector<RenderPhaseID>& declarationOrder )
    {
        std::set<RenderPhaseID> allPhases = presentPhases;
        for ( const auto& [dependent, prereqs] : dependencies )
        {
            allPhases.insert( dependent );
            allPhases.insert( prereqs.begin(), prereqs.end() );
        }

        std::map<RenderPhaseID, std::size_t> declRank;
        for ( std::size_t i = 0; i < declarationOrder.size(); ++i )
            declRank.emplace( declarationOrder[i], i );

        // The tie-break key: declared phases in declaration order, undeclared ones after them by ID.
        const auto priorityOf = [&declRank, &declarationOrder]( RenderPhaseID phase )
        {
            const auto it = declRank.find( phase );
            if ( it != declRank.end() )
                return std::pair<std::size_t, RenderPhaseID>( it->second, 0 );
            return std::pair<std::size_t, RenderPhaseID>( declarationOrder.size(), phase );
        };

        std::map<RenderPhaseID, int>                        inDegree;
        std::map<RenderPhaseID, std::vector<RenderPhaseID>> dependents;
        for ( RenderPhaseID phase : allPhases )
            inDegree.emplace( phase, 0 );

        for ( const auto& [dependent, prereqs] : dependencies )
        {
            for ( RenderPhaseID prereq : prereqs )
            {
                dependents[prereq].push_back( dependent );
                ++inDegree[dependent];
            }
        }

        // A sorted ready-set, not a FIFO queue: with a queue the output would depend on the order in
        // which predecessors happened to release their dependents, which is the walk order of whatever
        // container held the edges.
        std::set<std::pair<std::pair<std::size_t, RenderPhaseID>, RenderPhaseID>> ready;
        for ( const auto& [phase, degree] : inDegree )
        {
            if ( degree == 0 )
                ready.emplace( priorityOf( phase ), phase );
        }

        std::vector<RenderPhaseID> order;
        order.reserve( allPhases.size() );
        while ( !ready.empty() )
        {
            const RenderPhaseID current = ready.begin()->second;
            ready.erase( ready.begin() );
            order.push_back( current );

            const auto it = dependents.find( current );
            if ( it == dependents.end() )
                continue;

            for ( RenderPhaseID dependent : it->second )
            {
                if ( --inDegree[dependent] == 0 )
                    ready.emplace( priorityOf( dependent ), dependent );
            }
        }

        return order;
    }

    // Returns indices into `keys`, in execution order: by position of the pass's phase in `phaseOrder`,
    // then by registration index. The final fall-back to the key's own position
    // keeps the result total even if a caller hands in duplicate registration indices — std::sort leaves
    // equal elements in an unspecified order, and "unspecified" is the bug this function exists to kill.
    //
    // A pass whose phase is missing from `phaseOrder` is placed after all ordered phases (by phase ID)
    // rather than dropped: losing a pass silently is worse than drawing it late.
    inline std::vector<std::size_t> OrderRenderPasses( const std::vector<RenderPassSortKey>& keys,
                                                       const std::vector<RenderPhaseID>&     phaseOrder )
    {
        std::map<RenderPhaseID, std::size_t> phaseRank;
        for ( std::size_t i = 0; i < phaseOrder.size(); ++i )
            phaseRank.emplace( phaseOrder[i], i );

        const auto rankOf = [&phaseRank, &phaseOrder]( RenderPhaseID phase )
        {
            const auto it = phaseRank.find( phase );
            if ( it != phaseRank.end() )
                return std::pair<std::size_t, RenderPhaseID>( it->second, 0 );
            return std::pair<std::size_t, RenderPhaseID>( phaseOrder.size(), phase );
        };

        std::vector<std::size_t> indices( keys.size() );
        for ( std::size_t i = 0; i < keys.size(); ++i )
            indices[i] = i;

        std::sort( indices.begin(), indices.end(),
                   [&keys, &rankOf]( std::size_t lhs, std::size_t rhs )
                   {
                       const auto lhsRank = rankOf( keys[lhs].Phase );
                       const auto rhsRank = rankOf( keys[rhs].Phase );
                       return std::tie( lhsRank, keys[lhs].RegistrationIndex, lhs ) <
                              std::tie( rhsRank, keys[rhs].RegistrationIndex, rhs );
                   } );

        return indices;
    }

} // namespace Desert::Graphic
