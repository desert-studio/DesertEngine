#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>
#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>

namespace Desert::Graphic
{
    class MaterialTonemap final : public Material
    {
    public:
        explicit MaterialTonemap();

        struct Params
        {
            // The scene's tonemapping curve, carried as the ENUM and not as an int: the shader reads it
            // as a float and there is exactly one place (below) where that conversion happens, so no
            // caller can be wrong about which number meant which operator.
            Core::TonemapOperator Operator;
            float                 Exposure;
            bool                  AutoExposure;
            float                 ChromaticBloom; // lens dispersion strength on the bloom halo (0 = off)
            float     LightShaftIntensity; // Bloom Scale x screen fade of the sun light's shafts (0 = off)
            glm::vec3 LightShaftTint;      // the sun light's Bloom Tint
            float     LensFlareIntensity;  // Lens Flare Intensity x screen fade (0 = off)
            glm::vec3 LensFlareTint;       // the lens's Tint
            float     PreExposure;         // ViewFrame::PreExposure the scene colour is stored at (> 0)
            // The bloom dirt mask's tint and intensity (UE BloomDirtMaskTint / Intensity); 0 = no mask.
            glm::vec3 BloomDirtMaskTint{ 0.5f };
            float     BloomDirtMaskIntensity = 0.0f;
            // Local exposure (TonemapRenderer::FillMaterial): vec4 packing so std140 cannot drift.
            glm::vec4 LocalExposureContrast{ 1.0f, 1.0f, 1.0f, 0.6f };  // highlight, shadow, detail, blurred blend
            glm::vec4 LocalExposureThreshold{ 0.0f, 0.0f, 1.0f, 1.0f }; // hi thr, sh thr, hi strength, sh strength
            glm::vec4 LocalExposureUV{ 1.0f };                          // grid UV scale xy, blur UV scale xy
            glm::vec4 LocalExposureSlice{ 0.0f };                       // log middle grey, slice log min, 1/EV, mode
            float     LocalExposureMethod = 0.0f; // 1: Fusion (ExposureFusionTexture), 0: Bilateral
            // A display-referred buffer view (Graphic::IsDisplayReferredView): the scene colour, un-pre-exposed,
            // is the picture - SceneComposite's operator 2 skips exposure, the effects and the curve.
            bool VisualizeBuffer = false;
            // The lens stage (LENS-POST, TonemapRenderer::FillMaterial); all zero = off, the picture untouched.
            glm::vec4 LensFringeVignette{ 0.0f }; // aberration scale R, scale G, start offset; vignette intensity
            glm::vec4 LensGrain{ 0.0f };          // grain intensity, grain texel size (pixels), frame seed, 0
            glm::vec4 LensPanini{ 0.0f };         // Panini D, Panini S, tan(half FOV x), tan(half FOV y)
        };

        // Uniform values only: every texture the shader samples (GeometryTexture, AverageLuminanceTexture,
        // BloomTexture, LightShaftTexture, LensFlareTexture, BloomDirtMaskTexture) is a graph resource the tonemap
        // exec binds through RDG::PassBindings, never here.
        void BindValues( const Params& params );

        MPROPERTY( float, Exposure, "Exposure", 1.0f )
        // Graphic::kExposureMiddleGrey, the exposure law's constant (auto: middle grey / adapted luminance).
        MPROPERTY( float, MiddleGrey,          "MiddleGrey",          kExposureMiddleGrey )
        MPROPERTY( float, AutoExposureEnabled, "AutoExposureEnabled", 0.0f )
        MPROPERTY( float, ChromaticBloom,      "ChromaticBloom",      0.0f )
        // Core::TonemapOperator's numbering (0 ACES, 1 Reinhard, 2 Filmic), 3 = the buffer visualization.
        MPROPERTY( float, TonemapOperator, "TonemapOperator", 0.0f )
        MPROPERTY( float, InvPreExposure, "InversePreExposure", 1.0f )
        // rgb = tint, a = intensity: one vec4 slot, and the two cannot disagree about being on.
        MPROPERTY( glm::vec4, LightShaftTintIntensity, "LightShaftTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
        // Same packing as the shafts above, for the same reason: tint and intensity travel together so
        // they cannot disagree about whether the effect is on.
        MPROPERTY( glm::vec4, LensFlareTintIntensity, "LensFlareTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
        // rgb = the dirt mask's tint, a = its intensity: on together or not at all.
        MPROPERTY( glm::vec4, BloomDirtMaskTintIntensity, "BloomDirtMaskTintIntensity",
                   ( glm::vec4( 0.5f, 0.5f, 0.5f, 0.0f ) ) )
        MPROPERTY( glm::vec4, LocalExposureContrast, "LocalExposureContrast", ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.6f ) ) )
        MPROPERTY( glm::vec4, LocalExposureThreshold, "LocalExposureThreshold", ( glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f ) ) )
        MPROPERTY( glm::vec4, LocalExposureUV, "LocalExposureUV", ( glm::vec4( 1.0f ) ) )
        MPROPERTY( glm::vec4, LocalExposureSlice, "LocalExposureSlice", ( glm::vec4( 0.0f ) ) )
        MPROPERTY( float, LocalExposureMethod, "LocalExposureMethod", 0.0f )
        MPROPERTY( glm::vec4, LensFringeVignette, "LensFringeVignette", ( glm::vec4( 0.0f ) ) )
        MPROPERTY( glm::vec4, LensGrain, "LensGrain", ( glm::vec4( 0.0f ) ) )
        MPROPERTY( glm::vec4, LensPanini, "LensPanini", ( glm::vec4( 0.0f ) ) )
    };
} // namespace Desert::Graphic
