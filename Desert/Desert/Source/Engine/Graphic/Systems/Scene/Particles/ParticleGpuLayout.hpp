#pragma once

#include <cstdint>

namespace Desert::Graphic::System
{
    // THE NUMBERS ParticleRenderer SHARES WITH ITS SHADERS, somewhere a test can reach them.
    //
    // None is a tunable: each is a C++ restatement of something declared in the particle programs
    // (Programs/Particles, Common/Particle*.glslh), and until Г4 the whole of their protection was the
    // words "must match" in a comment. Tests/Engine/ShaderCacheKey now compiles the real shaders and
    // reads the numbers back out of the SPIR-V, so a change to either side alone is a red test rather
    // than a frame that renders wrongly and says nothing.
    //
    // Neither may be written in terms of another: each restates its own shader declaration.

    // The size of one slot of the scene's particle pool, in bytes: Common/ParticleState.glslh's `struct
    // ParticleSlot` (uint Id, uint Alive) in a std430 array - the pool's bookkeeping, none of the particle's data
    // (that is the emitter's attribute columns, the one store the simulation writes and the sprite draw and the
    // sort read). ParticleWorldGpu allocates the pool as capacity * this; a member appended to the GLSL and not
    // here gives the simulation and the compact a different idea of where slot i begins. VFX-SPRITE-DRAW: the
    // 80-byte `struct Particle` (a per-step copy of the drawn attributes) -> the slot, 8.
    constexpr std::uint32_t kParticleSlotStride = 8;

    // The size of an emitter's draw layout (Common/ParticleDrawLayout.glslh `struct ParticleDrawLayout`, the
    // C++ ParticleDrawLayoutGpu): four 16-byte members - range, two rows of attribute columns, emitter position.
    constexpr std::uint32_t kParticleDrawLayoutSize = 64;

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
    // when bound) + vec4 direction (w = 1 when bound) + vec4 colour (rgba) + vec4 scalars (x = lifetime seconds,
    // z = 1 when the colour is bound, w = 1 when the lifetime is bound).
    constexpr std::uint32_t kParticleChannelSpawnStride = 64;

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
