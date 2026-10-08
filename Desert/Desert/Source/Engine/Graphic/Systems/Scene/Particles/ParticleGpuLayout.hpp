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

    // The size of one element of ParticleSimulate's step table (binding 1, `struct VFXStep`: three
    // uints - id base, seed, budget). ParticleRenderer uploads the frame's steps as an array of this stride
    // and the shader indexes it by the step number in the push constant.
    // VFX-10: + ChannelFirst, ChannelCount (the step's Spawn from Channel particles, VFXWorld EmitterStep).
    constexpr std::uint32_t kParticleStepStride = 20;

    // One Spawn from Channel particle (ParticleSimulate binding 5, `struct VFXChannelSpawn`): vec4 position (w = 1
    // when bound) + vec4 direction (w = 1 when bound).
    constexpr std::uint32_t kParticleChannelSpawnStride = 32;

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
