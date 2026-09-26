// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/BoxTypes.h:246-249,382,438,466,884-885,
// adapted: only the AxisAlignedBox3 members the DynamicMesh3 port calls (construction incl. the three-point
// constructor, Empty, Contain, Center/Extents/DiagonalLength/IsEmpty), written over the shim TVector; transform,
// distance and interval members are not ported.
#pragma once

#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    template <typename RealType>
    struct AxisAlignedBox3
    {
        glm::vec<3, RealType> Min{};
        glm::vec<3, RealType> Max{};

        AxisAlignedBox3()
             : Min( std::numeric_limits<RealType>::max(), std::numeric_limits<RealType>::max(),
                    std::numeric_limits<RealType>::max() ),
               Max( -std::numeric_limits<RealType>::max(), -std::numeric_limits<RealType>::max(),
                    -std::numeric_limits<RealType>::max() )
        {
        }

        AxisAlignedBox3( const glm::vec<3, RealType>& MinIn, const glm::vec<3, RealType>& MaxIn )
             : Min( MinIn ), Max( MaxIn )
        {
        }

        AxisAlignedBox3( const glm::vec<3, RealType>& A, const glm::vec<3, RealType>& B,
                         const glm::vec<3, RealType>& C )
             : Min( std::min( A.x, std::min( B.x, C.x ) ), std::min( A.y, std::min( B.y, C.y ) ),
                    std::min( A.z, std::min( B.z, C.z ) ) ),
               Max( std::max( A.x, std::max( B.x, C.x ) ), std::max( A.y, std::max( B.y, C.y ) ),
                    std::max( A.z, std::max( B.z, C.z ) ) )
        {
        }

        static AxisAlignedBox3<RealType> Empty()
        {
            return AxisAlignedBox3();
        }

        glm::vec<3, RealType> Center() const
        {
            return glm::vec<3, RealType>( ( Min.x + Max.x ) * (RealType)0.5, ( Min.y + Max.y ) * (RealType)0.5,
                                          ( Min.z + Max.z ) * (RealType)0.5 );
        }

        glm::vec<3, RealType> Extents() const
        {
            return ( Max - Min ) * (RealType)0.5;
        }

        RealType DiagonalLength() const
        {
            return std::sqrt( glm::length2( ( Max - Min ) ) );
        }

        bool IsEmpty() const
        {
            return Max.x < Min.x || Max.y < Min.y || Max.z < Min.z;
        }

        void Contain( const glm::vec<3, RealType>& V )
        {
            if ( V.x < Min.x )
                Min.x = V.x;
            if ( V.x > Max.x )
                Max.x = V.x;
            if ( V.y < Min.y )
                Min.y = V.y;
            if ( V.y > Max.y )
                Max.y = V.y;
            if ( V.z < Min.z )
                Min.z = V.z;
            if ( V.z > Max.z )
                Max.z = V.z;
        }

        void Contain( const AxisAlignedBox3<RealType>& Other )
        {
            Min.x = Min.x < Other.Min.x ? Min.x : Other.Min.x;
            Min.y = Min.y < Other.Min.y ? Min.y : Other.Min.y;
            Min.z = Min.z < Other.Min.z ? Min.z : Other.Min.z;
            Max.x = Max.x > Other.Max.x ? Max.x : Other.Max.x;
            Max.y = Max.y > Other.Max.y ? Max.y : Other.Max.y;
            Max.z = Max.z > Other.Max.z ? Max.z : Other.Max.z;
        }
    };

    using AxisAlignedBox3f = AxisAlignedBox3<float>;
    using AxisAlignedBox3d = AxisAlignedBox3<double>;
} // namespace Desert::Geometry
