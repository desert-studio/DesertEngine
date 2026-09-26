// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Distance/DistPoint3Triangle3.h:18-251, adapted:
// namespace Desert::Geometry, UE::Math::TVector2 is our TVector2 shim. The algorithm is UE's (itself after
// Geometric Tools' DistPointTriangle) unchanged.
#pragma once

#include "Engine/Geometry/UECore/TriangleTypes.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    /**
     * Compute unsigned distance between 3D Point and 3D Triangle
     */
    template <typename Real>
    class DistPoint3Triangle3
    {
    public:
        // Input
        glm::vec<3, Real> Point{};
        Triangle3<Real>   Triangle;

        // Results
        glm::vec<3, Real> TriangleBaryCoords{};
        glm::vec<3, Real> ClosestTrianglePoint{};

        DistPoint3Triangle3( const glm::vec<3, Real>& PointIn, const Triangle3<Real>& TriangleIn )
             : Point( PointIn ), Triangle( TriangleIn )
        {
        }

        Real GetSquared()
        {
            return ComputeResult();
        }

        Real ComputeResult()
        {
            const glm::vec<3, Real> diff  = Point - Triangle.V[0];
            const glm::vec<3, Real> edge0 = Triangle.V[1] - Triangle.V[0];
            const glm::vec<3, Real> edge1 = Triangle.V[2] - Triangle.V[0];
            const Real              a00   = glm::length2( edge0 );
            const Real              a01   = glm::dot( edge0, edge1 );
            const Real              a11   = glm::length2( edge1 );
            const Real              b0    = -glm::dot( diff, edge0 );
            const Real              b1    = -glm::dot( diff, edge1 );

            const Real f00 = b0;
            const Real f10 = b0 + a00;
            const Real f01 = b0 + a01;

            glm::vec<2, Real> p0{};
            glm::vec<2, Real> p1{};
            glm::vec<2, Real> p{};
            Real           dt1;
            Real           h0;
            Real           h1;

            // Compute the endpoints p0 and p1 of the segment.  The segment is
            // parameterized by L(z) = (1-z)*p0 + z*p1 for z in [0,1] and the
            // directional derivative of half the quadratic on the segment is
            // H(z) = Dot(p1-p0,gradient[Q](L(z))/2), where gradient[Q]/2 = (F,G).
            // By design, F(L(z)) = 0 for cases (2), (4), (5), and (6).  Cases (1) and
            // (3) can correspond to no-intersection or intersection of F = 0 with the
            // Triangle.
            if ( f00 >= static_cast<Real>( 0 ) )
            {
                if ( f01 >= static_cast<Real>( 0 ) )
                {
                    // (1) p0 = (0,0), p1 = (0,1), H(z) = G(L(z))
                    GetMinEdge02( a11, b1, p );
                }
                else
                {
                    // (2) p0 = (0,t10), p1 = (t01,1-t01), H(z) = (t11 - t10)*G(L(z))
                    p0[0] = static_cast<Real>( 0 );
                    p0[1] = f00 / ( f00 - f01 );
                    p1[0] = f01 / ( f01 - f10 );
                    p1[1] = static_cast<Real>( 1 ) - p1[0];
                    dt1   = p1[1] - p0[1];
                    h0    = dt1 * ( a11 * p0[1] + b1 );
                    if ( h0 >= static_cast<Real>( 0 ) )
                    {
                        GetMinEdge02( a11, b1, p );
                    }
                    else
                    {
                        h1 = dt1 * ( a01 * p1[0] + a11 * p1[1] + b1 );
                        if ( h1 <= static_cast<Real>( 0 ) )
                        {
                            GetMinEdge12( a01, a11, b1, f10, f01, p );
                        }
                        else
                        {
                            GetMinInterior( p0, h0, p1, h1, p );
                        }
                    }
                }
            }
            else if ( f01 <= static_cast<Real>( 0 ) )
            {
                if ( f10 <= static_cast<Real>( 0 ) )
                {
                    // (3) p0 = (1,0), p1 = (0,1), H(z) = G(L(z)) - F(L(z))
                    GetMinEdge12( a01, a11, b1, f10, f01, p );
                }
                else
                {
                    // (4) p0 = (t00,0), p1 = (t01,1-t01), H(z) = t11*G(L(z))
                    p0[0] = f00 / ( f00 - f10 );
                    p0[1] = static_cast<Real>( 0 );
                    p1[0] = f01 / ( f01 - f10 );
                    p1[1] = static_cast<Real>( 1 ) - p1[0];
                    h0    = p1[1] * ( a01 * p0[0] + b1 );
                    if ( h0 >= static_cast<Real>( 0 ) )
                    {
                        p = p0; // GetMinEdge01
                    }
                    else
                    {
                        h1 = p1[1] * ( a01 * p1[0] + a11 * p1[1] + b1 );
                        if ( h1 <= static_cast<Real>( 0 ) )
                        {
                            GetMinEdge12( a01, a11, b1, f10, f01, p );
                        }
                        else
                        {
                            GetMinInterior( p0, h0, p1, h1, p );
                        }
                    }
                }
            }
            else if ( f10 <= static_cast<Real>( 0 ) )
            {
                // (5) p0 = (0,t10), p1 = (t01,1-t01), H(z) = (t11 - t10)*G(L(z))
                p0[0] = static_cast<Real>( 0 );
                p0[1] = f00 / ( f00 - f01 );
                p1[0] = f01 / ( f01 - f10 );
                p1[1] = static_cast<Real>( 1 ) - p1[0];
                dt1   = p1[1] - p0[1];
                h0    = dt1 * ( a11 * p0[1] + b1 );
                if ( h0 >= static_cast<Real>( 0 ) )
                {
                    GetMinEdge02( a11, b1, p );
                }
                else
                {
                    h1 = dt1 * ( a01 * p1[0] + a11 * p1[1] + b1 );
                    if ( h1 <= static_cast<Real>( 0 ) )
                    {
                        GetMinEdge12( a01, a11, b1, f10, f01, p );
                    }
                    else
                    {
                        GetMinInterior( p0, h0, p1, h1, p );
                    }
                }
            }
            else
            {
                // (6) p0 = (t00,0), p1 = (0,t11), H(z) = t11*G(L(z))
                p0[0] = f00 / ( f00 - f10 );
                p0[1] = static_cast<Real>( 0 );
                p1[0] = static_cast<Real>( 0 );
                p1[1] = f00 / ( f00 - f01 );
                h0    = p1[1] * ( a01 * p0[0] + b1 );
                if ( h0 >= static_cast<Real>( 0 ) )
                {
                    p = p0; // GetMinEdge01
                }
                else
                {
                    h1 = p1[1] * ( a11 * p1[1] + b1 );
                    if ( h1 <= static_cast<Real>( 0 ) )
                    {
                        GetMinEdge02( a11, b1, p );
                    }
                    else
                    {
                        GetMinInterior( p0, h0, p1, h1, p );
                    }
                }
            }

            TriangleBaryCoords   = glm::vec<3, Real>( static_cast<Real>( 1 ) - p[0] - p[1], p[0], p[1] );
            ClosestTrianglePoint = Triangle.V[0] + p[0] * edge0 + p[1] * edge1;
            return DistanceSquared( Point, ClosestTrianglePoint );
        }

    private:
        void GetMinEdge02( Real const& a11, Real const& b1, glm::vec<2, Real>& p ) const
        {
            p[0] = static_cast<Real>( 0 );
            if ( b1 >= static_cast<Real>( 0 ) )
            {
                p[1] = static_cast<Real>( 0 );
            }
            else if ( a11 + b1 <= static_cast<Real>( 0 ) )
            {
                p[1] = static_cast<Real>( 1 );
            }
            else
            {
                p[1] = -b1 / a11;
            }
        }

        void GetMinEdge12( Real const& a01, Real const& a11, Real const& b1, Real const& f10, Real const& f01,
                           glm::vec<2, Real>& p ) const
        {
            const Real h0 = a01 + b1 - f10;
            if ( h0 >= static_cast<Real>( 0 ) )
            {
                p[1] = static_cast<Real>( 0 );
            }
            else
            {
                const Real h1 = a11 + b1 - f01;
                if ( h1 <= static_cast<Real>( 0 ) )
                {
                    p[1] = static_cast<Real>( 1 );
                }
                else
                {
                    p[1] = h0 / ( h0 - h1 );
                }
            }
            p[0] = static_cast<Real>( 1 ) - p[1];
        }

        void GetMinInterior( glm::vec<2, Real> const& p0, Real const& h0, glm::vec<2, Real> const& p1,
                             Real const& h1, glm::vec<2, Real>& p ) const
        {
            const Real z = h0 / ( h0 - h1 );
            p            = ( static_cast<Real>( 1 ) - z ) * p0 + z * p1;
        }
    };

    using DistPoint3Triangle3f = DistPoint3Triangle3<float>;
    using DistPoint3Triangle3d = DistPoint3Triangle3<double>;
} // namespace Desert::Geometry
