// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Parameterization/
// DynamicMeshUVEditor.cpp:142-148, 184-196, 486-524, 543-630 and DynamicMesh3::GetVertexFrame
// (GeometryCore/Private/DynamicMesh/DynamicMesh3_Queries.cpp:824-850, bFrameNormalY = false, with a normal),
// adapted: GetVertexFrame is a local helper (MeshCore's DynamicMesh3 has no Frame3d); the submesh gets no vertex
// normals (UE's QuickComputeVertexNormals), MeshLocalParam computes the area-weighted normal per vertex instead;
// a triangle the submesh cannot append counts as failed. SetTriangleUVsFromFreeBoundarySpectralConformal is
// DynamicMeshUVEditor.cpp:754-988 with Options.bUseSpectral fixed true (the other branch is not ported).
// SetToPerVertexUVs is :198-222 without the UVEditResult; ScaleUVAreaTo3DArea is :1461-1497 with
// DetermineAreaFromUVs (:1743-1768) and GetVolumeArea's area written out.
#include "Engine/Geometry/MeshCore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/DynamicSubmesh3.hpp"
#include "Engine/Geometry/MeshCore/MeshBoundaryLoops.hpp"
#include "Engine/Geometry/MeshCore/Parameterization/MeshDijkstra.hpp"
#include "Engine/Geometry/MeshCore/Parameterization/MeshLocalParam.hpp"
#include "Engine/Geometry/MeshCore/Parameterization/UVPacking.hpp"
#include "Engine/Geometry/MeshCore/Selections/MeshConnectedComponents.hpp"
#include "Engine/Geometry/MeshCore/Solvers/MeshUVSolver.hpp"
#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
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
        VertexIDOut                    = *Mesh.VertexIndicesItr().begin();
        glm::dvec3              Normal = MeshNormals::ComputeVertexNormal( Mesh, VertexIDOut );
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
    namespace
    {
        glm::dvec3 ToFramePoint( const Frame3d& Frame, const glm::dvec3& P )
        {
            const glm::dvec3 D = P - Frame.Origin;
            return { glm::dot( D, Frame.X() ), glm::dot( D, Frame.Y() ), glm::dot( D, Frame.Z() ) };
        }

        glm::dvec3 ToFrameVector( const Frame3d& Frame, const glm::dvec3& V )
        {
            return { glm::dot( V, Frame.X() ), glm::dot( V, Frame.Y() ), glm::dot( V, Frame.Z() ) };
        }

        double InverseOrOne( double Dimension )
        {
            return std::abs( Dimension ) > static_cast<double>( std::numeric_limits<float>::epsilon() )
                        ? 1.0 / Dimension
                        : 1.0;
        }

        uint64_t ElementKey( int32_t VertexID, int32_t Bucket )
        {
            return ( static_cast<uint64_t>( static_cast<uint32_t>( VertexID ) ) << 8 ) |
                   static_cast<uint64_t>( static_cast<uint32_t>( Bucket ) & 0xffu );
        }

        double Sign( double V )
        {
            return V > 0.0 ? 1.0 : ( V < 0.0 ? -1.0 : 0.0 );
        }

        // FConvexHull2f::Solve's answer for the polygon: the hull's point indices, counter-clockwise, no
        // repeated end point (Andrew's monotone chain). Empty when the points are all collinear.
        std::vector<int32_t> ConvexHullIndices( const std::vector<glm::vec2>& Points )
        {
            std::vector<int32_t> Order( Points.size() );
            for ( size_t i = 0; i < Points.size(); ++i )
                Order[i] = static_cast<int32_t>( i );
            std::sort( Order.begin(), Order.end(),
                       [&]( int32_t A, int32_t B )
                       { return Points[A].x < Points[B].x || ( Points[A].x == Points[B].x && Points[A].y < Points[B].y ); } );
            auto Cross = [&]( int32_t O, int32_t A, int32_t B )
            {
                const glm::dvec2 OA = glm::dvec2( Points[A] - Points[O] );
                const glm::dvec2 OB = glm::dvec2( Points[B] - Points[O] );
                return OA.x * OB.y - OA.y * OB.x;
            };
            std::vector<int32_t> Hull( 2 * Order.size() );
            size_t               k = 0;
            for ( const int32_t i : Order )
            {
                while ( k >= 2 && Cross( Hull[k - 2], Hull[k - 1], i ) <= 0.0 )
                    --k;
                Hull[k++] = i;
            }
            for ( size_t j = Order.size(), t = k + 1; j-- > 0; )
            {
                const int32_t i = Order[j];
                while ( k >= t && Cross( Hull[k - 2], Hull[k - 1], i ) <= 0.0 )
                    --k;
                Hull[k++] = i;
            }
            Hull.resize( k > 0 ? k - 1 : 0 );
            if ( Hull.size() < 3 )
                Hull.clear();
            return Hull;
        }
    } // namespace

    void DynamicMeshUVEditor::SetTriangleUVsFromPlanarProjection(
         const std::vector<int32_t>& Triangles, const std::function<glm::dvec3( const glm::dvec3& )>& PointTransform,
         const Frame3d& ProjectionFrame, const glm::dvec2& Dimensions, UVEditResult* Result )
    {
        if ( m_UVOverlay == nullptr || Triangles.empty() )
            return;
        ResetUVs( Triangles );

        const double ScaleX = InverseOrOne( Dimensions.x );
        const double ScaleY = InverseOrOne( Dimensions.y );

        std::unordered_map<int32_t, int32_t> BaseToOverlayVIDMap;
        std::vector<int32_t>                 NewUVIndices;
        for ( const int32_t TID : Triangles )
        {
            const Index3i BaseTri = m_Mesh->GetTriangle( TID );
            Index3i       ElemTri;
            for ( int32_t j = 0; j < 3; ++j )
            {
                const auto Found = BaseToOverlayVIDMap.find( BaseTri[j] );
                if ( Found == BaseToOverlayVIDMap.end() )
                {
                    const glm::dvec3 Local = ToFramePoint( ProjectionFrame, PointTransform( m_Mesh->GetVertex( BaseTri[j] ) ) );
                    const glm::vec2  UV( static_cast<float>( Local.x * ScaleX ), static_cast<float>( Local.y * ScaleY ) );
                    ElemTri[j] = m_UVOverlay->AppendElement( UV );
                    NewUVIndices.push_back( ElemTri[j] );
                    BaseToOverlayVIDMap.emplace( BaseTri[j], ElemTri[j] );
                }
                else
                    ElemTri[j] = Found->second;
            }
            m_UVOverlay->SetTriangle( TID, ElemTri );
        }
        if ( Result != nullptr )
            Result->NewUVElements = std::move( NewUVIndices );
    }

    void DynamicMeshUVEditor::SetTriangleUVsFromBoxProjection(
         const std::vector<int32_t>& Triangles, const std::function<glm::dvec3( const glm::dvec3& )>& PointTransform,
         const Frame3d& BoxFrame, const glm::dvec3& BoxDimensions, UVEditResult* Result )
    {
        const size_t NumTriangles = Triangles.size();
        if ( m_UVOverlay == nullptr || NumTriangles == 0 )
            return;
        ResetUVs( Triangles );

        constexpr int Minor1s[3]    = { 1, 0, 0 };
        constexpr int Minor2s[3]    = { 2, 2, 1 };
        constexpr int Minor1Flip[3] = { -1, 1, 1 };
        constexpr int Minor2Flip[3] = { -1, -1, 1 };

        const glm::dvec3 Scale( InverseOrOne( BoxDimensions.x ), InverseOrOne( BoxDimensions.y ),
                                InverseOrOne( BoxDimensions.z ) );

        // Assign each triangle to a box plane by its normal
        std::vector<glm::dvec3> TriNormals( NumTriangles );
        std::vector<Index2i>    TriangleBoxPlaneAssignments( NumTriangles );
        for ( size_t i = 0; i < NumTriangles; ++i )
        {
            glm::dvec3 A, B, C;
            m_Mesh->GetTriVertices( Triangles[i], A, B, C );
            TriNormals[i]                  = VectorUtil::Normal( PointTransform( A ), PointTransform( B ), PointTransform( C ) );
            const glm::dvec3 ScaledNormal  = ToFrameVector( BoxFrame, TriNormals[i] ) * Scale;
            const glm::dvec3 NAbs          = glm::abs( ScaledNormal );
            const int        MajorAxis     = NAbs[0] > NAbs[1] ? ( NAbs[0] > NAbs[2] ? 0 : 2 ) : ( NAbs[1] > NAbs[2] ? 1 : 2 );
            const int        Bucket        = Sign( ScaledNormal[MajorAxis] ) > 0 ? MajorAxis + 3 : MajorAxis;
            TriangleBoxPlaneAssignments[i] = Index2i( MajorAxis, Bucket );
        }

        std::unordered_map<uint64_t, int32_t> BaseToOverlayVIDMap;
        std::vector<int32_t>                  NewUVIndices;
        for ( size_t i = 0; i < NumTriangles; ++i )
        {
            const int32_t    tid           = Triangles[i];
            const Index3i    BaseTri       = m_Mesh->GetTriangle( tid );
            const glm::dvec3 N             = ToFrameVector( BoxFrame, TriNormals[i] );
            const int        MajorAxis     = TriangleBoxPlaneAssignments[i].A;
            const int        Bucket        = TriangleBoxPlaneAssignments[i].B;
            const int        MajorAxisSign = static_cast<int>( Sign( N[MajorAxis] ) );
            const int        Minor1        = Minor1s[MajorAxis];
            const int        Minor2        = Minor2s[MajorAxis];

            Index3i ElemTri;
            for ( int32_t j = 0; j < 3; ++j )
            {
                const uint64_t Key   = ElementKey( BaseTri[j], Bucket );
                const auto     Found = BaseToOverlayVIDMap.find( Key );
                if ( Found == BaseToOverlayVIDMap.end() )
                {
                    const glm::dvec3 BoxPos = ToFramePoint( BoxFrame, PointTransform( m_Mesh->GetVertex( BaseTri[j] ) ) ) * Scale;
                    const glm::vec2  UV( static_cast<float>( BoxPos[Minor1] ) * static_cast<float>( MajorAxisSign * Minor1Flip[MajorAxis] ),
                                         static_cast<float>( BoxPos[Minor2] ) * static_cast<float>( Minor2Flip[MajorAxis] ) );
                    ElemTri[j] = m_UVOverlay->AppendElement( UV );
                    NewUVIndices.push_back( ElemTri[j] );
                    BaseToOverlayVIDMap.emplace( Key, ElemTri[j] );
                }
                else
                    ElemTri[j] = Found->second;
            }
            m_UVOverlay->SetTriangle( tid, ElemTri );
        }

        // The buckets can make bowties: split them on the new elements
        SplitBowtiesOnUVElements( NewUVIndices, true );
        if ( Result != nullptr )
            Result->NewUVElements = std::move( NewUVIndices );
    }

    void DynamicMeshUVEditor::SetTriangleUVsFromCylinderProjection(
         const std::vector<int32_t>& Triangles, const std::function<glm::dvec3( const glm::dvec3& )>& PointTransform,
         const Frame3d& BoxFrame, const glm::dvec3& BoxDimensions, float CylinderSplitAngle, UVEditResult* Result )
    {
        const size_t NumTriangles = Triangles.size();
        if ( m_UVOverlay == nullptr || NumTriangles == 0 )
            return;
        ResetUVs( Triangles );

        constexpr int Minor1Flip[3] = { -1, 1, 1 };
        constexpr int Minor2Flip[3] = { -1, -1, 1 };

        const glm::dvec3 Scale( InverseOrOne( BoxDimensions.x ), InverseOrOne( BoxDimensions.y ),
                                InverseOrOne( BoxDimensions.z ) );
        const double DotThresholdRejectFromPlane = std::cos( CylinderSplitAngle * std::numbers::pi / 180.0 );

        // Buckets: 1/0 is +/-Z, 3/4 the negative/positive angle around the cylinder ([-180, 0] and [0, 180]
        // each get their own island).
        std::vector<glm::dvec3> TriNormals( NumTriangles );
        std::vector<Index2i>    TriangleCylinderAssignments( NumTriangles );
        for ( size_t i = 0; i < NumTriangles; ++i )
        {
            glm::dvec3 A, B, C;
            m_Mesh->GetTriVertices( Triangles[i], A, B, C );
            A             = PointTransform( A );
            B             = PointTransform( B );
            C             = PointTransform( C );
            TriNormals[i] = VectorUtil::Normal( A, B, C );
            const glm::dvec3 N = Normalized( ToFrameVector( BoxFrame, TriNormals[i] ) * Scale );
            if ( std::abs( N.z ) > DotThresholdRejectFromPlane )
                TriangleCylinderAssignments[i] = Index2i( 2, Sign( N.z ) > 0 ? 1 : 0 );
            else
            {
                const glm::dvec3 Centroid      = ToFramePoint( BoxFrame, ( A + B + C ) / 3.0 );
                const double     CentroidAngle = std::atan2( Centroid.y, Centroid.x );
                TriangleCylinderAssignments[i] = Index2i( -1, CentroidAngle < 0 ? 3 : 4 );
            }
        }

        std::unordered_map<uint64_t, int32_t> BaseToOverlayVIDMap;
        std::vector<int32_t>                  NewUVIndices;
        for ( size_t i = 0; i < NumTriangles; ++i )
        {
            const int32_t    tid       = Triangles[i];
            const Index3i    BaseTri   = m_Mesh->GetTriangle( tid );
            const glm::dvec3 N         = ToFrameVector( BoxFrame, TriNormals[i] );
            const int        MajorAxis = TriangleCylinderAssignments[i].A;
            const int        Bucket    = TriangleCylinderAssignments[i].B;

            Index3i ElemTri;
            for ( int32_t j = 0; j < 3; ++j )
            {
                const uint64_t Key   = ElementKey( BaseTri[j], Bucket );
                const auto     Found = BaseToOverlayVIDMap.find( Key );
                if ( Found == BaseToOverlayVIDMap.end() )
                {
                    const glm::dvec3 BoxPos = Scale * ToFramePoint( BoxFrame, PointTransform( m_Mesh->GetVertex( BaseTri[j] ) ) );
                    glm::vec2        UV( 0.0f );
                    if ( Bucket <= 2 )
                    {
                        const int MajorAxisSign = static_cast<int>( Sign( N[MajorAxis] ) );
                        UV = glm::vec2( static_cast<float>( BoxPos.x ) * static_cast<float>( MajorAxisSign * Minor1Flip[MajorAxis] ),
                                        static_cast<float>( BoxPos.y ) * static_cast<float>( Minor2Flip[MajorAxis] ) );
                    }
                    else
                    {
                        double VAngle = std::atan2( BoxPos.y, BoxPos.x );
                        if ( Bucket == 4 && VAngle < -std::numbers::pi / 2.0 ) // 4 = [0, 180]
                            VAngle += 2.0 * std::numbers::pi;
                        else if ( Bucket == 3 && VAngle > std::numbers::pi / 2.0 ) // 3 = [-180, 0]
                            VAngle -= 2.0 * std::numbers::pi;
                        UV = glm::vec2( -( static_cast<float>( VAngle / std::numbers::pi ) - 1.0f ),
                                        -static_cast<float>( BoxPos.z ) );
                    }
                    ElemTri[j] = m_UVOverlay->AppendElement( UV );
                    NewUVIndices.push_back( ElemTri[j] );
                    BaseToOverlayVIDMap.emplace( Key, ElemTri[j] );
                }
                else
                    ElemTri[j] = Found->second;
            }
            m_UVOverlay->SetTriangle( tid, ElemTri );
        }

        SplitBowtiesOnUVElements( NewUVIndices, true );
        if ( Result != nullptr )
            Result->NewUVElements = std::move( NewUVIndices );
    }

    void DynamicMeshUVEditor::SplitBowtiesOnUVElements( std::vector<int32_t>& UVElementIDs,
                                                        bool                  bAddNewElementsToInputArray )
    {
        if ( m_UVOverlay == nullptr )
            return;
        const size_t InitialNumElements = UVElementIDs.size();
        for ( size_t Idx = 0; Idx < InitialNumElements; ++Idx )
        {
            const int32_t ParentVID = m_UVOverlay->GetParentVertex( UVElementIDs[Idx] );
            if ( m_UVOverlay->IsBowtieInOverlay( ParentVID ) )
                m_UVOverlay->SplitBowtiesAtVertex( ParentVID, bAddNewElementsToInputArray ? &UVElementIDs : nullptr );
        }
    }

    bool DynamicMeshUVEditor::AutoOrientUVArea( const std::vector<int32_t>& Triangles )
    {
        std::unordered_set<int32_t> Elements;
        std::vector<glm::vec2>      UVs;
        glm::vec2                   BoundsMin( std::numeric_limits<float>::max() );
        glm::vec2                   BoundsMax( -std::numeric_limits<float>::max() );
        for ( const int32_t tid : Triangles )
        {
            if ( !m_UVOverlay->IsSetTriangle( tid ) )
                continue;
            const Index3i UVTri = m_UVOverlay->GetTriangle( tid );
            for ( int32_t j = 0; j < 3; ++j )
                if ( Elements.insert( UVTri[j] ).second )
                {
                    const glm::vec2 UV = m_UVOverlay->GetElement( UVTri[j] );
                    UVs.push_back( UV );
                    BoundsMin = glm::min( BoundsMin, UV );
                    BoundsMax = glm::max( BoundsMax, UV );
                }
        }
        if ( UVs.empty() )
            return false;

        // Shift to the origin so the subtraction below can be skipped
        const glm::vec2 BoxCenter = ( BoundsMin + BoundsMax ) * 0.5f;
        for ( glm::vec2& UV : UVs )
            UV -= BoxCenter;

        const std::vector<int32_t> HullPointIndices = ConvexHullIndices( UVs );
        if ( HullPointIndices.empty() )
            return false;
        std::vector<glm::vec2> HullPoints;
        HullPoints.reserve( HullPointIndices.size() );
        for ( const int32_t k : HullPointIndices )
            HullPoints.push_back( UVs[k] );
        const int32_t NV = static_cast<int32_t>( HullPoints.size() );

        // Brute-force rotating calipers
        const glm::vec2 Extent            = BoundsMax - BoundsMin;
        double          MinBoxArea        = static_cast<double>( Extent.x ) * Extent.y;
        glm::vec2       MinAxisDirection  = glm::vec2( 1.0f, 0.0f );
        bool            bFoundSmallerBox  = false;
        for ( int32_t j = 0; j <= NV; ++j )
        {
            const glm::vec2 A = HullPoints[j % NV], B = HullPoints[( j + 1 ) % NV], C = HullPoints[( j + NV / 2 ) % NV];
            glm::vec2       Axis0      = B - A;
            float           Dimension0 = glm::length( Axis0 );
            if ( Dimension0 <= 0.0f )
                continue;
            Axis0 /= Dimension0;
            const glm::vec2 Axis1( Axis0.y, -Axis0.x ); // PerpCW
            float           Dimension1 = glm::dot( C - A, Axis1 );

            float Min0 = std::numeric_limits<float>::max(), Max0 = -std::numeric_limits<float>::max();
            float Min1 = std::numeric_limits<float>::max(), Max1 = -std::numeric_limits<float>::max();
            bool  bAbort = false;
            for ( const glm::vec2& P : HullPoints )
            {
                const float D0 = glm::dot( Axis0, P );
                const float D1 = glm::dot( Axis1, P );
                Min0           = std::min( Min0, D0 );
                Max0           = std::max( Max0, D0 );
                Min1           = std::min( Min1, D1 );
                Max1           = std::max( Max1, D1 );
                Dimension0     = std::max( Dimension0, Max0 - Min0 );
                Dimension1     = std::max( Dimension1, Max1 - Min1 );
                if ( static_cast<double>( Dimension0 ) * Dimension1 > MinBoxArea )
                {
                    bAbort = true;
                    break;
                }
            }
            if ( !bAbort && static_cast<double>( Dimension0 ) * Dimension1 < MinBoxArea )
            {
                MinBoxArea       = static_cast<double>( Dimension0 ) * Dimension1;
                MinAxisDirection = Axis0;
                bFoundSmallerBox = true;
            }
        }

        if ( bFoundSmallerBox )
        {
            const float RotationAngle = std::atan2( MinAxisDirection.y, MinAxisDirection.x );
            const float c = std::cos( -RotationAngle ), s = std::sin( -RotationAngle );
            for ( const int32_t eid : Elements )
            {
                const glm::vec2 UV = m_UVOverlay->GetElement( eid ) - BoxCenter;
                m_UVOverlay->SetElement( eid, glm::vec2( c * UV.x - s * UV.y, s * UV.x + c * UV.y ) + BoxCenter );
            }
        }
        return true;
    }

    namespace
    {
        // MeshUVPacking.cpp's FUVOverlayView.
        class UVOverlayView final : public UVPacker::IUVMeshView
        {
        public:
            UVOverlayView( const DynamicMesh3* Mesh, DynamicMeshUVOverlay* Overlay ) : m_Mesh( Mesh ), m_Overlay( Overlay )
            {
            }
            Index3i GetTriangle( int32_t TID ) const override
            {
                return m_Mesh->GetTriangle( TID );
            }
            Index3i GetUVTriangle( int32_t TID ) const override
            {
                return m_Overlay->GetTriangle( TID );
            }
            glm::dvec3 GetVertex( int32_t VID ) const override
            {
                return m_Mesh->GetVertex( VID );
            }
            glm::vec2 GetUV( int32_t EID ) const override
            {
                return m_Overlay->GetElement( EID );
            }
            void SetUV( int32_t EID, glm::vec2 UV ) override
            {
                m_Overlay->SetElement( EID, UV );
            }

        private:
            const DynamicMesh3*   m_Mesh;
            DynamicMeshUVOverlay* m_Overlay;
        };
    } // namespace

    bool DynamicMeshUVEditor::QuickPack( int32_t TargetTextureResolution, float GutterSize )
    {
        // Always split bowties before packing
        m_UVOverlay->SplitBowties();

        UVPacker Packer;
        Packer.TextureResolution = TargetTextureResolution;
        Packer.GutterSize        = GutterSize;
        Packer.bAllowFlips       = false;

        // FDynamicMeshUVPacker::CollectUVIslandsToPack: the islands are the UV-connected triangle components.
        MeshConnectedComponents UVComponents( m_Mesh );
        std::vector<int>        AllTriangles;
        for ( const int tid : m_Mesh->TriangleIndicesItr() )
            if ( m_UVOverlay->IsSetTriangle( tid ) )
                AllTriangles.push_back( tid );
        UVComponents.FindConnectedTriangles( AllTriangles, [this]( int32_t T0, int32_t T1 )
                                             { return m_UVOverlay->AreTrianglesConnected( T0, T1 ); } );
        UVOverlayView View( m_Mesh, m_UVOverlay );
        return Packer.StandardPack( &View, UVComponents.Num(), [&UVComponents]( int32_t Idx, std::vector<int32_t>& Island )
                                    { Island.assign( UVComponents.GetComponent( Idx ).Indices.begin(),
                                                     UVComponents.GetComponent( Idx ).Indices.end() ); } );
    }
} // namespace Desert::Geometry
