#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Water/WaterBodyQuery.hpp>
#include <Engine/Graphic/Render/Commands/WaterZoneCommand.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
            std::vector<Assets::AssetHandle>      materials; // each body's WaterMaterial, in bodies' order
            std::vector<Assets::AssetHandle>      waves;     // each body's WaterWaves, in bodies' order
            std::vector<glm::vec2>                origins;   // each body's WaveOrigin (its X, Z)
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
                materials.push_back( view.get<WaterBodyComponent>( entity ).Data.WaterMaterial );
                waves.push_back( view.get<WaterBodyComponent>( entity ).Data.WaterWaves );
                origins.push_back( centre );
            }
            if ( bodies.empty() )
                return;

            // The ground every zone's WaterInfo is baked from; a height edit on any tile re-bakes.
            std::vector<World::Landscape::LandscapeRayTile> ground;
            uint64_t                                        groundKey = 1469598103934665603ull;
            float                                           spacingCm = 0.0f;
            bool                                            edited    = false;
            const auto mix = [&groundKey]( const uint64_t v ) { groundKey = ( groundKey ^ v ) * 1099511628211ull; };
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
                const glm::vec3&     location = zones.get<TransformComponent>( entity ).Translation;

                Graphic::System::WaterZoneDraw draw;
                draw.Zone.ZoneCenter         = glm::vec2( location.x, location.z );
                draw.Zone.ZoneExtent         = data.ZoneExtent;
                draw.Zone.TileSize           = data.TileSize;
                draw.Zone.TessellationFactor = data.TessellationFactor;
                draw.Zone.LODScale           = data.LODScale;
                draw.Bodies                  = bodies;
                draw.BodyMaterials           = materials;
                draw.BodyWaves               = waves;
                draw.BodyWaveOrigins         = origins;
                draw.Ground                  = GroundFor( entity, ground, groundKey, spacingCm, draw.Zone );
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
            const uint32_t size =
                 static_cast<uint32_t>( std::clamp( std::ceil( extent / spacingCm ), 16.0f, float( kMaxGroundTexels ) ) );
            key = ( key ^ std::bit_cast<uint32_t>( desc.ZoneCenter.x ) ) * 1099511628211ull;
            key = ( key ^ std::bit_cast<uint32_t>( desc.ZoneCenter.y ) ) * 1099511628211ull;
            key = ( key ^ std::bit_cast<uint32_t>( extent ) ) * 1099511628211ull;
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
        std::unordered_map<entt::entity, CachedGround> m_Ground;
    };
} // namespace Desert::ECS
