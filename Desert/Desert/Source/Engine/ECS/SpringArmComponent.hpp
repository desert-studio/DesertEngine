#pragma once

#include <entt/entt.hpp>

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

namespace Desert::ECS
{
    // UE USpringArmComponent: on a child entity of the pawn, whose rotation is the arm's (scripts pitch it, the
    // pawn yaws). Each frame PhysicsECSSystem places the arm's CAMERA child TargetArmLength behind the arm's
    // origin (along the arm's +Z, the camera's back) plus SocketOffset in arm space; with Do Collision Test a
    // sphere of Probe Size is swept from the origin to that point and the camera is pulled in to the first hit,
    // and it goes back out when the obstacle is gone. Camera Lag smooths the ORIGIN the arm hangs from (UE
    // bEnableCameraLag), never the collision. The solve is ECS/System/SpringArm.cpp.
    struct SpringArmData
    {
        REFLECT()

        PROPERTY( DisplayName( "Target Arm Length" ), Category( "Camera" ), Range( 0.0f, 5000.0f ), Length,
                  Summary )
        float TargetArmLength = 300.0f;

        // Arm-space offset of the camera at the arm's end (UE SocketOffset): (0, 60, 0) lifts it over the head.
        PROPERTY( DisplayName( "Socket Offset" ), Category( "Camera" ), Length )
        glm::vec3 SocketOffset = { 0.0f, 0.0f, 0.0f };

        PROPERTY( DisplayName( "Do Collision Test" ), Category( "Camera Collision" ) )
        bool DoCollisionTest = true;

        // Radius of the swept probe sphere (UE ProbeSize): how far the camera keeps from the wall it meets.
        PROPERTY( DisplayName( "Probe Size" ), Category( "Camera Collision" ), Range( 0.0f, 200.0f ), Length )
        float ProbeSize = 12.0f;

        PROPERTY( DisplayName( "Enable Camera Lag" ), Category( "Lag" ) )
        bool EnableCameraLag = false;

        // UE CameraLagSpeed: the origin closes this fraction of its distance per second (VInterpTo).
        PROPERTY( DisplayName( "Camera Lag Speed" ), Category( "Lag" ), Range( 0.0f, 1000.0f ) )
        float CameraLagSpeed = 10.0f;
    };

    struct SpringArmComponent
    {
        COMPONENT( Key( "SpringArm" ), Block( Data ), Run( ActorsAndUI ) )
        SpringArmData Data;

        // Transient: the lagged origin (valid once HasLaggedOrigin) and the length the last solve reached.
        glm::vec3 LaggedOrigin     = { 0.0f, 0.0f, 0.0f };
        bool      HasLaggedOrigin  = false;
        float     CurrentArmLength = 0.0f;
    };
} // namespace Desert::ECS
