#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // UE's Exponential Height Fog, parameter for parameter (Docs/Sky/UE_SKYATMOSPHERE_RESEARCH.md
    // section 3.2): a participating medium whose density falls exponentially with height, integrated in
    // CLOSED FORM per pixel — no march, no LUT — by the HeightFog pass
    // (Graphic/Systems/Scene/Fog/HeightFogRenderer). Parameter names, grouping and every default below
    // are UExponentialHeightFogComponent's, so a UE-calibrated fog transplants number for number; the
    // maths that consumes them is reimplemented from the formulas in the research doc, never from Epic's
    // shader text.
    //
    // THE FOG HEIGHT IS NOT A FIELD. It is the Y of this entity's TransformComponent, exactly as UE takes
    // it from the component transform — a second authorable height here would be the duplicated state
    // that lets the fog floor disagree with the entity that owns it. The second layer's HeightOffset is
    // relative to that same Y.
    //
    // UNITS. Distances are world units (centimetres, Length). FogDensity and the height falloffs keep
    // UE's authored semantics — "per 1000 cm", so 0.02 and 0.2 mean in this engine exactly what they mean
    // in UE — and are converted to per-kilometre coefficients once, in Graphic::PackFogParams
    // (Graphic/Fog/FogPayload.hpp), following the "kilometres once, inside the shader" rule.
    //
    // DELIBERATELY NOT PORTED from UExponentialHeightFogComponent (research doc section 4, non-goals):
    // the Inscattering Texture cubemap group, FSSS and EndDistance. From UE's VOLUMETRIC FOG group,
    // VolumetricFogStaticLightingScatteringIntensity (the engine has no baked lighting to scale) and
    // OverrideLightColorsWithFogInscatteringColors (an artist cheat that replaces every light's colour)
    // are not declared: a field with no reader is a dead knob, and the contract forbids shipping one.
    //
    // VOLUMETRIC FOG (UE "Volumetric Fog" group, VolumetricFogRenderer): the same medium, sampled per FROXEL
    // of a camera-aligned grid instead of integrated in closed form, so it can be LIT - by the sun through
    // its cascades (light shafts), by every point and spot light through the view's light grid and the
    // local shadow atlas (halos), and by the sky. Inside VolumetricFogDistance the grid replaces the
    // analytic fog; beyond it the closed form carries on, exactly as UE splits them.
    struct ExponentialHeightFogData
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Exponential Height Fog" ), Summary,
                  Tooltip( "Master switch. Off dispatches nothing: a scene with the fog disabled pays "
                           "zero GPU cost, exactly like a scene without the component." ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Fog Density" ), Category( "Exponential Height Fog" ), Range( 0.0f, 0.5f ), Summary,
                  Tooltip( "Global density of the fog medium, UE's units (per 1000 cm at the fog "
                           "height). UE's default is 0.02: visibility of a few kilometres at ground "
                           "level." ) )
        float FogDensity = 0.02f;

        PROPERTY( DisplayName( "Fog Height Falloff" ), Category( "Exponential Height Fog" ), Range( 0.0f, 2.0f ),
                  Tooltip( "How fast the density thins with altitude (per 1000 cm). At UE's default 0.2 "
                           "the density halves every 50 m; 0 is a uniform medium with no height "
                           "dependence at all." ) )
        float FogHeightFalloff = 0.2f;

        PROPERTY( DisplayName( "Fog Inscattering Color" ), Category( "Exponential Height Fog" ), Color,
                  Tooltip( "Colour the fog scatters toward the camera. The sky's ambient contribution "
                           "(scaled below) is added on top of this." ) )
        glm::vec3 FogInscatteringLuminance = { 0.447f, 0.638f, 1.0f };

        PROPERTY( DisplayName( "Sky Atmosphere Ambient Contribution Color Scale" ),
                  Category( "Exponential Height Fog" ), Color, Advanced,
                  Tooltip( "Scales the sky-ambient light added to the fog's inscattering colour. White "
                           "is the full contribution; black removes the sky from the fog entirely." ) )
        glm::vec3 SkyAtmosphereAmbientContributionColorScale = { 1.0f, 1.0f, 1.0f };

        PROPERTY( DisplayName( "Fog Max Opacity" ), Category( "Exponential Height Fog" ), Range( 0.0f, 1.0f ),
                  Tooltip( "The most the fog may ever obscure. 1 lets dense fog swallow the scene "
                           "completely; 0.5 always leaves half of the scene visible however thick the "
                           "medium gets." ) )
        float FogMaxOpacity = 1.0f;

        PROPERTY( DisplayName( "Start Distance" ), Category( "Exponential Height Fog" ), Length,
                  Range( 0.0f, 5000000.0f ),
                  Tooltip( "Distance from the camera at which the fog begins. The excluded near segment "
                           "contributes nothing to the integral - not a fade, a true exclusion." ) )
        float StartDistance = 0.0f;

        PROPERTY( DisplayName( "Fog Cutoff Distance" ), Category( "Exponential Height Fog" ), Length,
                  Range( 0.0f, 20000000.0f ), Advanced,
                  Tooltip( "Beyond this distance the fog is removed entirely. 0 disables the cutoff "
                           "(UE's default)." ) )
        float FogCutoffDistance = 0.0f;

        // ---- Second fog layer (UE's SecondFogData). A second exponential term summed with the first:
        // e.g. a thin valley layer under a tall atmospheric one. Density 0 - UE's default - makes it
        // free: the closed form of an all-zero layer is exactly zero.
        PROPERTY( DisplayName( "Fog Density" ), Category( "Second Fog Layer" ), Range( 0.0f, 0.5f ),
                  Tooltip( "Density of the second fog layer, same units as the first. UE's default 0 "
                           "switches the layer off." ) )
        float SecondFogDensity = 0.0f;

        PROPERTY( DisplayName( "Fog Height Falloff" ), Category( "Second Fog Layer" ), Range( 0.0f, 2.0f ),
                  Tooltip( "Height falloff of the second fog layer, same units as the first." ) )
        float SecondFogHeightFalloff = 0.2f;

        PROPERTY( DisplayName( "Fog Height Offset" ), Category( "Second Fog Layer" ), Length,
                  Range( -10000000.0f, 10000000.0f ),
                  Tooltip( "Height of the second layer relative to the first (the entity's own Y). "
                           "Negative sinks it into a valley below the main fog floor." ) )
        float SecondFogHeightOffset = 0.0f;

        // ---- Directional inscattering: the bright cone around the sun that fog gets when you look
        // toward the light - an artist-friendly stand-in for the forward-scattering phase function.
        PROPERTY( DisplayName( "Directional Inscattering Exponent" ), Category( "Directional Inscattering" ),
                  Range( 1.0f, 64.0f ),
                  Tooltip( "Sharpness of the bright lobe around the sun. Higher is tighter; UE's "
                           "default 4 is a broad glow." ) )
        float DirectionalInscatteringExponent = 4.0f;

        PROPERTY( DisplayName( "Directional Inscattering Start Distance" ), Category( "Directional Inscattering" ),
                  Length, Range( 0.0f, 5000000.0f ),
                  Tooltip( "Distance from the camera before the directional lobe starts to build up, so "
                           "near geometry does not catch the sun-glow of fog that is not between it and "
                           "the camera." ) )
        float DirectionalInscatteringStartDistance = 10000.0f;

        PROPERTY( DisplayName( "Directional Inscattering Color" ), Category( "Directional Inscattering" ), Color,
                  Tooltip( "Authored colour added to the lobe. UE defaults it to black because the sun's "
                           "own post-transmittance illuminance usually supplies the light; ours comes "
                           "from the Sky Atmosphere's sun the same way." ) )
        glm::vec3 DirectionalInscatteringLuminance = { 0.0f, 0.0f, 0.0f };

        // ---- Volumetric fog (UE: bEnableVolumetricFog and the group under it).
        PROPERTY( DisplayName( "Volumetric Fog" ), Category( "Volumetric Fog" ), Summary,
                  Tooltip( "Evaluates the fog per froxel, lit by the sun's shadows, every point and spot "
                           "light and the sky: light shafts and halos around lamps. Also needs the machine's "
                           "Effects quality to allow it (High and above, UE's sg.EffectsQuality)." ) )
        bool VolumetricFog = false;

        PROPERTY( DisplayName( "Scattering Distribution" ), Category( "Volumetric Fog" ), Range( -0.9f, 0.9f ),
                  Tooltip( "Henyey-Greenstein anisotropy g of the medium: 0 scatters evenly, toward 0.9 "
                           "light scatters forward, so lamps and the sun glow strongly when looked at." ) )
        float VolumetricFogScatteringDistribution = 0.2f;

        PROPERTY( DisplayName( "Albedo" ), Category( "Volumetric Fog" ), Color,
                  Tooltip( "Fraction of the extinguished light that is scattered rather than absorbed, per "
                           "channel. White fog is (1,1,1)." ) )
        glm::vec3 VolumetricFogAlbedo = { 1.0f, 1.0f, 1.0f };

        PROPERTY( DisplayName( "Emissive" ), Category( "Volumetric Fog" ), Color,
                  Tooltip( "Light the medium emits on its own, per unit of fog extinction." ) )
        glm::vec3 VolumetricFogEmissive = { 0.0f, 0.0f, 0.0f };

        PROPERTY( DisplayName( "Extinction Scale" ), Category( "Volumetric Fog" ), Range( 0.1f, 10.0f ),
                  Tooltip( "Multiplies the height fog's density inside the froxel grid; above 1 the "
                           "volumetric fog is thicker than the analytic fog it continues into." ) )
        float VolumetricFogExtinctionScale = 1.0f;

        PROPERTY( DisplayName( "View Distance" ), Category( "Volumetric Fog" ), Length,
                  Range( 1000.0f, 1000000.0f ),
                  Tooltip( "How far from the camera the froxel grid reaches. The analytic fog takes over "
                           "beyond it. Longer means coarser froxels at the same grid size." ) )
        float VolumetricFogDistance = 6000.0f;

        PROPERTY( DisplayName( "Start Distance" ), Category( "Volumetric Fog" ), Length, Range( 0.0f, 1000000.0f ),
                  Tooltip( "Distance from the camera before the volumetric fog has any density." ) )
        float VolumetricFogStartDistance = 0.0f;

        PROPERTY( DisplayName( "Near Fade In Distance" ), Category( "Volumetric Fog" ), Length,
                  Range( 0.0f, 1000000.0f ),
                  Tooltip( "Over this distance past Start Distance the density fades in from zero, hiding "
                           "the froxels nearest the camera." ) )
        float VolumetricFogNearFadeInDistance = 0.0f;
    };

    struct ExponentialHeightFogComponent
    {
        COMPONENT( Key( "ExponentialHeightFog" ), Block( Data ), Run( SkyAndAtmosphere ) )
        ExponentialHeightFogData Data;
    };
} // namespace Desert::ECS
