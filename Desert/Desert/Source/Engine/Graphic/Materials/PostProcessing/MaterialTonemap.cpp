#include "MaterialTonemap.hpp"

namespace Desert::Graphic
{
    MaterialTonemap::MaterialTonemap() : Material( "MaterialTonemap", "SceneComposite" )
    {
    }

    void MaterialTonemap::BindValues( const Params& params )
    {
        SetExposure( params.Exposure );
        SetBloomIntensity( params.BloomIntensity );
        SetExposureKey( params.ExposureKey );
        SetAutoExposureEnabled( params.AutoExposure ? 1.0f : 0.0f );
        SetChromaticBloom( params.ChromaticBloom );
        // 3 = the buffer visualization (SceneComposite.shader): not a Core::TonemapOperator a scene can author.
        SetTonemapOperator( params.VisualizeBuffer ? 3.0f
                                                   : static_cast<float>( static_cast<int>( params.Operator ) ) );
        SetLightShaftTintIntensity( glm::vec4( params.LightShaftTint, params.LightShaftIntensity ) );
        SetLensFlareTintIntensity( glm::vec4( params.LensFlareTint, params.LensFlareIntensity ) );
        SetInvPreExposure( 1.0f / params.PreExposure );
        SetLocalExposureContrast( params.LocalExposureContrast );
        SetLocalExposureThreshold( params.LocalExposureThreshold );
        SetLocalExposureUV( params.LocalExposureUV );
        SetLocalExposureSlice( params.LocalExposureSlice );
        SetLocalExposureMethod( params.LocalExposureMethod );
        float quantizationStep = 0.0f;
        if ( params.QuantizationBits > 0u )
            quantizationStep = 1.0f / static_cast<float>( ( 1u << params.QuantizationBits ) - 1u );
        SetOutputQuantizationStep( quantizationStep );
        // Uploaded where the draw reads them (MaterialExecutor::Apply: dirty MPROPERTYs into their fields, then
        // every field-filled buffer whose active copy is behind -- each frame-in-flight copy catches up on its
        // own).
    }
} // namespace Desert::Graphic
