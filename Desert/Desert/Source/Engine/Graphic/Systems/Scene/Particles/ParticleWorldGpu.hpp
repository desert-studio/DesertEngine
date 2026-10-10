#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include "ParticleGpuLayout.hpp"
#include "ParticlePool.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Graphic::System
{
    // Push constant for ParticleSimulate (must match the shader's 128-byte block).
    // ParticleSimulate.shader's PushConstants, field for field.
    struct ParticleSimPush
    {
        glm::vec4  EmitterPos; // xyz world pos (cm), w the fixed step length (seconds)
        glm::uvec4 Counts;     // range count, step index into the step table, range pool base, local-space
        glm::uvec4 Stack;      // x = the world's serial of this tick's step 0, yzw 0
    };

    // ParticleCompact's push constant: x = pool base, y = particle count, z = the slot filled, w = flags
    // (kParticleCompactReset, kParticleCompactFullScan).
    struct ParticleCompactPush
    {
        glm::uvec4 Range;
    };
    // Compact 0 of an emitter that restarts: its range is killed before the scan.
    constexpr uint32_t kParticleCompactReset = 1u;
    // Compact 0: every particle of the range is scanned (the lists are rebuilt from the pool). A later compact
    // scans only the particles the previous step touched - the alive list plus the spawned - and is dispatched
    // indirectly over exactly that many.
    constexpr uint32_t kParticleCompactFullScan = 2u;

    // ParticleDispatchArgs' push constant: x = the step whose Spawn+Update and following compact it sizes.
    struct ParticleArgsPush
    {
        glm::uvec4 Step;
    };

    // One element of ParticleSimulate's step table (`struct VFXStep`).
    struct ParticleStepGpu
    {
        uint32_t IdBase       = 0;
        uint32_t Seed         = 0;
        uint32_t Budget       = 0;
        uint32_t ChannelFirst = 0;
        uint32_t ChannelCount = 0;
    };
    static_assert( sizeof( ParticleStepGpu ) == kParticleStepStride );

    // One Spawn from Channel particle (ParticleSimulate `struct VFXChannelSpawn`), the payload bound to it.
    struct ParticleChannelSpawnGpu
    {
        glm::vec4 Position  = glm::vec4( 0.0f ); // xyz world cm, w = 1 when the module binds a Position field
        glm::vec4 Direction = glm::vec4( 0.0f ); // xyz start velocity direction, w = 1 when bound
        glm::vec4 Color     = glm::vec4( 1.0f ); // linear rgba -> the particle's Tint when Scalars.z = 1
        glm::vec4 Scalars   = glm::vec4( 0.0f ); // x = lifetime s, z = 1 colour bound, w = 1 lifetime bound
    };
    static_assert( sizeof( ParticleChannelSpawnGpu ) == kParticleChannelSpawnStride );

    // One slot of an emitter's Counters (Common/ParticlePool.glslh ParticleDrawSlot); the first four members are
    // the VkDrawIndirectCommand the billboard draw reads.
    struct ParticleDrawSlotGpu
    {
        uint32_t VertexCount   = 0;
        uint32_t InstanceCount = 1;
        uint32_t FirstVertex   = 0;
        uint32_t FirstInstance = 0;
        uint32_t FreeCount     = 0;
        uint32_t Touched       = 0;
        uint32_t Pad[2]        = {};
    };
    static_assert( sizeof( ParticleDrawSlotGpu ) == kParticleDrawSlotStride );

    // One emitter's GPU state in the scene's pool, cached across ticks by entity id. Its particles are Range of
    // the pool; its alive lists are the two halves [2 Base + h Count, 2 Base + (h + 1) Count) of the alive list.
    struct ParticleEmitterGpu
    {
        std::shared_ptr<ShaderResources::StorageBuffer> Steps;        // this tick's step table
        std::shared_ptr<ShaderResources::StorageBuffer> Counters;     // two draw slots, uploaded per tick
        std::shared_ptr<ShaderResources::StorageBuffer> DispatchArgs; // Spawn+Update's and the next compact's
        std::shared_ptr<ShaderResources::StorageBuffer> ChannelSpawns; // this tick's Spawn from Channel particles

        // VFX-HOST: the emitter's compiled stack - its host program, its attribute columns (SoA, Count rows per
        // component), its parameter rows and its system's curve atlas (re-uploaded every tick: the copies are
        // per frame in flight).
        std::shared_ptr<ComputePipeline>                Pipeline; // null = the stack was refused, it sits out
        std::shared_ptr<ShaderResources::StorageBuffer> AttributeFloats;
        std::shared_ptr<ShaderResources::StorageBuffer> AttributeInts;
        std::shared_ptr<ShaderResources::StorageBuffer> Params;
        std::shared_ptr<ShaderResources::StorageBuffer> Curves;
        std::vector<glm::vec4>                          ParamRows;
        std::vector<float>                              CurveFloats;
        uint32_t                                        Capacity    = 0;
        uint32_t                                        FloatColumns = 0;
        uint32_t                                        IntColumns   = 0;
        bool                                            Local       = false;

        ParticlePoolRange Range;
        uint32_t          StepCapacity    = 0;
        uint32_t          ChannelCapacity = 0;    // particles ChannelSpawns holds
        uint64_t          Generation   = 0;    // the VFXWorld instance generation this state belongs to
        bool              NeedsReset   = true; // compact 0 kills the range (fresh, moved, restarted, pool grew)
    };

    // One emitter active in this scene update.
    struct ParticleFrameEmitter
    {
        uint32_t            EntityId = 0;
        ParticleEmitterGpu* Gpu      = nullptr;
        ParticleSimPush     Push;
        ComputePipeline*    Pipeline = nullptr; // the emitter's stack host program (Gpu->Pipeline)
        // The sprite renderer's material (null = the default sprite template; a `.dfx` sprite row names none yet,
        // VFXS 1); the drawing view resolves its ParticleSprite.Forward cell and reads the blend mode off it.
        Common::AssetHandle Material;
        uint32_t            StepCount = 0; // fixed steps this tick; compacts 0..StepCount
    };

    // The pool's buffers: particles, the free list (one entry per particle) and the alive list (two per particle).
    struct ParticlePoolBuffers
    {
        std::shared_ptr<ShaderResources::StorageBuffer> Particles;
        std::shared_ptr<ShaderResources::StorageBuffer> FreeList;
        std::shared_ptr<ShaderResources::StorageBuffer> AliveList;
        uint32_t                                        Capacity = 0;
    };

    // THE SCENE'S GPU PARTICLE STATE (UE: FScene::FXSystem, the world's GPU particle system; VFX-07b). Owned by
    // the scene's VFXWorld (VFX::WorldGpuState), so every view of one scene sees ONE pool: the first view to
    // prepare a VFXWorld tick claims it, uploads the tick's step tables and counters and adds the simulation
    // nodes; every other view of that tick only draws the same pool (ParticleRenderer::DrawPass). Dropped with the
    // world (VFXWorld::Clear, the scene's destruction); the buffers go through the allocator's deletion ring.
    // One placed system's GPU state (per entity): an emitter state per emitter of the system it was built from.
    struct ParticleSystemGpu
    {
        std::shared_ptr<const Assets::Serialization::VFXSystemData> System;
        std::vector<ParticleEmitterGpu>                             Emitters;
    };

    class ParticleWorldGpu final : public VFX::WorldGpuState
    {
    public:
        // The state of @p world, made on first use.
        [[nodiscard]] static ParticleWorldGpu& Of( const VFX::VFXWorld& world );

        // Called by every view of @p scene once per frame. The first call of a VFXWorld tick snapshots the
        // emitters (params, world position) and the tick's steps, uploads the step tables and counters, and
        // returns true: that view simulates. Every later call of the same tick changes nothing and returns false.
        [[nodiscard]] bool PrepareTick( const Core::Scene& scene );

        [[nodiscard]] const std::vector<ParticleFrameEmitter>& FrameEmitters() const
        {
            return m_FrameEmitters;
        }
        [[nodiscard]] const ParticlePoolBuffers& Pool() const
        {
            return m_Pool;
        }

    private:
        // Grows the pool to hold @p particles (recreating it: every emitter restarts); false when it failed.
        bool                EnsurePoolCapacity( uint32_t particles );
        // Uploads emitter @p emitter's tick into @p gpu and adds its frame emitter; false = it sits out this tick.
        bool PrepareEmitter( ParticleEmitterGpu& gpu, const VFX::EmitterInstance& emitter,
                             const ParticlePoolRange& range, uint32_t entityId, const glm::vec3& worldPos,
                             float stepSeconds, uint32_t stepSerial );
        // Makes @p gpu's per-tick buffers (step table, counters, dispatch args, channel spawns); false = it sits out.
        static bool EnsureTickBuffers( ParticleEmitterGpu& gpu, uint32_t stepCapacity );
        // Compiles emitter @p index of @p system into @p gpu (program, columns, parameters, curves).
        void BuildEmitter( ParticleEmitterGpu& gpu, const Assets::Serialization::VFXSystemData& system,
                           std::size_t index, uint32_t entityId );
        // The host program for one compiled stack, by its key (null = refused, said once).
        std::shared_ptr<ComputePipeline> ProgramFor( uint64_t key, const std::string& name,
                                                     const std::string& hostSource );

        std::unordered_map<uint32_t, ParticleSystemGpu>                m_Systems;
        std::unordered_map<uint64_t, std::shared_ptr<ComputePipeline>> m_Programs;
        std::vector<ParticleFrameEmitter>                m_FrameEmitters;
        ParticlePoolRanges                               m_Ranges;
        ParticlePoolBuffers                              m_Pool;
        ParticleTickClaim                                m_Claim;
    };
} // namespace Desert::Graphic::System
