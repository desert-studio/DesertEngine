#include <Engine/ECS/System/LandscapeGrassECSSystem.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/Render/Commands/DrawMeshCommand.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <cmath>
#include <set>

namespace Desert::ECS
{
    namespace Landscape = World::Landscape;

    namespace
    {
        using TileKey = std::tuple<uint64_t, int32_t, int32_t>; // (root UUID, tile x, tile z)

        Assets::AssetHandle HandleOfGuidText( const std::string& text )
        {
            const auto guid = Common::Content::AssetGuidFromText( text );
            if ( !guid )
                return Assets::AssetHandle::Null();
            return Assets::AssetHandle(
                 static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) );
        }
    } // namespace

    LandscapeGrassECSSystem::LandscapeGrassECSSystem()
         : m_Material( Graphic::MaterialPBR::Create( Graphic::MeshVertexPath::Static ) ),
           m_MaterialInstance( m_Material->CreateInstance() )
    {
    }

    LandscapeGrassECSSystem::~LandscapeGrassECSSystem() = default;

    void LandscapeGrassECSSystem::SetCameraSnapshot( const glm::mat4& /*view*/, const glm::vec3& position )
    {
        m_Camera = position;
    }

    void LandscapeGrassECSSystem::Update( entt::registry&                       registry,
                                          Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                                          const Common::Timestep& /*ts*/ )
    {
        // Every drawable tile by (root, x, z): the surface the cells are sampled from. A refused tile is the
        // landscape system's to report; it grows no grass because it is not here.
        const DrawableLandscape    landscape = DrawableLandscapeTiles( registry );
        std::map<TileKey, size_t>  tileAt;
        std::map<uint64_t, size_t> anyTileOfRoot;
        for ( size_t i = 0; i < landscape.Tiles.size(); ++i )
        {
            const LandscapeTileComponent& c    = *landscape.Tiles[i].Component;
            const uint64_t                root = static_cast<uint64_t>( Common::UUID( c.Landscape ) );
            tileAt.emplace( TileKey{ root, c.TileX, c.TileZ }, i );
            anyTileOfRoot.emplace( root, i );
        }

        // A stroke drops the cells under it in every streamer of that root; they regrow below from the new
        // surface. Taken always, like the other consumers, so the list never grows without bound.
        for ( const LandscapeTileRef& tile : landscape.Tiles )
        {
            const uint64_t root = static_cast<uint64_t>( Common::UUID( tile.Component->Landscape ) );
            for ( const Landscape::LandscapeRect& r :
                  tile.Component->Heights->TakeDirtyRects( Landscape::LandscapeDirtyConsumer::Grass ) )
            {
                const glm::vec2 lo( tile.Frame.OriginX + static_cast<float>( r.X0 ) * tile.Frame.SpacingCm,
                                    tile.Frame.OriginZ + static_cast<float>( r.Z0 ) * tile.Frame.SpacingCm );
                const glm::vec2 hi( tile.Frame.OriginX + static_cast<float>( r.X1 ) * tile.Frame.SpacingCm,
                                    tile.Frame.OriginZ + static_cast<float>( r.Z1 ) * tile.Frame.SpacingCm );
                for ( auto& [key, streamer] : m_Streamers )
                    if ( std::get<0>( key ) == root )
                        streamer.Invalidate( lo, hi );
            }
        }

        const glm::vec2       camera( m_Camera.x, m_Camera.z );
        uint32_t              budget = kGrassCellsPerFrame;
        std::set<StreamerKey> seen;

        auto roots = registry.view<LandscapeComponent, UUIDComponent>();
        for ( const entt::entity rootEntity : roots )
        {
            if ( IsHidden( registry, rootEntity ) )
                continue;
            const uint64_t rootId = static_cast<uint64_t>( registry.get<UUIDComponent>( rootEntity ).UUID );
            const auto     any    = anyTileOfRoot.find( rootId );
            if ( any == anyTileOfRoot.end() )
                continue;
            const Landscape::LandscapeRoot& root   = landscape.Tiles[any->second].Root;
            const float                     extent = Landscape::LandscapeTileExtentCm( root );

            const auto tileUnder = [&]( int32_t tx, int32_t tz ) -> const LandscapeTileRef*
            {
                const auto found = tileAt.find( TileKey{ rootId, tx, tz } );
                return found == tileAt.end() ? nullptr : &landscape.Tiles[found->second];
            };

            for ( const Assets::AssetHandle& layerHandle : registry.get<LandscapeComponent>( rootEntity ).Layers )
            {
                // Pending or failed layer infos and grass types grow nothing; the services say why, once.
                const auto* info = Runtime::ResourceRegistry::GetLandscapeLayerInfoService()->Get( layerHandle );
                if ( !info || info->GrassType.Guid.empty() )
                    continue;
                const auto* grass = Runtime::ResourceRegistry::GetLandscapeGrassTypeService()->Get(
                     HandleOfGuidText( info->GrassType.Guid ) );
                if ( !grass )
                    continue;

                const std::string& layerName = info->LayerName;
                const auto surface = [&]( float x, float z ) -> std::optional<Landscape::GrassSurfaceSample>
                {
                    const int32_t tx = static_cast<int32_t>( std::floor( ( x - root.Origin.x ) / extent ) );
                    const int32_t tz = static_cast<int32_t>( std::floor( ( z - root.Origin.z ) / extent ) );
                    const LandscapeTileRef* tile = tileUnder( tx, tz );
                    if ( !tile )
                        return std::nullopt;
                    const Landscape::LandscapeTileData& data  = *tile->Component->Heights;
                    const auto                          layer = data.FindWeightLayer( layerName );
                    if ( !layer )
                        return std::nullopt; // this tile was never painted with the layer: nothing grows
                    const auto height = Landscape::SampleLandscapeHeight( data, tile->Frame, x, z );
                    const auto weight = Landscape::SampleLandscapeWeight( data, tile->Frame, *layer, x, z );
                    if ( !height || !weight )
                        return std::nullopt;
                    const auto neighbour = [&]( int32_t dx, int32_t dz ) -> const Landscape::LandscapeTileData*
                    {
                        const LandscapeTileRef* n = tileUnder( tx + dx, tz + dz );
                        return n ? &*n->Component->Heights : nullptr;
                    };
                    const Landscape::LandscapeTileNeighbours around{ neighbour( -1, 0 ), neighbour( 1, 0 ),
                                                                     neighbour( 0, -1 ), neighbour( 0, 1 ) };
                    const auto normal = Landscape::SampleLandscapeNormal( data, tile->Frame, around, x, z );
                    return Landscape::GrassSurfaceSample{
                         *height, normal.value_or( glm::vec3( 0.0f, 1.0f, 0.0f ) ), *weight };
                };

                for ( uint32_t v = 0; v < grass->GrassVarieties.size(); ++v )
                {
                    const auto& variety = grass->GrassVarieties[v];
                    Mesh*       mesh    = Runtime::ResourceRegistry::GetMeshService()->Get(
                         HandleOfGuidText( variety.GrassMesh.Guid ) );
                    if ( !mesh )
                        continue; // pending: requested by the ask, drawn from the frame it lands

                    const StreamerKey key{ rootId, static_cast<uint64_t>( layerHandle ), v };
                    seen.insert( key );
                    auto& streamer = m_Streamers[key];
                    // The salt makes two layers (or two varieties) of one cell grow different grass.
                    const uint32_t salt = static_cast<uint32_t>( static_cast<uint64_t>( layerHandle ) ) ^
                                          static_cast<uint32_t>( static_cast<uint64_t>( layerHandle ) >> 32 ) ^
                                          ( v * 0x9E3779B9u );
                    const auto generated = streamer.Tick(
                         camera, variety.EndCullDistance, budget, [&]( Landscape::GrassCellCoord cell )
                         { return Landscape::GenerateGrassCell( variety, cell, salt, surface ); } );
                    budget -= generated.Generated;

                    // The cull distance takes the foliage path (FO-5): the renderer fades the instances out
                    // between Start and End per instance, in the ISM loop that already culls by frustum. Grass
                    // has no wind field (UE drives it from the material's WPO, which this engine lacks): still.
                    auto instances = streamer.Instances();
                    if ( !instances->empty() )
                        renderCommandBuffer.Emplace<Graphic::Render::DrawInstancedStaticMeshCommand>(
                             mesh, m_MaterialInstance, std::move( instances ), variety.CastDynamicShadow,
                             Landscape::GrassCullDistance( variety ), Graphic::InstanceWind{} );
                }
            }
        }

        // A root, layer or variety that is gone (or hidden) drops its cells: nothing is kept for it.
        for ( auto it = m_Streamers.begin(); it != m_Streamers.end(); )
            it = seen.contains( it->first ) ? std::next( it ) : m_Streamers.erase( it );
    }
} // namespace Desert::ECS
