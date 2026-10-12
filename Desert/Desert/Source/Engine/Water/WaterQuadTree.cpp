#include <Engine/Water/WaterQuadTree.hpp>

#include <Common/Core/Math/AABB.hpp>

#include <glm/common.hpp>
#include <glm/exponential.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Desert::Water
{
    WaterQuadTree::WaterQuadTree( const WaterQuadTreeDesc& desc, std::span<const WaterQuadTreeBody> bodies )
    {
        m_LeafSize     = std::max( desc.TileSize, 1.0f );
        m_NumQuadsLOD0 = 1 << std::clamp( desc.TessellationFactor, 1, 12 );
        m_LODScale     = m_LeafSize * std::max( desc.LODScale, 0.5f );

        const glm::vec2  extent = glm::max( desc.ZoneExtent, glm::vec2( m_LeafSize ) );
        const glm::ivec2 extentInTiles( static_cast<int32_t>( std::ceil( extent.x / m_LeafSize ) ),
                                        static_cast<int32_t>( std::ceil( extent.y / m_LeafSize ) ) );
        const auto       rootTiles =
             std::bit_ceil( static_cast<uint32_t>( std::max( extentInTiles.x, extentInTiles.y ) ) );
        m_TreeDepth    = std::countr_zero( rootTiles ) + 1;
        m_DensityCount = std::min( m_TreeDepth, std::clamp( desc.TessellationFactor, 1, 12 ) );
        m_RootMin      = desc.ZoneCenter - 0.5f * extent;

        // Each body rasterised into the zone's leaf tiles (UE FWaterQuadTree::AddWaterTilesInsideBounds): a tile
        // is the body's when the footprint touches it, so the surface reaches the footprint's edge to within a
        // tile.
        const glm::vec2 zoneMax = m_RootMin + extent;
        m_MinSurfaceY           = std::numeric_limits<float>::max();
        m_MaxSurfaceY           = std::numeric_limits<float>::lowest();
        for ( const WaterQuadTreeBody& body : bodies )
        {
            const glm::vec2 lo = glm::max( body.FootprintMin, m_RootMin );
            const glm::vec2 hi = glm::min( body.FootprintMax, zoneMax );
            if ( lo.x >= hi.x || lo.y >= hi.y )
                continue;
            TileRange range;
            range.Min      = glm::ivec2( glm::floor( ( lo - m_RootMin ) / m_LeafSize ) );
            range.Max      = glm::min( glm::ivec2( glm::ceil( ( hi - m_RootMin ) / m_LeafSize ) ), extentInTiles );
            range.SurfaceY = body.SurfaceY;
            m_Bodies.push_back( range );
            m_MinSurfaceY = std::min( m_MinSurfaceY, body.SurfaceY );
            m_MaxSurfaceY = std::max( m_MaxSurfaceY, body.SurfaceY );
        }
    }

    float WaterQuadTree::GetLODDistance( const int32_t lodLevel, const float lodScale )
    {
        return std::exp2( static_cast<float>( lodLevel + 1 ) ) * lodScale;
    }

    WaterLODParams WaterQuadTree::GetWaterLODParams( const glm::vec3& observer ) const
    {
        WaterLODParams params;
        if ( m_Bodies.empty() )
            return params;

        // The still level under the observer (UE QueryInterpolatedTileBaseHeightAtLocation); outside every body,
        // the nearest body's, so flying off the zone's edge does not refine the whole surface as if at sea level.
        const glm::vec2 point( observer.x, observer.z );
        float           nearest = std::numeric_limits<float>::max();
        for ( const TileRange& body : m_Bodies )
        {
            const glm::vec2 lo = m_RootMin + glm::vec2( body.Min ) * m_LeafSize;
            const glm::vec2 hi = m_RootMin + glm::vec2( body.Max ) * m_LeafSize;
            const float     distance =
                 glm::length( glm::max( glm::max( lo - point, point - hi ), glm::vec2( 0.0f ) ) );
            if ( distance < nearest )
            {
                nearest                  = distance;
                params.WaterHeightForLOD = body.SurfaceY;
            }
        }

        // UE GetWaterLODParams: the lowest LOD morphs globally toward the next as the observer climbs; once fully
        // morphed the LOD selection promotes it (the clamp keeps the last LOD from morphing).
        float distToWater      = std::abs( observer.y - params.WaterHeightForLOD ) / m_LODScale;
        distToWater            = std::max( distToWater - 2.0f, 0.0f ) * 2.0f;
        const float floatLod   = distToWater > 0.0f ? std::clamp( std::log2( distToWater ), 0.0f,
                                                                  static_cast<float>( m_TreeDepth - 1 ) )
                                                    : 0.0f;
        params.HeightLODFactor = floatLod - std::floor( floatLod );
        params.LowestLOD       = std::clamp( static_cast<int32_t>( std::floor( floatLod ) ), 0, m_TreeDepth - 1 );
        return params;
    }

    std::vector<WaterTileInstance> WaterQuadTree::Traverse( const glm::vec3&     observer,
                                                            const Core::Frustum* frustum,
                                                            const float          verticalExtent ) const
    {
        std::vector<WaterTileInstance> output;
        if ( m_Bodies.empty() )
            return output;

        Traversal t;
        t.Observer       = glm::vec2( observer.x, observer.z );
        t.Frustum        = frustum;
        t.VerticalExtent = verticalExtent;
        t.LOD            = GetWaterLODParams( observer );
        t.Output         = &output;
        SelectLOD( Node{ .Level = m_TreeDepth - 1, .Tile = glm::ivec2( 0 ) }, m_TreeDepth - 1, t );

        // One draw per density (UE's buckets): the renderer draws each run with that density's grid.
        std::stable_sort( output.begin(), output.end(),
                          []( const WaterTileInstance& a, const WaterTileInstance& b )
                          { return a.DensityIndex < b.DensityIndex; } );
        return output;
    }

    WaterQuadTree::Coverage WaterQuadTree::Classify( const Node& node, float& surfaceY ) const
    {
        const glm::ivec2 lo  = node.Tile;
        const glm::ivec2 hi  = node.Tile + glm::ivec2( 1 << node.Level );
        bool             any = false;
        for ( const TileRange& body : m_Bodies )
        {
            if ( lo.x >= body.Max.x || lo.y >= body.Max.y || hi.x <= body.Min.x || hi.y <= body.Min.y )
                continue;
            if ( lo.x >= body.Min.x && lo.y >= body.Min.y && hi.x <= body.Max.x && hi.y <= body.Max.y )
            {
                surfaceY = body.SurfaceY;
                return Coverage::Complete;
            }
            if ( !any )
                surfaceY = body.SurfaceY;
            any = true;
        }
        return any ? Coverage::Partial : Coverage::Empty;
    }

    bool WaterQuadTree::Visible( const Node& node, const Traversal& t ) const
    {
        if ( t.Frustum == nullptr )
            return true;
        const glm::vec2    lo = m_RootMin + glm::vec2( node.Tile ) * m_LeafSize;
        const glm::vec2    hi = lo + glm::vec2( static_cast<float>( 1 << node.Level ) * m_LeafSize );
        Common::Math::AABB box;
        box.Min = glm::vec3( lo.x, m_MinSurfaceY - t.VerticalExtent, lo.y );
        box.Max = glm::vec3( hi.x, m_MaxSurfaceY + t.VerticalExtent, hi.y );
        return t.Frustum->Intersects( box );
    }

    float WaterQuadTree::DistanceToNode( const Node& node, const glm::vec2& point ) const
    {
        const glm::vec2 lo = m_RootMin + glm::vec2( node.Tile ) * m_LeafSize;
        const glm::vec2 hi = lo + glm::vec2( static_cast<float>( 1 << node.Level ) * m_LeafSize );
        return glm::length( glm::max( glm::max( lo - point, point - hi ), glm::vec2( 0.0f ) ) );
    }

    WaterQuadTree::Node WaterQuadTree::Child( const Node& node, const int32_t index ) const
    {
        const int32_t half = 1 << ( node.Level - 1 );
        return Node{ .Level = node.Level - 1,
                     .Tile  = node.Tile + glm::ivec2( ( index & 1 ) * half, ( index >> 1 ) * half ) };
    }

    // UE FNode::SelectLOD (WaterQuadTree.cpp:179-298). The node's level IS @p lodLevel: the walk starts at the
    // root with the deepest LOD and descends one level per LOD.
    void WaterQuadTree::SelectLOD( const Node& node, const int32_t lodLevel, const Traversal& t ) const
    {
        float          surfaceY = 0.0f;
        const Coverage coverage = Classify( node, surfaceY );
        if ( coverage == Coverage::Empty || !Visible( node, t ) )
            return;

        // Distance to the tile (0 inside it).
        const float closest = DistanceToNode( node, t.Observer );

        // Outside this LOD's range: the node belongs to the LOD above. Drawn as that LOD with half the quads, so
        // its quads are that LOD's size.
        if ( closest > GetLODDistance( lodLevel, m_LODScale ) )
        {
            if ( coverage == Coverage::Complete )
            {
                AddNodeForRender( node, surfaceY, 1, lodLevel + 1, t );
            }
            else
            {
                for ( int32_t child = 0; child < 4; ++child )
                    SelectLODRefinement( Child( node, child ), 2, lodLevel + 1, t );
            }
            return;
        }

        // The finest LOD: a leaf is always whole.
        if ( lodLevel == 0 )
        {
            AddNodeForRender( node, surfaceY, 0, 0, t );
            return;
        }

        // Fully inside its LOD (or the lowest LOD the camera height allows).
        if ( closest > GetLODDistance( lodLevel - 1, m_LODScale ) || lodLevel == t.LOD.LowestLOD )
        {
            if ( coverage == Coverage::Complete )
            {
                AddNodeForRender( node, surfaceY, 0, lodLevel, t );
            }
            else
            {
                for ( int32_t child = 0; child < 4; ++child )
                    SelectLODRefinement( Child( node, child ), 1, lodLevel, t );
            }
            return;
        }

        for ( int32_t child = 0; child < 4; ++child )
            SelectLOD( Child( node, child ), lodLevel - 1, t );
    }

    // UE FNode::SelectLODRefinement (WaterQuadTree.cpp:141-176): the LOD is decided, the node is not whole; its
    // children draw at the same LOD with a sparser grid each (same quad size).
    void WaterQuadTree::SelectLODRefinement( const Node& node, const int32_t densityLevel, const int32_t lodLevel,
                                             const Traversal& t ) const
    {
        float          surfaceY = 0.0f;
        const Coverage coverage = Classify( node, surfaceY );
        if ( coverage == Coverage::Empty || !Visible( node, t ) )
            return;
        if ( coverage == Coverage::Complete || node.Level == 0 )
        {
            AddNodeForRender( node, surfaceY, densityLevel, lodLevel, t );
            return;
        }
        for ( int32_t child = 0; child < 4; ++child )
            SelectLODRefinement( Child( node, child ), densityLevel + 1, lodLevel, t );
    }

    // UE FNode::AddNodeForRender (WaterQuadTree.cpp:11-100).
    void WaterQuadTree::AddNodeForRender( const Node& node, const float surfaceY, const int32_t densityLevel,
                                          const int32_t lodLevel, const Traversal& t ) const
    {
        const auto span = static_cast<float>( 1 << node.Level );

        WaterTileInstance tile;
        tile.Corner       = glm::vec2( node.Tile ) * m_LeafSize;
        tile.Size         = span * m_LeafSize;
        tile.SurfaceY     = surfaceY;
        tile.LODLevel     = lodLevel;
        tile.DensityIndex = std::min( densityLevel, m_DensityCount - 1 );
        // Only a tile that is not the sparsest grid can morph (there is no grid to morph to); one that is not the
        // next to sparsest can morph twice, across three LODs, because a tile reaches into the next LOD's ring.
        tile.ShouldMorph   = tile.DensityIndex != m_DensityCount - 1;
        tile.CanMorphTwice = tile.DensityIndex < m_DensityCount - 2;
        tile.HeightMorph   = lodLevel == t.LOD.LowestLOD ? t.LOD.HeightLODFactor : 0.0f;
        t.Output->push_back( tile );
    }
} // namespace Desert::Water
