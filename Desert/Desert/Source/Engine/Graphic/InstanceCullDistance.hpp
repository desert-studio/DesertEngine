#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/Public/FoliageType.h (UFoliageType::CullDistance, an
// FInt32Interval: instances fade between Min and Max and are culled beyond Max; Max = 0 never culls), adapted:
// UE fades with a per-pixel dither in the material; here the fade drops a per-INSTANCE share on the CPU, in
// the ISM loops that already cull by frustum. A stable hash of the instance's index is its fade threshold, so
// the thinning grows smoothly with distance, never flickers while the camera stands still, and needs no new
// shader input or pass. Units are centimetres.

#include <Common/Core/Math/AABB.hpp>
#include <Common/Settings/Scalability.hpp>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Graphic
{
    /// A foliage type's CullDistance as the renderer reads it (cm from the camera). Max <= 0: never culled.
    struct InstanceCullDistance
    {
        float Min = 0.0f;
        float Max = 0.0f;
        // Whether the field has a foliage type at all: only then do the ViewDistance foliage rows
        // (FoliageQualityFor) reach it — an ISM that is not foliage keeps its MaxDrawDistance and every instance.
        bool FoliageType = false;
        // The share of the field drawn at all, (0, 1] (UE foliage.DensityScale): an instance whose fade threshold
        // lies at or above it is never drawn, and the distance fade thins the rest. 1: every instance.
        float Density = 1.0f;

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
        return { std::min( fade.Min, max ), max, fade.FoliageType, fade.Density };
    }

    /// The cull as seen through a lens: both ends stretched by Graphic::DrawDistanceScale, so a narrow FOV
    /// draws a foliage field farther out, as it does every MaxDrawDistance.
    [[nodiscard]] inline InstanceCullDistance ScaledByLens( const InstanceCullDistance& cull, float scale )
    {
        return { cull.Min * scale, cull.Max * scale, cull.FoliageType, cull.Density };
    }

    /// What the ViewDistance level asks of every foliage field (UE r.ViewDistanceScale on a foliage type's
    /// CullDistance, and foliage.DensityScale). 1/1 at High, Epic and Cinematic: the authored fields, unchanged.
    struct FoliageQualityScale
    {
        float CullDistanceScale = 1.0f; // both ends of the CullDistance fade
        float Density           = 1.0f; // share of a field's instances drawn at all
    };

    [[nodiscard]] inline FoliageQualityScale FoliageQualityFor( int32_t cullDistancePercent,
                                                                int32_t densityPercent )
    {
        return { static_cast<float>( cullDistancePercent ) / 100.0f,
                 static_cast<float>( densityPercent ) / 100.0f };
    }

    /// What a view's resolved quality asks of foliage (SceneRenderer::SetQuality is the one caller).
    [[nodiscard]] inline FoliageQualityScale
    FoliageQualityFor( const Common::Scalability::ResolvedQuality& quality )
    {
        using Common::Scalability::Parameter;
        return FoliageQualityFor( quality.As<int32_t>( Parameter::FoliageCullDistancePercent ),
                                  quality.As<int32_t>( Parameter::FoliageDensityPercent ) );
    }

    /// The cull a foliage field gets at this quality: both ends scaled, the density multiplied. A cull that is
    /// not a foliage type's comes back as it is (UE scales only foliage by foliage.DensityScale).
    [[nodiscard]] inline InstanceCullDistance ScaledForQuality( const InstanceCullDistance& cull,
                                                                const FoliageQualityScale&  quality )
    {
        if ( !cull.FoliageType )
            return cull;
        return { cull.Min * quality.CullDistanceScale, cull.Max * quality.CullDistanceScale, true,
                 cull.Density * quality.Density };
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

    /// Whether instance @p index at @p instancePosition is drawn from @p viewPosition. An instance whose threshold
    /// lies at or above Density: never (InstanceCullDistance::Density). Otherwise nearer than Min: always; from
    /// Max on: never; between, the fade t = (d - Min) / (Max - Min) drops every kept instance whose threshold lies
    /// below t * Density, so the share drawn falls linearly from Density to none across the band.
    [[nodiscard]] inline bool KeepsInstanceAtDistance( const InstanceCullDistance& cull, uint32_t index,
                                                       const glm::vec3& instancePosition,
                                                       const glm::vec3& viewPosition )
    {
        // Density first: the dropped share is the instances whose threshold lies at or above it, so lowering the
        // density never brings back an instance the fade had dropped.
        const float threshold = InstanceFadeThreshold( index );
        if ( threshold >= cull.Density )
            return false;
        if ( !cull.Culls() )
            return true;
        const glm::vec3 offset     = instancePosition - viewPosition;
        const float     distanceSq = glm::dot( offset, offset );
        if ( distanceSq >= cull.Max * cull.Max )
            return false;
        if ( distanceSq <= cull.Min * cull.Min )
            return true;
        const float fade = ( std::sqrt( distanceSq ) - cull.Min ) / ( cull.Max - cull.Min );
        // Among the kept share the fade runs as before: the share drawn is Density * (1 - fade).
        return threshold >= fade * cull.Density;
    }

    /**
     * @brief The instances of one ISM a pass draws, and the LOD level of each.
     *
     * ONE function for the geometry pass and every shadow cascade, so an instance the camera does not see is
     * not a caster either: the frustum and the LOD view are the pass's, but the cull distance is measured
     * from @p cullView, which every caller passes as the MAIN camera's view — a cascade has no position of
     * its own that the fade could be measured from without leaving shadows of absent trees — and stretched by
     * that view's lens (DrawDistanceScale).
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
            if ( !KeepsInstanceAtDistance( lensCull, static_cast<uint32_t>( i ), glm::vec3( instanceTransform[3] ),
                                           cullView.Origin ) )
                continue;
            // Tested on the box the WIND can reach, not the authored one: a swaying tip leaves the mesh's box
            // by up to Strength, and a test on the authored box would drop the instance - and its shadow -
            // while its tip is still on screen (UE widens the bounds for a WPO the same way).
            if ( !Geometry::IsEmpty( localBounds ) &&
                 !frustum.Intersects(
                      WindExpandedBounds( Geometry::TransformBounds( instanceTransform, localBounds ), wind ) ) )
                continue;
            visible.push_back( instanceTransform );
            levels.push_back(
                 Geometry::SelectLODFromBounds( instanceTransform, localBounds, lodView, lodScreenSizes, -1, 0 ) );
        }
    }
} // namespace Desert::Graphic
