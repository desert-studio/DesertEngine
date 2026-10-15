#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/Public/FoliageType.h (UFoliageType::CullDistance, an
// FInt32Interval: instances fade between Min and Max and are culled beyond Max; Max = 0 never culls), adapted:
// UE fades with a per-pixel dither in the material; here the fade drops a per-INSTANCE share on the CPU, in
// the ISM loops that already cull by frustum. A stable hash of the instance's index is its fade threshold, so
// the thinning grows smoothly with distance, never flickers while the camera stands still, and needs no new
// shader input or pass. Units are centimetres.

#include <Common/Core/Math/AABB.hpp>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Graphic
{
    /// A foliage type's CullDistance as the renderer reads it (cm from the camera). Max <= 0: never culled.
    struct InstanceCullDistance
    {
        float Min = 0.0f;
        float Max = 0.0f;

        [[nodiscard]] bool Culls() const
        {
            return Max > 0.0f;
        }
    };

    /// The cull an ISM's instances get: the foliage type's fade, clipped to the component's MaxDrawDistance (UE's
    /// LDMaxDrawDistance), so neither limit is lost when both are set. maxDrawDistance <= 0: the fade as it is.
    [[nodiscard]] inline InstanceCullDistance ClipToMaxDrawDistance( const InstanceCullDistance& fade,
                                                                     float                       maxDrawDistance )
    {
        if ( maxDrawDistance <= 0.0f )
            return fade;
        const float max = fade.Culls() ? std::min( fade.Max, maxDrawDistance ) : maxDrawDistance;
        return { std::min( fade.Min, max ), max };
    }

    /// The cull as seen through a lens: both ends stretched by Graphic::DrawDistanceScale, so a narrow FOV
    /// draws a foliage field farther out, as it does every MaxDrawDistance.
    [[nodiscard]] inline InstanceCullDistance ScaledByLens( const InstanceCullDistance& cull, float scale )
    {
        return { cull.Min * scale, cull.Max * scale };
    }

    /// The instance's place in the fade, in [0, 1): an instance is dropped once the fade has advanced past it.
    /// A hash of the index and not of the position, so moving an instance does not change whether it fades.
    [[nodiscard]] inline float InstanceFadeThreshold( uint32_t index )
    {
        // PCG output permutation (O'Neill) over the index: consecutive indices land far apart in [0, 1).
        uint32_t state = index * 747796405u + 2891336453u;
        uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
        word           = ( word >> 22u ) ^ word;
        return static_cast<float>( word >> 8u ) * ( 1.0f / 16777216.0f );
    }

    /// Whether instance @p index at @p instancePosition is drawn from @p viewPosition. Nearer than Min: always.
    /// From Max on: never. Between: the fade t = (d - Min) / (Max - Min) drops every instance whose threshold
    /// lies below t, so the share drawn falls linearly from all to none across the band.
    [[nodiscard]] inline bool KeepsInstanceAtDistance( const InstanceCullDistance& cull, uint32_t index,
                                                       const glm::vec3& instancePosition,
                                                       const glm::vec3& viewPosition )
    {
        if ( !cull.Culls() )
            return true;
        const glm::vec3 offset     = instancePosition - viewPosition;
        const float     distanceSq = glm::dot( offset, offset );
        if ( distanceSq >= cull.Max * cull.Max )
            return false;
        if ( distanceSq <= cull.Min * cull.Min )
            return true;
        const float fade = ( std::sqrt( distanceSq ) - cull.Min ) / ( cull.Max - cull.Min );
        return InstanceFadeThreshold( index ) >= fade;
    }

    /**
     * @brief Where one ISM instance can cast or be seen from, before any frustum is asked: nullopt when the
     * cull distance measured from @p cullOrigin drops it, otherwise the box its geometry can reach.
     *
     * The box the WIND can reach, not the authored one: a swaying tip leaves the mesh's box by up to Strength,
     * and a test on the authored box would drop the instance - and its shadow - while its tip is still on
     * screen (UE widens the bounds for a WPO the same way). An empty box when the mesh has no extent: it is
     * in every view (IsVisibleInView's rule), never in none.
     *
     * ONE step for the geometry pass (CollectIsmInstances) and the shadow casters' gather
     * (MeshRenderer::GatherShadowCasters), so an instance the camera does not keep is not a caster either.
     */
    [[nodiscard]] inline std::optional<Common::Math::AABB>
    IsmInstanceReach( const InstanceCullDistance& lensCull, uint32_t index, const glm::mat4& instanceTransform,
                      const Common::Math::AABB& localBounds, const InstanceWind& wind,
                      const glm::vec3& cullOrigin )
    {
        if ( !KeepsInstanceAtDistance( lensCull, index, glm::vec3( instanceTransform[3] ), cullOrigin ) )
            return std::nullopt;
        if ( Geometry::IsEmpty( localBounds ) )
            return localBounds;
        return WindExpandedBounds( Geometry::TransformBounds( instanceTransform, localBounds ), wind );
    }

    /**
     * @brief The instances of one ISM a pass draws, and the LOD level of each.
     *
     * The frustum and the LOD view are the pass's, but the cull distance is measured from @p cullView, which
     * every caller passes as the MAIN camera's view, and stretched by that view's lens (DrawDistanceScale).
     */
    inline void CollectIsmInstances( const std::vector<glm::mat4>& transforms,
                                     const Common::Math::AABB& localBounds, const Core::Frustum& frustum,
                                     const InstanceCullDistance& cull, const InstanceWind& wind,
                                     const Geometry::LODView& cullView, const Geometry::LODView& lodView,
                                     std::span<const float> lodScreenSizes, std::vector<glm::mat4>& visible,
                                     std::vector<uint32_t>& levels )
    {
        visible.clear();
        levels.clear();
        const InstanceCullDistance lensCull = ScaledByLens( cull, DrawDistanceScale( cullView ) );
        for ( std::size_t i = 0; i < transforms.size(); ++i )
        {
            const glm::mat4& instanceTransform = transforms[i];
            const std::optional<Common::Math::AABB> reach             = IsmInstanceReach(
                 lensCull, static_cast<uint32_t>( i ), instanceTransform, localBounds, wind, cullView.Origin );
            if ( !reach || ( !Geometry::IsEmpty( *reach ) && !frustum.Intersects( *reach ) ) )
                continue;
            visible.push_back( instanceTransform );
            levels.push_back(
                 Geometry::SelectLODFromBounds( instanceTransform, localBounds, lodView, lodScreenSizes, -1, 0 ) );
        }
    }
} // namespace Desert::Graphic
