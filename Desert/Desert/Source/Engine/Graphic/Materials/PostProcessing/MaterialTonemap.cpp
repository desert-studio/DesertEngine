#include "MaterialTonemap.hpp"

namespace Desert::Graphic
{
    MaterialTonemap::MaterialTonemap() : Material( "MaterialTonemap", "SceneComposite" )
    {
        m_GeometryTexture   = m_MaterialExecutor->GetTexture2DProperty( "u_GeometryTexture" ).get();
        m_BloomTexture      = m_MaterialExecutor->GetTexture2DProperty( "u_BloomTexture" ).get();
        m_AvgLuminance      = m_MaterialExecutor->GetTexture2DProperty( "u_AvgLuminance" ).get();
        m_LightShaftTexture = m_MaterialExecutor->GetTexture2DProperty( "u_LightShaftTexture" ).get();
        m_LensFlareTexture  = m_MaterialExecutor->GetTexture2DProperty( "u_LensFlareTexture" ).get();
    }

    void MaterialTonemap::BindInputs( const std::shared_ptr<Image2D>& targetImage,
                                      const std::shared_ptr<Image2D>& bloomImage,
                                      const std::shared_ptr<Image2D>& avgLuminance,
                                      const std::shared_ptr<Image2D>& lightShaftImage,
                                      const std::shared_ptr<Image2D>& lensFlareImage, const Params& params )
    {
        if ( m_GeometryTexture && targetImage )
            m_GeometryTexture->SetImage( targetImage.get(), RDG::Access::SampledGraphics );

        if ( m_BloomTexture && bloomImage )
            m_BloomTexture->SetImage( bloomImage.get(), RDG::Access::SampledGraphics );

        if ( m_AvgLuminance && avgLuminance )
            m_AvgLuminance->SetImage( avgLuminance.get(), RDG::Access::SampledGraphics );

        if ( m_LightShaftTexture && lightShaftImage )
            m_LightShaftTexture->SetImage( lightShaftImage.get(), RDG::Access::SampledGraphics );

        if ( m_LensFlareTexture && lensFlareImage )
            m_LensFlareTexture->SetImage( lensFlareImage.get(), RDG::Access::SampledGraphics );

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
