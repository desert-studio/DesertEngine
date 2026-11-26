#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Projection.hpp>

namespace Desert::ECS
{
    // "Reflected render-data block": editable, reflected fields the editor draws and the renderer maps
    // to its GPU representation. This is the general concept — a surface material (its template's
    // MaterialLayout) is just ONE specialization; camera and lights are others. NOT a material, hence the member
    // is `Data`.
    struct CameraData
    {
        REFLECT()

        // UE's UCameraComponent::bAutoActivateForPlayer: Play views through this camera when the pawn has
        // none of its own. Off by default, because a default of true on EVERY camera made "which one?" a
        // question of entity order; more than one set is refused at Play (Core::Scene::ResolveViewTarget).
        PROPERTY( DisplayName( "Auto Activate for Player" ), Category( "Camera" ),
                  Tooltip( "Play views through this camera when the player's pawn has no camera of its own. "
                           "At most one camera in a level may have it." ) )
        bool AutoActivateForPlayer = false;

        PROPERTY( DisplayName( "Field of View" ), Category( "Camera" ), Range( 10.0f, 120.0f ),
                  Header( "Projection" ), Units( "deg" ), Summary,
                  Tooltip( "Vertical field of view, in degrees." ) )
        float FOV = 45.0f;

        PROPERTY( DisplayName( "Near" ), Category( "Camera" ), Range( 1.0f, 1000.0f ), Length,
                  Tooltip( "Near clip plane distance. Anything closer is not drawn." ) )
        float Near = Core::kDefaultNearPlane;

        // The range reaches 100 km because reversed-Z made it affordable — see Core/Camera.hpp for why
        // the default is 50 km and why it used to be 1 km. A slider that stopped at 10 km would have made
        // the new default unreachable from the editor, which is a dead setting by another name.
        PROPERTY( DisplayName( "Far" ), Category( "Camera" ), Range( 1000.0f, 10000000.0f ), Length,
                  Tooltip( "Far clip plane distance. Anything beyond is not drawn." ) )
        float Far = Core::kDefaultFarPlane;
    };

    struct CameraComponent
    {
        COMPONENT( Key( "Camera" ), Block( Data ), Run( ActorsAndUI ) )
        std::shared_ptr<Core::Camera> Camera;
        CameraData                    Data;
    };
} // namespace Desert::ECS
