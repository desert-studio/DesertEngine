// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Quaternion.h (operator*, AxisX/Y/Z, SetAxisAngleR,
// SetFromTo, SetFromRotationMatrix, Normalize) and Public/FrameTypes.h:70-160,340-440 (TFrame3 constructors,
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
    struct TQuaternion
    {
        RealType X = 0, Y = 0, Z = 0, W = 1;

        TQuaternion() = default;
        TQuaternion( RealType XIn, RealType YIn, RealType ZIn, RealType WIn )
             : X( XIn ), Y( YIn ), Z( ZIn ), W( WIn )
        {
        }
        TQuaternion( const TVector<RealType>& From, const TVector<RealType>& To )
        {
            SetFromTo( From, To );
        }

        RealType Normalize( RealType Epsilon = 0 )
        {
            const RealType Length = std::sqrt( X * X + Y * Y + Z * Z + W * W );
            if ( Length > Epsilon )
            {
                const RealType Inv = static_cast<RealType>( 1 ) / Length;
                X *= Inv;
                Y *= Inv;
                Z *= Inv;
                W *= Inv;
                return Length;
            }
            X = Y = Z = W = 0;
            return 0;
        }
        TVector<RealType> AxisX() const
        {
            const RealType twoY = 2 * Y;
            const RealType twoZ = 2 * Z;
            return TVector<RealType>( 1 - ( twoY * Y + twoZ * Z ), twoY * X + twoZ * W, twoZ * X - twoY * W );
        }
        TVector<RealType> AxisY() const
        {
            const RealType twoX = 2 * X;
            const RealType twoY = 2 * Y;
            const RealType twoZ = 2 * Z;
            return TVector<RealType>( twoY * X - twoZ * W, 1 - ( twoX * X + twoZ * Z ), twoZ * Y + twoX * W );
        }
        TVector<RealType> AxisZ() const
        {
            const RealType twoX = 2 * X;
            const RealType twoY = 2 * Y;
            const RealType twoZ = 2 * Z;
            return TVector<RealType>( twoZ * X + twoY * W, twoZ * Y - twoX * W, 1 - ( twoX * X + twoY * Y ) );
        }
        void SetAxisAngleR( const TVector<RealType>& Axis, RealType AngleRad )
        {
            const RealType Half = static_cast<RealType>( 0.5 ) * AngleRad;
            const RealType Sn   = std::sin( Half );
            W                   = std::cos( Half );
            X                   = Sn * Axis.X;
            Y                   = Sn * Axis.Y;
            Z                   = Sn * Axis.Z;
        }
        void SetFromTo( const TVector<RealType>& From, const TVector<RealType>& To )
        {
            const TVector<RealType> from = Normalized( From );
            const TVector<RealType> to   = Normalized( To );
            const TVector<RealType> bisector = Normalized( from + to, TMathUtil<RealType>::ZeroTolerance );
            W                                = from.Dot( bisector );
            if ( W != 0 )
            {
                const TVector<RealType> cross = from.Cross( bisector );
                X                             = cross.X;
                Y                             = cross.Y;
                Z                             = cross.Z;
            }
            else if ( std::abs( from.X ) >= std::abs( from.Y ) )
            {
                const RealType invLength = static_cast<RealType>( 1 ) / std::sqrt( from.X * from.X + from.Z * from.Z );
                X                        = -from.Z * invLength;
                Y                        = 0;
                Z                        = +from.X * invLength;
            }
            else
            {
                const RealType invLength = static_cast<RealType>( 1 ) / std::sqrt( from.Y * from.Y + from.Z * from.Z );
                X                        = 0;
                Y                        = +from.Z * invLength;
                Z                        = -from.Y * invLength;
            }
            Normalize();
        }
        /** UE SetFromRotationMatrix of TMatrix3(AxisX, AxisY, AxisZ, bRows = false): M(r, c) = Axis[c][r]. */
        void SetFromAxes( const TVector<RealType>& AxisXIn, const TVector<RealType>& AxisYIn,
                          const TVector<RealType>& AxisZIn )
        {
            const TVector<RealType> Cols[3] = { AxisXIn, AxisYIn, AxisZIn };
            auto                    M       = [&Cols]( int R, int C ) { return Cols[C][R]; };
            const RealType          trace   = M( 0, 0 ) + M( 1, 1 ) + M( 2, 2 );
            if ( trace > 0 )
            {
                RealType root = std::sqrt( trace + 1 );
                W             = static_cast<RealType>( 0.5 ) * root;
                root          = static_cast<RealType>( 0.5 ) / root;
                X             = ( M( 2, 1 ) - M( 1, 2 ) ) * root;
                Y             = ( M( 0, 2 ) - M( 2, 0 ) ) * root;
                Z             = ( M( 1, 0 ) - M( 0, 1 ) ) * root;
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
                TVector<RealType> quat( X, Y, Z );
                quat[i] = static_cast<RealType>( 0.5 ) * root;
                root    = static_cast<RealType>( 0.5 ) / root;
                W       = ( M( k, j ) - M( j, k ) ) * root;
                quat[j] = ( M( j, i ) + M( i, j ) ) * root;
                quat[k] = ( M( k, i ) + M( i, k ) ) * root;
                X       = quat.X;
                Y       = quat.Y;
                Z       = quat.Z;
            }
            Normalize();
        }
    };

    template <typename RealType>
    TQuaternion<RealType> operator*( const TQuaternion<RealType>& A, const TQuaternion<RealType>& B )
    {
        return TQuaternion<RealType>(
             A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y, A.W * B.Y + A.Y * B.W + A.Z * B.X - A.X * B.Z,
             A.W * B.Z + A.Z * B.W + A.X * B.Y - A.Y * B.X, A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z );
    }

    template <typename RealType>
    struct TFrame3
    {
        TVector<RealType>     Origin = TVector<RealType>::Zero();
        TQuaternion<RealType> Rotation;

        TFrame3() = default;
        TFrame3( const TVector<RealType>& OriginIn, const TVector<RealType>& SetZ ) : Origin( OriginIn )
        {
            Rotation.SetFromTo( TVector<RealType>::UnitZ(), SetZ );
        }
        TFrame3( const TVector<RealType>& OriginIn, const TVector<RealType>& XIn, const TVector<RealType>& YIn,
                 const TVector<RealType>& ZIn )
             : Origin( OriginIn )
        {
            Rotation.SetFromAxes( XIn, YIn, ZIn );
        }

        TVector<RealType> GetAxis( int AxisIndex ) const
        {
            if ( AxisIndex == 0 )
                return Rotation.AxisX();
            return AxisIndex == 1 ? Rotation.AxisY() : Rotation.AxisZ();
        }
        TVector<RealType> X() const
        {
            return Rotation.AxisX();
        }
        TVector<RealType> Y() const
        {
            return Rotation.AxisY();
        }
        TVector<RealType> Z() const
        {
            return Rotation.AxisZ();
        }
        void Rotate( const TQuaternion<RealType>& Quat )
        {
            TQuaternion<RealType> NewRotation = Quat * Rotation;
            if ( NewRotation.Normalize() > 0 )
                Rotation = NewRotation;
        }
        void AlignAxis( int AxisIndex, const TVector<RealType>& ToDirection )
        {
            Rotate( TQuaternion<RealType>( GetAxis( AxisIndex ), ToDirection ) );
        }
        void ConstrainedAlignAxis( int AxisIndex, const TVector<RealType>& ToDirection,
                                   const TVector<RealType>& AroundVector )
        {
            const TVector<RealType> N    = Normalized( AroundVector );
            TVector<RealType>       From = GetAxis( AxisIndex );
            TVector<RealType>       To   = ToDirection;
            From                         = Normalized( From - N * From.Dot( N ) );
            To                           = Normalized( To - N * To.Dot( N ) );
            TQuaternion<RealType> RelRotation;
            RelRotation.SetAxisAngleR( AroundVector, std::atan2( From.Cross( To ).Dot( N ), From.Dot( To ) ) );
            Rotate( RelRotation );
        }
        void ConstrainedAlignPerpAxes( int PerpAxis1, int PerpAxis2, int NormalAxis,
                                       const TVector<RealType>& UpAxis, const TVector<RealType>& FallbackAxis,
                                       RealType UpDotTolerance )
        {
            const TVector<RealType>  NormalVec = GetAxis( NormalAxis );
            const TVector<RealType>& TargetAxis =
                 ( std::abs( NormalVec.Dot( UpAxis ) ) > UpDotTolerance ) ? FallbackAxis : UpAxis;
            const RealType DotA    = GetAxis( PerpAxis1 ).Dot( TargetAxis );
            const RealType DotB    = GetAxis( PerpAxis2 ).Dot( TargetAxis );
            const int      UseAxis = ( std::abs( DotA ) > std::abs( DotB ) ) ? 0 : 1;
            const RealType UseSign = ( UseAxis == 0 ? DotA : DotB ) < 0 ? -1 : 1;
            // UE passes UseAxis (0/1), not PerpAxis1/2; equal for the (0, 1, 2) the ExpMap uses.
            ConstrainedAlignAxis( UseAxis, TargetAxis * UseSign, NormalVec );
        }
    };

    using FQuaterniond = TQuaternion<double>;
    using FFrame3d     = TFrame3<double>;
} // namespace Desert::Geometry
