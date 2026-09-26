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
        FFrame3d GetVertexFrame( const FDynamicMesh3& Mesh, int32_t VertexID, const glm::dvec3& UseNormal )
        {
            const glm::dvec3 v      = Mesh.GetVertex( VertexID );
            const glm::dvec3 normal = Normalized( UseNormal );
            int32_t         eid    = FDynamicMesh3::InvalidID;
            for ( const int32_t VtxEdge : Mesh.VtxEdgesItr( VertexID ) )
            {
                eid = VtxEdge;
                break;
            }
            const FIndex2i  ev    = Mesh.GetEdgeV( eid );
            glm::dvec3       edge  = Normalized( Mesh.GetVertex( ev.A == VertexID ? ev.B : ev.A ) - v );
            const glm::dvec3 other = glm::cross( normal, edge );
            edge                   = glm::cross( other, normal );
            return { v, edge, other, normal };
        }

        glm::vec2 ToFloat( const glm::dvec2& UV )
        {
            return { static_cast<float>( UV.x ), static_cast<float>( UV.y ) };
        }
    } // namespace

    void FDynamicMeshUVEditor::ResetUVs( const std::vector<int32_t>& Triangles )
    {
        UVOverlay->ClearElements( Triangles );
    }

    void
    FDynamicMeshUVEditor::TransformUVElements( const std::vector<int32_t>&                         ElementIDs,
                                               const std::function<glm::vec2( const glm::vec2& )>& TransformFunc )
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
        glm::dvec3               Normal = FMeshNormals::ComputeVertexNormal( Mesh, VertexIDOut );
        const FMeshBoundaryLoops LoopsCalc( &Mesh, true );
        if ( LoopsCalc.GetLoopCount() == 0 )
        {
            FrameOut = GetVertexFrame( Mesh, VertexIDOut, Normal );
            return false;
        }
        const FEdgeLoop* Loop = &LoopsCalc.Loops[0];
        for ( const FEdgeLoop& Candidate : LoopsCalc.Loops )
        {
            if ( static_cast<int32_t>( Candidate.Vertices.size() ) >
                 static_cast<int32_t>( Loop->Vertices.size() ) )
                Loop = &Candidate;
        }
        using FDijkstra = TMeshDijkstra<FDynamicMesh3>;
        std::vector<FDijkstra::FSeedPoint> SeedPoints;
        for ( const int32_t vid : Loop->Vertices )
            SeedPoints.push_back( FDijkstra::FSeedPoint{ vid, vid, 0.0 } );
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
            FrameOut.ConstrainedAlignPerpAxes( 0, 1, 2, glm::dvec3( 1, 0, 0 ), glm::dvec3( 0, 1, 0 ), 0.95 );
        return true;
    }

    bool FDynamicMeshUVEditor::SetTriangleUVsFromExpMap( const std::vector<int32_t>& Triangles,
                                                         FUVEditResult*              Result )
    {
        if ( UVOverlay == nullptr || Triangles.empty() )
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

        std::vector<int32_t> VtxElementIDs;
        std::vector<int32_t> NewElementIDs;
        VtxElementIDs.assign( Submesh.MaxVertexID(), FDynamicMesh3::InvalidID );
        const double MaxFloat = std::numeric_limits<float>::max();
        for ( const int32_t vid : Submesh.VertexIndicesItr() )
        {
            if ( !Param.HasUV( vid ) )
                continue;
            const glm::dvec2 UVd = Param.GetUV( vid );
            const glm::vec2  UV( static_cast<float>( UVd.x > MaxFloat ? MaxFloat : UVd.x ),
                                 static_cast<float>( UVd.y > MaxFloat ? MaxFloat : UVd.y ) );
            VtxElementIDs[vid] = UVOverlay->AppendElement( UV );
            NewElementIDs.push_back( VtxElementIDs[vid] );
        }

        int32_t NumFailed = static_cast<int32_t>( SubmeshCalc.GetFailedTriangles().size() );
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

    bool FDynamicMeshUVEditor::SetTriangleUVsFromFreeBoundarySpectralConformal(
         const std::vector<int32_t>& Triangles, bool bUseExistingUVTopology, bool bPreserveIrregularity,
         FUVEditResult* Result )
    {
        if ( UVOverlay == nullptr || Triangles.empty() )
            return false;
        if ( !bUseExistingUVTopology )
            ResetUVs( Triangles );

        FDynamicMesh3                        Submesh;
        std::unordered_map<int32_t, int32_t> BaseToSubmeshV;
        std::vector<int32_t>                 SubmeshToBaseV;
        std::vector<int32_t>                 SubmeshToBaseT;
        for ( const int32_t tid : Triangles )
        {
            if ( bUseExistingUVTopology && !UVOverlay->IsSetTriangle( tid ) )
                continue;
            const FIndex3i Triangle =
                 bUseExistingUVTopology ? UVOverlay->GetTriangle( tid ) : Mesh->GetTriangle( tid );
            FIndex3i NewTriangle;
            for ( int32_t j = 0; j < 3; ++j )
            {
                const auto Found = BaseToSubmeshV.find( Triangle[j] );
                if ( Found != BaseToSubmeshV.end() )
                {
                    NewTriangle[j] = Found->second;
                    continue;
                }
                const glm::dvec3 Position = Mesh->GetVertex(
                     bUseExistingUVTopology ? UVOverlay->GetParentVertex( Triangle[j] ) : Triangle[j] );
                NewTriangle[j] = Submesh.AppendVertex( Position );
                SubmeshToBaseV.push_back( Triangle[j] );
                BaseToSubmeshV.emplace( Triangle[j], NewTriangle[j] );
            }
            if ( Submesh.AppendTriangle( NewTriangle ) < 0 )
                return false; // the UV topology is not a manifold submesh: nothing to parameterize
            SubmeshToBaseT.push_back( tid );
        }

        const FMeshBoundaryLoops Loops( &Submesh, true );
        const FEdgeLoop*         Longest = nullptr;
        for ( const FEdgeLoop& Loop : Loops.Loops )
        {
            if ( Longest == nullptr || Loop.Vertices.size() > Longest->Vertices.size() )
                Longest = &Loop;
        }
        if ( Longest == nullptr )
            return false;
        FSpectralConformalMeshUVSolver Solver( Submesh, bPreserveIrregularity );
        for ( const int32_t vid : Longest->Vertices )
            Solver.AddBoundaryVertex( vid );
        std::vector<glm::dvec2> UVBuffer;
        if ( !Solver.SolveUVs( UVBuffer ) )
            return false;

        if ( bUseExistingUVTopology )
        {
            for ( int32_t k = 0; k < static_cast<int32_t>( SubmeshToBaseV.size() ); ++k )
                UVOverlay->SetElement( SubmeshToBaseV[k], ToFloat( UVBuffer[k] ) );
            if ( Result != nullptr )
                Result->NewUVElements = std::move( SubmeshToBaseV );
            return true;
        }
        std::vector<int32_t> VtxElementIDs;
        std::vector<int32_t> NewElementIDs;
        VtxElementIDs.assign( Submesh.MaxVertexID(), FDynamicMesh3::InvalidID );
        for ( const int32_t vid : Submesh.VertexIndicesItr() )
        {
            VtxElementIDs[vid] = UVOverlay->AppendElement( ToFloat( UVBuffer[vid] ) );
            NewElementIDs.push_back( VtxElementIDs[vid] );
        }
        for ( const int32_t tid : Submesh.TriangleIndicesItr() )
        {
            const FIndex3i SubTri = Submesh.GetTriangle( tid );
            UVOverlay->SetTriangle(
                 SubmeshToBaseT[tid],
                 FIndex3i( VtxElementIDs[SubTri.A], VtxElementIDs[SubTri.B], VtxElementIDs[SubTri.C] ) );
        }
        if ( Result != nullptr )
            Result->NewUVElements = std::move( NewElementIDs );
        return true;
    }

    void FDynamicMeshUVEditor::SetToPerVertexUVs( std::vector<int32_t>& VertexToUVOut, bool& bIsIdentityMapOut )
    {
        bIsIdentityMapOut = true;
        VertexToUVOut.assign( Mesh->MaxVertexID(), FDynamicMesh3::InvalidID );
        UVOverlay->ClearElements();
        for ( const int32_t VertexID : Mesh->VertexIndicesItr() )
        {
            const int32_t UVID      = UVOverlay->AppendElement( glm::vec2( 0.0f, 0.0f ) );
            VertexToUVOut[VertexID] = UVID;
            bIsIdentityMapOut       = bIsIdentityMapOut && UVID == VertexID;
        }
        for ( const int32_t TriangleID : Mesh->TriangleIndicesItr() )
        {
            const FIndex3i Tri = Mesh->GetTriangle( TriangleID );
            UVOverlay->SetTriangle( TriangleID,
                                    FIndex3i( VertexToUVOut[Tri.A], VertexToUVOut[Tri.B], VertexToUVOut[Tri.C] ) );
        }
    }

    bool FDynamicMeshUVEditor::ScaleUVAreaTo3DArea( const std::vector<int32_t>& Triangles, bool bRecenterAtOrigin,
                                                    float ScaleFactor )
    {
        double Area3D = 0.0;
        for ( const int32_t tid : Triangles )
        {
            if ( !Mesh->IsTriangle( tid ) )
                continue;
            const FIndex3i Tri = Mesh->GetTriangle( tid );
            Area3D +=
                 VectorUtil::Area( Mesh->GetVertex( Tri.A ), Mesh->GetVertex( Tri.B ), Mesh->GetVertex( Tri.C ) );
        }
        if ( std::abs( Area3D ) < FMathf::Epsilon || !std::isfinite( Area3D ) )
            return false;

        std::unordered_set<int32_t> Elements;
        double                      Area2D = 0.0;
        glm::vec2 BoundsMin( std::numeric_limits<float>::max(), std::numeric_limits<float>::max() );
        glm::vec2 BoundsMax( -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() );
        for ( const int32_t tid : Triangles )
        {
            if ( !UVOverlay->IsSetTriangle( tid ) )
                continue;
            const FIndex3i                 UVTri = UVOverlay->GetTriangle( tid );
            const std::array<glm::vec2, 3> UV    = { UVOverlay->GetElement( UVTri.A ),
                                                     UVOverlay->GetElement( UVTri.B ),
                                                     UVOverlay->GetElement( UVTri.C ) };
            for ( int32_t j = 0; j < 3; ++j )
            {
                Elements.insert( UVTri[j] );
                BoundsMin = glm::vec2( std::min( BoundsMin.x, UV[j].x ), std::min( BoundsMin.y, UV[j].y ) );
                BoundsMax = glm::vec2( std::max( BoundsMax.x, UV[j].x ), std::max( BoundsMax.y, UV[j].y ) );
            }
            const glm::vec2 E1 = UV[1] - UV[0];
            const glm::vec2 E2 = UV[2] - UV[0];
            Area2D += 0.5 * std::abs( static_cast<double>( E1.x ) * E2.y - static_cast<double>( E1.y ) * E2.x );
        }
        if ( Elements.empty() || std::abs( Area2D ) < FMathf::Epsilon || !std::isfinite( Area2D ) )
            return false;

        const double UVScale = ScaleFactor * std::sqrt( Area3D ) / std::sqrt( Area2D );
        if ( !std::isfinite( UVScale ) )
            return false;
        const glm::vec2 ScaleOrigin = ( BoundsMin + BoundsMax ) * 0.5f;
        const glm::vec2 Translation = bRecenterAtOrigin ? glm::vec2( 0.0f, 0.0f ) : ScaleOrigin;
        for ( const int32_t eid : Elements )
            UVOverlay->SetElement( eid,
                                   ( UVOverlay->GetElement( eid ) - ScaleOrigin ) * static_cast<float>( UVScale ) +
                                        Translation );
        return true;
    }
} // namespace Desert::Geometry
