// VoxWrap: UE 5.8 FVoxelSolidifyMeshesOp::CalculateResult (VoxelSolidifyMeshesOp.cpp:60-103) and
// TImplicitSolidify::Generate (Solidify.h:85-153), adapted - see MeshVoxelOperations.hpp.
#include "MeshVoxelOperations.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/Implicit/MarchingCubes.hpp"
#include "Engine/Geometry/MeshCore/Spatial/FastWinding.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        // Geometry only: VoxWrap reads positions and triangles (UE copies into CombinedMesh the same way).
        DynamicMesh3 GeometryOf( const DynamicMesh3& mesh, const std::vector<int>* triangles )
        {
            DynamicMesh3     out;
            std::vector<int> map( static_cast<size_t>( mesh.MaxVertexID() ), -1 );
            const auto       add = [&]( int tid )
            {
                const Index3i t = mesh.GetTriangle( tid );
                int           v[3];
                for ( int k = 0; k < 3; ++k )
                {
                    int& m = map[static_cast<size_t>( t[k] )];
                    if ( m < 0 )
                        m = out.AppendVertex( mesh.GetVertex( t[k] ) );
                    v[k] = m;
                }
                out.AppendTriangle( Index3i( v[0], v[1], v[2] ) );
            };
            if ( triangles )
                for ( int tid : *triangles )
                    add( tid );
            else
                for ( int tid : mesh.TriangleIndicesItr() )
                    add( tid );
            return out;
        }

        // The triangles of every connected component that has an open edge (UE FExtrudeMesh bSkipClosedComponents).
        std::vector<int> OpenShellTriangles( const DynamicMesh3& mesh )
        {
            std::vector<int> component( static_cast<size_t>( mesh.MaxTriangleID() ), -1 ), open, result, stack;
            for ( int seed : mesh.TriangleIndicesItr() )
            {
                if ( component[static_cast<size_t>( seed )] >= 0 )
                    continue;
                const int id      = static_cast<int>( open.size() );
                bool      hasOpen = false;
                stack.assign( 1, seed );
                component[static_cast<size_t>( seed )] = id;
                while ( !stack.empty() )
                {
                    const int     tid = stack.back();
                    const Index3i e   = mesh.GetTriEdges( tid );
                    stack.pop_back();
                    for ( int k = 0; k < 3; ++k )
                    {
                        if ( mesh.IsBoundaryEdge( e[k] ) )
                            hasOpen = true;
                        const Index2i et    = mesh.GetEdgeT( e[k] );
                        const int     other = et.A == tid ? et.B : et.A;
                        if ( other >= 0 && component[static_cast<size_t>( other )] < 0 )
                        {
                            component[static_cast<size_t>( other )] = id;
                            stack.push_back( other );
                        }
                    }
                }
                open.push_back( hasOpen ? 1 : 0 );
            }
            for ( int tid : mesh.TriangleIndicesItr() )
                if ( open[static_cast<size_t>( component[static_cast<size_t>( tid )] )] )
                    result.push_back( tid );
            return result;
        }
    } // namespace

    Common::ResultStr<DeformOutcome> VoxWrap( const DynamicMesh3& mesh, const VoxWrapSettings& s,
                                              const ProgressCancel* progress )
    {
        const auto cancelled = [progress] { return progress != nullptr && progress->Cancelled(); };
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "the mesh has no triangles" );
        if ( s.VoxelCount < 8 || s.VoxelCount > 1024 )
            return Common::MakeError<DeformOutcome>( std::format( "Voxel Count {} is outside 8..1024", s.VoxelCount ) );
        if ( s.SurfaceSearchSteps < 0 || s.SurfaceSearchSteps > 10 )
            return Common::MakeError<DeformOutcome>( std::format( "Surface Search Steps {} is outside 0..10", s.SurfaceSearchSteps ) );
        if ( s.ShellThickness < 0.0f || s.ExtendBounds < 0.0f )
            return Common::MakeError<DeformOutcome>( "Shell Thickness and Extend Bounds cannot be negative" );

        DynamicMesh3         combined = GeometryOf( mesh, nullptr );
        DynamicMeshAABBTree3 spatial( &combined );
        FastWindingTree      winding( &spatial );
        if ( cancelled() )
            return Common::MakeError<DeformOutcome>( kVoxWrapCancelled );
        const AxisAlignedBox3d bounds   = combined.GetBounds();
        const glm::dvec3       size     = bounds.Max - bounds.Min;
        const double           maxDim   = std::max( { size.x, size.y, size.z } );
        const double           extend   = s.ExtendBounds;
        // UE TImplicitSolidify::SetCellSizeAndExtendBounds.
        const double cell = ( maxDim + extend * 2.0 ) / double( s.VoxelCount );
        if ( !( cell > 0.0 ) || !std::isfinite( cell ) )
            return Common::MakeError<DeformOutcome>( "the mesh has no extent" );

        // UE bApplyThickenShells extrudes each open shell inward by max( 2 cells, ThickenShells ) before the
        // winding number is taken; here the same slab - behind the open shell's surface, nearer than that
        // thickness - is added to the solid as a field term, which gives the slab without building it.
        const std::vector<int>               openTris = s.ThickenShells ? OpenShellTriangles( combined )
                                                                        : std::vector<int>{};
        DynamicMesh3                         shells   = GeometryOf( combined, &openTris );
        std::unique_ptr<DynamicMeshAABBTree3> shellTree;
        if ( shells.TriangleCount() > 0 )
            shellTree = std::make_unique<DynamicMeshAABBTree3>( &shells );
        const double thickness = std::max( cell * 2.0, double( s.ShellThickness ) );
        const double threshold = s.WindingThreshold;

        AxisAlignedBox3d useBounds = bounds;
        useBounds.Min -= glm::dvec3( extend );
        useBounds.Max += glm::dvec3( extend );
        MarchingCubes mc;
        mc.CubeSize      = cell;
        mc.Bounds        = useBounds;
        if ( s.SolidAtBoundaries ) // UE: the swept region reaches a tenth of a cell past the solid region
        {
            mc.Bounds.Min -= glm::dvec3( cell * 0.1 );
            mc.Bounds.Max += glm::dvec3( cell * 0.1 );
        }
        mc.RootMode      = MarchingCubesRootMode::Bisection;
        mc.RootModeSteps = s.SurfaceSearchSteps;
        mc.IsoValue      = threshold;
        mc.Cancel        = cancelled;
        mc.Implicit      = [&]( const glm::dvec3& p )
        {
            const bool inBounds = p.x >= useBounds.Min.x && p.y >= useBounds.Min.y && p.z >= useBounds.Min.z &&
                                  p.x <= useBounds.Max.x && p.y <= useBounds.Max.y && p.z <= useBounds.Max.z;
            if ( s.SolidAtBoundaries && !inBounds )
                return -( threshold + 1.0 );
            if ( shellTree )
            {
                double    d2  = 0.0;
                const int tid = shellTree->FindNearestTriangle( p, d2 );
                if ( tid >= 0 && d2 < thickness * thickness )
                {
                    const Index3i    t  = shells.GetTriangle( tid );
                    const glm::dvec3 a  = shells.GetVertex( t[0] );
                    const glm::dvec3 n  = glm::cross( shells.GetVertex( t[1] ) - a, shells.GetVertex( t[2] ) - a );
                    if ( glm::dot( p - a, n ) <= 0.0 )
                        return threshold + 1.0;
                }
            }
            return winding.FastWindingNumber( p );
        };
        DynamicMesh3 result = mc.Generate();
        if ( mc.Cancelled || cancelled() )
            return Common::MakeError<DeformOutcome>( kVoxWrapCancelled );
        if ( result.TriangleCount() == 0 )
            return Common::MakeError<DeformOutcome>( "the solid is empty at this Winding Threshold" );

        // Attributes: per-vertex normals (UE PostProcessResult recomputes them) and box-projected UVs, 1 UV unit
        // per metre, so the result draws with the entity's material.
        result.EnableAttributes();
        DynamicMeshAttributeSet* attributes = result.Attributes();
        MeshNormals::InitializeOverlayToPerVertexNormals( attributes->PrimaryNormals(), false );
        DynamicMeshUVOverlay* uvs = attributes->PrimaryUV();
        std::vector<int>      uvOf( static_cast<size_t>( result.MaxVertexID() ), -1 );
        for ( int vid : result.VertexIndicesItr() )
        {
            const glm::dvec3 n = glm::abs( MeshNormals::ComputeVertexNormal( result, vid ) );
            const glm::dvec3 p = result.GetVertex( vid ) * 0.01;
            const glm::vec2  uv = n.x >= n.y && n.x >= n.z ? glm::vec2( p.y, p.z )
                                  : n.y >= n.z             ? glm::vec2( p.x, p.z )
                                                           : glm::vec2( p.x, p.y );
            uvOf[static_cast<size_t>( vid )] = uvs->AppendElement( &uv.x );
        }
        for ( int tid : result.TriangleIndicesItr() )
        {
            const Index3i t = result.GetTriangle( tid );
            uvs->SetTriangle( tid, Index3i( uvOf[size_t( t[0] )], uvOf[size_t( t[1] )], uvOf[size_t( t[2] )] ) );
        }

        DeformOutcome out;
        out.MovedVertices = result.VertexCount();
        out.Report        = std::format( "VoxWrap: {}^3 cells of {:.2f} cm -> {} vertices, {} triangles{}", s.VoxelCount,
                                         cell, result.VertexCount(), result.TriangleCount(),
                                         mc.RejectedTriangles > 0
                                             ? std::format( " ({} non-manifold triangles dropped)", mc.RejectedTriangles )
                                             : std::string() );
        out.Mesh = std::move( result );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
