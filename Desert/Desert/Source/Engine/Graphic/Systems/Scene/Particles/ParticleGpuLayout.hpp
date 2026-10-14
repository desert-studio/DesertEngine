#pragma once

#include <cstdint>

namespace Desert::Graphic::System
{
    // THE NUMBERS ParticleRenderer SHARES WITH ITS SHADERS, somewhere a test can reach them.
    //
    // None is a tunable: each is a C++ restatement of something a compiled stack's simulation program
    // (VFX/Simulate/<key>, VFX/VFXSimulationProgram.cpp) or Common/ParticlePool.glslh declares.
    // Tests/Engine/ShaderCacheKey compiles the real shaders and reads the numbers back out of the SPIR-V, so a
    // change to either side alone is a red test rather than a frame that renders wrongly and says nothing.

    // The x of the simulation program's LocalSize, which is the number the dispatch divides MaxParticles by
    // to get its group count. Too large here and the tail of every emitter is never simulated; too small and the
    // dispatch runs threads past the end, which the shader's own bound check discards.
    constexpr std::uint32_t kParticleLocalSize = 64;

    // The size of one element of the simulation program's step table (`struct VFXStep`: id base, seed, budget,
    // ChannelFirst, ChannelCount - the step's Spawn from Channel particles, VFXWorld EmitterStep).
    // ParticleRenderer uploads the frame's steps as an array of this stride and the shader indexes it by the step
    // number in the push constant.
    constexpr std::uint32_t kParticleStepStride = 20;

    // The size of one element of an emitter's Counters buffer (Common/ParticlePool.glslh `struct
    // ParticleDrawSlot`: a VkDrawIndirectCommand - vertex count = 6 x alive, instance count 1, first vertex =
    // 6 x the start of the slot's alive half (2 x pool base + slot x count), first instance 0 - then the free
    // count, the touched count the next compact scans, and two pad uints). ParticleCompact
    // writes slot (compact index & 1); the billboard draw reads the last compact's slot as its indirect
    // arguments at slot x this offset. Two slots per emitter.
    constexpr std::uint32_t kParticleDrawSlotStride = 32;
    constexpr std::uint32_t kParticleDrawSlots      = 2;

    // An emitter's DispatchArgs buffer (ParticleDispatchArgs.shader `uvec4 u_Args[]`): two
    // VkDispatchIndirectCommand padded to 16 bytes - Spawn+Update's at kParticleSimulateArgsOffset, the next
    // compact's at kParticleCompactArgsOffset - written per step from the GPU counts.
    constexpr std::uint32_t kParticleDispatchArgsStride = 16;
    constexpr std::uint32_t kParticleDispatchArgsCount  = 2;
    constexpr std::uint64_t kParticleSimulateArgsOffset = 0;
    constexpr std::uint64_t kParticleCompactArgsOffset  = kParticleDispatchArgsStride;
} // namespace Desert::Graphic::System
