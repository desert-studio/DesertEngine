#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/DecalComponent.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Render/Commands/DecalCommand.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>

namespace Desert::ECS
{
    /**
     * @brief Collects the scene's deferred decals for the frame (UE: the scene's FDeferredDecalProxy list).
     *
     * A pure render-data collector: per visible DecalComponent one command with the box in world (the entity's
     * world transform x scale( Extent )), its material, sort order, entity id and fade numbers. The per-view half
     * - the camera fade, the sort, the draws - is Graphic::System::DecalRenderer.
     *
     * Safe to run in parallel with the other collectors: it only reads component state.
     */
    class DecalECSSystem final : public System
    {
    public:
        using System::System;

        bool CanRunParallel() const override
        {
            return true;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            auto view = registry.view<DecalComponent, TransformComponent>();
            view.each(
                 [&]( auto entity, const auto& decal, const auto& transform )
                 {
                     // A hidden decal projects nothing (the outliner's eye, as for the lights).
                     if ( ECS::IsHidden( registry, entity ) )
                         return;

                     glm::mat4    worldTransform = transform.GetTransform();
                     entt::entity current        = entity;
                     while ( registry.has<RelationshipComponent>( current ) )
                     {
                         const auto& rel = registry.get<RelationshipComponent>( current );
                         if ( rel.Parent == entt::null )
                             break;
                         current = rel.Parent;
                         if ( registry.has<TransformComponent>( current ) )
                             worldTransform =
                                  registry.get<TransformComponent>( current ).GetTransform() * worldTransform;
                     }

                     Graphic::System::DecalProxy proxy;
                     proxy.DecalToWorld   = glm::scale( worldTransform, decal.Data.Extent );
                     proxy.Material       = decal.Data.Material;
                     proxy.EntityId       = EntityId( registry, entity );
                     proxy.SortOrder      = decal.Data.SortOrder;
                     proxy.ChannelMask    = decal.Data.ChannelMask;
                     proxy.FadeStart      = decal.Data.FadeStart;
                     proxy.FadeLength     = decal.Data.FadeLength;
                     proxy.FadeScreenSize = decal.Data.FadeScreenSize;
                     proxy.EdgeFade       = decal.Data.EdgeFade;
                     renderCommandBuffer.Emplace<Graphic::Render::DecalCommand>( proxy );
                 } );
        }

    private:
        static uint64_t EntityId( entt::registry& registry, entt::entity entity )
        {
            if ( auto* id = registry.try_get<ECS::UUIDComponent>( entity ) )
                return static_cast<uint64_t>( id->UUID );
            return 0;
        }
    };
} // namespace Desert::ECS
