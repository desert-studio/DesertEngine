#include <Engine/ECS/LandscapeRootOf.hpp>

#include <Engine/World/Landscape/LandscapeData.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Desert::ECS
{
    void SetLandscapeTileHeights( entt::registry& registry, entt::entity entity,
                                         std::optional<World::Landscape::LandscapeTileData> heights )
    {
        registry.patch<LandscapeTileComponent>( entity, [&heights]( LandscapeTileComponent& tile )
                                                { tile.Heights = std::move( heights ); } );
    }

    LandscapeTileTable& RefreshLandscapeTileTable( entt::registry& registry )
    {
        auto* found = registry.try_ctx<LandscapeTileTable>();
        if ( found == nullptr )
        {
            found = &registry.set<LandscapeTileTable>();
            registry.on_construct<LandscapeTileComponent>().connect<&RaiseLandscapeTileRevision>();
            registry.on_update<LandscapeTileComponent>().connect<&RaiseLandscapeTileRevision>();
            registry.on_destroy<LandscapeTileComponent>().connect<&RaiseLandscapeTileRevision>();
        }
        LandscapeTileTable& table = *found;
        if ( table.BuiltRevision == table.Revision && LandscapeRootsUnchanged( registry, table ) )
            return table;

        table.Roots.clear();
        for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
            table.Roots.push_back( { entity, registry.get<UUIDComponent>( entity ).UUID,
                                     LandscapeRootOf( Entity( entity, registry ) ) } );

        DrawableLandscape& out = table.Drawable;
        out.Tiles.clear();
        out.Refused.clear();
        out.RayEntities.clear();
        out.RayTiles.clear();
        table.ByCoord.clear();
        table.ByEntity.clear();
        for ( const auto entity : registry.view<LandscapeTileComponent>() )
        {
            auto& component = registry.get<LandscapeTileComponent>( entity );
            if ( !component.Heights.has_value() )
                continue;
            const LandscapeTileTable::BuiltRoot* root = nullptr;
            if ( component.Landscape != Common::UUID::Null() )
                for ( const auto& candidate : table.Roots )
                    if ( candidate.Id == component.Landscape )
                    {
                        root = &candidate;
                        break;
                    }
            if ( root == nullptr )
            {
                out.Refused.push_back( { entity, "its root is not loaded or is not a landscape" } );
                continue;
            }
            const auto fits = World::Landscape::CheckTileMatchesRoot( *component.Heights, root->Root );
            if ( !fits.IsSuccess() )
            {
                out.Refused.push_back( { entity, fits.GetError() } );
                continue;
            }
            const World::Landscape::LandscapeFrame frame =
                 World::Landscape::LandscapeTileFrame( root->Root, component.TileX, component.TileZ );
            table.ByCoord.push_back( { static_cast<uint64_t>( Common::UUID( component.Landscape ) ),
                                       component.TileX, component.TileZ,
                                       static_cast<uint32_t>( out.Tiles.size() ) } );
            LandscapeTileRef ref;
            ref.Entity    = entity;
            ref.Component = &component;
            ref.Root      = root->Root;
            ref.Frame     = frame;
            out.Tiles.push_back( ref );
            // Every scene entity has a UUID; a bare test registry's tile has none and is hit as Null.
            Common::UUID rayId = Common::UUID::Null();
            if ( const auto* id = registry.try_get<UUIDComponent>( entity ) )
                rayId = id->UUID;
            if ( rayId != Common::UUID::Null() )
                table.ByEntity.push_back(
                     { static_cast<uint64_t>( rayId ), static_cast<uint32_t>( out.Tiles.size() - 1u ) } );
            out.RayEntities.push_back( rayId );
            out.RayTiles.push_back( { &*component.Heights, frame } );
        }
        LinkLandscapeNeighbours( out.Tiles, table.ByCoord );
        std::stable_sort( table.ByEntity.begin(), table.ByEntity.end(),
                          []( const LandscapeTileTable::TileOf& a, const LandscapeTileTable::TileOf& b )
                          { return a.Id < b.Id; } );
        table.BuiltRevision = table.Revision;
        return table;
    }

    uint32_t FindLandscapeTileAt( const LandscapeTileTable& table, const float x, const float z )
    {
        const auto holds = [&]( const uint32_t index )
        {
            const World::Landscape::LandscapeRayTile& tile = table.Drawable.RayTiles[index];
            const float spanX = tile.Frame.SpacingCm * static_cast<float>( tile.Heights->SamplesX() - 1u );
            const float spanZ = tile.Frame.SpacingCm * static_cast<float>( tile.Heights->SamplesZ() - 1u );
            return x >= tile.Frame.OriginX && x <= tile.Frame.OriginX + spanX && z >= tile.Frame.OriginZ &&
                   z <= tile.Frame.OriginZ + spanZ;
        };
        for ( const LandscapeTileTable::BuiltRoot& root : table.Roots )
        {
            const float extentCm = static_cast<float>( root.Root.QuadsPerTile ) * root.Root.SpacingCm;
            if ( !( extentCm > 0.0f ) )
                continue;
            const auto tx = static_cast<int32_t>( std::floor( ( x - root.Root.Origin.x ) / extentCm ) );
            const auto tz = static_cast<int32_t>( std::floor( ( z - root.Root.Origin.z ) / extentCm ) );
            const auto id = static_cast<uint64_t>( root.Id );
            for ( const int32_t dz : { 0, -1 } )
                for ( const int32_t dx : { 0, -1 } )
                {
                    const uint32_t index = FindLandscapeTileCoord( table.ByCoord, id, tx + dx, tz + dz );
                    if ( index != NoLandscapeNeighbour && holds( index ) )
                        return index;
                }
        }
        return NoLandscapeNeighbour;
    }
} // namespace Desert::ECS
