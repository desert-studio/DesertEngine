#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Systems/Scene/ShadowCaster.hpp>

#include "ParticleGpuLayout.hpp"
#include "ParticleWorldGpu.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Core
{
    class Scene;
    class Frustum;
}

namespace Desert::Graphic::RDG
{
    class Builder;
    class PassContext;
    class PassBindings;
} // namespace Desert::Graphic::RDG

namespace Desert::Graphic
{
    class DataDrivenMaterial;
}

namespace Desert::Graphic::System
{
    class MeshRenderer;

    // One VIEW's side of the scene's GPU particle system (VFX-07b). The pool, the per-emitter step tables,
    // counters and dispatch arguments are the scene's (ParticleWorldGpu, owned by its VFXWorld); this system holds
    // what is per view: one draw state per material CELL (its pipeline, its cell material with the camera block
    // and the Materials rows), each renderer row's runtime material instance, the simulation pipelines and this
    // view's graph imports. Per frame: PrepareFrame asks the scene's state to prepare the VFXWorld tick - the
    // first view of the tick simulates (ClaimsSimulation), every other view of it only draws. The simulating
    // view's frame build adds "Particles: Compact 0", then per fixed step "Particles: Dispatch Args {s}",
    // "Particles: Spawn+Update {s}" and "Particles: Compact {s+1}"; Spawn+Update and the later compacts are
    // dispatched INDIRECT from the arguments Dispatch Args wrote from the GPU counts (UE: indirect dispatch from
    // the GPU instance counts). Every view's ParticlePass (DrawPass, translucency) draws every enabled renderer
    // row of every emitter (UE: one FNiagaraRenderer per renderer properties) through its material's cell for the
    // row's kind - ParticleSprite / ParticleMesh / ParticleRibbon.Forward - the blend from the cell's template
    // (ApplySurfaceBlendMode), depth-TESTED against the scene depth it holds read-only (and samples for the depth
    // fade where the cell does): a sprite or ribbon row six vertices per alive particle INDIRECT from the last
    // compact's slot, a mesh row one instance of its static mesh per alive particle INDIRECT from the commands
    // AddMeshDrawArgsPasses wrote.
    //
    // THE OPAQUE MESH PARTICLE IS NOT A TRANSLUCENT ONE (UE: an opaque / masked Niagara mesh renderer is drawn by
    // the base pass and the shadow depth passes; only translucent particles are drawn by the translucency). A Mesh
    // row whose material is opaque or masked is never a ParticlePass draw. On the deferred render path it is drawn
    // through its template's ParticleMesh.GBuffer cell by GBufferPass - the G-buffer's depth, normal, material and
    // velocity, lit by the deferred lighting like any mesh.
    // On the forward render path it is drawn through its LIT ParticleMesh.Forward cell by BasePass - depth,
    // velocity, the scene's lights, cascades, sky and cloud shadow, as a forward static mesh. On both it casts
    // through its ParticleMesh.ShadowDepth cell into the sun's cascades and the point / spot lights' views, from
    // inside the mesh renderer's shadow passes (IShadowCaster).
    class ParticleRenderer final : public RenderSystem, public IShadowCaster, public IVirtualShadowCaster
    {
    public:
        using RenderSystem::RenderSystem;
        ~ParticleRenderer() override; // defined in the .cpp so unique_ptr<SpriteDraw> sees DataDrivenMaterial

        Common::BoolResultStr Initialize() override;

        // The sprite draw over the lit scene target (LOAD, depth read-only), added by
        // SceneRenderer::AddFrameTranslucency after the fog apply and the cloud composite - by EVERY view,
        // simulating or not. Empty (no TargetFramebuffer) without a target.
        [[nodiscard]] SystemRasterPass DrawPass();

        // The opaque / masked mesh rows into the deferred G-buffer (LOAD: after the meshes' and the terrain's
        // G-buffer nodes, before the depth resolve), depth-WRITING, through each row's ParticleMesh.GBuffer cell.
        // Added by SceneRenderer::AddFrameParticleGBuffer on the deferred path. Empty without a G-buffer.
        [[nodiscard]] SystemRasterPass GBufferPass();

        // The opaque / masked mesh rows into the scene target on the FORWARD render path (LOAD: after the meshes'
        // and the terrain's base pass nodes), depth-WRITING, through each row's lit ParticleMesh.Forward cell with
        // the view's scene inputs (SceneViewInputs) and the mesh renderer's frame state (lights, cascades,
        // environment: MeshRenderer::CaptureFrameState). Added by SceneRenderer::AddFrameBasePass. Empty without a
        // target.
        [[nodiscard]] SystemRasterPass BasePass();

        // The view's mesh renderer: the frame state (lights, cascades, environment) BasePass lights the forward
        // rows with. Set once by the SceneRenderer with
        // SetCascadeTarget.
        void SetMeshRenderer( std::weak_ptr<MeshRenderer> meshRenderer )
        {
            m_MeshRenderer = std::move( meshRenderer );
        }

        // The target the cascade casters' pipelines are built against (MeshRenderer::GetCascadeFramebuffer: every
        // cascade has its formats). Set once by the SceneRenderer before it registers this system as a caster.
        void SetCascadeTarget( std::shared_ptr<Framebuffer> cascadeFramebuffer )
        {
            m_CascadeFramebuffer = std::move( cascadeFramebuffer );
        }

        // IShadowCaster: the opaque / masked rows of every kind (CastsShadow) whose emitter's bounds the view's
        // frustum holds (MeshRenderer::GetShadowCasterViewProj, the mesh casters' cull), into shadow view @p
        // cascade - a sun cascade or a point / spot light's view of the local atlas - through each row's
        // <Path>.ShadowDepth cell (one cell material per view: its camera block is the view's light).
        [[nodiscard]] uint32_t DeclareShadowCascade( RenderPassDeclaration& declared, uint32_t cascade ) override;
        [[nodiscard]] Common::BoolResultStr RecordShadowCascade( const RDG::PassContext& context, uint32_t cascade,
                                                                 uint32_t                 firstBlock,
                                                                 const glm::mat4&         cascadeViewProj,
                                                                 const CascadeCasterBias& bias ) override;
        // A light casts the opaque rows of the emitters whose bounds (ParticleFrameEmitter::WorldBounds) its
        // sphere reaches; their particles move every frame (Moving). A light no such emitter reaches is not
        // redrawn for them.
        [[nodiscard]] LocalShadowReach LocalShadowReachOf( const glm::vec3& centre, float radius ) const override;

        // THE SUN'S VIRTUAL SHADOW MAP (UE: a Niagara opaque renderer casts into the VSM through the non-Nanite
        // page raster). Every opaque / masked row with a <Path>.VirtualShadowDepth cell draws into each clipmap
        // level whose casters' frustum holds its emitter's fixed bounds, from the state the PreOpaque nodes read
        // (DrawHalf( PreOpaque ): a PostOpaque emitter's previous tick), with its own GPU-written draw arguments.
        // The particles move with every step, so an emitter's box is a new entry each tick it stepped (its
        // cached pages are drawn again, as UE invalidates a movable primitive's pages).
        void AppendVirtualShadowEntries( std::vector<ShadowCasterEntry>& entries ) const override;
        // No records: a particle draw's counts are written on the GPU (the compact's Counters slot, the mesh
        // rows' MeshDrawArgs), which a CPU record of "VSM: Cull Casters" cannot carry. The level's frustum test
        // of the emitter's bounds (DeclareVirtualShadow / RecordVirtualShadow) culls instead, and only the pages
        // the level draws this frame take the depth (Common/VirtualShadowRaster.glslh).
        void AppendVirtualShadowDraws( std::vector<VirtualShadowCasterDraw>& draws ) const override;
        [[nodiscard]] uint32_t DeclareVirtualShadow( RDG::PassBuilder& pass, uint32_t level,
                                                     const VirtualShadowBlockParameters& parameters ) override;
        [[nodiscard]] Common::BoolResultStr
        RecordVirtualShadow( const RDG::PassContext& context, uint32_t level, uint32_t firstBlock,
                             const glm::mat4& levelViewProj, RDG::BufferRef arguments, uint32_t firstRecord ) override;

        // Drops this view's emitter instances and imports (IRenderSystem::OnSceneReplaced, kind 2). m_Materials is
        // keyed by the raw entt entity value, which a fresh registry hands out from zero again. The GPU particle
        // state itself is the scene's and goes with it.
        void OnSceneReplaced() override;

        // Prepares the scene's VFXWorld tick (ParticleWorldGpu::PrepareTick) and resolves each emitter's material
        // to its sprite draw: the emitter's ParticleSprite cell, or - for no material, a material without a
        // ParticleSprite cell, or one whose cell cannot draw - the ParticleSpriteDefault cell, the refusal logged
        // ONCE per (emitter, material) naming both. Call once per frame in BeginScene. The simulation's time is
        // the VFXWorld's fixed step; this system reads no clock and no frame timestep.
        void PrepareFrame( const ::Desert::Core::Scene& scene );

        // Whether this view simulates the scene's particles this frame: it was the first view to prepare the
        // VFXWorld tick. SceneRenderer::AddFrameParticlesSimulate adds the simulation nodes only then.
        [[nodiscard]] bool ClaimsSimulation() const
        {
            return m_Simulates;
        }

        // How many fixed steps this frame simulates at @p stage: the most any declared emitter of that tick stage
        // runs (0 in a view that does not simulate). Every node below takes the stage: SceneRenderer adds the
        // chain twice, before the opaque passes (PreOpaque) and after them (PostOpaque, reading this frame's
        // depth), each dispatching only its stage's emitters (UE: the GPU compute tick stages).
        [[nodiscard]] uint32_t SimulationStepCount( VFX::VFXTickStage stage ) const;

        // Imports the scene's pool (particles, free list, alive list) and every emitter's Counters into @p graph,
        // and in the simulating view its step table and dispatch arguments too (Renderer::ImportBuffer +
        // RegisterExternal); an emitter whose buffers cannot be imported is logged and sits the frame out, and
        // with no pool no emitter is declared. With no declared emitter the particle nodes declare nothing and the
        // graph culls them. Call once per frame graph, after PrepareFrame, in every view.
        // VFX-PARAMS: the simulating view also imports each emitter's event table and its system's event buffer
        // (once per system), and writes its CollisionView - the camera @p sceneDepth was drawn with; @p
        // hasSceneDepth false = @p sceneDepth is the fallback image and nothing collides (CollisionView.Camera.w =
        // 0).
        void ImportFrameBuffers( RDG::Builder& graph, RDG::TextureRef sceneDepth, bool hasSceneDepth );

        // Compact @p compact (0..SimulationStepCount()): one ParticleCompact dispatch per declared emitter that
        // runs it, rebuilding its free list, the alive half (compact & 1) and its Counters slot (compact & 1) -
        // the indirect draw arguments. Compact 0 scans the whole range (and kills it first when it restarts);
        // a later compact scans only what the step touched and is dispatched INDIRECT from the step's Dispatch
        // Args (kParticleCompactArgsOffset). Block n is the n-th such emitter (DeclareCompactBindings).
        [[nodiscard]] Common::BoolResultStr Compact( const RDG::PassContext& context, uint32_t compact,
                                                     VFX::VFXTickStage stage );
        void DeclareCompactBindings( RDG::PassBuilder& pass, uint32_t compact, VFX::VFXTickStage stage ) const;

        // Dispatch Args of step @p step: one single-thread ParticleDispatchArgs dispatch per emitter running the
        // step, writing from compact @p step's alive / free counts and the step's spawn budget the group counts of
        // Spawn+Update @p step and of compact @p step + 1, and opening compact @p step + 1's Counters slot.
        [[nodiscard]] Common::BoolResultStr BuildDispatchArgs( const RDG::PassContext& context, uint32_t step,
                                                               VFX::VFXTickStage stage );
        void DeclareDispatchArgsBindings( RDG::PassBuilder& pass, uint32_t step, VFX::VFXTickStage stage ) const;

        // Spawn+Update of step @p step: one ParticleSimulate dispatch per declared emitter that runs the step,
        // INDIRECT from the step's Dispatch Args (kParticleSimulateArgsOffset); block n = the n-th such emitter.
        [[nodiscard]] Common::BoolResultStr Simulate( const RDG::PassContext& context, uint32_t step,
                                                      VFX::VFXTickStage stage );
        void DeclareSimulateBindings( RDG::PassBuilder& pass, uint32_t step, VFX::VFXTickStage stage ) const;

        // VFX-08: the sorts the draws recorded at @p at read (ParticleSortGraph.hpp ParticleSortKind), one Compute
        // node per sort stage. PreOpaque (after the PreOpaque simulation, before the G-buffer): the ribbon order of
        // every emitter with an opaque / masked Ribbon row - the G-buffer, the base pass and every shadow view
        // draw it, from the half DrawHalf( PreOpaque ) names. PostOpaque (after the PostOpaque simulation, before
        // ParticlePass): the back-to-front order of every emitter with a translucent / additive row and the ribbon
        // order of its blended Ribbon rows, of this tick's half. Every view adds its own (the key is the view
        // depth), whether or not it claimed the simulation. Call after ImportFrameBuffers, once per stage.
        void AddSortPasses( RDG::Builder& graph, VFX::VFXTickStage at );

        // VFX-RENDER: the indexed-indirect commands of this view's Mesh renderer rows (UE
        // FNiagaraGPUInstanceCountManager::UpdateDrawIndirectBuffers): one Compute node per mesh row, after the
        // last Compact and before ParticlePass, writing one command per mesh section (instances = the emitter's
        // alive count) into the view's transient MeshDrawArgs. Call after ImportFrameBuffers, once per stage:
        // PreOpaque writes the commands of every mesh row drawn before the PostOpaque simulation (a PostOpaque
        // emitter's from its previous half), PostOpaque those of a PostOpaque emitter's blended rows (DrawSource).
        void AddMeshDrawArgsPasses( RDG::Builder& graph, VFX::VFXTickStage stage );

    private:
        // One particle cell's draw state in this view, shared by every renderer row that draws through the cell
        // (the cell's vertex path is its renderer kind's): the cell's own DataDrivenMaterial (its descriptor sets
        // from the cell's reflection; the camera block and the Materials rows are written once per frame, before
        // the blocks are declared), the pipeline the cell's template blend built, and the layout cache.
        struct CellDraw
        {
            std::string                         Shader; // "<Template>/<Cell>"
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            std::shared_ptr<DataDrivenMaterial> Material;
            MaterialInstancePtr                 Instance; // the cell's defaults: the row of a draw without one
            Core::Formats::SurfaceBlendMode     Blend = Core::Formats::SurfaceBlendMode::Opaque;
            mutable ShaderBindingLayoutCache    Layout;
            std::vector<glm::vec4>              Rows; // this frame's Materials rows, one per draw through the cell
        };

        // Which node a cell draw state draws in: the pipeline state (target, depth write, cull) and the per-view
        // blocks are the node's, so one cell shader has one draw state per node (and per cascade).
        enum class CellNode : uint8_t
        {
            Particles, // ParticlePass: the scene target, depth read-only
            GBuffer,   // GBufferPass: the G-buffer, depth written
            Shadow,    // a shadow view - a sun cascade or a local light's (standard-Z), depth written
            Base,      // BasePass: the scene target on the forward path, lit, depth written
            // a "VSM: Raster" level: no attachment, the fragment atomic-maxes into the physical pool
            VirtualShadow,
        };

        // One renderer row's material in this view: the asset its instance was built from, the runtime instance of
        // that asset's cell for the row's kind (null = the cell's defaults), and the asset last refused (the
        // refusal is said once per (row, material)).
        struct DrawMaterial
        {
            Common::AssetHandle Handle;
            MaterialInstancePtr Instance;
            Common::AssetHandle Refused;
            bool                HasRefused = false;
        };

        // This view's imports of the scene's pool.
        struct ViewPool
        {
            RDG::ExternalBuffer SlotsImport;
            RDG::ExternalBuffer FreeImport;
            RDG::ExternalBuffer AliveImport;
            RDG::BufferRef      SlotsRef;
            RDG::BufferRef      FreeRef;
            RDG::BufferRef      AliveRef;
            bool                Declared = false;
        };

        // One emitter of the scene's tick as this view sees it: its simulation buffers (built by PrepareFrame,
        // consumed by the simulation nodes, the sorts and the draws). What draws it is its ViewDraws.
        struct ViewEmitter
        {
            const ParticleFrameEmitter* Frame = nullptr;
            RDG::ExternalBuffer         StepsImport;
            RDG::ExternalBuffer         CountersImport;
            RDG::ExternalBuffer         ArgsImport;
            RDG::ExternalBuffer         ChannelImport; // Spawn from Channel particles, read by Spawn+Update
            RDG::BufferRef              ChannelRef;
            // The emitter's stack: attribute columns (also read by the sorts and the draws), parameters, curves.
            RDG::ExternalBuffer         FloatsImport;
            RDG::ExternalBuffer         IntsImport; // also read by the ribbon order and the ribbon draw (RibbonID)
            RDG::ExternalBuffer         ParamsImport;
            RDG::ExternalBuffer         CurvesImport;
            // Where the sorts and the draws read the drawn attributes, per ParticleDrawTick (DrawTickOf).
            std::array<RDG::ExternalBuffer, kParticleDrawTickCount> DrawLayoutImports;
            std::array<RDG::BufferRef, kParticleDrawTickCount>      DrawLayoutRefs;
            RDG::BufferRef              FloatsRef;
            RDG::BufferRef              IntsRef;
            RDG::BufferRef              ParamsRef;
            RDG::BufferRef              CurvesRef;
            RDG::BufferRef              StepsRef;
            RDG::BufferRef              CountersRef; // read by the sprite / ribbon draws as IndirectArgs
            RDG::BufferRef              ArgsRef;     // written by Dispatch Args, read as IndirectArgs
            // VFX-PARAMS: the emitter's event table and its system's event buffer (m_Events' ref).
            RDG::ExternalBuffer         EventTableImport;
            RDG::BufferRef              EventTableRef;
            RDG::BufferRef              EventsRef;
            bool                        Declared = false;
            // Its bounds are in this view's frustum (PrepareFrame): its rows are drawn by the view's own passes.
            // The light views cull it by their own frusta (shadows) and the simulation runs either way.
            bool InView       = true;
            bool ViewSorted   = false; // its blended draws read m_SortedRef, not AliveList
            // Per draw stage (VFXTickStage): its ribbon draws recorded there read m_RibbonOrderRefs[stage].
            std::array<bool, 2> RibbonSorted{};
        };

        // One LOD 0 section of a mesh row's static mesh (Renderer::RenderMesh's submesh walk): its index range and
        // the section's own transform, pushed before its draw.
        struct MeshSection
        {
            uint32_t  IndexCount   = 0;
            uint32_t  FirstIndex   = 0;
            uint32_t  VertexOffset = 0;
            glm::mat4 Transform{ 1.0f };
        };

        // One enabled renderer row of one emitter as this view draws it (UE FNiagaraRenderer): every row is its
        // own draw of ParticlePass, through its material's cell for the row's kind.
        struct ViewDraw
        {
            uint32_t                   Emitter       = 0; // index in m_ViewEmitters
            uint32_t                   RendererIndex = 0; // index in the emitter's ParticleEmitterGpu::Renderers
            const ParticleRendererGpu* Renderer      = nullptr;
            CellDraw*                  Cell          = nullptr;
            MaterialInstance*          Instance      = nullptr; // the row's own (null = the cell's defaults)
            uint32_t                   Row           = 0;       // the draw's row in Cell->Rows this frame
            // Kind == Mesh: the resident static mesh (null = still streaming: not drawn this frame), its sections,
            // and its first command in m_MeshArgsRefs[stage] per stage.
            const ::Desert::Mesh*    StaticMesh = nullptr;
            std::vector<MeshSection> Sections;
            std::array<uint32_t, 2>  ArgsBase{};
            // A row of any kind with an opaque / masked material (Opaque: never a ParticlePass draw; the
            // template's shading model lights it as any mesh): on the deferred path its G-buffer cell
            // (GBufferPass), on the forward path its lit forward cell (BasePass), on
            // both its shadow cell shader (the shadow views), and its rows there.
            bool                  Opaque      = false;
            CellDraw*             GBufferCell = nullptr;
            CellDraw*             BaseCell    = nullptr;
            std::string           ShadowShader;
            uint32_t              GBufferRow = 0;
            uint32_t              BaseRow    = 0;
            std::vector<uint32_t> ShadowRows; // per shadow view
            // Its <Path>.VirtualShadowDepth cell shader (empty: the template has none - the row casts into the
            // cascades and the local lights, not the virtual shadow map, said once), and its rows there per level.
            std::string           VirtualShadowShader;
            std::vector<uint32_t> VirtualShadowRows;
            // The row's settings (ParticleRendererParamsGpu), RendererParams in every particle vertex header.
            RDG::ExternalBuffer RendererParamsImport;
            RDG::BufferRef      RendererParamsRef;
            bool                Declared = false;
        };

        bool CreatePipelines();
        // Whether the row can draw this frame (its emitter declared, a cell, a resident mesh for a Mesh row), in
        // any view: what the sorts and the mesh arguments are planned from.
        bool IsDrawable( const ViewDraw& draw ) const;
        // Whether the row's draw arguments exist this frame: a mesh row's commands (m_MeshArgsRefs), a ribbon row's
        // order (RibbonSorted); a sprite draws from its Counters slot.
        bool HasDrawArgs( const ViewDraw& draw, VFX::VFXTickStage at ) const;
        // WHICH SIMULATION STATE a draw recorded at @p at reads: a PostOpaque emitter drawn before its simulation
        // (every node but ParticlePass: the G-buffer, the base pass, the shadow views) reads the previous tick's
        // half (PreOpaque state); everything else this frame's. DrawHalf is that half; HasDrawData is false for a
        // PostOpaque emitter with no previous tick, whose pre-simulation draws record nothing.
        [[nodiscard]] VFX::VFXTickStage DrawSource( const ViewEmitter& ve, VFX::VFXTickStage at ) const;
        [[nodiscard]] uint32_t          DrawHalf( const ViewEmitter& ve, VFX::VFXTickStage at ) const;
        // The tick whose layout (half, counter slot, emitter positions) a draw recorded at @p at binds: Previous
        // exactly where DrawHalf is the previous half.
        [[nodiscard]] ParticleDrawTick DrawTickOf( const ViewEmitter& ve, VFX::VFXTickStage at ) const;
        [[nodiscard]] bool              HasDrawData( const ViewEmitter& ve, VFX::VFXTickStage at ) const;
        // Whether an opaque / masked row will cast this frame, asked before the graph imports anything
        // (LocalShadowReachOf): a shadow cell, a cell, and for a mesh row a resident mesh.
        bool CanCast( const ViewDraw& draw ) const;
        // Whether the row draws into the virtual shadow map's levels this frame (a VirtualShadowDepth cell, drawable,
        // its PreOpaque arguments), and the level's casters' frustum (MeshRenderer's view kVsmViewBase + level).
        bool CastsVirtualShadow( const ViewDraw& draw ) const;
        [[nodiscard]] Core::Frustum VirtualShadowFrustum( uint32_t level ) const;
        // Whether a light view whose casters draw with @p frustum holds the row's emitter (its WorldBounds).
        bool InFrustum( const ViewDraw& draw, const Core::Frustum& frustum ) const;
        // Whether ParticlePass draws @p draw this frame - the one condition its Declare and its exec both walk the
        // draws by, so the exec's n-th drawn row opens the n-th declared block. A ribbon also needs its order.
        bool IsDrawn( const ViewDraw& draw ) const;
        // Whether GBufferPass draws @p draw this frame (the deferred opaque / masked mesh rows).
        bool IsDrawnOpaque( const ViewDraw& draw ) const;
        // Whether BasePass draws @p draw this frame (the forward opaque / masked mesh rows).
        bool IsDrawnBase( const ViewDraw& draw ) const;
        // Whether the shadow views can draw @p draw this frame: an opaque / masked row with a shadow cell, on
        // either render path (each view then culls it by its frustum, InFrustum).
        bool CastsShadow( const ViewDraw& draw ) const;
        // The opaque cells of a row (any kind) drawing through a <Path>.Forward cell whose template is opaque or
        // masked: its template's <Path>.ShadowDepth, and <Path>.GBuffer on the deferred path or the lit Forward
        // cell itself on the forward one; none for a translucent cell. An opaque template without them is said
        // once by name and its rows are not drawn (its Forward cell is lit: the particle pass cannot draw it).
        void ResolveOpaqueCells( ViewDraw& draw );
        // The particle storage block every particle cell's vertex header reads (AttributeFloats, DrawLayout, the
        // list ListFor names, the row's settings; a ribbon's int columns and Counters), declared on @p declared
        // against
        // @p cell's layout and route fill, with the row's draw arguments read as IndirectArgs; the caller adds
        // what only its node binds. On a RenderPassDeclaration (a system pass) or an RDG::PassBuilder (a raster
        // the SceneRenderer adds); defined and used in the .cpp only.
        template <typename Declaration>
        auto DeclareDrawBlock( Declaration& declared, const ViewDraw& draw, const CellDraw& cell,
                               VFX::VFXTickStage at ) const;
        // The particle list @p draw reads through @p cell: the view's RibbonOrder for a ribbon, the view's
        // SortedAlive for a blended row of a sorted emitter, the alive list otherwise.
        RDG::BufferRef ListFor( const ViewDraw& draw, const CellDraw& cell, VFX::VFXTickStage at ) const;
        // The row's draws through @p cell, bound by @p bindings, in any node: a mesh row's indexed-indirect draws,
        // one per section; a sprite / ribbon row's six vertices per particle from its Counters slot, faced to the
        // view's camera (pushed: the light views face them as the camera sees them, UE's dependent view).
        Common::BoolResultStr RecordDraw( const RDG::PassBindings& bindings, const ViewDraw& draw,
                                          const CellDraw& cell, VFX::VFXTickStage at ) const;
        // A mesh row's indexed-indirect draws through @p cell, one per section, bound by @p bindings.
        Common::BoolResultStr RecordMeshDraws( const RDG::PassBindings& bindings, const ViewDraw& draw,
                                               const CellDraw& cell, VFX::VFXTickStage at ) const;
        // Whether step @p step's Dispatch Args and Spawn+Update nodes dispatch @p ve.
        bool RunsStep( const ViewEmitter& ve, uint32_t step, VFX::VFXTickStage stage ) const;
        // Whether compact @p compact's node dispatches @p ve (compacts 0..StepCount of a declared emitter).
        bool RunsCompact( const ViewEmitter& ve, uint32_t compact, VFX::VFXTickStage stage ) const;
        // The draw state of @p cellShader (a cell of vertex path @p path) in this view, built on first use (null =
        // refused, said once by name).
        CellDraw* CellDrawFor( const std::string& cellShader, MeshVertexPath path,
                               CellNode node = CellNode::Particles, uint32_t cascade = 0 );
        // The cell renderer row @p renderer of emitter @p entityId draws with this frame (its material's cell for
        // the row's kind, or the kind's default cell with the refusal logged once), its instance kept in @p
        // material.
        CellDraw* ResolveCell( uint32_t entityId, const ParticleRendererGpu& renderer, DrawMaterial& material );

        std::shared_ptr<ComputePipeline> m_CompactPipeline;
        std::shared_ptr<ComputePipeline> m_ArgsPipeline;
        std::shared_ptr<ComputePipeline> m_SortPipeline;
        std::shared_ptr<ComputePipeline> m_MeshArgsPipeline;
        mutable ShaderBindingLayoutCache m_SortLayout;
        mutable ShaderBindingLayoutCache m_MeshArgsLayout;
        RDG::BufferRef                   m_SortKeysRef;
        RDG::BufferRef                   m_SortedRef;      // this view's SortedAlive, valid while any ViewSorted
        // Per draw stage, this view's RibbonOrder (of the half the draws recorded there read), valid while any
        // RibbonSorted[stage].
        std::array<RDG::BufferRef, 2> m_RibbonOrderRefs;
        // Per tick stage, this view's mesh draw commands (AddMeshDrawArgsPasses).
        std::array<RDG::BufferRef, 2>    m_MeshArgsRefs;
        // The shaders' binding layouts, kept between frames (re-derived on a swapped or reloaded shader).
        // Per stack host program (ParticleWorldGpu's, alive with the scene's GPU state).
        mutable std::unordered_map<const ComputePipeline*, ShaderBindingLayoutCache> m_SimLayouts;
        mutable ShaderBindingLayoutCache                                             m_CompactLayout;
        mutable ShaderBindingLayoutCache                                             m_ArgsLayout;

        // The default templates' cells, per renderer kind (VFXRendererKind order: ParticleSpriteDefault's
        // ParticleSprite.Forward, ParticleRibbonDefault's ParticleRibbon.Forward, ParticleMeshDefault's
        // ParticleMesh.Forward; Initialize refuses without any of them).
        std::array<std::string, 3> m_DefaultCells;
        // Per (cell shader, node, cascade), per view (a draw's camera block is the view's or the cascade light's).
        // Null = that cell was refused.
        std::unordered_map<std::string, std::unique_ptr<CellDraw>> m_Cells;
        // The opaque particle-mesh templates that have no G-buffer / shadow cell (said once each).
        std::unordered_map<std::string, bool> m_RefusedOpaque;
        // The mesh rows' meshes that will never be drawn (failed or unknown to MeshService), said once each.
        std::unordered_map<uint64_t, bool> m_MissingMeshes;
        std::shared_ptr<Framebuffer>       m_CascadeFramebuffer;
        std::weak_ptr<MeshRenderer>        m_MeshRenderer;
        // Per emitter entity (raw entt entity value), per (emitter index << 32 | renderer row), per view: every
        // emitter of a system and every row of an emitter keeps its own instance.
        std::unordered_map<uint32_t, std::unordered_map<uint64_t, DrawMaterial>> m_Materials;
        const ParticleWorldGpu*                                                  m_World     = nullptr;
        bool                                                                     m_Simulates = false;
        std::vector<ViewEmitter>                                                 m_ViewEmitters;
        std::vector<ViewDraw>                                                    m_ViewDraws;
        ViewPool                                                                 m_Pool;
        // VFX-PARAMS: each placed system's event buffer imported ONCE (its emitters share it), by entity id; and
        // the simulation's collision inputs - the view's CollisionView block and this frame's scene depth.
        struct ViewEvents
        {
            RDG::ExternalBuffer Import;
            RDG::BufferRef      Ref;
        };
        std::unordered_map<uint32_t, ViewEvents>        m_Events;
        std::shared_ptr<ShaderResources::StorageBuffer> m_CollisionView;
        RDG::ExternalBuffer                             m_CollisionViewImport;
        RDG::BufferRef                                  m_CollisionViewRef;
        RDG::TextureRef                                 m_SceneDepthRef;
    };
} // namespace Desert::Graphic::System
