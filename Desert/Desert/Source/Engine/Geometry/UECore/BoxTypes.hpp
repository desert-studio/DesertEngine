// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/BoxTypes.h:246-249,382,438,466,884-885,
// adapted: only the TAxisAlignedBox3 members the FDynamicMesh3 port calls (construction incl. the three-point
// constructor, Empty, Contain, Center/Extents/DiagonalLength/IsEmpty), written over the shim TVector; transform,
// distance and interval members are not ported.
#pragma once

#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    template <typename RealType>
    struct TAxisAlignedBox3
    {
        TVector<RealType> Min;
        TVector<RealType> Max;

        TAxisAlignedBox3()
             : Min( TMathUtil<RealType>::MaxReal, TMathUtil<RealType>::MaxReal, TMathUtil<RealType>::MaxReal ),
               Max( -TMathUtil<RealType>::MaxReal, -TMathUtil<RealType>::MaxReal, -TMathUtil<RealType>::MaxReal )
        {
        }

        TAxisAlignedBox3( const TVector<RealType>& MinIn, const TVector<RealType>& MaxIn )
             : Min( MinIn ), Max( MaxIn )
        {
        }

        TAxisAlignedBox3( const TVector<RealType>& A, const TVector<RealType>& B, const TVector<RealType>& C )
             : Min( std::min( A.X, std::min( B.X, C.X ) ), std::min( A.Y, std::min( B.Y, C.Y ) ),
                    std::min( A.Z, std::min( B.Z, C.Z ) ) ),
               Max( std::max( A.X, std::max( B.X, C.X ) ), std::max( A.Y, std::max( B.Y, C.Y ) ),
                    std::max( A.Z, std::max( B.Z, C.Z ) ) )
        {
        }

        static TAxisAlignedBox3<RealType> Empty()
        {
            return TAxisAlignedBox3();
        }

        TVector<RealType> Center() const
        {
            return TVector<RealType>( ( Min.X + Max.X ) * (RealType)0.5, ( Min.Y + Max.Y ) * (RealType)0.5,
                                      ( Min.Z + Max.Z ) * (RealType)0.5 );
        }

        TVector<RealType> Extents() const
        {
            return ( Max - Min ) * (RealType)0.5;
        }

        RealType DiagonalLength() const
        {
            return TMathUtil<RealType>::Sqrt( ( Max - Min ).SquaredLength() );
        }

        bool IsEmpty() const
        {
            return Max.X < Min.X || Max.Y < Min.Y || Max.Z < Min.Z;
        }

        void Contain( const TVector<RealType>& V )
        {
            if ( V.X < Min.X )
                Min.X = V.X;
            if ( V.X > Max.X )
                Max.X = V.X;
            if ( V.Y < Min.Y )
                Min.Y = V.Y;
            if ( V.Y > Max.Y )
                Max.Y = V.Y;
            if ( V.Z < Min.Z )
                Min.Z = V.Z;
            if ( V.Z > Max.Z )
                Max.Z = V.Z;
        }

        void Contain( const TAxisAlignedBox3<RealType>& Other )
        {
            Min.X = Min.X < Other.Min.X ? Min.X : Other.Min.X;
            Min.Y = Min.Y < Other.Min.Y ? Min.Y : Other.Min.Y;
            Min.Z = Min.Z < Other.Min.Z ? Min.Z : Other.Min.Z;
            Max.X = Max.X > Other.Max.X ? Max.X : Other.Max.X;
            Max.Y = Max.Y > Other.Max.Y ? Max.Y : Other.Max.Y;
            Max.Z = Max.Z > Other.Max.Z ? Max.Z : Other.Max.Z;
        }
    };

    using FAxisAlignedBox3f = TAxisAlignedBox3<float>;
    using FAxisAlignedBox3d = TAxisAlignedBox3<double>;
} // namespace Desert::Geometry
