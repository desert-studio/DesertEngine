#include "EditMeshOperations.hpp"

#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace Desert::Geometry
{
    namespace
    {
        // A face may tilt this far from the averaged direction before the lengthening stops: 1 / 0.25 = at
        // most 4x the distance, the point past which a vertex would shoot off rather than keep faces apart.
        constexpr float kMinNormalAgreement = 0.25f;
        // A triangle whose normal turns by more than 90 degrees has flipped over.
        constexpr float kFlipDot = 0.0f;

        // One edge of the region's boundary, directed the way the region triangle traverses it.
        struct BoundaryEdge
        {
            int From     = InvalidId;
            int To       = InvalidId;
            int Triangle = InvalidId; // the region triangle on it
            int Outside  = InvalidId; // the other triangle, InvalidId on the mesh's open border
        };

        struct Region
        {
            std::vector<int>  Triangles; // ascending
            std::vector<char> InRegion;  // by triangle ID
            std::vector<int>  Vertices;  // ascending
            // Closed loops of boundary edges, each in region winding order (loop[i].To == loop[i+1].From).
            std::vector<std::vector<BoundaryEdge>> Loops;
            int                                    BoundaryEdgeCount = 0;
        };

        glm::vec3 AreaNormal( const EditMesh& mesh, int t ) // length = twice the area
        {
            const auto& c = mesh.GetTriangle( t );
            return glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                               mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) );
        }

        // The selection as a triangle region with its boundary loops. `what` names the operation in refusals.
        Common::ResultStr<Region> BuildRegion( const EditMesh& mesh, const ElementSelection& selection,
                                               const char* what )
        {
            if ( selection.Empty() )
                return Common::MakeFormattedError<Region>( "{}: the selection is empty ({} mode)", what,
                                                           ToString( selection.Mode() ) );
            ElementSelection  live  = selection;
            const PruneReport stale = live.Prune( mesh );
            if ( stale.Total() > 0 )
                return Common::MakeFormattedError<Region>(
                     "{}: {} of the {} selected {} elements are no longer on this mesh ({} missing, {} changed)",
                     what, stale.Total(), selection.Size(), ToString( selection.Mode() ), stale.Missing,
                     stale.Changed );
            const ElementSelection triangles = ConvertSelection( mesh, live, ElementMode::Triangle );
            if ( triangles.Empty() )
                return Common::MakeFormattedError<Region>(
                     "{}: the {} selected {} elements cover no whole triangle - select every edge (or vertex) "
                     "of a triangle, or switch to Triangle / PolyGroup mode",
                     what, selection.Size(), ToString( selection.Mode() ) );

            Region region;
            region.Triangles.assign( triangles.Ids().begin(), triangles.Ids().end() );
            region.InRegion.assign( static_cast<size_t>( mesh.MaxTriangleId() ), 0 );
            std::vector<char> vertexIn( static_cast<size_t>( mesh.MaxVertexId() ), 0 );
            for ( const int t : region.Triangles )
            {
                region.InRegion[t] = 1;
                for ( const int v : mesh.GetTriangle( t ) )
                    vertexIn[v] = 1;
            }
            for ( int v = 0; v < mesh.MaxVertexId(); ++v )
                if ( vertexIn[v] )
                    region.Vertices.push_back( v );

            // Boundary edges keyed by their start vertex; a vertex that starts two is where the region
            // touches itself.
            std::unordered_map<int, BoundaryEdge> byFrom;
            for ( const int t : region.Triangles )
            {
                const auto& corners = mesh.GetTriangle( t );
                const auto& edges   = mesh.GetTriangleEdges( t );
                for ( int j = 0; j < 3; ++j )
                {
                    const auto& et    = mesh.GetEdgeTriangles( edges[j] );
                    const int   other = et[0] == t ? et[1] : et[0];
                    if ( other != InvalidId && region.InRegion[other] )
                        continue;
                    const BoundaryEdge b{ corners[j], corners[( j + 1 ) % 3], t, other };
                    if ( !byFrom.emplace( b.From, b ).second )
                        return Common::MakeFormattedError<Region>(
                             "{}: the region touches itself at vertex {} (two boundary loops pass through it); "
                             "the cut-loose copy would pinch there - grow or shrink the selection by that corner",
                             what, b.From );
                    ++region.BoundaryEdgeCount;
                }
            }
            while ( !byFrom.empty() )
            {
                // Start from the lowest vertex so the loop order does not depend on hashing.
                int start = byFrom.begin()->first;
                for ( const auto& [from, edge] : byFrom )
                    start = std::min( start, from );
                std::vector<BoundaryEdge> loop;
                int                       at = start;
                do
                {
                    const auto it = byFrom.find( at );
                    if ( it == byFrom.end() )
                        return Common::MakeFormattedError<Region>(
                             "{}: the boundary of the region does not close at vertex {} - the mesh's edge "
                             "table is inconsistent",
                             what, at );
                    loop.push_back( it->second );
                    at = it->second.To;
                    byFrom.erase( it );
                } while ( at != start );
                region.Loops.push_back( std::move( loop ) );
            }
            return Common::MakeSuccess( std::move( region ) );
        }

        // Per region vertex: the angle-weighted average of the region's face normals at it, lengthened so each
        // of those faces moves by the full distance.
        std::unordered_map<int, glm::vec3> RegionVertexDirections( const EditMesh& mesh, const Region& region )
        {
            std::unordered_map<int, glm::vec3> sums;
            for ( const int t : region.Triangles )
            {
                const glm::vec3 n = TriangleNormal( mesh, t );
                for ( int j = 0; j < 3; ++j )
                    sums[mesh.GetTriangle( t )[j]] += n * CornerAngle( mesh, t, j );
            }
            std::unordered_map<int, glm::vec3> directions;
            for ( auto& [v, sum] : sums )
            {
                const float length = glm::length( sum );
                if ( length <= 0.0f )
                {
                    directions[v] = glm::vec3( 0.0f );
                    continue;
                }
                const glm::vec3 n         = sum / length;
                float           agreement = 1.0f;
                for ( const int t : mesh.GetVertexTriangles( v ) )
                    if ( region.InRegion[t] && glm::length( AreaNormal( mesh, t ) ) > 0.0f )
                        agreement = std::min( agreement, glm::dot( n, TriangleNormal( mesh, t ) ) );
                directions[v] = n / std::max( agreement, kMinNormalAgreement );
            }
            return directions;
        }

        // ── normals: rebuilt per polygroup at a set of vertices ─────────────────────────────────────────

        // Refuses when a triangle's normal turned by more than 90 degrees between `before` (ID b) and the
        // result (ID a).
        Common::BoolResultStr RequireNoFlip( const EditMesh& before, const EditMesh& after,
                                             const std::vector<std::pair<int, int>>& pairs, const char* what,
                                             float distance )
        {
            for ( const auto& [b, a] : pairs )
            {
                const glm::vec3 nb = TriangleNormal( before, b );
                const glm::vec3 na = TriangleNormal( after, a );
                if ( glm::length( nb ) > 0.0f && glm::dot( nb, na ) <= kFlipDot )
                    return Common::MakeFormattedError<bool>(
                         "{}: at {} cm triangle {} turns over (its normal turns by more than 90 degrees) - the "
                         "distance is larger than the region allows there",
                         what, distance, b );
            }
            return Common::MakeSuccess( true );
        }

    } // namespace

    Common::ResultStr<MeshEditOutcome> DeleteSelection( const EditMesh& mesh, const ElementSelection& selection )
    {
        auto built = BuildRegion( mesh, selection, "Delete" );
        if ( !built.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( built.GetError() );
        const Region&   region = built.GetValue();
        MeshEditOutcome out;
        out.Mesh      = mesh;
        out.Selection = ElementSelection( selection.Mode() );
        for ( const int t : region.Triangles )
            if ( const EditResult r = out.Mesh.RemoveTriangle( t, true ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>( "Delete: removing triangle {} refused: {}", t,
                                                                    ToString( r ) );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<MeshEditOutcome> OffsetSelection( const EditMesh& mesh, const ElementSelection& selection,
                                                        float distance )
    {
        if ( distance == 0.0f )
            return Common::MakeError<MeshEditOutcome>( "Offset: distance 0 cm changes nothing" );
        auto built = BuildRegion( mesh, selection, "Offset" );
        if ( !built.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( built.GetError() );
        const Region& region = built.GetValue();

        MeshEditOutcome out;
        out.Mesh        = mesh;
        const auto dirs = RegionVertexDirections( mesh, region );
        for ( const int v : region.Vertices )
            out.Mesh.SetPosition( v, mesh.GetPosition( v ) + dirs.at( v ) * distance );

        // Every triangle at a moved vertex changed shape; normals are rebuilt at all of their corners.
        std::vector<int> touched;
        std::vector<int> corners;
        for ( const int v : region.Vertices )
            for ( const int t : mesh.GetVertexTriangles( v ) )
                touched.push_back( t );
        std::sort( touched.begin(), touched.end() );
        touched.erase( std::unique( touched.begin(), touched.end() ), touched.end() );
        std::vector<std::pair<int, int>> pairs;
        for ( const int t : touched )
        {
            pairs.emplace_back( t, t );
            for ( const int v : mesh.GetTriangle( t ) )
                corners.push_back( v );
        }
        std::sort( corners.begin(), corners.end() );
        corners.erase( std::unique( corners.begin(), corners.end() ), corners.end() );
        if ( auto r = RequireNoFlip( mesh, out.Mesh, pairs, "Offset", distance ); !r.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( r.GetError() );
        if ( auto r = RebuildNormalsByPolyGroupAt( out.Mesh, corners ); !r.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( std::string( "Offset: " ) + r.GetError() );
        out.Selection = selection;
        (void)out.Selection.Prune( out.Mesh );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
