#pragma once

#include <glm/glm.hpp>

#include <cmath>

#include <Common/Core/Units.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::Core
{
    // The curve that maps HDR scene luminance onto the 0..1 the display can show. Both branches are
    // authored features, and which one a view is graded through is part of its grade.
    //
    //  Filmic   — UE's default operator (ETonemappingMethod::Filmic): its parametric film curve (the Film*
    //             fields below) with gamut expansion and blue correction, Common/FilmToneMap.glslh. Ours by
    //             default too (decision D-10, Docs/Clouds/ANALYSIS_APPROACH.md §7): the reference frames are
    //             captured through it. Until POST-LOOK this value was a fit of the ACES RRT+ODT, which is NOT
    //             what UE grades through (mid grey 0.77 stop darker); the number 0 is unchanged.
    //  Reinhard — extended Reinhard with an explicit WhitePoint (below). Scenes authored before
    //             2026-08-19 had their Exposure and WhitePoint chosen by eye on this curve.
    enum class TonemapOperator : int
    {
        Filmic   = 0,
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

        // THE FILM, as UE's FPostProcessSettings defaults it (Engine/Private/Scene.cpp, FPostProcessSettings
        // ctor): the Filmic operator with Slope 0.88 / Toe 0.55 / Shoulder 0.26 / BlackClip 0 / WhiteClip 0.04,
        // blue correction 0.6, gamut expansion 1, the whole tone curve. Read by Common/FilmToneMap.glslh.
        PROPERTY( DisplayName( "Tonemapper" ), Category( "Film" ) )
        TonemapOperator Tonemapper = TonemapOperator::Filmic;
        PROPERTY( DisplayName( "Slope" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float FilmSlope = 0.88f;
        PROPERTY( DisplayName( "Toe" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float FilmToe = 0.55f;
        PROPERTY( DisplayName( "Shoulder" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float FilmShoulder = 0.26f;
        PROPERTY( DisplayName( "Black Clip" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float FilmBlackClip = 0.0f;
        PROPERTY( DisplayName( "White Clip" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float FilmWhiteClip = 0.04f;
        PROPERTY( DisplayName( "Blue Correction" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float BlueCorrection = 0.6f;
        PROPERTY( DisplayName( "Expand Gamut" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float ExpandGamut = 1.0f;
        PROPERTY( DisplayName( "Tone Curve Amount" ), Category( "Film" ), Range( 0.0f, 1.0f ) )
        float ToneCurveAmount = 1.0f;
        // UE's DisplayGamma: the film colour is raised to 2.2 / Gamma before the sRGB encoding, the identity
        // at 2.2.
        PROPERTY( DisplayName( "Gamma" ), Category( "Film" ), Range( 1.0f, 3.0f ) )
        float Gamma = 2.2f;
        PROPERTY( DisplayName( "White Point (Reinhard)" ), Category( "Film" ), Range( 1.0f, 20.0f ) )
        float WhitePoint = 8.0f;

        // UE's white balance (ETemperatureMethod::TEMP_WhiteBalance): the colour temperature, in kelvin, of the
        // white the scene is lit by, adapted to D65; Tint moves it along the isotherm (green - / magenta +).
        // 6500 / 0 is the identity.
        PROPERTY( DisplayName( "Temp" ), Category( "White Balance" ), Range( 1500.0f, 15000.0f ) )
        float WhiteTemp = 6500.0f;
        PROPERTY( DisplayName( "Tint" ), Category( "White Balance" ), Range( -1.0f, 1.0f ) )
        float WhiteTint = 0.0f;

        // UE's colour grading (FColorGradingSettings, Engine/Classes/Engine/Scene.h; defaults Scene.cpp
        // FPostProcessSettings ctor), applied in AP1 before the tone curve (Common/FilmToneMap.glslh
        // FilmColorCorrectAll) and baked with the rest of the grade into the 32^3 table the tonemap samples. Each
        // control is UE's FVector4: rgb per channel, w a multiplier of all three (Offset: an added constant). A
        // range's control is combined with the global one (multiplied; Offset added); the shadow weight falls to 0
        // at ShadowsMax, the highlight weight rises from HighlightsMin to HighlightsMax, midtones take the rest.
        // All defaults are the identity.
        PROPERTY( DisplayName( "Saturation" ), Category( "Color Grading" ), Header( "Global" ),
                  Range( 0.0f, 2.0f ) )
        glm::vec4 ColorSaturation = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Contrast" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorContrast = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gamma" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGamma = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gain" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGain = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Offset" ), Category( "Color Grading" ), Range( -1.0f, 1.0f ) )
        glm::vec4 ColorOffset = glm::vec4( 0.0f );

        PROPERTY( DisplayName( "Saturation" ), Category( "Color Grading" ), Header( "Shadows" ),
                  Range( 0.0f, 2.0f ) )
        glm::vec4 ColorSaturationShadows = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Contrast" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorContrastShadows = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gamma" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGammaShadows = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gain" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGainShadows = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Offset" ), Category( "Color Grading" ), Range( -1.0f, 1.0f ) )
        glm::vec4 ColorOffsetShadows = glm::vec4( 0.0f );
        PROPERTY( DisplayName( "Shadows Max" ), Category( "Color Grading" ), Range( -1.0f, 1.0f ) )
        float ColorCorrectionShadowsMax = 0.09f;

        PROPERTY( DisplayName( "Saturation" ), Category( "Color Grading" ), Header( "Midtones" ),
                  Range( 0.0f, 2.0f ) )
        glm::vec4 ColorSaturationMidtones = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Contrast" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorContrastMidtones = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gamma" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGammaMidtones = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gain" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGainMidtones = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Offset" ), Category( "Color Grading" ), Range( -1.0f, 1.0f ) )
        glm::vec4 ColorOffsetMidtones = glm::vec4( 0.0f );

        PROPERTY( DisplayName( "Saturation" ), Category( "Color Grading" ), Header( "Highlights" ),
                  Range( 0.0f, 2.0f ) )
        glm::vec4 ColorSaturationHighlights = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Contrast" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorContrastHighlights = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gamma" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGammaHighlights = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Gain" ), Category( "Color Grading" ), Range( 0.0f, 2.0f ) )
        glm::vec4 ColorGainHighlights = glm::vec4( 1.0f );
        PROPERTY( DisplayName( "Offset" ), Category( "Color Grading" ), Range( -1.0f, 1.0f ) )
        glm::vec4 ColorOffsetHighlights = glm::vec4( 0.0f );
        PROPERTY( DisplayName( "Highlights Min" ), Category( "Color Grading" ), Range( -1.0f, 1.0f ) )
        float ColorCorrectionHighlightsMin = 0.5f;
        PROPERTY( DisplayName( "Highlights Max" ), Category( "Color Grading" ), Range( 1.0f, 10.0f ) )
        float ColorCorrectionHighlightsMax = 1.0f;

        // UE's Manual metering mode, in EV100: the meter reading the exposure is fixed at when auto-exposure is
        // off. The scale is Graphic::ManualExposureScale - 2^AutoExposureBias x 0.18 / EV100ToLuminance(EV100),
        // the auto path's own law with the reading fixed, so a manual EV100 matches an auto-exposure adapted to
        // the same EV100, and the Exposure Compensation offsets both (UE applies it in Manual too). Higher EV100 =
        // darker picture: ~15 a 100 000 lux sunlit scene, ~0 a lit interior. Scenes before v57 stated a
        // multiplier; v57 re-stated it as the EV100 giving the same scale at that scene's key, and v62 states the
        // key as the compensation of a volume that stays manual. The default is the former multiplier 1 at
        // compensation 0.
        PROPERTY( DisplayName( "Manual EV100" ), Category( "Exposure" ), Range( -15.0f, 25.0f ), Units( "EV100" ),
                  Tooltip( "Exposure when Auto Exposure is off (UE Manual): the scene EV100 shown as middle "
                           "grey. ~15 clear sun (100 000 lux), ~10 overcast, ~0 interior, ~-3 night street." ) )
        float ManualExposureEV100 = 0.5260688f;

        // UE's histogram eye adaptation, with UE's defaults (FPostProcessSettings, extended luminance range): on;
        // the metered log-average puts 0.18 at 2^Bias x middle grey; outside [Low, High] percent of the histogram
        // is not metered; the average is clamped to the Min / Max EV100 range (the meter law, EV100ToLuminance);
        // the eye adapts in EV per second, faster toward bright than toward dark.
        PROPERTY( DisplayName( "Auto Exposure" ), Category( "Exposure" ) )
        bool AutoExposure = true;
        PROPERTY( DisplayName( "Exposure Compensation" ), Category( "Exposure" ), Range( -15.0f, 15.0f ) )
        float AutoExposureBias = 1.0f;
        PROPERTY( DisplayName( "Low Percent" ), Category( "Exposure" ), Range( 0.0f, 100.0f ) )
        float AutoExposureLowPercent = 10.0f;
        PROPERTY( DisplayName( "High Percent" ), Category( "Exposure" ), Range( 0.0f, 100.0f ) )
        float AutoExposureHighPercent = 90.0f;
        // UE AutoExposureMinBrightness / MaxBrightness with "Extend default luminance range" (the only mode UE5
        // ships): the range the metered average may adapt over, in EV100 - the average luminance = 2^(EV100 - 3)
        // cd/m^2 (Graphic::EV100ToLuminance). The UE defaults span night to a 100 000 lux sun, so physical light
        // values (ECS::LightUnits, lux) expose correctly without retuning. Scenes before v56 stated luminance
        // clamps (0.02 / 8 by default): the migration re-states them in EV100, the same clamp; v62 drops those
        // re-stated old defaults for UE's range.
        PROPERTY(
             DisplayName( "Min EV100" ), Category( "Exposure" ), Range( -15.0f, 25.0f ), Units( "EV100" ),
             Tooltip( "Darkest scene the eye adapts to (UE Min EV100): -10 starlight .. 20 beyond sunlit snow." ) )
        float AutoExposureMinEV100 = -10.0f;
        PROPERTY( DisplayName( "Max EV100" ), Category( "Exposure" ), Range( -15.0f, 25.0f ), Units( "EV100" ),
                  Tooltip( "Brightest scene the eye adapts to (UE Max EV100); a 100 000 lux sun meters ~15." ) )
        float AutoExposureMaxEV100 = 20.0f;
        PROPERTY( DisplayName( "Speed Up" ), Category( "Exposure" ), Range( 0.02f, 20.0f ) )
        float AutoExposureSpeedUp = 3.0f;
        PROPERTY( DisplayName( "Speed Down" ), Category( "Exposure" ), Range( 0.02f, 20.0f ) )
        float AutoExposureSpeedDown = 1.0f;

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
    };

    // UE's GetAutoExposureCompensationFromSettings (PostProcessEyeAdaptation.cpp:446-460, desktop path): the bias
    // is in EV, the compensation the metered average is scaled by is 2^Bias.
    inline float AutoExposureCompensation( float autoExposureBias )
    {
        return std::exp2( autoExposureBias );
    }

    // UE's CombineLUTsCommon skips the white balance when both values are within its 1e-8 of the identity
    // (6500 K, tint 0) - the matrix there is the identity, and the shader is spared building it.
    inline bool IsIdentityWhiteBalance( float whiteTemp, float whiteTint )
    {
        constexpr float kDefaultTemperature = 6500.0f;
        constexpr float kEpsilon            = 1e-8f;
        return std::abs( whiteTemp - kDefaultTemperature ) <= kEpsilon && std::abs( whiteTint ) <= kEpsilon;
    }
} // namespace Desert::Core
