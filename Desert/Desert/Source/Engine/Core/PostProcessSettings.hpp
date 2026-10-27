#pragma once

#include <cstdint>
#include <string_view>

#include <glm/glm.hpp>

#include <Common/Core/Units.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::Core
{
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

    // UE ELocalExposureMethod (Scene.h:45). Bilateral — a bilateral grid of log luminance plus a blurred log
    // luminance give each pixel its base layer (Graphic/PostProcessing/LocalExposureRules.hpp). Fusion — UE 5.4's
    // exposure fusion: three synthetic exposures blended by well-exposedness through a Laplacian pyramid
    // (LocalExposureRenderer, Shaders/Common/LocalExposure.glslh); no halos at shade/sun edges.
    enum class LocalExposureMethod : int
    {
        Bilateral = 0,
        Fusion    = 1,
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

        // THE THREE RENDER METHODS this view ASKS for (Common/Settings/RenderMethods.hpp). What it gets is
        // Graphic::ResolveRenderMethods' minimum of this, the machine's Scalability ceiling and the device, walked
        // down to an implemented method; a downgrade is one log line and an overlay row. A declared method whose
        // renderer has not landed is refused where it is set (Refuse) — the loader, Details and scripts alike.
        // Taken whole when volumes blend (a method has no value between two).
        //
        //  ScreenSpace          — gathers from sun-lit G-buffer neighbours inside the lighting pass: only geometry
        //                         on screen bounces, no denoiser.
        //  ProbeVolume          — the world radiance cache (probes on the global distance field, every light and
        //                         the sky, off screen too) read per pixel: the cheap traced path.
        //  SoftwareTraced       — the same cache behind screen probes traced per frame: contact detail and
        //                         rough reflections.
        PROPERTY( DisplayName( "Method" ), Category( "Global Illumination" ),
                  Refuse( "Common::Scalability::RefuseGIMethod" ),
                  Tooltip( "Requested method; the viewport overlay shows the effective one." ) )
        Common::Scalability::GIMethod GlobalIlluminationMethod = Common::Scalability::GIMethod::ScreenSpace;
        // The screen-space gather's strength (ScreenSpace only: the traced methods carry the lights' own energy).
        PROPERTY( DisplayName( "GI Intensity" ), Category( "Global Illumination" ), Range( 0.0f, 20.0f ) )
        float GIIntensity = 2.0f;

        // Screen space by default (SSR1B, scene v48; the method since v52): the trace + resolve cost 0.25-0.29 ms
        // after SSR1, so a view no volume touches reflects, as UE's ScreenSpaceReflectionIntensity default does.
        // Off = the sky's prefiltered environment only.
        PROPERTY( DisplayName( "Method" ), Category( "Reflections" ),
                  Refuse( "Common::Scalability::RefuseReflectionMethod" ),
                  Tooltip( "Requested method; the viewport overlay shows the effective one." ) )
        Common::Scalability::ReflectionMethod ReflectionsMethod = Common::Scalability::ReflectionMethod::ScreenSpace;
        PROPERTY( DisplayName( "SSR Intensity" ), Category( "Reflections" ), Range( 0.0f, 1.0f ) )
        float SSRIntensity = 1.0f;
        PROPERTY( DisplayName( "SSR Max Distance" ), Category( "Reflections" ), Length, Range( 100.0f, 20000.0f ) )
        float SSRMaxDistance = Common::Units::Metres( 40.0f );

        PROPERTY( DisplayName( "Method" ), Category( "Shadows" ),
                  Refuse( "Common::Scalability::RefuseShadowMethod" ),
                  Tooltip( "Requested method; the viewport overlay shows the effective one." ) )
        Common::Scalability::ShadowMethod ShadowsMethod = Common::Scalability::ShadowMethod::CascadedMaps;
        // The two supplements every shadow method takes: screen-space contact shadows for the detail a map
        // misses, distance-field shadows for the sun beyond the maps' reach. Declared; refused until they land.
        PROPERTY( DisplayName( "Contact Shadows" ), Category( "Shadows" ),
                  Refuse( "Common::Scalability::RefuseContactShadows" ) )
        bool ContactShadows = false;
        PROPERTY( DisplayName( "Distance Field Shadows" ), Category( "Shadows" ),
                  Refuse( "Common::Scalability::RefuseDistanceFieldShadows" ) )
        bool DistanceFieldShadows = false;

        PROPERTY( DisplayName( "Tonemapper" ), Category( "Film" ) )
        TonemapOperator Tonemapper = TonemapOperator::ACES;
        // UE's Manual metering mode, in EV100: the meter reading the exposure is fixed at when auto-exposure is
        // off. The scale is Graphic::ManualExposureScale (key / EV100ToLuminance) — the auto path's own law, so a
        // manual EV100 matches an auto-exposure adapted to the same EV100. Higher EV100 = darker picture: ~15 a
        // 100 000 lux sunlit scene, ~0 a lit interior. Scenes before v57 stated a multiplier; the migration
        // re-states it as the EV100 giving the same scale at that scene's key. The default is the former
        // multiplier 1 at the default key 0.18.
        PROPERTY( DisplayName( "Manual EV100" ), Category( "Exposure" ), Range( -15.0f, 25.0f ), Units( "EV100" ),
                  Tooltip( "Exposure when Auto Exposure is off (UE Manual): the scene EV100 shown as middle "
                           "grey. ~15 clear sun (100 000 lux), ~10 overcast, ~0 interior, ~-3 night street." ) )
        float ManualExposureEV100 = 0.5260688f;
        PROPERTY( DisplayName( "White Point (Reinhard)" ), Category( "Film" ), Range( 1.0f, 20.0f ) )
        float WhitePoint = 8.0f;

        PROPERTY( DisplayName( "Auto Exposure" ), Category( "Exposure" ) )
        bool AutoExposure = false;
        PROPERTY( DisplayName( "Auto Exposure Key" ), Category( "Exposure" ), Range( 0.0f, 1.0f ) )
        float AutoExposureKey = 0.18f; // middle-grey target
        PROPERTY( DisplayName( "Auto Exposure Speed" ), Category( "Exposure" ), Range( 0.0f, 10.0f ) )
        float AutoExposureSpeed = 1.5f; // higher = adapts faster
        // UE AutoExposureMinBrightness / MaxBrightness with "Extend default luminance range" (the only mode UE5
        // ships): the range the metered average may adapt over, in EV100 - luminance = 2^(EV100 - 3) cd/m^2
        // (Graphic::EV100ToLuminance). The UE defaults span night to a 100 000 lux sun, so physical light values
        // (ECS::LightUnits, lux) expose correctly without retuning. Scenes before v56 stated luminance clamps
        // (0.02 / 8 by default): the migration re-states them in EV100, the same clamp.
        PROPERTY(
             DisplayName( "Min EV100" ), Category( "Exposure" ), Range( -15.0f, 25.0f ), Units( "EV100" ),
             Tooltip( "Darkest scene the eye adapts to (UE Min EV100): -10 starlight .. 20 beyond sunlit snow." ) )
        float AutoExposureMinEV100 = -10.0f;
        PROPERTY( DisplayName( "Max EV100" ), Category( "Exposure" ), Range( -15.0f, 25.0f ), Units( "EV100" ),
                  Tooltip( "Brightest scene the eye adapts to (UE Max EV100); a 100 000 lux sun meters ~15." ) )
        float AutoExposureMaxEV100 = 20.0f;

        // LOCAL EXPOSURE — UE's Local Exposure, Bilateral method (Scene.h:2025-2115, defaults Scene.cpp:525-534).
        // The defaults are the IDENTITY: contrast 1 / 1 and detail 1 give every pixel exactly 1 whatever the rest,
        // and no pass runs (Graphic::IsLocalExposureIdentity). Below 1 the contrast scales compress the base layer
        // toward the exposed middle grey: shade is lifted, highlights pulled down, local detail kept.
        PROPERTY( DisplayName( "Method" ), Category( "Local Exposure" ) )
        Core::LocalExposureMethod LocalExposureMethod = Core::LocalExposureMethod::Bilateral;
        PROPERTY( DisplayName( "Highlight Contrast Scale" ), Category( "Local Exposure" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Contrast of the base layer above middle grey: < 1 pulls bright regions down." ) )
        float LocalExposureHighlightContrastScale = 1.0f;
        PROPERTY( DisplayName( "Shadow Contrast Scale" ), Category( "Local Exposure" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Contrast of the base layer below middle grey: < 1 lifts dark regions." ) )
        float LocalExposureShadowContrastScale = 1.0f;
        PROPERTY( DisplayName( "Detail Strength" ), Category( "Local Exposure" ), Range( 0.0f, 4.0f ),
                  Tooltip( "Scale of the detail (pixel minus base layer): > 1 sharpens local contrast." ) )
        float LocalExposureDetailStrength = 1.0f;
        PROPERTY( DisplayName( "Blurred Luminance Blend" ), Category( "Local Exposure" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Base layer = bilateral grid blended toward the blurred luminance by this much." ) )
        float LocalExposureBlurredLuminanceBlend = 0.6f;
        PROPERTY( DisplayName( "Blurred Luminance Kernel Size" ), Category( "Local Exposure" ), Range( 0.0f, 100.0f ),
                  Units( "%" ), Tooltip( "Width of the blurred-luminance window, percent of the view width." ) )
        float LocalExposureBlurredLuminanceKernelSizePercent = 50.0f;
        PROPERTY( DisplayName( "Middle Grey Bias" ), Category( "Local Exposure" ), Range( -15.0f, 15.0f ),
                  Units( "EV" ), Tooltip( "Shifts the grey the base layer is compressed toward." ) )
        float LocalExposureMiddleGreyBias = 0.0f;
        PROPERTY( DisplayName( "Highlight Threshold" ), Category( "Local Exposure" ), Range( 0.0f, 4.0f ),
                  Units( "EV" ), Tooltip( "Stops above middle grey left uncompressed." ) )
        float LocalExposureHighlightThreshold = 0.0f;
        PROPERTY( DisplayName( "Shadow Threshold" ), Category( "Local Exposure" ), Range( 0.0f, 4.0f ),
                  Units( "EV" ), Tooltip( "Stops below middle grey left uncompressed." ) )
        float LocalExposureShadowThreshold = 0.0f;
        PROPERTY( DisplayName( "Highlight Threshold Strength" ), Category( "Local Exposure" ), Range( 0.0f, 1.0f ),
                  Tooltip( "How fully the highlight threshold holds back compression (1 = fully)." ) )
        float LocalExposureHighlightThresholdStrength = 1.0f;
        PROPERTY( DisplayName( "Shadow Threshold Strength" ), Category( "Local Exposure" ), Range( 0.0f, 1.0f ),
                  Tooltip( "How fully the shadow threshold holds back compression (1 = fully)." ) )
        float LocalExposureShadowThresholdStrength = 1.0f;

        PROPERTY( DisplayName( "Enable Bloom" ), Category( "Bloom" ) )
        bool EnableBloom = false; // off by default -> no glow out of the box
        PROPERTY( DisplayName( "Bloom Threshold" ), Category( "Bloom" ), Range( 0.0f, 10.0f ) )
        float BloomThreshold = 2.0f;
        PROPERTY( DisplayName( "Bloom Intensity" ), Category( "Bloom" ), Range( 0.0f, 5.0f ) )
        float BloomIntensity = 0.8f;
        PROPERTY( DisplayName( "Lens Dispersion" ), Category( "Bloom" ), Range( 0.0f, 3.0f ) )
        float LensDispersion = 0.0f; // chromatic rainbow fringe on the bloom halo (glare); 0 = off

        // MR3 (Graphic/View/DepthOfField.hpp): UE DepthOfFieldFocalDistance / DepthOfFieldFstop /
        // DepthOfFieldSensorWidth, UE's defaults (focal distance 0 = no depth of field), and the largest bokeh
        // radius (UE r.DOF.Kernel.MaxBackgroundRadius 0.025 of the width). UE keeps the lens in the post-process
        // settings and a CineCamera writes its filmback / focus into them; the focal length is not authored - it
        // follows from the view's FOV and this sensor width. The gather ring count is a cost knob (Scalability
        // PostProcess.DepthOfFieldQuality), not authored here.
        PROPERTY( DisplayName( "Focal Distance" ), Category( "Depth of Field" ), Length, Range( 0.0f, 100000.0f ),
                  Tooltip( "Distance to the plane in focus. 0 turns depth of field off." ) )
        float DepthOfFieldFocalDistance = 0.0f;
        PROPERTY( DisplayName( "Aperture (F-stop)" ), Category( "Depth of Field" ), Range( 1.0f, 32.0f ) )
        float DepthOfFieldFstop = 4.0f; // f / aperture diameter; smaller = shallower focus
        PROPERTY( DisplayName( "Sensor Width (mm)" ), Category( "Depth of Field" ), Range( 0.1f, 1000.0f ) )
        float DepthOfFieldSensorWidth = 24.576f; // filmback width (Super 35); with the FOV gives the focal length
        PROPERTY( DisplayName( "Max Bokeh Size (% of width)" ), Category( "Depth of Field" ),
                  Range( 0.0f, 10.0f ) )
        float DepthOfFieldMaxBokehSize = 2.5f; // largest circle-of-confusion RADIUS, percent of the output width

        // MR2 (Graphic/View/MotionBlur.hpp): UE MotionBlurAmount / MotionBlurMax / MotionBlurTargetFPS, UE's
        // defaults. The sample count is a cost knob (Scalability PostProcess.MotionBlurQuality), not authored
        // here.
        PROPERTY( DisplayName( "Amount" ), Category( "Motion Blur" ), Range( 0.0f, 1.0f ) )
        float MotionBlurAmount = 0.5f; // shutter fraction of the frame's motion; 0 = off
        PROPERTY( DisplayName( "Max (% of screen)" ), Category( "Motion Blur" ), Range( 0.0f, 100.0f ) )
        float MotionBlurMax = 5.0f; // longest blur, percent of the output width
        PROPERTY( DisplayName( "Target FPS" ), Category( "Motion Blur" ), Range( 0.0f, 120.0f ) )
        float MotionBlurTargetFPS = 30.0f; // > 0: blur length as at this frame rate; 0: per rendered frame

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

        // SSS-PROFILE (CONTRACT §6): the screen-space Burley scatter of the materials that name a Subsurface Profile
        // (UE r.SubsurfaceScattering / ShowFlag.SubsurfaceScattering). Off: those materials shade unsplit, one
        // formula, as every other texel (UE SUBSURFACE_RECOMBINE_MODE_NO_SCATTERING).
        PROPERTY( DisplayName( "Subsurface Scattering" ), Category( "Subsurface Scattering" ) )
        bool SubsurfaceScattering = true;
        // Scales every profile's mean free path (UE r.SSS.Scale): 0 = no scatter, 4 = four times the radius.
        PROPERTY( DisplayName( "Scale" ), Category( "Subsurface Scattering" ), Range( 0.0f, 10.0f ) )
        float SubsurfaceScale = 1.0f;
    };
} // namespace Desert::Core
