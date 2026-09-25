// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/LineTypes.h:140-222, adapted: namespace
// Desert::Geometry; only TLine3 (FMeshBevel, the inset solve and FDistLine3Line3d use it). TLine2 is not ported.
#pragma once

#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    /**
     * TLine3 is a three-dimensional infinite line.
     * The line is stored in (Center,Direction) form.
     */
    template <typename T>
    struct TLine3
    {
        /** Origin / Center Point of Line */
        TVector<T> Origin = TVector<T>::Zero();
        /** Direction of Line, Normalized */
        TVector<T> Direction = TVector<T>::UnitX();

        /** Construct default line along X axis */
        TLine3() = default;

        /** Construct line with given Origin and Direction */
        TLine3( const TVector<T>& OriginIn, const TVector<T>& DirectionIn )
             : Origin( OriginIn ), Direction( DirectionIn )
        {
        }

        /** @return line between two points */
        static TLine3<T> FromPoints( const TVector<T>& Point0, const TVector<T>& Point1 )
        {
            return TLine3<T>( Point0, Normalized( Point1 - Point0 ) );
        }

        /** @return point on line at given line parameter value (distance along line from origin) */
        TVector<T> PointAt( T LineParameter ) const
        {
            return Origin + LineParameter * Direction;
        }

        /** @return line parameter (ie distance from Origin) at nearest point on line to QueryPoint */
        T Project( const TVector<T>& QueryPoint ) const
        {
            return ( QueryPoint - Origin ).Dot( Direction );
        }

        /** @return smallest squared distance from line to QueryPoint */
        T DistanceSquared( const TVector<T>& QueryPoint ) const
        {
            const T          t    = ( QueryPoint - Origin ).Dot( Direction );
            const TVector<T> proj = Origin + t * Direction;
            return ( proj - QueryPoint ).SquaredLength();
        }

        /** @return nearest point on line to QueryPoint */
        TVector<T> NearestPoint( const TVector<T>& QueryPoint ) const
        {
            const T ParameterT = ( QueryPoint - Origin ).Dot( Direction );
            return Origin + ParameterT * Direction;
        }
    };

    using FLine3d = TLine3<double>;
    using FLine3f = TLine3<float>;
} // namespace Desert::Geometry
