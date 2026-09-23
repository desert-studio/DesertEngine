#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Render/Commands/HeightFogCommand.hpp>

#include <Common/Core/Logger.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::ECS
{
    /**
     * @brief Collects the scene's exponential height fog for the frame.
     *
     * A pure render-data collector: it reads one component
     * (plus the owning entity's transform, because the fog floor IS that transform's Y) and emits one
     * command. The GPU-owning half is Graphic::System::HeightFogRenderer.
     *
     * Safe to run in parallel with the other collectors: it only reads component state.
     */
    class HeightFogECSSystem final : public System
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
            std::vector<entt::entity> entities;
            auto                      view = registry.view<ECS::ExponentialHeightFogComponent>();
            for ( const auto entity : view )
            {
                // A hidden fog entity is NOT A CANDIDATE, rather than "the election still picks it and
                // then the frame drops it": with two fog volumes authored, hiding the one that currently
                // wins is how an artist asks for the other one. Dropping it after the election would
                // leave the scene with no fog at all and no way to say which volume was meant.
                if ( ECS::IsHidden( registry, entity ) )
                    continue;
                entities.push_back( entity );
            }

            if ( entities.empty() )
            {
                renderCommandBuffer.Emplace<Graphic::Render::HeightFogCommand>(
                     false, ECS::ExponentialHeightFogData{}, 0.0f );
                return;
            }

            // Lowest UUID drives the frame, exactly as the sky picks its own: "whichever
            // entity entt happened to visit first" changes when an unrelated component is added
            // somewhere else, and fog that thickens for no visible reason is a hard bug to describe.
            size_t chosen = 0;
            for ( size_t i = 1; i < entities.size(); ++i )
            {
                if ( EntityId( registry, entities[i] ) < EntityId( registry, entities[chosen] ) )
                    chosen = i;
            }

            if ( entities.size() > 1 && !m_DuplicateLogged )
            {
                LOG_WARN( "[HeightFog] {} Exponential Height Fog components in the scene; the one on "
                          "entity '{}' drives the frame (lowest id). The others are ignored.",
                          entities.size(), EntityName( registry, entities[chosen] ) );
                m_DuplicateLogged = true;
            }

            // The fog floor is the entity's own Y — one owner, never a second authored height.
            float fogHeightY = 0.0f;
            if ( auto* transform = registry.try_get<ECS::TransformComponent>( entities[chosen] ) )
                fogHeightY = transform->Translation.y;

            const auto& fog = registry.get<ECS::ExponentialHeightFogComponent>( entities[chosen] );
            renderCommandBuffer.Emplace<Graphic::Render::HeightFogCommand>( true, fog.Data, fogHeightY );
        }

    private:
        static uint64_t EntityId( entt::registry& registry, entt::entity entity )
        {
            if ( auto* id = registry.try_get<ECS::UUIDComponent>( entity ) )
                return static_cast<uint64_t>( id->UUID );
            return 0;
        }

        static std::string EntityName( entt::registry& registry, entt::entity entity )
        {
            if ( auto* tag = registry.try_get<ECS::TagComponent>( entity ) )
                return tag->Tag;
            return "<unnamed>";
        }

        // Describes the SCENE, not the frame, so it is said once.
        bool m_DuplicateLogged = false;
    };
} // namespace Desert::ECS
