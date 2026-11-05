#include "MeshBooleanOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/SimpleHoleFiller.hpp"
#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/MeshCore/MeshBoundaryLoops.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <unordered_set>

namespace Desert::Geometry
{
    const char* ToString( BooleanOperation operation )
    {
        switch ( operation )
        {
            case BooleanOperation::Union:
                return "Union";
            case BooleanOperation::Difference:
                return "Difference";
            case BooleanOperation::Intersect:
                return "Intersect";
            case BooleanOperation::TrimInside:
                return "Trim Inside";
            case BooleanOperation::TrimOutside:
                return "Trim Outside";
            case BooleanOperation::NewGroupInside:
                return "New Group Inside";
            case BooleanOperation::NewGroupOutside:
                return "New Group Outside";
        }
        return "Unknown";
    }

    namespace
    {
        std::ptrdiff_t CountOpenEdges( const DynamicMesh3& mesh )
        {
            std::ptrdiff_t count = 0;
            for ( [[maybe_unused]] int const edge : mesh.BoundaryEdgeIndicesItr() )
                ++count;
            return count;
        }

        std::unordered_set<int32_t> OpenOf( const DynamicMesh3& mesh, const std::vector<int>& edges )
        {
            std::unordered_set<int32_t> open;
            for ( const int edge : edges )
                if ( mesh.IsEdge( edge ) && mesh.IsBoundaryEdge( edge ) )
                    open.insert( edge );
            return open;
        }

        // UE FBooleanMeshesOp::CalculateResult (BooleanMeshesOp.cpp, bAttemptFixHoles) after FMeshBoolean: the
        // seam edges MergeEdges left open are closed in two steps, both restricted to those edges.
        //  1. FMergeCoincidentMeshEdges at the distance a seam vertex can have drifted: each mesh's matching pass
        //     moves an endpoint by up to SnapTolerance, so the two halves of one seam segment can lie
        //     2 * SnapTolerance apart (in the boolean's unit cube; the caller scales it back to cm).
        //  2. the loops the remaining open seam edges form (a sliver where cut segments met at a corner or edge
        //     of the other mesh) are filled - one triangle for three vertices, a fan otherwise - and the fill
        //     takes its neighbour's polygroup and material, flat normals and a planar UV projection, as the hole
        //     fill tools set them. A seam that does not close into loops stays open and the caller refuses it.
        // Returns the number of fill triangles.
        Common::ResultStr<int> CloseSeamCracks( DynamicMesh3& mesh, const std::vector<int>& seamEdges,
                                                double mergeTolerance, const char* name )
        {
            std::unordered_set<int32_t> open = OpenOf( mesh, seamEdges );
            if ( open.empty() )
                return Common::MakeSuccess( 0 );
            MergeCoincidentMeshEdges merge( &mesh );
            merge.m_MergeVertexTolerance = mergeTolerance;
            merge.m_EdgesToMerge         = &open;
            merge.Apply();

            const std::unordered_set<int32_t> remaining = OpenOf( mesh, seamEdges );
            if ( remaining.empty() )
                return Common::MakeSuccess( 0 );
            if ( mesh.HasAttributes() && mesh.Attributes()->HasPrimaryColors() )
                return Common::MakeFormattedError<int>(
                     "Mesh {}: {} seam edges are left open and the mesh has a primary colour layer, whose fill "
                     "(UE HoleFillUtil::FillColorOverlay) is not ported",
                     name, remaining.size() );

            MeshBoundaryLoops loops( &mesh, false );
            loops.EdgeFilterFunc = [&remaining]( int edge ) { return remaining.count( edge ) != 0; };
            loops.Compute();

            const glm::dvec3 extents = mesh.GetBounds().Extents();
            const double     uvScale = 1.0 / ( 2.0 * std::max( { extents.x, extents.y, extents.z, 1e-9 } ) );
            int              filled  = 0;
            for ( const EdgeLoop& loop : loops.m_Loops )
            {
                const int        neighbour = mesh.GetEdgeT( loop.Edges.front() ).A;
                const int        group     = mesh.HasTriangleGroups() ? mesh.GetTriangleGroup( neighbour ) : 0;
                SimpleHoleFiller filler( &mesh, loop );
                if ( !filler.Fill( group ) )
                    return Common::MakeFormattedError<int>( "Mesh {}: a {}-edge crack of the cut seam: {}", name,
                                                            loop.Edges.size(), filler.m_FailureReason );
                const std::vector<int>& tris = filler.m_NewTriangles;
                filled += static_cast<int>( tris.size() );
                if ( !mesh.HasAttributes() )
                    continue;
                DynamicMeshAttributeSet* attributes = mesh.Attributes();
                if ( DynamicMeshMaterialAttribute* material = attributes->GetMaterialID() )
                {
                    int32_t id = 0;
                    material->GetValue( neighbour, &id );
                    for ( const int tri : tris )
                        material->SetValue( tri, &id );
                }
                // Newell's plane of the loop: the projection axis of the fill's UVs
                glm::dvec3   normal( 0.0 ), origin( 0.0 );
                const size_t count = loop.Vertices.size();
                for ( size_t i = count - 1, j = 0; j < count; i = j++ )
                {
                    const glm::dvec3 pi = mesh.GetVertex( loop.Vertices[i] );
                    const glm::dvec3 pj = mesh.GetVertex( loop.Vertices[j] );
                    origin += pj;
                    normal.x += ( pj.y - pi.y ) * ( pi.z + pj.z );
                    normal.y += ( pj.z - pi.z ) * ( pi.x + pj.x );
                    normal.z += ( pj.x - pi.x ) * ( pi.y + pj.y );
                }
                origin /= static_cast<double>( count );
                const double length = glm::length( normal );
                normal              = length > 1e-12 ? normal / length : glm::dvec3( 0.0, 0.0, 1.0 );
                const DynamicMeshEditor editor( &mesh );
                editor.SetTriangleNormals( tris );
                if ( attributes->PrimaryUV() != nullptr )
                    editor.SetTriangleUVsFromProjection( tris, origin, -normal, static_cast<float>( uvScale ) );
            }
            if ( filled > 0 )
                if ( auto tangents = RecomputeTangentSpace( mesh, name ); !tangents.IsSuccess() )
                    return Common::MakeError<int>( tangents.GetError() );
            return Common::MakeSuccess( filled );
        }
    } // namespace

    Common::ResultStr<BooleanOutcome> RunMeshBoolean( BooleanOperation operation, const DynamicMesh3& target,
                                                      const DynamicMesh3& cutter )
    {
        const char* name = ToString( operation );
        if ( target.TriangleCount() == 0 )
            return Common::MakeFormattedError<BooleanOutcome>( "Mesh {}: the mesh has no triangles", name );
        if ( cutter.TriangleCount() == 0 )
            return Common::MakeFormattedError<BooleanOutcome>( "Mesh {}: the cutter has no triangles", name );
        // The winding number of an open surface is fractional near it, so "inside" is not a property of the
        // cutter any more; the target is classified against the cutter only in the two-mesh operations.
        if ( const std::ptrdiff_t open = CountOpenEdges( cutter ); open != 0 )
            return Common::MakeFormattedError<BooleanOutcome>(
                 "Mesh {}: the cutter has {} open edges - inside and outside of an open mesh are undefined", name,
                 open );
        const bool bSingleMesh = MeshBoolean::OperatesOnSingleMesh( operation );
        if ( !bSingleMesh )
        {
            if ( const std::ptrdiff_t open = CountOpenEdges( target ); open != 0 )
                return Common::MakeFormattedError<BooleanOutcome>(
                     "Mesh {}: the mesh has {} open edges - a boolean of two meshes needs both closed (Trim "
                     "takes an open mesh)",
                     name, open );
        }

        auto        mesh = std::make_shared<DynamicMesh3>();
        MeshBoolean boolean( &target, &cutter, mesh.get(), operation );
        boolean.Compute();
        if ( !bSingleMesh )
        {
            // both inputs are closed, so every open edge of the result is a seam crack MergeEdges left
            AxisAlignedBox3d bounds = target.GetBounds();
            bounds.Contain( cutter.GetBounds() );
            const glm::dvec3 span = bounds.Max - bounds.Min;
            // MeshBoolean's unit-cube scale (MeshBoolean.cpp ComputeWithCustomInside)
            const double size = std::clamp( std::max( { span.x, span.y, span.z } ), 0.01, 1000000.0 );
            const auto   closed =
                 CloseSeamCracks( *mesh, boolean.CreatedBoundaryEdges, 2.0 * boolean.SnapTolerance * size, name );
            if ( !closed.IsSuccess() )
                return Common::MakeError<BooleanOutcome>( closed.GetError() );
            if ( const std::ptrdiff_t open = CountOpenEdges( *mesh ); open != 0 )
                return Common::MakeFormattedError<BooleanOutcome>(
                     "Mesh {}: {} edges of the cut seam could not be welded shut ({} were left by the weld)",
                     name, open, boolean.CreatedBoundaryEdges.size() );
        }
        if ( mesh->TriangleCount() == 0 )
            return Common::MakeFormattedError<BooleanOutcome>( "Mesh {}: the result has no triangles left", name );
        // a cut adds vertices even where nothing is removed, so the unchanged case is "no cut at all": the same
        // vertex and triangle counts as the input
        const bool bTrim = operation == BooleanOperation::TrimInside || operation == BooleanOperation::TrimOutside;
        if ( bTrim && mesh->TriangleCount() == target.TriangleCount() &&
             mesh->VertexCount() == target.VertexCount() )
            return Common::MakeFormattedError<BooleanOutcome>(
                 "Mesh {}: the cutter does not cross the mesh ({} triangles unchanged)", name,
                 target.TriangleCount() );
        return Common::MakeSuccess( BooleanOutcome{ std::move( mesh ) } );
    }
} // namespace Desert::Geometry
