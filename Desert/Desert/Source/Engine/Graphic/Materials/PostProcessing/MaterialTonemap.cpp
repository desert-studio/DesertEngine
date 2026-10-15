#include "MaterialTonemap.hpp"

namespace Desert::Graphic
{
    MaterialTonemap::MaterialTonemap() : Material( "MaterialTonemap", "SceneComposite" )
    {
    }

    void MaterialTonemap::BindValues( const Params& params )
    {
        const Core::PostProcessSettings& grade = params.Grade;
        SetExposure( grade.Exposure );
        SetGamma( grade.Gamma );
        SetBloomIntensity( params.BloomIntensity );
        SetAutoExposureCompensation( Core::AutoExposureCompensation( grade.AutoExposureBias ) );
        SetAutoExposureEnabled( grade.AutoExposure ? 1.0f : 0.0f );
        SetChromaticBloom( params.ChromaticBloom );
        SetWhitePoint( grade.WhitePoint );
        SetTonemapOperator( static_cast<float>( static_cast<int>( grade.Tonemapper ) ) );
        SetFilmCurve( FilmCurveOf( grade ) );
        SetFilmLook( FilmLookOf( grade ) );
        SetWhiteBalance( WhiteBalanceOf( grade ) );
        SetLightShaftTintIntensity( glm::vec4( params.LightShaftTint, params.LightShaftIntensity ) );
        SetLensFlareTintIntensity( glm::vec4( params.LensFlareTint, params.LensFlareIntensity ) );

        UploadRegisteredProperties();

        // EVERY UB with dirty fields (not just the ones touched this frame) so each per-frame-in-flight
        // copy receives the value — same reason as MaterialJFAComposite (TProperty::Set skips unchanged
        // values, which would otherwise leave other frame copies uninitialized → flicker).
        FlushFieldFilledUniformBuffers();
    }
} // namespace Desert::Graphic
