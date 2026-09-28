#pragma once

#include <glm/glm.hpp>

#include <Common/Core/Units.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

namespace Desert::Core
{
    // Source of the one-bounce indirect light (Deferred path only).
    //  ScreenSpace — gathers from sun-lit G-buffer neighbours right inside the lighting pass. Cheap and
    //                self-contained, but ONLY geometry currently on screen can bounce, and there is no
    //                denoiser, so a wide radius reads as noise.
    //  RSM         — bounces light off everything the SUN sees (off-screen geometry included) via a
    //                reflective shadow map, resolved into its own buffer and temporally accumulated.
    //                Better and stabler; costs one sun-view raster pass plus two fullscreen passes.
    enum class GIMode : int
    {
        Off         = 0,
        ScreenSpace = 1,
        RSM         = 2,
    };

    // The curve that maps HDR scene luminance onto the 0..1 the display can show. Both branches are
    // authored features, and which one a view is graded through is part of its grade.
    //
    //  ACES     — the filmic curve Unreal ships as its default. It is OURS by default too (decision D-10,
    //             Docs/Clouds/ANALYSIS_APPROACH.md §7): the reference frame the sky programme calibrates
    //             against was captured through it. It takes no white point — the ODT carries its own.
    //  Reinhard — extended Reinhard with an explicit WhitePoint (below). Scenes authored before
    //             2026-08-19 had their Exposure and WhitePoint chosen by eye on this curve.
    enum class TonemapOperator : int
    {
        ACES     = 0,
        Reinhard = 1,
    };

    // THE GRADE OF A VIEW — UE's FPostProcessSettings. Until SET1 these fields were Core::SceneSettings, one
    // value per level, which made "a darker cave inside a bright level" inexpressible and put the level's
    // grade in the same block as its gravity. They are now authored on PostProcessVolume entities
    // (ECS::PostProcessVolumeData::Settings) and BLENDED per view by Graphic::ResolveViewSettings, the one
    // point the renderer reads them from — UE's FFinalPostProcessSettings. The defaults below are the
    // grade of a view that no volume touches, and they are exactly the defaults SceneSettings had, so a
    // scene migrated to an Unbound volume and a scene that never stated a value render the same frame.
    struct PostProcessSettings
    {
        REFLECT()

        // Deferred screen-space effects (Deferred path only). Each costs a full-screen multi-sample pass.
        PROPERTY( DisplayName( "Enable SSAO" ), Category( "Rendering Features" ) )
        bool EnableSSAO = true;

        PROPERTY( DisplayName( "Global Illumination" ), Category( "Global Illumination" ) )
        GIMode GlobalIllumination = GIMode::ScreenSpace;
        // ONE knob for both GI modes, but they are not on the same scale: the screen-space gather is bright
        // at ~2, while the RSM gather is deliberately dim (32 taps, 1/d^2 with a distance floor) and wants ~6.
        PROPERTY( DisplayName( "GI Intensity" ), Category( "Global Illumination" ), Range( 0.0f, 20.0f ) )
        float GIIntensity = 2.0f;

        PROPERTY( DisplayName( "Enable SSR" ), Category( "Reflections" ) )
        bool EnableSSR = false;
        PROPERTY( DisplayName( "SSR Intensity" ), Category( "Reflections" ), Range( 0.0f, 1.0f ) )
        float SSRIntensity = 1.0f;
        PROPERTY( DisplayName( "SSR Max Distance" ), Category( "Reflections" ), Length, Range( 100.0f, 20000.0f ) )
        float SSRMaxDistance = Common::Units::Metres( 40.0f );

        PROPERTY( DisplayName( "Tonemapper" ), Category( "Film" ) )
        TonemapOperator Tonemapper = TonemapOperator::ACES;
        PROPERTY( DisplayName( "Exposure" ), Category( "Exposure" ), Range( 0.0f, 10.0f ) )
        float Exposure = 1.0f; // manual exposure (used when auto-exposure is off)
        PROPERTY( DisplayName( "Gamma" ), Category( "Film" ), Range( 1.0f, 3.0f ) )
        float Gamma = 2.2f;
        PROPERTY( DisplayName( "White Point (Reinhard)" ), Category( "Film" ), Range( 1.0f, 20.0f ) )
        float WhitePoint = 8.0f;

        PROPERTY( DisplayName( "Auto Exposure" ), Category( "Exposure" ) )
        bool AutoExposure = false;
        PROPERTY( DisplayName( "Auto Exposure Key" ), Category( "Exposure" ), Range( 0.0f, 1.0f ) )
        float AutoExposureKey = 0.18f; // middle-grey target
        PROPERTY( DisplayName( "Auto Exposure Speed" ), Category( "Exposure" ), Range( 0.0f, 10.0f ) )
        float AutoExposureSpeed = 1.5f; // higher = adapts faster
        PROPERTY( DisplayName( "Auto Exposure Min" ), Category( "Exposure" ), Range( 0.0f, 5.0f ) )
        float AutoExposureMin = 0.02f; // luminance clamp
        PROPERTY( DisplayName( "Auto Exposure Max" ), Category( "Exposure" ), Range( 0.0f, 20.0f ) )
        float AutoExposureMax = 8.0f;

        PROPERTY( DisplayName( "Enable Bloom" ), Category( "Bloom" ) )
        bool EnableBloom = false; // off by default -> no glow out of the box
        PROPERTY( DisplayName( "Bloom Threshold" ), Category( "Bloom" ), Range( 0.0f, 10.0f ) )
        float BloomThreshold = 2.0f;
        PROPERTY( DisplayName( "Bloom Intensity" ), Category( "Bloom" ), Range( 0.0f, 5.0f ) )
        float BloomIntensity = 0.8f;
        PROPERTY( DisplayName( "Lens Dispersion" ), Category( "Bloom" ), Range( 0.0f, 3.0f ) )
        float LensDispersion = 0.0f; // chromatic rainbow fringe on the bloom halo (glare); 0 = off

        PROPERTY( DisplayName( "Enable Lens Flare" ), Category( "Lens Flare" ) )
        bool EnableLensFlare = false;
        PROPERTY( DisplayName( "Intensity" ), Category( "Lens Flare" ), Range( 0.0f, 5.0f ) )
        float LensFlareIntensity = 0.35f;
        PROPERTY( DisplayName( "Tint" ), Category( "Lens Flare" ), Color )
        glm::vec3 LensFlareTint = glm::vec3( 1.0f );
        PROPERTY( DisplayName( "Threshold" ), Category( "Lens Flare" ), Range( 0.0f, 50.0f ) )
        float LensFlareThreshold = 4.0f;

        PROPERTY( DisplayName( "Ghost Count" ), Category( "Lens Flare" ), Range( 0.0f, 8.0f ) )
        int LensFlareGhostCount = 4;
        PROPERTY( DisplayName( "Ghost Spacing" ), Category( "Lens Flare" ), Range( 0.05f, 1.5f ) )
        float LensFlareGhostSpacing = 0.35f;
        PROPERTY( DisplayName( "Ghost Size Near" ), Category( "Lens Flare" ), Range( 0.05f, 16.0f ) )
        float LensFlareGhostSizeNear = 1.0f;
        PROPERTY( DisplayName( "Ghost Size Far" ), Category( "Lens Flare" ), Range( 0.05f, 16.0f ) )
        float LensFlareGhostSizeFar = 3.0f;
        PROPERTY( DisplayName( "Ghost Tint Inner" ), Category( "Lens Flare" ), Color )
        glm::vec3 LensFlareGhostTintInner = glm::vec3( 1.0f, 0.86f, 0.62f );
        PROPERTY( DisplayName( "Ghost Tint Outer" ), Category( "Lens Flare" ), Color )
        glm::vec3 LensFlareGhostTintOuter = glm::vec3( 0.45f, 0.68f, 1.0f );

        PROPERTY( DisplayName( "Halo Intensity" ), Category( "Lens Flare" ), Range( 0.0f, 3.0f ) )
        float LensFlareHaloIntensity = 0.25f;
        PROPERTY( DisplayName( "Halo Radius" ), Category( "Lens Flare" ), Range( 0.02f, 1.0f ) )
        float LensFlareHaloRadius = 0.18f;

        PROPERTY( DisplayName( "Streak Intensity" ), Category( "Lens Flare" ), Range( 0.0f, 30.0f ) )
        float LensFlareStreakIntensity = 8.0f;
        PROPERTY( DisplayName( "Streak Length" ), Category( "Lens Flare" ), Range( 0.0f, 1.0f ) )
        float LensFlareStreakLength = 0.35f;
        PROPERTY( DisplayName( "Streak Angle" ), Category( "Lens Flare" ), Range( -180.0f, 180.0f ) )
        float LensFlareStreakAngle = 0.0f;

        PROPERTY( DisplayName( "Chromatic Shift" ), Category( "Lens Flare" ), Range( 0.0f, 1.0f ) )
        float LensFlareChromaShift = 0.15f;
    };
} // namespace Desert::Core
