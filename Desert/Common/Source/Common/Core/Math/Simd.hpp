#pragma once

// THE ONE VECTOR LAYER (UE: VectorRegister4Float behind UnrealMathSSE.h / UnrealMathNeon.h / UnrealMathFPU.h).
//
// Four floats in one register and the handful of operations the hand-written hot loops need (frustum culling,
// box transforms, batch box culls). The path is chosen at COMPILE time, never by a runtime dispatch:
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

#include <cmath>
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

    // Lanes 0..3 to p[0..3] (unaligned).
    inline void Store4( float* p, const Float4 v ) noexcept
    {
        _mm_storeu_ps( p, v );
    }

    // Per lane: a < b ? a : b (minps). Operand ORDER is the contract: a NaN in either lane, or a tie between
    // -0 and +0, yields @p b. glm::min( x, y ) is ( y < x ) ? y : x, so glm::min( acc, v ) == Min( v, acc ).
    [[nodiscard]] inline Float4 Min( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_min_ps( a, b );
    }

    // Per lane: a > b ? a : b (maxps); NaN or a signed-zero tie yields @p b. glm::max( x, y ) is
    // ( x < y ) ? y : x, so glm::max( acc, v ) == Max( v, acc ).
    [[nodiscard]] inline Float4 Max( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_max_ps( a, b );
    }

    [[nodiscard]] inline Mask4 CompareGT( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_cmpgt_ps( a, b );
    }

    [[nodiscard]] inline Mask4 MaskOr( const Mask4 a, const Mask4 b ) noexcept
    {
        return _mm_or_ps( a, b );
    }

    [[nodiscard]] inline Mask4 MaskAnd( const Mask4 a, const Mask4 b ) noexcept
    {
        return _mm_and_ps( a, b );
    }
    [[nodiscard]] inline Float4 Sub( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_sub_ps( a, b );
    }

    // Per lane a / b: divps is the correctly rounded IEEE quotient, as the float `/` is.
    [[nodiscard]] inline Float4 Div( const Float4 a, const Float4 b ) noexcept
    {
        return _mm_div_ps( a, b );
    }

    // Per lane sqrt: sqrtps is the correctly rounded IEEE root, as std::sqrt( float ) is (not rsqrtps).
    [[nodiscard]] inline Float4 Sqrt( const Float4 a ) noexcept
    {
        return _mm_sqrt_ps( a );
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

    inline void Store4( float* p, const Float4 v ) noexcept
    {
        for ( int lane = 0; lane < 4; ++lane )
            p[lane] = v.V[lane];
    }

    [[nodiscard]] inline Float4 Min( const Float4 a, const Float4 b ) noexcept
    {
        Float4 out{};
        for ( int lane = 0; lane < 4; ++lane )
        {
            if ( a.V[lane] < b.V[lane] )
                out.V[lane] = a.V[lane];
            else
                out.V[lane] = b.V[lane];
        }
        return out;
    }

    [[nodiscard]] inline Float4 Max( const Float4 a, const Float4 b ) noexcept
    {
        Float4 out{};
        for ( int lane = 0; lane < 4; ++lane )
        {
            if ( a.V[lane] > b.V[lane] )
                out.V[lane] = a.V[lane];
            else
                out.V[lane] = b.V[lane];
        }
        return out;
    }

    [[nodiscard]] inline Mask4 CompareGT( const Float4 a, const Float4 b ) noexcept
    {
        return Mask4{ { a.V[0] > b.V[0], a.V[1] > b.V[1], a.V[2] > b.V[2], a.V[3] > b.V[3] } };
    }

    [[nodiscard]] inline Mask4 MaskOr( const Mask4 a, const Mask4 b ) noexcept
    {
        return Mask4{ { a.V[0] || b.V[0], a.V[1] || b.V[1], a.V[2] || b.V[2], a.V[3] || b.V[3] } };
    }

    [[nodiscard]] inline Mask4 MaskAnd( const Mask4 a, const Mask4 b ) noexcept
    {
        return Mask4{ { a.V[0] && b.V[0], a.V[1] && b.V[1], a.V[2] && b.V[2], a.V[3] && b.V[3] } };
    }

    [[nodiscard]] inline Float4 Sub( const Float4 a, const Float4 b ) noexcept
    {
        return Float4{ { a.V[0] - b.V[0], a.V[1] - b.V[1], a.V[2] - b.V[2], a.V[3] - b.V[3] } };
    }

    [[nodiscard]] inline Float4 Div( const Float4 a, const Float4 b ) noexcept
    {
        return Float4{ { a.V[0] / b.V[0], a.V[1] / b.V[1], a.V[2] / b.V[2], a.V[3] / b.V[3] } };
    }

    [[nodiscard]] inline Float4 Sqrt( const Float4 a ) noexcept
    {
        return Float4{ { std::sqrt( a.V[0] ), std::sqrt( a.V[1] ), std::sqrt( a.V[2] ), std::sqrt( a.V[3] ) } };
    }
#endif

    [[nodiscard]] inline Float4 Zero() noexcept
    {
        return Splat( 0.0f );
    }

    // EIGHT LANES (UE VectorRegister8 / AVX). Under AVX (/arch:AVX2, DESERT_AVX2) one __m256; otherwise two
    // Float4 halves, each through the four-lane operations above - so the SSE2 build and the scalar build get
    // the same eight lanes with the same per-lane IEEE operation, and every path rounds alike (one add, subtract,
    // multiply, divide or root per lane, no FMA, no estimate).
#if DESERT_SIMD_SSE && defined( __AVX__ )
    using Float8 = __m256;

    [[nodiscard]] inline Float8 Load8( const float* p ) noexcept
    {
        return _mm256_loadu_ps( p );
    }

    [[nodiscard]] inline Float8 Splat8( const float v ) noexcept
    {
        return _mm256_set1_ps( v );
    }

    inline void Store8( float* p, const Float8 v ) noexcept
    {
        _mm256_storeu_ps( p, v );
    }

    [[nodiscard]] inline Float8 Add( const Float8 a, const Float8 b ) noexcept
    {
        return _mm256_add_ps( a, b );
    }

    [[nodiscard]] inline Float8 Sub( const Float8 a, const Float8 b ) noexcept
    {
        return _mm256_sub_ps( a, b );
    }

    [[nodiscard]] inline Float8 Mul( const Float8 a, const Float8 b ) noexcept
    {
        return _mm256_mul_ps( a, b );
    }

    [[nodiscard]] inline Float8 Div( const Float8 a, const Float8 b ) noexcept
    {
        return _mm256_div_ps( a, b );
    }

    [[nodiscard]] inline Float8 Sqrt( const Float8 a ) noexcept
    {
        return _mm256_sqrt_ps( a );
    }

    // Per lane a > b ? a : b (vmaxps, Max's operand contract: NaN or a signed-zero tie yields @p b).
    [[nodiscard]] inline Float8 Max( const Float8 a, const Float8 b ) noexcept
    {
        return _mm256_max_ps( a, b );
    }
#else
    struct Float8
    {
        Float4 Lo; // lanes 0..3
        Float4 Hi; // lanes 4..7
    };

    [[nodiscard]] inline Float8 Load8( const float* p ) noexcept
    {
        return Float8{ Load4( p ), Load4( p + 4 ) };
    }

    [[nodiscard]] inline Float8 Splat8( const float v ) noexcept
    {
        return Float8{ Splat( v ), Splat( v ) };
    }

    inline void Store8( float* p, const Float8 v ) noexcept
    {
        Store4( p, v.Lo );
        Store4( p + 4, v.Hi );
    }

    [[nodiscard]] inline Float8 Add( const Float8 a, const Float8 b ) noexcept
    {
        return Float8{ Add( a.Lo, b.Lo ), Add( a.Hi, b.Hi ) };
    }

    [[nodiscard]] inline Float8 Sub( const Float8 a, const Float8 b ) noexcept
    {
        return Float8{ Sub( a.Lo, b.Lo ), Sub( a.Hi, b.Hi ) };
    }

    [[nodiscard]] inline Float8 Mul( const Float8 a, const Float8 b ) noexcept
    {
        return Float8{ Mul( a.Lo, b.Lo ), Mul( a.Hi, b.Hi ) };
    }

    [[nodiscard]] inline Float8 Div( const Float8 a, const Float8 b ) noexcept
    {
        return Float8{ Div( a.Lo, b.Lo ), Div( a.Hi, b.Hi ) };
    }

    [[nodiscard]] inline Float8 Sqrt( const Float8 a ) noexcept
    {
        return Float8{ Sqrt( a.Lo ), Sqrt( a.Hi ) };
    }

    [[nodiscard]] inline Float8 Max( const Float8 a, const Float8 b ) noexcept
    {
        return Float8{ Max( a.Lo, b.Lo ), Max( a.Hi, b.Hi ) };
    }
#endif
} // namespace Common::Math::Simd
