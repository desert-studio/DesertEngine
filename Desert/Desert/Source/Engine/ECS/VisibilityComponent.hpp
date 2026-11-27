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

namespace Desert::ECS
{
    class VisibilityComponent;
    bool SetVisible( entt::registry& registry, entt::entity entity, bool visible );

    // Whether the entity is drawn (UE USceneComponent::bVisible). READ-ONLY, for the reason TransformComponent
    // is: every write goes through ECS::SetVisible, a registry.patch, so on_update<VisibilityComponent> fires
    // and Core::Scene's render-state dirty set sees the toggle at the source. Construction with a value
    // (emplace<VisibilityComponent>( e, false )) is not a write: it fires on_construct.
    class VisibilityComponent
    {
    public:
        VisibilityComponent() = default;
        explicit VisibilityComponent( bool visible ) : m_Visible( visible ) {}

        [[nodiscard]] bool IsVisible() const
        {
            return m_Visible;
        }

    private:
        bool m_Visible = true;

        friend bool SetVisible( entt::registry& registry, entt::entity entity, bool visible );
    };

    // THE ONE WRITER of VisibilityComponent (UE USceneComponent::SetVisibility -> MarkRenderStateDirty). Adds
    // the component when the entity has none (on_construct), patches it when the value changes (on_update), and
    // is no write at all when the value is unchanged. Returns whether anything changed. Scene::SetVisibleRecursive
    // calls it per entity of a subtree.
    inline bool SetVisible( entt::registry& registry, entt::entity entity, bool visible )
    {
        if ( auto* component = registry.try_get<VisibilityComponent>( entity ) )
        {
            if ( component->m_Visible == visible )
                return false;
            registry.patch<VisibilityComponent>( entity, [visible]( VisibilityComponent& c )
                                                 { c.m_Visible = visible; } );
            return true;
        }
        registry.emplace<VisibilityComponent>( entity, visible );
        return true;
    }
} // namespace Desert::ECS
