#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Materials/Particles/MaterialParticleBillboard.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

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
}

namespace Desert::Graphic::System
{
    // GPU particle system. Per emitter: a PERSISTENT storage buffer of particle state that a compute shader
    // (ParticleSimulate) integrates + respawns each frame, drawn as camera-facing billboards (ParticleBillboard)
    // in the Transparency phase. The flow is: PrepareFrame snapshots emitters (CPU) in
    // BeginScene, SimulateInFrame dispatches the compute (command buffer active) BEFORE the render graph, and
    // the registered Transparency pass draws the result.
    class ParticleRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~ParticleRenderer() override; // defined in the .cpp so the per-emitter
                                      // unique_ptr<MaterialParticleBillboard> sees the complete type

        Common::BoolResultStr Initialize() override;
        void                  RegisterPasses( RenderGraphBuilder& builder ) override;

        // Drops every cached emitter — see IRenderSystem::OnSceneReplaced, kind 2. m_Emitters is keyed by
        // the raw entt entity value, and a fresh registry hands those out from zero again, so the next
        // scene's first emitter IS the previous scene's first emitter as far as this cache can tell: same
        // key, and GetOrCreate reuses the buffer outright whenever MaxParticles happens to match — live
        // particles at the old scene's world positions, and the old spawn phase, included.
        //
        // Dropping them is done HERE rather than by re-keying on the UUID because the map also has to
        // SHRINK, and this is the only path that has idled the device first: a persistent particle SSBO
        // may still be being read by the last submitted frame, and this engine has no deferred-free queue.
        void OnSceneReplaced() override;

        // Camera cut - see IRenderSystem::OnTemporalHistoryReset. Every cached emitter's state is zeroed
        // (the same in-place clear as the editor's Restart: the buffer may still be read by the frame in
        // flight, so it is rewritten, never dropped), spawn carries go to zero and the simulated time - the
        // shader's noise seed - starts again at zero. The next frame simulates from the authored state.
        void OnTemporalHistoryReset() override;

        // CPU snapshot of the scene's emitters (params, world position, per-frame spawn budget, zeroed spawn
        // counters). Call once per frame in BeginScene.
        void PrepareFrame( const ::Desert::Core::Scene& scene );

        // Record the per-emitter compute dispatches. Call in OnUpdate, outside any render pass, BEFORE the
        // render graph records the billboard draw.
        //
        // @p frameSeconds is the FRAME'S timestep (SceneRenderer::UpdateInfo::Timestep) and the only time
        // this system integrates: the spawn budget, the shader's dt and its seed (the accumulated simulated
        // time) all come from it. Outside a `--play` capture that is the measured wall-clock step, so the
        // editor sees what it always saw; under one it is the fixed step, so two runs simulate alike. This
        // system reads no clock of its own - the ParticleTimestep suite holds it to that.
        void SimulateInFrame( float frameSeconds );

        // Imports every emitter of this frame's particle-state and spawn-counter buffers into @p graph
        // (Renderer::ImportBuffer) and returns their handles, which the node that runs SimulateInFrame
        // ("Particles: Simulate") declares as StorageWrite: the graph then places the barrier against the
        // previous graph's use of the same buffer (last frame's simulation of the persistent state). An
        // emitter whose buffers cannot be imported is logged and sits the frame out: SimulateInFrame
        // dispatches only emitters whose writes the graph was told about. Call once per frame graph, after
        // PrepareFrame. The ExternalBuffers live in m_FrameEmitters, which the graph points at until its
        // Execute ends; only the next PrepareFrame refills it.
        std::vector<RDG::BufferRef> ImportSimulationBuffers( RDG::Builder& graph );

    private:
        // Push constant for ParticleSimulate (must match the shader's 128-byte block).
        struct SimPush
        {
            glm::vec4  EmitterPos; // xyz world pos, w dt
            glm::vec4  Gravity;    // xyz gravity, w time
            glm::vec4  Direction;  // xyz dir, w cone half-angle (rad)
            glm::vec4  Params;     // startSpeed, speedVar, lifetime, lifetimeVar
            glm::vec4  StartColor; // rgb + start alpha
            glm::vec4  EndColor;   // rgb + end alpha
            glm::vec4  Sizes;      // startSize, endSize, 0, 0
            glm::uvec4 Counts;     // maxParticles, spawnBudget, enabled, 0
        };

        // One emitter's persistent GPU state, cached across frames by entity id.
        struct EmitterGpu
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Particles; // persistent particle state
            std::shared_ptr<ShaderResources::StorageBuffer> Counter;   // per-frame spawn counter

            // The billboard material is PER EMITTER, never shared across them: the particle SSBO is a
            // descriptor, a descriptor set belongs to the material, and the set is written at most once
            // per frame before its first bind — with one shared material every emitter after the first
            // drew the FIRST one's buffer, silently (the rebind was swallowed by the per-frame stamp).
            // Same arrangement as JumpFloodOutlineRenderer's per-step materials.
            std::unique_ptr<MaterialParticleBillboard> Material;

            int   MaxParticles = 0;
            float SpawnAccum   = 0.0f; // fractional spawn carry
        };

        // This frame's active emitters (built by PrepareFrame, consumed by SimulateInFrame + the draw pass).
        struct FrameEmitter
        {
            EmitterGpu* Gpu = nullptr;
            SimPush     Push;
            bool        Additive  = true;
            float       SpawnRate = 0.0f; // particles/s; turned into this frame's budget by SimulateInFrame
            bool        Looping   = true;
            // This frame's graph imports of Gpu->Particles and Gpu->Counter (ImportSimulationBuffers), and
            // whether both were imported: SimulateInFrame dispatches only a declared emitter.
            RDG::ExternalBuffer ParticlesImport;
            RDG::ExternalBuffer CounterImport;
            RDG::BufferRef      ParticlesRef; // this frame's graph handle of ParticlesImport, read by ParticlePass
            bool                Declared = false;
        };

        bool        CreatePipelines();
        EmitterGpu& GetOrCreate( uint32_t entityId, int maxParticles );
        // Rewrites @p gpu's particle state with zeros in place and drops its spawn carry. The failure is
        // returned for the caller to report, since only the caller knows why it asked.
        static Common::BoolResultStr ClearEmitterState( EmitterGpu& gpu );

        std::shared_ptr<ComputePipeline>  m_SimPipeline;
        std::shared_ptr<GraphicsPipeline> m_AddPipeline;   // additive blend
        std::shared_ptr<GraphicsPipeline> m_AlphaPipeline; // alpha blend

        std::unordered_map<uint32_t, EmitterGpu> m_Emitters;
        std::vector<FrameEmitter>                m_FrameEmitters;
        // Simulated seconds since the last OnTemporalHistoryReset: the sum of the frames' timesteps. The
        // shader's per-frame seed (Push.Gravity.w).
        double m_SimSeconds = 0.0;
    };
} // namespace Desert::Graphic::System
