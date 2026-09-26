// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Quaternion.h (operator*, AxisX/Y/Z, SetAxisAngleR,
// SetFromTo, SetFromRotationMatrix, Normalize) and Public/FrameTypes.h:70-160,340-440 (Frame3 constructors,
// GetAxis, X/Y/Z, Rotate, AlignAxis, ConstrainedAlignAxis, ConstrainedAlignPerpAxes), adapted: only what the
// ExpMap parameterization calls; the 3-axis constructor builds the quaternion from the columns directly (no
// TMatrix3), and ConstrainedAlignAxis takes the signed plane angle as atan2 (UE's VectorUtil::PlaneAngleSignedD is
// the same angle through acos plus a sign test).
#pragma once

#include "Engine/Geometry/UECore/MathUtil.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

#include <cmath>

namespace Desert::Geometry
{
    template <typename RealType>
    struct Quaternion
    {
        RealType x = 0, y = 0, z = 0, w = 1;

        Quaternion() = default;
        Quaternion( RealType XIn, RealType YIn, RealType ZIn, RealType WIn )
             : x( XIn ), y( YIn ), z( ZIn ), w( WIn )
        {
        }
        Quaternion( const glm::vec<3, RealType>& From, const glm::vec<3, RealType>& To )
        {
            SetFromTo( From, To );
        }

        RealType Normalize( RealType Epsilon = 0 )
        {
            const RealType Length = std::sqrt( x * x + y * y + z * z + w * w );
            if ( Length > Epsilon )
            {
                const RealType Inv = static_cast<RealType>( 1 ) / Length;
                x *= Inv;
                y *= Inv;
                z *= Inv;
                w *= Inv;
                return Length;
            }
            x = y = z = w = 0;
            return 0;
        }
        glm::vec<3, RealType> AxisX() const
        {
            const RealType twoY = 2 * y;
            const RealType twoZ = 2 * z;
            return glm::vec<3, RealType>( 1 - ( twoY * y + twoZ * z ), twoY * x + twoZ * w, twoZ * x - twoY * w );
        }
        glm::vec<3, RealType> AxisY() const
        {
            const RealType twoX = 2 * x;
            const RealType twoY = 2 * y;
            const RealType twoZ = 2 * z;
            return glm::vec<3, RealType>( twoY * x - twoZ * w, 1 - ( twoX * x + twoZ * z ), twoZ * y + twoX * w );
        }
        glm::vec<3, RealType> AxisZ() const
        {
            const RealType twoX = 2 * x;
            const RealType twoY = 2 * y;
            const RealType twoZ = 2 * z;
            return glm::vec<3, RealType>( twoZ * x + twoY * w, twoZ * y - twoX * w, 1 - ( twoX * x + twoY * y ) );
        }
        void SetAxisAngleR( const glm::vec<3, RealType>& Axis, RealType AngleRad )
        {
            const RealType Half = static_cast<RealType>( 0.5 ) * AngleRad;
            const RealType Sn   = std::sin( Half );
            w                   = std::cos( Half );
            x                   = Sn * Axis.x;
            y                   = Sn * Axis.y;
            z                   = Sn * Axis.z;
        }
        void SetFromTo( const glm::vec<3, RealType>& From, const glm::vec<3, RealType>& To )
        {
            const glm::vec<3, RealType> from     = Normalized( From );
            const glm::vec<3, RealType> to       = Normalized( To );
            const glm::vec<3, RealType> bisector = Normalized( from + to, ZeroTolerance<RealType> );
            w                                    = glm::dot( from, bisector );
            if ( w != 0 )
            {
                const glm::vec<3, RealType> cross = glm::cross( from, bisector );
                x                                 = cross.x;
                y                                 = cross.y;
                z                                 = cross.z;
            }
            else if ( std::abs( from.x ) >= std::abs( from.y ) )
            {
                const RealType invLength =
                     static_cast<RealType>( 1 ) / std::sqrt( from.x * from.x + from.z * from.z );
                x = -from.z * invLength;
                y = 0;
                z = +from.x * invLength;
            }
            else
            {
                const RealType invLength =
                     static_cast<RealType>( 1 ) / std::sqrt( from.y * from.y + from.z * from.z );
                x = 0;
                y = +from.z * invLength;
                z = -from.y * invLength;
            }
            Normalize();
        }
        /** UE SetFromRotationMatrix of TMatrix3(AxisX, AxisY, AxisZ, bRows = false): M(r, c) = Axis[c][r]. */
        void SetFromAxes( const glm::vec<3, RealType>& AxisXIn, const glm::vec<3, RealType>& AxisYIn,
                          const glm::vec<3, RealType>& AxisZIn )
        {
            const glm::vec<3, RealType> Cols[3] = { AxisXIn, AxisYIn, AxisZIn };
            auto                    M       = [&Cols]( int R, int C ) { return Cols[C][R]; };
            const RealType          trace   = M( 0, 0 ) + M( 1, 1 ) + M( 2, 2 );
            if ( trace > 0 )
            {
                RealType root = std::sqrt( trace + 1 );
                w             = static_cast<RealType>( 0.5 ) * root;
                root          = static_cast<RealType>( 0.5 ) / root;
                x             = ( M( 2, 1 ) - M( 1, 2 ) ) * root;
                y             = ( M( 0, 2 ) - M( 2, 0 ) ) * root;
                z             = ( M( 1, 0 ) - M( 0, 1 ) ) * root;
            }
            else
            {
                const int Next[3] = { 1, 2, 0 };
                int       i       = 0;
                if ( M( 1, 1 ) > M( 0, 0 ) )
                    i = 1;
                if ( M( 2, 2 ) > M( i, i ) )
                    i = 2;
                const int         j    = Next[i];
                const int         k    = Next[j];
                RealType          root = std::sqrt( M( i, i ) - M( j, j ) - M( k, k ) + 1 );
                glm::vec<3, RealType> quat( x, y, z );
                quat[i] = static_cast<RealType>( 0.5 ) * root;
                root    = static_cast<RealType>( 0.5 ) / root;
                w       = ( M( k, j ) - M( j, k ) ) * root;
                quat[j] = ( M( j, i ) + M( i, j ) ) * root;
                quat[k] = ( M( k, i ) + M( i, k ) ) * root;
                x       = quat.x;
                y       = quat.y;
                z       = quat.z;
            }
            Normalize();
        }
    };

    template <typename RealType>
    Quaternion<RealType> operator*( const Quaternion<RealType>& A, const Quaternion<RealType>& B )
    {
        return Quaternion<RealType>(
             A.w * B.x + A.x * B.w + A.y * B.z - A.z * B.y, A.w * B.y + A.y * B.w + A.z * B.x - A.x * B.z,
             A.w * B.z + A.z * B.w + A.x * B.y - A.y * B.x, A.w * B.w - A.x * B.x - A.y * B.y - A.z * B.z );
    }

    template <typename RealType>
    struct Frame3
    {
        glm::vec<3, RealType> Origin = glm::vec<3, RealType>( 0 );
        Quaternion<RealType>  Rotation;

        Frame3() = default;
        Frame3( const glm::vec<3, RealType>& OriginIn, const glm::vec<3, RealType>& SetZ ) : Origin( OriginIn )
        {
            Rotation.SetFromTo( glm::vec<3, RealType>( 0, 0, 1 ), SetZ );
        }
        Frame3( const glm::vec<3, RealType>& OriginIn, const glm::vec<3, RealType>& XIn,
                const glm::vec<3, RealType>& YIn, const glm::vec<3, RealType>& ZIn )
             : Origin( OriginIn )
        {
            Rotation.SetFromAxes( XIn, YIn, ZIn );
        }

        glm::vec<3, RealType> GetAxis( int AxisIndex ) const
        {
            if ( AxisIndex == 0 )
                return Rotation.AxisX();
            return AxisIndex == 1 ? Rotation.AxisY() : Rotation.AxisZ();
        }
        glm::vec<3, RealType> X() const
        {
            return Rotation.AxisX();
        }
        glm::vec<3, RealType> Y() const
        {
            return Rotation.AxisY();
        }
        glm::vec<3, RealType> Z() const
        {
            return Rotation.AxisZ();
        }
        void Rotate( const Quaternion<RealType>& Quat )
        {
            Quaternion<RealType> NewRotation = Quat * Rotation;
            if ( NewRotation.Normalize() > 0 )
                Rotation = NewRotation;
        }
        void AlignAxis( int AxisIndex, const glm::vec<3, RealType>& ToDirection )
        {
            Rotate( Quaternion<RealType>( GetAxis( AxisIndex ), ToDirection ) );
        }
        void ConstrainedAlignAxis( int AxisIndex, const glm::vec<3, RealType>& ToDirection,
                                   const glm::vec<3, RealType>& AroundVector )
        {
            const glm::vec<3, RealType> N    = Normalized( AroundVector );
            glm::vec<3, RealType>       From = GetAxis( AxisIndex );
            glm::vec<3, RealType>       To   = ToDirection;
            From                             = Normalized( From - N * glm::dot( From, N ) );
            To                               = Normalized( To - N * glm::dot( To, N ) );
            Quaternion<RealType> RelRotation;
            RelRotation.SetAxisAngleR( AroundVector,
                                       std::atan2( glm::dot( glm::cross( From, To ), N ), glm::dot( From, To ) ) );
            Rotate( RelRotation );
        }
        void ConstrainedAlignPerpAxes( int PerpAxis1, int PerpAxis2, int NormalAxis,
                                       const glm::vec<3, RealType>& UpAxis,
                                       const glm::vec<3, RealType>& FallbackAxis, RealType UpDotTolerance )
        {
            const glm::vec<3, RealType>  NormalVec = GetAxis( NormalAxis );
            const glm::vec<3, RealType>& TargetAxis =
                 ( std::abs( glm::dot( NormalVec, UpAxis ) ) > UpDotTolerance ) ? FallbackAxis : UpAxis;
            const RealType DotA    = glm::dot( GetAxis( PerpAxis1 ), TargetAxis );
            const RealType DotB    = glm::dot( GetAxis( PerpAxis2 ), TargetAxis );
            const int      UseAxis = ( std::abs( DotA ) > std::abs( DotB ) ) ? 0 : 1;
            const RealType UseSign = ( UseAxis == 0 ? DotA : DotB ) < 0 ? -1 : 1;
            // UE passes UseAxis (0/1), not PerpAxis1/2; equal for the (0, 1, 2) the ExpMap uses.
            ConstrainedAlignAxis( UseAxis, TargetAxis * UseSign, NormalVec );
        }
    };

    using Quaterniond = Quaternion<double>;
    using Frame3d     = Frame3<double>;
} // namespace Desert::Geometry
