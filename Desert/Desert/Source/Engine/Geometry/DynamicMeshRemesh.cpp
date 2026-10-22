#include "DynamicMeshRemesh.hpp"

#include "Engine/Geometry/MeshCore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Desert::Geometry
{
    namespace
    {
        // UE FRemesher: an edge is split above 4/3 of the target and collapsed below 2/3 of it (the band keeps a
        // split's two halves from being collapsed straight back).
        constexpr double kMaxShare = 4.0 / 3.0;
        constexpr double kMinShare = 2.0 / 3.0;
        // The valence a regular interior vertex has (UE: the flip target).
        constexpr int kTargetValence = 6;
        // A collapse or flip that turns a triangle further than this (cosine) is refused (UE bPreventNormalFlips).
        constexpr double kMinNormalDot = 0.5;

        struct Frame
        {
            DynamicMesh3&     Mesh;
            const glm::dmat4& ToWorld;

            glm::dvec3 World( int v ) const
            {
                return glm::dvec3( ToWorld * glm::dvec4( Mesh.GetVertex( v ), 1.0 ) );
            }
            double WorldLength( int a, int b ) const
            {
                return glm::length( World( a ) - World( b ) );
            }
        };

        // A vertex the remesher must not move or remove: on an open border or an attribute seam.
        bool IsFixedVertex( const DynamicMesh3& mesh, int v )
        {
            if ( mesh.IsBoundaryVertex( v ) )
                return true;
            return mesh.HasAttributes() && mesh.Attributes()->IsSeamVertex( v, true );
        }

        glm::dvec3 TriNormalWith( const DynamicMesh3& mesh, int t, int movedVertex, const glm::dvec3& movedPos )
        {
            const Index3i tv = mesh.GetTriangle( t );
            glm::dvec3    p[3];
            for ( int j = 0; j < 3; ++j )
                p[j] = tv[j] == movedVertex ? movedPos : mesh.GetVertex( tv[j] );
            const glm::dvec3 n   = glm::cross( p[1] - p[0], p[2] - p[0] );
            const double     len = glm::length( n );
            return len > 1e-20 ? n / len : glm::dvec3( 0.0 );
        }

        // UE FRemesher::CollapseEdge's checks: both ends free, no surviving triangle turned over and no new
        // edge longer than the split threshold (or the collapse would be split straight back).
        bool TryCollapse( const Frame& f, int eid, double maxLen, std::vector<int>& touched )
        {
            DynamicMesh3& mesh = f.Mesh;
            const Index2i ev   = mesh.GetEdgeV( eid );
            if ( IsFixedVertex( mesh, ev.A ) || IsFixedVertex( mesh, ev.B ) )
                return false;
            const glm::dvec3 mid      = ( mesh.GetVertex( ev.A ) + mesh.GetVertex( ev.B ) ) * 0.5;
            const glm::dvec3 midWorld = glm::dvec3( f.ToWorld * glm::dvec4( mid, 1.0 ) );
            for ( const int moved : { ev.A, ev.B } )
            {
                const int other = moved == ev.A ? ev.B : ev.A;
                for ( const int t : mesh.VtxTrianglesItr( moved ) )
                {
                    const Index3i tv = mesh.GetTriangle( t );
                    if ( tv.A == other || tv.B == other || tv.C == other )
                        continue; // removed by the collapse
                    const glm::dvec3 after = TriNormalWith( mesh, t, moved, mid );
                    if ( glm::dot( mesh.GetTriNormal( t ), after ) < kMinNormalDot )
                        return false;
                }
                for ( const int n : mesh.VtxVerticesItr( moved ) )
                    if ( n != other && glm::length( f.World( n ) - midWorld ) > maxLen )
                        return false;
            }
            DynamicMesh3::EdgeCollapseInfo info;
            if ( mesh.CollapseEdge( ev.A, ev.B, 0.5, info ) != MeshResult::Ok )
                return false;
            mesh.SetVertex( info.KeptVertex, mid );
            touched.push_back( info.KeptVertex );
            return true;
        }

        // UE FRemesher::FlipEdge: flip when it brings the four vertices closer to valence 6, and never when it
        // folds the pair of triangles over.
        bool TryFlip( DynamicMesh3& mesh, int eid, std::vector<int>& touched )
        {
            if ( mesh.IsBoundaryEdge( eid ) || ( mesh.HasAttributes() && mesh.Attributes()->IsSeamEdge( eid ) ) )
                return false;
            const Index2i ev = mesh.GetEdgeV( eid );
            const Index2i ov = mesh.GetEdgeOpposingV( eid );
            if ( ov.A == DynamicMesh3::InvalidID || ov.B == DynamicMesh3::InvalidID )
                return false;
            const int  a = mesh.GetVtxEdgeCount( ev.A ), b = mesh.GetVtxEdgeCount( ev.B );
            const int  c = mesh.GetVtxEdgeCount( ov.A ), d = mesh.GetVtxEdgeCount( ov.B );
            const auto dev    = []( int v ) { return std::abs( v - kTargetValence ); };
            const int  before = dev( a ) + dev( b ) + dev( c ) + dev( d );
            const int  after  = dev( a - 1 ) + dev( b - 1 ) + dev( c + 1 ) + dev( d + 1 );
            if ( after >= before || a <= 3 || b <= 3 )
                return false;
            const Index2i              et = mesh.GetEdgeT( eid );
            const glm::dvec3           n0 = mesh.GetTriNormal( et.A ), n1 = mesh.GetTriNormal( et.B );
            DynamicMesh3::EdgeFlipInfo info;
            if ( mesh.FlipEdge( eid, info ) != MeshResult::Ok )
                return false;
            const glm::dvec3 avg = glm::normalize( n0 + n1 );
            if ( glm::dot( mesh.GetTriNormal( info.Triangles.A ), avg ) < kMinNormalDot ||
                 glm::dot( mesh.GetTriNormal( info.Triangles.B ), avg ) < kMinNormalDot )
            {
                DynamicMesh3::EdgeFlipInfo back;
                mesh.FlipEdge( info.EdgeID, back ); // folds: undo it
                return false;
            }
            touched.insert( touched.end(), { ev.A, ev.B, ov.A, ov.B } );
            return true;
        }

        // One sweep over the candidate edges (UE FRemesher::BasicRemeshPass): split, else collapse, else flip.
        // Returns how many edits it made.
        int TopologyPass( const Frame& f, const RemeshSettings& s, const std::vector<int>& edges,
                          std::vector<int>& touched )
        {
            DynamicMesh3& mesh   = f.Mesh;
            const double  target = s.TargetEdgeLength;
            const double  maxLen = target * kMaxShare, minLen = target * kMinShare;
            int           edits = 0;
            for ( const int eid : edges )
            {
                if ( !mesh.IsEdge( eid ) )
                    continue; // removed by an earlier collapse this pass
                const Index2i ev  = mesh.GetEdgeV( eid );
                const double  len = f.WorldLength( ev.A, ev.B );
                if ( s.Splits && len > maxLen )
                {
                    DynamicMesh3::EdgeSplitInfo info;
                    if ( mesh.SplitEdge( eid, info, 0.5 ) == MeshResult::Ok )
                    {
                        touched.insert( touched.end(), { info.NewVertex, ev.A, ev.B } );
                        ++edits;
                    }
                }
                else if ( s.Collapses && len < minLen )
                    edits += TryCollapse( f, eid, maxLen, touched ) ? 1 : 0;
                else if ( s.Flips )
                    edits += TryFlip( mesh, eid, touched ) ? 1 : 0;
            }
            return edits;
        }

        std::vector<int> AllEdges( const DynamicMesh3& mesh )
        {
            std::vector<int> edges;
            edges.reserve( static_cast<size_t>( mesh.EdgeCount() ) );
            for ( const int e : mesh.EdgeIndicesItr() )
                edges.push_back( e );
            return edges;
        }

        void RecomputeNormals( DynamicMesh3& mesh )
        {
            if ( !mesh.HasAttributes() || mesh.Attributes()->PrimaryNormals() == nullptr )
                return;
            MeshNormals normals( &mesh );
            normals.RecomputeOverlayNormals( mesh.Attributes()->PrimaryNormals() );
            normals.CopyToOverlay( mesh.Attributes()->PrimaryNormals() );
        }
    } // namespace

    bool RemeshMesh( DynamicMesh3& mesh, const glm::dmat4& localToWorld, const RemeshSettings& settings )
    {
        if ( settings.TargetEdgeLength <= 0.0f || mesh.TriangleCount() == 0 )
            return false;
        // The surface the remesh must keep (UE: the tool's projection target, a copy of the input + its tree).
        const DynamicMesh3   original( mesh );
        DynamicMeshAABBTree3 tree( &original );
        const Frame          f{ mesh, localToWorld };
        const double         alpha = std::clamp( static_cast<double>( settings.SmoothingStrength ), 0.0, 1.0 );
        std::vector<int>     touched;
        for ( int pass = 0; pass < std::max( 1, settings.Iterations ); ++pass )
        {
            touched.clear();
            TopologyPass( f, settings, AllEdges( mesh ), touched );

            // UE FRemesher::FullSmoothPass_Buffer: tangential Laplacian (the one-ring centroid with its normal
            // component removed, so smoothing evens the triangles without shrinking the shape), then
            // FullProjectionPass: every vertex back onto the original surface.
            std::vector<std::pair<int, glm::dvec3>> next;
            next.reserve( static_cast<size_t>( mesh.VertexCount() ) );
            for ( const int v : mesh.VertexIndicesItr() )
            {
                glm::dvec3 p = mesh.GetVertex( v );
                if ( alpha > 0.0 && !IsFixedVertex( mesh, v ) )
                {
                    glm::dvec3 centroid( 0.0 );
                    int        count = 0;
                    for ( const int n : mesh.VtxVerticesItr( v ) )
                    {
                        centroid += mesh.GetVertex( n );
                        ++count;
                    }
                    if ( count > 0 )
                    {
                        const glm::dvec3 normal = MeshNormals::ComputeVertexNormal( mesh, v );
                        glm::dvec3       delta  = centroid / static_cast<double>( count ) - p;
                        delta -= normal * glm::dot( delta, normal );
                        p += alpha * delta;
                    }
                }
                double    distSqr = 0.0;
                const int tid     = tree.FindNearestTriangle( p, distSqr );
                if ( tid != DynamicMesh3::InvalidID )
                {
                    Triangle3d tri;
                    original.GetTriVertices( tid, tri.V[0], tri.V[1], tri.V[2] );
                    DistPoint3Triangle3<double> dist( p, tri );
                    dist.GetSquared();
                    p = dist.m_ClosestTrianglePoint;
                }
                next.emplace_back( v, p );
            }
            for ( const auto& [v, p] : next )
                mesh.SetVertex( v, p );
        }
        RecomputeNormals( mesh );
        return true;
    }

    std::vector<int> RemeshRegion( DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                   const glm::dvec3& centreWorld, double radius, const RemeshSettings& settings )
    {
        std::vector<int> touched;
        if ( settings.TargetEdgeLength <= 0.0f || radius <= 0.0 ||
             !( settings.Splits || settings.Collapses || settings.Flips ) )
            return touched;
        const Frame  f{ mesh, localToWorld };
        const double r2 = radius * radius;
        // UE UDynamicMeshSculptTool remeshes the stamp's ROI until it settles; the cap only bounds a stamp that
        // never settles (a flip cycle), it is not a resolution budget: each pass halves the longest edge.
        for ( int pass = 0; pass < std::max( 1, settings.Iterations ); ++pass )
        {
            std::vector<int> edges;
            for ( const int e : mesh.EdgeIndicesItr() )
            {
                const Index2i    ev  = mesh.GetEdgeV( e );
                const glm::dvec3 mid = ( f.World( ev.A ) + f.World( ev.B ) ) * 0.5;
                const glm::dvec3 d   = mid - centreWorld;
                if ( glm::dot( d, d ) <= r2 )
                    edges.push_back( e );
            }
            if ( edges.empty() || TopologyPass( f, settings, edges, touched ) == 0 )
                break;
        }
        std::sort( touched.begin(), touched.end() );
        touched.erase( std::unique( touched.begin(), touched.end() ), touched.end() );
        touched.erase(
             std::remove_if( touched.begin(), touched.end(), [&]( int v ) { return !mesh.IsVertex( v ); } ),
             touched.end() );
        return touched;
    }
} // namespace Desert::Geometry
