// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/LineTypes.h:140-222, adapted: namespace
// Desert::Geometry; only Line3 (MeshBevel, the inset solve and DistLine3Line3d use it). TLine2 is not ported.
#pragma once

#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    /**
     * Line3 is a three-dimensional infinite line.
     * The line is stored in (Center,Direction) form.
     */
    template <typename T>
    struct Line3
    {
        /** Origin / Center Point of Line */
        glm::vec<3, T> Origin = glm::vec<3, T>( 0 );
        /** Direction of Line, Normalized */
        glm::vec<3, T> Direction = glm::vec<3, T>( 1, 0, 0 );

        /** Construct default line along X axis */
        Line3() = default;

        /** Construct line with given Origin and Direction */
        Line3( const glm::vec<3, T>& OriginIn, const glm::vec<3, T>& DirectionIn )
             : Origin( OriginIn ), Direction( DirectionIn )
        {
        }

        /** @return line between two points */
        static Line3<T> FromPoints( const glm::vec<3, T>& Point0, const glm::vec<3, T>& Point1 )
        {
            return Line3<T>( Point0, Normalized( Point1 - Point0 ) );
        }

        /** @return point on line at given line parameter value (distance along line from origin) */
        glm::vec<3, T> PointAt( T LineParameter ) const
        {
            return Origin + LineParameter * Direction;
        }

        /** @return line parameter (ie distance from Origin) at nearest point on line to QueryPoint */
        T Project( const glm::vec<3, T>& QueryPoint ) const
        {
            return glm::dot( ( QueryPoint - Origin ), Direction );
        }

        /** @return smallest squared distance from line to QueryPoint */
        T DistanceSquared( const glm::vec<3, T>& QueryPoint ) const
        {
            const T              t    = glm::dot( ( QueryPoint - Origin ), Direction );
            const glm::vec<3, T> proj = Origin + t * Direction;
            return glm::length2( ( proj - QueryPoint ) );
        }

        /** @return nearest point on line to QueryPoint */
        glm::vec<3, T> NearestPoint( const glm::vec<3, T>& QueryPoint ) const
        {
            const T ParameterT = glm::dot( ( QueryPoint - Origin ), Direction );
            return Origin + ParameterT * Direction;
        }
    };

    using Line3d = Line3<double>;
    using Line3f = Line3<float>;
} // namespace Desert::Geometry
