#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/ECS/VFXPointSet.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
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
#include <utility>
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
        // The entity's full transform (cm; UE Engine.Owner.SystemLocalToWorld): a World emitter's newborns are
        // placed through it, a Local emitter's Spawn-from-Channel positions come back through its inverse.
        glm::mat4  LocalToWorld = glm::mat4( 1.0f );
        glm::vec4  Step;   // x = the fixed step length (seconds), yzw 0
        glm::uvec4 Counts; // range count, step index into the step table, range pool base, local-space
        glm::uvec4 Stack;  // x = the emitter's own serial of this tick's step 0 (the random streams' step), y = the
                           // system's event step serial of step 0 (Common/VFXEvents.glslh), zw 0
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

    // ParticleDispatchArgs' push constant: x = the step whose Spawn+Update and following compact it sizes, y = the
    // system's event step serial of step 0 (ParticleSimPush::Stack.y).
    struct ParticleArgsPush
    {
        glm::uvec4 Step;
    };

    // VFX-16 (UE Niagara Debugger, dev only): ParticleDebugReadback's push constant and its readback's shape. The
    // readback is 8 header uints (alive, free, debug serial, rows written, the events the emitter stored over its
    // last tick - Common/VFXEvents.glslh's tick counts -, 3 zeros) then per row the pool index and the emitter's
    // float attribute components, then its int components (their bits); one page is kParticleDebugMaxRows alive
    // particles.
    struct ParticleDebugPush
    {
        glm::uvec4 Page;  // x = last compact's slot, y = first alive row, z = rows (0 = counts only), w = float columns
        glm::uvec4 Range; // x = drawn alive half's offset, y = range count, z = pool base, w = debug serial (never 0)
        glm::uvec4 Columns; // x = int columns; yzw = 0
    };
    constexpr uint32_t kParticleDebugHeaderUints = 8u; // ParticleDebugReadback.shader PARTICLE_DEBUG_HEADER_UINTS
    constexpr uint32_t kParticleDebugMaxRows     = 64u;

    // VFX-LIGHT (UE Niagara Light Renderer): ParticleLightReadback's push constant and its readback's shape - 4
    // header uints (lights written, frame serial, 0, 0), then per light 8 floats: world position (cm), the larger
    // SpriteSize axis (cm), Color rgba. The CPU reads the copy FramesInFlight frames later (no stall) and turns
    // each row into a point light with the emitter's Light row (ParticleWorldGpu::ReadLightReadbacks).
    struct ParticleLightPush
    {
        glm::uvec4 Page;       // x = the last compact's slot, y = lights wanted, z = serial (never 0), w = 0
        glm::uvec4 AliveRange; // x = the drawn alive half's offset in AliveList, yzw = 0
    };
    constexpr uint32_t kParticleLightHeaderUints = 4u;
    constexpr uint32_t kParticleLightRowFloats   = 8u;

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
    // false until the first copy came back). Columns / RowIndex / Floats / Ints only for the requested emitter.
    struct ParticleEmitterDebug
    {
        uint32_t                             EntityId     = 0;
        uint32_t                             Emitter      = 0;
        std::string                          Name;
        uint32_t                             Capacity     = 0;
        uint32_t                             Alive        = 0;
        uint32_t                             Free         = 0;
        uint32_t                             FramesOld    = 0;
        uint32_t                             Events       = 0; // stored over the emitter's last tick (all kinds)
        bool                                 Read         = false;
        uint32_t                             FloatColumns = 0;
        uint32_t                             IntColumns   = 0;
        std::vector<VFX::VFXAttributeLayout> Columns;  // the compiled layout's attributes (Float/Int Start, Count)
        std::vector<uint32_t>                RowIndex; // pool index per row
        std::vector<float>                   Floats;   // RowIndex.size() x FloatColumns, row-major
        std::vector<int32_t>                 Ints;     // RowIndex.size() x IntColumns, row-major
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
        // x = range count (column stride), y = pool base, z = 1 local, w = the emitter's Sprite row alignment
        // (0 Unaligned, 1 VelocityAligned; Vertex_ParticleSprite.glslh)
        glm::uvec4 Range      = glm::uvec4( 0u );
        glm::uvec4 Columns0   = glm::uvec4( kParticleAttrNone ); // Position, SpriteSize, Color, SpriteRotation
        glm::uvec4 Columns1   = glm::uvec4( kParticleAttrNone ); // Age, Lifetime, RibbonWidth, RibbonLinkOrder
        // The entity's full transform (cm): a Local emitter's particles are drawn through it (UE: a local-space
        // emitter's LocalToWorld in the sprite / mesh vertex factory).
        glm::mat4 LocalToWorld = glm::mat4( 1.0f );
        // x = the Ribbon row's RibbonIdBinding attribute (an Int; none = one ribbon), y = Scale, z =
        // MeshOrientation, w = Velocity (a VelocityAligned sprite's axis).
        glm::uvec4 Columns2 = glm::uvec4( kParticleAttrNone );
        // The emitter's Ribbon row (Vertex_ParticleRibbon.glslh): x = Width (cm), y = UV mode (0 Stretch,
        // 1 TileByDistance), z = UVTileDistance (cm); zero without a Ribbon row.
        glm::vec4 Ribbon = glm::vec4( 0.0f );
    };
    static_assert( sizeof( ParticleDrawLayoutGpu ) == kParticleDrawLayoutSize );

    // One emitter's GPU state in the scene's pool, cached across ticks by entity id. Its particles are Range of
    // the pool; its alive lists are the two halves [2 Base + h Count, 2 Base + (h + 1) Count) of the alive list.
    // THE EVENT CHANNELS OF ONE SYSTEM (PORT-VFX-MODULES; Common/VFXEvents.glslh states the layout): a channel per
    // (source emitter, event kind) some Event Handler of the system consumes, in the order handlers first name
    // them (emitter order, handler order); each holds two halves of Capacity rows, Capacity = its source's
    // particle capacity (@p capacities, 0 for an emitter that does not simulate: its channel never fills).
    // Tables[k] is emitter k's event table; BufferRows the uvec4 rows of the system's event buffer (>= 1).
    struct ParticleEventLayout
    {
        std::vector<std::vector<glm::uvec4>> Tables;
        uint32_t                             BufferRows = 1;
    };
    [[nodiscard]] ParticleEventLayout BuildParticleEventLayout( const Assets::Serialization::VFXSystemData& system,
                                                                const std::vector<uint32_t>& capacities );

    struct ParticleEmitterGpu
    {
        std::shared_ptr<ShaderResources::StorageBuffer> Steps;         // this tick's step table
        std::shared_ptr<ShaderResources::StorageBuffer> Counters;      // two draw slots, persistent, GPU-opened
        std::shared_ptr<ShaderResources::StorageBuffer> DispatchArgs;  // Spawn+Update's and the next compact's
        std::shared_ptr<ShaderResources::StorageBuffer> ChannelSpawns; // this tick's Spawn from Channel particles
        // PORT-VFX-MODULES: the emitter's event table (uploaded with the parameters every tick) and its SYSTEM's
        // event buffer (ParticleSystemGpu::Events, shared by the system's emitters: GPU-written, read the step
        // after).
        std::shared_ptr<ShaderResources::StorageBuffer> EventTable;
        std::vector<glm::uvec4>                         EventTableRows;
        std::shared_ptr<ShaderResources::StorageBuffer> Events;

        // VFX-HOST: the emitter's compiled stack - its host program, its attribute columns (SoA, Count rows per
        // component), its parameter rows and its system's curve atlas (re-uploaded every tick: the copies are
        // per frame in flight).
        std::shared_ptr<ComputePipeline>                Pipeline; // null = the stack was refused, it sits out
        std::shared_ptr<ShaderResources::StorageBuffer> AttributeFloats;
        std::shared_ptr<ShaderResources::StorageBuffer> AttributeInts;
        std::shared_ptr<ShaderResources::StorageBuffer> Params;
        std::shared_ptr<ShaderResources::StorageBuffer> Curves;
        // VFX-15: the placement's point sets this emitter's PointSet.* bindings read, back to back (PointSetData).
        std::shared_ptr<ShaderResources::StorageBuffer> PointSets;
        // Where the sprite draw and the sort read the drawn attributes in AttributeFloats (VFX-SPRITE-DRAW): its
        // columns are the compiled layout's, its range and emitter position are re-uploaded every tick.
        std::shared_ptr<ShaderResources::StorageBuffer> DrawLayout;
        ParticleDrawLayoutGpu                           DrawColumns;
        std::vector<glm::vec4>                          ParamRows;
        // The ParamRows a User.* binding fills (row, user parameter name): the placement's overrides replace
        // these rows, every other row is the stack's own.
        std::vector<std::pair<uint32_t, std::string>> UserRows;
        // The ParamRow of each PointSet.* binding (row, set name): (first, count) of the placement's set in
        // PointSetData (ApplyPointSets).
        std::vector<std::pair<uint32_t, std::string>> PointSetRows;
        // The PointSets buffer's contents: xyz = the painted point in the placement frame (cm; the simulation's
        // LocalToWorld places it like every other shape offset), w = the set's
        // cumulative weight through it, normalised to 1 (the weighted pick's search key). Never empty.
        std::vector<glm::vec4>                          PointSetData;
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
        // When its simulation runs (VFX::VFXTickStage, from what its stack reads): PreOpaque or PostOpaque.
        VFX::VFXTickStage TickStage = VFX::VFXTickStage::PreOpaque;
        // Its own stack reads the scene depth (VFX_READS_SCENE_DEPTH: the program declares CollisionView /
        // SceneDepth). TickStage is PostOpaque then, and also when an emitter linked to it by events reads it
        // (VFX::UnifyEventTickStages) - the stage, not the slots, is what the link moves.
        bool ReadsSceneDepth = false;
        // The alive half / draw slot the last scheduled tick's last compact fills (its StepCount & 1), and whether
        // there was such a tick: what a PostOpaque emitter's draws recorded before this frame's simulation read
        // (ParticleFrameEmitter::PreviousHalf).
        uint32_t LastHalf    = 0;
        bool     HasLastHalf = false;

        // VFX-16: the debugger's per-frame-in-flight readback (made on the first debugged tick, dropped when the
        // emitter is rebuilt) and what its rows mean.
        std::shared_ptr<ShaderResources::StorageBuffer> DebugReadback;
        std::vector<VFX::VFXAttributeLayout>            DebugColumns;
        std::string                                     DebugName;
        uint32_t                                        DebugEntity  = 0;
        uint32_t                                        DebugEmitter = 0;

        // VFX-LIGHT: the Light row's per-frame-in-flight readback (made on the first tick with an enabled Light
        // row, kept while the emitter lives).
        std::shared_ptr<ShaderResources::StorageBuffer> LightReadback;
    };

    // One enabled renderer row of an emitter this tick (UE: the FNiagaraRenderer made per
    // UNiagaraRendererProperties): the drawing view builds one draw of the emitter's particles from each.
    // Material is ParticleRendererMaterial's answer for the row (ParticleWorldGpu.cpp): null = the engine's
    // particle material for Kind (Sprite: ParticleSpriteDefault; Ribbon: the engine ribbon material; Mesh: the
    // mesh's own materials). Ribbon/Mesh point into the system's data (ParticleSystemGpu::System holds it) and
    // are set exactly for their Kind.
    struct ParticleFrameRenderer
    {
        Assets::Serialization::VFXRendererKind              Kind = Assets::Serialization::VFXRendererKind::Sprite;
        uint32_t                                            Row  = 0; // the row's index in the emitter's Renderers
        Common::AssetHandle                                 Material;
        const Assets::Serialization::VFXRibbonRendererData* Ribbon = nullptr;
        const Assets::Serialization::VFXMeshRendererData*   Mesh   = nullptr;
    };

    // One emitter active in this scene update.
    struct ParticleFrameEmitter
    {
        uint32_t            EntityId = 0;
        uint32_t            Emitter  = 0; // the emitter's index in its system (VFXSystemData::Emitters)
        ParticleEmitterGpu* Gpu      = nullptr;
        ParticleSimPush     Push;
        ComputePipeline*    Pipeline = nullptr; // the emitter's stack host program (Gpu->Pipeline)
        // The emitter's enabled renderer rows in `.dfx` order (VFXS 2); none = the emitter simulates and draws
        // nothing (UE: an emitter without renderers).
        std::vector<ParticleFrameRenderer> Renderers;
        // VFX-LIGHT: the emitter's first enabled Light row (null = it casts no light); the system's data holds it.
        const Assets::Serialization::VFXLightRendererData* Light = nullptr;
        uint32_t            StepCount = 0; // fixed steps this tick; compacts 0..StepCount
        // The half the previous tick's last compact filled, read by the draws recorded BEFORE this emitter's
        // simulation (a PostOpaque emitter's G-buffer rows and Mesh instances); HasPrevious false = no previous tick
        // since the last reset, and those draws are skipped.
        uint32_t PreviousHalf = 0;
        bool     HasPrevious  = false;
        // VFX-PORT-2: the emitter's fixed Bounds through this tick's placement transform (VFXBoundsAt), cm: what every view and
        // shadow view culls the emitter's draws by (ParticleRenderer).
        Common::Math::AABB WorldBounds;
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
        // The VFXComponent overrides the emitters' User rows were last filled from (UE OverrideParameters).
        std::vector<ECS::VFXUserOverride> Overrides;
        // The point sets the emitters' PointSet rows were last filled from (VFX-15).
        std::vector<ECS::VFXPointSet> PointSets;
        // PORT-VFX-MODULES: the system's event buffer (its emitters' Events) and the event step serial of the next
        // tick's step 0 - contiguous over the system's simulated steps, so "the step before" of step 0 is the last
        // step of the previous tick. Starts at 1: the zeroed buffer's halves hold serial 0, which no step reads as
        // its own.
        std::shared_ptr<ShaderResources::StorageBuffer> Events;
        uint32_t                                        EventSerial = 1;
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

        // VFX-LIGHT (UE Niagara Light Renderer): the simulating view reads every Light row's readback right after
        // PrepareTick (before this frame's "Particles: Light Readback" writes the copy it reads) into the scene's
        // particle lights, which EVERY view of the scene adds to its local lights (SceneRenderer, after
        // ParticleRenderer::PrepareFrame). A tick that does not simulate keeps the last lights.
        void                                                                 ReadLightReadbacks();
        [[nodiscard]] const std::vector<ShaderProtocols::PointLightPayload>& ParticleLights() const
        {
            return m_Lights;
        }
        [[nodiscard]] uint32_t LightSerial() const
        {
            return m_LightSerial;
        }

        // Counts are read back while the panel's request is enabled or the world's debugger shows the viewport
        // overview (VFXDebugSettings::Overview, taken at PrepareTick); the attribute table only for the request.
        [[nodiscard]] bool DebugReading() const
        {
            return m_DebugRequest.Enabled || m_DebugCounts;
        }
        void FillDebugStats( VFX::VFXDebugStats& stats ) const override;

    private:
        // Grows the pool to hold @p particles (recreating it: every emitter restarts); false when it failed.
        bool EnsurePoolCapacity( uint32_t particles );
        // Uploads emitter @p emitter's tick into @p gpu and adds its frame emitter; false = it sits out this tick.
        bool PrepareEmitter( ParticleEmitterGpu& gpu, const VFX::EmitterInstance& emitter,
                             const Assets::Serialization::VFXEmitterData& data, const ParticlePoolRange& range,
                             uint32_t entityId, uint32_t emitterIndex, const glm::mat4& localToWorld,
                             float stepSeconds, uint32_t eventSerial );
        // Builds @p system's event buffer and each emitter's event table (BuildParticleEventLayout) and gives the
        // emitters linked by events one tick stage (VFX::UnifyEventTickStages), after its emitters were built; an
        // emitter whose event buffers could not be made sits out (Capacity 0, said by name).
        static void BuildSystemEvents( ParticleSystemGpu& system, uint32_t entityId );
        // Makes @p gpu's per-tick buffers (step table, counters, dispatch args, channel spawns); false = it sits
        // out.
        static bool EnsureTickBuffers( ParticleEmitterGpu& gpu, uint32_t stepCapacity );
        // Compiles emitter @p index of @p system into @p gpu (program, columns, parameters, curves).
        void BuildEmitter( ParticleEmitterGpu& gpu, const Assets::Serialization::VFXSystemData& system,
                           std::size_t index, uint32_t entityId );
        // Fills every emitter's User rows of @p system from the placement's @p overrides (else the defaults).
        static void ApplyUserOverrides( ParticleSystemGpu& system, const std::vector<ECS::VFXUserOverride>& overrides,
                                        uint32_t entityId );
        // Fills every emitter's PointSetData (and its PointSet rows' first/count) from the placement's @p sets,
        // each point as painted, in the placement frame (the simulation's LocalToWorld places it); a buffer too
        // small for the points is made anew.
        static void ApplyPointSets( ParticleSystemGpu& system, const std::vector<ECS::VFXPointSet>& sets,
                                    uint32_t entityId );
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
        std::vector<ShaderProtocols::PointLightPayload>                m_Lights;
        uint32_t                                                       m_LightSerial = 0;
        bool                                                           m_DebugCounts = false;
    };
} // namespace Desert::Graphic::System
