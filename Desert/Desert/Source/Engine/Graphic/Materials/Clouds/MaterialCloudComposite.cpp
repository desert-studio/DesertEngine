#include "MaterialCloudComposite.hpp"

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    MaterialCloudComposite::MaterialCloudComposite() : Material( "MaterialCloudComposite", "CloudComposite" )
    {
        m_ScatterTexture = m_MaterialExecutor->GetTexture2DProperty( "u_CloudScatter" ).get();
        m_GuideTexture   = m_MaterialExecutor->GetTexture2DProperty( "u_CloudGuide" ).get();

        // Reflection did not hand us a sampler the composite declares. Binding would then be a no-op and
        // the pass would quietly composite the fallback texture over the whole screen — a full-screen
        // artefact with no error anywhere. Say which name was looked for.
        if ( !m_ScatterTexture )
            LOG_ERROR( "[Clouds] The CloudComposite shader has no sampler named 'u_CloudScatter'; the "
                       "cloud composite would draw the fallback texture." );

        // The guide is not decoration: without it the upsample has no way to tell a cloud texel from an
        // empty one, and a fallback texture reads as a guide that says every neighbour is at distance
        // zero — perfectly coherent, so every pixel would silently take the plain bilinear path.
        if ( !m_GuideTexture )
            LOG_ERROR( "[Clouds] The CloudComposite shader has no sampler named 'u_CloudGuide'; the cloud "
                       "upsample would fall back to bilinear over the whole screen." );
    }

    void MaterialCloudComposite::BindInputs( const Image2D* scatterImage, const Image2D* guideImage )
    {
        if ( m_ScatterTexture && scatterImage )
            m_ScatterTexture->SetImage( scatterImage, RDG::Access::SampledGraphics );

        if ( m_GuideTexture && guideImage )
            m_GuideTexture->SetImage( guideImage, RDG::Access::SampledGraphics );
    }
} // namespace Desert::Graphic
