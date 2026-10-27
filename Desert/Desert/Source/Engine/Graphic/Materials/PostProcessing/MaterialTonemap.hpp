#pragma once

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Graphic/Materials/Material.hpp>

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
            float                 BloomIntensity;
            float                 ExposureKey;
            bool                  AutoExposure;
            float                 ChromaticBloom; // lens dispersion strength on the bloom halo (0 = off)
            float                 WhitePoint; // REINHARD only: luminance that maps to pure white (1 = identity)
            float     LightShaftIntensity; // Bloom Scale x screen fade of the sun light's shafts (0 = off)
            glm::vec3 LightShaftTint;      // the sun light's Bloom Tint
            float     LensFlareIntensity;  // Lens Flare Intensity x screen fade (0 = off)
            glm::vec3 LensFlareTint;       // the lens's Tint
            float     PreExposure;         // ViewFrame::PreExposure the scene colour is stored at (> 0)
            // Local exposure (TonemapRenderer::FillMaterial): vec4 packing so std140 cannot drift.
            glm::vec4 LocalExposureContrast{ 1.0f, 1.0f, 1.0f, 0.6f };  // highlight, shadow, detail, blurred blend
            glm::vec4 LocalExposureThreshold{ 0.0f, 0.0f, 1.0f, 1.0f }; // hi thr, sh thr, hi strength, sh strength
            glm::vec4 LocalExposureUV{ 1.0f };                          // grid UV scale xy, blur UV scale xy
            glm::vec4 LocalExposureSlice{ 0.0f };                       // log middle grey, slice log min, 1/EV, mode
            float     LocalExposureMethod = 0.0f; // 1: Fusion (ExposureFusionTexture), 0: Bilateral
        };

        // Uniform values only: every texture the shader samples (GeometryTexture, AverageLuminanceTexture,
        // BloomTexture, LightShaftTexture, LensFlareTexture) is a graph resource the tonemap exec binds
        // through RDG::PassBindings, never here.
        void BindValues( const Params& params );

        MPROPERTY( float, Exposure, "Exposure", 1.0f )
        MPROPERTY( float, BloomIntensity,      "BloomIntensity",      0.0f )
        MPROPERTY( float, ExposureKey,         "ExposureKey",         0.18f )
        MPROPERTY( float, AutoExposureEnabled, "AutoExposureEnabled", 0.0f )
        MPROPERTY( float, ChromaticBloom,      "ChromaticBloom",      0.0f )
        MPROPERTY( float, WhitePoint, "WhitePoint", 8.0f )
        // 0 = ACES, 1 = extended Reinhard — Core::TonemapOperator's own numbering, converted in Bind().
        MPROPERTY( float, TonemapOperator, "TonemapOperator", 0.0f )
        MPROPERTY( float, InvPreExposure, "InversePreExposure", 1.0f )
        // rgb = tint, a = intensity: one vec4 slot, and the two cannot disagree about being on.
        MPROPERTY( glm::vec4, LightShaftTintIntensity, "LightShaftTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
        // Same packing as the shafts above, for the same reason: tint and intensity travel together so
        // they cannot disagree about whether the effect is on.
        MPROPERTY( glm::vec4, LensFlareTintIntensity, "LensFlareTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
        MPROPERTY( glm::vec4, LocalExposureContrast, "LocalExposureContrast", ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.6f ) ) )
        MPROPERTY( glm::vec4, LocalExposureThreshold, "LocalExposureThreshold", ( glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f ) ) )
        MPROPERTY( glm::vec4, LocalExposureUV, "LocalExposureUV", ( glm::vec4( 1.0f ) ) )
        MPROPERTY( glm::vec4, LocalExposureSlice, "LocalExposureSlice", ( glm::vec4( 0.0f ) ) )
        MPROPERTY( float, LocalExposureMethod, "LocalExposureMethod", 0.0f )
    };
} // namespace Desert::Graphic
