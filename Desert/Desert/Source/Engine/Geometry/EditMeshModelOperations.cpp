#include "EditMeshModelOperations.hpp"

#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        using Outcome = Common::ResultStr<MeshEditOutcome>;

        // Two attribute values closer than this are the same value: a reflected seam element that lands on
        // the original shares it instead of starting a seam.
        constexpr float kSameValue = 1e-6f;

        template <typename Range>
        std::vector<int> Snapshot( const Range& ids )
        {
            std::vector<int> out;
            for ( const int id : ids )
                out.push_back( id );
            return out;
        }

        void EnableLayersLike( const EditMeshAttributes& from, EditMeshAttributes& to )
        {
            if ( from.Normals() != nullptr )
                to.EnableNormals();
            if ( from.Tangents() != nullptr )
                to.EnableTangents();
            if ( from.Colors() != nullptr )
                to.EnableColors();
        }

        // ── Subdivide ────────────────────────────────────────────────────────────────────────────────

        // The four children of one source triangle, in the order CarryLayer relies on:
        // (c0, m0, m2), (m0, c1, m1), (m2, m1, c2), (m0, m1, m2) - where m_j splits edge j (corners j, j+1).
        struct Children
        {
            int                Source = InvalidId;
            std::array<int, 4> Triangles{};
        };

        // One layer through one level: every source element keeps one element, every distinct pair of end
        // elements on an edge gets one midpoint element - a seam stays a seam in both halves.
        template <typename T, typename Midpoint>
        Common::BoolResultStr CarryLayer( const EditMeshOverlay<T>* from, EditMesh& result, EditMeshOverlay<T>* to,
                                          const std::vector<Children>& children, Midpoint midpoint,
                                          const char* name )
        {
            if ( from == nullptr )
                return Common::MakeSuccess( true );
            std::vector<int>                   corner( static_cast<size_t>( from->MaxElementId() ), InvalidId );
            std::map<std::pair<int, int>, int> middle;
            auto                               cornerOf = [&]( int e )
            {
                if ( corner[e] == InvalidId )
                    corner[e] = to->AppendElement( from->GetElement( e ) );
                return corner[e];
            };
            auto middleOf = [&]( int a, int b )
            {
                const std::pair<int, int> key{ std::min( a, b ), std::max( a, b ) };
                const auto                it = middle.find( key );
                if ( it != middle.end() )
                    return it->second;
                const int e = to->AppendElement( midpoint( from->GetElement( a ), from->GetElement( b ) ) );
                middle.emplace( key, e );
                return e;
            };
            for ( const Children& c : children )
            {
                if ( !from->IsSetTriangle( c.Source ) )
                    continue;
                const std::array<int, 3>                e  = from->GetTriangle( c.Source );
                const int                               c0 = cornerOf( e[0] );
                const int                               c1 = cornerOf( e[1] );
                const int                               c2 = cornerOf( e[2] );
                const int                               m0 = middleOf( e[0], e[1] );
                const int                               m1 = middleOf( e[1], e[2] );
                const int                               m2 = middleOf( e[2], e[0] );
                const std::array<std::array<int, 3>, 4> sets{
                     { { c0, m0, m2 }, { m0, c1, m1 }, { m2, m1, c2 }, { m0, m1, m2 } } };
                for ( int k = 0; k < 4; ++k )
                    if ( const EditResult r = to->SetTriangle( result, c.Triangles[k], sets[k] );
                         r != EditResult::Ok )
                        return Common::MakeFormattedError<bool>(
                             "Subdivide: the {} of triangle {} could not be carried to its child {} ({})", name,
                             c.Source, c.Triangles[k], ToString( r ) );
            }
            return Common::MakeSuccess( true );
        }

        glm::vec3 LoopVertex( const EditMesh& mesh, int v )
        {
            const glm::vec3        p          = mesh.GetPosition( v );
            const std::vector<int> neighbours = mesh.GetVertexNeighbours( v );
            const int              n          = static_cast<int>( neighbours.size() );
            if ( n < 3 )
                return p;
            const float beta = n == 3 ? 3.0f / 16.0f : 3.0f / ( 8.0f * static_cast<float>( n ) );
            glm::vec3   sum( 0.0f );
            for ( const int o : neighbours )
                sum += mesh.GetPosition( o );
            return ( 1.0f - static_cast<float>( n ) * beta ) * p + beta * sum;
        }

        int OppositeCorner( const EditMesh& mesh, int t, const std::array<int, 2>& ends )
        {
            for ( const int c : mesh.GetTriangle( t ) )
                if ( c != ends[0] && c != ends[1] )
                    return c;
            return InvalidId;
        }

        // Loop only: one normal element per vertex, the angle-weighted average of its triangles.
        Common::BoolResultStr SmoothNormals( EditMesh& mesh )
        {
            NormalOverlay*         normals = mesh.Attributes().Normals();
            std::vector<glm::vec3> sum( static_cast<size_t>( mesh.MaxVertexId() ), glm::vec3( 0.0f ) );
            for ( const int t : mesh.TriangleIds() )
            {
                const glm::vec3 n = TriangleNormal( mesh, t );
                const auto&     c = mesh.GetTriangle( t );
                for ( int j = 0; j < 3; ++j )
                    sum[c[j]] += CornerAngle( mesh, t, j ) * n;
            }
            std::vector<int> element( sum.size(), InvalidId );
            for ( const int v : mesh.VertexIds() )
            {
                if ( mesh.GetVertexEdges( v ).empty() )
                    continue;
                const float length = glm::length( sum[v] );
                if ( !( length > 0.0f ) )
                    return Common::MakeFormattedError<bool>(
                         "Subdivide: vertex {} at ({}, {}, {}) has only zero-area triangles - it has no normal", v,
                         mesh.GetPosition( v ).x, mesh.GetPosition( v ).y, mesh.GetPosition( v ).z );
                element[v] = normals->AppendElement( sum[v] / length );
            }
            for ( const int t : mesh.TriangleIds() )
            {
                const auto& c = mesh.GetTriangle( t );
                if ( const EditResult r =
                          normals->SetTriangle( mesh, t, { element[c[0]], element[c[1]], element[c[2]] } );
                     r != EditResult::Ok )
                    return Common::MakeFormattedError<bool>( "Subdivide: the smooth normals of triangle {} ({})",
                                                             t, ToString( r ) );
            }
            return Common::MakeSuccess( true );
        }

        Common::ResultStr<EditMesh> SubdivideOnce( const EditMesh& source, SubdivideScheme scheme )
        {
            const bool loop = scheme == SubdivideScheme::Loop;
            EditMesh   result;

            std::vector<int> vertex( static_cast<size_t>( source.MaxVertexId() ), InvalidId );
            for ( const int v : source.VertexIds() )
            {
                const bool pinned = source.IsBoundaryVertex( v ) || source.IsBowtieVertex( v );
                vertex[v] =
                     result.AppendVertex( loop && !pinned ? LoopVertex( source, v ) : source.GetPosition( v ) );
            }
            std::vector<int> middle( static_cast<size_t>( source.MaxEdgeId() ), InvalidId );
            for ( const int e : source.EdgeIds() )
            {
                const auto&     ends = source.GetEdgeVertices( e );
                const auto&     tris = source.GetEdgeTriangles( e );
                const glm::vec3 a    = source.GetPosition( ends[0] );
                const glm::vec3 b    = source.GetPosition( ends[1] );
                glm::vec3       p    = 0.5f * ( a + b );
                if ( loop && tris[1] != InvalidId )
                    p = 0.375f * ( a + b ) +
                        0.125f * ( source.GetPosition( OppositeCorner( source, tris[0], ends ) ) +
                                   source.GetPosition( OppositeCorner( source, tris[1], ends ) ) );
                middle[e] = result.AppendVertex( p );
            }

            const EditMeshAttributes& from = source.Attributes();
            EditMeshAttributes&       to   = result.Attributes();
            EnableLayersLike( from, to );
            if ( !to.SetUVLayerCount( from.UVLayerCount() ) )
                return Common::MakeFormattedError<EditMesh>( "Subdivide: {} UV layers could not be set up",
                                                             from.UVLayerCount() );

            std::vector<Children> children;
            children.reserve( static_cast<size_t>( source.TriangleCount() ) );
            for ( const int t : source.TriangleIds() )
            {
                const auto&                             c  = source.GetTriangle( t );
                const auto&                             e  = source.GetTriangleEdges( t );
                const int                               m0 = middle[e[0]];
                const int                               m1 = middle[e[1]];
                const int                               m2 = middle[e[2]];
                const std::array<std::array<int, 3>, 4> corners{ { { vertex[c[0]], m0, m2 },
                                                                   { m0, vertex[c[1]], m1 },
                                                                   { m2, m1, vertex[c[2]] },
                                                                   { m0, m1, m2 } } };
                Children                                child{ t, {} };
                for ( int k = 0; k < 4; ++k )
                {
                    const EditResult r =
                         result.AppendTriangle( corners[k][0], corners[k][1], corners[k][2], child.Triangles[k] );
                    if ( r != EditResult::Ok )
                        return Common::MakeFormattedError<EditMesh>(
                             "Subdivide: child {} of triangle {} could not be added ({})", k, t, ToString( r ) );
                    to.SetPolyGroup( child.Triangles[k], from.GetPolyGroup( t ) );
                    to.SetMaterialId( child.Triangles[k], from.GetMaterialId( t ) );
                }
                children.push_back( child );
            }

            const auto linear2 = []( const glm::vec2& a, const glm::vec2& b ) { return 0.5f * ( a + b ); };
            const auto linear4 = []( const glm::vec4& a, const glm::vec4& b ) { return 0.5f * ( a + b ); };
            const auto unit3   = []( const glm::vec3& a, const glm::vec3& b )
            {
                const glm::vec3 s = a + b;
                const float     l = glm::length( s );
                // Opposite unit normals on one edge only happen on a knife-thin fin; either end is as right.
                return l > 0.0f ? s / l : a;
            };
            Common::BoolResultStr carried =
                 CarryLayer( from.Colors(), result, to.Colors(), children, linear4, "colours" );
            for ( int layer = 0; layer < from.UVLayerCount() && carried.IsSuccess(); ++layer )
                carried = CarryLayer( from.UV( layer ), result, to.UV( layer ), children, linear2, "UVs" );
            if ( carried.IsSuccess() && ( from.Normals() != nullptr ) )
                carried = loop ? SmoothNormals( result )
                               : CarryLayer( from.Normals(), result, to.Normals(), children, unit3, "normals" );
            if ( carried.IsSuccess() && ( from.Tangents() != nullptr ) )
                carried = ComputeTangentsAt( result, Snapshot( result.TriangleIds() ) );
            if ( !carried.IsSuccess() )
                return Common::MakeError<EditMesh>( carried.GetError() );
            return Common::MakeSuccess( std::move( result ) );
        }
    } // namespace

    const char* ToString( SubdivideScheme scheme )
    {
        switch ( scheme )
        {
            case SubdivideScheme::Uniform:
                return "Uniform";
            case SubdivideScheme::Loop:
                return "Loop";
        }
        return "Unknown";
    }

    Outcome SubdivideMesh( const EditMesh& mesh, int levels, SubdivideScheme scheme )
    {
        if ( levels < 1 || levels > kMaxSubdivideLevels )
            return Common::MakeFormattedError<MeshEditOutcome>( "Subdivide: the level must be 1 .. {}, not {}",
                                                                kMaxSubdivideLevels, levels );
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<MeshEditOutcome>( "Subdivide: the mesh has no triangles" );
        const long long triangles = static_cast<long long>( mesh.TriangleCount() ) << ( 2 * levels );
        if ( triangles > kMaxSubdividedTriangles )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Subdivide: {} levels would turn {} triangles into {}, above the limit of {}", levels,
                 mesh.TriangleCount(), triangles, kMaxSubdividedTriangles );

        MeshEditOutcome out;
        out.Mesh = mesh;
        for ( int level = 0; level < levels; ++level )
        {
            auto next = SubdivideOnce( out.Mesh, scheme );
            if ( !next.IsSuccess() )
                return Common::MakeFormattedError<MeshEditOutcome>( "{} (level {} of {})", next.GetError(),
                                                                    level + 1, levels );
            out.Mesh = next.ExtractValue();
        }
        out.Report = fmt::format( "{} x{}: {} -> {} triangles", ToString( scheme ), levels, mesh.TriangleCount(),
                                  out.Mesh.TriangleCount() );
        return Common::MakeSuccess( std::move( out ) );
    }
    const char* ToString( TrimSide side )
    {
        switch ( side )
        {
            case TrimSide::RemoveInside:
                return "Remove Inside";
            case TrimSide::RemoveOutside:
                return "Remove Outside";
        }
        return "Unknown";
    }

    Outcome TrimMesh( const EditMesh& mesh, const EditMesh& cutter, const glm::mat4& cutterToMesh, TrimSide side )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<MeshEditOutcome>( "Trim: the mesh has no triangle" );
        if ( cutter.TriangleCount() == 0 )
            return Common::MakeError<MeshEditOutcome>( "Trim: the cutter has no triangle" );
        for ( const int e : cutter.EdgeIds() )
            if ( cutter.IsBoundaryEdge( e ) )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Trim: the cutter is open at its edge {} - it must be a closed convex solid (a Boolean, "
                     "which would take any closed cutter, is not in this wave)",
                     e );

        std::vector<glm::vec3> corners( static_cast<size_t>( cutter.MaxVertexId() ) );
        float                  extent = 0.0f;
        for ( const int v : cutter.VertexIds() )
        {
            corners[v] = glm::vec3( cutterToMesh * glm::vec4( cutter.GetPosition( v ), 1.0f ) );
            extent     = std::max(
                 { extent, std::abs( corners[v].x ), std::abs( corners[v].y ), std::abs( corners[v].z ) } );
        }
        // Float noise of a transformed corner grows with its distance from the origin.
        const float tolerance = 1e-3f + 1e-6f * extent;
        // A mirroring transform turns the cutter inside out; its faces' outward normals flip with it.
        const float handed = glm::determinant( glm::mat3( cutterToMesh ) ) < 0.0f ? -1.0f : 1.0f;

        std::vector<CutPlane> planes;
        for ( const int t : cutter.TriangleIds() )
        {
            const auto&     c = cutter.GetTriangle( t );
            const glm::vec3 face =
                 handed * glm::cross( corners[c[1]] - corners[c[0]], corners[c[2]] - corners[c[0]] );
            const float area = glm::length( face );
            if ( !( area > tolerance * tolerance ) )
                continue;
            const CutPlane plane{ corners[c[0]], face / area };
            const bool     known =
                 std::any_of( planes.begin(), planes.end(),
                              [&]( const CutPlane& p )
                              {
                                  return glm::dot( p.Normal, plane.Normal ) > 1.0f - 1e-6f &&
                                         std::abs( glm::dot( plane.Point - p.Point, p.Normal ) ) <= tolerance;
                              } );
            if ( known )
                continue;
            for ( const int v : cutter.VertexIds() )
                if ( const float d = glm::dot( corners[v] - plane.Point, plane.Normal ); d > tolerance )
                    return Common::MakeFormattedError<MeshEditOutcome>(
                         "Trim: the cutter is not convex - its vertex {} is {:.3f} cm in front of the plane of "
                         "its "
                         "triangle {}; without a Boolean only a convex cutter can trim",
                         v, d, t );
            planes.push_back( plane );
        }
        if ( planes.size() < 4 )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Trim: the cutter has {} distinct face planes - a closed solid needs at least 4", planes.size() );

        MeshEditOutcome out{ mesh, ElementSelection( ElementMode::Triangle ), {} };
        EditMesh&       work = out.Mesh;
        const int       from = work.TriangleCount();
        for ( const CutPlane& plane : planes )
        {
            std::vector<char> inSet( static_cast<size_t>( work.MaxTriangleId() ), 1 );
            if ( auto split = SplitMeshAlongPlane( work, inSet, plane, "Trim" ); !split.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( split.GetError() );
        }
        std::vector<int> inside;
        std::vector<int> outside;
        for ( const int t : work.TriangleIds() )
        {
            const auto&     c = work.GetTriangle( t );
            const glm::vec3 center =
                 ( work.GetPosition( c[0] ) + work.GetPosition( c[1] ) + work.GetPosition( c[2] ) ) / 3.0f;
            const bool in = std::all_of( planes.begin(), planes.end(), [&]( const CutPlane& p )
                                         { return glm::dot( center - p.Point, p.Normal ) < -1e-4f; } );
            ( in ? inside : outside ).push_back( t );
        }
        const std::vector<int>& removed = side == TrimSide::RemoveInside ? inside : outside;
        if ( inside.empty() )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Trim: the cutter does not reach the mesh - no triangle of it is inside the cutter's {} planes",
                 planes.size() );
        if ( removed.size() == static_cast<size_t>( work.TriangleCount() ) )
            return Common::MakeFormattedError<MeshEditOutcome>( "Trim ({}): all {} triangles would be removed",
                                                                ToString( side ), removed.size() );
        const int split = work.TriangleCount() - from;
        for ( const int t : removed )
            if ( const EditResult r = work.RemoveTriangle( t, true ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>( "Trim: removing triangle {} refused: {}", t,
                                                                    ToString( r ) );
        work.Compact();
        if ( auto valid = work.CheckValidity(); !valid.IsSuccess() )
            return Common::MakeFormattedError<MeshEditOutcome>( "Trim: the result is not a valid mesh: {}",
                                                                valid.GetError() );
        out.Report = fmt::format( "{}: {} cutter planes, {} triangles added by the splits, {} removed",
                                  ToString( side ), planes.size(), split, removed.size() );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
