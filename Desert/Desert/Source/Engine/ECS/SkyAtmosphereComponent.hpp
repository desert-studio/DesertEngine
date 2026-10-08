#pragma once

#include <cstdint>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <Engine/Reflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // Size of the equirectangular panorama the sky is baked into for image-based lighting. The bake is a
    // WaitDeviceIdle operation and its cost is paid PER LIVE SceneRenderer (viewport, mesh preview,
    // thumbnail renderer, extra scene views), which is why the size is authored and not a constant.
    enum class SkyEnvironmentResolution : uint8_t
    {
        Low,    // 512x256
        Medium, // 1024x512 - what the engine baked at unconditionally before this was authorable
        High    // 2048x1024
    };

    // Which preset the palette last came from. An ENUM rather than a string on purpose: a string can hold
    // a preset that does not exist (a typo, a renamed preset, a hand-edited scene file) and there is no
    // validation layer between the file and the widget that would catch it. Removing a preset is then a
    // build error instead of a load-time surprise.
    enum class SkyPreset : uint8_t
    {
        Custom, // not a preset: "these values were authored by hand"
        ClearNoon,
        GoldenHour,
        OvercastGrey,
        Night,
        StudioNeutral
    };

    // Which model the sky pass evaluates. An enum and not a bool so a third model (a captured sky, a
    // stylised model) is an enumerator away instead of a second flag that can contradict the first.
    //
    // ArtisticGradient is the engine's original authored-palette sky, and the default: a scene saved
    // before this field existed carries no value for it, rfl::DefaultIfMissing keeps the C++ default, and
    // the scene keeps the exact sky it was authored with. PhysicalAtmosphere is the Hillaire 2020
    // scattering model (EGSR 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering
    // Technique") driven by the physical parameter groups below.
    enum class SkyModel : uint8_t
    {
        ArtisticGradient,
        PhysicalAtmosphere
    };

    // The procedural atmosphere: the sky the engine generates when no HDR cubemap is used. Everything the
    // sky pass, the IBL bake and the time-of-day driver read lives here; SkyboxComponent keeps only the HDR
    // path (its asset handle and intensity).
    //
    // ON THE TWO PAIRS OF SUN NUMBERS. `SunColor` x `SunIntensity` here and `Color` x `Intensity` on
    // DirectionalLightData are NOT the same quantity stored twice:
    //   * these two are the RADIANCE of the sky and of the solar disk - how bright the sun looks IN THE
    //     PICTURE. They are written into the sky's parameter block and into the IBL bake.
    //   * the light's two are the ILLUMINANCE arriving at scene surfaces - how bright the sun is ON THE
    //     GROUND. They are written into DirectionLightsUB and integrated by every lit surface.
    // No code path reads one where it means the other, and neither is derived from the other. The one
    // quantity they genuinely share is DIRECTION, and that has a single owner: the atmosphere sun light's
    // TransformComponent (see DirectionalLightData::AtmosphereSunLight).
    struct SkyAtmosphereData
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Atmosphere" ), Summary )
        bool Enabled = true;

        PROPERTY( DisplayName( "Sky Brightness" ), Category( "Atmosphere" ), Range( 0.0f, 4.0f ), Units( "x" ) )
        float SkyBrightness = 1.0f;

        PROPERTY( DisplayName( "Horizon Falloff" ), Category( "Atmosphere" ), Range( 0.1f, 2.0f ) )
        float HorizonFalloff = 0.85f;

        PROPERTY( DisplayName( "Zenith Color" ), Category( "Sky Color" ), Color )
        glm::vec3 ZenithColor = { 0.08f, 0.26f, 0.70f };

        PROPERTY( DisplayName( "Horizon Color" ), Category( "Sky Color" ), Color )
        glm::vec3 HorizonColor = { 0.50f, 0.66f, 0.92f };

        PROPERTY( DisplayName( "Ground Color" ), Category( "Sky Color" ), Color )
        glm::vec3 GroundColor = { 0.16f, 0.19f, 0.24f };

        PROPERTY( DisplayName( "Night Color" ), Category( "Sky Color" ), Color )
        glm::vec3 NightColor = { 0.010f, 0.020f, 0.050f };

        PROPERTY( DisplayName( "Sun Intensity" ), Category( "Sun" ), Range( 1.0f, 50.0f ), Units( "x" ), Summary,
                  Tooltip( "Brightness of the sky and the solar disk as seen by the camera. Scene surfaces "
                           "are lit by the directional light's own Colour and Intensity, not by this." ) )
        float SunIntensity = 22.0f;

        PROPERTY( DisplayName( "Sun Color" ), Category( "Sun" ), Color,
                  Tooltip( "Tint of the sky and the solar disk as seen by the camera. Scene surfaces are lit "
                           "by the directional light's own Colour and Intensity, not by this." ) )
        glm::vec3 SunColor = { 1.00f, 0.96f, 0.88f };

        // DEGREES and a DIAMETER, unlike the radians-and-radius number this replaces: degrees is what an
        // artist reads, and the default being 2.29 deg - four times the real sun - is worth being able to
        // see. The shaders still take the angular RADIUS in radians; the conversion happens once, in C++.
        PROPERTY( DisplayName( "Sun Angular Diameter" ), Category( "Sun" ), Range( 0.1f, 10.0f ), Units( "deg" ),
                  Tooltip( "Apparent size of the solar disk. The real sun is 0.53 deg." ) )
        float SunAngularDiameter = 2.2918f;

        PROPERTY( DisplayName( "Sun Glow" ), Category( "Sun" ), Range( 0.0f, 5.0f ) )
        float SunGlow = 1.0f;

        PROPERTY( DisplayName( "Sunset Color" ), Category( "Sun" ), Color )
        glm::vec3 SunsetColor = { 1.00f, 0.42f, 0.18f };

        PROPERTY( DisplayName( "Sunset Intensity" ), Category( "Sun" ), Range( 0.0f, 3.0f ) )
        float SunsetIntensity = 1.0f;

        PROPERTY( DisplayName( "Star Intensity" ), Category( "Night Sky" ), Range( 0.0f, 5.0f ) )
        float StarIntensity = 1.0f;

        PROPERTY( DisplayName( "Auto Rebake" ), Category( "Environment Lighting" ), Advanced )
        bool AutoRebakeEnvironment = true;

        PROPERTY( DisplayName( "Rebake Sun Angle" ), Category( "Environment Lighting" ), Range( 0.25f, 45.0f ),
                  Units( "deg" ), Advanced, EditCondition( "AutoRebakeEnvironment" ) )
        float RebakeSunAngleThreshold = 5.0f;

        PROPERTY( DisplayName( "Environment Resolution" ), Category( "Environment Lighting" ), Advanced,
                  Tooltip( "Size of the baked sky cubemap. High costs 32 MiB for the panorama alone, per "
                           "live scene renderer." ) )
        SkyEnvironmentResolution EnvironmentResolution = SkyEnvironmentResolution::Medium;

        PROPERTY( DisplayName( "Preset" ), Category( "Atmosphere" ), ReadOnly,
                  Tooltip( "Which preset the palette came from. Reverts to Custom as soon as any sky colour "
                           "or sun value is edited." ) )
        SkyPreset ActivePreset = SkyPreset::Custom;

        // KILOMETRES, and deliberately not Length: 6360 is a number a human can read and check, while the
        // same value in world units is 636000000. The conversion to centimetres happens once, on the C++
        // side, at the point the value is handed to a shader.
        //
        // The planet CENTRE is derived from this, never authored: it sits PlanetRadius below the world
        // origin along +Y. A second authorable value that must agree with this one is exactly the
        // duplicated state that produces a horizon disagreeing with itself.
        PROPERTY( DisplayName( "Planet Radius" ), Category( "Atmosphere" ), Range( 1.0f, 20000.0f ), Units( "km" ),
                  Tooltip( "Radius of the planet the sky sits on. This is the only planet radius in the "
                           "engine, so nothing can disagree with the sky about where the horizon is." ) )
        float PlanetRadius = 6360.0f;

        // ---- The physical atmosphere (SkyModel::PhysicalAtmosphere) ---------------------------------
        //
        // Parameter names, grouping and every default below are UE's USkyAtmosphereComponent, verbatim
        // (Docs/Sky/UE_SKYATMOSPHERE_RESEARCH.md section 1.7), so a UE-calibrated atmosphere transplants
        // number for number. The maths that consumes them is implemented from the Hillaire 2020 paper.
        //
        // The coefficient fields follow UE's scale-times-colour split: the SCALE carries the magnitude
        // (a per-kilometre coefficient, readable on its own), the COLOUR carries the spectral shape. The
        // two are multiplied into one coefficient exactly once, in MakeSkySettings.

        PROPERTY( DisplayName( "Sky Model" ), Category( "Atmosphere" ),
                  Tooltip( "Artistic Gradient is the authored palette above. Physical Atmosphere is the "
                           "Hillaire 2020 scattering model driven by the physical parameter groups." ) )
        SkyModel Model = SkyModel::ArtisticGradient;

        PROPERTY( DisplayName( "Atmosphere Height" ), Category( "Physical Atmosphere" ), Range( 10.0f, 200.0f ),
                  Units( "km" ),
                  Tooltip( "Height of the atmosphere shell above the planet surface. Earth: 60 km." ) )
        float AtmosphereHeight = 60.0f;

        PROPERTY( DisplayName( "MultiScattering" ), Category( "Physical Atmosphere" ), Range( 0.0f, 2.0f ),
                  Tooltip( "Strength of the multiple-scattering contribution (the light that has bounced "
                           "through the air more than once). 1 is physical; 0 disables it." ) )
        float MultiScatteringFactor = 1.0f;

        PROPERTY( DisplayName( "Ground Albedo" ), Category( "Physical Atmosphere" ), Color,
                  Tooltip( "Average reflectance of the planet surface, as seen by light bouncing under "
                           "the atmosphere. Brightens the sky near the horizon and the multiple "
                           "scattering over bright ground." ) )
        glm::vec3 GroundAlbedo = { 0.401978f, 0.401978f, 0.401978f };

        PROPERTY( DisplayName( "Rayleigh Scattering Scale" ), Category( "Rayleigh" ), Range( 0.0f, 2.0f ),
                  Units( "/km" ),
                  Tooltip( "Magnitude of Rayleigh scattering (air molecules) per kilometre. Earth: "
                           "0.0331." ) )
        float RayleighScatteringScale = 0.0331f;

        PROPERTY( DisplayName( "Rayleigh Scattering" ), Category( "Rayleigh" ), Color,
                  Tooltip( "Spectral shape of Rayleigh scattering; blue-dominant is what makes the day "
                           "sky blue. Multiplied by Rayleigh Scattering Scale." ) )
        glm::vec3 RayleighScattering = { 0.175287f, 0.409607f, 1.0f };

        PROPERTY( DisplayName( "Exp Distribution" ), Category( "Rayleigh" ), Range( 0.1f, 20.0f ), Units( "km" ),
                  Tooltip( "Scale height of the Rayleigh density falloff: density = exp(-altitude / "
                           "this). Earth: 8 km." ) )
        float RayleighExponentialDistribution = 8.0f;

        PROPERTY( DisplayName( "Mie Scattering Scale" ), Category( "Mie" ), Range( 0.0f, 5.0f ), Units( "/km" ),
                  Tooltip( "Magnitude of Mie scattering (aerosols, haze) per kilometre. Earth: "
                           "0.003996." ) )
        float MieScatteringScale = 0.003996f;

        PROPERTY( DisplayName( "Mie Scattering" ), Category( "Mie" ), Color,
                  Tooltip( "Spectral shape of Mie scattering. Aerosols scatter almost neutrally, hence "
                           "white. Multiplied by Mie Scattering Scale." ) )
        glm::vec3 MieScattering = { 1.0f, 1.0f, 1.0f };

        PROPERTY( DisplayName( "Mie Absorption Scale" ), Category( "Mie" ), Range( 0.0f, 5.0f ), Units( "/km" ),
                  Tooltip( "Magnitude of Mie absorption (light the aerosols swallow rather than "
                           "redirect) per kilometre. Earth: 0.000444." ) )
        float MieAbsorptionScale = 0.000444f;

        PROPERTY( DisplayName( "Mie Absorption" ), Category( "Mie" ), Color,
                  Tooltip( "Spectral shape of Mie absorption. Multiplied by Mie Absorption Scale." ) )
        glm::vec3 MieAbsorption = { 1.0f, 1.0f, 1.0f };

        PROPERTY( DisplayName( "Mie Anisotropy" ), Category( "Mie" ), Range( 0.0f, 0.999f ),
                  Tooltip( "Directionality of Mie scattering (the phase-function g). 0 scatters evenly; "
                           "toward 1 the light is thrown forward, which is the bright halo around the "
                           "sun. Earth: 0.8." ) )
        float MieAnisotropy = 0.8f;

        PROPERTY( DisplayName( "Exp Distribution" ), Category( "Mie" ), Range( 0.01f, 20.0f ), Units( "km" ),
                  Tooltip( "Scale height of the Mie density falloff: density = exp(-altitude / this). "
                           "Earth: 1.2 km — haze hugs the ground." ) )
        float MieExponentialDistribution = 1.2f;

        PROPERTY( DisplayName( "Absorption Scale" ), Category( "Absorption" ), Range( 0.0f, 5.0f ), Units( "/km" ),
                  Tooltip( "Magnitude of the ozone-layer absorption per kilometre. Earth: 0.001881. This "
                           "is what keeps the zenith blue instead of drifting green at sunset." ) )
        float OtherAbsorptionScale = 0.001881f;

        PROPERTY( DisplayName( "Absorption" ), Category( "Absorption" ), Color,
                  Tooltip( "Spectral shape of the ozone absorption (green-dominant). Multiplied by "
                           "Absorption Scale." ) )
        glm::vec3 OtherAbsorption = { 0.345561f, 1.0f, 0.045189f };

        PROPERTY( DisplayName( "Tip Altitude" ), Category( "Absorption" ), Range( 0.0f, 100.0f ), Units( "km" ),
                  Tooltip( "Altitude at which the ozone tent distribution peaks. Earth: 25 km." ) )
        float AbsorptionTipAltitude = 25.0f;

        PROPERTY( DisplayName( "Tip Value" ), Category( "Absorption" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Ozone density at the tent's peak; the density falls linearly to zero over "
                           "Tent Width on both sides." ) )
        float AbsorptionTipValue = 1.0f;

        PROPERTY( DisplayName( "Tent Width" ), Category( "Absorption" ), Range( 0.1f, 50.0f ), Units( "km" ),
                  Tooltip( "Distance above and below Tip Altitude over which the ozone density falls to "
                           "zero. Earth: 15 km — the layer spans 10 to 40 km." ) )
        float AbsorptionTentWidth = 15.0f;

        PROPERTY( DisplayName( "Sky Luminance Factor" ), Category( "Art Direction" ), Color,
                  Tooltip( "Art-direction tint on the sky pixels of the physical model only (not on the "
                           "aerial perspective over geometry). White is physical." ) )
        glm::vec3 SkyLuminanceFactor = { 1.0f, 1.0f, 1.0f };

        PROPERTY( DisplayName( "Sky And Aerial Perspective Luminance Factor" ), Category( "Art Direction" ), Color,
                  Tooltip( "Art-direction tint applied inside every scattering integration of the "
                           "physical model — the sky and the aerial perspective together. White is "
                           "physical." ) )
        glm::vec3 SkyAndAerialPerspectiveLuminanceFactor = { 1.0f, 1.0f, 1.0f };

        PROPERTY( DisplayName( "Aerial Perspective View Distance Scale" ), Category( "Art Direction" ),
                  Range( 0.0f, 3.0f ),
                  Tooltip( "Scales how quickly distance haze accumulates on geometry: above 1 the world "
                           "hazes up faster than physics says, below 1 slower." ) )
        float AerialPerspectiveViewDistanceScale = 1.0f;

        PROPERTY( DisplayName( "Aerial Perspective Start Depth" ), Category( "Art Direction" ),
                  Range( 0.0f, 100.0f ), Units( "km" ),
                  Tooltip( "Distance from the camera at which aerial perspective starts being applied. "
                           "UE default: 0.1 km." ) )
        float AerialPerspectiveStartDepth = 0.1f;

        // Where UE keeps this as an engine cvar (r.SkyAtmosphere.AerialPerspectiveLUT.Depth), it is a
        // component field here for one reason: the froxel volume has 16 slices whatever the scene, and
        // 96 km of them in a level whose whole world is 800 m across spends 14 of the 16 on air nobody
        // will ever look through. Authoring the range is what lets a small level keep its resolution.
        PROPERTY( DisplayName( "Aerial Perspective Distance" ), Category( "Art Direction" ), Range( 1.0f, 200.0f ),
                  Units( "km" ),
                  Tooltip( "How far the aerial-perspective froxel volume reaches. Its 16 slices are "
                           "distributed over this range, so set it near the scene's own view distance "
                           "for the most resolution where geometry actually is. Beyond it the haze "
                           "holds at the value it had at this distance. UE default: 96 km." ) )
        float AerialPerspectiveDistance = 96.0f;
    };

    struct SkyAtmosphereComponent
    {
        SkyAtmosphereData Data;

        // Transient (no PROPERTY -> not reflected, not serialized): the editor's "Bake Sky IBL" button and
        // the offscreen renderers raise it for one frame; the sky system consumes and clears it.
        bool RequestBake = false;
    };
} // namespace Desert::ECS
