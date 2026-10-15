#include "ViewSettings.hpp"

#include <Engine/ECS/AtmosphereSunLight.hpp>
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

        glm::vec4 Lerp( const glm::vec4& a, const glm::vec4& b, float w )
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
        target.EnableSSAO               = volume.EnableSSAO;
        target.GlobalIlluminationMethod = volume.GlobalIlluminationMethod;
        target.ReflectionsMethod        = volume.ReflectionsMethod;
        target.ShadowsMethod            = volume.ShadowsMethod;
        target.ContactShadows           = volume.ContactShadows;
        target.DistanceFieldShadows     = volume.DistanceFieldShadows;
        target.Tonemapper          = volume.Tonemapper;
        target.AutoExposure        = volume.AutoExposure;
        // A texture has no value between two (UE IF_PP: the volume's mask replaces the one under it).
        target.AutoExposureMeterMask = volume.AutoExposureMeterMask;
        target.EnableBloom         = volume.EnableBloom;
        target.EnableLensFlare     = volume.EnableLensFlare;
        target.LensFlareGhostCount = volume.LensFlareGhostCount;

        target.GIIntensity       = Lerp( target.GIIntensity, volume.GIIntensity, weight );
        target.SSRIntensity      = Lerp( target.SSRIntensity, volume.SSRIntensity, weight );
        target.SSRMaxDistance    = Lerp( target.SSRMaxDistance, volume.SSRMaxDistance, weight );
        target.ManualExposureEV100 = Lerp( target.ManualExposureEV100, volume.ManualExposureEV100, weight );
        target.Gamma             = Lerp( target.Gamma, volume.Gamma, weight );
        target.WhitePoint        = Lerp( target.WhitePoint, volume.WhitePoint, weight );
        target.WhiteTemp           = Lerp( target.WhiteTemp, volume.WhiteTemp, weight );
        target.WhiteTint           = Lerp( target.WhiteTint, volume.WhiteTint, weight );
        target.FilmSlope           = Lerp( target.FilmSlope, volume.FilmSlope, weight );
        target.FilmToe             = Lerp( target.FilmToe, volume.FilmToe, weight );
        target.FilmShoulder        = Lerp( target.FilmShoulder, volume.FilmShoulder, weight );
        target.FilmBlackClip       = Lerp( target.FilmBlackClip, volume.FilmBlackClip, weight );
        target.FilmWhiteClip       = Lerp( target.FilmWhiteClip, volume.FilmWhiteClip, weight );
        target.BlueCorrection      = Lerp( target.BlueCorrection, volume.BlueCorrection, weight );
        target.ExpandGamut         = Lerp( target.ExpandGamut, volume.ExpandGamut, weight );
        target.ToneCurveAmount     = Lerp( target.ToneCurveAmount, volume.ToneCurveAmount, weight );
        target.ColorSaturation     = Lerp( target.ColorSaturation, volume.ColorSaturation, weight );
        target.ColorContrast       = Lerp( target.ColorContrast, volume.ColorContrast, weight );
        target.ColorGamma          = Lerp( target.ColorGamma, volume.ColorGamma, weight );
        target.ColorGain           = Lerp( target.ColorGain, volume.ColorGain, weight );
        target.ColorOffset         = Lerp( target.ColorOffset, volume.ColorOffset, weight );
        target.ColorSaturationShadows =
             Lerp( target.ColorSaturationShadows, volume.ColorSaturationShadows, weight );
        target.ColorContrastShadows = Lerp( target.ColorContrastShadows, volume.ColorContrastShadows, weight );
        target.ColorGammaShadows    = Lerp( target.ColorGammaShadows, volume.ColorGammaShadows, weight );
        target.ColorGainShadows     = Lerp( target.ColorGainShadows, volume.ColorGainShadows, weight );
        target.ColorOffsetShadows   = Lerp( target.ColorOffsetShadows, volume.ColorOffsetShadows, weight );
        target.ColorSaturationMidtones =
             Lerp( target.ColorSaturationMidtones, volume.ColorSaturationMidtones, weight );
        target.ColorContrastMidtones = Lerp( target.ColorContrastMidtones, volume.ColorContrastMidtones, weight );
        target.ColorGammaMidtones    = Lerp( target.ColorGammaMidtones, volume.ColorGammaMidtones, weight );
        target.ColorGainMidtones     = Lerp( target.ColorGainMidtones, volume.ColorGainMidtones, weight );
        target.ColorOffsetMidtones   = Lerp( target.ColorOffsetMidtones, volume.ColorOffsetMidtones, weight );
        target.ColorSaturationHighlights =
             Lerp( target.ColorSaturationHighlights, volume.ColorSaturationHighlights, weight );
        target.ColorContrastHighlights =
             Lerp( target.ColorContrastHighlights, volume.ColorContrastHighlights, weight );
        target.ColorGammaHighlights  = Lerp( target.ColorGammaHighlights, volume.ColorGammaHighlights, weight );
        target.ColorGainHighlights   = Lerp( target.ColorGainHighlights, volume.ColorGainHighlights, weight );
        target.ColorOffsetHighlights = Lerp( target.ColorOffsetHighlights, volume.ColorOffsetHighlights, weight );
        target.ColorCorrectionShadowsMax =
             Lerp( target.ColorCorrectionShadowsMax, volume.ColorCorrectionShadowsMax, weight );
        target.ColorCorrectionHighlightsMin =
             Lerp( target.ColorCorrectionHighlightsMin, volume.ColorCorrectionHighlightsMin, weight );
        target.ColorCorrectionHighlightsMax =
             Lerp( target.ColorCorrectionHighlightsMax, volume.ColorCorrectionHighlightsMax, weight );
        target.AutoExposureBias      = Lerp( target.AutoExposureBias, volume.AutoExposureBias, weight );
        target.AutoExposureSpeedUp   = Lerp( target.AutoExposureSpeedUp, volume.AutoExposureSpeedUp, weight );
        target.AutoExposureSpeedDown = Lerp( target.AutoExposureSpeedDown, volume.AutoExposureSpeedDown, weight );
        // EV100 blends linearly (UE), i.e. the luminance clamps blend geometrically.
        target.AutoExposureMinEV100 = Lerp( target.AutoExposureMinEV100, volume.AutoExposureMinEV100, weight );
        target.AutoExposureMaxEV100 = Lerp( target.AutoExposureMaxEV100, volume.AutoExposureMaxEV100, weight );
        target.AutoExposureLowPercent =
             Lerp( target.AutoExposureLowPercent, volume.AutoExposureLowPercent, weight );
        target.AutoExposureHighPercent =
             Lerp( target.AutoExposureHighPercent, volume.AutoExposureHighPercent, weight );
        target.BloomThreshold    = Lerp( target.BloomThreshold, volume.BloomThreshold, weight );
        target.BloomIntensity    = Lerp( target.BloomIntensity, volume.BloomIntensity, weight );
        target.LensDispersion    = Lerp( target.LensDispersion, volume.LensDispersion, weight );
        target.DepthOfFieldFocalDistance =
             Lerp( target.DepthOfFieldFocalDistance, volume.DepthOfFieldFocalDistance, weight );
        target.DepthOfFieldFstop = Lerp( target.DepthOfFieldFstop, volume.DepthOfFieldFstop, weight );
        target.DepthOfFieldSensorWidth =
             Lerp( target.DepthOfFieldSensorWidth, volume.DepthOfFieldSensorWidth, weight );
        target.DepthOfFieldMaxBokehSize =
             Lerp( target.DepthOfFieldMaxBokehSize, volume.DepthOfFieldMaxBokehSize, weight );
        target.MotionBlurAmount    = Lerp( target.MotionBlurAmount, volume.MotionBlurAmount, weight );
        target.MotionBlurMax       = Lerp( target.MotionBlurMax, volume.MotionBlurMax, weight );
        target.MotionBlurTargetFPS = Lerp( target.MotionBlurTargetFPS, volume.MotionBlurTargetFPS, weight );

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

        // The shadow settings are the SUN'S — the same light the sky stands on and the deferred pass shades
        // with (ECS::PickAtmosphereSun), not whichever directional light the view happens to visit first.
        if ( const auto sun = ECS::PickAtmosphereSun( registry, ECS::SunCandidacy::Lit ).Sun() )
        {
            const ECS::DirectionalLightData& light = registry.get<ECS::DirectionLightComponent>( *sun ).Data;
            result.Shadows.Enabled                 = light.CastShadows;
            result.Shadows.Bias                    = { .BiasCm       = light.ShadowBias,
                                                       .SlopeBiasCm  = light.ShadowSlopeBias,
                                                       .Distribution = light.ShadowCascadeBiasDistribution };
            result.Shadows.CascadeSplitLambda      = light.CascadeSplitLambda;
            result.Shadows.SourceAngleDegrees               = light.SourceAngle;
            result.Shadows.ContactShadowLength              = light.ContactShadowLength;
            result.Shadows.ContactShadowLengthInWorldSpace  = light.ContactShadowLengthInWorldSpace;
            result.Shadows.ContactShadowCastingIntensity    = light.ContactShadowCastingIntensity;
            result.Shadows.ContactShadowNonCastingIntensity = light.ContactShadowNonCastingIntensity;
        }

        return result;
    }
} // namespace Desert::Graphic
