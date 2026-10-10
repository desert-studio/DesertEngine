#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Core/Public/Math/RandomStream.h (FRandomStream). Adapted: the seed is
// held unsigned, so the shift that builds a fraction is logical on every compiler.

#include <algorithm>
#include <bit>
#include <cstdint>

namespace Common::Math
{
    /**
     * @brief UE's FRandomStream: the 32-bit linear congruential stream UE's seeded generators draw from (the
     * procedural foliage simulation, the Gerstner wave generator).
     *
     * Integer arithmetic only (wrapping unsigned), so a seed yields the same sequence on every platform.
     */
    class RandomStream
    {
    public:
        RandomStream() = default;
        explicit RandomStream( int32_t seed )
        {
            Initialize( seed );
        }

        void Initialize( int32_t seed )
        {
            m_Seed = static_cast<uint32_t>( seed );
        }

        /// A float in [0, 1): the 23 high bits of the next seed as the mantissa of a float in [1, 2), minus 1.
        float GetFraction()
        {
            MutateSeed();
            const uint32_t bits = 0x3F800000u | ( m_Seed >> 9 );
            return std::bit_cast<float>( bits ) - 1.0f;
        }
        float FRand()
        {
            return GetFraction();
        }
        uint32_t GetUnsignedInt()
        {
            MutateSeed();
            return m_Seed;
        }
        /// A float in [min, max).
        float FRandRange( float min, float max )
        {
            return min + ( max - min ) * FRand();
        }
        /// An integer in [min, max].
        int32_t RandRange( int32_t min, int32_t max )
        {
            const int32_t range = ( max - min ) + 1;
            if ( range <= 0 )
                return min;
            return min + std::min( static_cast<int32_t>( FRand() * static_cast<float>( range ) ), range - 1 );
        }

    private:
        void MutateSeed()
        {
            m_Seed = m_Seed * 196314165u + 907633515u;
        }

        uint32_t m_Seed = 0;
    };
} // namespace Common::Math
