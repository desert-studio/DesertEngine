// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/VectorTypes.h:127-131,160-213,253-259,355-362,
// adapted: only the TVector/TVector2 helpers the DynamicMesh3 port calls; UE::Math::TVector is our shim
// TVector; component-wise Min/Max come from Core's Math/Vector.h (FVector::Min/Max) as free functions here.
#pragma once

#include "Engine/Geometry/UECore/MathUtil.hpp"

namespace Desert::Geometry
{
    template <typename T>
    glm::vec<2, T> Lerp( const glm::vec<2, T>& A, const glm::vec<2, T>& B, T Alpha )
    {
        T OneMinusAlpha = (T)1 - Alpha;
        return glm::vec<2, T>( OneMinusAlpha * A.x + Alpha * B.x, OneMinusAlpha * A.y + Alpha * B.y );
    }

    template <typename T>
    constexpr bool IsNormalized( const glm::vec<3, T>& Vector, const T Tolerance = ZeroTolerance<T> )
    {
        return std::abs( ( Vector.x * Vector.x + Vector.y * Vector.y + Vector.z * Vector.z ) - 1 ) < Tolerance;
    }

    template <typename T>
    T Normalize( glm::vec<3, T>& Vector, const T Epsilon = 0 )
    {
        T length = glm::length( Vector );
        if ( length > Epsilon )
        {
            T invLength = ( (T)1 ) / length;
            Vector.x *= invLength;
            Vector.y *= invLength;
            Vector.z *= invLength;
            return length;
        }
        Vector.x = Vector.y = Vector.z = (T)0;
        return (T)0;
    }

    template <typename T>
    constexpr glm::vec<3, T> Normalized( const glm::vec<3, T>& Vector, const T Epsilon = 0 )
    {
        T length = glm::length( Vector );
        if ( length > Epsilon )
        {
            T invLength = ( (T)1 ) / length;
            return glm::vec<3, T>( Vector.x * invLength, Vector.y * invLength, Vector.z * invLength );
        }
        return glm::vec<3, T>( (T)0, (T)0, (T)0 );
    }

    template <typename T>
    T Distance( const glm::vec<3, T>& V1, const glm::vec<3, T>& V2 )
    {
        T dx = V2.x - V1.x;
        T dy = V2.y - V1.y;
        T dz = V2.z - V1.z;
        return std::sqrt( dx * dx + dy * dy + dz * dz );
    }

    template <typename T>
    T DistanceSquared( const glm::vec<3, T>& V1, const glm::vec<3, T>& V2 )
    {
        T dx = V2.x - V1.x;
        T dy = V2.y - V1.y;
        T dz = V2.z - V1.z;
        return dx * dx + dy * dy + dz * dz;
    }

    template <typename T>
    T AngleR( const glm::vec<3, T>& V1, const glm::vec<3, T>& V2 )
    {
        T DotVal     = glm::dot( V1, V2 );
        T ClampedDot = ( DotVal < (T)-1 ) ? (T)-1 : ( ( DotVal > (T)1 ) ? (T)1 : DotVal );
        return std::acos( ClampedDot );
    }

    template <typename T>
    glm::vec<3, T> Lerp( const glm::vec<3, T>& A, const glm::vec<3, T>& B, T Alpha )
    {
        T OneMinusAlpha = (T)1 - Alpha;
        return glm::vec<3, T>( OneMinusAlpha * A.x + Alpha * B.x, OneMinusAlpha * A.y + Alpha * B.y,
                               OneMinusAlpha * A.z + Alpha * B.z );
    }

    template <typename T>
    glm::vec<3, T> Min( const glm::vec<3, T>& A, const glm::vec<3, T>& B )
    {
        return glm::vec<3, T>( std::min( A.x, B.x ), std::min( A.y, B.y ), std::min( A.z, B.z ) );
    }

    template <typename T>
    glm::vec<3, T> Max( const glm::vec<3, T>& A, const glm::vec<3, T>& B )
    {
        return glm::vec<3, T>( std::max( A.x, B.x ), std::max( A.y, B.y ), std::max( A.z, B.z ) );
    }
} // namespace Desert::Geometry
