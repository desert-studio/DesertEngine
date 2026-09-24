#include "LandscapeECSSystem.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/Render/Commands/DrawLandscapeTileCommand.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Common/Core/Logger.hpp>

#include <cstring>
#include <map>
#include <tuple>
#include <optional>
#include <string>
#include <vector>

namespace Desert::ECS
{
    namespace
    {
        namespace Landscape = World::Landscape;

        // The root a tile names, in the frame LandscapeLayout computes with — the same construction the
        // scene loader validates tiles against (SceneSerializer.cpp, FindLandscapeRoot).
        std::optional<Landscape::LandscapeRoot> FindRoot( entt::registry& registry, const Common::UUID& id )
        {
            if ( id == Common::UUID::Null() )
                return std::nullopt;
            for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
            {
                if ( registry.get<UUIDComponent>( entity ).UUID != id )
                    continue;
                const auto&              component = registry.get<LandscapeComponent>( entity );
                Landscape::LandscapeRoot root;
                root.Origin       = glm::vec3( Entity( entity, registry ).GetWorldTransform()[3] );
                root.QuadsPerTile = component.QuadsPerTile;
                root.SpacingCm    = component.SpacingCm;
                root.ZScale       = component.ZScale;
                return root;
            }
            return std::nullopt;
        }

        std::shared_ptr<Graphic::Image2D> UploadHeightmap( const Landscape::LandscapeTileData&       tile,
                                                           const Landscape::LandscapeTileNeighbours& neighbours,
                                                           int32_t tileX, int32_t tileZ )
        {
            // The tile's samples with the one-sample ring from its neighbours (LandscapeBorderedSamples),
            // row-major, X fastest — the image's own row order, so the byte copy IS the upload layout (R16
            // texel = one uint16, host byte order, which is the device's on every platform this builds for).
            const std::vector<uint16_t> samples = Landscape::LandscapeBorderedSamples( tile, neighbours );
            std::vector<unsigned char>  bytes( samples.size() * sizeof( uint16_t ) );
            std::memcpy( bytes.data(), samples.data(), bytes.size() );

            Core::Formats::Image2DSpecification spec = {
                 .Tag        = "LandscapeTile(" + std::to_string( tileX ) + "," + std::to_string( tileZ ) + ")",
                 .Width      = tile.SamplesX() + 2u,
                 .Height     = tile.SamplesZ() + 2u,
                 .Format     = Core::Formats::ImageFormat::R16_UNORM,
                 .Mips       = 1,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
            };
            spec.Data = std::move( bytes );

            // A derived copy: the recipe is the component's samples, which the scene owns, so the owner is
            // the renderer side that re-derives it — releasing it loses nothing.
            const Graphic::ResourceAttributionScope owned( Graphic::ResourceOwner::SceneRenderer );
            return Graphic::Image2D::Create( spec );
        }
    } // namespace

    LandscapeECSSystem::~LandscapeECSSystem() = default;

    void LandscapeECSSystem::Update( entt::registry&                       registry,
                                     Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                                     const Common::Timestep& /*ts*/ )
    {
        // Pass 1: every drawable tile, by (root, tile x, tile z) — a tile's heightmap carries a ring row from
        // each loaded neighbour, so it cannot be built before its neighbours are known.
        struct Drawable
        {
            entt::entity             Entity = entt::null;
            Landscape::LandscapeRoot Root;
            bool                     Dirty = false;
        };
        using Coord = std::tuple<uint64_t, int32_t, int32_t>;
        std::vector<Drawable>   drawables;
        std::map<Coord, size_t> byCoord;

        auto view = registry.view<LandscapeTileComponent>();
        for ( const auto entity : view )
        {
            auto& tileComp = view.get<LandscapeTileComponent>( entity );
            if ( !tileComp.Heights.has_value() )
                continue; // not loaded, or refused at load (the loader said why); the sweep below frees it
            Landscape::LandscapeTileData& heights = *tileComp.Heights;

            const auto root = FindRoot( registry, tileComp.Landscape );
            const auto fits = root ? Landscape::CheckTileMatchesRoot( heights, *root )
                                   : Common::MakeError<bool>( "its root is not loaded or is not a landscape" );
            if ( !fits.IsSuccess() )
            {
                if ( m_Warned.insert( entity ).second )
                    LOG_WARN( "[Landscape] tile ({}, {}) is not drawn: {}", tileComp.TileX, tileComp.TileZ,
                              fits.GetError() );
                continue;
            }
            m_Warned.erase( entity );

            // ALWAYS taken, drawn or hidden: the list is "what the GPU copy does not have yet", and a hidden
            // tile's copy is refreshed like any other so showing it again shows the current heights.
            Drawable d;
            d.Entity = entity;
            d.Root   = *root;
            d.Dirty  = !heights.TakeDirtyRects().empty();
            byCoord.emplace( Coord{ static_cast<uint64_t>( Common::UUID( tileComp.Landscape ) ), tileComp.TileX,
                                    tileComp.TileZ },
                             drawables.size() );
            drawables.push_back( d );
        }

        const auto neighbourAt = [&]( const LandscapeTileComponent& c, int32_t dx, int32_t dz ) -> const Drawable*
        {
            const auto it = byCoord.find(
                 Coord{ static_cast<uint64_t>( Common::UUID( c.Landscape ) ), c.TileX + dx, c.TileZ + dz } );
            return it == byCoord.end() ? nullptr : &drawables[it->second];
        };

        // Pass 2: upload what changed, draw what is shown.
        for ( const Drawable& d : drawables )
        {
            const auto&                         tileComp = registry.get<LandscapeTileComponent>( d.Entity );
            const Landscape::LandscapeTileData& heights  = *tileComp.Heights;

            const Drawable* west  = neighbourAt( tileComp, -1, 0 );
            const Drawable* east  = neighbourAt( tileComp, 1, 0 );
            const Drawable* south = neighbourAt( tileComp, 0, -1 );
            const Drawable* north = neighbourAt( tileComp, 0, 1 );

            const auto heightsOf = [&]( const Drawable* n ) -> const Landscape::LandscapeTileData*
            { return n ? &*registry.get<LandscapeTileComponent>( n->Entity ).Heights : nullptr; };
            Landscape::LandscapeTileNeighbours neighbours;
            neighbours.West     = heightsOf( west );
            neighbours.East     = heightsOf( east );
            neighbours.South    = heightsOf( south );
            neighbours.North    = heightsOf( north );
            const uint32_t mask = Landscape::LandscapeNeighbourMask( neighbours );

            // The ring is a copy of the neighbours' rows, so a neighbour's edit, arrival or departure makes
            // this tile's copy stale as surely as its own edit does.
            bool neighbourDirty = false;
            for ( const Drawable* n : { west, east, south, north } )
                neighbourDirty = neighbourDirty || ( n && n->Dirty );

            TileGpu& gpu = m_Tiles[d.Entity];
            if ( d.Dirty || neighbourDirty || !gpu.Heightmap || gpu.SamplesX != heights.SamplesX() ||
                 gpu.SamplesZ != heights.SamplesZ() || gpu.NeighbourMask != mask )
            {
                gpu.Heightmap     = UploadHeightmap( heights, neighbours, tileComp.TileX, tileComp.TileZ );
                gpu.SamplesX      = heights.SamplesX();
                gpu.SamplesZ      = heights.SamplesZ();
                gpu.NeighbourMask = mask;
                if ( !gpu.Heightmap )
                {
                    LOG_ERROR( "[Landscape] tile ({}, {}): the {} x {} R16 heightmap could not be created; the "
                               "tile is not drawn",
                               tileComp.TileX, tileComp.TileZ, gpu.SamplesX + 2u, gpu.SamplesZ + 2u );
                    m_Tiles.erase( d.Entity );
                    continue;
                }
            }

            if ( IsHidden( registry, d.Entity ) )
                continue;

            const Landscape::LandscapeRoot&    root = d.Root;
            Graphic::System::LandscapeTileDraw draw;
            draw.OriginX       = root.Origin.x;
            draw.OriginZ       = root.Origin.z;
            draw.BaseY         = root.Origin.y;
            draw.SpacingCm     = root.SpacingCm;
            draw.ZScale        = root.ZScale;
            draw.FirstSampleX  = tileComp.TileX * static_cast<int32_t>( root.QuadsPerTile );
            draw.FirstSampleZ  = tileComp.TileZ * static_cast<int32_t>( root.QuadsPerTile );
            draw.QuadsPerTile  = root.QuadsPerTile;
            draw.NeighbourMask = mask;
            renderCommandBuffer.Emplace<Graphic::Render::DrawLandscapeTileCommand>( gpu.Heightmap.get(), draw );
        }

        // Release: the entity is gone, lost its tile component, or its heights were unloaded.
        for ( auto it = m_Tiles.begin(); it != m_Tiles.end(); )
        {
            const bool alive = registry.valid( it->first ) && registry.has<LandscapeTileComponent>( it->first ) &&
                               registry.get<LandscapeTileComponent>( it->first ).Heights.has_value();
            it = alive ? std::next( it ) : m_Tiles.erase( it );
        }
        for ( auto it = m_Warned.begin(); it != m_Warned.end(); )
            it = registry.valid( *it ) ? std::next( it ) : m_Warned.erase( it );
    }
} // namespace Desert::ECS
