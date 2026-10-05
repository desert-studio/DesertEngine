#include "MaterialTonemap.hpp"

namespace Desert::Graphic
{
    MaterialTonemap::MaterialTonemap() : Material( "MaterialTonemap", "SceneComposite" )
    {
    }

    void MaterialTonemap::BindValues( const Params& params )
    {
        SetExposure( params.Exposure );
        SetGamma( params.Gamma );
        SetBloomIntensity( params.BloomIntensity );
        SetExposureKey( params.ExposureKey );
        SetAutoExposureEnabled( params.AutoExposure ? 1.0f : 0.0f );
        SetChromaticBloom( params.ChromaticBloom );
        SetWhitePoint( params.WhitePoint );
        SetTonemapOperator( static_cast<float>( static_cast<int>( params.Operator ) ) );
        SetLightShaftTintIntensity( glm::vec4( params.LightShaftTint, params.LightShaftIntensity ) );
        SetLensFlareTintIntensity( glm::vec4( params.LensFlareTint, params.LensFlareIntensity ) );

        UploadRegisteredProperties();

        // EVERY UB with dirty fields (not just the ones touched this frame) so each per-frame-in-flight
        // copy receives the value — same reason as MaterialJFAComposite (TProperty::Set skips unchanged
        // values, which would otherwise leave other frame copies uninitialized → flicker).
        FlushFieldFilledUniformBuffers();
    }
} // namespace Desert::Graphic
