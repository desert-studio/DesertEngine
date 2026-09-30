#include "MaterialFXAA.hpp"

namespace Desert::Graphic
{
    MaterialFXAA::MaterialFXAA() : Material( "MaterialFXAA", "FXAA" )
    {
        m_InputTexture = m_MaterialExecutor->GetTexture2DProperty( "u_InputTexture" ).get();
    }

    void MaterialFXAA::BindInputs( const std::shared_ptr<Image2D>& inputImage )
    {
        if ( m_InputTexture && inputImage )
            m_InputTexture->SetImage( inputImage.get(), RDG::Access::SampledGraphics );
    }
} // namespace Desert::Graphic
