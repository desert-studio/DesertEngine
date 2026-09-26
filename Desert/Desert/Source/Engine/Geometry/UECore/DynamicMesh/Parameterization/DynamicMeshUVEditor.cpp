// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Parameterization/
// DynamicMeshUVEditor.cpp:142-148, 184-196, 486-524, 543-630 and FDynamicMesh3::GetVertexFrame
// (GeometryCore/Private/DynamicMesh/DynamicMesh3_Queries.cpp:824-850, bFrameNormalY = false, with a normal),
// adapted: GetVertexFrame is a local helper (UECore's FDynamicMesh3 has no FFrame3d); the submesh gets no vertex
// normals (UE's QuickComputeVertexNormals), TMeshLocalParam computes the area-weighted normal per vertex instead;
// a triangle the submesh cannot append counts as failed. SetTriangleUVsFromFreeBoundarySpectralConformal is
// DynamicMeshUVEditor.cpp:754-988 with Options.bUseSpectral fixed true (the other branch is not ported).
// SetToPerVertexUVs is :198-222 without the FUVEditResult; ScaleUVAreaTo3DArea is :1461-1497 with
// DetermineAreaFromUVs (:1743-1768) and GetVolumeArea's area written out.
#include "Engine/Geometry/UECore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/DynamicSubmesh3.hpp"
#include "Engine/Geometry/UECore/MeshBoundaryLoops.hpp"
#include "Engine/Geometry/UECore/Parameterization/MeshDijkstra.hpp"
#include "Engine/Geometry/UECore/Parameterization/MeshLocalParam.hpp"
#include "Engine/Geometry/UECore/Solvers/MeshUVSolver.hpp"
#include "Engine/Geometry/UECore/VectorUtil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Geometry
{
    namespace
    {
        FFrame3d GetVertexFrame( const FDynamicMesh3& Mesh, int32 VertexID, const FVector3d& UseNormal )
        {
            const FVector3d v      = Mesh.GetVertex( VertexID );
            const FVector3d normal = Normalized( UseNormal );
            int32           eid    = FDynamicMesh3::InvalidID;
            for ( const int32 VtxEdge : Mesh.VtxEdgesItr( VertexID ) )
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

    void FDynamicMeshUVEditor::ResetUVs( const TArray<int32>& Triangles )
    {
        UVOverlay->ClearElements( Triangles );
    }

    void
    FDynamicMeshUVEditor::TransformUVElements( const TArray<int32>&                                ElementIDs,
                                               const std::function<FVector2f( const FVector2f& )>& TransformFunc )
    {
        for ( const int32 elemid : ElementIDs )
        {
            if ( UVOverlay->IsElement( elemid ) )
                UVOverlay->SetElement( elemid, TransformFunc( UVOverlay->GetElement( elemid ) ) );
        }
    }

    bool FDynamicMeshUVEditor::EstimateGeodesicCenterFrameVertex( const FDynamicMesh3& Mesh, FFrame3d& FrameOut,
                                                                  int32& VertexIDOut, bool bAlignToUnitAxes )
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
        for ( const int32 vid : Loop->Vertices )
            SeedPoints.Add( FDijkstra::FSeedPoint{ vid, vid, 0.0 } );
        FDijkstra Dijkstra( &Mesh );
        Dijkstra.ComputeToMaxDistance( SeedPoints, TNumericLimits<float>::Max() );
        const int32 MaxDistVID = Dijkstra.GetMaxGraphDistancePointID();
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

    bool FDynamicMeshUVEditor::SetTriangleUVsFromExpMap( const TArray<int32>& Triangles, FUVEditResult* Result )
    {
        if ( UVOverlay == nullptr || Triangles.Num() == 0 )
            return false;
        ResetUVs( Triangles );

        FDynamicSubmesh3     SubmeshCalc( Mesh, Triangles );
        const FDynamicMesh3& Submesh = SubmeshCalc.GetSubmesh();
        if ( Submesh.TriangleCount() == 0 )
            return false;

        FFrame3d   SeedFrame;
        int32      FrameVertexID = FDynamicMesh3::InvalidID;
        const bool bFrameOK      = EstimateGeodesicCenterFrameVertex( Submesh, SeedFrame, FrameVertexID, true );
        if ( !Submesh.IsVertex( FrameVertexID ) )
            return false;

        TMeshLocalParam<FDynamicMesh3> Param( &Submesh );
        Param.ParamMode = ELocalParamTypes::ExponentialMapUpwindAvg;
        Param.ComputeToMaxDistance( FrameVertexID, SeedFrame, TNumericLimits<float>::Max() );

        TArray<int32> VtxElementIDs;
        TArray<int32> NewElementIDs;
        VtxElementIDs.Init( FDynamicMesh3::InvalidID, Submesh.MaxVertexID() );
        const double MaxFloat = TNumericLimits<float>::Max();
        for ( const int32 vid : Submesh.VertexIndicesItr() )
        {
            if ( !Param.HasUV( vid ) )
                continue;
            const FVector2d UVd = Param.GetUV( vid );
            const FVector2f UV( static_cast<float>( UVd.X > MaxFloat ? MaxFloat : UVd.X ),
                                static_cast<float>( UVd.Y > MaxFloat ? MaxFloat : UVd.Y ) );
            VtxElementIDs[vid] = UVOverlay->AppendElement( UV );
            NewElementIDs.Add( VtxElementIDs[vid] );
        }

        int32 NumFailed = SubmeshCalc.GetFailedTriangles().Num();
        for ( const int32 tid : Submesh.TriangleIndicesItr() )
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
            Result->NewUVElements = MoveTemp( NewElementIDs );
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

    void FDynamicMeshUVEditor::SetToPerVertexUVs( TArray<int32>& VertexToUVOut, bool& bIsIdentityMapOut )
    {
        bIsIdentityMapOut = true;
        VertexToUVOut.Init( FDynamicMesh3::InvalidID, Mesh->MaxVertexID() );
        UVOverlay->ClearElements();
        for ( const int32 VertexID : Mesh->VertexIndicesItr() )
        {
            const int32 UVID        = UVOverlay->AppendElement( FVector2f( 0.0f, 0.0f ) );
            VertexToUVOut[VertexID] = UVID;
            bIsIdentityMapOut       = bIsIdentityMapOut && UVID == VertexID;
        }
        for ( const int32 TriangleID : Mesh->TriangleIndicesItr() )
        {
            const FIndex3i Tri = Mesh->GetTriangle( TriangleID );
            UVOverlay->SetTriangle( TriangleID,
                                    FIndex3i( VertexToUVOut[Tri.A], VertexToUVOut[Tri.B], VertexToUVOut[Tri.C] ) );
        }
    }

    bool FDynamicMeshUVEditor::ScaleUVAreaTo3DArea( const TArray<int32>& Triangles, bool bRecenterAtOrigin,
                                                    float ScaleFactor )
    {
        double Area3D = 0.0;
        for ( const int32 tid : Triangles )
        {
            if ( !Mesh->IsTriangle( tid ) )
                continue;
            const FIndex3i Tri = Mesh->GetTriangle( tid );
            Area3D +=
                 VectorUtil::Area( Mesh->GetVertex( Tri.A ), Mesh->GetVertex( Tri.B ), Mesh->GetVertex( Tri.C ) );
        }
        if ( std::abs( Area3D ) < FMathf::Epsilon || !std::isfinite( Area3D ) )
            return false;

        std::unordered_set<int32> Elements;
        double                    Area2D = 0.0;
        FVector2f BoundsMin( std::numeric_limits<float>::max(), std::numeric_limits<float>::max() );
        FVector2f BoundsMax( -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() );
        for ( const int32 tid : Triangles )
        {
            if ( !UVOverlay->IsSetTriangle( tid ) )
                continue;
            const FIndex3i                 UVTri = UVOverlay->GetTriangle( tid );
            const std::array<FVector2f, 3> UV    = { UVOverlay->GetElement( UVTri.A ),
                                                     UVOverlay->GetElement( UVTri.B ),
                                                     UVOverlay->GetElement( UVTri.C ) };
            for ( int32 j = 0; j < 3; ++j )
            {
                Elements.insert( UVTri[j] );
                BoundsMin = FVector2f( std::min( BoundsMin.X, UV[j].X ), std::min( BoundsMin.Y, UV[j].Y ) );
                BoundsMax = FVector2f( std::max( BoundsMax.X, UV[j].X ), std::max( BoundsMax.Y, UV[j].Y ) );
            }
            const FVector2f E1 = UV[1] - UV[0];
            const FVector2f E2 = UV[2] - UV[0];
            Area2D += 0.5 * std::abs( static_cast<double>( E1.X ) * E2.Y - static_cast<double>( E1.Y ) * E2.X );
        }
        if ( Elements.empty() || std::abs( Area2D ) < FMathf::Epsilon || !std::isfinite( Area2D ) )
            return false;

        const double UVScale = ScaleFactor * std::sqrt( Area3D ) / std::sqrt( Area2D );
        if ( !std::isfinite( UVScale ) )
            return false;
        const FVector2f ScaleOrigin = ( BoundsMin + BoundsMax ) * 0.5f;
        const FVector2f Translation = bRecenterAtOrigin ? FVector2f( 0.0f, 0.0f ) : ScaleOrigin;
        for ( const int32 eid : Elements )
            UVOverlay->SetElement( eid,
                                   ( UVOverlay->GetElement( eid ) - ScaleOrigin ) * static_cast<float>( UVScale ) +
                                        Translation );
        return true;
    }
} // namespace Desert::Geometry
