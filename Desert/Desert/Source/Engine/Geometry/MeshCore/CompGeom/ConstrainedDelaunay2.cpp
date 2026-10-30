// Ported from UE 5.8 .../GeometryAlgorithms/Private/ConstrainedDelaunay2.cpp (see the header for the line ranges
// and what was adapted).
#include "Engine/Geometry/MeshCore/CompGeom/ConstrainedDelaunay2.hpp"

#include "Engine/Geometry/MeshCore/CompGeom/Delaunay2.hpp"

#include <algorithm>
#include <unordered_map>

using namespace Desert::Geometry;

namespace
{
    // unordered edge (min, max) -> whether the input edge ran max -> min
    using EdgeMap = std::unordered_map<Index2i, bool>;

    void AddOrderedEdge( EdgeMap& Map, int VertA, int VertB )
    {
        const bool bReversed = VertA > VertB;
        if ( bReversed )
            std::swap( VertA, VertB );
        Map.insert_or_assign( Index2i( VertA, VertB ), bReversed );
    }

    // the change in winding number from crossing the oriented edge VertA -> VertB: 1 for a forward input edge, -1
    // for a reversed one, 0 if the edge is not an input edge
    int WindingAcross( const EdgeMap& Map, int VertA, int VertB )
    {
        const bool bReversed = VertA > VertB;
        if ( bReversed )
            std::swap( VertA, VertB );
        const auto Found = Map.find( Index2i( VertA, VertB ) );
        if ( Found == Map.end() )
            return 0;
        return bReversed == Found->second ? 1 : -1;
    }

    bool HasUnorderedEdge( const EdgeMap& Map, int VertA, int VertB )
    {
        return Map.contains( Index2i( std::min( VertA, VertB ), std::max( VertA, VertB ) ) );
    }
} // namespace

void ConstrainedDelaunay2::Add( const Polygon2& Polygon, bool bIsHole )
{
    const int VertexStart = static_cast<int>( m_Vertices.size() );
    const int VertexEnd   = VertexStart + static_cast<int>( Polygon.size() );
    m_Vertices.insert( m_Vertices.end(), Polygon.begin(), Polygon.end() );
    std::vector<Index2i>& EdgeArr = bIsHole ? m_HoleEdges : m_Edges;
    for ( int A = VertexEnd - 1, B = VertexStart; B < VertexEnd; A = B++ )
        EdgeArr.emplace_back( A, B );
}

void ConstrainedDelaunay2::Add( const GeneralPolygon2& GPolygon )
{
    Add( GPolygon.Outer, false );
    for ( const Polygon2& Hole : GPolygon.Holes )
        Add( Hole, true );
}

bool ConstrainedDelaunay2::ClassifyFromRule( int Winding ) const
{
    switch ( m_FillRule )
    {
        case EFillRule::Odd:
            return Winding % 2 != 0;
        case EFillRule::NonZero:
            return Winding != 0;
        case EFillRule::Positive:
            return Winding > 0;
        case EFillRule::Negative:
            return Winding < 0;
    }
    return false;
}

bool ConstrainedDelaunay2::Triangulate()
{
    m_Triangles.clear();

    Delaunay2 Delaunay;
    Delaunay.m_bAutomaticallyFixEdgesToDuplicateVertices = true;
    if ( !Delaunay.Triangulate( m_Vertices ) )
        return false;

    Delaunay.m_bKeepFastEdgeAdjacencyData = true;
    Delaunay.m_bValidateEdges             = false; // validated below, edge by edge
    Delaunay.ConstrainEdges( m_Vertices, m_Edges );
    Delaunay.ConstrainEdges( m_Vertices, m_HoleEdges );

    // the boundary and hole edges as they went in, to flood fill across them for the inside/outside decision
    EdgeMap                     BoundaryMap;
    EdgeMap                     HoleMap;
    EdgeMap*                    EdgeAndHoleMaps[2]       = { &BoundaryMap, &HoleMap };
    const std::vector<Index2i>* InputEdgesAndHoles[2]    = { &m_Edges, &m_HoleEdges };
    bool                        bBoundaryTrackingFailure = false;
    for ( int EdgeOrHole = 0; EdgeOrHole < 2; ++EdgeOrHole )
        for ( Index2i Edge : *InputEdgesAndHoles[EdgeOrHole] )
        {
            Delaunay.FixDuplicatesOnEdge( Edge );
            // a missing edge (e.g. exactly on top of another) is noted and the walk proceeds without it
            if ( !Delaunay.HasEdge( Edge, false ) )
                bBoundaryTrackingFailure = true;
            else
                AddOrderedEdge( *EdgeAndHoleMaps[EdgeOrHole], Edge.A, Edge.B );
        }

    std::vector<Index3i> Indices;
    std::vector<Index3i> Adj;
    Delaunay.GetTrianglesAndAdjacency( Indices, Adj );

    const int TriNum = static_cast<int>( Adj.size() );
    // 0 unprocessed (delete), 1 keep, -1 processed, delete
    std::vector<signed char> Keep( TriNum, 0 );

    // seed with every triangle on the convex hull: the walk refuses to cross hole edges, so one seed is not enough
    std::vector<std::pair<int, int>> ToWalkQ; // triangle, winding number
    for ( int TriIdx = 0; TriIdx < TriNum; ++TriIdx )
        for ( int SubIdx = 0, NextIdx = 2; SubIdx < 3; NextIdx = SubIdx++ )
            if ( Adj[TriIdx][NextIdx] < 0 ) // on the hull
            {
                const int VertA = Indices[TriIdx][SubIdx];
                const int VertB = Indices[TriIdx][NextIdx];
                if ( HasUnorderedEdge( HoleMap, VertA, VertB ) )
                    continue; // cannot cross hole edges
                // negated: a hull triangle is already on the inner side of its hull edge, not across it
                const int Winding = -WindingAcross( BoundaryMap, VertA, VertB );
                ToWalkQ.emplace_back( TriIdx, Winding );
                Keep[TriIdx] = ClassifyFromRule( Winding ) ? 1 : -1;
                break;
            }

    size_t SelIdx = 0; // a rotating pop index: less depth-first, a little more robust to bad data
    while ( !ToWalkQ.empty() )
    {
        SelIdx                           = ( SelIdx + 1 ) % ToWalkQ.size();
        const auto [TriIdx, LastWinding] = ToWalkQ[SelIdx];
        ToWalkQ[SelIdx]                  = ToWalkQ.back();
        ToWalkQ.pop_back();
        for ( int SubIdx = 0, NextIdx = 2; SubIdx < 3; NextIdx = SubIdx++ )
        {
            const int VertA = Indices[TriIdx][SubIdx];
            const int VertB = Indices[TriIdx][NextIdx];
            if ( HasUnorderedEdge( HoleMap, VertA, VertB ) )
                continue;
            const int AdjTri = Adj[TriIdx][NextIdx];
            if ( AdjTri >= 0 && Keep[AdjTri] == 0 )
            {
                const int Winding = LastWinding + WindingAcross( BoundaryMap, VertA, VertB );
                ToWalkQ.emplace_back( AdjTri, Winding );
                Keep[AdjTri] = ClassifyFromRule( Winding ) ? 1 : -1;
            }
        }
    }

    // BuildFinalTriangles without bowtie updates
    for ( int I = 0; I < TriNum; ++I )
        if ( Keep[I] > 0 )
        {
            Index3i& Tri = m_Triangles.emplace_back( Indices[I].A, Indices[I].B, Indices[I].C );
            if ( !m_bOutputCCW )
                std::swap( Tri.B, Tri.C );
        }
    return !bBoundaryTrackingFailure;
}

std::vector<Index3i> Desert::Geometry::ConstrainedDelaunayTriangulate( const GeneralPolygon2& GeneralPolygon )
{
    ConstrainedDelaunay2 Triangulation;
    Triangulation.m_FillRule = ConstrainedDelaunay2::EFillRule::Positive;
    Triangulation.Add( GeneralPolygon );
    Triangulation.Triangulate();
    return std::move( Triangulation.m_Triangles );
}
