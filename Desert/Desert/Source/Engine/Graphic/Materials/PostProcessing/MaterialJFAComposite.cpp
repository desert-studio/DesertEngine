#include "MaterialJFAComposite.hpp"

namespace Desert::Graphic
{
    MaterialJFAComposite::MaterialJFAComposite() : Material( "MaterialJFAComposite", "JFA_Final" )
    {
        m_JFATexture   = m_MaterialExecutor->GetTexture2DProperty( "u_JFATexture" ).get();
        m_SceneTexture = m_MaterialExecutor->GetTexture2DProperty( "u_SceneTexture" ).get();
    }

    void MaterialJFAComposite::BindInputs( const Image2D* jfaSeed, const Image2D* sceneColor,
                                           const glm::vec4& outlineColor, float outlineWidth, float smoothness )
    {
        if ( m_JFATexture && jfaSeed )
            m_JFATexture->SetImage( jfaSeed, RDG::Access::SampledGraphics );

        if ( m_SceneTexture && sceneColor )
            m_SceneTexture->SetImage( sceneColor, RDG::Access::SampledGraphics );

        SetOutlineColor( outlineColor );
        SetOutlineWidth( outlineWidth );
        SetSmoothness( smoothness );

        UploadRegisteredProperties();

        // EVERY UB that still has dirty fields — NOT just the ones touched this frame. TProperty::Set
        // skips re-marking when the value is unchanged (the outline color is constant every frame), so
        // flushing only what changed updates the first frame's copy and leaves the others
        // uninitialized — that was the yellow<->white outline flicker.
        FlushFieldFilledUniformBuffers();
    }
} // namespace Desert::Graphic
