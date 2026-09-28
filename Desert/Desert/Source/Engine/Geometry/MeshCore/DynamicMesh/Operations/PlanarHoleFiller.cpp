// Ported from UE 5.8 .../DynamicMesh/Private/Operations/PlanarHoleFiller.cpp and Public/Operations/HoleFillUtil.h
// (see the header for the line ranges and what was replaced).
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/PlanarHoleFiller.hpp"

#include "Engine/Geometry/MeshCore/CompGeom/PolygonTriangulation.hpp"
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

    // Crossing-number test of Point against the closed polygon Poly (both in the plane frame). Stands in for
    // FPlanarComplex::FindSolidRegions' nesting test, which P14b ports.
    bool PointInPolygon( const glm::dvec2& Point, const std::vector<glm::dvec2>& Poly )
    {
        bool      bInside = false;
        const int N       = static_cast<int>( Poly.size() );
        for ( int I = 0, J = N - 1; I < N; J = I++ )
        {
            const glm::dvec2& A = Poly[I];
            const glm::dvec2& B = Poly[J];
            if ( ( A.y > Point.y ) != ( B.y > Point.y ) &&
                 Point.x < ( B.x - A.x ) * ( Point.y - A.y ) / ( B.y - A.y ) + A.x )
                bInside = !bInside;
        }
        return bInside;
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
    std::vector<std::vector<glm::dvec2>> Polygons( Loops.size() );
    for ( size_t L = 0; L < Loops.size(); ++L )
        for ( const int VID : Loops[L] )
        {
            const glm::dvec3 VertMinusOrigin = m_Mesh->GetVertex( VID ) - m_PlaneOrigin;
            Polygons[L].emplace_back( glm::dot( PlaneX, VertMinusOrigin ), glm::dot( PlaneY, VertMinusOrigin ) );
        }
    for ( size_t Inner = 0; Inner < Loops.size(); ++Inner )
        for ( size_t Outer = 0; Outer < Loops.size(); ++Outer )
            if ( Inner != Outer && !Polygons[Inner].empty() &&
                 PointInPolygon( Polygons[Inner][0], Polygons[Outer] ) )
            {
                m_FailureReason = fmt::format(
                     "the section has a hole: loop {} ({} vertices) lies inside loop {} ({} vertices); filling a "
                     "section with holes is a limitation of this port (v1) - UE fills it through "
                     "ConstrainedDelaunay2 / PlanarComplex, ported by P14b",
                     Inner, Loops[Inner].size(), Outer, Loops[Outer].size() );
                return false;
            }

    bool                 bAddedAll = true;
    std::vector<Index3i> Triangles;
    for ( size_t L = 0; L < Loops.size(); ++L )
    {
        const std::vector<int>& Loop = Loops[L];
        std::vector<glm::dvec3> Positions;
        Positions.reserve( Loop.size() );
        for ( const int VID : Loop )
            Positions.push_back( m_Mesh->GetVertex( VID ) );
        Triangles.clear();
        PolygonTriangulation::TriangulateSimplePolygon( Positions, Triangles, true );
        if ( Triangles.empty() )
        {
            m_FailureReason = fmt::format( "loop {} ({} vertices) did not triangulate", L, Loop.size() );
            return false;
        }
        for ( const Index3i& SourceTri : Triangles )
        {
            const Index3i OutTri( Loop[SourceTri.A], Loop[SourceTri.B], Loop[SourceTri.C] );
            const int     NewTID = m_Mesh->AppendTriangle( OutTri, GroupID );
            // e.g. a triangle that would make the mesh non-manifold is skipped, as in UE
            if ( NewTID < 0 )
            {
                if ( bAddedAll )
                    m_FailureReason =
                         fmt::format( "a fill triangle of loop {} ({}, {}, {}) could not join the mesh "
                                      "(error {})",
                                      L, OutTri.A, OutTri.B, OutTri.C, NewTID );
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
