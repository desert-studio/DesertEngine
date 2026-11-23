#pragma once

#include <array>
#include <cstdint>

namespace Desert::VFX
{
    // THE ONE RANDOM FUNCTION OF THE EFFECTS LAYER: a stateless hash of four integers.
    //
    // Port of UE's `Rand4DPCG32` (Engine/Shaders/Private/RandomPCG.ush:80-104, Jarzynski & Olano, JCGT
    // 9(3) 2020: a PCG linear-congruential step followed by a Feistel-style shuffle). Niagara's stateless
    // modules draw every random number through it (NiagaraStatelessCommon.ush:73-91, `RandomUInt4` =
    // `Rand4DPCG32(seed ⊕ callOffset)`), so a value is a FUNCTION of who asks — never of when or of how
    // many asked before. That is the property the effects layer is built on: the same seed, particle and
    // call give the same number on every run, on every platform, in any thread order.
    //
    // The GPU copy is Editor/Resources/Shaders/Common/VFXRandom.glslh; both are the same eleven integer
    // operations on uint32 with wrap-around, which C++ and GLSL define identically.
    [[nodiscard]] constexpr std::array<std::uint32_t, 4> Rand4DPCG32( std::array<std::uint32_t, 4> v ) noexcept
    {
        for ( auto& c : v )
            c = c * 1664525u + 1013904223u;

        v[0] += v[1] * v[3];
        v[1] += v[2] * v[0];
        v[2] += v[0] * v[1];
        v[3] += v[1] * v[2];

        for ( auto& c : v )
            c ^= c >> 16u;

        v[0] += v[1] * v[3];
        v[1] += v[2] * v[0];
        v[2] += v[0] * v[1];
        v[3] += v[1] * v[2];
        return v;
    }

    // The seed one emitter instance draws from (plan §3.3-1: system ⊕ entity ⊕ emitter). Mixed through
    // the hash rather than XOR-ed: an XOR of a UUID and a small index flips a single bit, and two emitters
    // one bit apart would start their sequences one bit apart too. UE offsets the system seed per emitter
    // the same way (NiagaraEmitterInstanceImpl.cpp:247); it takes the seed from a random stream when
    // determinism is off, and here determinism is never off.
    [[nodiscard]] constexpr std::uint32_t MakeEmitterSeed( std::uint32_t systemSeed, std::uint64_t entityUuid,
                                                           std::uint32_t emitterIndex ) noexcept
    {
        return Rand4DPCG32( { systemSeed, static_cast<std::uint32_t>( entityUuid ),
                              static_cast<std::uint32_t>( entityUuid >> 32u ), emitterIndex } )[0];
    }
} // namespace Desert::VFX
