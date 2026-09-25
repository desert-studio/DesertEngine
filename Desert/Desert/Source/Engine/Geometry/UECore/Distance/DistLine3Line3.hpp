// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Distance/DistLine3Line3.h:14-104, adapted:
// namespace Desert::Geometry. The algorithm is UE's (itself after geometry3Sharp) unchanged.
#pragma once

#include "Engine/Geometry/UECore/LineTypes.hpp"
#include "Engine/Geometry/UECore/MathUtil.hpp"

#include <cmath>

namespace Desert::Geometry
{
    /**
     * Compute distance between two 3D lines
     */
    template <typename Real>
    class TDistLine3Line3
    {
    public:
        // Input
        TLine3<Real> Line1;
        TLine3<Real> Line2;

        // Results
        Real          DistanceSquared = -1.0;
        bool          bIsParallel     = false;
        TVector<Real> Line1ClosestPoint;
        Real          Line1Parameter = 0;
        TVector<Real> Line2ClosestPoint;
        Real          Line2Parameter = 0;

        TDistLine3Line3( const TLine3<Real>& Line1In, const TLine3<Real>& Line2In )
             : Line1( Line1In ), Line2( Line2In )
        {
        }

        Real Get()
        {
            return static_cast<Real>( std::sqrt( ComputeResult() ) );
        }

        Real GetSquared()
        {
            return ComputeResult();
        }

        Real ComputeResult()
        {
            if ( DistanceSquared >= 0 )
            {
                return DistanceSquared;
            }

            TVector<Real> kDiff = Line1.Origin - Line2.Origin;
            Real          a01   = -Line1.Direction.Dot( Line2.Direction );
            Real          b0    = kDiff.Dot( Line1.Direction );
            Real          c     = kDiff.SquaredLength();
            Real          det   = std::abs( static_cast<Real>( 1 ) - a01 * a01 );
            Real          b1;
            Real          s0;
            Real          s1;
            Real          sqrDist;

            if ( det >= TMathUtil<Real>::ZeroTolerance )
            {
                b1 = -kDiff.Dot( Line2.Direction );
                s1 = a01 * b0 - b1;

                // Two interior points are closest.
                Real invDet = static_cast<Real>( 1 ) / det;
                s0          = ( a01 * b1 - b0 ) * invDet;
                s1 *= invDet;
                sqrDist =
                     s0 * ( s0 + a01 * s1 + static_cast<Real>( 2 ) * b0 ) + s1 * ( a01 * s0 + s1 + static_cast<Real>( 2 ) * b1 ) + c;
                Line1ClosestPoint = Line1.Origin + s0 * Line1.Direction;
                Line2ClosestPoint = Line2.Origin + s1 * Line2.Direction;
                Line1Parameter    = s0;
                Line2Parameter    = s1;
                bIsParallel       = false;
            }
            else
            {
                // Lines are parallel, closest pair at line1 origin
                Line1Parameter    = static_cast<Real>( 0 );
                Line1ClosestPoint = Line1.Origin;
                Line2Parameter    = Line2.Project( Line1.Origin );
                Line2ClosestPoint = Line2.PointAt( Line2Parameter );
                sqrDist           = Line1.DistanceSquared( Line2ClosestPoint );
                bIsParallel       = true;
            }

            // Account for numerical round-off errors.
            DistanceSquared = ( sqrDist < static_cast<Real>( 0 ) ) ? static_cast<Real>( 0 ) : sqrDist;
            return DistanceSquared;
        }
    };

    using FDistLine3Line3f = TDistLine3Line3<float>;
    using FDistLine3Line3d = TDistLine3Line3<double>;
} // namespace Desert::Geometry
