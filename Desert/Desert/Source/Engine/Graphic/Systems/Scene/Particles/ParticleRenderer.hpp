#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Materials/Particles/MaterialParticleBillboard.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

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

namespace Desert::Graphic::RDG
{
    class Builder;
    class PassContext;
} // namespace Desert::Graphic::RDG

namespace Desert::Graphic::System
{
    // GPU particle system (VFX-07: the world's particle pool). Every emitter of the scene owns a range of ONE
    // persistent pool (ParticlePoolRanges); its free and alive lists live in the same range of two pool-sized
    // index lists. Per frame: PrepareFrame snapshots the emitters and their VFXWorld steps (CPU); the frame build
    // adds "Particles: Compact 0", then per fixed step "Particles: Spawn+Update {s}" and "Particles: Compact
    // {s+1}" (Compute nodes declaring the pool, the lists and each emitter's Counters); the billboard draw
    // (DrawPass, translucency) reads the alive list and draws INDIRECT from the last compact's slot of the
    // emitter's Counters - six vertices per particle the compact found alive, none for the dead.
    class ParticleRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~ParticleRenderer() override; // defined in the .cpp so the per-emitter
                                      // unique_ptr<MaterialParticleBillboard> sees the complete type

        Common::BoolResultStr Initialize() override;

        // The billboard draw over the lit scene target (LOAD), added by SceneRenderer::AddFrameTranslucency after
        // the fog apply and the cloud composite. Empty (no TargetFramebuffer) without a target or pipeline.
        [[nodiscard]] SystemRasterPass DrawPass();

        // Drops every cached emitter — see IRenderSystem::OnSceneReplaced, kind 2. m_Emitters is keyed by
        // the raw entt entity value, and a fresh registry hands those out from zero again, so the next
        // scene's first emitter IS the previous scene's first emitter as far as this cache can tell: same
        // key, and GetOrCreate reuses the buffer outright whenever MaxParticles happens to match — live
        // particles at the old scene's world positions, and the old spawn phase, included.
        //
        // Mid-session, an emitter whose entity is destroyed is dropped by PrepareFrame
        // (RetireDestroyedEmitters); its buffers and material go through the allocator's deletion ring, so
        // the frame still reading them finishes first. This clears the rest at a scene swap.
        void OnSceneReplaced() override;

        // CPU snapshot of the scene's emitters (params, world position) and of this frame's steps from the
        // scene's VFXWorld (id bases, seeds, budgets — uploaded as each emitter's step table). A state whose
        // generation the world has moved past is zeroed first. Call once per frame in BeginScene. The
        // simulation's time is the VFXWorld's fixed step; this system reads no clock and no frame timestep.
        void PrepareFrame( const ::Desert::Core::Scene& scene );

        // How many fixed steps this frame simulates: the most any declared emitter runs. SceneRenderer adds, after
        // "Particles: Compact 0", one "Particles: Spawn+Update {s}" and one "Particles: Compact {s+1}" per step.
        [[nodiscard]] uint32_t SimulationStepCount() const;

        // Compact @p compact (0..SimulationStepCount()): one ParticleCompact dispatch per declared emitter that
        // runs it (compacts 0..its StepCount), rebuilding the emitter's free and alive lists and its Counters slot
        // (compact & 1) - the indirect draw arguments. Compact 0 of an emitter that needs a reset kills its range
        // first. Block n is the n-th such emitter (DeclareCompactBindings walks them by the same RunsCompact).
        [[nodiscard]] Common::BoolResultStr Compact( const RDG::PassContext& context, uint32_t compact );
        void DeclareCompactBindings( RDG::PassBuilder& pass, uint32_t compact ) const;

        // Spawn+Update of step @p step: one ParticleSimulate dispatch per declared emitter that runs the step,
        // reading the lists compact @p step built; block n = the n-th such emitter (DeclareSimulateBindings).
        [[nodiscard]] Common::BoolResultStr Simulate( const RDG::PassContext& context, uint32_t step );

        // Imports the world pool (particles, free list, alive list) once and every emitter's step table and
        // Counters into @p graph (Renderer::ImportBuffer + RegisterExternal); an emitter whose buffers cannot be
        // imported is logged and sits the frame out, and with no pool no emitter is declared. With no declared
        // emitter the particle nodes declare nothing and the graph culls them. Call once per frame graph, after
        // PrepareFrame.
        void ImportSimulationBuffers( RDG::Builder& graph );

        // The setup of step @p step's Spawn+Update node: per emitter running it, Particles StorageWrite, StepTable
        // / FreeList / AliveList / Counters StorageRead, and the SimPush bytes.
        void DeclareSimulateBindings( RDG::PassBuilder& pass, uint32_t step ) const;

    private:
        // Push constant for ParticleSimulate (must match the shader's 128-byte block).
        struct SimPush
        {
            glm::vec4  EmitterPos; // xyz world pos, w the fixed step length (seconds)
            glm::vec4  Gravity;    // xyz gravity, w unused
            glm::vec4  Direction;  // xyz dir, w cone half-angle (rad)
            glm::vec4  Params;     // startSpeed, speedVar, lifetime, lifetimeVar
            glm::vec4  StartColor; // rgb + start alpha
            glm::vec4  EndColor;   // rgb + end alpha
            glm::vec4  Sizes;      // startSize, endSize, 0, 0
            glm::uvec4 Counts;     // range count, step index into the step table, range pool base, local-space
        };

        // ParticleCompact's push constant: x = pool base, y = particle count, z = the slot filled, w = reset.
        struct CompactPush
        {
            glm::uvec4 Range;
        };

        // One element of ParticleSimulate's step table (binding 1, `struct VFXStep`).
        struct StepGpu
        {
            uint32_t IdBase = 0;
            uint32_t Seed   = 0;
            uint32_t Budget = 0;
        };
        static_assert( sizeof( StepGpu ) == kParticleStepStride );

        // One slot of an emitter's Counters (Common/ParticlePool.glslh ParticleDrawSlot); the first four members
        // are the VkDrawIndirectCommand the billboard draw reads.
        struct DrawSlotGpu
        {
            uint32_t VertexCount   = 0;
            uint32_t InstanceCount = 1;
            uint32_t FirstVertex   = 0;
            uint32_t FirstInstance = 0;
            uint32_t FreeCount     = 0;
            uint32_t Pad[3]        = {};
        };
        static_assert( sizeof( DrawSlotGpu ) == kParticleDrawSlotStride );

        // One emitter's GPU state, cached across frames by entity id. Its particles are Range of the world pool.
        struct EmitterGpu
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Steps;    // this frame's step table
            std::shared_ptr<ShaderResources::StorageBuffer> Counters; // two draw slots, uploaded zeroed per frame

            // The billboard material is PER EMITTER (its camera block per draw), never shared across them.
            std::unique_ptr<MaterialParticleBillboard> Material;

            ParticlePoolRange Range;
            uint32_t          StepCapacity = 0;
            uint64_t          Generation   = 0;  // the VFXWorld instance generation this state belongs to
            bool              NeedsReset = true; // compact 0 kills the range (fresh, moved, restarted, pool grew)
        };

        // The world pool: one persistent buffer of particles and two of indices, sized to the ranges' End.
        struct WorldPool
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Particles;
            std::shared_ptr<ShaderResources::StorageBuffer> FreeList;
            std::shared_ptr<ShaderResources::StorageBuffer> AliveList;
            uint32_t                                        Capacity = 0;
            RDG::ExternalBuffer                             ParticlesImport;
            RDG::ExternalBuffer                             FreeImport;
            RDG::ExternalBuffer                             AliveImport;
            RDG::BufferRef                                  ParticlesRef;
            RDG::BufferRef                                  FreeRef;
            RDG::BufferRef                                  AliveRef;
            bool                                            Declared = false;
        };

        // This frame's active emitters (built by PrepareFrame, consumed by the nodes and the draw pass).
        struct FrameEmitter
        {
            EmitterGpu*         Gpu = nullptr;
            SimPush             Push;
            bool                Additive  = true;
            uint32_t            StepCount = 0; // fixed steps to run this frame; compacts 0..StepCount
            RDG::ExternalBuffer StepsImport;
            RDG::ExternalBuffer CountersImport;
            RDG::BufferRef      StepsRef;
            RDG::BufferRef      CountersRef; // read by ParticlePass as IndirectArgs
            bool                Declared = false;
        };

        bool        CreatePipelines();
        // Whether ParticlePass draws @p fe this frame - the one condition its Declare and its exec both walk the
        // emitters by, so the exec's n-th drawn emitter opens the n-th declared block.
        static bool IsDrawn( const FrameEmitter& fe );
        // Whether step @p step's node dispatches @p fe - the one condition DeclareSimulateBindings and Simulate
        // both walk the emitters by.
        static bool RunsStep( const FrameEmitter& fe, uint32_t step );
        // Whether compact @p compact's node dispatches @p fe (compacts 0..StepCount of a declared emitter).
        static bool RunsCompact( const FrameEmitter& fe, uint32_t compact );
        // Grows the world pool to hold @p particles (recreating it: every emitter restarts); false when it failed.
        bool EnsurePoolCapacity( uint32_t particles );
        // The billboard pipeline of @p fe's blend (null when that pipeline failed to build).
        GraphicsPipeline* BillboardPipeline( const FrameEmitter& fe ) const;
        EmitterGpu&       GetOrCreate( uint32_t entityId, uint32_t stepCapacity );

        std::shared_ptr<ComputePipeline>  m_SimPipeline;
        std::shared_ptr<ComputePipeline>  m_CompactPipeline;
        std::shared_ptr<GraphicsPipeline> m_AddPipeline;   // additive blend
        std::shared_ptr<GraphicsPipeline> m_AlphaPipeline; // alpha blend
        // The three shaders' binding layouts, kept between frames (re-derived on a swapped or reloaded shader).
        mutable ShaderBindingLayoutCache m_SimLayout;
        mutable ShaderBindingLayoutCache m_CompactLayout;
        mutable ShaderBindingLayoutCache m_AddLayout;
        mutable ShaderBindingLayoutCache m_AlphaLayout;

        std::unordered_map<uint32_t, EmitterGpu> m_Emitters;
        std::vector<FrameEmitter>                m_FrameEmitters;
        ParticlePoolRanges                       m_Ranges;
        WorldPool                                m_Pool;
    };
} // namespace Desert::Graphic::System
