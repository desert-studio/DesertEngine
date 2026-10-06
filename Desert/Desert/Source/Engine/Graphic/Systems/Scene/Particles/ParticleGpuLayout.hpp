#pragma once

#include <cstdint>

namespace Desert::Graphic::System
{
    // THE TWO NUMBERS ParticleRenderer SHARES WITH ITS SHADERS, somewhere a test can reach them.
    //
    // Neither is a tunable and neither is a length: each is a C++ restatement of something declared in
    // Programs/Particles/ParticleSimulate.shader, and until Г4 the whole of their protection was the
    // words "must match" in a comment. Tests/Engine/ShaderCacheKey now compiles the real shaders and
    // reads both numbers back out of the SPIR-V, so a change to either side alone is a red test rather
    // than a frame that renders wrongly and says nothing.
    //
    // They are equal by coincidence and NOT by derivation — one is a byte count and the other a thread
    // count. Neither may be written in terms of the other.

    // The size of one element of the particle state storage, in bytes: Common/ParticleState.glslh's
    // `struct Particle`, which is four vec4s in a std430 array. ParticleRenderer allocates the storage
    // as MaxParticles * this and zeroes it with a buffer of the same size, so a member appended to the
    // GLSL and not to this number gives the simulation and the draw a different idea of where particle
    // i begins — visible only as an emitter that has come apart.
    constexpr std::uint32_t kParticleStride = 64;

    // The x of ParticleSimulate's LocalSize, which is the number the dispatch divides MaxParticles by
    // to get its group count. Too large here and the tail of every emitter is never simulated (those
    // particles stay at their zeroed state — dead, invisible, never respawned); too small and the
    // dispatch runs threads past the end, which the shader's own bound check discards.
    constexpr std::uint32_t kParticleLocalSize = 64;

    // The size of one element of ParticleSimulate's step table (binding 1, `struct VFXStep`: four
    // uints). ParticleRenderer uploads the frame's steps as an array of this stride and the shader
    // indexes it by the step number in the push constant.
    constexpr std::uint32_t kParticleStepStride = 16;
} // namespace Desert::Graphic::System
