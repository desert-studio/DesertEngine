#pragma once

#include <cstdint>

namespace Common::Math
{
    /**
     * @brief PCG32 (O'Neill, pcg-random.org, XSH-RR): the one random stream placement draws from — the foliage
     * brush (FO-2).
     *
     * std::uniform_real_distribution is implementation-defined, so libc++ and MSVC would scatter the same seed
     * differently; this stream is integer-only and defined bit for bit, so a seed is the same numbers on every
     * compiler. (What is then COMPUTED from them in float may still differ by FMA contraction between toolsets;
     * compare the integers across platforms, never a hash of float bytes.)
     */
    class Pcg32
    {
    public:
        explicit Pcg32( uint64_t seed )
        {
            // pcg32_srandom with the default stream: state 0, step, add the seed, step.
            NextU32();
            m_State += seed;
            NextU32();
        }

        uint32_t NextU32()
        {
            const uint64_t old    = m_State;
            m_State               = old * 6364136223846793005ull + 1442695040888963407ull;
            const auto xorshifted = static_cast<uint32_t>( ( ( old >> 18u ) ^ old ) >> 27u );
            const auto rot        = static_cast<uint32_t>( old >> 59u );
            return ( xorshifted >> rot ) | ( xorshifted << ( ( 32u - rot ) & 31u ) );
        }

        /// Uniform in [0, 1), 24 bits: exact in float, so the value itself is the same everywhere.
        float Next01()
        {
            return static_cast<float>( NextU32() >> 8u ) * ( 1.0f / 16777216.0f );
        }

    private:
        uint64_t m_State = 0u;
    };
} // namespace Common::Math
