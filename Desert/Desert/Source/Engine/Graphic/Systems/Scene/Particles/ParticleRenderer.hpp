#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Materials/Particles/MaterialParticleBillboard.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include "ParticleGpuLayout.hpp"
#include "ParticleWorldGpu.hpp"

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
    // One VIEW's side of the scene's GPU particle system (VFX-07b). The pool, the per-emitter step tables,
    // counters and dispatch arguments are the scene's (ParticleWorldGpu, owned by its VFXWorld); this system holds
    // what is per view: the billboard material per emitter (its camera block), the pipelines and this view's graph
    // imports. Per frame: PrepareFrame asks the scene's state to prepare the VFXWorld tick - the first view of
    // the tick simulates (ClaimsSimulation), every other view of it only draws. The simulating view's frame build
    // adds "Particles: Compact 0", then per fixed step "Particles: Dispatch Args {s}", "Particles: Spawn+Update
    // {s}" and "Particles: Compact {s+1}"; Spawn+Update and the later compacts are dispatched INDIRECT from the
    // arguments Dispatch Args wrote from the GPU counts (UE: indirect dispatch from the GPU instance counts).
    // Every view's billboard draw (DrawPass, translucency) reads the alive list and draws INDIRECT from the last
    // compact's slot of the emitter's Counters - six vertices per particle the compact found alive.
    class ParticleRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~ParticleRenderer() override; // defined in the .cpp so the per-emitter
                                      // unique_ptr<MaterialParticleBillboard> sees the complete type

        Common::BoolResultStr Initialize() override;

        // The billboard draw over the lit scene target (LOAD), added by SceneRenderer::AddFrameTranslucency after
        // the fog apply and the cloud composite - by EVERY view, simulating or not. Empty (no TargetFramebuffer)
        // without a target or pipeline.
        [[nodiscard]] SystemRasterPass DrawPass();

        // Drops this view's materials and imports (IRenderSystem::OnSceneReplaced, kind 2). m_Materials is keyed
        // by the raw entt entity value, which a fresh registry hands out from zero again. The GPU particle state
        // itself is the scene's and goes with it.
        void OnSceneReplaced() override;

        // Prepares the scene's VFXWorld tick (ParticleWorldGpu::PrepareTick) and this view's per-emitter
        // materials. Call once per frame in BeginScene. The simulation's time is the VFXWorld's fixed step; this
        // system reads no clock and no frame timestep.
        void PrepareFrame( const ::Desert::Core::Scene& scene );

        // Whether this view simulates the scene's particles this frame: it was the first view to prepare the
        // VFXWorld tick. SceneRenderer::AddFrameParticlesSimulate adds the simulation nodes only then.
        [[nodiscard]] bool ClaimsSimulation() const
        {
            return m_Simulates;
        }

        // How many fixed steps this frame simulates: the most any declared emitter runs (0 in a view that does not
        // simulate).
        [[nodiscard]] uint32_t SimulationStepCount() const;

        // Imports the scene's pool (particles, free list, alive list) and every emitter's Counters into @p graph,
        // and in the simulating view its step table and dispatch arguments too (Renderer::ImportBuffer +
        // RegisterExternal); an emitter whose buffers cannot be imported is logged and sits the frame out, and
        // with no pool no emitter is declared. With no declared emitter the particle nodes declare nothing and the
        // graph culls them. Call once per frame graph, after PrepareFrame, in every view.
        void ImportFrameBuffers( RDG::Builder& graph );

        // Compact @p compact (0..SimulationStepCount()): one ParticleCompact dispatch per declared emitter that
        // runs it, rebuilding its free list, the alive half (compact & 1) and its Counters slot (compact & 1) -
        // the indirect draw arguments. Compact 0 scans the whole range (and kills it first when it restarts);
        // a later compact scans only what the step touched and is dispatched INDIRECT from the step's Dispatch
        // Args (kParticleCompactArgsOffset). Block n is the n-th such emitter (DeclareCompactBindings).
        [[nodiscard]] Common::BoolResultStr Compact( const RDG::PassContext& context, uint32_t compact );
        void DeclareCompactBindings( RDG::PassBuilder& pass, uint32_t compact ) const;

        // Dispatch Args of step @p step: one single-thread ParticleDispatchArgs dispatch per emitter running the
        // step, writing from compact @p step's alive / free counts and the step's spawn budget the group counts of
        // Spawn+Update @p step and of compact @p step + 1, and opening compact @p step + 1's Counters slot.
        [[nodiscard]] Common::BoolResultStr BuildDispatchArgs( const RDG::PassContext& context, uint32_t step );
        void DeclareDispatchArgsBindings( RDG::PassBuilder& pass, uint32_t step ) const;

        // Spawn+Update of step @p step: one ParticleSimulate dispatch per declared emitter that runs the step,
        // INDIRECT from the step's Dispatch Args (kParticleSimulateArgsOffset); block n = the n-th such emitter.
        [[nodiscard]] Common::BoolResultStr Simulate( const RDG::PassContext& context, uint32_t step );
        void DeclareSimulateBindings( RDG::PassBuilder& pass, uint32_t step ) const;

    private:
        // This view's imports of the scene's pool.
        struct ViewPool
        {
            RDG::ExternalBuffer ParticlesImport;
            RDG::ExternalBuffer FreeImport;
            RDG::ExternalBuffer AliveImport;
            RDG::BufferRef      ParticlesRef;
            RDG::BufferRef      FreeRef;
            RDG::BufferRef      AliveRef;
            bool                Declared = false;
        };

        // One emitter of the scene's tick as this view sees it (built by PrepareFrame, consumed by the nodes and
        // the draw pass).
        struct ViewEmitter
        {
            const ParticleFrameEmitter* Frame    = nullptr;
            MaterialParticleBillboard*  Material = nullptr;
            RDG::ExternalBuffer         StepsImport;
            RDG::ExternalBuffer         CountersImport;
            RDG::ExternalBuffer         ArgsImport;
            RDG::BufferRef              StepsRef;
            RDG::BufferRef              CountersRef; // read by ParticlePass as IndirectArgs
            RDG::BufferRef              ArgsRef;     // written by Dispatch Args, read as IndirectArgs
            bool                        Declared = false;
        };

        bool CreatePipelines();
        // Whether ParticlePass draws @p ve this frame - the one condition its Declare and its exec both walk the
        // emitters by, so the exec's n-th drawn emitter opens the n-th declared block.
        static bool IsDrawn( const ViewEmitter& ve );
        // Whether step @p step's Dispatch Args and Spawn+Update nodes dispatch @p ve.
        bool RunsStep( const ViewEmitter& ve, uint32_t step ) const;
        // Whether compact @p compact's node dispatches @p ve (compacts 0..StepCount of a declared emitter).
        bool RunsCompact( const ViewEmitter& ve, uint32_t compact ) const;
        // The billboard pipeline of @p ve's blend (null when that pipeline failed to build).
        GraphicsPipeline* BillboardPipeline( const ViewEmitter& ve ) const;
        // Interim (VFX-08 step R replaces it): whether @p ve composites additively.
        static bool DrawsAdditive( const ViewEmitter& ve );

        std::shared_ptr<ComputePipeline>  m_SimPipeline;
        std::shared_ptr<ComputePipeline>  m_CompactPipeline;
        std::shared_ptr<ComputePipeline>  m_ArgsPipeline;
        std::shared_ptr<GraphicsPipeline> m_AddPipeline;   // additive blend
        std::shared_ptr<GraphicsPipeline> m_AlphaPipeline; // alpha blend
        // The shaders' binding layouts, kept between frames (re-derived on a swapped or reloaded shader).
        mutable ShaderBindingLayoutCache m_SimLayout;
        mutable ShaderBindingLayoutCache m_CompactLayout;
        mutable ShaderBindingLayoutCache m_ArgsLayout;
        mutable ShaderBindingLayoutCache m_AddLayout;
        mutable ShaderBindingLayoutCache m_AlphaLayout;

        // The billboard material is PER EMITTER and PER VIEW (its camera block per draw), never shared.
        std::unordered_map<uint32_t, std::unique_ptr<MaterialParticleBillboard>> m_Materials;
        const ParticleWorldGpu*                                                  m_World     = nullptr;
        bool                                                                     m_Simulates = false;
        std::vector<ViewEmitter>                                                 m_ViewEmitters;
        ViewPool                                                                 m_Pool;
    };
} // namespace Desert::Graphic::System
