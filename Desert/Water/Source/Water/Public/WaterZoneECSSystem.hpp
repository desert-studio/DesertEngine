#pragma once

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/System/WindField.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Water/Public/WaterWavesService.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Landscape/Public/LandscapeRootOf.hpp>
#include <LandscapeCore/Public/LandscapeData.hpp>
#include <Water/Public/WaterBodyQuery.hpp>
#include <Water/Public/WaterZoneCommand.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
     * Each zone also carries its WATER INFO GROUND (Water::WaterInfoGround, UE's WaterInfo ground depth): the
     * landscape under the zone seen from above, one texel per landscape sample, baked when the zone, the tiles
     * or their heights change (LandscapeDirtyConsumer::Water) and shared with the renderer until then.
     *
     * Safe to run in parallel with the other collectors: it reads component state and takes only its own
     * consumer's dirty list of each tile (LandscapeTileData keeps one list per consumer).
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
                if ( ECS::IsHidden( registry, entity ) )
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
                       glm::vec2( location.x, location.z ) == rows.WaveOrigins[i] &&
                       data.TargetWaveMaskDepth == rows.WaveMaskDepths[i];
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
                    if ( ECS::IsHidden( registry, entity ) )
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
                    rows->WaveMaskDepths.push_back( data.TargetWaveMaskDepth );
                }
                m_Bodies = std::move( rows );
            }
            if ( m_Bodies->Footprints.empty() )
                return;

            // The ground every zone's WaterInfo is baked from; a height edit on any tile re-bakes. The rows are
            // the system's own storage, refilled each frame.
            std::vector<World::Landscape::LandscapeRayTile>& ground = m_GroundRows;
            ground.clear();
            uint64_t   groundKey = 1469598103934665603ull;
            float      spacingCm = 0.0f;
            bool       edited    = false;
            const auto mix       = [&groundKey]( const uint64_t v )
            { groundKey = ( groundKey ^ v ) * 1099511628211ull; };
            for ( const LandscapeTileRef& tile : DrawableLandscapeTiles( registry ).Tiles )
            {
                World::Landscape::LandscapeTileData& heights = *tile.Component->Heights;
                if ( !heights.DirtyRects( World::Landscape::LandscapeDirtyConsumer::Water ).empty() )
                {
                    (void)heights.TakeDirtyRects( World::Landscape::LandscapeDirtyConsumer::Water );
                    edited = true;
                }
                ground.push_back( { &heights, tile.Frame } );
                mix( reinterpret_cast<uintptr_t>( &heights ) );
                mix( static_cast<uint64_t>( static_cast<uint32_t>( tile.Component->TileX ) ) << 32 |
                     static_cast<uint32_t>( tile.Component->TileZ ) );
                mix( std::bit_cast<uint32_t>( tile.Root.Origin.x ) ^
                     ( uint64_t( std::bit_cast<uint32_t>( tile.Root.Origin.y ) ) << 32 ) );
                mix( std::bit_cast<uint32_t>( tile.Root.Origin.z ) ^
                     ( uint64_t( std::bit_cast<uint32_t>( tile.Root.ZScale ) ) << 32 ) );
                spacingCm = spacingCm == 0.0f ? tile.Root.SpacingCm : std::min( spacingCm, tile.Root.SpacingCm );
            }
            if ( edited )
                m_Ground.clear();

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
                draw.Ground                  = GroundFor( entity, ground, groundKey, spacingCm, draw.Zone );
                draw.Gravity                 = m_Settings->Gravity;
                renderCommandBuffer.Emplace<Graphic::Render::WaterZoneCommand>( std::move( draw ) );
            }
        }

    private:
        // The zone's baked ground, re-baked only when its key moves. The grid is one texel per landscape sample
        // (the finest root's spacing) over the zone, the GPU's bilinear filter then interpolating the samples as
        // the landscape's own triangles do; a zone over no landscape has none (no surf, no shore).
        std::shared_ptr<const Water::WaterInfoGround>
        GroundFor( const entt::entity zone, const std::vector<World::Landscape::LandscapeRayTile>& ground,
                   uint64_t key, const float spacingCm, const Water::WaterQuadTreeDesc& desc )
        {
            if ( ground.empty() || spacingCm <= 0.0f || desc.ZoneExtent.x <= 0.0f )
                return nullptr;
            const float    extent = std::max( desc.ZoneExtent.x, desc.ZoneExtent.y );
            const uint32_t size   = static_cast<uint32_t>(
                 std::clamp( std::ceil( extent / spacingCm ), 16.0f, float( kMaxGroundTexels ) ) );
            key                  = ( key ^ std::bit_cast<uint32_t>( desc.ZoneCenter.x ) ) * 1099511628211ull;
            key                  = ( key ^ std::bit_cast<uint32_t>( desc.ZoneCenter.y ) ) * 1099511628211ull;
            key                  = ( key ^ std::bit_cast<uint32_t>( extent ) ) * 1099511628211ull;
            CachedGround& cached = m_Ground[zone];
            if ( cached.Ground == nullptr || cached.Key != key )
            {
                cached.Key    = key;
                cached.Ground = std::make_shared<const Water::WaterInfoGround>(
                     Water::BakeWaterInfoGround( ground, desc.ZoneCenter, extent, size ) );
            }
            return cached.Ground;
        }

        // UE's WaterInfo render target is capped the same way (r.Water.WaterInfo.RenderTargetResolution 2048).
        static constexpr uint32_t kMaxGroundTexels = 2048u;

        struct CachedGround
        {
            uint64_t                                      Key = 0;
            std::shared_ptr<const Water::WaterInfoGround> Ground;
        };
        const Core::SceneSettings*                     m_Settings = nullptr;
        std::unordered_map<entt::entity, CachedGround> m_Ground;
        // The visible bodies the zones drew last frame (rebuilt only on a change) and the ground rows' storage.
        std::shared_ptr<const Graphic::System::WaterZoneBodies> m_Bodies;
        std::vector<World::Landscape::LandscapeRayTile>         m_GroundRows;
        std::vector<Assets::AssetHandle>                        m_WindStated; // StateWaveWinds' frame rows
    };
} // namespace Desert::ECS
