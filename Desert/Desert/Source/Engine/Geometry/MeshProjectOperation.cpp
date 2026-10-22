// UE 5.8 MeshModelingTools ProjectToTargetTool.cpp + RemeshMeshOp.cpp + DynamicMesh/Remesher.cpp (FRemesher::
// BasicRemeshPass / ProcessEdge, FMeshRefinerBase) on our DynamicMesh3: every pass walks the edges collapsing the
// ones shorter than 0.66x the target edge length, flipping the ones whose flip moves the four vertices' valences
// towards 6 (4 on a boundary), splitting the ones longer than 1.33x; then smooths every free vertex a Smoothing
// Strength step towards its one-ring centroid; then projects every vertex onto the target's nearest surface point.
// The projection target is the second mesh (UE UProjectToTargetTool's ProjectionTarget), brought into the source's
// object space. A collapse or flip that would turn a neighbouring triangle over is refused (UE
// CheckIfCollapseCreatesFlipOrInvalid / CheckIfFlipInvertsNormals). Boundary and attribute-seam edges stay (UE's
// seam constraints, no "allow seam collapse"): split only, their vertices projected but not smoothed.
#include "MeshProjectOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        double MeshArea( const DynamicMesh3& mesh )
        {
            double area = 0.0;
            for ( const int tid : mesh.TriangleIndicesItr() )
            {
                glm::dvec3 a{}, b{}, c{};
                mesh.GetTriVertices( tid, a, b, c );
                area += 0.5 * glm::length( glm::cross( b - a, c - a ) );
            }
            return area;
        }

        glm::dvec3 Normal( const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c )
        {
            return glm::cross( b - a, c - a );
        }
    } // namespace

    Common::ResultStr<DeformOutcome> ProjectToTarget( const DynamicMesh3& source, const DynamicMesh3& target,
                                                      const glm::dmat4&      targetToSource,
                                                      const ProjectSettings& settings )
    {
        if ( settings.TargetTriangleCount < 1 || !( settings.TargetEdgeLength > 0.0f ) ||
             settings.SmoothingStrength < 0.0f || settings.SmoothingStrength > 1.0f ||
             settings.RemeshIterations < 0 || settings.RemeshIterations > 100 )
            return Common::MakeFormattedError<DeformOutcome>(
                 "Project: Target Triangle Count must be >= 1 (is {}), Target Edge Length > 0 (is {}), Smoothing "
                 "Strength in [0, 1] (is {}), Remesh Iterations in [0, 100] (are {})",
                 settings.TargetTriangleCount, settings.TargetEdgeLength, settings.SmoothingStrength,
                 settings.RemeshIterations );
        if ( source.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "Project: the source mesh has no triangle" );
        if ( target.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "Project: the target mesh has no triangle" );

        // UE: the target is copied into the tool mesh's frame and an AABB tree built over it.
        DynamicMesh3 projectTo = target;
        for ( const int vid : projectTo.VertexIndicesItr() )
            projectTo.SetVertex( vid, glm::dvec3( targetToSource * glm::dvec4( projectTo.GetVertex( vid ), 1.0 ) ),
                                 false );
        DynamicMeshAABBTree3 tree( &projectTo );

        // UE URemeshMeshTool: the edge length straight, or the equilateral edge that tiles the source's area with
        // Target Triangle Count triangles (CalculateTargetEdgeLength).
        const double sourceArea = MeshArea( source );
        const double edgeLength =
             settings.UseTargetEdgeLength
                  ? static_cast<double>( settings.TargetEdgeLength )
                  : std::sqrt( 4.0 * sourceArea / ( std::sqrt( 3.0 ) * settings.TargetTriangleCount ) );
        const double expected =
             std::max( sourceArea, MeshArea( projectTo ) ) / ( std::sqrt( 3.0 ) / 4.0 * edgeLength * edgeLength );
        if ( !( edgeLength > 0.0 ) || expected > 1.0e6 )
            return Common::MakeFormattedError<DeformOutcome>(
                 "Project: a Target Edge Length of {:.3f} cm would make about {:.0f} triangles (at most 1000000)",
                 edgeLength, expected );
        const double minLength = 0.66 * edgeLength;
        const double maxLength = 1.33 * edgeLength;

        DynamicMesh3 mesh = source;
        if ( settings.DiscardAttributes )
            mesh.DiscardAttributes();

        const auto seamEdge = [&]( int eid )
        { return mesh.IsBoundaryEdge( eid ) || ( mesh.HasAttributes() && mesh.Attributes()->IsSeamEdge( eid ) ); };
        const auto fixedVertex = [&]( int vid )
        {
            return mesh.IsBoundaryVertex( vid ) ||
                   ( mesh.HasAttributes() && mesh.Attributes()->IsSeamVertex( vid, true ) );
        };
        // Moving vid to newPos turns one of its triangles (other than skip) over, or flattens it.
        const auto turnsOver = [&]( int vid, const glm::dvec3& newPos, Index2i skip )
        {
            for ( const int tid : mesh.VtxTrianglesItr( vid ) )
            {
                if ( tid == skip.A || tid == skip.B )
                    continue;
                const Index3i    tri = mesh.GetTriangle( tid );
                glm::dvec3       p[3]{ mesh.GetVertex( tri.A ), mesh.GetVertex( tri.B ), mesh.GetVertex( tri.C ) };
                const glm::dvec3 before = Normal( p[0], p[1], p[2] );
                for ( int k = 0; k < 3; ++k )
                    if ( tri[k] == vid )
                        p[k] = newPos;
                const glm::dvec3 after = Normal( p[0], p[1], p[2] );
                if ( glm::dot( before, after ) <= 0.0 || glm::length( after ) < 1e-12 )
                    return true;
            }
            return false;
        };
        const auto valenceError = [&]( int vid )
        { return mesh.GetVtxEdgeCount( vid ) - ( mesh.IsBoundaryVertex( vid ) ? 4 : 6 ); };

        int32_t splits = 0, collapses = 0, flips = 0;
        for ( int pass = 0; pass < settings.RemeshIterations; ++pass )
        {
            const int edgeCount = mesh.MaxEdgeID();
            for ( int eid = 0; eid < edgeCount; ++eid )
            {
                if ( !mesh.IsEdge( eid ) )
                    continue;
                const Index2i    ev     = mesh.GetEdgeV( eid );
                const Index2i    et     = mesh.GetEdgeT( eid );
                const glm::dvec3 a      = mesh.GetVertex( ev.A );
                const glm::dvec3 b      = mesh.GetVertex( ev.B );
                const double     length = glm::length( b - a );
                const bool       seam   = seamEdge( eid );

                // UE ProcessEdge: collapse a short edge - to the midpoint, or onto its fixed end.
                if ( settings.Collapses && !seam && length < minLength )
                {
                    const bool fixedA = fixedVertex( ev.A ), fixedB = fixedVertex( ev.B );
                    if ( !( fixedA && fixedB ) )
                    {
                        const int        keep   = fixedB ? ev.B : ev.A;
                        const int        remove = keep == ev.A ? ev.B : ev.A;
                        const glm::dvec3 newPos = ( fixedA || fixedB ) ? mesh.GetVertex( keep ) : 0.5 * ( a + b );
                        if ( !turnsOver( keep, newPos, et ) && !turnsOver( remove, newPos, et ) )
                        {
                            DynamicMeshInfo::EdgeCollapseInfo info;
                            if ( mesh.CollapseEdge( keep, remove, 0.0, info ) == MeshResult::Ok )
                            {
                                mesh.SetVertex( keep, newPos );
                                ++collapses;
                                continue;
                            }
                        }
                    }
                }

                // UE ProcessEdge: flip when it brings the four valences closer to their targets.
                if ( settings.Flips && !seam && et.B != DynamicMesh3::InvalidID )
                {
                    const Index2i ov = mesh.GetEdgeOpposingV( eid );
                    const int     da = valenceError( ev.A ), db = valenceError( ev.B );
                    const int     dc = valenceError( ov.A ), dd = valenceError( ov.B );
                    const int     before = da * da + db * db + dc * dc + dd * dd;
                    const int after = ( da - 1 ) * ( da - 1 ) + ( db - 1 ) * ( db - 1 ) + ( dc + 1 ) * ( dc + 1 ) +
                                      ( dd + 1 ) * ( dd + 1 );
                    if ( after < before )
                    {
                        const glm::dvec3 n0 = mesh.GetTriNormal( et.A );
                        const glm::dvec3 n1 = mesh.GetTriNormal( et.B );
                        DynamicMeshInfo::EdgeFlipInfo     info;
                        if ( mesh.FlipEdge( eid, info ) == MeshResult::Ok )
                        {
                            // UE CheckIfFlipInvertsNormals: the new pair must face the way the old pair did.
                            const glm::dvec3 m0  = mesh.GetTriNormal( info.Triangles.A );
                            const glm::dvec3 m1  = mesh.GetTriNormal( info.Triangles.B );
                            const glm::dvec3 avg = n0 + n1;
                            if ( glm::dot( m0, avg ) <= 0.0 || glm::dot( m1, avg ) <= 0.0 )
                            {
                                DynamicMeshInfo::EdgeFlipInfo back;
                                (void)mesh.FlipEdge( eid, back );
                            }
                            else
                            {
                                ++flips;
                                continue;
                            }
                        }
                    }
                }

                // UE ProcessEdge: split a long edge at its midpoint.
                if ( settings.Splits && length > maxLength )
                {
                    DynamicMeshInfo::EdgeSplitInfo info;
                    if ( mesh.SplitEdge( eid, info, 0.5 ) == MeshResult::Ok )
                        ++splits;
                }
            }

            // UE FullSmoothPass (uniform): every free vertex a Smoothing Strength step towards its one-ring
            // centroid.
            if ( settings.SmoothingStrength > 0.0f )
            {
                const double            alpha = settings.SmoothingStrength;
                std::vector<glm::dvec3> moved( mesh.MaxVertexID() );
                std::vector<char>       move( mesh.MaxVertexID(), 0 );
                for ( const int vid : mesh.VertexIndicesItr() )
                {
                    if ( fixedVertex( vid ) )
                        continue;
                    glm::dvec3 centroid( 0.0 );
                    int        n = 0;
                    for ( const int nb : mesh.VtxVerticesItr( vid ) )
                    {
                        centroid += mesh.GetVertex( nb );
                        ++n;
                    }
                    if ( n == 0 )
                        continue;
                    moved[vid] = glm::mix( mesh.GetVertex( vid ), centroid / static_cast<double>( n ), alpha );
                    move[vid]  = 1;
                }
                for ( const int vid : mesh.VertexIndicesItr() )
                    if ( move[vid] )
                        mesh.SetVertex( vid, moved[vid] );
            }

            // UE FullProjectionPass: every vertex onto the target's nearest surface point.
            if ( settings.Projection )
                for ( const int vid : mesh.VertexIndicesItr() )
                {
                    const glm::dvec3 p      = mesh.GetVertex( vid );
                    double           distSq = 0.0;
                    const int        tid    = tree.FindNearestTriangle( p, distSq );
                    if ( tid < 0 )
                        continue;
                    glm::dvec3 a{}, b{}, c{};
                    projectTo.GetTriVertices( tid, a, b, c );
                    DistPoint3Triangle3<double> dist( p, Triangle3d( a, b, c ) );
                    (void)dist.GetSquared();
                    mesh.SetVertex( vid, dist.m_ClosestTrianglePoint );
                }
        }

        // The overlays follow the new surface: per-vertex normals for a mesh without attributes (UE: the discarded
        // attributes come back as a normal overlay only), recomputed normals otherwise.
        if ( !mesh.HasAttributes() )
        {
            mesh.EnableAttributes();
            mesh.Attributes()->SetNumUVLayers( 0 );
            MeshNormals::InitializeOverlayToPerVertexNormals( mesh.Attributes()->PrimaryNormals(), false );
        }
        else
            MeshNormals::QuickRecomputeOverlayNormals( mesh );

        DeformOutcome outcome;
        outcome.MovedVertices = mesh.VertexCount();
        outcome.Report =
             std::format( "Project: {} triangles, edge length {:.2f} cm ({} splits, {} collapses, "
                          "{} flips over {} passes)",
                          mesh.TriangleCount(), edgeLength, splits, collapses, flips, settings.RemeshIterations );
        outcome.Mesh = std::move( mesh );
        return Common::MakeSuccess( std::move( outcome ) );
    }
} // namespace Desert::Geometry
