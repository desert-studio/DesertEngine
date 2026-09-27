// Ported from UE 5.8 .../DynamicMesh/Private/Operations/MeshPlaneCut.cpp (see the header for the line ranges and
// what was left out).
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MeshPlaneCut.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/PlanarHoleFiller.hpp"
#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/MeshCore/MeshBoundaryLoops.hpp"

#include <spdlog/fmt/fmt.h>

#include <cmath>

using namespace Desert::Geometry;

void MeshPlaneCut::ComputeVertexSignedDistances( std::vector<double>& Signs, double InvalidDist ) const
{
    const int MaxVID = m_Mesh->MaxVertexID();
    Signs.assign( static_cast<size_t>( MaxVID ), InvalidDist );
    for ( int VID = 0; VID < MaxVID; ++VID )
        if ( m_Mesh->IsVertex( VID ) )
            Signs[VID] = glm::dot( m_Mesh->GetVertex( VID ) - m_PlaneOrigin, m_PlaneNormal );
}

void MeshPlaneCut::SplitCrossingEdges( bool bDeleteTrisOnPlane, std::vector<double>& Signs,
                                       std::unordered_set<int>& AlreadyOnPlaneEdges,
                                       std::unordered_set<int>& CutPlaneEdges )
{
    AlreadyOnPlaneEdges.clear();
    CutPlaneEdges.clear();
    // Invalid dists are zero, because any vertex we add will be on the plane.
    ComputeVertexSignedDistances( Signs, 0.0 );

    if ( bDeleteTrisOnPlane )
    {
        for ( int TID = 0; TID < m_Mesh->MaxTriangleID(); TID++ )
        {
            if ( !m_Mesh->IsTriangle( TID ) )
                continue;
            const Index3i Tri = m_Mesh->GetTriangle( TID );
            if ( std::abs( Signs[Tri.A] ) < m_PlaneTolerance && std::abs( Signs[Tri.B] ) < m_PlaneTolerance &&
                 std::abs( Signs[Tri.C] ) < m_PlaneTolerance )
                static_cast<void>( m_Mesh->RemoveTriangle( TID, true, false ) );
        }
    }

    // New edges must not be processed: an ID >= the max at the start is new, and so is one in this set.
    const int               MaxEID = m_Mesh->MaxEdgeID();
    std::unordered_set<int> NewEdgesBeforeMaxID;
    auto                    AddNewEdge = [&NewEdgesBeforeMaxID, MaxEID]( int NewEID )
    {
        if ( NewEID < MaxEID )
            NewEdgesBeforeMaxID.insert( NewEID );
    };

    for ( int EID = 0; EID < MaxEID; ++EID )
    {
        if ( !m_Mesh->IsEdge( EID ) || NewEdgesBeforeMaxID.contains( EID ) )
            continue;
        const Index2i EdgeV = m_Mesh->GetEdgeV( EID );
        const double  DistA = Signs[EdgeV.A];
        const double  DistB = Signs[EdgeV.B];
        // Both on plane: the edge is on the contour; one on plane: that vertex is.
        const bool bAOnPlane = std::abs( DistA ) < m_PlaneTolerance;
        const bool bBOnPlane = std::abs( DistB ) < m_PlaneTolerance;
        if ( bAOnPlane || bBOnPlane )
        {
            if ( bAOnPlane && bBOnPlane )
                AlreadyOnPlaneEdges.insert( EID );
            continue;
        }
        if ( DistA * DistB > 0 )
            continue; // no crossing

        DynamicMeshInfo::EdgeSplitInfo SplitInfo;
        const double     Param       = DistA / ( DistA - DistB );
        const MeshResult SplitResult = m_Mesh->SplitEdge( EID, SplitInfo, Param );
        // A split of a valid edge between two off-plane vertices of opposite signs cannot fail; UE ensure()s and
        // skips the edge, and so does this port (its loops then report the gap).
        if ( SplitResult != MeshResult::Ok )
            continue;
        AddNewEdge( SplitInfo.NewEdges.A );
        AddNewEdge( SplitInfo.NewEdges.B );

        // The edges to the other vertices lie in the plane if that vertex is new or within tolerance of it.
        const int OtherVIDA = SplitInfo.OtherVertices.A;
        if ( OtherVIDA >= static_cast<int>( Signs.size() ) || std::abs( Signs[OtherVIDA] ) < m_PlaneTolerance )
            CutPlaneEdges.insert( SplitInfo.NewEdges.B );
        if ( SplitInfo.NewEdges.C != DynamicMesh3::InvalidID )
        {
            AddNewEdge( SplitInfo.NewEdges.C );
            const int OtherVIDB = SplitInfo.OtherVertices.B;
            if ( OtherVIDB >= static_cast<int>( Signs.size() ) || std::abs( Signs[OtherVIDB] ) < m_PlaneTolerance )
                CutPlaneEdges.insert( SplitInfo.NewEdges.C );
        }
    }
}

bool MeshPlaneCut::Cut()
{
    std::vector<double>     Signs;
    std::unordered_set<int> ZeroEdges;
    std::unordered_set<int> OnCutEdges;
    SplitCrossingEdges( true, Signs, ZeroEdges, OnCutEdges );

    // remove the one-rings of all positive-side vertices
    for ( int VID = 0; VID < static_cast<int>( Signs.size() ); ++VID )
        if ( Signs[VID] > m_PlaneTolerance )
            static_cast<void>( m_Mesh->RemoveVertex( VID, false ) );

    if ( m_bCollapseDegenerateEdgesOnCut )
        CollapseDegenerateEdges( OnCutEdges );

    OpenBoundary& Boundary = m_OpenBoundaries.emplace_back();
    return ExtractBoundaryLoops( OnCutEdges, ZeroEdges, Boundary );
}

bool MeshPlaneCut::ExtractBoundaryLoops( const std::unordered_set<int>& OnCutEdges,
                                         const std::unordered_set<int>& ZeroEdges, OpenBoundary& Boundary ) const
{
    // Loops restricted to the zero-edges found and the edges the splits created.
    MeshBoundaryLoops Loops( m_Mesh, false );
    Loops.EdgeFilterFunc = [&OnCutEdges, &ZeroEdges]( int EID )
    { return OnCutEdges.contains( EID ) || ZeroEdges.contains( EID ); };
    if ( Loops.Compute() )
    {
        Boundary.CutLoops       = Loops.m_Loops;
        Boundary.CutSpans       = Loops.m_Spans;
        Boundary.CutLoopsFailed = false;
        Boundary.FoundOpenSpans = !Boundary.CutSpans.empty();
    }
    else
    {
        Boundary.CutLoops.clear();
        Boundary.CutLoopsFailed = true;
    }
    return !Boundary.CutLoopsFailed;
}

void MeshPlaneCut::CollapseDegenerateEdges( std::unordered_set<int>& Edges )
{
    const double Tol2      = m_DegenerateEdgeTol * m_DegenerateEdgeTol;
    int          Collapsed = 0;
    do
    {
        Collapsed = 0;
        for ( auto It = Edges.begin(); It != Edges.end(); )
        {
            const int  EID = *It;
            glm::dvec3 A;
            glm::dvec3 B;
            if ( !m_Mesh->IsEdge( EID ) || !m_Mesh->GetEdgeV( EID, A, B ) || glm::dot( A - B, A - B ) > Tol2 )
            {
                ++It;
                continue;
            }
            Index2i EV = m_Mesh->GetEdgeV( EID );
            // if the vertex we'd remove is on a seam, try removing the other one instead; both on seams: keep
            if ( m_Mesh->HasAttributes() && m_Mesh->Attributes()->IsSeamVertex( EV.B, false ) )
            {
                std::swap( EV.A, EV.B );
                if ( m_Mesh->Attributes()->IsSeamVertex( EV.B, false ) )
                {
                    ++It;
                    continue;
                }
            }
            DynamicMeshInfo::EdgeCollapseInfo CollapseInfo;
            if ( m_Mesh->CollapseEdge( EV.A, EV.B, CollapseInfo ) == MeshResult::Ok )
            {
                Collapsed++;
                It = Edges.erase( It );
            }
            else
                ++It;
        }
    } while ( Collapsed != 0 );
}

bool MeshPlaneCut::HoleFill( bool bFillSpans, int ConstantGroupID, int MaterialID )
{
    bool bAllOk = true;
    m_FailureReason.clear();
    m_HoleFillTriangles.clear();
    for ( const OpenBoundary& Boundary : m_OpenBoundaries )
    {
        std::vector<std::vector<int>> LoopVertices;
        for ( const EdgeLoop& Loop : Boundary.CutLoops )
            LoopVertices.push_back( Loop.Vertices );
        if ( bFillSpans )
            for ( const EdgeSpan& Span : Boundary.CutSpans )
                LoopVertices.push_back( Span.Vertices );

        const glm::dvec3 SignedPlaneNormal = m_PlaneNormal * static_cast<double>( Boundary.NormalSign );
        PlanarHoleFiller Filler( m_Mesh, &LoopVertices, m_PlaneOrigin, SignedPlaneNormal );
        const int        GID = ConstantGroupID >= 0 ? ConstantGroupID : m_Mesh->AllocateTriangleGroup();
        const bool       bFullyFilledHole = Filler.Fill( GID );

        if ( m_Mesh->HasAttributes() )
        {
            DynamicMeshEditor Editor( m_Mesh );
            Editor.SetTriangleNormals( Filler.m_NewTriangles, glm::vec3( SignedPlaneNormal ) );
            for ( int UVLayerIdx = 0, NumLayers = m_Mesh->Attributes()->NumUVLayers(); UVLayerIdx < NumLayers;
                  UVLayerIdx++ )
                Editor.SetTriangleUVsFromProjection( Filler.m_NewTriangles, m_PlaneOrigin, SignedPlaneNormal,
                                                     m_UVScaleFactor, UVLayerIdx );
            if ( MaterialID > -1 && m_Mesh->Attributes()->HasMaterialID() )
                for ( const int TID : Filler.m_NewTriangles )
                    m_Mesh->Attributes()->GetMaterialID()->SetValue( TID, MaterialID );
        }
        m_HoleFillTriangles.push_back( Filler.m_NewTriangles );
        if ( !bFullyFilledHole && m_FailureReason.empty() )
            m_FailureReason = Filler.m_FailureReason;
        bAllOk = bAllOk && bFullyFilledHole;
    }
    return bAllOk;
}
