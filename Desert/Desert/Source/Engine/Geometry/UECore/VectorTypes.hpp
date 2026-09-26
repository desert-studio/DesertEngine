// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/VectorTypes.h:127-131,160-213,253-259,355-362,
// adapted: only the TVector/TVector2 helpers the FDynamicMesh3 port calls; UE::Math::TVector is our shim
// TVector; component-wise Min/Max come from Core's Math/Vector.h (FVector::Min/Max) as free functions here.
#pragma once

#include "Engine/Geometry/UECore/MathUtil.hpp"

namespace Desert::Geometry
{
    template <typename T>
    TVector2<T> Lerp( const TVector2<T>& A, const TVector2<T>& B, T Alpha )
    {
        T OneMinusAlpha = (T)1 - Alpha;
        return TVector2<T>( OneMinusAlpha * A.X + Alpha * B.X, OneMinusAlpha * A.Y + Alpha * B.Y );
    }

    template <typename T>
    constexpr bool IsNormalized( const TVector<T>& Vector, const T Tolerance = TMathUtil<T>::ZeroTolerance )
    {
        return TMathUtil<T>::Abs( ( Vector.X * Vector.X + Vector.Y * Vector.Y + Vector.Z * Vector.Z ) - 1 ) <
               Tolerance;
    }

    template <typename T>
    T Normalize( TVector<T>& Vector, const T Epsilon = 0 )
    {
        T length = Vector.Length();
        if ( length > Epsilon )
        {
            T invLength = ( (T)1 ) / length;
            Vector.X *= invLength;
            Vector.Y *= invLength;
            Vector.Z *= invLength;
            return length;
        }
        Vector.X = Vector.Y = Vector.Z = (T)0;
        return (T)0;
    }

    template <typename T>
    constexpr TVector<T> Normalized( const TVector<T>& Vector, const T Epsilon = 0 )
    {
        T length = Vector.Length();
        if ( length > Epsilon )
        {
            T invLength = ( (T)1 ) / length;
            return TVector<T>( Vector.X * invLength, Vector.Y * invLength, Vector.Z * invLength );
        }
        return TVector<T>( (T)0, (T)0, (T)0 );
    }

    template <typename T>
    T Distance( const TVector<T>& V1, const TVector<T>& V2 )
    {
        T dx = V2.X - V1.X;
        T dy = V2.Y - V1.Y;
        T dz = V2.Z - V1.Z;
        return TMathUtil<T>::Sqrt( dx * dx + dy * dy + dz * dz );
    }

    template <typename T>
    T DistanceSquared( const TVector<T>& V1, const TVector<T>& V2 )
    {
        T dx = V2.X - V1.X;
        T dy = V2.Y - V1.Y;
        T dz = V2.Z - V1.Z;
        return dx * dx + dy * dy + dz * dz;
    }

    template <typename T>
    T AngleR( const TVector<T>& V1, const TVector<T>& V2 )
    {
        T DotVal     = V1.Dot( V2 );
        T ClampedDot = ( DotVal < (T)-1 ) ? (T)-1 : ( ( DotVal > (T)1 ) ? (T)1 : DotVal );
        return TMathUtil<T>::ACos( ClampedDot );
    }

    template <typename T>
    TVector<T> Lerp( const TVector<T>& A, const TVector<T>& B, T Alpha )
    {
        T OneMinusAlpha = (T)1 - Alpha;
        return TVector<T>( OneMinusAlpha * A.X + Alpha * B.X, OneMinusAlpha * A.Y + Alpha * B.Y,
                           OneMinusAlpha * A.Z + Alpha * B.Z );
    }

    template <typename T>
    TVector<T> Min( const TVector<T>& A, const TVector<T>& B )
    {
        return TVector<T>( std::min( A.X, B.X ), std::min( A.Y, B.Y ), std::min( A.Z, B.Z ) );
    }

    template <typename T>
    TVector<T> Max( const TVector<T>& A, const TVector<T>& B )
    {
        return TVector<T>( std::max( A.X, B.X ), std::max( A.Y, B.Y ), std::max( A.Z, B.Z ) );
    }
} // namespace Desert::Geometry
