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
    // A parent-relative transform AS A VALUE: what a TransformComponent holds, what a caller builds before
    // handing it to SetLocalTransform, and what code that never touches a registry (gizmo maths, prefab
    // records, tests) passes around. UE's FTransform next to USceneComponent::RelativeLocation/Rotation/Scale.
    struct LocalTransform
    {
        glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation    = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale       = { 1.0f, 1.0f, 1.0f };

        [[nodiscard]] glm::mat4 GetTransform() const
        {
            return glm::translate( glm::mat4( 1.0f ), Translation ) * glm::toMat4( glm::quat( Rotation ) ) *
                   glm::scale( glm::mat4( 1.0f ), Scale );
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

    class TransformComponent;
    bool SetLocalTransform( entt::registry& registry, entt::entity entity, const LocalTransform& local );

    // Recomposes the cached WORLD transform of `root` (from its nearest transformed ancestor's cached world)
    // and of every entity under it, parent before child (UE USceneComponent::UpdateComponentToWorld, which
    // propagates to the attached children). Called by every hierarchy write: SetLocalTransform, Scene::Attach /
    // Detach, and Core::Scene's construct/replace hooks. Cost is the subtree, on the write; reads are a load.
    void UpdateWorldTransforms( entt::registry& registry, entt::entity root );
    // Whether `transform` still holds the root-only world its constructor gave it (a fresh emplace or a
    // registry.replace), i.e. its parent chain has not been composed into it yet. Core::Scene's hooks ask.
    [[nodiscard]] bool WorldTransformPending( const TransformComponent& transform );
    // The cached world transform of the nearest ancestor of `entity` that has a TransformComponent (identity
    // at a root): what a child composes under, and what an entity without a transform of its own stands at.
    [[nodiscard]] glm::mat4 ParentWorldTransform( const entt::registry& registry, entt::entity entity );

    // THE ENTITY'S LOCAL TRANSFORM, READ-ONLY. Nothing may write it in place: every write goes through
    // SetLocalTransform (below), which is a registry.patch, so EnTT's on_update<TransformComponent> fires for
    // every change and Core::Scene's render-state dirty set (UE MarkRenderStateDirty) sees it at the source —
    // a direct field write would move the entity on screen while the dirty set, and every persistent render
    // proxy fed from it, said nothing changed. Construction (emplace / AddComponent with a LocalTransform) is
    // not a write: it fires on_construct. Whole-object assignment stays possible only because EnTT's storage
    // moves components by assignment; replace a component with registry.replace, which fires on_update.
    class TransformComponent
    {
    public:
        TransformComponent() = default;
        TransformComponent( const LocalTransform& local ) : m_Local( local ), m_World( local.GetTransform() ) {}

        [[nodiscard]] const LocalTransform& GetLocal() const
        {
            return m_Local;
        }
        [[nodiscard]] const glm::vec3& GetTranslation() const
        {
            return m_Local.Translation;
        }
        [[nodiscard]] const glm::vec3& GetRotation() const
        {
            return m_Local.Rotation;
        }
        [[nodiscard]] const glm::vec3& GetScale() const
        {
            return m_Local.Scale;
        }
        [[nodiscard]] glm::mat4 GetTransform() const
        {
            return m_Local.GetTransform();
        }
        // THE CACHED WORLD TRANSFORM (UE USceneComponent::ComponentToWorld): parent world x local, kept current
        // by UpdateWorldTransforms on every hierarchy write, so a reader neither walks the parent chain nor
        // recomposes a TRS. Copies with the component (registry copy, duplicate) stay consistent with it.
        [[nodiscard]] const glm::mat4& GetWorldTransform() const
        {
            return m_World;
        }

    private:
        LocalTransform m_Local;
        glm::mat4      m_World = glm::mat4( 1.0f );
        // False until UpdateWorldTransforms composed the parent chain in; a constructed component holds the
        // root-only world (local), which is right for an unparented entity in a registry without hooks.
        bool m_WorldComposed = false;

        friend bool SetLocalTransform( entt::registry& registry, entt::entity entity, const LocalTransform& local );
        friend void UpdateWorldTransforms( entt::registry& registry, entt::entity root );
        friend bool WorldTransformPending( const TransformComponent& transform );
    };

    // THE ONE WRITER of a TransformComponent (UE USceneComponent::SetRelativeTransform, which ends in
    // MarkRenderTransformDirty). An unchanged value is not a write (UE early-outs the same way): no on_update,
    // so a system that re-asserts the same pose every frame (attachments, physics at rest) dirties nothing.
    // Returns whether the transform changed.
    inline bool SetLocalTransform( entt::registry& registry, entt::entity entity, const LocalTransform& local )
    {
        if ( registry.get<TransformComponent>( entity ).m_Local == local )
            return false;
        // The world cache (this entity's and its subtree's) is recomposed inside the patch, so every
        // on_update<TransformComponent> listener already reads the new world.
        registry.patch<TransformComponent>( entity,
                                            [&registry, entity, &local]( TransformComponent& component )
                                            {
                                                component.m_Local = local;
                                                UpdateWorldTransforms( registry, entity );
                                            } );
        return true;
    }

    // One field of the local transform; each is SetLocalTransform with the other two fields kept.
    inline bool SetLocalTranslation( entt::registry& registry, entt::entity entity, const glm::vec3& translation )
    {
        LocalTransform local = registry.get<TransformComponent>( entity ).GetLocal();
        local.Translation    = translation;
        return SetLocalTransform( registry, entity, local );
    }
    inline bool SetLocalRotation( entt::registry& registry, entt::entity entity, const glm::vec3& rotation )
    {
        LocalTransform local = registry.get<TransformComponent>( entity ).GetLocal();
        local.Rotation       = rotation;
        return SetLocalTransform( registry, entity, local );
    }
    inline bool SetLocalScale( entt::registry& registry, entt::entity entity, const glm::vec3& scale )
    {
        LocalTransform local = registry.get<TransformComponent>( entity ).GetLocal();
        local.Scale          = scale;
        return SetLocalTransform( registry, entity, local );
    }
} // namespace Desert::ECS
