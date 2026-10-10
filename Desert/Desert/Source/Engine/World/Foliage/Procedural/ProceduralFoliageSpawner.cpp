#include <Engine/World/Foliage/Procedural/ProceduralFoliageSpawner.hpp>

#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::World::Foliage::Procedural
{
    ProceduralFoliageSpawner::ProceduralFoliageSpawner( ProceduralFoliageSpawnerSettings settings,
                                                        std::vector<Assets::Serialization::FoliageTypeData> types )
         : m_Settings( settings ), m_Types( std::move( types ) )
    {
    }

    Common::BoolResultStr ProceduralFoliageSpawner::Validate() const
    {
        if ( !std::isfinite( m_Settings.TileSize ) || m_Settings.TileSize <= 0.0f )
            return Common::MakeFormattedError<bool>( "TileSize {} must be a positive distance, cm",
                                                     m_Settings.TileSize );
        if ( !std::isfinite( m_Settings.MinimumQuadTreeSize ) || m_Settings.MinimumQuadTreeSize <= 0.0f )
            return Common::MakeFormattedError<bool>( "MinimumQuadTreeSize {} must be a positive distance, cm",
                                                     m_Settings.MinimumQuadTreeSize );
        if ( m_Settings.NumUniqueTiles < 1 )
            return Common::MakeFormattedError<bool>( "NumUniqueTiles {} must be at least 1",
                                                     m_Settings.NumUniqueTiles );
        for ( size_t i = 0; i < m_Types.size(); ++i )
            if ( auto ok = Assets::Serialization::ValidateFoliageProcedural( m_Types[i].Procedural ); !ok )
                return Common::MakeFormattedError<bool>( "type {}: {}", i, ok.GetError() );
        return BOOLSUCCESS;
    }

    void ProceduralFoliageSpawner::Simulate( int32_t numSteps )
    {
        // The seeds are drawn in order from the one stream, so each tile's seed is the same however the tiles are
        // then scheduled; a tile reads only this spawner and writes only itself, so the tiles grow in parallel.
        Common::Math::RandomStream stream( m_Settings.RandomSeed );
        std::vector<int32_t> seeds;
        m_Tiles.clear();
        for ( int32_t i = 0; i < m_Settings.NumUniqueTiles; ++i )
        {
            seeds.push_back( static_cast<int32_t>( stream.FRand() * static_cast<float>( kProceduralRandMax ) ) );
            m_Tiles.push_back( std::make_unique<ProceduralFoliageTile>() );
        }
        Common::JobSystem::Get().ParallelFor( m_Tiles.size(), [&]( size_t i )
                                              { m_Tiles[i]->Simulate( *this, seeds[i], numSteps ); } );
    }

    const ProceduralFoliageTile* ProceduralFoliageSpawner::GetRandomTile( int32_t x, int32_t y ) const
    {
        if ( m_Tiles.empty() )
            return nullptr;
        // A random stream as a hash of the coordinate.
        Common::Math::RandomStream hash( x );
        const float  xRand = hash.FRand();
        hash.Initialize( y );
        const float yRand = hash.FRand();
        const auto  number =
             static_cast<int32_t>( static_cast<float>( kProceduralRandMax ) * xRand / ( yRand + 0.01f ) );
        const auto count = static_cast<int32_t>( m_Tiles.size() );
        return m_Tiles[static_cast<size_t>( std::clamp( number % count, 0, count - 1 ) )].get();
    }

    ProceduralFoliageTileLayout TileLayoutFor( glm::vec2 min, glm::vec2 max, float tileSize, float tileOverlap )
    {
        const glm::vec2             lo = min + tileOverlap;
        const glm::vec2             hi = max - tileOverlap;
        ProceduralFoliageTileLayout layout;
        layout.BottomLeftX = static_cast<int32_t>( std::floor( lo.x / tileSize ) );
        layout.BottomLeftY = static_cast<int32_t>( std::floor( lo.y / tileSize ) );
        layout.NumTilesX   = static_cast<int32_t>( std::floor( hi.x / tileSize ) ) - layout.BottomLeftX + 1;
        layout.NumTilesY   = static_cast<int32_t>( std::floor( hi.y / tileSize ) ) - layout.BottomLeftY + 1;
        return layout;
    }

    std::vector<ProceduralFoliagePlacement> GenerateProceduralContent( const ProceduralFoliageSpawner&    spawner,
                                                                       const ProceduralFoliageTileLayout& layout,
                                                                       glm::vec2 origin, float tileOverlap )
    {
        std::vector<ProceduralFoliagePlacement> placements;
        const float                             size = spawner.Settings().TileSize;
        const float                             ov   = tileOverlap;
        for ( int32_t x = 0; x < layout.NumTilesX; ++x )
            for ( int32_t y = 0; y < layout.NumTilesY; ++y )
            {
                const int32_t tx   = x + layout.BottomLeftX;
                const int32_t ty   = y + layout.BottomLeftY;
                const auto*   tile = spawner.GetRandomTile( tx, ty );
                if ( !tile )
                    return {};
                const bool hasRight = x + 1 < layout.NumTilesX;
                const bool hasTop   = y + 1 < layout.NumTilesY;
                const bool hasBelow = y > 0;
                const bool hasLeft  = x > 0;

                // The tile's own region; the first row and column take the strip no neighbour before them holds
                // (UE GetTileRegion).
                const Box2 base{ { hasLeft ? ov : -ov, hasBelow ? ov : -ov }, { size + ov, size + ov } };

                ProceduralFoliageTile composite;
                composite.InitSimulation( spawner, 0 );
                tile->CopyInstancesToTile( composite, base, base, glm::vec2( 0.0f ) );
                if ( hasRight )
                {
                    // Its [0, Overlap) strip is placed here; past it, and below this row, its plants are placed
                    // by the tile to the right and the one below that.
                    const Box2 owned{ { 0.0f, base.Min.y }, { ov, base.Max.y } };
                    const Box2 blocking{ { 0.0f, hasBelow ? base.Min.y - ov : base.Min.y },
                                         { 2.0f * ov, base.Max.y } };
                    spawner.GetRandomTile( tx + 1, ty )
                         ->CopyInstancesToTile( composite, owned, blocking, { size, 0.0f } );
                }
                if ( hasTop )
                {
                    const Box2 owned{ { base.Min.x, 0.0f }, { base.Max.x, ov } };
                    const Box2 blocking{ { hasLeft ? base.Min.x - ov : base.Min.x, 0.0f },
                                         { base.Max.x, 2.0f * ov } };
                    spawner.GetRandomTile( tx, ty + 1 )
                         ->CopyInstancesToTile( composite, owned, blocking, { 0.0f, size } );
                }
                if ( hasRight && hasTop )
                {
                    const Box2 owned{ { 0.0f, 0.0f }, { ov, ov } };
                    const Box2 blocking{ { 0.0f, 0.0f }, { 2.0f * ov, 2.0f * ov } };
                    spawner.GetRandomTile( tx + 1, ty + 1 )
                         ->CopyInstancesToTile( composite, owned, blocking, { size, size } );
                }
                if ( hasRight && hasBelow )
                {
                    // The tile below-right places the plants at this tile's lower-right corner; no composite holds
                    // both it and this tile otherwise, so a pair across that corner would never compete.
                    const Box2 none{ { 0.0f, 0.0f }, { 0.0f, 0.0f } };
                    const Box2 blocking{ { ov, size }, { 2.0f * ov, size + ov } };
                    spawner.GetRandomTile( tx + 1, ty - 1 )
                         ->CopyInstancesToTile( composite, none, blocking, { size, -size } );
                }

                const glm::vec2 tileOrigin =
                     origin + glm::vec2( static_cast<float>( x ), static_cast<float>( y ) ) * size;
                for ( const auto& instance : composite.PlacedInstances() )
                    placements.push_back( ProceduralFoliagePlacement{
                         instance.Location + tileOrigin, instance.YawDegrees, instance.PitchDegrees,
                         instance.Scale, instance.Age, instance.TypeIndex } );
            }
        return placements;
    }
} // namespace Desert::World::Foliage::Procedural
