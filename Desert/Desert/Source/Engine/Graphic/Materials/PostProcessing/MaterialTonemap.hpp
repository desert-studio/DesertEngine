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
            // The view's grade: operator, film curve, white balance, gamma, manual exposure and the auto-exposure
            // switch and bias are read from it in BindValues and nowhere else, so the uniform block and the
            // settings cannot name a value two ways.
            const Core::PostProcessSettings& Grade;
            float                            BloomIntensity;
            float     ChromaticBloom;      // lens dispersion strength on the bloom halo (0 = off)
            float     LightShaftIntensity; // Bloom Scale x screen fade of the sun light's shafts (0 = off)
            glm::vec3 LightShaftTint;      // the sun light's Bloom Tint
            float     LensFlareIntensity;  // Lens Flare Intensity x screen fade (0 = off)
            glm::vec3 LensFlareTint;       // the lens's Tint
        };

        // Uniform values only: every texture the shader samples (u_GeometryTexture, u_AvgLuminance,
        // u_BloomTexture, u_LightShaftTexture, u_LensFlareTexture) is a graph resource the tonemap exec binds
        // through RDG::PassBindings, never here.
        void BindValues( const Params& params );

        // The three packed vec4s of TonemapUB, from a grade. Static so the MPROPERTY defaults below use them too.
        static glm::vec4 FilmCurveOf( const Core::PostProcessSettings& grade )
        {
            return { grade.FilmSlope, grade.FilmToe, grade.FilmShoulder, grade.FilmBlackClip };
        }
        static glm::vec4 FilmLookOf( const Core::PostProcessSettings& grade )
        {
            return { grade.FilmWhiteClip, grade.BlueCorrection, grade.ExpandGamut, grade.ToneCurveAmount };
        }
        static glm::vec4 WhiteBalanceOf( const Core::PostProcessSettings& grade )
        {
            const bool identity = Core::IsIdentityWhiteBalance( grade.WhiteTemp, grade.WhiteTint );
            return { grade.WhiteTemp, grade.WhiteTint, identity ? 0.0f : 1.0f, 0.0f };
        }

        MPROPERTY( float, Exposure,            "u_Exposure",            1.0f )
        MPROPERTY( float, Gamma,               "u_Gamma",               2.2f )
        MPROPERTY( float, BloomIntensity,      "u_BloomIntensity",      0.0f )
        MPROPERTY( float, AutoExposureCompensation, "u_AutoExposureCompensation", 1.0f )
        MPROPERTY( float, AutoExposureEnabled, "u_AutoExposureEnabled", 0.0f )
        MPROPERTY( float, ChromaticBloom,      "u_ChromaticBloom",      0.0f )
        MPROPERTY( float, WhitePoint, "u_WhitePoint", 8.0f )
        // 0 = Filmic, 1 = extended Reinhard — Core::TonemapOperator's own numbering, converted in BindValues().
        MPROPERTY( float, TonemapOperator, "u_TonemapOperator", 0.0f )
        // UE's film curve and look, packed as SceneComposite's TonemapUB declares them. The starting values are
        // the settings' own defaults (Core::PostProcessSettings), never a second copy of UE's numbers.
        MPROPERTY( glm::vec4, FilmCurve, "u_FilmCurve", ( FilmCurveOf( Core::PostProcessSettings{} ) ) )
        MPROPERTY( glm::vec4, FilmLook, "u_FilmLook", ( FilmLookOf( Core::PostProcessSettings{} ) ) )
        // x = WhiteTemp, y = WhiteTint, z = 1 when they are not UE's identity (6500 K, 0).
        MPROPERTY( glm::vec4, WhiteBalance, "u_WhiteBalance", ( WhiteBalanceOf( Core::PostProcessSettings{} ) ) )
        // rgb = tint, a = intensity: one vec4 slot, and the two cannot disagree about being on.
        MPROPERTY( glm::vec4, LightShaftTintIntensity, "u_LightShaftTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
        // Same packing as the shafts above, for the same reason: tint and intensity travel together so
        // they cannot disagree about whether the effect is on.
        MPROPERTY( glm::vec4, LensFlareTintIntensity, "u_LensFlareTintIntensity",
                   ( glm::vec4( 1.0f, 1.0f, 1.0f, 0.0f ) ) )
    };
} // namespace Desert::Graphic
