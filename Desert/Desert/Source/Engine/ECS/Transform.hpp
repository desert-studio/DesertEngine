#pragma once

// The transform VALUE types - LocalTransform (parent-relative) and WorldTransform (the cached world) - apart
// from the component, so Entity.hpp can return a WorldTransform by value without pulling the component,
// reflection and asset headers into every translation unit that names an entity.

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Desert::ECS
{
    // A parent-relative transform AS A VALUE: what a TransformComponent holds, what a caller builds before
    // handing it to SetLocalTransform, and what code that never touches a registry (gizmo maths, prefab
    // records, tests) passes around. UE's FTransform next to USceneComponent::RelativeLocation/Rotation/Scale.
    //
    // LARGE WORLD COORDINATES (WP-LWC, Docs/contracts/LWC.md; UE5 FTransform/FVector are double): a position is
    // DOUBLE - 1 unit = 1 cm, so a float holds 0.125 cm at 20 km and 16 cm at 2000 km. Rotation and Scale stay
    // float: an orientation or a scale factor has no large magnitude. Every float consumer narrows EXPLICITLY at
    // its own boundary (glm::vec3( Translation ), WorldTransform::ToFloatMatrix) - the renderer turns camera-
    // relative in pass 3, physics/net/World Partition in passes 4..6; nothing narrows by an implicit conversion.
    struct LocalTransform
    {
        glm::dvec3 Translation = { 0.0, 0.0, 0.0 };
        glm::vec3  Rotation    = { 0.0f, 0.0f, 0.0f };
        glm::vec3  Scale       = { 1.0f, 1.0f, 1.0f };

        // Rotation * scale: the part of the transform that has no large magnitude.
        [[nodiscard]] glm::mat3 GetBasis() const
        {
            return glm::mat3( glm::toMat4( glm::quat( Rotation ) ) ) *
                   glm::mat3( glm::scale( glm::mat4( 1.0f ), Scale ) );
        }
        // The local TRS as a float matrix: an explicit narrowing of Translation, for parent-relative maths whose
        // magnitudes are small (a child's offset, a gizmo's local delta, a prefab record). A root's world
        // position is WorldTransform::Origin, never this.
        [[nodiscard]] glm::mat4 GetTransform() const
        {
            return glm::translate( glm::mat4( 1.0f ), glm::vec3( Translation ) ) *
                   glm::toMat4( glm::quat( Rotation ) ) * glm::scale( glm::mat4( 1.0f ), Scale );
        }

        [[nodiscard]] bool operator==( const LocalTransform& other ) const
        {
            return Translation == other.Translation && Rotation == other.Rotation && Scale == other.Scale;
        }
        [[nodiscard]] bool operator!=( const LocalTransform& other ) const
        {
            return !( *this == other );
        }
    };

    // A WORLD transform (UE USceneComponent::ComponentToWorld, an FTransform of doubles): the origin in double,
    // rotation*scale as a float basis. Composition and point transforms run in double; ToFloatMatrix is the one
    // named narrowing for a consumer that still takes an absolute float matrix (renderer until LWC pass 3).
    struct WorldTransform
    {
        glm::dvec3 Origin = { 0.0, 0.0, 0.0 };
        glm::mat3  Basis  = glm::mat3( 1.0f );

        [[nodiscard]] static WorldTransform FromLocal( const LocalTransform& local )
        {
            return WorldTransform{ local.Translation, local.GetBasis() };
        }
        // parent * child: child's origin goes through the parent's basis in double.
        [[nodiscard]] WorldTransform operator*( const LocalTransform& local ) const
        {
            return WorldTransform{ Origin + glm::dmat3( Basis ) * local.Translation, Basis * local.GetBasis() };
        }
        [[nodiscard]] glm::dvec3 TransformPoint( const glm::dvec3& point ) const
        {
            return Origin + glm::dmat3( Basis ) * point;
        }
        // EXPLICIT NARROWING of the origin to float (absolute world-space matrix).
        [[nodiscard]] glm::mat4 ToFloatMatrix() const
        {
            glm::mat4 matrix( Basis );
            matrix[3] = glm::vec4( glm::vec3( Origin ), 1.0f );
            return matrix;
        }

        /** The full-precision matrix, for tools that bake world positions in double (normal bake, UV, paint
         * groups). */
        [[nodiscard]] glm::dmat4 ToDoubleMatrix() const
        {
            glm::dmat4 m = glm::dmat4( glm::dmat3( Basis ) );
            m[3]         = glm::dvec4( Origin, 1.0 );
            return m;
        }
        [[nodiscard]] bool operator==( const WorldTransform& other ) const
        {
            return Origin == other.Origin && Basis == other.Basis;
        }
    };
} // namespace Desert::ECS
