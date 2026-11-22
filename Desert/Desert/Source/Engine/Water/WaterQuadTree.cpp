#include <Engine/Water/WaterQuadTree.hpp>

#include <Common/Core/Math/AABB.hpp>

#include <glm/common.hpp>
#include <glm/exponential.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace Desert::Water
{
    WaterQuadTree::WaterQuadTree( const WaterQuadTreeDesc& desc, std::span<const WaterQuadTreeBody> bodies )
    {
        m_LeafSize     = std::max( desc.TileSize, 1.0f );
        m_NumQuadsLOD0 = 1 << std::clamp( desc.TessellationFactor, 1, kMaxDensityCount );
        m_LODScale     = m_LeafSize * std::max( desc.LODScale, 0.5f );

        const glm::vec2  extent = glm::max( desc.ZoneExtent, glm::vec2( m_LeafSize ) );
        const glm::ivec2 extentInTiles( static_cast<int32_t>( std::ceil( extent.x / m_LeafSize ) ),
                                        static_cast<int32_t>( std::ceil( extent.y / m_LeafSize ) ) );
        const auto       rootTiles =
             std::bit_ceil( static_cast<uint32_t>( std::max( extentInTiles.x, extentInTiles.y ) ) );
        m_TreeDepth    = std::countr_zero( rootTiles ) + 1;
        m_DensityCount = std::min( m_TreeDepth, std::clamp( desc.TessellationFactor, 1, kMaxDensityCount ) );
        m_RootMin      = desc.ZoneCenter - 0.5f * extent;

        // Each body rasterised into the zone's leaf tiles (UE FWaterQuadTree::AddWaterTilesInsideBounds): a tile
        // is the body's when the footprint touches it, so the surface reaches the footprint's edge to within a
        // tile.
        const glm::vec2 zoneMax = m_RootMin + extent;
        m_MinSurfaceY           = std::numeric_limits<float>::max();
        m_MaxSurfaceY           = std::numeric_limits<float>::lowest();
        for ( size_t bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex )
        {
            const WaterQuadTreeBody& body = bodies[bodyIndex];
            const glm::vec2 lo = glm::max( body.FootprintMin, m_RootMin );
            const glm::vec2 hi = glm::min( body.FootprintMax, zoneMax );
            if ( lo.x >= hi.x || lo.y >= hi.y )
                continue;
            TileRange range;
            range.Min      = glm::ivec2( glm::floor( ( lo - m_RootMin ) / m_LeafSize ) );
            range.Max      = glm::min( glm::ivec2( glm::ceil( ( hi - m_RootMin ) / m_LeafSize ) ), extentInTiles );
            range.SurfaceY  = body.SurfaceY;
            range.BodyIndex = static_cast<uint32_t>( bodyIndex );
            range.Priority  = body.Priority;
            if ( body.Ribbon != nullptr && !BuildRibbonLeaves( *body.Ribbon, range ) )
                continue; // no in-bank texel inside the zone
            m_Bodies.push_back( std::move( range ) );
            m_MinSurfaceY = std::min( m_MinSurfaceY, body.SurfaceY );
            m_MaxSurfaceY = std::max( m_MaxSurfaceY, body.SurfaceY );
        }
    }

    bool WaterQuadTree::BuildRibbonLeaves( const RiverFlowField& flow, TileRange& range ) const noexcept
    {
        // Every leaf one of the ribbon's in-bank flow texels lies in (texels of kRiverFlowCmPerTexel are far
        // finer than a leaf, so no touched leaf is missed).
        const glm::ivec2     span  = range.Max - range.Min;
        const auto           sx    = static_cast<size_t>( span.x );
        const auto           sy    = static_cast<size_t>( span.y );
        const size_t         width = sx + 1u;
        std::vector<uint8_t> touched( sx * sy, 0u );
        const glm::vec2      step = flow.Extent / glm::vec2( glm::max( flow.Size, glm::uvec2( 1u ) ) );
        for ( uint32_t z = 0; z < flow.Size.y; ++z )
            for ( uint32_t x = 0; x < flow.Size.x; ++x )
            {
                if ( flow.Texels[static_cast<size_t>( z ) * flow.Size.x + x].z < 0.5f )
                    continue;
                const glm::vec2  centre = flow.Min + ( glm::vec2( x, z ) + 0.5f ) * step;
                const glm::ivec2 leaf =
                     glm::ivec2( glm::floor( ( centre - m_RootMin ) / m_LeafSize ) ) - range.Min;
                if ( leaf.x < 0 || leaf.y < 0 || leaf.x >= span.x || leaf.y >= span.y )
                    continue;
                touched[static_cast<size_t>( leaf.y ) * sx + static_cast<size_t>( leaf.x )] = 1u;
            }
        range.LeafSums.assign( width * ( sy + 1u ), 0 );
        for ( size_t y = 0; y < sy; ++y )
            for ( size_t x = 0; x < sx; ++x )
                range.LeafSums[( y + 1u ) * width + x + 1u] =
                     touched[y * sx + x] + range.LeafSums[y * width + x + 1u] +
                     range.LeafSums[( y + 1u ) * width + x] - range.LeafSums[y * width + x];
        return range.LeafSums.back() > 0;
    }

    int32_t WaterQuadTree::TileRange::CountLeaves( const glm::ivec2 lo, const glm::ivec2 hi ) const noexcept
    {
        const glm::ivec2 a = glm::clamp( lo, Min, Max ) - Min;
        const glm::ivec2 b = glm::clamp( hi, Min, Max ) - Min;
        if ( a.x >= b.x || a.y >= b.y )
            return 0;
        if ( LeafSums.empty() )
            return ( b.x - a.x ) * ( b.y - a.y );
        const auto width = static_cast<size_t>( Max.x - Min.x ) + 1u;
        const auto at    = [&]( const int32_t x, const int32_t y )
        { return LeafSums[static_cast<size_t>( y ) * width + static_cast<size_t>( x )]; };
        return at( b.x, b.y ) - at( a.x, b.y ) - at( b.x, a.y ) + at( a.x, a.y );
    }

    float WaterQuadTree::GetLODDistance( const int32_t lodLevel, const float lodScale )
    {
        return std::exp2( static_cast<float>( lodLevel + 1 ) ) * lodScale;
    }

    float WaterQuadTree::GetViewLODScale( const float viewDistanceScale ) const
    {
        return m_LODScale * viewDistanceScale; // DrawDistanceScale is > 0 (1 for an orthographic view)
    }

    WaterLODParams WaterQuadTree::GetWaterLODParams( const glm::vec3& observer, const float viewLODScale ) const
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
        float distToWater      = std::abs( observer.y - params.WaterHeightForLOD ) / viewLODScale;
        distToWater            = std::max( distToWater - 2.0f, 0.0f ) * 2.0f;
        const float floatLod   = distToWater > 0.0f ? std::clamp( std::log2( distToWater ), 0.0f,
                                                                  static_cast<float>( m_TreeDepth - 1 ) )
                                                    : 0.0f;
        params.HeightLODFactor = floatLod - std::floor( floatLod );
        params.LowestLOD       = std::clamp( static_cast<int32_t>( std::floor( floatLod ) ), 0, m_TreeDepth - 1 );
        return params;
    }

    std::span<const WaterTileInstance> WaterQuadTree::Traverse( const glm::vec3&       observer,
                                                                const float            viewDistanceScale,
                                                                const Core::Frustum*   frustum,
                                                                const float            verticalExtent,
                                                                WaterTraversalBuffers& buffers ) const
    {
        std::vector<WaterTileInstance>& output = buffers.Staged;
        output.clear();
        buffers.Tiles.clear();
        if ( m_Bodies.empty() )
            return buffers.Tiles;

        Traversal t;
        t.Observer       = glm::vec2( observer.x, observer.z );
        t.Frustum        = frustum;
        t.VerticalExtent = verticalExtent;
        t.LODScale       = GetViewLODScale( viewDistanceScale );
        t.LOD            = GetWaterLODParams( observer, t.LODScale );
        t.Output         = &output;
        // The walk tests a node's four children at once (VisibleChildren) before descending; the root alone
        // is tested here.
        const Node root{ .Level = m_TreeDepth - 1, .Tile = glm::ivec2( 0 ) };
        if ( Visible( root, t ) )
            SelectLOD( root, m_TreeDepth - 1, t );

        // One draw per density (UE's buckets): the renderer draws each run with that density's grid. A stable
        // counting pass over the m_DensityCount (<= 12) buckets into the caller's buffer - std::stable_sort would
        // take a heap temporary every traversal.
        std::array<uint32_t, kMaxDensityCount + 1> first{};
        for ( const WaterTileInstance& tile : output )
            ++first[static_cast<size_t>( tile.DensityIndex ) + 1u];
        for ( size_t d = 1; d < first.size(); ++d )
            first[d] += first[d - 1];
        buffers.Tiles.resize( output.size() );
        for ( const WaterTileInstance& tile : output )
            buffers.Tiles[first[static_cast<size_t>( tile.DensityIndex )]++] = tile;
        return buffers.Tiles;
    }

    // A node is Complete when one body covers it whole and no body of a higher priority touches it (a river
    // inside an ocean splits the ocean's node down to the river's leaves). A partial node - and so a leaf - is
    // drawn as the highest-priority body touching it, the first of equals.
    WaterQuadTree::Coverage WaterQuadTree::Classify( const Node& node, TileOwner& owner ) const
    {
        const glm::ivec2 lo       = node.Tile;
        const glm::ivec2 hi       = node.Tile + glm::ivec2( 1 << node.Level );
        const TileRange* touching = nullptr; // the highest priority touching the node
        const TileRange* covering = nullptr; // the highest priority covering it whole
        for ( const TileRange& body : m_Bodies )
        {
            if ( lo.x >= body.Max.x || lo.y >= body.Max.y || hi.x <= body.Min.x || hi.y <= body.Min.y )
                continue;
            // A ribbon touches a node only through a leaf it owns and covers it only when it owns every leaf.
            const int32_t owned = body.LeafSums.empty() ? -1 : body.CountLeaves( lo, hi );
            if ( owned == 0 )
                continue;
            if ( touching == nullptr || body.Priority > touching->Priority )
                touching = &body;
            const bool inside =
                 lo.x >= body.Min.x && lo.y >= body.Min.y && hi.x <= body.Max.x && hi.y <= body.Max.y;
            const bool whole = inside && ( owned < 0 || owned == ( hi.x - lo.x ) * ( hi.y - lo.y ) );
            if ( whole && ( covering == nullptr || body.Priority > covering->Priority ) )
                covering = &body;
        }
        if ( touching == nullptr )
            return Coverage::Empty;
        if ( covering != nullptr && covering->Priority >= touching->Priority )
        {
            owner = TileOwner{ covering->SurfaceY, covering->BodyIndex };
            return Coverage::Complete;
        }
        owner = TileOwner{ touching->SurfaceY, touching->BodyIndex };
        return Coverage::Partial;
    }

    Common::Math::AABB WaterQuadTree::NodeBox( const Node& node, const Traversal& t ) const
    {
        const glm::vec2    lo = m_RootMin + glm::vec2( node.Tile ) * m_LeafSize;
        const glm::vec2    hi = lo + glm::vec2( static_cast<float>( 1 << node.Level ) * m_LeafSize );
        Common::Math::AABB box;
        box.Min = glm::vec3( lo.x, m_MinSurfaceY - t.VerticalExtent, lo.y );
        box.Max = glm::vec3( hi.x, m_MaxSurfaceY + t.VerticalExtent, hi.y );
        return box;
    }

    bool WaterQuadTree::Visible( const Node& node, const Traversal& t ) const
    {
        if ( t.Frustum == nullptr )
            return true;
        return t.Frustum->Intersects( NodeBox( node, t ) );
    }

    // UE FConvexVolume::IntersectBox4 over a quadtree node's children: the four boxes Visible builds, one
    // Intersects4 (Intersects bit for bit), so bit i is Visible( Child( node, i ) ).
    uint32_t WaterQuadTree::VisibleChildren( const Node& node, const Traversal& t ) const
    {
        if ( t.Frustum == nullptr )
            return 0xFu;
        Common::Math::AABBQuad quad;
        for ( int32_t child = 0; child < 4; ++child )
            quad.Set( static_cast<std::size_t>( child ), NodeBox( Child( node, child ), t ) );
        return t.Frustum->Intersects4( quad );
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
        TileOwner      owner;
        const Coverage coverage = Classify( node, owner );
        if ( coverage == Coverage::Empty ) // visibility: the caller's (the root's, or VisibleChildren)
            return;

        // Distance to the tile (0 inside it).
        const float closest = DistanceToNode( node, t.Observer );

        // Outside this LOD's range: the node belongs to the LOD above. Drawn as that LOD with half the quads, so
        // its quads are that LOD's size.
        if ( closest > GetLODDistance( lodLevel, t.LODScale ) )
        {
            if ( coverage == Coverage::Complete )
            {
                AddNodeForRender( node, owner, 1, lodLevel + 1, t );
            }
            else
            {
                const uint32_t visible = VisibleChildren( node, t );
                for ( int32_t child = 0; child < 4; ++child )
                    if ( ( visible >> child ) & 1u )
                        SelectLODRefinement( Child( node, child ), 2, lodLevel + 1, t );
            }
            return;
        }

        // The finest LOD: a leaf is always whole.
        if ( lodLevel == 0 )
        {
            AddNodeForRender( node, owner, 0, 0, t );
            return;
        }

        // Fully inside its LOD (or the lowest LOD the camera height allows).
        if ( closest > GetLODDistance( lodLevel - 1, t.LODScale ) || lodLevel == t.LOD.LowestLOD )
        {
            if ( coverage == Coverage::Complete )
            {
                AddNodeForRender( node, owner, 0, lodLevel, t );
            }
            else
            {
                const uint32_t visible = VisibleChildren( node, t );
                for ( int32_t child = 0; child < 4; ++child )
                    if ( ( visible >> child ) & 1u )
                        SelectLODRefinement( Child( node, child ), 1, lodLevel, t );
            }
            return;
        }

        const uint32_t visible = VisibleChildren( node, t );
        for ( int32_t child = 0; child < 4; ++child )
            if ( ( visible >> child ) & 1u )
                SelectLOD( Child( node, child ), lodLevel - 1, t );
    }

    // UE FNode::SelectLODRefinement (WaterQuadTree.cpp:141-176): the LOD is decided, the node is not whole; its
    // children draw at the same LOD with a sparser grid each (same quad size).
    void WaterQuadTree::SelectLODRefinement( const Node& node, const int32_t densityLevel, const int32_t lodLevel,
                                             const Traversal& t ) const
    {
        TileOwner      owner;
        const Coverage coverage = Classify( node, owner );
        if ( coverage == Coverage::Empty ) // visibility: the caller's (the root's, or VisibleChildren)
            return;
        if ( coverage == Coverage::Complete || node.Level == 0 )
        {
            AddNodeForRender( node, owner, densityLevel, lodLevel, t );
            return;
        }
        const uint32_t visible = VisibleChildren( node, t );
        for ( int32_t child = 0; child < 4; ++child )
            if ( ( visible >> child ) & 1u )
                SelectLODRefinement( Child( node, child ), densityLevel + 1, lodLevel, t );
    }

    // UE FNode::AddNodeForRender (WaterQuadTree.cpp:11-100).
    void WaterQuadTree::AddNodeForRender( const Node& node, const TileOwner& owner, const int32_t densityLevel,
                                          const int32_t lodLevel, const Traversal& t ) const
    {
        const auto span = static_cast<float>( 1 << node.Level );

        WaterTileInstance tile;
        tile.Corner       = glm::vec2( node.Tile ) * m_LeafSize;
        tile.Size         = span * m_LeafSize;
        tile.SurfaceY     = owner.SurfaceY;
        tile.BodyIndex    = owner.BodyIndex;
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
