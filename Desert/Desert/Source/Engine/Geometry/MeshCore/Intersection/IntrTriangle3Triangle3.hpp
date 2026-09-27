// Ported from UE 5.8
// Engine/Source/Runtime/GeometryCore/Public/Intersection/IntrTriangle3Triangle3.h:32-280,501-734,
// IntrSegment2Triangle2.h:84-220, IntrLine2Triangle2.h:148-270, Intersector1.h:75-104, adapted: UE Core as glm,
// namespace Desert::Geometry, double only, the three helper classes folded into free functions. Left out, with
// reasons: the coplanar polygon (GetCoplanarIntersection + IntrTriangle2Triangle2) - FMeshBoolean never turns
// bReportCoplanarIntersection on, so two coplanar triangles report no intersection exactly as in UE; Test() and
// the static Intersects() - the Boolean calls Find() directly. ContainsPoint's IsInsideOrOn_Oriented (a vertex of
// one triangle touching the other's plane) is a closed barycentric-sign test here: it only yields a POINT result,
// which no consumer of this port (the AABB tree's segment list, MeshMeshCut) reads.

#pragma once

#include "Engine/Geometry/MeshCore/MathUtil.hpp"
#include "Engine/Geometry/MeshCore/TriangleTypes.hpp"
#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

#include <glm/geometric.hpp>

#include <array>
#include <cstdint>
#include <cmath>
#include <utility>

namespace Desert::Geometry
{
    enum class TriTriResult : uint8_t
    {
        NotComputed,
        Intersects,
        NoIntersection,
    };

    enum class TriTriType : uint8_t
    {
        Empty,
        Point,
        Segment,
    };

    namespace TriTriDetail
    {
        inline double DotPerp( const glm::dvec2& A, const glm::dvec2& B )
        {
            return A.x * B.y - A.y * B.x;
        }

        // IntrSegment2Triangle2::Find with the segment as center / unit direction / half-length.
        // Returns the number of points (0, 1 or 2) written to Out.
        inline int SegmentTriangle2( const glm::dvec2& P0, const glm::dvec2& P1,
                                     const std::array<glm::dvec2, 3>& Tri, double Tolerance, glm::dvec2 Out[2] )
        {
            const glm::dvec2 Center    = 0.5 * ( P0 + P1 );
            glm::dvec2       Direction = P1 - P0;
            const double     Length    = glm::length( Direction );
            const double     Extent    = 0.5 * Length;
            if ( Length > 0 )
                Direction /= Length;
            if ( Extent == 0.0 )
            {
                int pos = 0, neg = 0;
                for ( int TriPrev = 2, TriIdx = 0; TriIdx < 3; TriPrev = TriIdx++ )
                {
                    const glm::dvec2 ToPt = Center - Tri[TriIdx];
                    const glm::dvec2 Edge = Tri[TriIdx] - Tri[TriPrev];
                    glm::dvec2       Perp = glm::dvec2( Edge.y, -Edge.x );
                    double           Plen = glm::length( Perp );
                    if ( Plen == 0 )
                    {
                        const glm::dvec2 OtherV = Tri[( TriIdx + 1 ) % 3];
                        Perp                    = OtherV - Tri[TriIdx];
                        Plen                    = glm::length( Perp );
                        if ( Plen == 0 )
                        {
                            const glm::dvec2 d = Tri[0] - Center;
                            pos = neg = glm::dot( d, d ) <= Tolerance * Tolerance ? 0 : 1;
                            break;
                        }
                        Perp /= Plen;
                        const double OtherSideSign = glm::dot( -Perp, Center - OtherV );
                        if ( OtherSideSign < -Tolerance )
                            neg++;
                        else if ( OtherSideSign > Tolerance )
                            pos++;
                    }
                    else
                        Perp /= Plen;
                    const double SideSign = glm::dot( Perp, ToPt );
                    if ( SideSign < -Tolerance )
                        neg++;
                    else if ( SideSign > Tolerance )
                        pos++;
                }
                if ( pos == 0 || neg == 0 )
                {
                    Out[0] = Out[1] = Center;
                    return 2;
                }
                return 0;
            }

            // IntrLine2Triangle2::TriangleLineRelations
            glm::dvec3 Dist{};
            int        Sign[3]{};
            int        positive = 0, negative = 0;
            for ( int i = 0; i < 3; ++i )
            {
                Dist[i] = DotPerp( Tri[i] - Center, Direction );
                if ( Dist[i] > Tolerance )
                {
                    Sign[i] = 1;
                    ++positive;
                }
                else if ( Dist[i] < -Tolerance )
                {
                    Sign[i] = -1;
                    ++negative;
                }
                else
                {
                    Dist[i] = 0.0;
                    Sign[i] = 0;
                }
            }
            if ( positive == 3 || negative == 3 )
                return 0;

            // IntrLine2Triangle2::GetInterval
            double proj[3];
            for ( int i = 0; i < 3; ++i )
                proj[i] = glm::dot( Direction, Tri[i] - Center );
            double param[2]{};
            int    quantity = 0;
            for ( int i0 = 2, i1 = 0; i1 < 3; i0 = i1++ )
                if ( Sign[i0] * Sign[i1] < 0 )
                    param[quantity++] = ( Dist[i0] * proj[i1] - Dist[i1] * proj[i0] ) / ( Dist[i0] - Dist[i1] );
            if ( quantity < 2 )
                for ( int i = 0; i < 3; i++ )
                {
                    if ( Sign[i] != 0 )
                        continue;
                    if ( quantity == 2 )
                    {
                        if ( param[0] > param[1] )
                            std::swap( param[0], param[1] );
                        if ( proj[i] < param[0] )
                            param[0] = proj[i];
                        else if ( proj[i] > param[1] )
                            param[1] = proj[i];
                    }
                    else
                        param[quantity++] = proj[i];
                }
            if ( quantity == 0 )
                return 0;
            if ( quantity == 2 )
            {
                if ( param[0] > param[1] )
                    std::swap( param[0], param[1] );
            }
            else
                param[1] = param[0];

            // Intersector1::Find of [param0, param1] against [-Extent, Extent]
            const double UMin = param[0], UMax = param[1], VMin = -Extent, VMax = Extent;
            int          Num  = 0;
            double       IMin = 0, IMax = 0;
            if ( UMax < VMin || UMin > VMax )
                Num = 0;
            else if ( UMax > VMin )
            {
                if ( UMin < VMax )
                {
                    Num  = 2;
                    IMin = UMin < VMin ? VMin : UMin;
                    IMax = UMax > VMax ? VMax : UMax;
                    if ( IMin == IMax )
                        Num = 1;
                }
                else
                {
                    Num  = 1;
                    IMin = UMin;
                }
            }
            else
            {
                Num  = 1;
                IMin = UMax;
            }
            if ( Num >= 1 )
                Out[0] = Center + IMin * Direction;
            if ( Num == 2 )
                Out[1] = Center + IMax * Direction;
            return Num;
        }
    } // namespace TriTriDetail

    // Intersection of two 3D triangles as a point or a segment (UE's FIntrTriangle3Triangle3d with the coplanar
    // report off). Reuse one object across pairs: SetTriangle0/1 reset the result, as in UE.
    class IntrTriangle3Triangle3
    {
    public:
        TriTriResult Result   = TriTriResult::NotComputed;
        TriTriType   Type     = TriTriType::Empty;
        int          Quantity = 0;
        glm::dvec3   Points[2]{};

        IntrTriangle3Triangle3() = default;
        IntrTriangle3Triangle3( const Triangle3d& T0, const Triangle3d& T1 ) : m_Triangle0( T0 ), m_Triangle1( T1 )
        {
        }

        void SetTriangle0( const Triangle3d& T )
        {
            Result      = TriTriResult::NotComputed;
            m_Triangle0 = T;
        }
        void SetTriangle1( const Triangle3d& T )
        {
            Result      = TriTriResult::NotComputed;
            m_Triangle1 = T;
        }
        void SetTolerance( double ToleranceIn )
        {
            m_Tolerance = ToleranceIn;
        }

        bool Find()
        {
            if ( Result != TriTriResult::NotComputed )
                return Result != TriTriResult::NoIntersection;
            Result = TriTriResult::NoIntersection;
            Type   = TriTriType::Empty;

            const glm::dvec3 Normal0 = VectorUtil::Normal( m_Triangle0.V[0], m_Triangle0.V[1], m_Triangle0.V[2] );
            if ( Normal0 == glm::dvec3( 0 ) )
            {
                if ( VectorUtil::Normal( m_Triangle1.V[0], m_Triangle1.V[1], m_Triangle1.V[2] ) ==
                     glm::dvec3( 0 ) )
                    return false;
                IntrTriangle3Triangle3 Swapped( m_Triangle1, m_Triangle0 );
                Swapped.m_Tolerance = m_Tolerance;
                const bool bRes     = Swapped.Find();
                Result              = Swapped.Result;
                Type                = Swapped.Type;
                Quantity            = Swapped.Quantity;
                Points[0]           = Swapped.Points[0];
                Points[1]           = Swapped.Points[1];
                return bRes;
            }
            const double Constant0 = glm::dot( Normal0, m_Triangle0.V[0] );

            // TrianglePlaneRelations
            glm::dvec3 dist1{};
            int        sign1[3]{};
            int        pos1 = 0, neg1 = 0, zero1 = 0;
            for ( int i = 0; i < 3; ++i )
            {
                dist1[i] = glm::dot( Normal0, m_Triangle1.V[i] ) - Constant0;
                if ( dist1[i] > m_Tolerance )
                {
                    sign1[i] = 1;
                    pos1++;
                }
                else if ( dist1[i] < -m_Tolerance )
                {
                    sign1[i] = -1;
                    neg1++;
                }
                else
                {
                    sign1[i] = 0;
                    zero1++;
                }
            }
            if ( pos1 == 3 || neg1 == 3 || zero1 == 3 )
                return false;

            if ( pos1 == 0 || neg1 == 0 )
            {
                for ( int i = 0; i < 3; ++i )
                {
                    if ( zero1 == 2 && sign1[i] != 0 )
                        return IntersectsSegment( Normal0, Constant0, m_Triangle1.V[( i + 2 ) % 3],
                                                  m_Triangle1.V[( i + 1 ) % 3] );
                    if ( zero1 == 1 && sign1[i] == 0 )
                        return ContainsPoint( Normal0, m_Triangle1.V[i] );
                }
            }

            if ( zero1 == 0 )
            {
                const int iSign = pos1 == 1 ? +1 : -1;
                for ( int i = 0; i < 3; ++i )
                    if ( sign1[i] == iSign )
                    {
                        const int        iM = ( i + 2 ) % 3, iP = ( i + 1 ) % 3;
                        double           t     = dist1[i] / ( dist1[i] - dist1[iM] );
                        const glm::dvec3 intr0 = m_Triangle1.V[i] + t * ( m_Triangle1.V[iM] - m_Triangle1.V[i] );
                        t                      = dist1[i] / ( dist1[i] - dist1[iP] );
                        const glm::dvec3 intr1 = m_Triangle1.V[i] + t * ( m_Triangle1.V[iP] - m_Triangle1.V[i] );
                        return IntersectsSegment( Normal0, Constant0, intr0, intr1 );
                    }
            }

            for ( int i = 0; i < 3; ++i )
                if ( sign1[i] == 0 )
                {
                    const int        iM = ( i + 2 ) % 3, iP = ( i + 1 ) % 3;
                    const double     t     = dist1[iM] / ( dist1[iM] - dist1[iP] );
                    const glm::dvec3 intr0 = m_Triangle1.V[iM] + t * ( m_Triangle1.V[iP] - m_Triangle1.V[iM] );
                    return IntersectsSegment( Normal0, Constant0, m_Triangle1.V[i], intr0 );
                }
            return false;
        }

        // IntersectTriangleWithCoplanarSegment: project on the plane's dominant axis, clip in 2D, lift back.
        static int IntersectTriangleWithCoplanarSegment( const glm::dvec3& PlaneNormal, double PlaneConstant,
                                                         const Triangle3d& Tri, const glm::dvec3& End0,
                                                         const glm::dvec3& End1, glm::dvec3& OutA,
                                                         glm::dvec3& OutB, double Tolerance )
        {
            int    maxNormal = 0;
            double fmax      = std::abs( PlaneNormal.x );
            if ( std::abs( PlaneNormal.y ) > fmax )
            {
                maxNormal = 1;
                fmax      = std::abs( PlaneNormal.y );
            }
            if ( std::abs( PlaneNormal.z ) > fmax )
                maxNormal = 2;
            const int                 a = maxNormal == 0 ? 1 : 0;
            const int                 b = maxNormal == 2 ? 1 : 2;
            std::array<glm::dvec2, 3> projTri;
            for ( int i = 0; i < 3; ++i )
                projTri[i] = glm::dvec2( Tri.V[i][a], Tri.V[i][b] );
            glm::dvec2 intr[2];
            const int  Quantity = TriTriDetail::SegmentTriangle2(
                 glm::dvec2( End0[a], End0[b] ), glm::dvec2( End1[a], End1[b] ), projTri, Tolerance, intr );
            glm::dvec3*  OutPts[2]{ &OutA, &OutB };
            const double inv = 1.0 / PlaneNormal[maxNormal];
            for ( int i = 0; i < Quantity; ++i )
            {
                glm::dvec3 P;
                P[a]         = intr[i].x;
                P[b]         = intr[i].y;
                P[maxNormal] = inv * ( PlaneConstant - PlaneNormal[a] * P[a] - PlaneNormal[b] * P[b] );
                *OutPts[i]   = P;
            }
            return Quantity;
        }

    private:
        Triangle3d m_Triangle0;
        Triangle3d m_Triangle1;
        double     m_Tolerance = ZeroTolerance<double>;

        bool ContainsPoint( const glm::dvec3& PlaneNormal, const glm::dvec3& Point )
        {
            // Closed barycentric test in the plane (see the header): inside or on an edge counts.
            const glm::dvec3& A  = m_Triangle0.V[0];
            const glm::dvec3  n0 = glm::cross( m_Triangle0.V[1] - A, Point - A );
            const glm::dvec3  n1 = glm::cross( m_Triangle0.V[2] - m_Triangle0.V[1], Point - m_Triangle0.V[1] );
            const glm::dvec3  n2 = glm::cross( A - m_Triangle0.V[2], Point - m_Triangle0.V[2] );
            const double      s0 = glm::dot( n0, PlaneNormal ), s1 = glm::dot( n1, PlaneNormal ),
                         s2   = glm::dot( n2, PlaneNormal );
            const bool inside = ( s0 >= 0 && s1 >= 0 && s2 >= 0 ) || ( s0 <= 0 && s1 <= 0 && s2 <= 0 );
            if ( !inside )
                return false;
            Result    = TriTriResult::Intersects;
            Type      = TriTriType::Point;
            Quantity  = 1;
            Points[0] = Point;
            return true;
        }

        bool IntersectsSegment( const glm::dvec3& PlaneNormal, double PlaneConstant, const glm::dvec3& End0,
                                const glm::dvec3& End1 )
        {
            Quantity = IntersectTriangleWithCoplanarSegment( PlaneNormal, PlaneConstant, m_Triangle0, End0, End1,
                                                             Points[0], Points[1], m_Tolerance );
            if ( Quantity > 0 )
            {
                Result = TriTriResult::Intersects;
                Type   = Quantity == 2 ? TriTriType::Segment : TriTriType::Point;
                return true;
            }
            Result = TriTriResult::NoIntersection;
            Type   = TriTriType::Empty;
            return false;
        }
    };
} // namespace Desert::Geometry
