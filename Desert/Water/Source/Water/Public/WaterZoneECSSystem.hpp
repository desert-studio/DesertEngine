#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Water/Public/WaterRiverBakes.hpp>
#include <Water/Public/WaterWaveSources.hpp>
#include <Water/Public/WaterShoreFieldSource.hpp>
#include <Water/Public/WaterZoneCommand.hpp>

#include <cstdint>
#include <memory>
#include <vector>
#include <Engine/ECS/TransformComponent.hpp>
#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterZoneComponent.hpp>

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
     * Each zone also carries the world's SHORE FIELD (Water::WaterShoreField), the one WaterShoreFieldSource
     * bakes for the registry; until the first bake is done no water is drawn.
     *
     * Safe to run in parallel with the other collectors: it reads component state, and the shore field's source
     * serialises its own two readers.
     */
    class WaterZoneECSSystem final : public System
    {
    public:
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

            // The world's wave record, refreshed by the module's Prepares hook before any system of the frame.
            const auto* const sources = registry.try_ctx<std::shared_ptr<WaterWaveSources>>();
            if ( sources == nullptr )
                return;
            const WaterWaveSources& waves = **sources;
            // The world's river bakes (WaterRiverBakes, refreshed right after the wave record): a river is drawn
            // from its bake, never baked here.
            const auto* const bakesLink = registry.try_ctx<std::shared_ptr<WaterRiverBakes>>();
            if ( bakesLink == nullptr )
                return;
            const WaterRiverBakes& bakes = **bakesLink;
            // A hidden body, a second ocean (WaterWaveSources) and a river with no bake (WaterRiverBakes refused
            // the world) are no water.
            const auto view   = registry.view<WaterBodyComponent, TransformComponent>();
            const auto absent = [&waves, &registry, &bakes, &view]( const entt::entity entity )
            {
                return ECS::IsHidden( registry, entity ) || waves.IsRefused( entity ) ||
                       ( view.get<WaterBodyComponent>( entity ).Data.Type == Water::WaterBodyType::River &&
                         bakes.Find( entity ) == nullptr );
            };

            // A hidden body is no water, as a hidden mesh is no geometry. The visible bodies are compared with the
            // rows the zones drew last frame; only a change builds new rows (immutable and shared by every zone's
            // draw, so the renderer may still hold the old ones). A steady frame allocates nothing here. A river's
            // rows follow its bake: any changed bake (WaterRiverBakes::Revision) builds new rows.
            const auto footprint = [&view]( const entt::entity entity )
            {
                const glm::vec3& location = view.get<TransformComponent>( entity ).GetTranslation();
                const glm::vec2  half     = 0.5f * view.get<WaterBodyComponent>( entity ).Data.OceanExtents;
                const glm::vec2  centre( location.x, location.z );
                return Water::WaterQuadTreeBody{
                     .FootprintMin = centre - half, .FootprintMax = centre + half, .SurfaceY = location.y };
            };
            // The wave lane a body is drawn with: its own, or, for a river that ends in an ocean (Water::RiverMouth),
            // that ocean's (faded in over Mouth.Radius by the tiles, WaterMouthWeight).
            const auto waveLaneOf = [&waves, &bakes, &view]( const entt::entity entity ) -> int32_t
            {
                if ( view.get<WaterBodyComponent>( entity ).Data.Type == Water::WaterBodyType::River )
                {
                    const WaterRiverBakes::River* const river = bakes.Find( entity );
                    if ( river != nullptr && river->Flow->Mouth )
                        return waves.LaneOf( static_cast<entt::entity>( river->Flow->Mouth->Into ) );
                }
                return waves.LaneOf( entity );
            };
            bool   same    = m_Bodies != nullptr && m_Bodies->RiverRevision == bakes.Revision();
            size_t visible = 0;
            for ( const entt::entity entity : view )
            {
                if ( !same )
                    break;
                if ( absent( entity ) )
                    continue;
                const size_t                            i    = visible++;
                const Graphic::System::WaterZoneBodies& rows = *m_Bodies;
                if ( i >= rows.Footprints.size() )
                {
                    same = false;
                    break;
                }
                const WaterBodyData& data     = view.get<WaterBodyComponent>( entity ).Data;
                const glm::vec3&     location = view.get<TransformComponent>( entity ).GetTranslation();
                same = static_cast<uint32_t>( entity ) == rows.Ids[i] && data.Type == rows.Types[i] &&
                       data.WaterMaterial == rows.Materials[i] &&
                       waveLaneOf( entity ) == rows.WaveLanes[i] &&
                       glm::vec2( location.x, location.z ) == rows.WaveOrigins[i];
                if ( same && data.Type != Water::WaterBodyType::River ) // a river's footprint follows from its bake
                {
                    const Water::WaterQuadTreeBody body = footprint( entity );
                    same                                = body.FootprintMin == rows.Footprints[i].FootprintMin &&
                           body.FootprintMax == rows.Footprints[i].FootprintMax &&
                           body.SurfaceY == rows.Footprints[i].SurfaceY;
                }
            }
            if ( !same || visible != m_Bodies->Footprints.size() )
            {
                auto rows           = std::make_shared<Graphic::System::WaterZoneBodies>();
                rows->RiverRevision = bakes.Revision();
                for ( const entt::entity entity : view )
                {
                    if ( absent( entity ) )
                        continue;
                    const WaterBodyData& data     = view.get<WaterBodyComponent>( entity ).Data;
                    const glm::vec3&     location = view.get<TransformComponent>( entity ).GetTranslation();
                    Water::WaterQuadTreeBody body     = footprint( entity );
                    std::shared_ptr<const Water::RiverFlowField> flow;
                    if ( data.Type == Water::WaterBodyType::River )
                    {
                        flow = bakes.Find( entity )->Flow; // present: absent() skipped a river without one
                        // A river is drawn over its ribbon at its mean level, over every ocean it crosses.
                        body = Water::WaterQuadTreeBody{
                             .FootprintMin = flow->Min,
                             .FootprintMax = flow->Size.x != 0u ? flow->Min + flow->Extent : flow->Min - 1.0f,
                             .SurfaceY     = flow->SurfaceY,
                             .Priority     = Water::WaterBodyPriority( data.Type ),
                             .Ribbon       = flow };
                    }
                    rows->Footprints.push_back( std::move( body ) );
                    rows->Ids.push_back( static_cast<uint32_t>( entity ) );
                    rows->Types.push_back( data.Type );
                    rows->Flows.push_back( std::move( flow ) );
                    rows->Materials.push_back( data.WaterMaterial );
                    rows->WaveLanes.push_back( waveLaneOf( entity ) );
                    rows->WaveOrigins.emplace_back( location.x, location.z );
                }
                m_Bodies = std::move( rows );
            }
            if ( m_Bodies->Footprints.empty() )
                return;

            const auto* const source = registry.try_ctx<std::shared_ptr<WaterShoreFieldSource>>();
            const auto        shore  = source != nullptr ? ( *source )->Update( registry ) : nullptr;
            if ( shore == nullptr )
                return; // the first bake is running: a later frame draws the water

            for ( const entt::entity entity : zones )
            {
                if ( ECS::IsHidden( registry, entity ) )
                    continue;
                const WaterZoneData& data     = zones.get<WaterZoneComponent>( entity ).Data;
                const glm::vec3&     location = zones.get<TransformComponent>( entity ).GetTranslation();

                Graphic::System::WaterZoneDraw draw;
                draw.Zone.ZoneCenter         = glm::vec2( location.x, location.z );
                draw.Zone.ZoneExtent         = data.ZoneExtent;
                draw.Zone.TileSize           = data.TileSize;
                draw.Zone.TessellationFactor = data.TessellationFactor;
                draw.Zone.LODScale           = data.LODScale;
                draw.Bodies                  = m_Bodies;
                draw.Waves                   = waves.Table();
                draw.Shore                   = shore;
                renderCommandBuffer.Emplace<Graphic::Render::WaterZoneCommand>( std::move( draw ) );
            }
        }

    private:
        // The visible bodies the zones drew last frame (rebuilt only on a change).
        std::shared_ptr<const Graphic::System::WaterZoneBodies> m_Bodies;
    };
} // namespace Desert::ECS
