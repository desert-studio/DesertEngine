#include "EditMeshModelOperations.hpp"

#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>

#include <fmt/format.h>

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
            if ( from.Normals() )
                to.EnableNormals();
            if ( from.Tangents() )
                to.EnableTangents();
            if ( from.Colors() )
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
            if ( carried.IsSuccess() && from.Normals() )
                carried = loop ? SmoothNormals( result )
                               : CarryLayer( from.Normals(), result, to.Normals(), children, unit3, "normals" );
            if ( carried.IsSuccess() && from.Tangents() )
                carried = ComputeTangentsAt( result, Snapshot( result.TriangleIds() ) );
            if ( !carried.IsSuccess() )
                return Common::MakeError<EditMesh>( carried.GetError() );
            return Common::MakeSuccess( std::move( result ) );
        }

        // ── Mirror ───────────────────────────────────────────────────────────────────────────────────

        template <typename T, typename Reflect>
        Common::BoolResultStr MirrorLayer( EditMesh& mesh, EditMeshOverlay<T>* layer,
                                           const std::vector<std::pair<int, int>>& pairs,
                                           const std::vector<int>& mirrorOf, Reflect reflect, const char* name )
        {
            if ( layer == nullptr )
                return Common::MakeSuccess( true );
            std::vector<int> copyOf( static_cast<size_t>( layer->MaxElementId() ), InvalidId );
            // The reflected triangle is wound (c0, c2, c1): its corners take the elements in that order.
            constexpr std::array<int, 3> order{ 0, 2, 1 };
            for ( const auto& [original, mirrored] : pairs )
            {
                if ( !layer->IsSetTriangle( original ) )
                    continue;
                const std::array<int, 3> elements = layer->GetTriangle( original );
                const std::array<int, 3> corners  = mesh.GetTriangle( original );
                std::array<int, 3>       out{};
                for ( int k = 0; k < 3; ++k )
                {
                    const int e     = elements[order[k]];
                    const int v     = corners[order[k]];
                    const T   value = reflect( layer->GetElement( e ) );
                    if ( mirrorOf[v] == v && glm::length( value - layer->GetElement( e ) ) <= kSameValue )
                        out[k] = e;
                    else
                    {
                        if ( copyOf[e] == InvalidId )
                            copyOf[e] = layer->AppendElement( value );
                        out[k] = copyOf[e];
                    }
                }
                if ( const EditResult r = layer->SetTriangle( mesh, mirrored, out ); r != EditResult::Ok )
                    return Common::MakeFormattedError<bool>(
                         "Mirror: the {} of triangle {} could not be reflected onto triangle {} ({})", name,
                         original, mirrored, ToString( r ) );
            }
            return Common::MakeSuccess( true );
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

    const char* ToString( MirrorMode mode )
    {
        switch ( mode )
        {
            case MirrorMode::AddMirroredCopy:
                return "Add Mirrored Copy";
            case MirrorMode::CutAndMirror:
                return "Cut and Mirror";
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

    Outcome MirrorMesh( const EditMesh& mesh, const CutPlane& plane, MirrorMode mode, float weldTolerance )
    {
        const float normalLength = glm::length( plane.Normal );
        if ( !( normalLength > 0.0f ) )
            return Common::MakeError<MeshEditOutcome>( "Mirror: the plane has a zero normal" );
        if ( !( weldTolerance >= 0.0f ) )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Mirror: the weld tolerance must be >= 0 cm, not {}", weldTolerance );
        const glm::vec3 normal         = plane.Normal / normalLength;
        const auto      signedDistance = [&]( const glm::vec3& p ) { return glm::dot( p - plane.Point, normal ); };

        MeshEditOutcome out;
        EditMesh&       work = out.Mesh;
        work                 = mesh;
        int splitEdges       = 0;
        int cutTriangles     = 0;
        int inPlaneTriangles = 0;
        int farVertices      = 0;

        if ( mode == MirrorMode::CutAndMirror )
            for ( const int e : Snapshot( work.EdgeIds() ) )
            {
                const auto& ends = work.GetEdgeVertices( e );
                const float da   = signedDistance( work.GetPosition( ends[0] ) );
                const float db   = signedDistance( work.GetPosition( ends[1] ) );
                if ( !( ( da > weldTolerance && db < -weldTolerance ) ||
                        ( da < -weldTolerance && db > weldTolerance ) ) )
                    continue;
                SplitEdgeInfo info;
                if ( const EditResult r = work.SplitEdge( e, da / ( da - db ), info ); r != EditResult::Ok )
                    return Common::MakeFormattedError<MeshEditOutcome>(
                         "Mirror: edge {} crossing the plane could not be split ({})", e, ToString( r ) );
                ++splitEdges;
            }

        std::vector<char> onPlane( static_cast<size_t>( work.MaxVertexId() ), 0 );
        for ( const int v : work.VertexIds() )
        {
            const glm::vec3 p = work.GetPosition( v );
            const float     d = signedDistance( p );
            if ( std::abs( d ) <= weldTolerance )
            {
                work.SetPosition( v, p - d * normal );
                onPlane[v] = 1;
            }
            else if ( d < 0.0f )
                ++farVertices;
        }
        for ( const int t : Snapshot( work.TriangleIds() ) )
        {
            const auto& c      = work.GetTriangle( t );
            bool        inside = true;
            bool        beyond = false;
            for ( const int v : c )
            {
                inside = inside && onPlane[v] != 0;
                beyond = beyond || ( onPlane[v] == 0 && signedDistance( work.GetPosition( v ) ) < 0.0f );
            }
            const bool drop = inside || ( mode == MirrorMode::CutAndMirror && beyond );
            if ( !drop )
                continue;
            if ( const EditResult r = work.RemoveTriangle( t, true ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Mirror: triangle {} could not be removed ({})", t, ToString( r ) );
            ++( inside ? inPlaneTriangles : cutTriangles );
        }
        if ( work.TriangleCount() == 0 )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Mirror ({}): nothing is left to mirror - {} triangles lie in the plane, {} on its far side",
                 ToString( mode ), inPlaneTriangles, cutTriangles );

        std::vector<int> mirrorOf( static_cast<size_t>( work.MaxVertexId() ), InvalidId );
        std::vector<int> seam;
        for ( const int v : Snapshot( work.VertexIds() ) )
        {
            if ( work.GetVertexEdges( v ).empty() )
                continue;
            if ( onPlane[v] != 0 )
            {
                mirrorOf[v] = v;
                seam.push_back( v );
                continue;
            }
            const glm::vec3 p = work.GetPosition( v );
            mirrorOf[v]       = work.AppendVertex( p - 2.0f * signedDistance( p ) * normal );
        }
        std::vector<std::pair<int, int>> pairs;
        for ( const int t : Snapshot( work.TriangleIds() ) )
        {
            const std::array<int, 3> c = work.GetTriangle( t );
            int                      m = InvalidId;
            if ( const EditResult r = work.AppendTriangle( mirrorOf[c[0]], mirrorOf[c[2]], mirrorOf[c[1]], m );
                 r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Mirror: the reflection of triangle {} cannot join the surface ({}) - an edge of it in the "
                     "plane "
                     "already has a triangle on each side",
                     t, ToString( r ) );
            EditMeshAttributes& attributes = work.Attributes();
            attributes.SetPolyGroup( m, attributes.GetPolyGroup( t ) );
            attributes.SetMaterialId( m, attributes.GetMaterialId( t ) );
            pairs.emplace_back( t, m );
        }

        EditMeshAttributes& attributes = work.Attributes();
        const auto          same       = []( const auto& value ) { return value; };
        const auto reflectNormal = [&]( const glm::vec3& n ) { return n - 2.0f * glm::dot( n, normal ) * normal; };
        const auto reflectTangent = [&]( const glm::vec4& t )
        { return glm::vec4( reflectNormal( glm::vec3( t ) ), -t.w ); };
        Common::BoolResultStr mirrored =
             MirrorLayer( work, attributes.Normals(), pairs, mirrorOf, reflectNormal, "normals" );
        if ( mirrored.IsSuccess() )
            mirrored = MirrorLayer( work, attributes.Tangents(), pairs, mirrorOf, reflectTangent, "tangents" );
        if ( mirrored.IsSuccess() )
            mirrored = MirrorLayer( work, attributes.Colors(), pairs, mirrorOf, same, "colours" );
        for ( int layer = 0; layer < attributes.UVLayerCount() && mirrored.IsSuccess(); ++layer )
            mirrored = MirrorLayer( work, attributes.UV( layer ), pairs, mirrorOf, same, "UVs" );
        if ( mirrored.IsSuccess() && !seam.empty() )
            mirrored = RebuildNormalsByPolyGroupAt( work, seam );
        if ( mirrored.IsSuccess() && !seam.empty() && attributes.Tangents() )
        {
            std::vector<int> around;
            for ( const int v : seam )
                for ( const int t : work.GetVertexTriangles( v ) )
                    around.push_back( t );
            std::sort( around.begin(), around.end() );
            around.erase( std::unique( around.begin(), around.end() ), around.end() );
            mirrored = ComputeTangentsAt( work, around );
        }
        if ( !mirrored.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( mirrored.GetError() );
        work.Compact();

        out.Report = fmt::format( "{}: {} triangles reflected, {} seam vertices shared", ToString( mode ),
                                  pairs.size(), seam.size() );
        if ( mode == MirrorMode::CutAndMirror )
            out.Report += fmt::format( ", {} edges split and {} triangles cut away", splitEdges, cutTriangles );
        if ( inPlaneTriangles > 0 )
            out.Report += fmt::format( ", {} triangles lying in the plane dropped", inPlaneTriangles );
        if ( mode == MirrorMode::AddMirroredCopy && farVertices > 0 )
            out.Report += fmt::format( ", {} vertices were on the far side: the copy overlaps the original there",
                                       farVertices );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
