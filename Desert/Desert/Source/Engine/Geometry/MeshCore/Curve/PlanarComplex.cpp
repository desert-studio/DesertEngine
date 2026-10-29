// Ported from UE 5.8 .../GeometryCore/Public/Curve/PlanarComplex.h and Polygon2.h (see the header).
#include "Engine/Geometry/MeshCore/Curve/PlanarComplex.hpp"

#include "Engine/Geometry/MeshCore/CompGeom/ExactPredicates.hpp"

#include <glm/common.hpp>

#include <algorithm>
#include <numeric>

using namespace Desert::Geometry;

namespace
{
    struct Bounds2
    {
        glm::dvec2 Min{ 0.0 };
        glm::dvec2 Max{ 0.0 };

        bool Contains( const Bounds2& Other ) const
        {
            return Other.Min.x >= Min.x && Other.Min.y >= Min.y && Other.Max.x <= Max.x && Other.Max.y <= Max.y;
        }
        bool Intersects( const Bounds2& Other ) const
        {
            return !( Other.Max.x < Min.x || Other.Min.x > Max.x || Other.Max.y < Min.y || Other.Min.y > Max.y );
        }
    };

    Bounds2 BoundsOf( const Polygon2& Polygon )
    {
        Bounds2 Box;
        if ( Polygon.empty() )
            return Box;
        Box.Min = Box.Max = Polygon[0];
        for ( const glm::dvec2& V : Polygon )
        {
            Box.Min = glm::min( Box.Min, V );
            Box.Max = glm::max( Box.Max, V );
        }
        return Box;
    }

    bool OnSegment( const glm::dvec2& P, const glm::dvec2& Q, const glm::dvec2& R )
    {
        return R.x >= std::min( P.x, Q.x ) && R.x <= std::max( P.x, Q.x ) && R.y >= std::min( P.y, Q.y ) &&
               R.y <= std::max( P.y, Q.y );
    }

    // UE TSegment2::Intersects: the closed segments share a point (crossing or touching).
    bool SegmentsIntersect( const glm::dvec2& A0, const glm::dvec2& A1, const glm::dvec2& B0,
                            const glm::dvec2& B1 )
    {
        const double D1 = ExactPredicates::Orient2( A0, A1, B0 );
        const double D2 = ExactPredicates::Orient2( A0, A1, B1 );
        const double D3 = ExactPredicates::Orient2( B0, B1, A0 );
        const double D4 = ExactPredicates::Orient2( B0, B1, A1 );
        if ( ( ( D1 > 0 && D2 < 0 ) || ( D1 < 0 && D2 > 0 ) ) && ( ( D3 > 0 && D4 < 0 ) || ( D3 < 0 && D4 > 0 ) ) )
            return true;
        return ( D1 == 0 && OnSegment( A0, A1, B0 ) ) || ( D2 == 0 && OnSegment( A0, A1, B1 ) ) ||
               ( D3 == 0 && OnSegment( B0, B1, A0 ) ) || ( D4 == 0 && OnSegment( B0, B1, A1 ) );
    }

    // UE TPolygon2::Intersects: any edge of A meets any edge of B.
    bool EdgesIntersect( const Polygon2& A, const Polygon2& B )
    {
        const size_t NA = A.size();
        const size_t NB = B.size();
        for ( size_t I = 0; I < NA; ++I )
            for ( size_t J = 0; J < NB; ++J )
                if ( SegmentsIntersect( A[I], A[( I + 1 ) % NA], B[J], B[( J + 1 ) % NB] ) )
                    return true;
        return false;
    }
} // namespace

double PolygonUtil::SignedArea( const Polygon2& Polygon )
{
    double    Area = 0.0;
    const int N    = static_cast<int>( Polygon.size() );
    for ( int I = 0; I < N; ++I )
    {
        const glm::dvec2& V0 = Polygon[I];
        const glm::dvec2& V1 = Polygon[( I + 1 ) % N];
        Area += V0.x * V1.y - V0.y * V1.x;
    }
    return Area * 0.5;
}

// UE CurveUtil::Contains2: nonzero winding number (Sunday's crossing rule).
bool PolygonUtil::Contains( const Polygon2& Polygon, const glm::dvec2& Point )
{
    int       WindingNumber = 0;
    const int N             = static_cast<int>( Polygon.size() );
    for ( int I = 0; I < N; ++I )
    {
        const glm::dvec2& A = Polygon[I];
        const glm::dvec2& B = Polygon[( I + 1 ) % N];
        if ( A.y <= Point.y )
        {
            if ( B.y > Point.y && ExactPredicates::Orient2( A, B, Point ) > 0 )
                ++WindingNumber;
        }
        else if ( B.y <= Point.y && ExactPredicates::Orient2( A, B, Point ) < 0 )
            --WindingNumber;
    }
    return WindingNumber != 0;
}

bool PolygonUtil::Contains( const Polygon2& Outer, const Polygon2& Inner )
{
    for ( const glm::dvec2& V : Inner )
        if ( !Contains( Outer, V ) )
            return false;
    return !EdgesIntersect( Outer, Inner );
}

bool PolygonUtil::Overlaps( const Polygon2& A, const Polygon2& B )
{
    if ( !BoundsOf( A ).Intersects( BoundsOf( B ) ) )
        return false;
    for ( const glm::dvec2& V : B )
        if ( Contains( A, V ) )
            return true;
    for ( const glm::dvec2& V : A )
        if ( Contains( B, V ) )
            return true;
    return EdgesIntersect( A, B );
}

void PlanarComplex::FindSolidRegions()
{
    m_Solids.clear();
    const int N = static_cast<int>( m_Polygons.size() );

    struct PolygonInfo
    {
        double  Area        = 0.0;
        bool    Orientation = false;
        Bounds2 Bounds;
    };
    std::vector<PolygonInfo> Info( N );
    for ( int I = 0; I < N; ++I )
    {
        const double SignedArea = PolygonUtil::SignedArea( m_Polygons[I] );
        Info[I].Orientation     = SignedArea > 0;
        Info[I].Area            = Info[I].Orientation ? SignedArea : -SignedArea;
        Info[I].Bounds          = BoundsOf( m_Polygons[I] );
    }

    std::vector<int> PolygonIndices( N );
    std::iota( PolygonIndices.begin(), PolygonIndices.end(), 0 );
    std::stable_sort( PolygonIndices.begin(), PolygonIndices.end(),
                      [&Info]( int Ind1, int Ind2 ) { return Info[Ind1].Area > Info[Ind2].Area; } );

    // largest to smallest, greedily assign parents; a smaller containing parent found later replaces the earlier
    // one (unless that would make the hole overlap a sibling)
    std::vector<int> Parent( N, -1 );
    for ( int ParentCand = 0; ParentCand + 1 < N; ++ParentCand )
    {
        const int          PIdx  = PolygonIndices[ParentCand];
        const PolygonInfo& PInfo = Info[PIdx];
        if ( m_bTrustOrientations && !PInfo.Orientation )
            continue; // a hole: no holes in it
        for ( int ChildCand = ParentCand + 1; ChildCand < N; ++ChildCand )
        {
            const int          CIdx  = PolygonIndices[ChildCand];
            const PolygonInfo& CInfo = Info[CIdx];
            if ( m_bTrustOrientations && CInfo.Orientation )
                continue; // an outer: cannot be a hole
            if ( PInfo.Bounds.Contains( CInfo.Bounds ) &&
                 PolygonUtil::Contains( m_Polygons[PIdx], m_Polygons[CIdx] ) )
            {
                bool bOverlapFound = false;
                if ( !m_bAllowOverlappingHoles )
                    for ( int ParentRecheckIdx = ParentCand + 1; ParentRecheckIdx < ChildCand; ++ParentRecheckIdx )
                        if ( Parent[ParentRecheckIdx] == ParentCand &&
                             PolygonUtil::Overlaps( m_Polygons[PolygonIndices[ParentRecheckIdx]],
                                                    m_Polygons[CIdx] ) )
                        {
                            bOverlapFound = true;
                            break;
                        }
                if ( !bOverlapFound )
                    Parent[ChildCand] = ParentCand;
            }
        }
    }

    // nests from the parent relation
    std::vector<bool> Taken( N, false );
    for ( int NestCand = 0; NestCand < N; ++NestCand )
    {
        const int NIdx = PolygonIndices[NestCand];
        if ( Taken[NestCand] || ( m_bTrustOrientations && !Info[NIdx].Orientation ) )
            continue;
        PolygonNesting& Nest = m_Solids.emplace_back();
        Nest.OuterIndex      = NIdx;
        for ( int HoleCand = NestCand + 1; HoleCand < N; ++HoleCand )
            if ( Parent[HoleCand] == NestCand )
            {
                Nest.HoleIndices.push_back( PolygonIndices[HoleCand] );
                Taken[HoleCand] = true;
            }
    }
}

GeneralPolygon2 PlanarComplex::ConvertNestToGeneralPolygon( const PolygonNesting& Nest ) const
{
    GeneralPolygon2 GPoly;
    GPoly.Outer = m_Polygons[Nest.OuterIndex];
    for ( const int HoleIdx : Nest.HoleIndices )
        GPoly.Holes.push_back( m_Polygons[HoleIdx] );
    return GPoly;
}
