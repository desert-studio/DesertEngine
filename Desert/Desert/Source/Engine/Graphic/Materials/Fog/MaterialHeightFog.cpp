#include "MaterialHeightFog.hpp"

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    MaterialHeightFog::MaterialHeightFog() : Material( "MaterialHeightFog", "HeightFogApply" )
    {
        m_FogTexture = m_MaterialExecutor->GetTexture2DProperty( "u_FogApply" ).get();

        // Reflection did not hand us the sampler the apply pass declares. Binding would then be a no-op
        // and the pass would quietly composite the fallback texture over the whole screen — a
        // full-screen artefact with no error anywhere. Say which name was looked for.
        if ( !m_FogTexture )
            LOG_ERROR( "[HeightFog] The HeightFogApply shader has no sampler named 'u_FogApply'; the "
                       "fog apply would draw the fallback texture." );
    }

    void MaterialHeightFog::BindInputs( const Image2D* fogImage )
    {
        if ( m_FogTexture && fogImage )
            m_FogTexture->SetImage( fogImage, RDG::Access::SampledGraphics );
    }
} // namespace Desert::Graphic
