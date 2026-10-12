#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Render/Commands/WaterZoneCommand.hpp>

#include <vector>

namespace Desert::ECS
{
    /**
     * @brief Collects the scene's water zones for the frame (WATER-W6).
     *
     * A pure render-data collector: every visible WaterZoneComponent (placed at its entity's X, Z) becomes one
     * WaterZoneCommand carrying the footprints of the water bodies (WaterBodyComponent: the entity's Y is the
     * still level, OceanExtents the footprint around it) — the water the zone's quadtree draws. The GPU half is
     * Graphic::System::WaterRenderer.
     *
     * Safe to run in parallel with the other collectors: it only reads component state.
     */
    class WaterZoneECSSystem final : public System
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
            const auto zones = registry.view<WaterZoneComponent, TransformComponent>();
            if ( zones.begin() == zones.end() )
                return;

            // A hidden body is no water, as a hidden mesh is no geometry.
            std::vector<Water::WaterQuadTreeBody> bodies;
            const auto                            view = registry.view<WaterBodyComponent, TransformComponent>();
            for ( const entt::entity entity : view )
            {
                if ( ECS::IsHidden( registry, entity ) )
                    continue;
                const glm::vec3& location = view.get<TransformComponent>( entity ).Translation;
                const glm::vec2  half     = 0.5f * view.get<WaterBodyComponent>( entity ).Data.OceanExtents;
                const glm::vec2  centre( location.x, location.z );
                bodies.push_back(
                     { .FootprintMin = centre - half, .FootprintMax = centre + half, .SurfaceY = location.y } );
            }
            if ( bodies.empty() )
                return;

            for ( const entt::entity entity : zones )
            {
                if ( ECS::IsHidden( registry, entity ) )
                    continue;
                const WaterZoneData& data     = zones.get<WaterZoneComponent>( entity ).Data;
                const glm::vec3&     location = zones.get<TransformComponent>( entity ).Translation;

                Graphic::System::WaterZoneDraw draw;
                draw.Zone.ZoneCenter         = glm::vec2( location.x, location.z );
                draw.Zone.ZoneExtent         = data.ZoneExtent;
                draw.Zone.TileSize           = data.TileSize;
                draw.Zone.TessellationFactor = data.TessellationFactor;
                draw.Zone.LODScale           = data.LODScale;
                draw.Bodies                  = bodies;
                renderCommandBuffer.Emplace<Graphic::Render::WaterZoneCommand>( std::move( draw ) );
            }
        }
    };
} // namespace Desert::ECS
