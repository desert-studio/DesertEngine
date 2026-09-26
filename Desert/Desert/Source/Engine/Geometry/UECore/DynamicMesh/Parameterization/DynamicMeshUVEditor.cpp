// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Parameterization/
// DynamicMeshUVEditor.cpp:142-148, 184-196, 486-524, 543-630 and DynamicMesh3::GetVertexFrame
// (GeometryCore/Private/DynamicMesh/DynamicMesh3_Queries.cpp:824-850, bFrameNormalY = false, with a normal),
// adapted: GetVertexFrame is a local helper (UECore's DynamicMesh3 has no Frame3d); the submesh gets no vertex
// normals (UE's QuickComputeVertexNormals), MeshLocalParam computes the area-weighted normal per vertex instead;
// a triangle the submesh cannot append counts as failed. SetTriangleUVsFromFreeBoundarySpectralConformal is
// DynamicMeshUVEditor.cpp:754-988 with Options.bUseSpectral fixed true (the other branch is not ported).
// SetToPerVertexUVs is :198-222 without the UVEditResult; ScaleUVAreaTo3DArea is :1461-1497 with
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
        Frame3d GetVertexFrame( const DynamicMesh3& Mesh, int32_t VertexID, const glm::dvec3& UseNormal )
        {
            const glm::dvec3 v      = Mesh.GetVertex( VertexID );
            const glm::dvec3 normal = Normalized( UseNormal );
            int32_t          eid    = DynamicMesh3::InvalidID;
            for ( const int32_t VtxEdge : Mesh.VtxEdgesItr( VertexID ) )
            {
                eid = VtxEdge;
                break;
            }
            const Index2i    ev    = Mesh.GetEdgeV( eid );
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

    void DynamicMeshUVEditor::ResetUVs( const std::vector<int32_t>& Triangles )
    {
        m_UVOverlay->ClearElements( Triangles );
    }

    void
    DynamicMeshUVEditor::TransformUVElements( const std::vector<int32_t>&                         ElementIDs,
                                              const std::function<glm::vec2( const glm::vec2& )>& TransformFunc )
    {
        for ( const int32_t elemid : ElementIDs )
        {
            if ( m_UVOverlay->IsElement( elemid ) )
                m_UVOverlay->SetElement( elemid, TransformFunc( m_UVOverlay->GetElement( elemid ) ) );
        }
    }

    bool DynamicMeshUVEditor::EstimateGeodesicCenterFrameVertex( const DynamicMesh3& Mesh, Frame3d& FrameOut,
                                                                 int32_t& VertexIDOut, bool bAlignToUnitAxes )
    {
        VertexIDOut                     = *Mesh.VertexIndicesItr().begin();
        glm::dvec3              Normal  = MeshNormals::ComputeVertexNormal( Mesh, VertexIDOut );
        const MeshBoundaryLoops LoopsCalc( &Mesh, true );
        if ( LoopsCalc.GetLoopCount() == 0 )
        {
            FrameOut = GetVertexFrame( Mesh, VertexIDOut, Normal );
            return false;
        }
        const EdgeLoop* Loop = LoopsCalc.m_Loops.data();
        for ( const EdgeLoop& Candidate : LoopsCalc.m_Loops )
        {
            if ( static_cast<int32_t>( Candidate.Vertices.size() ) >
                 static_cast<int32_t>( Loop->Vertices.size() ) )
                Loop = &Candidate;
        }
        using Dijkstra = MeshDijkstra<DynamicMesh3>;
        std::vector<Dijkstra::SeedPoint> SeedPoints;
        SeedPoints.reserve( Loop->Vertices.size() );
        for ( const int32_t vid : Loop->Vertices )
            SeedPoints.push_back( Dijkstra::SeedPoint{ vid, vid, 0.0 } );
        Dijkstra Search( &Mesh );
        Search.ComputeToMaxDistance( SeedPoints, std::numeric_limits<float>::max() );
        const int32_t MaxDistVID = Search.GetMaxGraphDistancePointID();
        if ( !Mesh.IsVertex( MaxDistVID ) )
        {
            FrameOut = GetVertexFrame( Mesh, VertexIDOut, Normal );
            return false;
        }
        VertexIDOut = MaxDistVID;
        Normal      = MeshNormals::ComputeVertexNormal( Mesh, MaxDistVID );
        FrameOut    = GetVertexFrame( Mesh, MaxDistVID, Normal );
        if ( bAlignToUnitAxes ) // try to generate consistent frame alignment
            FrameOut.ConstrainedAlignPerpAxes( 0, 1, 2, glm::dvec3( 1, 0, 0 ), glm::dvec3( 0, 1, 0 ), 0.95 );
        return true;
    }

    bool DynamicMeshUVEditor::SetTriangleUVsFromExpMap( const std::vector<int32_t>& Triangles,
                                                        UVEditResult*               Result )
    {
        if ( m_UVOverlay == nullptr || Triangles.empty() )
            return false;
        ResetUVs( Triangles );

        DynamicSubmesh3     SubmeshCalc( m_Mesh, Triangles );
        const DynamicMesh3& Submesh = SubmeshCalc.GetSubmesh();
        if ( Submesh.TriangleCount() == 0 )
            return false;

        Frame3d    SeedFrame;
        int32_t    FrameVertexID = DynamicMesh3::InvalidID;
        const bool bFrameOK      = EstimateGeodesicCenterFrameVertex( Submesh, SeedFrame, FrameVertexID, true );
        if ( !Submesh.IsVertex( FrameVertexID ) )
            return false;

        MeshLocalParam<DynamicMesh3> Param( &Submesh );
        Param.m_ParamMode = LocalParamTypes::ExponentialMapUpwindAvg;
        Param.ComputeToMaxDistance( FrameVertexID, SeedFrame, std::numeric_limits<float>::max() );

        std::vector<int32_t> VtxElementIDs;
        std::vector<int32_t> NewElementIDs;
        VtxElementIDs.assign( Submesh.MaxVertexID(), DynamicMesh3::InvalidID );
        const double MaxFloat = std::numeric_limits<float>::max();
        for ( const int32_t vid : Submesh.VertexIndicesItr() )
        {
            if ( !Param.HasUV( vid ) )
                continue;
            const glm::dvec2 UVd = Param.GetUV( vid );
            const glm::vec2  UV( static_cast<float>( UVd.x > MaxFloat ? MaxFloat : UVd.x ),
                                 static_cast<float>( UVd.y > MaxFloat ? MaxFloat : UVd.y ) );
            VtxElementIDs[vid] = m_UVOverlay->AppendElement( UV );
            NewElementIDs.push_back( VtxElementIDs[vid] );
        }

        auto NumFailed = static_cast<int32_t>( SubmeshCalc.GetFailedTriangles().size() );
        for ( const int32_t tid : Submesh.TriangleIndicesItr() )
        {
            const Index3i SubTri = Submesh.GetTriangle( tid );
            const Index3i UVTri( VtxElementIDs[SubTri.A], VtxElementIDs[SubTri.B], VtxElementIDs[SubTri.C] );
            if ( UVTri.A == DynamicMesh3::InvalidID || UVTri.B == DynamicMesh3::InvalidID ||
                 UVTri.C == DynamicMesh3::InvalidID )
            {
                NumFailed++;
                continue;
            }
            m_UVOverlay->SetTriangle( SubmeshCalc.MapTriangleToBaseMesh( tid ), UVTri );
        }
        if ( Result != nullptr )
            Result->NewUVElements = std::move( NewElementIDs );
        // a fallback frame is always a failure (the quality would be very bad), as is any triangle left unset
        return bFrameOK && NumFailed == 0;
    }

    bool DynamicMeshUVEditor::SetTriangleUVsFromFreeBoundarySpectralConformal(
         const std::vector<int32_t>& Triangles, bool bUseExistingUVTopology, bool bPreserveIrregularity,
         UVEditResult* Result )
    {
        if ( m_UVOverlay == nullptr || Triangles.empty() )
            return false;
        if ( !bUseExistingUVTopology )
            ResetUVs( Triangles );

        DynamicMesh3                         Submesh;
        std::unordered_map<int32_t, int32_t> BaseToSubmeshV;
        std::vector<int32_t>                 SubmeshToBaseV;
        std::vector<int32_t>                 SubmeshToBaseT;
        for ( const int32_t tid : Triangles )
        {
            if ( bUseExistingUVTopology && !m_UVOverlay->IsSetTriangle( tid ) )
                continue;
            const Index3i Triangle =
                 bUseExistingUVTopology ? m_UVOverlay->GetTriangle( tid ) : m_Mesh->GetTriangle( tid );
            Index3i NewTriangle;
            for ( int32_t j = 0; j < 3; ++j )
            {
                const auto Found = BaseToSubmeshV.find( Triangle[j] );
                if ( Found != BaseToSubmeshV.end() )
                {
                    NewTriangle[j] = Found->second;
                    continue;
                }
                const glm::dvec3 Position = m_Mesh->GetVertex(
                     bUseExistingUVTopology ? m_UVOverlay->GetParentVertex( Triangle[j] ) : Triangle[j] );
                NewTriangle[j] = Submesh.AppendVertex( Position );
                SubmeshToBaseV.push_back( Triangle[j] );
                BaseToSubmeshV.emplace( Triangle[j], NewTriangle[j] );
            }
            if ( Submesh.AppendTriangle( NewTriangle ) < 0 )
                return false; // the UV topology is not a manifold submesh: nothing to parameterize
            SubmeshToBaseT.push_back( tid );
        }

        const MeshBoundaryLoops Loops( &Submesh, true );
        const EdgeLoop*         Longest = nullptr;
        for ( const EdgeLoop& Loop : Loops.m_Loops )
        {
            if ( Longest == nullptr || Loop.Vertices.size() > Longest->Vertices.size() )
                Longest = &Loop;
        }
        if ( Longest == nullptr )
            return false;
        SpectralConformalMeshUVSolver Solver( Submesh, bPreserveIrregularity );
        for ( const int32_t vid : Longest->Vertices )
            Solver.AddBoundaryVertex( vid );
        std::vector<glm::dvec2> UVBuffer;
        if ( !Solver.SolveUVs( UVBuffer ) )
            return false;

        if ( bUseExistingUVTopology )
        {
            for ( int32_t k = 0; k < static_cast<int32_t>( SubmeshToBaseV.size() ); ++k )
                m_UVOverlay->SetElement( SubmeshToBaseV[k], ToFloat( UVBuffer[k] ) );
            if ( Result != nullptr )
                Result->NewUVElements = std::move( SubmeshToBaseV );
            return true;
        }
        std::vector<int32_t> VtxElementIDs;
        std::vector<int32_t> NewElementIDs;
        VtxElementIDs.assign( Submesh.MaxVertexID(), DynamicMesh3::InvalidID );
        for ( const int32_t vid : Submesh.VertexIndicesItr() )
        {
            VtxElementIDs[vid] = m_UVOverlay->AppendElement( ToFloat( UVBuffer[vid] ) );
            NewElementIDs.push_back( VtxElementIDs[vid] );
        }
        for ( const int32_t tid : Submesh.TriangleIndicesItr() )
        {
            const Index3i SubTri = Submesh.GetTriangle( tid );
            m_UVOverlay->SetTriangle(
                 SubmeshToBaseT[tid],
                 Index3i( VtxElementIDs[SubTri.A], VtxElementIDs[SubTri.B], VtxElementIDs[SubTri.C] ) );
        }
        if ( Result != nullptr )
            Result->NewUVElements = std::move( NewElementIDs );
        return true;
    }

    void DynamicMeshUVEditor::SetToPerVertexUVs( std::vector<int32_t>& VertexToUVOut, bool& bIsIdentityMapOut )
    {
        bIsIdentityMapOut = true;
        VertexToUVOut.assign( m_Mesh->MaxVertexID(), DynamicMesh3::InvalidID );
        m_UVOverlay->ClearElements();
        for ( const int32_t VertexID : m_Mesh->VertexIndicesItr() )
        {
            const int32_t UVID      = m_UVOverlay->AppendElement( glm::vec2( 0.0f, 0.0f ) );
            VertexToUVOut[VertexID] = UVID;
            bIsIdentityMapOut       = bIsIdentityMapOut && UVID == VertexID;
        }
        for ( const int32_t TriangleID : m_Mesh->TriangleIndicesItr() )
        {
            const Index3i Tri = m_Mesh->GetTriangle( TriangleID );
            m_UVOverlay->SetTriangle(
                 TriangleID, Index3i( VertexToUVOut[Tri.A], VertexToUVOut[Tri.B], VertexToUVOut[Tri.C] ) );
        }
    }

    bool DynamicMeshUVEditor::ScaleUVAreaTo3DArea( const std::vector<int32_t>& Triangles, bool bRecenterAtOrigin,
                                                   float ScaleFactor )
    {
        double Area3D = 0.0;
        for ( const int32_t tid : Triangles )
        {
            if ( !m_Mesh->IsTriangle( tid ) )
                continue;
            const Index3i Tri = m_Mesh->GetTriangle( tid );
            Area3D += VectorUtil::Area( m_Mesh->GetVertex( Tri.A ), m_Mesh->GetVertex( Tri.B ),
                                        m_Mesh->GetVertex( Tri.C ) );
        }
        if ( std::abs( Area3D ) < std::numeric_limits<float>::epsilon() || !std::isfinite( Area3D ) )
            return false;

        std::unordered_set<int32_t> Elements;
        double                      Area2D = 0.0;
        glm::vec2 BoundsMin( std::numeric_limits<float>::max(), std::numeric_limits<float>::max() );
        glm::vec2 BoundsMax( -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() );
        for ( const int32_t tid : Triangles )
        {
            if ( !m_UVOverlay->IsSetTriangle( tid ) )
                continue;
            const Index3i                  UVTri = m_UVOverlay->GetTriangle( tid );
            const std::array<glm::vec2, 3> UV    = { m_UVOverlay->GetElement( UVTri.A ),
                                                     m_UVOverlay->GetElement( UVTri.B ),
                                                     m_UVOverlay->GetElement( UVTri.C ) };
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
        if ( Elements.empty() || std::abs( Area2D ) < std::numeric_limits<float>::epsilon() ||
             !std::isfinite( Area2D ) )
            return false;

        const double UVScale = ScaleFactor * std::sqrt( Area3D ) / std::sqrt( Area2D );
        if ( !std::isfinite( UVScale ) )
            return false;
        const glm::vec2 ScaleOrigin = ( BoundsMin + BoundsMax ) * 0.5f;
        const glm::vec2 Translation = bRecenterAtOrigin ? glm::vec2( 0.0f, 0.0f ) : ScaleOrigin;
        for ( const int32_t eid : Elements )
            m_UVOverlay->SetElement(
                 eid,
                 ( m_UVOverlay->GetElement( eid ) - ScaleOrigin ) * static_cast<float>( UVScale ) + Translation );
        return true;
    }
} // namespace Desert::Geometry
