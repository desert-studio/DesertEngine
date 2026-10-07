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
    // GPU particle system. Per emitter: a PERSISTENT storage buffer of particle state that a compute shader
    // (ParticleSimulate) integrates + respawns each frame, drawn as camera-facing billboards (ParticleBillboard)
    // in the Transparency phase. The flow is: PrepareFrame snapshots emitters (CPU) in
    // BeginScene, Simulate dispatches the compute from the frame graph's "Particles: Simulate" node (its
    // PassContext, the emitters' buffers bound by shader name), and the registered Transparency pass draws the
    // result.
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

        // CPU snapshot of the scene's emitters (params, world position) and of this frame's steps from the
        // scene's VFXWorld (id bases, seeds, budgets — uploaded as each emitter's step table). A state whose
        // generation the world has moved past is zeroed first. Call once per frame in BeginScene. The
        // simulation's time is the VFXWorld's fixed step; this system reads no clock and no frame timestep.
        void PrepareFrame( const ::Desert::Core::Scene& scene );

        // How many fixed steps this frame simulates: the most any declared emitter runs. SceneRenderer adds
        // one "Particles: Simulate" node per step, so step s+1 reads what step s wrote across a graph barrier.
        [[nodiscard]] uint32_t SimulationStepCount() const;

        // Records step @p step: one DispatchCompute per declared emitter that runs that step, on @p context's
        // command buffer, from that emitter's binding block (DeclareSimulateBindings with the same step, the
        // same emitters in the same order: block n is the n-th such emitter) plus its push constants with
        // Counts.y = @p step; the first refused dispatch is returned, naming the pass and the slot.
        [[nodiscard]] Common::BoolResultStr Simulate( const RDG::PassContext& context, uint32_t step );

        // Imports every emitter of this frame's particle-state and step-table buffers into @p graph
        // (Renderer::ImportBuffer) and keeps their handles in the frame emitters, which the step nodes declare
        // through DeclareSimulateBindings: the graph then places the barrier against the previous step's (or
        // the previous graph's) write of the same buffer. An emitter whose buffers cannot be imported is logged
        // and sits the frame out: Simulate dispatches only emitters whose writes the graph was told about. Call
        // once per frame graph, after PrepareFrame. The ExternalBuffers live in m_FrameEmitters, which the
        // graph points at until its Execute ends; only the next PrepareFrame refills it.
        void ImportSimulationBuffers( RDG::Builder& graph );

        // The setup of step @p step's node: one binding block per imported emitter that runs the step (block
        // n = the n-th, in m_FrameEmitters order, the order Simulate walks), against ParticleSimulate's layout
        // - Particles and StepTable StorageWrite (each entry is the declaration of its access) and the SimPush
        // bytes. With no simulation pipeline it declares nothing and Simulate dispatches nothing.
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
            glm::uvec4 Counts;     // maxParticles, step index into the step table, 0, local-space
        };

        // One element of ParticleSimulate's step table (binding 1, `struct VFXStep`).
        struct StepGpu
        {
            uint32_t SpawnCount = 0; // zeroed by the upload, consumed atomically by the step
            uint32_t IdBase     = 0;
            uint32_t Seed       = 0;
            uint32_t Budget     = 0;
        };
        static_assert( sizeof( StepGpu ) == kParticleStepStride );

        // One emitter's persistent GPU state, cached across frames by entity id.
        struct EmitterGpu
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Particles; // persistent particle state
            std::shared_ptr<ShaderResources::StorageBuffer> Steps;     // this frame's step table

            // The billboard material is PER EMITTER, never shared across them: the particle SSBO is a
            // descriptor, a descriptor set belongs to the material, and the set is written at most once
            // per frame before its first bind — with one shared material every emitter after the first
            // drew the FIRST one's buffer, silently (the rebind was swallowed by the per-frame stamp).
            // Same arrangement as JumpFloodOutlineRenderer's per-step materials.
            std::unique_ptr<MaterialParticleBillboard> Material;

            int      MaxParticles  = 0;
            uint32_t StepCapacity  = 0;
            uint64_t Generation    = 0; // the VFXWorld instance generation this state belongs to; 0 = fresh
        };

        // This frame's active emitters (built by PrepareFrame, consumed by Simulate + the draw pass).
        struct FrameEmitter
        {
            EmitterGpu* Gpu = nullptr;
            SimPush     Push;
            bool        Additive  = true;
            uint32_t    StepCount = 0; // fixed steps to run this frame
            // This frame's graph imports of Gpu->Particles and Gpu->Steps (ImportSimulationBuffers), and
            // whether both were imported: Simulate dispatches only a declared emitter.
            RDG::ExternalBuffer ParticlesImport;
            RDG::ExternalBuffer StepsImport;
            RDG::BufferRef      ParticlesRef; // this frame's graph handle of ParticlesImport, read by ParticlePass
            RDG::BufferRef      StepsRef;     // this frame's graph handle of StepsImport, consumed by the steps
            bool                Declared = false;
        };

        bool        CreatePipelines();
        // Whether ParticlePass draws @p fe this frame - the one condition its Declare and its exec both walk the
        // emitters by, so the exec's n-th drawn emitter opens the n-th declared block.
        static bool IsDrawn( const FrameEmitter& fe );
        // Whether step @p step's node dispatches @p fe - the one condition DeclareSimulateBindings and Simulate
        // both walk the emitters by.
        static bool RunsStep( const FrameEmitter& fe, uint32_t step );
        // The billboard pipeline of @p fe's blend (null when that pipeline failed to build).
        GraphicsPipeline* BillboardPipeline( const FrameEmitter& fe ) const;
        EmitterGpu& GetOrCreate( uint32_t entityId, int maxParticles, uint32_t stepCapacity );

        std::shared_ptr<ComputePipeline>  m_SimPipeline;
        std::shared_ptr<GraphicsPipeline> m_AddPipeline;   // additive blend
        std::shared_ptr<GraphicsPipeline> m_AlphaPipeline; // alpha blend
        // The three shaders' binding layouts, kept between frames (re-derived on a swapped or reloaded shader).
        mutable ShaderBindingLayoutCache m_SimLayout;
        mutable ShaderBindingLayoutCache m_AddLayout;
        mutable ShaderBindingLayoutCache m_AlphaLayout;

        std::unordered_map<uint32_t, EmitterGpu> m_Emitters;
        std::vector<FrameEmitter>                m_FrameEmitters;
    };
} // namespace Desert::Graphic::System
