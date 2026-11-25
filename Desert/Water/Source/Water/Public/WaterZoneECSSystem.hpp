#pragma once

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/System/WindField.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Water/Public/WaterWavesService.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Water/Public/WaterOceanRule.hpp>
#include <Water/Public/WaterShoreFieldSource.hpp>
#include <Water/Public/WaterZoneCommand.hpp>

#include <algorithm>
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

        // THE OCEAN'S WIND (WATER-UNDERWATER): one truth, the scene's ECS::WindAt - the query foliage, clouds,
        // cloth and groom ask - at the first visible body naming each wave set (its wave origin, at its still
        // level). A wave set holds no wind of its own; WaterWavesService rebuilds the set's FFT spectrum when
        // this changes, so a WindSource edit moves the sea without a reload. Every frame, before the zone check:
        // a body's buoyancy floats on the same sea whether or not a zone draws it.
        void StateWaveWinds( entt::registry& registry )
        {
            auto* const service = Runtime::GetWaterWavesService();
            if ( service == nullptr )
                return;
            m_WindStated.clear();
            for ( const entt::entity entity : registry.view<WaterBodyComponent, TransformComponent>() )
            {
                const Assets::AssetHandle waves = registry.get<WaterBodyComponent>( entity ).Data.WaterWaves;
                if ( static_cast<uint64_t>( waves ) == 0 || ECS::IsHidden( registry, entity ) ||
                     std::find( m_WindStated.begin(), m_WindStated.end(), waves ) != m_WindStated.end() )
                    continue;
                m_WindStated.push_back( waves );
                const glm::vec3 wind =
                     WindAt( registry, registry.get<TransformComponent>( entity ).GetTranslation() ).Velocity;
                service->SetWind( waves, glm::vec2( wind.x, wind.z ) );
            }
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            StateWaveWinds( registry );

            const auto zones = registry.view<WaterZoneComponent, TransformComponent>();
            if ( zones.begin() == zones.end() )
                return;

            m_NoOcean.clear();
            auto* const service = Runtime::GetWaterWavesService();
            if ( const auto* const rule = registry.try_ctx<std::shared_ptr<WaterOceanRule>>();
                 rule != nullptr && service != nullptr )
                ( *rule )->Refused( registry, [service]( const Assets::AssetHandle& waves )
                                    { return service->IsFFTSet( waves ); }, m_NoOcean );
            // A hidden body, and a second ocean (WaterOceanRule), is no water.
            const auto absent = [this, &registry]( const entt::entity entity )
            {
                return ECS::IsHidden( registry, entity ) ||
                       std::find( m_NoOcean.begin(), m_NoOcean.end(), entity ) != m_NoOcean.end();
            };

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
                same = data.Type == rows.Types[i] && data.WaterMaterial == rows.Materials[i] &&
                       data.WaterWaves == rows.Waves[i] &&
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
                    rows->Types.push_back( data.Type );
                    rows->Flows.push_back( std::move( flow ) );
                    rows->RiverKeys.push_back( key );
                    rows->Materials.push_back( data.WaterMaterial );
                    rows->Waves.push_back( data.WaterWaves );
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
                draw.Shore                   = shore;
                draw.Gravity                 = m_Settings->Gravity;
                renderCommandBuffer.Emplace<Graphic::Render::WaterZoneCommand>( std::move( draw ) );
            }
        }

    private:
        const Core::SceneSettings*                     m_Settings = nullptr;
        // The visible bodies the zones drew last frame (rebuilt only on a change).
        std::shared_ptr<const Graphic::System::WaterZoneBodies> m_Bodies;
        std::vector<Assets::AssetHandle>                        m_WindStated; // StateWaveWinds' frame rows
        std::vector<entt::entity>                               m_NoOcean;    // WaterOceanRule's refusals
    };
} // namespace Desert::ECS
