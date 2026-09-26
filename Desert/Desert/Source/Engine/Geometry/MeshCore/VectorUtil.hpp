// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/VectorUtil.h:42-56,70-109,148-178,476-489,540-552,
// 612-636, adapted: only the functions the DynamicMesh3 port and MeshTangents call; namespace
// Desert::Geometry::VectorUtil.
#pragma once

#include "Engine/Geometry/MeshCore/VectorTypes.hpp"

#include <cmath>

namespace Desert::Geometry::VectorUtil
{
    template <typename RealType>
    inline bool IsFinite( const glm::vec<2, RealType>& V )
    {
        return std::isfinite( V.x ) && std::isfinite( V.y );
    }

    template <typename RealType>
    inline bool IsFinite( const glm::vec<3, RealType>& V )
    {
        return std::isfinite( V.x ) && std::isfinite( V.y ) && std::isfinite( V.z );
    }

    template <typename RealType>
    inline RealType Clamp( RealType Value, RealType MinValue, RealType MaxValue )
    {
        return ( Value < MinValue ) ? MinValue : ( ( Value > MaxValue ) ? MaxValue : Value );
    }

    // UE's (V2-V0)x(V1-V0) order: the normal of a triangle wound clockwise when seen from its front.
    template <typename RealType>
    inline glm::vec<3, RealType> Normal( const glm::vec<3, RealType>& V0, const glm::vec<3, RealType>& V1,
                                         const glm::vec<3, RealType>& V2 )
    {
        const glm::vec<3, RealType> edge1( V1 - V0 );
        const glm::vec<3, RealType> edge2( V2 - V0 );
        const glm::vec<3, RealType> vCross( glm::cross( edge2, edge1 ) );
        return Normalized( vCross );
    }

    template <typename RealType>
    inline RealType Area( const glm::vec<3, RealType>& V0, const glm::vec<3, RealType>& V1,
                          const glm::vec<3, RealType>& V2 )
    {
        const glm::vec<3, RealType> Edge1( V1 - V0 );
        const glm::vec<3, RealType> Edge2( V2 - V0 );
        const glm::vec<3, RealType> Cross = glm::cross( Edge2, Edge1 );
        return static_cast<RealType>( 0.5 ) * glm::length( Cross );
    }

    template <typename RealType>
    inline glm::vec<3, RealType> NormalArea( const glm::vec<3, RealType>& V0, const glm::vec<3, RealType>& V1,
                                             const glm::vec<3, RealType>& V2, RealType& AreaOut )
    {
        const glm::vec<3, RealType> edge1( V1 - V0 );
        const glm::vec<3, RealType> edge2( V2 - V0 );
        glm::vec<3, RealType>       vCross = glm::cross( edge2, edge1 );
        AreaOut                            = RealType( 0.5 ) * Normalize( vCross );
        return vCross;
    }

    template <typename RealType>
    inline bool EpsilonEqual( RealType A, RealType B, RealType Epsilon )
    {
        return std::abs( A - B ) <= Epsilon;
    }

    template <typename RealType>
    inline bool EpsilonEqual( const glm::vec<2, RealType>& V0, const glm::vec<2, RealType>& V1, RealType Epsilon )
    {
        return EpsilonEqual( V0.x, V1.x, Epsilon ) && EpsilonEqual( V0.y, V1.y, Epsilon );
    }

    template <typename RealType>
    inline bool EpsilonEqual( const glm::vec<3, RealType>& V0, const glm::vec<3, RealType>& V1, RealType Epsilon )
    {
        return EpsilonEqual( V0.x, V1.x, Epsilon ) && EpsilonEqual( V0.y, V1.y, Epsilon ) &&
               EpsilonEqual( V0.z, V1.z, Epsilon );
    }

    template <typename RealType>
    inline RealType TriSolidAngle( glm::vec<3, RealType> A, glm::vec<3, RealType> B, glm::vec<3, RealType> C,
                                   const glm::vec<3, RealType>& P )
    {
        A -= P;
        B -= P;
        C -= P;
        RealType la  = glm::length( A );
        RealType lb  = glm::length( B );
        RealType lc  = glm::length( C );
        RealType top = ( la * lb * lc ) + glm::dot( A, B ) * lc + glm::dot( B, C ) * la + glm::dot( C, A ) * lb;
        RealType bottom =
             A.x * ( B.y * C.z - C.y * B.z ) - A.y * ( B.x * C.z - C.x * B.z ) + A.z * ( B.x * C.y - C.x * B.y );
        return RealType( -2.0 ) * std::atan2( bottom, top );
    }

    template <typename RealType>
    inline glm::vec<3, RealType> TriangleInternalAngles( const glm::vec<3, RealType>  A,
                                                         const glm::vec<3, RealType>  B,
                                                         const glm::vec<3, RealType>& C )
    {
        const glm::vec<3, RealType> ABhat = Normalized( B - A );
        const glm::vec<3, RealType> BChat = Normalized( C - B );
        const glm::vec<3, RealType> AChat = Normalized( C - A );
        return glm::vec<3, RealType>( AngleR( ABhat, AChat ), AngleR( -ABhat, BChat ), AngleR( AChat, BChat ) );
    }
    // Sign of Bitangent relative to Normal and Tangent (UE follows RenderUtils.h::GetBasisDeterminantSign()).
    template <typename RealType>
    inline RealType BitangentSign( const glm::vec<3, RealType>& NormalIn, const glm::vec<3, RealType>& TangentIn,
                                   const glm::vec<3, RealType>& BitangentIn )
    {
        RealType Cross00     = BitangentIn.y * NormalIn.z - BitangentIn.z * NormalIn.y;
        RealType Cross10     = BitangentIn.z * NormalIn.x - BitangentIn.x * NormalIn.z;
        RealType Cross20     = BitangentIn.x * NormalIn.y - BitangentIn.y * NormalIn.x;
        RealType Determinant = TangentIn.x * Cross00 + TangentIn.y * Cross10 + TangentIn.z * Cross20;
        return ( Determinant < 0 ) ? (RealType)-1 : (RealType)1;
    }

    // Bitangent from Normal, Tangent and a +1/-1 sign.
    template <typename RealType>
    inline glm::vec<3, RealType> Bitangent( const glm::vec<3, RealType>& NormalIn,
                                            const glm::vec<3, RealType>& TangentIn, RealType BitangentSign )
    {
        return BitangentSign * glm::vec<3, RealType>( NormalIn.y * TangentIn.z - NormalIn.z * TangentIn.y,
                                                      NormalIn.z * TangentIn.x - NormalIn.x * TangentIn.z,
                                                      NormalIn.x * TangentIn.y - NormalIn.y * TangentIn.x );
    }
} // namespace Desert::Geometry::VectorUtil
