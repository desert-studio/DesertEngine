// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Distance/DistLine3Line3.h:14-104, adapted:
// namespace Desert::Geometry. The algorithm is UE's (itself after geometry3Sharp) unchanged.
#pragma once

#include "Engine/Geometry/MeshCore/LineTypes.hpp"
#include "Engine/Geometry/MeshCore/MathUtil.hpp"

#include <cmath>

namespace Desert::Geometry
{
    /**
     * Compute distance between two 3D lines
     */
    template <typename Real>
    class DistLine3Line3
    {
    public:
        // Input
        Line3<Real> m_Line1;
        Line3<Real> m_Line2;

        // Results
        Real              m_DistanceSquared = -1.0;
        bool              m_bIsParallel     = false;
        glm::vec<3, Real> m_Line1ClosestPoint{};
        Real              m_Line1Parameter = 0;
        glm::vec<3, Real> m_Line2ClosestPoint{};
        Real              m_Line2Parameter = 0;

        DistLine3Line3( const Line3<Real>& Line1In, const Line3<Real>& Line2In )
             : m_Line1( Line1In ), m_Line2( Line2In )
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
            if ( m_DistanceSquared >= 0 )
            {
                return m_DistanceSquared;
            }

            glm::vec<3, Real> kDiff = m_Line1.Origin - m_Line2.Origin;
            Real              a01   = -glm::dot( m_Line1.Direction, m_Line2.Direction );
            Real              b0    = glm::dot( kDiff, m_Line1.Direction );
            Real              c     = glm::length2( kDiff );
            Real              det   = std::abs( static_cast<Real>( 1 ) - a01 * a01 );
            Real              b1;
            Real              s0;
            Real              s1;
            Real              sqrDist;

            if ( det >= ZeroTolerance<Real> )
            {
                b1 = -glm::dot( kDiff, m_Line2.Direction );
                s1 = a01 * b0 - b1;

                // Two interior points are closest.
                Real invDet = static_cast<Real>( 1 ) / det;
                s0          = ( a01 * b1 - b0 ) * invDet;
                s1 *= invDet;
                sqrDist = s0 * ( s0 + a01 * s1 + static_cast<Real>( 2 ) * b0 ) +
                          s1 * ( a01 * s0 + s1 + static_cast<Real>( 2 ) * b1 ) + c;
                m_Line1ClosestPoint = m_Line1.Origin + s0 * m_Line1.Direction;
                m_Line2ClosestPoint = m_Line2.Origin + s1 * m_Line2.Direction;
                m_Line1Parameter    = s0;
                m_Line2Parameter    = s1;
                m_bIsParallel       = false;
            }
            else
            {
                // Lines are parallel, closest pair at line1 origin
                m_Line1Parameter    = static_cast<Real>( 0 );
                m_Line1ClosestPoint = m_Line1.Origin;
                m_Line2Parameter    = m_Line2.Project( m_Line1.Origin );
                m_Line2ClosestPoint = m_Line2.PointAt( m_Line2Parameter );
                sqrDist             = m_Line1.DistanceSquared( m_Line2ClosestPoint );
                m_bIsParallel       = true;
            }

            // Account for numerical round-off errors.
            m_DistanceSquared = ( sqrDist < static_cast<Real>( 0 ) ) ? static_cast<Real>( 0 ) : sqrDist;
            return m_DistanceSquared;
        }
    };

    using DistLine3Line3f = DistLine3Line3<float>;
    using DistLine3Line3d = DistLine3Line3<double>;
} // namespace Desert::Geometry
