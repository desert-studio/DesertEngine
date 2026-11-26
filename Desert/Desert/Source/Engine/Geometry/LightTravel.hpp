#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// A DIRECTIONAL LIGHT'S DIRECTION IS ITS ROTATION, as in UE (ULightComponent::GetDirection =
// GetComponentTransform().GetUnitAxis( EAxis::X )). The light travels along the entity's forward axis, which in
// this engine is -Z for every entity (cameras, spot lights, projectiles). The one definition: ECS::Rules
// (SystemRules.hpp) names it for the systems, Core::MigrateLightRotationV0ToV1
// (Core/SceneUpgrade/Steps/Light1Rotation.hpp) for the upgrade of a light v0 file - Core sits below the ECS rules, so
// the rule lives in GeometryCore, beneath both.
namespace Desert::Geometry
{
    inline constexpr glm::vec3 kLightForwardAxis = glm::vec3( 0.0f, 0.0f, -1.0f );

    // The rotation whose forward axis is `travel` — the shortest arc from kLightForwardAxis, so roll stays
    // zero for every heading except the exact opposite one (a light aimed along +Z), which turns about +Y.
    // Roll does not change a directional light. This is the inverse of ECS::Rules::DirectionalLightTravel and
    // the one way the clock, the editor's direction widget and the scene upgrade turn a heading into a Rotation.
    inline glm::quat RotationForTravel( const glm::vec3& travel )
    {
        const glm::vec3 to = glm::normalize( travel );
        const float     d  = glm::dot( kLightForwardAxis, to );
        if ( d < -0.999999f )
            return glm::quat( 0.0f, glm::vec3( 0.0f, 1.0f, 0.0f ) ); // 180 deg about +Y
        const glm::vec3 axis = glm::cross( kLightForwardAxis, to );
        return glm::normalize( glm::quat( 1.0f + d, axis ) );
    }
} // namespace Desert::Geometry
