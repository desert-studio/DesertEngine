#include "DynamicMeshRemesh.hpp"

#include "Engine/Geometry/MeshCore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <algorithm>
#include <cmath>
#include <format>
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

        // A vertex the remesher must not move or remove: on an open border, or on an attribute seam while sharp
        // edges are preserved (UE ConstrainAllBoundariesAndSeams, bAllowSeamCollapse/Smoothing = !preserve).
        bool IsFixedVertex( const DynamicMesh3& mesh, int v, bool preserveSharp )
        {
            if ( mesh.IsBoundaryVertex( v ) )
                return true;
            return preserveSharp && mesh.HasAttributes() && mesh.Attributes()->IsSeamVertex( v, true );
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

        // UE FRemesher::CollapseEdge's checks: not both ends fixed (a fixed end keeps its place and the free one
        // collapses onto it), no surviving triangle turned over and no new edge longer than the split threshold
        // (or the collapse would be split straight back).
        bool TryCollapse( const Frame& f, int eid, double maxLen, bool preserveSharp, std::vector<int>& touched )
        {
            DynamicMesh3& mesh   = f.Mesh;
            const Index2i edge   = mesh.GetEdgeV( eid );
            const bool    fixedA = IsFixedVertex( mesh, edge.A, preserveSharp );
            const bool    fixedB = IsFixedVertex( mesh, edge.B, preserveSharp );
            if ( fixedA && fixedB )
                return false;
            // ev.A is the vertex that stays.
            const Index2i    ev       = fixedB ? Index2i( edge.B, edge.A ) : edge;
            const glm::dvec3 mid      = ( fixedA || fixedB )
                                             ? mesh.GetVertex( ev.A )
                                             : ( mesh.GetVertex( ev.A ) + mesh.GetVertex( ev.B ) ) * 0.5;
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
            if ( mesh.CollapseEdge( ev.A, ev.B, ( fixedA || fixedB ) ? 0.0 : 0.5, info ) != MeshResult::Ok )
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
            // A pair that is already folded (or degenerate) has no side to keep: normalising n0 + n1 there gives
            // NaN, every comparison with it is false, and the flip went through unchecked (M27 p9: 1..12
            // inward triangles per stroke). UE CheckIfFlipInvertsNormals refuses it; so does this.
            const double pairLen = glm::length( n0 + n1 );
            if ( !( pairLen > 1e-12 ) )
                return false;
            const glm::dvec3           avg = ( n0 + n1 ) / pairLen;
            DynamicMesh3::EdgeFlipInfo info;
            if ( mesh.FlipEdge( eid, info ) != MeshResult::Ok )
                return false;
            // Each new triangle must keep the side of BOTH old ones, not only of their average.
            const auto keepsSide = [&]( int t )
            {
                const glm::dvec3 n = mesh.GetTriNormal( t );
                return glm::dot( n, avg ) >= kMinNormalDot && glm::dot( n, n0 ) > 0.0 && glm::dot( n, n1 ) > 0.0;
            };
            if ( !keepsSide( info.Triangles.A ) || !keepsSide( info.Triangles.B ) )
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
                          std::vector<int>& touched, RemeshCounts& counts )
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
                        ++counts.Splits;
                    }
                }
                else if ( s.Collapses && len < minLen )
                {
                    if ( TryCollapse( f, eid, maxLen, s.PreserveSharpEdges, touched ) )
                    {
                        ++edits;
                        ++counts.Collapses;
                    }
                }
                else if ( s.Flips && TryFlip( mesh, eid, touched ) )
                {
                    ++edits;
                    ++counts.Flips;
                }
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

    namespace
    {
        // UE FRemesher::BasicRemeshPass + FullSmoothPass_Buffer + FullProjectionPass, `settings.Iterations` times.
        RemeshCounts RemeshPasses( DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                   const RemeshSettings& settings, const DynamicMesh3& projectTo )
        {
            DynamicMeshAABBTree3 tree( &projectTo );
            const Frame          f{ mesh, localToWorld };
            const double         alpha = std::clamp( static_cast<double>( settings.SmoothingStrength ), 0.0, 1.0 );
            std::vector<int>     touched;
            RemeshCounts         counts;
            for ( int pass = 0; pass < settings.Iterations; ++pass )
            {
                touched.clear();
                TopologyPass( f, settings, AllEdges( mesh ), touched, counts );

                // UE FullSmoothPass_Buffer: tangential Laplacian (the one-ring centroid with its normal component
                // removed, so smoothing evens the triangles without shrinking the shape), then FullProjectionPass:
                // every vertex onto the projection surface.
                std::vector<std::pair<int, glm::dvec3>> next;
                next.reserve( static_cast<size_t>( mesh.VertexCount() ) );
                for ( const int v : mesh.VertexIndicesItr() )
                {
                    glm::dvec3 p = mesh.GetVertex( v );
                    if ( alpha > 0.0 && !IsFixedVertex( mesh, v, settings.PreserveSharpEdges ) )
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
                    if ( settings.Reproject )
                    {
                        double    distSqr = 0.0;
                        const int tid     = tree.FindNearestTriangle( p, distSqr );
                        if ( tid != DynamicMesh3::InvalidID )
                        {
                            Triangle3d tri;
                            projectTo.GetTriVertices( tid, tri.V[0], tri.V[1], tri.V[2] );
                            DistPoint3Triangle3<double> dist( p, tri );
                            dist.GetSquared();
                            p = dist.m_ClosestTrianglePoint;
                        }
                    }
                    next.emplace_back( v, p );
                }
                for ( const auto& [v, p] : next )
                    mesh.SetVertex( v, p );
            }
            return counts;
        }

        double WorldArea( const DynamicMesh3& mesh, const glm::dmat4& localToWorld )
        {
            double area = 0.0;
            for ( const int tid : mesh.TriangleIndicesItr() )
            {
                glm::dvec3 a{}, b{}, c{};
                mesh.GetTriVertices( tid, a, b, c );
                a = glm::dvec3( localToWorld * glm::dvec4( a, 1.0 ) );
                b = glm::dvec3( localToWorld * glm::dvec4( b, 1.0 ) );
                c = glm::dvec3( localToWorld * glm::dvec4( c, 1.0 ) );
                area += 0.5 * glm::length( glm::cross( b - a, c - a ) );
            }
            return area;
        }
    } // namespace

    Common::ResultStr<DeformOutcome> RemeshMeshOperation( const DynamicMesh3&       input,
                                                          const glm::dmat4&         localToWorld,
                                                          const RemeshMeshSettings& settings,
                                                          const DynamicMesh3* projectTo, const char* toolName )
    {
        if ( settings.TargetTriangleCount < 1 || !( settings.TargetEdgeLength > 0.0f ) ||
             settings.SmoothingStrength < 0.0f || settings.SmoothingStrength > 1.0f ||
             settings.RemeshIterations < 0 || settings.RemeshIterations > 100 )
            return Common::MakeFormattedError<DeformOutcome>(
                 "{}: Target Triangle Count must be >= 1 (is {}), Target Edge Length > 0 (is {}), Smoothing "
                 "Strength in [0, 1] (is {}), Remesh Iterations in [0, 100] (are {})",
                 toolName, settings.TargetTriangleCount, settings.TargetEdgeLength, settings.SmoothingStrength,
                 settings.RemeshIterations );
        if ( input.TriangleCount() == 0 )
            return Common::MakeFormattedError<DeformOutcome>( "{}: the mesh has no triangle", toolName );
        if ( projectTo != nullptr && projectTo->TriangleCount() == 0 )
            return Common::MakeFormattedError<DeformOutcome>( "{}: the target mesh has no triangle", toolName );

        // UE CalculateTargetEdgeLength: the edge length straight, or the equilateral edge that tiles the mesh's
        // area with Target Triangle Count triangles.
        const double area = WorldArea( input, localToWorld );
        const double edgeLength =
             settings.UseTargetEdgeLength
                  ? static_cast<double>( settings.TargetEdgeLength )
                  : std::sqrt( 4.0 * area / ( std::sqrt( 3.0 ) * settings.TargetTriangleCount ) );
        const double coverArea = projectTo ? std::max( area, WorldArea( *projectTo, localToWorld ) ) : area;
        const double expected  = coverArea / ( std::sqrt( 3.0 ) / 4.0 * edgeLength * edgeLength );
        if ( !( edgeLength > 0.0 ) || expected > 1.0e6 )
            return Common::MakeFormattedError<DeformOutcome>(
                 "{}: a Target Edge Length of {:.3f} cm would make about {:.0f} triangles (at most 1000000)",
                 toolName, edgeLength, expected );

        DynamicMesh3 mesh = input;
        if ( settings.DiscardAttributes )
            mesh.DiscardAttributes();
        // UE URemeshMeshTool: the projection target is a copy of the input (the surface the remesh must keep).
        const DynamicMesh3   original = projectTo ? DynamicMesh3() : input;
        const RemeshSettings pass{ .TargetEdgeLength   = static_cast<float>( edgeLength ),
                                   .Splits             = settings.Splits,
                                   .Collapses          = settings.Collapses,
                                   .Flips              = settings.Flips,
                                   .SmoothingStrength  = settings.SmoothingStrength,
                                   .Iterations         = settings.RemeshIterations,
                                   .Reproject          = settings.Reproject,
                                   .PreserveSharpEdges = settings.PreserveSharpEdges };
        const RemeshCounts   counts = RemeshPasses( mesh, localToWorld, pass, projectTo ? *projectTo : original );

        // The overlays follow the new surface: per-vertex normals for a mesh without attributes (UE: the discarded
        // attributes come back as a normal overlay only), recomputed normals otherwise.
        if ( !mesh.HasAttributes() )
        {
            mesh.EnableAttributes();
            mesh.Attributes()->SetNumUVLayers( 0 );
            MeshNormals::InitializeOverlayToPerVertexNormals( mesh.Attributes()->PrimaryNormals(), false );
        }
        else
            RecomputeNormals( mesh );

        DeformOutcome outcome;
        outcome.MovedVertices = mesh.VertexCount();
        outcome.Report        = std::format( "{}: {} triangles, edge length {:.2f} cm ({} splits, {} collapses, "
                                                    "{} flips over {} passes)",
                                             toolName, mesh.TriangleCount(), edgeLength, counts.Splits, counts.Collapses,
                                             counts.Flips, settings.RemeshIterations );
        outcome.Mesh          = std::move( mesh );
        return Common::MakeSuccess( std::move( outcome ) );
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
            RemeshCounts counts;
            if ( edges.empty() || TopologyPass( f, settings, edges, touched, counts ) == 0 )
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
