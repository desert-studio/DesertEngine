// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/VectorUtil.h:42-56,70-109,148-178,476-489,540-552,
// 612-636, adapted: only the functions the FDynamicMesh3 port and MeshTangents call; namespace
// Desert::Geometry::VectorUtil.
#pragma once

#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry::VectorUtil
{
    template <typename RealType>
    inline bool IsFinite( const TVector2<RealType>& V )
    {
        return TMathUtil<RealType>::IsFinite( V.X ) && TMathUtil<RealType>::IsFinite( V.Y );
    }

    template <typename RealType>
    inline bool IsFinite( const TVector<RealType>& V )
    {
        return TMathUtil<RealType>::IsFinite( V.X ) && TMathUtil<RealType>::IsFinite( V.Y ) &&
               TMathUtil<RealType>::IsFinite( V.Z );
    }

    template <typename RealType>
    inline RealType Clamp( RealType Value, RealType MinValue, RealType MaxValue )
    {
        return ( Value < MinValue ) ? MinValue : ( ( Value > MaxValue ) ? MaxValue : Value );
    }

    // UE's (V2-V0)x(V1-V0) order: the normal of a triangle wound clockwise when seen from its front.
    template <typename RealType>
    inline TVector<RealType> Normal( const TVector<RealType>& V0, const TVector<RealType>& V1,
                                     const TVector<RealType>& V2 )
    {
        TVector<RealType> edge1( V1 - V0 );
        TVector<RealType> edge2( V2 - V0 );
        TVector<RealType> vCross( edge2.Cross( edge1 ) );
        return Normalized( vCross );
    }

    template <typename RealType>
    inline RealType Area( const TVector<RealType>& V0, const TVector<RealType>& V1, const TVector<RealType>& V2 )
    {
        TVector<RealType> Edge1( V1 - V0 );
        TVector<RealType> Edge2( V2 - V0 );
        TVector<RealType> Cross = Edge2.Cross( Edge1 );
        return (RealType)0.5 * Cross.Length();
    }

    template <typename RealType>
    inline TVector<RealType> NormalArea( const TVector<RealType>& V0, const TVector<RealType>& V1,
                                         const TVector<RealType>& V2, RealType& AreaOut )
    {
        TVector<RealType> edge1( V1 - V0 );
        TVector<RealType> edge2( V2 - V0 );
        TVector<RealType> vCross = edge2.Cross( edge1 );
        AreaOut                  = RealType( 0.5 ) * Normalize( vCross );
        return vCross;
    }

    template <typename RealType>
    inline bool EpsilonEqual( RealType A, RealType B, RealType Epsilon )
    {
        return TMathUtil<RealType>::Abs( A - B ) <= Epsilon;
    }

    template <typename RealType>
    inline bool EpsilonEqual( const TVector2<RealType>& V0, const TVector2<RealType>& V1, RealType Epsilon )
    {
        return EpsilonEqual( V0.X, V1.X, Epsilon ) && EpsilonEqual( V0.Y, V1.Y, Epsilon );
    }

    template <typename RealType>
    inline bool EpsilonEqual( const TVector<RealType>& V0, const TVector<RealType>& V1, RealType Epsilon )
    {
        return EpsilonEqual( V0.X, V1.X, Epsilon ) && EpsilonEqual( V0.Y, V1.Y, Epsilon ) &&
               EpsilonEqual( V0.Z, V1.Z, Epsilon );
    }

    template <typename RealType>
    inline RealType TriSolidAngle( TVector<RealType> A, TVector<RealType> B, TVector<RealType> C,
                                   const TVector<RealType>& P )
    {
        A -= P;
        B -= P;
        C -= P;
        RealType la  = A.Length();
        RealType lb  = B.Length();
        RealType lc  = C.Length();
        RealType top = ( la * lb * lc ) + A.Dot( B ) * lc + B.Dot( C ) * la + C.Dot( A ) * lb;
        RealType bottom =
             A.X * ( B.Y * C.Z - C.Y * B.Z ) - A.Y * ( B.X * C.Z - C.X * B.Z ) + A.Z * ( B.X * C.Y - C.X * B.Y );
        return RealType( -2.0 ) * std::atan2( bottom, top );
    }

    template <typename RealType>
    inline TVector<RealType> TriangleInternalAngles( const TVector<RealType> A, const TVector<RealType> B,
                                                     const TVector<RealType>& C )
    {
        const TVector<RealType> ABhat = Normalized( B - A );
        const TVector<RealType> BChat = Normalized( C - B );
        const TVector<RealType> AChat = Normalized( C - A );
        return TVector<RealType>( AngleR( ABhat, AChat ), AngleR( -ABhat, BChat ), AngleR( AChat, BChat ) );
    }
    // Sign of Bitangent relative to Normal and Tangent (UE follows RenderUtils.h::GetBasisDeterminantSign()).
    template <typename RealType>
    inline RealType BitangentSign( const TVector<RealType>& NormalIn, const TVector<RealType>& TangentIn,
                                   const TVector<RealType>& BitangentIn )
    {
        RealType Cross00     = BitangentIn.Y * NormalIn.Z - BitangentIn.Z * NormalIn.Y;
        RealType Cross10     = BitangentIn.Z * NormalIn.X - BitangentIn.X * NormalIn.Z;
        RealType Cross20     = BitangentIn.X * NormalIn.Y - BitangentIn.Y * NormalIn.X;
        RealType Determinant = TangentIn.X * Cross00 + TangentIn.Y * Cross10 + TangentIn.Z * Cross20;
        return ( Determinant < 0 ) ? (RealType)-1 : (RealType)1;
    }

    // Bitangent from Normal, Tangent and a +1/-1 sign.
    template <typename RealType>
    inline TVector<RealType> Bitangent( const TVector<RealType>& NormalIn, const TVector<RealType>& TangentIn,
                                        RealType BitangentSign )
    {
        return BitangentSign * TVector<RealType>( NormalIn.Y * TangentIn.Z - NormalIn.Z * TangentIn.Y,
                                                  NormalIn.Z * TangentIn.X - NormalIn.X * TangentIn.Z,
                                                  NormalIn.X * TangentIn.Y - NormalIn.Y * TangentIn.X );
    }
} // namespace Desert::Geometry::VectorUtil
