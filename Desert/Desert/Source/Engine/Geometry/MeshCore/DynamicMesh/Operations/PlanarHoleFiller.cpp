// Ported from UE 5.8 .../DynamicMesh/Private/Operations/PlanarHoleFiller.cpp and Public/Operations/HoleFillUtil.h
// (see the header for the line ranges and what was replaced).
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/PlanarHoleFiller.hpp"

#include "Engine/Geometry/MeshCore/CompGeom/ConstrainedDelaunay2.hpp"
#include "Engine/Geometry/MeshCore/Curve/PlanarComplex.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"

#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <unordered_map>

using namespace Desert::Geometry;

namespace
{
    // UE VectorUtil::MakePerpVectors (VectorUtil.h, Duff et al. 2017): two unit vectors completing a right-handed
    // frame around the unit Normal.
    void MakePerpVectors( const glm::dvec3& Normal, glm::dvec3& OutX, glm::dvec3& OutY )
    {
        const double Sign = std::copysign( 1.0, Normal.z );
        const double A    = -1.0 / ( Sign + Normal.z );
        const double B    = Normal.x * Normal.y * A;
        OutX              = glm::dvec3( 1.0 + Sign * Normal.x * Normal.x * A, Sign * B, -Sign * Normal.x );
        OutY              = glm::dvec3( B, Sign + Normal.y * Normal.y * A, -Normal.y );
    }

    // HoleFillUtil::FillOverlayElements for vertex loops and no new vertex: each fill triangle takes, per corner,
    // the colour element the loop edge's existing triangle has at that vertex.
    void FillColorOverlay( DynamicMesh3& Mesh, const std::vector<std::vector<int>>& Loops,
                           const std::vector<int>& NewTriangles )
    {
        if ( !Mesh.HasAttributes() || !Mesh.Attributes()->HasPrimaryColors() )
            return;
        DynamicMeshColorOverlay*     Overlay = Mesh.Attributes()->PrimaryColors();
        std::unordered_map<int, int> VIDToElementID;
        for ( const std::vector<int>& Loop : Loops )
        {
            const int N = static_cast<int>( Loop.size() );
            for ( int Idx = 0, PrevIdx = N - 1; Idx < N; PrevIdx = Idx++ )
            {
                const int EID = Mesh.FindEdge( Loop[Idx], Loop[PrevIdx] );
                if ( EID == IndexConstants::InvalidID )
                    continue;
                const DynamicMesh3::Edge Edge      = Mesh.GetEdge( EID );
                int                      SourceTID = IndexConstants::InvalidID;
                if ( Overlay->IsSetTriangle( Edge.Tri.A ) )
                    SourceTID = Edge.Tri.A;
                else if ( Mesh.IsTriangle( Edge.Tri.B ) && Overlay->IsSetTriangle( Edge.Tri.B ) )
                    SourceTID = Edge.Tri.B;
                if ( SourceTID == IndexConstants::InvalidID )
                    continue;
                const Index3i SetTri   = Mesh.GetTriangle( SourceTID );
                const Index3i SetElTri = Overlay->GetTriangle( SourceTID );
                for ( int SubIdx = 0; SubIdx < 2; ++SubIdx )
                    VIDToElementID.insert_or_assign( Edge.Vert[SubIdx],
                                                     SetElTri[SetTri.IndexOf( Edge.Vert[SubIdx] )] );
            }
        }
        for ( const int NewTID : NewTriangles )
        {
            const Index3i NewTri = Mesh.GetTriangle( NewTID );
            Index3i       NewElTri;
            bool          bAllFound = true;
            for ( int SubIdx = 0; SubIdx < 3 && bAllFound; ++SubIdx )
            {
                const auto Found = VIDToElementID.find( NewTri[SubIdx] );
                bAllFound        = Found != VIDToElementID.end();
                if ( bAllFound )
                    NewElTri[SubIdx] = Found->second;
            }
            if ( bAllFound )
                Overlay->SetTriangle( NewTID, NewElTri );
        }
    }
} // namespace

bool PlanarHoleFiller::Fill( int GroupID )
{
    m_FailureReason.clear();
    m_NewTriangles.clear();
    if ( GroupID < 0 && m_Mesh->HasTriangleGroups() )
        GroupID = m_Mesh->AllocateTriangleGroup();

    glm::dvec3 Normal = m_PlaneNormal;
    Normalize( Normal );
    glm::dvec3 PlaneX;
    glm::dvec3 PlaneY;
    MakePerpVectors( Normal, PlaneX, PlaneY );

    const std::vector<std::vector<int>>& Loops = *m_VertexLoops;
    PlanarComplex                        Complex;
    for ( const std::vector<int>& Loop : Loops )
    {
        Polygon2& Polygon = Complex.m_Polygons.emplace_back();
        for ( const int VID : Loop )
        {
            const glm::dvec3 VertMinusOrigin = m_Mesh->GetVertex( VID ) - m_PlaneOrigin;
            Polygon.emplace_back( glm::dot( PlaneX, VertMinusOrigin ), glm::dot( PlaneY, VertMinusOrigin ) );
        }
    }
    Complex.m_bTrustOrientations    = true;
    Complex.m_bAllowOverlappingHoles = false;
    Complex.FindSolidRegions();

    bool bAddedAll = true;
    for ( const PlanarComplex::PolygonNesting& Nest : Complex.m_Solids )
    {
        const GeneralPolygon2      GPoly     = Complex.ConvertNestToGeneralPolygon( Nest );
        const std::vector<Index3i> Triangles = ConstrainedDelaunayTriangulate( GPoly );
        if ( Triangles.empty() )
        {
            if ( bAddedAll )
                m_FailureReason = fmt::format( "the section of loop {} ({} vertices, {} holes) did not triangulate",
                                               Nest.OuterIndex, Loops[Nest.OuterIndex].size(),
                                               Nest.HoleIndices.size() );
            bAddedAll = false;
            continue;
        }
        // the local triangulation's vertices -> mesh vertex IDs: the outer loop, then each hole loop
        std::vector<int> VertexMapping = Loops[Nest.OuterIndex];
        for ( const int HoleIdx : Nest.HoleIndices )
            VertexMapping.insert( VertexMapping.end(), Loops[HoleIdx].begin(), Loops[HoleIdx].end() );
        for ( const Index3i& SourceTri : Triangles )
        {
            const Index3i OutTri( VertexMapping[SourceTri.A], VertexMapping[SourceTri.B], VertexMapping[SourceTri.C] );
            const int     NewTID = m_Mesh->AppendTriangle( OutTri, GroupID );
            // e.g. a triangle that would make the mesh non-manifold is skipped, as in UE
            if ( NewTID < 0 )
            {
                if ( bAddedAll )
                    m_FailureReason = fmt::format( "a fill triangle of loop {} ({}, {}, {}) could not join the mesh "
                                                   "(error {})",
                                                   Nest.OuterIndex, OutTri.A, OutTri.B, OutTri.C, NewTID );
                bAddedAll = false;
            }
            else
                m_NewTriangles.push_back( NewTID );
        }
    }

    if ( m_bAutoFillPrimaryColors )
        FillColorOverlay( *m_Mesh, Loops, m_NewTriangles );
    return bAddedAll;
}
