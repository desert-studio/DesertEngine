#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Final Jump Flood pass: composites the outline on top of scene color. Holds the uniform block only; the seed
    // and the scene colour are bound per pass by the frame graph (JumpFloodOutlineRenderer::RecordFinal).
    class MaterialJFAFinal final : public Material
    {
    public:
        MaterialJFAFinal();

        void SetParams( const glm::vec4& outlineColor, float outlineWidth, float smoothness );

        // Typed outline parameters — visible to editor via GetRegisteredProperties()
        MPROPERTY( glm::vec4, OutlineColor, "OutlineColor", (glm::vec4( 1.0f, 0.5f, 0.0f, 1.0f )) )
        MPROPERTY( float,     OutlineWidth, "OutlineWidth", 4.0f )
        MPROPERTY( float, Smoothness, "Smoothness", 2.0f )
    };
} // namespace Desert::Graphic
