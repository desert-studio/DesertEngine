#include "MaterialJFAComposite.hpp"

namespace Desert::Graphic
{
    MaterialJFAComposite::MaterialJFAComposite() : Material( "MaterialJFAComposite", "JFA_Final" )
    {
    }

    void MaterialJFAComposite::SetParams( const glm::vec4& outlineColor, float outlineWidth, float smoothness )
    {
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
