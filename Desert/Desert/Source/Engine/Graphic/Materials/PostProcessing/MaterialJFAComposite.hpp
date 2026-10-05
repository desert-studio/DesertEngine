#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Final Jump Flood pass: composites the outline on top of scene color. Holds the uniform block only; the seed
    // and the scene colour are bound per pass by the frame graph (JumpFloodOutlineRenderer::RecordFinal).
    class MaterialJFAComposite final : public Material
    {
    public:
        MaterialJFAComposite();

        void SetParams( const glm::vec4& outlineColor, float outlineWidth, float smoothness );

        // Typed outline parameters — visible to editor via GetRegisteredProperties()
        MPROPERTY( glm::vec4, OutlineColor, "u_OutlineColor", (glm::vec4( 1.0f, 0.5f, 0.0f, 1.0f )) )
        MPROPERTY( float,     OutlineWidth, "u_OutlineWidth", 4.0f )
        MPROPERTY( float, Smoothness, "u_Smoothness", 2.0f )
    };
} // namespace Desert::Graphic
