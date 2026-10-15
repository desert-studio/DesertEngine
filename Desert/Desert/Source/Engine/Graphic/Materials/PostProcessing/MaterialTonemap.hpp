#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    class MaterialTonemap final : public Material
    {
    public:
        explicit MaterialTonemap();

        struct Params
        {
            // The view's exposure (Graphic::ViewExposureOf) - the bloom setup thresholds under the same one. The
            // rest of the grade is not a uniform here: it is baked into the colour-grading table the shader
            // samples (CombineLUTsRenderer).
            glm::vec4 Exposure;
            float     BloomIntensity;
            float     ChromaticBloom;      // lens dispersion strength on the bloom halo (0 = off)
            float     LightShaftIntensity; // Bloom Scale x screen fade of the sun light's shafts (0 = off)
            glm::vec3 LightShaftTint;      // the sun light's Bloom Tint
            float     LensFlareIntensity;  // Lens Flare Intensity x screen fade (0 = off)
            glm::vec3 LensFlareTint;       // the lens's Tint
        };

        // Uniform values only: every texture the shader samples (u_GeometryTexture, u_AvgLuminance,
        // u_BloomTexture, u_LightShaftTexture, u_LensFlareTexture, u_ColorGradingLUT) is a graph resource the
        // tonemap exec binds through RDG::PassBindings, never here.
        void BindValues( const Params& params );

        // x = manual exposure, y = 1 when auto-exposure is on, z = 2^AutoExposureBias (ViewExposure.glslh).
        MPROPERTY( glm::vec4, ExposureParams, "u_ExposureParams", ( glm::vec4( 1.0f, 0.0f, 1.0f, 0.0f ) ) )
        MPROPERTY( float, BloomIntensity, "u_BloomIntensity", 0.0f )
        MPROPERTY( float, ChromaticBloom, "u_ChromaticBloom", 0.0f )
        // rgb = tint, a = intensity: one vec4 slot, and the two cannot disagree about being on.
        MPROPERTY( glm::vec4, LightShaftTintIntensity, "u_LightShaftTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
        // Same packing as the shafts above, for the same reason: tint and intensity travel together so
        // they cannot disagree about whether the effect is on.
        MPROPERTY( glm::vec4, LensFlareTintIntensity, "u_LensFlareTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
    };
} // namespace Desert::Graphic
