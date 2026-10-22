#include "MaterialJFAFinal.hpp"

namespace Desert::Graphic
{
    MaterialJFAFinal::MaterialJFAFinal() : Material( "MaterialJFAFinal", "JFA_Final" )
    {
    }

    void MaterialJFAFinal::SetParams( const glm::vec4& outlineColor, float outlineWidth, float smoothness )
    {
        SetOutlineColor( outlineColor );
        SetOutlineWidth( outlineWidth );
        SetSmoothness( smoothness );
        // Uploaded where the draw reads them (MaterialExecutor::Apply: dirty MPROPERTYs into their fields, then
        // every field-filled buffer whose active copy is behind -- each frame-in-flight copy catches up on its
        // own).
    }
} // namespace Desert::Graphic
