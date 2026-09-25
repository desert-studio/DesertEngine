#include "MeshRegionOperation.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshTangents.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/GroupEdgeInserter.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/InsetMeshRegion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/OffsetMeshRegion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/SimpleHoleFiller.hpp"
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/UECore/MeshBoundaryLoops.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::Geometry
{
    const char* ToString( RegionOperation operation )
    {
        switch ( operation )
        {
            case RegionOperation::Extrude:
                return "Extrude";
            case RegionOperation::PushPull:
                return "Push/Pull";
            case RegionOperation::Inset:
                return "Inset";
            case RegionOperation::Outset:
                return "Outset";
        }
        return "Unknown";
    }

    namespace
    {
        // UE rebuilds tangents after every edit (they are derived data); the per-triangle path is exact on the
        // flat faces the region operations create.
        Common::BoolResultStr RecomputeTangents( FDynamicMesh3& mesh, const char* name )
        {
            FDynamicMeshAttributeSet* attributes = mesh.Attributes();
            if ( attributes == nullptr || !attributes->HasTangentSpace() )
                return Common::MakeSuccess( true );
            const FDynamicMeshUVOverlay* uvs = attributes->PrimaryUV();
            if ( uvs == nullptr )
                return Common::MakeFormattedError<bool>(
                     "Mesh {}: the mesh carries tangents but has no UV layer 0 to derive them from", name );
            FMeshTangentsd tangents( &mesh );
            tangents.ComputeSeparatePerTriangleTangents( attributes->PrimaryNormals(), uvs );
            if ( !tangents.CopyToOverlays( mesh ) )
                return Common::MakeFormattedError<bool>(
                     "Mesh {}: tangents could not be written - {} normal layers, 3 expected", name,
                     attributes->NumNormalLayers() );
            return Common::MakeSuccess( true );
        }

        // Ported from UE 5.8 GeometryCore/Private/CompGeom/PolygonTriangulation.cpp:170-192
        // (ComputePolygonPlane, Newell's method), adapted: the loop's vertex IDs in, no area returned.
        void ComputeLoopPlane( const FDynamicMesh3& mesh, const TArray<int>& loopVertices, FVector3d& normal,
                               FVector3d& origin )
        {
            normal          = FVector3d( 0, 0, 0 );
            origin          = FVector3d( 0, 0, 0 );
            const int count = loopVertices.Num();
            for ( int i = count - 1, j = 0; j < count; i = j++ )
            {
                const FVector3d pi = mesh.GetVertex( loopVertices[i] );
                const FVector3d pj = mesh.GetVertex( loopVertices[j] );
                origin += pj;
                normal.X += ( pj.Y - pi.Y ) * ( pi.Z + pj.Z );
                normal.Y += ( pj.Z - pi.Z ) * ( pi.X + pj.X );
                normal.Z += ( pj.X - pi.X ) * ( pi.Y + pj.Y );
            }
            origin /= static_cast<double>( count );
            Normalize( normal );
        }

        // UE HoleFillOp.cpp:19-40 (LoopIsValid); FEdgeLoop::IsBoundaryLoop stands for CheckValidity.
        bool LoopIsValid( const FDynamicMesh3& mesh, const FEdgeLoop& loop )
        {
            if ( loop.Edges.Num() == 0 )
                return false;
            for ( const int e : loop.Edges )
                if ( !mesh.IsBoundaryEdge( e ) )
                    return false;
            return loop.IsBoundaryLoop( mesh );
        }
    } // namespace

    Common::ResultStr<RegionOutcome> FillHoles( const FDynamicMesh3& before, const ElementSelection& selection )
    {
        auto                     mesh = std::make_shared<FDynamicMesh3>( before );
        const FMeshBoundaryLoops boundary( mesh.get() );
        if ( boundary.GetLoopCount() == 0 )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Fill Hole: the mesh has no open loop ({} open spans)", boundary.Spans.Num() );
        TArray<int> loopIndices;
        if ( selection.Mode() == ElementMode::Edge && !selection.Empty() )
        {
            for ( const int e : selection.Ids() )
            {
                const int loop = boundary.FindLoopContainingEdge( e );
                if ( loop < 0 )
                    return Common::MakeFormattedError<RegionOutcome>(
                         "Mesh Fill Hole: selected edge {} lies on none of the {} open loops", e,
                         boundary.GetLoopCount() );
                if ( !loopIndices.Contains( loop ) )
                    loopIndices.Add( loop );
            }
        }
        else
            for ( int i = 0; i < boundary.GetLoopCount(); ++i )
                loopIndices.Add( i );

        // UE's MaxDim: the largest side of the bounds (Extents are half sides).
        const FVector3d extents = mesh->GetBounds().Extents();
        const double    uvScale = 1.0 / ( 2.0 * std::max( extents.X, std::max( extents.Y, extents.Z ) ) );
        TArray<int32>   newTriangles;
        for ( const int index : loopIndices )
        {
            const FEdgeLoop& loop = boundary.Loops[index];
            if ( !LoopIsValid( *mesh, loop ) )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Fill Hole: loop {} ({} edges) is no longer a boundary loop", index,
                     loop.GetEdgeCount() );
            FVector3d planeNormal;
            FVector3d planeOrigin;
            ComputeLoopPlane( *mesh, loop.Vertices, planeNormal, planeOrigin );
            planeNormal *= -1.0; // UE: ComputePolygonPlane orients opposite to what the fill expects
            FSimpleHoleFiller filler( mesh.get(), loop );
            if ( !filler.Fill( mesh->AllocateTriangleGroup() ) )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh Fill Hole: loop {} ({} edges): {}", index,
                                                                  loop.GetEdgeCount(), filler.FailureReason );
            if ( mesh->HasAttributes() )
            {
                FDynamicMeshEditor editor( mesh.get() );
                editor.SetTriangleNormals( filler.NewTriangles, FVector3f( static_cast<float>( planeNormal.X ),
                                                                           static_cast<float>( planeNormal.Y ),
                                                                           static_cast<float>( planeNormal.Z ) ) );
                editor.SetTriangleUVsFromProjection( filler.NewTriangles, planeOrigin, planeNormal,
                                                     static_cast<float>( uvScale ) );
            }
            for ( const int t : filler.NewTriangles )
                newTriangles.Add( t );
        }
        if ( auto tangents = RecomputeTangents( *mesh, "Fill Hole" ); !tangents.IsSuccess() )
            return Common::MakeError<RegionOutcome>( tangents.GetError() );

        ElementSelection result( ElementMode::Triangle );
        for ( const int32 t : newTriangles )
            if ( auto added = result.Add( *mesh, t ); !added.IsSuccess() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh Fill Hole: new triangle {}: {}", t,
                                                                  added.GetError() );
        if ( selection.Mode() != ElementMode::Triangle )
        {
            const FGroupTopology afterTopology( mesh.get(), true );
            result = ConvertSelection( *mesh, afterTopology, result, selection.Mode() );
        }
        return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), std::move( result ) } );
    }

    Common::ResultStr<RegionOutcome> InsertEdgeLoop( const FDynamicMesh3&    before,
                                                     const ElementSelection& selection, float position )
    {
        if ( selection.Mode() != ElementMode::Edge || selection.Empty() )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Insert Edge Loop: select one group edge in Edge mode ({} elements selected in mode {})",
                 selection.Ids().size(), static_cast<int>( selection.Mode() ) );
        // isfinite first: the negated form this replaced also refused NaN, and so must this one.
        if ( !std::isfinite( position ) || position <= 0.0f || position >= 1.0f )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Insert Edge Loop: position {} is outside (0, 1)", position );
        auto mesh = std::make_shared<FDynamicMesh3>( before );
        if ( !mesh->HasTriangleGroups() )
            return Common::MakeError<RegionOutcome>( "Mesh Insert Edge Loop: the mesh has no polygroups" );
        FGroupTopology topology( mesh.get(), true );
        int            groupEdge = IndexConstants::InvalidID;
        for ( const int e : selection.Ids() )
        {
            const int found = topology.FindGroupEdgeID( e );
            if ( found < 0 )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Insert Edge Loop: selected edge {} lies on no group edge", e );
            if ( groupEdge >= 0 && found != groupEdge )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Insert Edge Loop: the selection spans group edges {} and {} - select one", groupEdge,
                     found );
            groupEdge = found;
        }

        const TArray<double>                         proportions = { static_cast<double>( position ) };
        FGroupEdgeInserter::FEdgeLoopInsertionParams params;
        params.Mesh               = mesh.get();
        params.Topology           = &topology;
        params.GroupEdgeID        = groupEdge;
        params.SortedInputLengths = &proportions;
        params.StartCornerID      = topology.Edges[groupEdge].EndpointCorners.A;
        TSet<int32>                               newEids;
        TSet<int32>                               problemGroupEdges;
        FGroupEdgeInserter::FOptionalOutputParams out;
        out.NewEidsOut             = &newEids;
        out.ProblemGroupEdgeIDsOut = &problemGroupEdges;
        if ( !FGroupEdgeInserter::InsertEdgeLoops( params, out ) )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Insert Edge Loop: the loop across group edge {} failed ({} problem group edges)", groupEdge,
                 problemGroupEdges.Num() );
        if ( auto tangents = RecomputeTangents( *mesh, "Insert Edge Loop" ); !tangents.IsSuccess() )
            return Common::MakeError<RegionOutcome>( tangents.GetError() );

        ElementSelection result( ElementMode::Edge );
        for ( const int32 e : newEids )
            if ( auto added = result.Add( *mesh, e ); !added.IsSuccess() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh Insert Edge Loop: new edge {}: {}", e,
                                                                  added.GetError() );
        return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), std::move( result ) } );
    }

    Common::ResultStr<RegionOutcome> WeldEdges( const FDynamicMesh3& before, ElementMode mode )
    {
        auto                      mesh = std::make_shared<FDynamicMesh3>( before );
        FMergeCoincidentMeshEdges merger( mesh.get() );
        merger.bWeldAttrsOnMergedEdges                  = true;
        merger.SplitAttributeWelder.UVDistSqrdThreshold = 0.01f * 0.01f;
        merger.SplitAttributeWelder.NormalVecDotThreshold =
             std::abs( 1.f - std::cos( 0.1f * 3.14159265f / 180.f ) );
        merger.SplitAttributeWelder.TangentVecDotThreshold = merger.SplitAttributeWelder.NormalVecDotThreshold;
        if ( !merger.Apply() )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Weld Edges: the merge failed with {} of {} boundary edges left",
                 merger.FinalNumBoundaryEdges, merger.InitialNumBoundaryEdges );
        if ( merger.InitialNumBoundaryEdges == 0 )
            return Common::MakeError<RegionOutcome>( "Mesh Weld Edges: the mesh has no boundary edge to weld" );
        // UE's Weld Edges tool accepts a merge that welded nothing (WeldMeshEdgesTool.cpp:336 CanAccept checks
        // only a valid result) and commits an unchanged mesh; here a command without an effect leaves no undo
        // step.
        if ( merger.FinalNumBoundaryEdges == merger.InitialNumBoundaryEdges )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Weld Edges: no coincident edge pair within tolerance {} cm among the {} boundary edges",
                 merger.MergeVertexTolerance, merger.InitialNumBoundaryEdges );
        if ( auto tangents = RecomputeTangents( *mesh, "Weld Edges" ); !tangents.IsSuccess() )
            return Common::MakeError<RegionOutcome>( tangents.GetError() );
        return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), ElementSelection( mode ) } );
    }

    Common::ResultStr<RegionOutcome> RunRegionOperation( RegionOperation operation, const FDynamicMesh3& before,
                                                         const ElementSelection& selection, float distance )
    {
        const char* name = ToString( operation );
        if ( distance == 0.0f )
            return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: distance 0 cm changes nothing", name );
        if ( distance < 0.0f && operation != RegionOperation::PushPull )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh {}: distance {} cm is negative - {}", name, distance,
                 operation == RegionOperation::Extrude ? "Push/Pull takes a signed distance"
                 : operation == RegionOperation::Inset ? "use Outset"
                                                       : "use Inset" );
        const FGroupTopology   topology( &before, true );
        const ElementSelection triangles = ConvertSelection( before, topology, selection, ElementMode::Triangle );
        if ( triangles.Empty() )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh {}: the {} selected {} cover no whole triangle", name, selection.Size(),
                 ToString( selection.Mode() ) );
        TArray<int32> regionTriangles;
        for ( const int t : triangles.Ids() )
            regionTriangles.Add( t );

        auto          mesh = std::make_shared<FDynamicMesh3>( before );
        TArray<int32> resultTriangles;
        if ( operation == RegionOperation::Extrude || operation == RegionOperation::PushPull )
        {
            FOffsetMeshRegion extruder( mesh.get() );
            extruder.Triangles = regionTriangles;
            // UE's Extrude default (SelectedTriangleNormalsEven): every selected face moves by the full
            // distance. Push/Pull is FExtrudeOp's SingleDirection: one direction, the region's area-weighted
            // normal, so the walls are parallel and a push into a solid cannot fold them.
            extruder.ExtrusionVectorType =
                 FOffsetMeshRegion::EVertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAdjusted;
            extruder.DefaultOffsetDistance = distance;
            if ( operation == RegionOperation::PushPull )
            {
                FVector3d direction( 0.0, 0.0, 0.0 );
                for ( const int t : triangles.Ids() )
                    direction += before.GetTriNormal( t ) * before.GetTriArea( t );
                if ( direction.Length() <= 0.0 )
                    return Common::MakeFormattedError<RegionOutcome>(
                         "Mesh {}: the {} selected triangles have no average normal (they cancel out)", name,
                         triangles.Size() );
                Normalize( direction );
                const double signedDistance = distance;
                extruder.OffsetPositionFunc =
                     [direction, signedDistance]( const FVector3d& position, const FVector3d&, int )
                { return position + direction * signedDistance; };
            }
            extruder.bIsPositiveOffset = distance > 0.0f;
            if ( !extruder.Apply() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: {}", name, extruder.FailureReason );
            for ( const auto& region : extruder.OffsetRegions )
                for ( const int32 t : region.OffsetTids )
                    resultTriangles.Add( t );
        }
        else
        {
            FInsetMeshRegion inset( mesh.get() );
            inset.Triangles     = regionTriangles;
            inset.InsetDistance = operation == RegionOperation::Outset ? -distance : distance;
            if ( !inset.Apply() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: {}", name, inset.FailureReason );
            for ( const auto& region : inset.InsetRegions )
                for ( const int32 t : region.InitialTriangles )
                    resultTriangles.Add( t );
        }
        if ( auto tangents = RecomputeTangents( *mesh, name ); !tangents.IsSuccess() )
            return Common::MakeError<RegionOutcome>( tangents.GetError() );

        ElementSelection result( ElementMode::Triangle );
        for ( const int32 t : resultTriangles )
            if ( auto added = result.Add( *mesh, t ); !added.IsSuccess() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh {}: result triangle {}: {}", name, t,
                                                                  added.GetError() );
        if ( selection.Mode() != ElementMode::Triangle )
        {
            const FGroupTopology afterTopology( mesh.get(), true );
            result = ConvertSelection( *mesh, afterTopology, result, selection.Mode() );
        }
        return Common::MakeSuccess( RegionOutcome{ std::move( mesh ), std::move( result ) } );
    }
} // namespace Desert::Geometry
