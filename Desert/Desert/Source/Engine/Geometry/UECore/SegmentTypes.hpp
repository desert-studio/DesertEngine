// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/SegmentTypes.h:446-477,504-517,556-584,617-622,
// 661-663, adapted: namespace Desert::Geometry; only the TSegment3 members FMeshSurfacePath::EmbedSimplePath calls
// (two-point construction, end points, point distance, unit-range projection). TSegment2 is not ported.
#pragma once

#include "Engine/Geometry/UECore/MathUtil.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    /** 3D line segment stored as Center point, normalized Direction vector, and scalar Extent (half the length).
     */
    template <typename T>
    struct TSegment3
    {
        TVector<T> Center    = TVector<T>::Zero();
        TVector<T> Direction = TVector<T>::UnitX();
        T          Extent    = (T)0;

        TSegment3() = default;

        TSegment3( const TVector<T>& Point0, const TVector<T>& Point1 )
        {
            Center    = T( .5 ) * ( Point0 + Point1 );
            Direction = Point1 - Point0;
            Extent    = T( .5 ) * Normalize( Direction );
        }

        TVector<T> StartPoint() const
        {
            return Center - Extent * Direction;
        }

        TVector<T> EndPoint() const
        {
            return Center + Extent * Direction;
        }

        /** @return minimum squared distance from Point to the segment */
        T DistanceSquared( const TVector<T>& Point ) const
        {
            T DistParameter;
            return DistanceSquared( Point, DistParameter );
        }

        /** @param DistParameterOut calculated distance parameter in range [-Extent,Extent] */
        T DistanceSquared( const TVector<T>& Point, T& DistParameterOut ) const
        {
            DistParameterOut = ( Point - Center ).Dot( Direction );
            if ( DistParameterOut >= Extent )
            {
                DistParameterOut = Extent;
                return Desert::Geometry::DistanceSquared( Point, EndPoint() );
            }
            else if ( DistParameterOut <= -Extent )
            {
                DistParameterOut = -Extent;
                return Desert::Geometry::DistanceSquared( Point, StartPoint() );
            }
            const TVector<T> ProjectedPt = Center + DistParameterOut * Direction;
            return Desert::Geometry::DistanceSquared( ProjectedPt, Point );
        }

        /** @return projection of QueryPoint onto the segment, as a parameter in [0,1] from StartPoint to EndPoint
         */
        T ProjectUnitRange( const TVector<T>& QueryPoint ) const
        {
            const T ProjT = ( QueryPoint - Center ).Dot( Direction );
            const T Alpha = ( ( ProjT / Extent ) + (T)1 ) * (T)0.5;
            return TMathUtil<T>::Clamp( Alpha, (T)0, (T)1 );
        }
    };

    using FSegment3f = TSegment3<float>;
    using FSegment3d = TSegment3<double>;
} // namespace Desert::Geometry
