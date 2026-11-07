#pragma once

// THE ONE VECTOR LAYER (UE: VectorRegister4Float behind UnrealMathSSE.h / UnrealMathNeon.h / UnrealMathFPU.h).
//
// Four floats in one register and the handful of operations the hand-written hot loops need (frustum culling
// today). The path is chosen at COMPILE time, never by a runtime dispatch:
//   DESERT_SIMD_SSE   x86-64: SSE2 is the x64 baseline, so this path exists in every x64 build; under
//                     /arch:AVX2 (DESERT_AVX2, cmake/DesertWorkspace.cmake) the same intrinsics are emitted with
//                     VEX encodings and Select uses blendv.
//   otherwise         the scalar path (macOS ARM64 and any other target): the same four lanes in a plain
//                     struct, each operation lane by lane. NEON is not written: the ARM build has no hot loop
//                     that measured it yet.
//
// EVERY PATH ROUNDS ALIKE. Each operation is one IEEE single-precision add, multiply or compare per lane, in the
// order the caller writes it: no fused multiply-add (VS2022's /fp:precise does not contract, and no FMA
// intrinsic is used), no reciprocal estimates. A loop written once over these operations therefore accepts and
// rejects exactly what the same arithmetic written with plain floats does, NaN included: the compares are
// ORDERED (a NaN lane compares false), as C++'s >= and < are.

#if defined( _M_X64 ) || defined( _M_AMD64 ) || defined( __x86_64__ )
#define DESERT_SIMD_SSE 1
#include <immintrin.h>
#else
#define DESERT_SIMD_SSE 0
#endif

#include <cstdint>

namespace Common::Math::Simd
{
#if DESERT_SIMD_SSE
    using Float4 = __m128;
    // A lane mask: every bit of a lane set (true) or clear (false), as the SSE compares write it.
    using Mask4 = __m128;

    [[nodiscard]] inline Float4 Load4( const float* p ) noexcept
    {
        return _mm_loadu_ps( p );
    }

    [[nodiscard]] inline Float4 Splat( const float v ) noexcept
    {
        return _mm_set1_ps( v );
    }

    [[nodiscard]] inline Float4 Add( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_add_ps( a, b );
    }

    [[nodiscard]] inline Float4 Mul( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_mul_ps( a, b );
    }

    [[nodiscard]] inline Mask4 CompareGE( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_cmpge_ps( a, b );
    }

    [[nodiscard]] inline Mask4 CompareLT( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_cmplt_ps( a, b );
    }

    // Per lane: mask ? whenTrue : whenFalse.
    [[nodiscard]] inline Float4 Select( const Mask4 mask, const Float4 whenTrue, const Float4 whenFalse ) noexcept
    {
#if defined( __AVX__ )
        return _mm_blendv_ps( whenFalse, whenTrue, mask );
#else
        return _mm_or_ps( _mm_and_ps( mask, whenTrue ), _mm_andnot_ps( mask, whenFalse ) );
#endif
    }

    // Bit i set when lane i of @p mask is true.
    [[nodiscard]] inline uint32_t MaskBits( const Mask4 mask ) noexcept
    {
        return static_cast<uint32_t>( _mm_movemask_ps( mask ) );
    }
#else
    struct Float4
    {
        float V[4];
    };

    struct Mask4
    {
        bool V[4];
    };

    [[nodiscard]] inline Float4 Load4( const float* p ) noexcept
    {
        return Float4{ { p[0], p[1], p[2], p[3] } };
    }

    [[nodiscard]] inline Float4 Splat( const float v ) noexcept
    {
        return Float4{ { v, v, v, v } };
    }

    [[nodiscard]] inline Float4 Add( const Float4 a, const Float4 b ) noexcept
    {
        return Float4{ { a.V[0] + b.V[0], a.V[1] + b.V[1], a.V[2] + b.V[2], a.V[3] + b.V[3] } };
    }

    [[nodiscard]] inline Float4 Mul( const Float4 a, const Float4 b ) noexcept
    {
        return Float4{ { a.V[0] * b.V[0], a.V[1] * b.V[1], a.V[2] * b.V[2], a.V[3] * b.V[3] } };
    }

    [[nodiscard]] inline Mask4 CompareGE( const Float4 a, const Float4 b ) noexcept
    {
        return Mask4{ { a.V[0] >= b.V[0], a.V[1] >= b.V[1], a.V[2] >= b.V[2], a.V[3] >= b.V[3] } };
    }

    [[nodiscard]] inline Mask4 CompareLT( const Float4 a, const Float4 b ) noexcept
    {
        return Mask4{ { a.V[0] < b.V[0], a.V[1] < b.V[1], a.V[2] < b.V[2], a.V[3] < b.V[3] } };
    }

    [[nodiscard]] inline Float4 Select( const Mask4 mask, const Float4 whenTrue, const Float4 whenFalse ) noexcept
    {
        Float4 out{};
        for ( int lane = 0; lane < 4; ++lane )
        {
            if ( mask.V[lane] )
                out.V[lane] = whenTrue.V[lane];
            else
                out.V[lane] = whenFalse.V[lane];
        }
        return out;
    }

    [[nodiscard]] inline uint32_t MaskBits( const Mask4 mask ) noexcept
    {
        uint32_t bits = 0u;
        for ( uint32_t lane = 0u; lane < 4u; ++lane )
        {
            if ( mask.V[lane] )
                bits |= 1u << lane;
        }
        return bits;
    }
#endif

    [[nodiscard]] inline Float4 Zero() noexcept
    {
        return Splat( 0.0f );
    }
} // namespace Common::Math::Simd
