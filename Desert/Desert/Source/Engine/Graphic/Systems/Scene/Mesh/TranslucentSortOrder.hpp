#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace Desert::Graphic::System
{
    // THE ORDER THE TRANSLUCENCY PASS DRAWS IN. Blending is not commutative: glass seen through glass is
    // right only when the far pane is composited first and the near one over it. UE's translucency pass
    // answers this with FTranslucentPrimSet::SortPrimitives, and so does this, on the same key:
    //
    //   1. the component's TranslucencySortPriority, ASCENDING — a lower priority draws first, i.e. behind
    //      a higher one whatever their distances (the author's override for the cases distance gets
    //      wrong: a large shell around a small object whose centre is farther away);
    //   2. then the distance from the frame's camera to the centre of the object's WORLD bounds,
    //      DESCENDING — back to front;
    //   3. then submission order — the sort is STABLE, so two objects on the same key keep the order the
    //      queue gave them and the picture does not flicker between frames.
    //
    // A pure function over plain values so the policy is testable without a device
    // (Tests/Engine/TranslucentSortOrder); MeshRenderer::RenderGlassManual is its one caller.
    struct TranslucentSortItem
    {
        glm::vec3 WorldBoundsCenter = glm::vec3( 0.0f );
        int       Priority          = 0; // StaticMeshComponent::TranslucencySortPriority
    };

    /// Indices into @p items, in draw order (first = drawn first = farthest / lowest priority).
    [[nodiscard]] inline std::vector<uint32_t> TranslucentSortOrder( const glm::vec3& cameraPosition,
                                                                     std::span<const TranslucentSortItem> items )
    {
        // Squared distance orders exactly as distance does and needs no square root per object.
        std::vector<float> distance2( items.size() );
        for ( size_t i = 0; i < items.size(); ++i )
        {
            const glm::vec3 d = items[i].WorldBoundsCenter - cameraPosition;
            distance2[i]      = glm::dot( d, d );
        }

        std::vector<uint32_t> order( items.size() );
        std::iota( order.begin(), order.end(), 0u );
        std::stable_sort( order.begin(), order.end(),
                          [&]( uint32_t a, uint32_t b )
                          {
                              if ( items[a].Priority != items[b].Priority )
                                  return items[a].Priority < items[b].Priority;
                              return distance2[a] > distance2[b];
                          } );
        return order;
    }
} // namespace Desert::Graphic::System
