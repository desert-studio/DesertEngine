#include "LandscapeECSSystem.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/ECS/System/LandscapeTileImage.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Render/Commands/DrawLandscapeTileCommand.hpp>
#include <Engine/Graphic/Render/Commands/DrawLandscapeGrassCommand.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/ECS/System/WindField.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeWeightmap.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <bit>
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

        // The tile's samples with the one-sample ring from its neighbours (LandscapeBorderedSamples), row-major,
        // X fastest - the image's own row order, so the byte copy IS the upload layout (R16 texel = one uint16,
        // host byte order, which is the device's on every platform this builds for).
        std::vector<unsigned char> HeightmapTexels( const Landscape::LandscapeTileData&       tile,
                                                    const Landscape::LandscapeTileNeighbours& neighbours )
        {
            const std::vector<uint16_t> samples = Landscape::LandscapeBorderedSamples( tile, neighbours );
            std::vector<unsigned char>  bytes( samples.size() * sizeof( uint16_t ) );
            std::memcpy( bytes.data(), samples.data(), bytes.size() );
            return bytes;
        }

        std::shared_ptr<Graphic::Image2D> CreateHeightmap( const Landscape::LandscapeTileData& tile, int32_t tileX,
                                                           int32_t tileZ, std::vector<unsigned char> texels )
        {
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
            spec.Data = std::move( texels );

            // A derived copy: the recipe is the component's samples, which the scene owns, so the owner is
            // the renderer side that re-derives it - releasing it loses nothing.
            const Graphic::ResourceAttributionScope owned( Graphic::ResourceOwner::SceneRenderer );
            return Graphic::Image2D::Create( spec );
        }

        // The tile's weight layers as one RGBA8 texel per sample, its pages stacked along the height
        // (LandscapeWeightmapAtlasTexels): no neighbour ring — the surface samples it at the vertex's own
        // sample, which every tile holds for its edges.
        std::shared_ptr<Graphic::Image2D> CreateWeightmap( const Landscape::LandscapeTileData& tile, int32_t tileX,
                                                           int32_t tileZ, std::vector<unsigned char> texels )
        {
            Core::Formats::Image2DSpecification spec = {
                 .Tag     = "LandscapeWeights(" + std::to_string( tileX ) + "," + std::to_string( tileZ ) + ")",
                 .Width   = tile.SamplesX(),
                 .Height  = tile.SamplesZ() * Landscape::LandscapeWeightmapPageCount( tile.WeightLayers().size() ),
                 .Format  = Core::Formats::ImageFormat::RGBA8F,
                 .Mips    = 1,
                 .Samples = 1,
                 .Data    = Core::Formats::EmptyPixelData{},
                 .Usage   = Core::Formats::Image2DUsage::Image2D,
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
                                     const Common::Timestep& ts )
    {
        // Pass 1: every drawable tile, by (root, tile x, tile z) — a tile's heightmap carries a ring row from
        // each loaded neighbour, so it cannot be built before its neighbours are known.
        struct Drawable
        {
            entt::entity             Entity = entt::null;
            Landscape::LandscapeRoot Root;
            bool                     Dirty        = false;
            bool                     WeightsDirty = false;
            bool                     GrassDirty   = false;
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
            d.GrassDirty   = !heights.TakeDirtyRects( Landscape::LandscapeDirtyConsumer::Grass ).empty();
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
        // like every other: its values (Tint) arrive as named overrides that TerrainRenderer applies on top
        // of the shader's schema defaults. A root without a LandscapeMaterial draws with those defaults.
        struct Surface
        {
            Graphic::MaterialOverrides Overrides;
            // The root's weight layers (their `.delayerinfo` data), which give each tile channel its colour
            // and blend by name. Pending: a layer is still loading, so a channel that names nothing yet is
            // not reported as unknown.
            std::vector<Assets::Serialization::LandscapeLayerInfoData> Layers;
            bool                                                       LayersPending = false;
            // The root's FIRST layer's colour with a = 1 (UE shows its first layer where nothing is
            // painted); a = 0 while the root names none or that layer is not loaded: white ground.
            glm::vec4 Ground = glm::vec4( 0.0f );
        };
        std::map<uint64_t, Surface> surfaces;
        const auto                  surfaceOf = [&]( const Common::UUID& rootId ) -> const Surface&
        {
            const auto [it, inserted] = surfaces.try_emplace( static_cast<uint64_t>( rootId ) );
            if ( !inserted )
                return it->second;
            const entt::entity rootEntity = FindLandscapeRootEntity( registry, rootId );
            if ( rootEntity != entt::null && registry.has<LandscapeComponent>( rootEntity ) )
            {
                // The service logs a failed layer once; a failed layer is simply absent here, and the tile's
                // channels of it are reported below as layers the landscape does not list.
                auto& service = *Runtime::ResourceRegistry::GetLandscapeLayerInfoService();
                const auto& handles = registry.get<LandscapeComponent>( rootEntity ).Layers;
                for ( const Assets::AssetHandle& handle : handles )
                {
                    if ( const auto* info = service.Get( handle ) )
                    {
                        if ( handle == handles.front() )
                            it->second.Ground = glm::vec4( info->LayerUsageDebugColor, 1.0f );
                        it->second.Layers.push_back( *info );
                    }
                    else if ( service.StateOf( handle ) == Runtime::LandscapeLayerInfoService::State::Pending )
                        it->second.LayersPending = true;
                }
            }
            for ( const auto entity : registry.view<LandscapeMaterialComponent, UUIDComponent>() )
            {
                if ( registry.get<UUIDComponent>( entity ).UUID != rootId )
                    continue;
                const LandscapeMaterialData& look = registry.get<LandscapeMaterialComponent>( entity ).Data;
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
            // In place while the sample count holds (LandscapeTileImage.hpp): the image's address keys the tile's
            // terrain material, so sculpting must not replace it.
            const auto heightmap = RefreshLandscapeTileImage(
                 gpu.Heightmap, gpu.SamplesX, gpu.SamplesZ, heights.SamplesX(), heights.SamplesZ(),
                 d.Dirty || neighbourDirty || gpu.NeighbourMask != mask,
                 [&] { return HeightmapTexels( heights, neighbours ); }, [&]( std::vector<unsigned char> texels )
                 { return CreateHeightmap( heights, tileComp.TileX, tileComp.TileZ, std::move( texels ) ); } );
            if ( !heightmap.IsSuccess() )
            {
                LOG_ERROR(
                     "[Landscape] tile ({}, {}): R16 heightmap (samples {} x {} plus the neighbour ring): {}; "
                     "the tile is not drawn",
                     tileComp.TileX, tileComp.TileZ, heights.SamplesX(), heights.SamplesZ(),
                     heightmap.GetError() );
                m_Tiles.erase( d.Entity );
                continue;
            }
            gpu.NeighbourMask = mask;

            if ( d.WeightsDirty )
            {
                m_WarnedWeights.erase( d.Entity );
            }
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
            weights.Ground = surface.Ground;
            if ( gpu.Weightmap )
            {
                const Landscape::LandscapeWeightChannels channels =
                     Landscape::ResolveLandscapeWeightChannels( heights, surface.Layers );
                weights.Weightmap  = gpu.Weightmap.get();
                weights.LayerCount = channels.Count;
                weights.Colors     = channels.Colors;
                weights.AlphaBlend = Landscape::LandscapeAlphaBlendPages( channels );
                weights.RootLayer       = Landscape::LandscapeRootLayerPages( channels );
                weights.VisibilityLayer = channels.Visibility;
                if ( !channels.Unknown.empty() && !surface.LayersPending &&
                     m_WarnedWeights.insert( d.Entity ).second )
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
            renderCommandBuffer.Emplace<Graphic::Render::DrawLandscapeTileCommand>( gpu.Heightmap.get(), draw,
                                                                                    surface.Overrides, weights );
        }

        // Pass 3 (GRASS-GPU): every root whose layers grow grass hands the grass renderer its field and varieties.
        m_WindSeconds += static_cast<double>( ts.GetSeconds() );
        std::map<uint64_t, std::vector<GrassTile>>          grassRoots;
        std::map<uint64_t, Landscape::LandscapeRoot> grassFrames;
        for ( const Drawable& d : drawables )
        {
            const auto& tileComp = registry.get<LandscapeTileComponent>( d.Entity );
            const auto  rootId   = static_cast<uint64_t>( Common::UUID( tileComp.Landscape ) );
            grassRoots[rootId].push_back( { &tileComp, d.GrassDirty } );
            grassFrames[rootId] = d.Root;
        }
        for ( const auto& [rootId, tiles] : grassRoots )
            EmitGrass( registry, renderCommandBuffer, rootId, grassFrames[rootId],
                       surfaceOf( Common::UUID( rootId ) ).Layers, tiles );

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
        // In place per stroke, like the heightmap: the weightmap's address keys the tile's material too.
        const auto weightmap = RefreshLandscapeTileImage(
             gpu.Weightmap, gpu.WeightmapX, gpu.WeightmapZ, heights.SamplesX(),
             heights.SamplesZ() * Landscape::LandscapeWeightmapPageCount( heights.WeightLayers().size() ),
             weightsDirty, [&] { return Landscape::LandscapeWeightmapAtlasTexels( heights ); },
             [&]( std::vector<unsigned char> texels )
             { return CreateWeightmap( heights, tileComp.TileX, tileComp.TileZ, std::move( texels ) ); } );
        if ( weightmap.IsSuccess() )
            return;
        gpu.Weightmap.reset();
        if ( m_WarnedWeights.insert( entity ).second )
            LOG_ERROR( "[Landscape] tile ({}, {}): RGBA8 weightmap: {}; the tile's painted layers are not drawn",
                       tileComp.TileX, tileComp.TileZ, weightmap.GetError() );
    }
    void LandscapeECSSystem::EmitGrass( entt::registry&                                                   registry,
                                        Graphic::Render::RenderCommandBuffer&                             renderCommandBuffer,
                                        const uint64_t rootId, const World::Landscape::LandscapeRoot&     root,
                                        const std::vector<Assets::Serialization::LandscapeLayerInfoData>& layers,
                                        const std::vector<GrassTile>&                                     tiles )
    {
        std::vector<const Assets::Serialization::LandscapeLayerInfoData*> grassLayers;
        for ( const auto& layer : layers )
            if ( !layer.Grass.empty() )
                grassLayers.push_back( &layer );
        if ( grassLayers.empty() || tiles.empty() )
            return;

        // The field: rebuilt when a tile's heights or weights changed, or the tile set or the grass layers did.
        const uint32_t quads     = root.QuadsPerTile;
        int32_t        minX      = tiles.front().Tile->TileX;
        int32_t        maxX      = minX;
        int32_t        minZ      = tiles.front().Tile->TileZ;
        int32_t        maxZ      = minZ;
        uint64_t       signature = 1469598103934665603ull;
        bool           dirty     = false;
        const auto     mix       = [&]( uint64_t v ) { signature = ( signature ^ v ) * 1099511628211ull; };
        for ( const GrassTile& t : tiles )
        {
            minX  = std::min( minX, t.Tile->TileX );
            maxX  = std::max( maxX, t.Tile->TileX );
            minZ  = std::min( minZ, t.Tile->TileZ );
            maxZ  = std::max( maxZ, t.Tile->TileZ );
            dirty = dirty || t.Dirty;
            mix( ( static_cast<uint64_t>( static_cast<uint32_t>( t.Tile->TileX ) ) << 32u ) |
                 static_cast<uint32_t>( t.Tile->TileZ ) );
        }
        for ( const auto* layer : grassLayers )
            mix( std::hash<std::string>{}( layer->LayerName ) );
        mix( std::bit_cast<uint32_t>( root.SpacingCm ) );
        mix( std::bit_cast<uint32_t>( root.ZScale ) );

        GrassRootCache& cache = m_Grass[rootId];
        if ( dirty || !cache.Field || cache.Signature != signature )
        {
            const uint32_t samplesX = ( static_cast<uint32_t>( maxX - minX ) + 1u ) * quads + 1u;
            const uint32_t samplesZ = ( static_cast<uint32_t>( maxZ - minZ ) + 1u ) * quads + 1u;
            const size_t   count    = static_cast<size_t>( samplesX ) * samplesZ;
            const size_t   perLayer = ( count + 3u ) / 4u;
            auto           field    = std::make_shared<Graphic::System::GrassFieldData>();
            auto&          w        = field->Words;
            w.assign( 8u + count + ( perLayer * grassLayers.size() ), 0u );
            w[0]                = samplesX;
            w[1]                = samplesZ;
            w[2]                = static_cast<uint32_t>( grassLayers.size() );
            const float firstX  = static_cast<float>( minX * static_cast<int32_t>( quads ) ) * root.SpacingCm;
            const float firstZ  = static_cast<float>( minZ * static_cast<int32_t>( quads ) ) * root.SpacingCm;
            w[4]                = std::bit_cast<uint32_t>( root.Origin.x + firstX );
            w[5]                = std::bit_cast<uint32_t>( root.Origin.z + firstZ );
            w[6]                = std::bit_cast<uint32_t>( root.SpacingCm );
            std::fill( w.begin() + 8, w.begin() + 8 + static_cast<std::ptrdiff_t>( count ),
                       std::bit_cast<uint32_t>( Graphic::System::kGrassNoHeight ) );
            for ( const GrassTile& t : tiles )
            {
                const Landscape::LandscapeTileData& data = *t.Tile->Heights;
                if ( data.SamplesX() != quads + 1u || data.SamplesZ() != quads + 1u )
                    continue; // the loader refuses such a tile before it is drawable; nothing to grow from
                const uint32_t                     baseX = static_cast<uint32_t>( t.Tile->TileX - minX ) * quads;
                const uint32_t                     baseZ = static_cast<uint32_t>( t.Tile->TileZ - minZ ) * quads;
                std::vector<std::optional<size_t>> planes;
                for ( const auto* layer : grassLayers )
                    planes.push_back( data.FindWeightLayer( layer->LayerName ) );
                for ( uint32_t z = 0; z <= quads; ++z )
                    for ( uint32_t x = 0; x <= quads; ++x )
                    {
                        const size_t index = ( static_cast<size_t>( baseZ + z ) * samplesX ) + baseX + x;
                        w[8u + index]      = std::bit_cast<uint32_t>(
                             root.Origin.y + Landscape::LandscapeHeightCm( data.Sample( x, z ), root.ZScale ) );
                        for ( size_t k = 0; k < planes.size(); ++k )
                        {
                            if ( !planes[k] )
                                continue;
                            const uint32_t weight = data.Weight( *planes[k], x, z );
                            uint32_t&      word   = w[8u + count + ( k * perLayer ) + ( index / 4u )];
                            const uint32_t shift  = 8u * static_cast<uint32_t>( index & 3u );
                            word                  = ( word & ~( 0xffu << shift ) ) | ( weight << shift );
                        }
                    }
            }
            field->Revision = ++m_GrassRevision;
            cache.Field     = std::move( field );
            cache.Signature = signature;
        }

        // The varieties: mesh, its sections' own materials (UE: the mesh's materials), placement and wind.
        auto*                           meshes    = Runtime::ResourceRegistry::GetMeshService();
        auto*                           materials = Runtime::ResourceRegistry::GetMaterialService();
        const glm::vec3                 air       = ECS::WindAt( registry, root.Origin ).Velocity;
        Graphic::System::GrassFieldDraw draw;
        draw.Root  = rootId;
        draw.Field = cache.Field;
        for ( uint32_t k = 0; k < grassLayers.size(); ++k )
            for ( uint32_t vi = 0; vi < grassLayers[k]->Grass.size(); ++vi )
            {
                const auto& v    = grassLayers[k]->Grass[vi];
                const auto  guid = Common::Content::AssetGuidFromText( v.Mesh.Guid );
                if ( !guid )
                    continue; // the layer info's validation refused it on load
                const Assets::AssetHandle handle = Common::Content::HandleForGuid( guid.GetValue() );
                Desert::Mesh*             mesh   = meshes->Get( handle );
                const Assets::MeshAsset*  asset  = meshes->GetAsset( handle );
                if ( mesh == nullptr || asset == nullptr )
                    continue; // still loading, or MeshService said why it failed
                const uint64_t key      = ( rootId * 1000003ull ) + ( k * 4099ull ) + vi;
                GrassSections& sections = m_GrassSections[key];
                if ( sections.Mesh != mesh || sections.Version != materials->GetInvalidationVersion() )
                {
                    sections.Mesh    = mesh;
                    sections.Version = materials->GetInvalidationVersion();
                    sections.Sections.clear();
                    const auto&    slots     = asset->GetMaterialHandles();
                    const size_t   submeshes = std::min<size_t>( mesh->GetSubmeshes().size(), 64 );
                    const uint64_t all       = submeshes >= 64 ? ~0ull : ( 1ull << submeshes ) - 1ull;
                    for ( size_t slot = 0; slot < slots.size(); ++slot )
                    {
                        uint64_t hidden = 0;
                        for ( size_t si = 0; si < submeshes; ++si )
                            if ( SectionMaterialSlot( si, slots.size() ) != slot )
                                hidden |= 1ull << si;
                        if ( hidden == all )
                            continue;
                        auto instance =
                             materials->CreateRuntimeInstance( materials->GetAssetHandleByExternal( slots[slot] ) );
                        if ( !instance )
                        {
                            if ( m_WarnedGrass.insert( v.Mesh.Path ).second )
                                LOG_WARN( "[Landscape] grass mesh '{}': section material {} does not resolve; those "
                                          "sections do not grow",
                                          v.Mesh.Path, slot );
                            continue;
                        }
                        sections.Sections.push_back( { std::move( instance ), hidden } );
                    }
                }
                Graphic::System::GrassVarietyDraw variety;
                variety.Key                 = key;
                variety.Layer               = k;
                variety.Mesh                = mesh;
                variety.Sections            = sections.Sections;
                auto& p                     = variety.Placement;
                p.Density                   = v.Density;
                p.PlacementJitter           = v.PlacementJitter;
                p.StartCullDistance         = v.StartCullDistance;
                p.EndCullDistance           = v.EndCullDistance;
                p.AllowedDensityRange       = { v.AllowedDensityRange.Min, v.AllowedDensityRange.Max };
                p.Scaling                   = static_cast<uint32_t>( v.Scaling );
                p.ScaleX                    = { v.ScaleX.Min, v.ScaleX.Max };
                p.ScaleY                    = { v.ScaleY.Min, v.ScaleY.Max };
                p.ScaleZ                    = { v.ScaleZ.Min, v.ScaleZ.Max };
                p.WeightAttenuatesMaxScale  = v.WeightAttenuatesMaxScale;
                p.MaxScaleWeightAttenuation = v.MaxScaleWeightAttenuation;
                p.RandomRotation            = v.RandomRotation;
                p.AlignToSurface            = v.AlignToSurface;
                p.CastDynamicShadow         = v.CastDynamicShadow;
                variety.Wind = Graphic::MakeInstanceWind( v.Wind.Strength, v.Wind.Speed, v.Wind.Height,
                                                          glm::vec2( air.x, air.z ), m_WindSeconds );
                draw.Varieties.push_back( std::move( variety ) );
            }
        if ( !draw.Varieties.empty() )
            renderCommandBuffer.Emplace<Graphic::Render::DrawLandscapeGrassCommand>( std::move( draw ) );
    }
} // namespace Desert::ECS
