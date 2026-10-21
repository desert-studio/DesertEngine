#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/VFX/VFXWorld.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include "ParticleGpuLayout.hpp"
#include "ParticlePool.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
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

    // VFX-16 (UE Niagara Debugger, dev only): ParticleDebugReadback's push constant and its readback's shape. The
    // readback is 4 header uints (alive, free, debug serial, rows written) then per row the pool index and the
    // emitter's float attribute components; one page is kParticleDebugMaxRows alive particles.
    struct ParticleDebugPush
    {
        glm::uvec4 Page;  // x = last compact's slot, y = first alive row, z = rows (0 = counts only), w = float columns
        glm::uvec4 Range; // x = drawn alive half's offset, y = range count, z = pool base, w = debug serial (never 0)
    };
    constexpr uint32_t kParticleDebugHeaderUints = 4u;
    constexpr uint32_t kParticleDebugMaxRows     = 64u;

    // What an editor debugger asks of the scene's particles: nothing runs while Enabled is false. The attribute
    // table is the page [FirstRow, FirstRow + kParticleDebugMaxRows) of the alive list of emitter Emitter of
    // entity EntityId.
    struct ParticleDebugRequest
    {
        bool     Enabled  = false;
        uint32_t EntityId = 0;
        uint32_t Emitter  = 0;
        uint32_t FirstRow = 0;
    };

    // One emitter as the GPU last reported it (read back FramesOld simulated frames after the GPU wrote it; Read =
    // false until the first copy came back). Columns / RowIndex / Floats only for the requested emitter.
    struct ParticleEmitterDebug
    {
        uint32_t                             EntityId     = 0;
        uint32_t                             Emitter      = 0;
        std::string                          Name;
        uint32_t                             Capacity     = 0;
        uint32_t                             Alive        = 0;
        uint32_t                             Free         = 0;
        uint32_t                             FramesOld    = 0;
        bool                                 Read         = false;
        uint32_t                             FloatColumns = 0;
        std::vector<VFX::VFXAttributeLayout> Columns;  // the compiled layout's attributes (FloatStart / FloatCount)
        std::vector<uint32_t>                RowIndex; // pool index per row
        std::vector<float>                   Floats;   // RowIndex.size() x FloatColumns, row-major
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
        glm::vec4 Color     = glm::vec4( 1.0f ); // linear rgba -> InitializeColor when Scalars.z = 1
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

    // Where a renderer finds an emitter's particle (Common/ParticleDrawLayout.glslh `struct ParticleDrawLayout`;
    // UE FNiagaraSpriteUniformParameters' data offsets): the first float column of each drawn attribute in the
    // emitter's SoA columns (kParticleAttrNone = the stack does not declare it, the reader's default), the range
    // the columns are strided by, and the emitter's world position a Local emitter's particles are drawn around.
    constexpr std::uint32_t kParticleAttrNone = 0xFFFFFFFFu;
    struct ParticleDrawLayoutGpu
    {
        glm::uvec4 Range      = glm::uvec4( 0u ); // x = range count (column stride), y = pool base, z = 1 local
        glm::uvec4 Columns0   = glm::uvec4( kParticleAttrNone ); // Position, SpriteSize, Color, SpriteRotation
        glm::uvec4 Columns1   = glm::uvec4( kParticleAttrNone ); // Age, Lifetime, -, -
        glm::vec4  EmitterPos = glm::vec4( 0.0f );               // xyz = emitter world position (cm)
    };
    static_assert( sizeof( ParticleDrawLayoutGpu ) == kParticleDrawLayoutSize );

    // One emitter's GPU state in the scene's pool, cached across ticks by entity id. Its particles are Range of
    // the pool; its alive lists are the two halves [2 Base + h Count, 2 Base + (h + 1) Count) of the alive list.
    struct ParticleEmitterGpu
    {
        std::shared_ptr<ShaderResources::StorageBuffer> Steps;         // this tick's step table
        std::shared_ptr<ShaderResources::StorageBuffer> Counters;      // two draw slots, uploaded per tick
        std::shared_ptr<ShaderResources::StorageBuffer> DispatchArgs;  // Spawn+Update's and the next compact's
        std::shared_ptr<ShaderResources::StorageBuffer> ChannelSpawns; // this tick's Spawn from Channel particles

        // VFX-HOST: the emitter's compiled stack - its host program, its attribute columns (SoA, Count rows per
        // component), its parameter rows and its system's curve atlas (re-uploaded every tick: the copies are
        // per frame in flight).
        std::shared_ptr<ComputePipeline>                Pipeline; // null = the stack was refused, it sits out
        std::shared_ptr<ShaderResources::StorageBuffer> AttributeFloats;
        std::shared_ptr<ShaderResources::StorageBuffer> AttributeInts;
        std::shared_ptr<ShaderResources::StorageBuffer> Params;
        std::shared_ptr<ShaderResources::StorageBuffer> Curves;
        // Where the sprite draw and the sort read the drawn attributes in AttributeFloats (VFX-SPRITE-DRAW): its
        // columns are the compiled layout's, its range and emitter position are re-uploaded every tick.
        std::shared_ptr<ShaderResources::StorageBuffer> DrawLayout;
        ParticleDrawLayoutGpu                           DrawColumns;
        std::vector<glm::vec4>                          ParamRows;
        std::vector<float>                              CurveFloats;
        uint32_t                                        Capacity     = 0;
        uint32_t                                        FloatColumns = 0;
        uint32_t                                        IntColumns   = 0;
        bool                                            Local        = false;

        ParticlePoolRange Range;
        uint32_t          StepCapacity    = 0;
        uint32_t          ChannelCapacity = 0;    // particles ChannelSpawns holds
        uint64_t          Generation      = 0;    // the VFXWorld instance generation this state belongs to
        bool              NeedsReset      = true; // compact 0 kills the range (fresh, moved, restarted, pool grew)

        // VFX-16: the debugger's per-frame-in-flight readback (made on the first debugged tick, dropped when the
        // emitter is rebuilt) and what its rows mean.
        std::shared_ptr<ShaderResources::StorageBuffer> DebugReadback;
        std::vector<VFX::VFXAttributeLayout>            DebugColumns;
        std::string                                     DebugName;
        uint32_t                                        DebugEntity  = 0;
        uint32_t                                        DebugEmitter = 0;
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

    // The pool's buffers: the slots (id + alive flag per particle, Common/ParticleState.glslh), the free list (one
    // entry per particle) and the alive list (two per particle). The particles' data is the emitters' columns.
    struct ParticlePoolBuffers
    {
        std::shared_ptr<ShaderResources::StorageBuffer> Slots;
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

        // VFX-16 (UE Niagara Debugger, dev only). The editor's request; while it is enabled the simulating view
        // adds "Particles: Debug Readback" and ReadDebugReadbacks (called by that view right after PrepareTick,
        // before the node writes this frame's copy) reads what the GPU wrote FramesInFlight frames ago - no stall,
        // no CPU-side count.
        void SetDebugRequest( const ParticleDebugRequest& request )
        {
            m_DebugRequest = request;
        }
        [[nodiscard]] const ParticleDebugRequest& DebugRequest() const
        {
            return m_DebugRequest;
        }
        [[nodiscard]] const std::vector<ParticleEmitterDebug>& DebugEmitters() const
        {
            return m_Debug;
        }
        [[nodiscard]] uint32_t DebugSerial() const
        {
            return m_DebugSerial;
        }
        // Whether @p gpu is the emitter whose attribute page is asked for.
        [[nodiscard]] bool IsDebugTable( const ParticleEmitterGpu& gpu ) const
        {
            return m_DebugRequest.Enabled && gpu.DebugEntity == m_DebugRequest.EntityId &&
                   gpu.DebugEmitter == m_DebugRequest.Emitter;
        }
        void ReadDebugReadbacks();

    private:
        // Grows the pool to hold @p particles (recreating it: every emitter restarts); false when it failed.
        bool EnsurePoolCapacity( uint32_t particles );
        // Uploads emitter @p emitter's tick into @p gpu and adds its frame emitter; false = it sits out this tick.
        bool PrepareEmitter( ParticleEmitterGpu& gpu, const VFX::EmitterInstance& emitter,
                             const ParticlePoolRange& range, uint32_t entityId, const glm::vec3& worldPos,
                             float stepSeconds, uint32_t stepSerial );
        // Makes @p gpu's per-tick buffers (step table, counters, dispatch args, channel spawns); false = it sits
        // out.
        static bool EnsureTickBuffers( ParticleEmitterGpu& gpu, uint32_t stepCapacity );
        // Compiles emitter @p index of @p system into @p gpu (program, columns, parameters, curves).
        void BuildEmitter( ParticleEmitterGpu& gpu, const Assets::Serialization::VFXSystemData& system,
                           std::size_t index, uint32_t entityId );
        // The host program for one compiled stack, by its key (null = refused, said once).
        std::shared_ptr<ComputePipeline> ProgramFor( uint64_t key, const std::string& name,
                                                     const std::string& hostSource );

        std::unordered_map<uint32_t, ParticleSystemGpu>                m_Systems;
        std::unordered_map<uint64_t, std::shared_ptr<ComputePipeline>> m_Programs;
        std::vector<ParticleFrameEmitter>                              m_FrameEmitters;
        ParticlePoolRanges                                             m_Ranges;
        ParticlePoolBuffers                                            m_Pool;
        ParticleTickClaim                                              m_Claim;
        ParticleDebugRequest                                           m_DebugRequest;
        std::vector<ParticleEmitterDebug>                              m_Debug;
        uint32_t                                                       m_DebugSerial = 0;
    };
} // namespace Desert::Graphic::System
