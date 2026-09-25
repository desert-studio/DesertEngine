// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Parameterization/
// DynamicMeshUVEditor.cpp:142-148, 184-196, 486-524, 543-630 and FDynamicMesh3::GetVertexFrame
// (GeometryCore/Private/DynamicMesh/DynamicMesh3_Queries.cpp:824-850, bFrameNormalY = false, with a normal),
// adapted: GetVertexFrame is a local helper (UECore's FDynamicMesh3 has no FFrame3d); the submesh gets no vertex
// normals (UE's QuickComputeVertexNormals), TMeshLocalParam computes the area-weighted normal per vertex instead;
// a triangle the submesh cannot append counts as failed. SetTriangleUVsFromFreeBoundarySpectralConformal is
// DynamicMeshUVEditor.cpp:754-988 with Options.bUseSpectral fixed true (the other branch is not ported).
#include "Engine/Geometry/UECore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/DynamicSubmesh3.hpp"
#include "Engine/Geometry/UECore/MeshBoundaryLoops.hpp"
#include "Engine/Geometry/UECore/Parameterization/MeshDijkstra.hpp"
#include "Engine/Geometry/UECore/Parameterization/MeshLocalParam.hpp"
#include "Engine/Geometry/UECore/Solvers/MeshUVSolver.hpp"

#include <unordered_map>

namespace Desert::Geometry
{
    namespace
    {
        FFrame3d GetVertexFrame( const FDynamicMesh3& Mesh, int32_t VertexID, const FVector3d& UseNormal )
        {
            const FVector3d v      = Mesh.GetVertex( VertexID );
            const FVector3d normal = Normalized( UseNormal );
            int32_t         eid    = FDynamicMesh3::InvalidID;
            for ( const int32_t VtxEdge : Mesh.VtxEdgesItr( VertexID ) )
            {
                eid = VtxEdge;
                break;
            }
            const FIndex2i  ev    = Mesh.GetEdgeV( eid );
            FVector3d       edge  = Normalized( Mesh.GetVertex( ev.A == VertexID ? ev.B : ev.A ) - v );
            const FVector3d other = normal.Cross( edge );
            edge                  = other.Cross( normal );
            return { v, edge, other, normal };
        }

        FVector2f ToFloat( const FVector2d& UV )
        {
            return { static_cast<float>( UV.X ), static_cast<float>( UV.Y ) };
        }
    } // namespace

    void FDynamicMeshUVEditor::ResetUVs( const TArray<int32_t>& Triangles )
    {
        UVOverlay->ClearElements( Triangles );
    }

    void
    FDynamicMeshUVEditor::TransformUVElements( const TArray<int32_t>&                              ElementIDs,
                                               const std::function<FVector2f( const FVector2f& )>& TransformFunc )
    {
        for ( const int32_t elemid : ElementIDs )
        {
            if ( UVOverlay->IsElement( elemid ) )
                UVOverlay->SetElement( elemid, TransformFunc( UVOverlay->GetElement( elemid ) ) );
        }
    }

    bool FDynamicMeshUVEditor::EstimateGeodesicCenterFrameVertex( const FDynamicMesh3& Mesh, FFrame3d& FrameOut,
                                                                  int32_t& VertexIDOut, bool bAlignToUnitAxes )
    {
        VertexIDOut                     = *Mesh.VertexIndicesItr().begin();
        FVector3d                Normal = FMeshNormals::ComputeVertexNormal( Mesh, VertexIDOut );
        const FMeshBoundaryLoops LoopsCalc( &Mesh, true );
        if ( LoopsCalc.GetLoopCount() == 0 )
        {
            FrameOut = GetVertexFrame( Mesh, VertexIDOut, Normal );
            return false;
        }
        const FEdgeLoop* Loop = &LoopsCalc.Loops[0];
        for ( const FEdgeLoop& Candidate : LoopsCalc.Loops )
        {
            if ( Candidate.Vertices.Num() > Loop->Vertices.Num() )
                Loop = &Candidate;
        }
        using FDijkstra = TMeshDijkstra<FDynamicMesh3>;
        TArray<FDijkstra::FSeedPoint> SeedPoints;
        for ( const int32_t vid : Loop->Vertices )
            SeedPoints.Add( FDijkstra::FSeedPoint{ vid, vid, 0.0 } );
        FDijkstra Dijkstra( &Mesh );
        Dijkstra.ComputeToMaxDistance( SeedPoints, std::numeric_limits<float>::max() );
        const int32_t MaxDistVID = Dijkstra.GetMaxGraphDistancePointID();
        if ( !Mesh.IsVertex( MaxDistVID ) )
        {
            FrameOut = GetVertexFrame( Mesh, VertexIDOut, Normal );
            return false;
        }
        VertexIDOut = MaxDistVID;
        Normal      = FMeshNormals::ComputeVertexNormal( Mesh, MaxDistVID );
        FrameOut    = GetVertexFrame( Mesh, MaxDistVID, Normal );
        if ( bAlignToUnitAxes ) // try to generate consistent frame alignment
            FrameOut.ConstrainedAlignPerpAxes( 0, 1, 2, FVector3d::UnitX(), FVector3d::UnitY(), 0.95 );
        return true;
    }

    bool FDynamicMeshUVEditor::SetTriangleUVsFromExpMap( const TArray<int32_t>& Triangles, FUVEditResult* Result )
    {
        if ( UVOverlay == nullptr || Triangles.Num() == 0 )
            return false;
        ResetUVs( Triangles );

        FDynamicSubmesh3     SubmeshCalc( Mesh, Triangles );
        const FDynamicMesh3& Submesh = SubmeshCalc.GetSubmesh();
        if ( Submesh.TriangleCount() == 0 )
            return false;

        FFrame3d   SeedFrame;
        int32_t    FrameVertexID = FDynamicMesh3::InvalidID;
        const bool bFrameOK      = EstimateGeodesicCenterFrameVertex( Submesh, SeedFrame, FrameVertexID, true );
        if ( !Submesh.IsVertex( FrameVertexID ) )
            return false;

        TMeshLocalParam<FDynamicMesh3> Param( &Submesh );
        Param.ParamMode = ELocalParamTypes::ExponentialMapUpwindAvg;
        Param.ComputeToMaxDistance( FrameVertexID, SeedFrame, std::numeric_limits<float>::max() );

        TArray<int32_t> VtxElementIDs;
        TArray<int32_t> NewElementIDs;
        VtxElementIDs.Init( FDynamicMesh3::InvalidID, Submesh.MaxVertexID() );
        const double MaxFloat = std::numeric_limits<float>::max();
        for ( const int32_t vid : Submesh.VertexIndicesItr() )
        {
            if ( !Param.HasUV( vid ) )
                continue;
            const FVector2d UVd = Param.GetUV( vid );
            const FVector2f UV( static_cast<float>( UVd.X > MaxFloat ? MaxFloat : UVd.X ),
                                static_cast<float>( UVd.Y > MaxFloat ? MaxFloat : UVd.Y ) );
            VtxElementIDs[vid] = UVOverlay->AppendElement( UV );
            NewElementIDs.Add( VtxElementIDs[vid] );
        }

        int32_t NumFailed = SubmeshCalc.GetFailedTriangles().Num();
        for ( const int32_t tid : Submesh.TriangleIndicesItr() )
        {
            const FIndex3i SubTri = Submesh.GetTriangle( tid );
            const FIndex3i UVTri( VtxElementIDs[SubTri.A], VtxElementIDs[SubTri.B], VtxElementIDs[SubTri.C] );
            if ( UVTri.A == FDynamicMesh3::InvalidID || UVTri.B == FDynamicMesh3::InvalidID ||
                 UVTri.C == FDynamicMesh3::InvalidID )
            {
                NumFailed++;
                continue;
            }
            UVOverlay->SetTriangle( SubmeshCalc.MapTriangleToBaseMesh( tid ), UVTri );
        }
        if ( Result != nullptr )
            Result->NewUVElements = std::move( NewElementIDs );
        // a fallback frame is always a failure (the quality would be very bad), as is any triangle left unset
        return bFrameOK && NumFailed == 0;
    }

    bool FDynamicMeshUVEditor::SetTriangleUVsFromFreeBoundarySpectralConformal( const TArray<int32>& Triangles,
                                                                                bool bUseExistingUVTopology,
                                                                                bool bPreserveIrregularity,
                                                                                FUVEditResult* Result )
    {
        if ( UVOverlay == nullptr || Triangles.Num() == 0 )
            return false;
        if ( !bUseExistingUVTopology )
            ResetUVs( Triangles );

        FDynamicMesh3                    Submesh;
        std::unordered_map<int32, int32> BaseToSubmeshV;
        TArray<int32>                    SubmeshToBaseV;
        TArray<int32>                    SubmeshToBaseT;
        for ( const int32 tid : Triangles )
        {
            if ( bUseExistingUVTopology && !UVOverlay->IsSetTriangle( tid ) )
                continue;
            const FIndex3i Triangle =
                 bUseExistingUVTopology ? UVOverlay->GetTriangle( tid ) : Mesh->GetTriangle( tid );
            FIndex3i NewTriangle;
            for ( int32 j = 0; j < 3; ++j )
            {
                const auto Found = BaseToSubmeshV.find( Triangle[j] );
                if ( Found != BaseToSubmeshV.end() )
                {
                    NewTriangle[j] = Found->second;
                    continue;
                }
                const FVector3d Position = Mesh->GetVertex(
                     bUseExistingUVTopology ? UVOverlay->GetParentVertex( Triangle[j] ) : Triangle[j] );
                NewTriangle[j] = Submesh.AppendVertex( Position );
                SubmeshToBaseV.Add( Triangle[j] );
                BaseToSubmeshV.emplace( Triangle[j], NewTriangle[j] );
            }
            if ( Submesh.AppendTriangle( NewTriangle ) < 0 )
                return false; // the UV topology is not a manifold submesh: nothing to parameterize
            SubmeshToBaseT.Add( tid );
        }

        const FMeshBoundaryLoops Loops( &Submesh, true );
        const FEdgeLoop*         Longest = nullptr;
        for ( const FEdgeLoop& Loop : Loops.Loops )
        {
            if ( Longest == nullptr || Loop.Vertices.Num() > Longest->Vertices.Num() )
                Longest = &Loop;
        }
        if ( Longest == nullptr )
            return false;
        FSpectralConformalMeshUVSolver Solver( Submesh, bPreserveIrregularity );
        for ( const int32 vid : Longest->Vertices )
            Solver.AddBoundaryVertex( vid );
        TArray<FVector2d> UVBuffer;
        if ( !Solver.SolveUVs( UVBuffer ) )
            return false;

        if ( bUseExistingUVTopology )
        {
            for ( int32 k = 0; k < SubmeshToBaseV.Num(); ++k )
                UVOverlay->SetElement( SubmeshToBaseV[k], ToFloat( UVBuffer[k] ) );
            if ( Result != nullptr )
                Result->NewUVElements = MoveTemp( SubmeshToBaseV );
            return true;
        }
        TArray<int32> VtxElementIDs;
        TArray<int32> NewElementIDs;
        VtxElementIDs.Init( FDynamicMesh3::InvalidID, Submesh.MaxVertexID() );
        for ( const int32 vid : Submesh.VertexIndicesItr() )
        {
            VtxElementIDs[vid] = UVOverlay->AppendElement( ToFloat( UVBuffer[vid] ) );
            NewElementIDs.Add( VtxElementIDs[vid] );
        }
        for ( const int32 tid : Submesh.TriangleIndicesItr() )
        {
            const FIndex3i SubTri = Submesh.GetTriangle( tid );
            UVOverlay->SetTriangle(
                 SubmeshToBaseT[tid],
                 FIndex3i( VtxElementIDs[SubTri.A], VtxElementIDs[SubTri.B], VtxElementIDs[SubTri.C] ) );
        }
        if ( Result != nullptr )
            Result->NewUVElements = MoveTemp( NewElementIDs );
        return true;
    }
} // namespace Desert::Geometry
