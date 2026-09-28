#include "ViewSettings.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <algorithm>
#include <functional>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        // a*(1-w) + b*w rather than a + (b-a)*w: at w == 1 this is b EXACTLY, which is what makes a scene
        // migrated onto one Unbound volume of weight 1 render byte-for-byte what its Settings block did.
        float Lerp( float a, float b, float w )
        {
            return a * ( 1.0f - w ) + b * w;
        }

        glm::vec3 Lerp( const glm::vec3& a, const glm::vec3& b, float w )
        {
            return a * ( 1.0f - w ) + b * w;
        }
    } // namespace

    float PostProcessVolumeWeight( const ECS::PostProcessVolumeData& volume, const glm::vec3& volumePosition,
                                   const std::optional<glm::vec3>& viewPosition )
    {
        if ( volume.BlendWeight <= 0.0f )
            return 0.0f;
        if ( volume.Unbound )
            return volume.BlendWeight;
        if ( !viewPosition )
            return 0.0f;

        const glm::vec3 outside  = glm::max( glm::abs( *viewPosition - volumePosition ) - volume.Extent, 0.0f );
        const float     distance = glm::length( outside );
        if ( distance <= 0.0f )
            return volume.BlendWeight;
        if ( volume.BlendRadius <= 0.0f || distance >= volume.BlendRadius )
            return 0.0f;
        return volume.BlendWeight * ( 1.0f - distance / volume.BlendRadius );
    }

    void BlendPostProcessSettings( Core::PostProcessSettings& target, const Core::PostProcessSettings& volume,
                                   float weight )
    {
        // Taken whole: a switch, a mode or a count has no value between its two ends.
        target.EnableSSAO          = volume.EnableSSAO;
        target.GlobalIllumination  = volume.GlobalIllumination;
        target.EnableSSR           = volume.EnableSSR;
        target.Tonemapper          = volume.Tonemapper;
        target.AutoExposure        = volume.AutoExposure;
        target.EnableBloom         = volume.EnableBloom;
        target.EnableLensFlare     = volume.EnableLensFlare;
        target.LensFlareGhostCount = volume.LensFlareGhostCount;

        target.GIIntensity       = Lerp( target.GIIntensity, volume.GIIntensity, weight );
        target.SSRIntensity      = Lerp( target.SSRIntensity, volume.SSRIntensity, weight );
        target.SSRMaxDistance    = Lerp( target.SSRMaxDistance, volume.SSRMaxDistance, weight );
        target.Exposure          = Lerp( target.Exposure, volume.Exposure, weight );
        target.Gamma             = Lerp( target.Gamma, volume.Gamma, weight );
        target.WhitePoint        = Lerp( target.WhitePoint, volume.WhitePoint, weight );
        target.AutoExposureKey   = Lerp( target.AutoExposureKey, volume.AutoExposureKey, weight );
        target.AutoExposureSpeed = Lerp( target.AutoExposureSpeed, volume.AutoExposureSpeed, weight );
        target.AutoExposureMin   = Lerp( target.AutoExposureMin, volume.AutoExposureMin, weight );
        target.AutoExposureMax   = Lerp( target.AutoExposureMax, volume.AutoExposureMax, weight );
        target.BloomThreshold    = Lerp( target.BloomThreshold, volume.BloomThreshold, weight );
        target.BloomIntensity    = Lerp( target.BloomIntensity, volume.BloomIntensity, weight );
        target.LensDispersion    = Lerp( target.LensDispersion, volume.LensDispersion, weight );

        target.LensFlareIntensity    = Lerp( target.LensFlareIntensity, volume.LensFlareIntensity, weight );
        target.LensFlareTint         = Lerp( target.LensFlareTint, volume.LensFlareTint, weight );
        target.LensFlareThreshold    = Lerp( target.LensFlareThreshold, volume.LensFlareThreshold, weight );
        target.LensFlareGhostSpacing = Lerp( target.LensFlareGhostSpacing, volume.LensFlareGhostSpacing, weight );
        target.LensFlareGhostSizeNear =
             Lerp( target.LensFlareGhostSizeNear, volume.LensFlareGhostSizeNear, weight );
        target.LensFlareGhostSizeFar = Lerp( target.LensFlareGhostSizeFar, volume.LensFlareGhostSizeFar, weight );
        target.LensFlareGhostTintInner =
             Lerp( target.LensFlareGhostTintInner, volume.LensFlareGhostTintInner, weight );
        target.LensFlareGhostTintOuter =
             Lerp( target.LensFlareGhostTintOuter, volume.LensFlareGhostTintOuter, weight );
        target.LensFlareHaloIntensity =
             Lerp( target.LensFlareHaloIntensity, volume.LensFlareHaloIntensity, weight );
        target.LensFlareHaloRadius = Lerp( target.LensFlareHaloRadius, volume.LensFlareHaloRadius, weight );
        target.LensFlareStreakIntensity =
             Lerp( target.LensFlareStreakIntensity, volume.LensFlareStreakIntensity, weight );
        target.LensFlareStreakLength = Lerp( target.LensFlareStreakLength, volume.LensFlareStreakLength, weight );
        target.LensFlareStreakAngle  = Lerp( target.LensFlareStreakAngle, volume.LensFlareStreakAngle, weight );
        target.LensFlareChromaShift  = Lerp( target.LensFlareChromaShift, volume.LensFlareChromaShift, weight );
    }

    FinalViewSettings ResolveViewSettings( const entt::registry&           registry,
                                           const std::optional<glm::vec3>& viewPosition )
    {
        FinalViewSettings result;

        struct Contribution
        {
            float                                                   Priority;
            float                                                   Weight;
            std::reference_wrapper<const Core::PostProcessSettings> Settings;
        };
        std::vector<Contribution> contributions;

        const auto volumes = registry.view<const ECS::PostProcessVolumeComponent, const ECS::TransformComponent>();
        for ( const entt::entity entity : volumes )
        {
            if ( ECS::IsHidden( registry, entity ) )
                continue;
            const ECS::PostProcessVolumeData& volume =
                 volumes.get<const ECS::PostProcessVolumeComponent>( entity ).Data;
            const ECS::TransformComponent& transform = volumes.get<const ECS::TransformComponent>( entity );
            const float weight = PostProcessVolumeWeight( volume, transform.Translation, viewPosition );
            if ( weight > 0.0f )
                contributions.push_back(
                     { volume.Priority, std::min( weight, 1.0f ), std::cref( volume.Settings ) } );
        }

        // Stable: two volumes of one priority keep the registry's order, so the result does not flicker
        // between frames on a tie.
        std::stable_sort( contributions.begin(), contributions.end(),
                          []( const Contribution& a, const Contribution& b ) { return a.Priority < b.Priority; } );
        for ( const Contribution& c : contributions )
            BlendPostProcessSettings( result.Post, c.Settings.get(), c.Weight );

        const auto lights = registry.view<const ECS::DirectionLightComponent, const ECS::TransformComponent>();
        for ( const entt::entity entity : lights )
        {
            if ( ECS::IsHidden( registry, entity ) )
                continue;
            if ( !ECS::Rules::DirectionalLightTravel(
                      lights.get<const ECS::TransformComponent>( entity ).Translation ) )
                continue;
            const ECS::DirectionalLightData& light = lights.get<const ECS::DirectionLightComponent>( entity ).Data;
            result.Shadows.Enabled                 = light.CastShadows;
            result.Shadows.Bias                    = light.ShadowBias;
            result.Shadows.CascadeSplitLambda      = light.CascadeSplitLambda;
            break;
        }

        return result;
    }
} // namespace Desert::Graphic
