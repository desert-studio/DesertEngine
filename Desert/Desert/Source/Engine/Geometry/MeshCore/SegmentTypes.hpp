// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/SegmentTypes.h:446-477,504-517,556-584,617-622,
// 661-663, adapted: namespace Desert::Geometry; only the Segment3 members MeshSurfacePath::EmbedSimplePath calls
// (two-point construction, end points, point distance, unit-range projection). TSegment2 is not ported.
#pragma once

#include "Engine/Geometry/MeshCore/MathUtil.hpp"
#include "Engine/Geometry/MeshCore/VectorTypes.hpp"

#include <algorithm>

namespace Desert::Geometry
{
    /** 3D line segment stored as Center point, normalized Direction vector, and scalar Extent (half the length).
     */
    template <typename T>
    struct Segment3
    {
        glm::vec<3, T> Center    = glm::vec<3, T>( 0 );
        glm::vec<3, T> Direction = glm::vec<3, T>( 1, 0, 0 );
        T              Extent    = static_cast<T>( 0 );

        Segment3() = default;

        // Extent's initializer normalizes Direction in place; declaration order puts Direction first.
        Segment3( const glm::vec<3, T>& Point0, const glm::vec<3, T>& Point1 )
             : Center( T( .5 ) * ( Point0 + Point1 ) ), Direction( Point1 - Point0 ),
               Extent( T( .5 ) * Normalize( Direction ) )
        {
        }

        glm::vec<3, T> StartPoint() const
        {
            return Center - Extent * Direction;
        }

        glm::vec<3, T> EndPoint() const
        {
            return Center + Extent * Direction;
        }

        /** @return minimum squared distance from Point to the segment */
        T DistanceSquared( const glm::vec<3, T>& Point ) const
        {
            T DistParameter;
            return DistanceSquared( Point, DistParameter );
        }

        /** @param DistParameterOut calculated distance parameter in range [-Extent,Extent] */
        T DistanceSquared( const glm::vec<3, T>& Point, T& DistParameterOut ) const
        {
            DistParameterOut = glm::dot( ( Point - Center ), Direction );
            if ( DistParameterOut >= Extent )
            {
                DistParameterOut = Extent;
                return Desert::Geometry::DistanceSquared( Point, EndPoint() );
            }
            if ( DistParameterOut <= -Extent )
            {
                DistParameterOut = -Extent;
                return Desert::Geometry::DistanceSquared( Point, StartPoint() );
            }
            const glm::vec<3, T> ProjectedPt = Center + DistParameterOut * Direction;
            return Desert::Geometry::DistanceSquared( ProjectedPt, Point );
        }

        /** @return projection of QueryPoint onto the segment, as a parameter in [0,1] from StartPoint to EndPoint
         */
        T ProjectUnitRange( const glm::vec<3, T>& QueryPoint ) const
        {
            const T ProjT = glm::dot( ( QueryPoint - Center ), Direction );
            const T Alpha = ( ( ProjT / Extent ) + static_cast<T>( 1 ) ) * static_cast<T>( 0.5 );
            return std::clamp<T>( Alpha, static_cast<T>( 0 ), static_cast<T>( 1 ) );
        }
    };

    using Segment3f = Segment3<float>;
    using Segment3d = Segment3<double>;
} // namespace Desert::Geometry
