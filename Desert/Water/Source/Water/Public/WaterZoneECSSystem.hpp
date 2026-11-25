#pragma once

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
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
        // The settings of the scene the zones draw in (Core::Scene::GetSettings, owned by the scene for its whole
        // life): their gravity is what the rivers' falls accelerate by. Not the Scene itself - this header stays
        // out of Scene.hpp's include graph.
        explicit WaterZoneECSSystem( const Core::SceneSettings& settings ) : m_Settings( &settings )
        {
        }

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
            // A hidden body, and a second ocean (WaterWaveSources), is no water.
            const auto absent = [&waves, &registry]( const entt::entity entity )
            { return ECS::IsHidden( registry, entity ) || waves.IsRefused( entity ); };

            // A hidden body is no water, as a hidden mesh is no geometry. The visible bodies are compared with the
            // rows the zones drew last frame; only a change builds new rows (immutable and shared by every zone's
            // draw, so the renderer may still hold the old ones). A steady frame allocates nothing here.
            const auto view      = registry.view<WaterBodyComponent, TransformComponent>();
            const auto footprint = [&view]( const entt::entity entity )
            {
                const glm::vec3& location = view.get<TransformComponent>( entity ).GetTranslation();
                const glm::vec2  half     = 0.5f * view.get<WaterBodyComponent>( entity ).Data.OceanExtents;
                const glm::vec2  centre( location.x, location.z );
                return Water::WaterQuadTreeBody{
                     .FootprintMin = centre - half, .FootprintMax = centre + half, .SurfaceY = location.y };
            };
            // A river's bake input and key (0 for an ocean): the rows re-bake a river only when its key changed.
            const auto riverInput = [&view]( const entt::entity entity )
            {
                const WaterBodyData& data = view.get<WaterBodyComponent>( entity ).Data;
                return Water::RiverBakeInput{ .LocalPoints = RiverPointsOf( data ),
                                              .Origin    = view.get<TransformComponent>( entity ).GetTranslation(),
                                              .Width     = data.RiverWidth,
                                              .FlowSpeed = data.RiverFlowSpeed,
                                              .FlowPeriod = data.RiverFlowPeriod };
            };
            const auto riverKey = [&view, &riverInput]( const entt::entity entity ) -> uint64_t
            {
                if ( view.get<WaterBodyComponent>( entity ).Data.Type != Water::WaterBodyType::River )
                    return 0u;
                return Water::RiverBakeKey( riverInput( entity ) ) | 1u; // never 0 for a river
            };
            bool   same    = m_Bodies != nullptr;
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
                       waves.LaneOf( entity ) == rows.WaveLanes[i] &&
                       glm::vec2( location.x, location.z ) == rows.WaveOrigins[i];
                if ( same && data.Type == Water::WaterBodyType::River )
                    same = riverKey( entity ) == rows.RiverKeys[i]; // the footprint follows from the bake
                else if ( same )
                {
                    const Water::WaterQuadTreeBody body = footprint( entity );
                    same                                = body.FootprintMin == rows.Footprints[i].FootprintMin &&
                           body.FootprintMax == rows.Footprints[i].FootprintMax &&
                           body.SurfaceY == rows.Footprints[i].SurfaceY;
                }
            }
            if ( !same || visible != m_Bodies->Footprints.size() )
            {
                auto rows = std::make_shared<Graphic::System::WaterZoneBodies>();
                for ( const entt::entity entity : view )
                {
                    if ( absent( entity ) )
                        continue;
                    const WaterBodyData& data     = view.get<WaterBodyComponent>( entity ).Data;
                    const glm::vec3&     location = view.get<TransformComponent>( entity ).GetTranslation();
                    Water::WaterQuadTreeBody body     = footprint( entity );
                    std::shared_ptr<const Water::RiverFlowField> flow;
                    const uint64_t                               key = riverKey( entity );
                    if ( key != 0u )
                    {
                        // The last rows' bake of the same river is reused; a changed river is baked here, once.
                        if ( m_Bodies != nullptr )
                            for ( size_t k = 0; k < m_Bodies->RiverKeys.size() && flow == nullptr; ++k )
                                if ( m_Bodies->RiverKeys[k] == key )
                                    flow = m_Bodies->Flows[k];
                        if ( flow == nullptr )
                            flow = Water::BakeRiver( riverInput( entity ) );
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
                    rows->RiverKeys.push_back( key );
                    rows->Materials.push_back( data.WaterMaterial );
                    rows->WaveLanes.push_back( waves.LaneOf( entity ) );
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
                draw.Gravity                 = m_Settings->Gravity;
                renderCommandBuffer.Emplace<Graphic::Render::WaterZoneCommand>( std::move( draw ) );
            }
        }

    private:
        const Core::SceneSettings*                     m_Settings = nullptr;
        // The visible bodies the zones drew last frame (rebuilt only on a change).
        std::shared_ptr<const Graphic::System::WaterZoneBodies> m_Bodies;
    };
} // namespace Desert::ECS
