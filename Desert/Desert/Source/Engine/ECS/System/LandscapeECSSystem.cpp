#include "LandscapeECSSystem.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/Render/Commands/DrawLandscapeTileCommand.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeWeightmap.hpp>
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
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
                 .MipLevels  = {},
            };
            spec.Data = std::move( bytes );

            // A derived copy: the recipe is the component's samples, which the scene owns, so the owner is
            // the renderer side that re-derives it — releasing it loses nothing.
            const Graphic::ResourceAttributionScope owned( Graphic::ResourceOwner::SceneRenderer );
            return Graphic::Image2D::Create( spec );
        }

        // The tile's weight layers as one RGBA8 texel per sample (LandscapeWeightmapTexels): no neighbour
        // ring — the surface samples it at the vertex's own sample, which every tile holds for its edges.
        std::shared_ptr<Graphic::Image2D> CreateWeightmap( const Landscape::LandscapeTileData& tile, int32_t tileX,
                                                           int32_t tileZ, std::vector<unsigned char> texels )
        {
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = "LandscapeWeights(" + std::to_string( tileX ) + "," + std::to_string( tileZ ) + ")",
                 .Width      = tile.SamplesX(),
                 .Height     = tile.SamplesZ(),
                 .Format     = Core::Formats::ImageFormat::RGBA8F,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
                 .MipLevels  = {},
            };
            spec.Data = std::move( texels );

            // Derived from the component's weights, like the heightmap copy above.
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
            bool                     Dirty        = false;
            bool                     WeightsDirty = false;
        };
        using Coord = std::tuple<uint64_t, int32_t, int32_t>;
        std::vector<Drawable>   drawables;
        std::map<Coord, size_t> byCoord;

        // Unloaded tiles are in neither list (the loader said why; the sweep below frees them).
        const DrawableLandscape landscape = DrawableLandscapeTiles( registry );
        for ( const RefusedLandscapeTile& refused : landscape.Refused )
        {
            if ( !m_Warned.insert( refused.Entity ).second )
                continue;
            const auto& tileComp = registry.get<LandscapeTileComponent>( refused.Entity );
            LOG_WARN( "[Landscape] tile ({}, {}) is not drawn: {}", tileComp.TileX, tileComp.TileZ,
                      refused.Reason );
        }
        for ( const LandscapeTileRef& tile : landscape.Tiles )
        {
            const entt::entity            entity   = tile.Entity;
            const LandscapeTileComponent& tileComp = *tile.Component;
            Landscape::LandscapeTileData& heights  = *tile.Component->Heights;
            m_Warned.erase( entity );

            // ALWAYS taken, drawn or hidden: the list is "what the GPU copy does not have yet", and a hidden
            // tile's copy is refreshed like any other so showing it again shows the current heights.
            Drawable d;
            d.Entity = entity;
            d.Root   = tile.Root;
            d.Dirty  = !heights.TakeDirtyRects( Landscape::LandscapeDirtyConsumer::Gpu ).empty();
            // Same rule for the weightmap copy: taken always, so a hidden tile shows its current paint.
            d.WeightsDirty = !heights.TakeDirtyRects( Landscape::LandscapeDirtyConsumer::Weights ).empty();
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

        // Each root's look, resolved once per frame and shared by all its tiles. The material is a `.demat`
        // like every other: its values (Tint, DetailTiling and the u_GrassTex/u_RockTex/u_SnowTex layers)
        // arrive as named overrides that TerrainRenderer applies on top of the shader's schema defaults. A
        // root without a LandscapeMaterial draws with those defaults and every layer on Auto.
        struct Surface
        {
            glm::vec3                  LayerModes = glm::vec3( 0.0f );
            Graphic::MaterialOverrides Overrides;
            // The root's weight layers, which give each tile channel its colour and blend by name.
            std::vector<LandscapeLayerInfo> Layers;
        };
        std::map<uint64_t, Surface> surfaces;
        const auto                  surfaceOf = [&]( const Common::UUID& rootId ) -> const Surface&
        {
            const auto [it, inserted] = surfaces.try_emplace( static_cast<uint64_t>( rootId ) );
            if ( !inserted )
                return it->second;
            const entt::entity rootEntity = FindLandscapeRootEntity( registry, rootId );
            if ( rootEntity != entt::null && registry.has<LandscapeComponent>( rootEntity ) )
                it->second.Layers = registry.get<LandscapeComponent>( rootEntity ).Layers;
            for ( const auto entity : registry.view<LandscapeMaterialComponent, UUIDComponent>() )
            {
                if ( registry.get<UUIDComponent>( entity ).UUID != rootId )
                    continue;
                const LandscapeMaterialData& look = registry.get<LandscapeMaterialComponent>( entity ).Data;
                it->second.LayerModes =
                     glm::vec3( static_cast<float>( look.GrassMode ), static_cast<float>( look.RockMode ),
                                static_cast<float>( look.SnowMode ) );
                const auto raw = static_cast<uint64_t>( look.Material );
                if ( raw == 0 )
                    break;
                auto* materials = Runtime::ResourceRegistry::GetMaterialService();
                if ( ( materials == nullptr ||
                       !materials->ResolveOverrides( look.Material, it->second.Overrides ) ) &&
                     m_WarnedMaterials.insert( raw ).second )
                {
                    // Not a silent default: the scene names a material the asset database does not have, and
                    // the landscape then renders in the shader's defaults. Said once per handle, not per frame.
                    LOG_WARN( "[Landscape] material handle {} does not resolve to a registered material — the "
                              "landscape renders with the Terrain shader's own defaults.",
                              raw );
                }
                break;
            }
            return it->second;
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
            { return ( n != nullptr ) ? &*registry.get<LandscapeTileComponent>( n->Entity ).Heights : nullptr; };
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
                neighbourDirty = neighbourDirty || ( ( n != nullptr ) && n->Dirty );

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

            if ( d.WeightsDirty )
                m_WarnedWeights.erase( d.Entity );
            UpdateWeightmap( gpu, heights, d.Entity, tileComp, d.WeightsDirty );

            if ( ECS::IsHidden( registry, d.Entity ) )
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
            const Surface& surface = surfaceOf( Common::UUID( tileComp.Landscape ) );

            Graphic::System::LandscapeWeightDraw weights;
            if ( gpu.Weightmap )
            {
                const Landscape::LandscapeWeightChannels channels =
                     Landscape::ResolveLandscapeWeightChannels( heights, surface.Layers );
                weights.Weightmap  = gpu.Weightmap.get();
                weights.LayerCount = channels.Count;
                weights.Colors     = channels.Colors;
                weights.AlphaBlend = channels.AlphaBlend;
                if ( !channels.Unknown.empty() && m_WarnedWeights.insert( d.Entity ).second )
                {
                    // Not dropped silently: the tile keeps the layer's weights (renaming it back on the root
                    // restores it), it is just not drawn while the root does not name it.
                    std::string names;
                    for ( const std::string& name : channels.Unknown )
                        names += ( names.empty() ? "" : ", " ) + name;
                    LOG_WARN( "[Landscape] tile ({}, {}) paints layer(s) {} that its landscape does not list; "
                              "they are not drawn",
                              tileComp.TileX, tileComp.TileZ, names );
                }
            }
            renderCommandBuffer.Emplace<Graphic::Render::DrawLandscapeTileCommand>(
                 gpu.Heightmap.get(), draw, surface.LayerModes, surface.Overrides, weights );
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
        for ( auto it = m_WarnedWeights.begin(); it != m_WarnedWeights.end(); )
            it = registry.valid( *it ) ? std::next( it ) : m_WarnedWeights.erase( it );
    }

    void LandscapeECSSystem::UpdateWeightmap( TileGpu& gpu, const Landscape::LandscapeTileData& heights,
                                              entt::entity entity, const LandscapeTileComponent& tileComp,
                                              bool weightsDirty )
    {
        if ( heights.WeightLayers().empty() )
        {
            gpu.Weightmap.reset();
            return;
        }
        const bool sameSize =
             gpu.Weightmap && gpu.WeightmapX == heights.SamplesX() && gpu.WeightmapZ == heights.SamplesZ();
        if ( sameSize && !weightsDirty )
            return;

        std::vector<unsigned char> texels = Landscape::LandscapeWeightmapTexels( heights );
        if ( sameSize )
        {
            // In place: the image, and with it the tile's material and descriptor, stay the same.
            const Common::BoolResultStr written = gpu.Weightmap->SetData( std::move( texels ) );
            if ( written.IsSuccess() )
                return;
            if ( m_WarnedWeights.insert( entity ).second )
                LOG_ERROR( "[Landscape] tile ({}, {}): the {} x {} RGBA8 weightmap could not be written ({}); "
                           "the tile's painted layers are not drawn",
                           tileComp.TileX, tileComp.TileZ, gpu.WeightmapX, gpu.WeightmapZ, written.GetError() );
            gpu.Weightmap.reset();
            return;
        }

        gpu.Weightmap  = CreateWeightmap( heights, tileComp.TileX, tileComp.TileZ, std::move( texels ) );
        gpu.WeightmapX = heights.SamplesX();
        gpu.WeightmapZ = heights.SamplesZ();
        if ( !gpu.Weightmap && m_WarnedWeights.insert( entity ).second )
            LOG_ERROR( "[Landscape] tile ({}, {}): the {} x {} RGBA8 weightmap could not be created; the tile's "
                       "painted layers are not drawn",
                       tileComp.TileX, tileComp.TileZ, gpu.WeightmapX, gpu.WeightmapZ );
    }
} // namespace Desert::ECS
