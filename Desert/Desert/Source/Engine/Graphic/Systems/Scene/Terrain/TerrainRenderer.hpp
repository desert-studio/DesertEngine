#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Graphic/Systems/Scene/ShadowCaster.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainLayerInputs.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    struct ViewFrame;
    class FrameTextures;
}

namespace Desert::Graphic::System
{
    // One landscape tile's render parameters, submitted per frame from the ECS (LandscapeECSSystem ->
    // DrawLandscapeTileCommand -> SceneRenderer::SubmitLandscapeTile). Graphic-side, no ECS dependency.
    struct TerrainDrawData
    {
        // The tile's R16 heightmap (LandscapeECSSystem's GPU copy of the tile's samples), and where the tile
        // sits in its root's frame.
        std::shared_ptr<Image2D> Heightmap;
        LandscapeTileDraw Landscape;

        // The tile's ground and painted weight layers (LayerCount 0 = none painted).
        LandscapeWeightDraw Weights;

        // The root's material, as param + texture overrides applied generically to the DataDrivenMaterial by
        // name (e.g. "Tint"). Unset parameters keep the shader's schema defaults. Never null: one object per
        // material revision, shared by every tile of the root (LandscapeECSSystem::m_RootOverrides), so its
        // pointer is the revision the tile proxies (TileProxy) compare.
        std::shared_ptr<const Graphic::MaterialOverrides> Overrides;
    };

    // GPU landscape renderer. Draws each tile as a triangle grid over its samples into the scene framebuffer's
    // Geometry phase (vertexless draw at the tile's LOD grid; the vertex stage morphs and displaces each vertex
    // from the tile's heightmap — UE's landscape vertex factory, ported, see LandscapeLod.glslh).
    // Driven by the ECS: every drawn LandscapeTileComponent is submitted as a TerrainDrawData each frame.
    // Draws every terrain of the frame, in whichever pass the render path shades opaque geometry with, and
    // into the sun's cascades. One frame's data — materials, param rows, TerrainInstances — is resolved ONCE
    // by PrepareFrame, and every pass that follows draws from it:
    //   - Forward:  the "TerrainPass" graph pass (Terrain.shader, lit by itself);
    //   - Deferred: RenderGBufferManual, after the meshes' G-buffer fill (TerrainGBuffer.shader, lit by the
    //     deferred composite like every other opaque surface — so it receives the cascaded shadows);
    //   - both:     RecordShadowView, from inside the mesh renderer's shadow passes — the sun's cascades and the
    //     point/spot lights' views of the local shadow atlas (TerrainShadow.shader).
    // The three programs share their vertex stage (Programs/Terrain/*.glslh) and differ in what the
    // fragment writes and in the matrix pushed per draw (camera or cascade).
    class TerrainRenderer final : public RenderSystem, public IShadowCaster
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // The forward terrain program's DESERT_LOCAL_SHADOW_VSM permutation (Shadows.Method VirtualMaps on the
        // Forward path), ShippedByCook - the one statement EnsureForwardVsmPipeline acquires and the cook lists.
        [[nodiscard]] static Core::ShaderVariant ForwardVsmVariant();
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants()
        {
            return { { "Terrain", ForwardVsmVariant() } };
        }

        // The terrain draw on the scene target, after the meshes in SceneRenderer::AddFrameBasePass (LOAD: depth
        // shared, so terrain and meshes resolve against each other). Empty (no TargetFramebuffer) without a target
        // or pipeline.
        [[nodiscard]] SystemRasterPass GeometryPass();

        // The caster pipeline is built against the cascade targets, which the mesh renderer owns and makes.
        // Under a zero-cascade budget there are none and none is built (RecordShadowView is never reached). The
        // local shadow atlas has the cascades' depth format, so the one pipeline serves its views too.
        Common::BoolResultStr CreateShadowPipeline( const std::shared_ptr<Framebuffer>& cascadeFramebuffer );

        // Before the render graph records, inside the view's frame (after SceneViewState::BeginFrame accepted
        // it): resolves every queued terrain's material, packs the rows and uploads them, with @p frame's
        // matrices — the same view every other pass of the frame draws for. The instance rows go up in
        // PrepareLocalShadowViews and the scene's blocks in ApplySceneFrame, which must follow it: nothing may
        // draw a terrain before all three ran.
        void PrepareFrame( const ViewFrame& frame );

        // After PrepareFrame AND MeshRenderer::UpdateLocalShadows (the lights get their views there, later in the
        // frame than PrepareFrame, whose rows the TLAS and the motion rows read first): for each of @p localLights
        // (MeshRenderer::GetLocalShadowCasterLights) the tiles in its reach at the LOD its own position asks for,
        // with their cache key (LocalShadowSignature); for each sun cascade view v (index of
        // @p cascadeWorldPerTexel, MeshRenderer::GetCascadeWorldPerTexel; 0 = no cascade) the camera's tiles diced
        // to the cascade's own texels; then the instance rows of every group are uploaded.
        void PrepareLocalShadowViews( const std::vector<LocalShadowCasterLight>& localLights,
                                      const std::vector<float>&                  cascadeWorldPerTexel );

        // After PrepareLocalShadowViews and SceneRenderer::AssignLocalShadowMaskSlots (every point/spot light has
        // its final ShadowView / ShadowMaskSlot): @p scene's lights, shadow block and cloud shadow onto the
        // forward program of every group of the frame (MeshRenderer::CaptureFrameState - the snapshot the meshes'
        // forward materials take). Forward path only; the deferred composite lights the G-buffer terrain.
        void ApplySceneFrame( const SceneFrameBinding& scene );

        // After PrepareFrame, before any node that draws the terrain is declared: "Landscape: Dice Pool"
        // (LandscapeDicePool.shader), the displaced sub-vertices of every tile the camera dices, evaluated ONCE
        // into the frame's dice pool (UE: Nanite displaces a vertex once, its triangles index it) instead of once
        // per triangle corner in every view; every terrain draw of the frame binds the pool. With no diced tile
        // the pool is one zero vertex, still bound (the vertex stage declares it).
        [[nodiscard]] Common::BoolResultStr AddDicePoolPass( RDG::Builder& graph, FrameTextures& textures );

        // Deferred path: draws the frame's terrain into the G-buffer, in a LOAD pass after the meshes'. SETUP of
        // that node: one binding block per G-buffer material of the frame's groups (the G-buffer shader samples
        // no scene/view input); the exec records through them.
        void                                DeclareGBufferDraws( RDG::PassBuilder& pass );
        [[nodiscard]] Common::BoolResultStr RenderGBufferManual( const RDG::PassContext& context );

        // Forward path: the frame's terrain into the scene DEPTH alone, before the base pass (UE: the DepthPass;
        // SceneRenderer::AddFrameForwardDepthPrepass), through TerrainDepth.shader - the base pass's vertex stage,
        // the hole discard, no shading. SETUP: one binding block per Depth material of the frame's groups (no
        // scene/view input); the exec records through them. "TerrainPass" then tests CloserOrEqual, writes none.
        void                                DeclareDepthDraws( RDG::PassBuilder& pass );
        [[nodiscard]] Common::BoolResultStr RenderDepthManual( const RDG::PassContext& context );

        // THE WIREFRAME VIEW (DebugViewState::WireframeMode, as MeshRenderer::SetWireframe): the forward base pass
        // draws through the line-polygon twin of its pipeline, writing its own depth, and the depth prepass draws
        // no ground. In a player's build the argument is dropped: nothing there can set the flag
        // (ShippingPipelines).
        void SetWireframe( [[maybe_unused]] bool enabled )
        {
#if DESERT_DEV_INSTRUMENTS
            m_Wireframe = enabled;
#endif
        }
        [[nodiscard]] bool WireframeView() const
        {
#if DESERT_DEV_INSTRUMENTS
            return m_Wireframe;
#else
            return false;
#endif
        }

        [[nodiscard]] bool CastsShadowThisFrame() const override
        {
            return !m_Queue.empty();
        }
        // The terrain part of SceneRenderer::LocalShadowMapsRead / SunCascadesRead: this frame shades a queued
        // terrain with the Forward program (render path Forward; in Deferred the terrain is in the G-buffer and
        // the composite reads the screen masks) and that program samples @p textureName.
        [[nodiscard]] bool ForwardReadsTexture( std::string_view textureName ) const;
        // The tiles whose ground (XZ) footprint the light reaches, each by what its depth is made of: placement,
        // LOD (the light's, LocalShadowCasterLight — not the camera's, so a moving camera keeps the cache),
        // holes, and the revisions of its two maps' texels. Resolved by PrepareFrame for this frame's lights.
        [[nodiscard]] std::optional<uint64_t>
                               LocalShadowSignature( const LocalShadowCasterLight& light ) const override;
        // The tiles cascade view @p view draws (ShadowViewDraws: the camera's tiles and LOD at the cascade's
        // dicing), each by every input of its depth: its whole instance row and vertex count, its heightmap and
        // weightmap with their texels' revisions, and the frame's dice pool those rows read (every pooled row,
        // its maps and the layer heights array's device generation, so a streamed mip redraws). Resolved by
        // PrepareLocalShadowViews for this frame; the hash of nothing when no tile is queued.
        [[nodiscard]] std::optional<uint64_t> CascadeShadowSignature( uint32_t view ) const override;
        [[nodiscard]] std::optional<uint64_t>
        CascadeShadowContent( uint32_t view, const glm::mat4& casterViewProj ) const override;
        [[nodiscard]] uint32_t DeclareShadowView( RenderPassDeclaration& declared, uint32_t view ) override;
        [[nodiscard]] Common::BoolResultStr RecordShadowView( const RDG::PassContext& context, uint32_t view,
                                                              uint32_t firstBlock, const glm::mat4& casterViewProj,
                                                              const CascadeCasterBias&   bias,
                                                              std::span<const glm::mat4> region ) override;

        void AppendVirtualShadowEntries( std::vector<ShadowCasterEntry>& entries ) const override;
        [[nodiscard]] uint32_t DeclareVirtualShadow( RDG::PassBuilder&                   pass,
                                                     const VirtualShadowBlockParameters& parameters ) override;
        void AppendVirtualShadowDraws( std::vector<VirtualShadowCasterDraw>& draws ) const override;
        [[nodiscard]] Common::BoolResultStr
        RecordVirtualShadow( const RDG::PassContext& context, uint32_t firstBlock, const glm::mat4& levelViewProj,
                             RDG::BufferRef arguments, uint32_t firstRecord ) override;

        void Submit( const TerrainDrawData& data )
        {
            m_Queue.push_back( data );
        }

        // This frame's tiles and, parallel to them, the row PrepareFrame packed for each (LOD, borders, frame):
        // what the view's ray-tracing landscape (LandscapeRayTracing) builds its BLAS from.
        [[nodiscard]] std::span<const TerrainDrawData> GetQueue() const
        {
            return m_Queue;
        }
        [[nodiscard]] std::span<const TerrainInstance> GetFrameInstances() const
        {
            return m_FrameInstances;
        }
        // ...and the landscape material each tile drew with (its row after the defaults and the tile's overrides,
        // its layer inputs' maps): the landscape's ray tracing hit group shades a hit with the raster's own
        // ground.
        [[nodiscard]] std::span<const TerrainFrameMaterial> GetFrameMaterials() const
        {
            return m_FrameMaterials;
        }

        // THE WORLD WAS TORN DOWN (SceneRenderer::ReleaseWorldResources): the landscape materials and their layer
        // arrays belong to the old world's landscapes. Kept, a reopen of the same scene rebuilt every array from
        // its re-read textures while the old ones were still owed deletion (FIELD-MEM p3: 4 x 64.5 MB twice).
        void ReleaseWorldCaches()
        {
            ClearQueue();
            // The pooled group slots and the tile proxies name the sets and the layer inputs below: they go first.
            m_FrameGroupPool.clear();
            m_TileProxies.clear();
            m_Materials.clear();
            m_LayerInputs.clear();
        }

        void ClearQueue()
        {
            m_Queue.clear();
            m_FrameInstances.clear();
            m_FrameMaterials.clear();
            // The group slots go back to the pool with their vectors' capacity; the next PrepareFrame takes them.
            std::swap( m_FrameGroups, m_FrameGroupPool );
            m_FrameGroups.clear();
            m_FrameDraws.clear();
            m_ViewDraws.clear();
            m_LocalLightDraws.clear();
            m_CascadeDraws.clear();
        }

    private:
        // One texture set's materials, one per program. Textures are the materials' identity (see
        // TerrainTextureKey), so each program needs its own copy of the set.
        struct ProgramMaterials
        {
            std::unique_ptr<DataDrivenMaterial> Forward;
            // Forward's DESERT_LOCAL_SHADOW_VSM permutation (Shadows.Method VirtualMaps: the view's virtual shadow
            // map in place of the atlas), made on the first frame that draws with it.
            std::unique_ptr<DataDrivenMaterial> ForwardVsm;
            std::unique_ptr<DataDrivenMaterial> GBuffer;
            // TerrainDepth.shader: the forward view's depth prepass (the same vertex stage as Forward).
            std::unique_ptr<DataDrivenMaterial> Depth;
            std::unique_ptr<DataDrivenMaterial> Shadow;
            // TerrainVirtualShadow.shader: the virtual shadow map's caster (the same vertex stage as Shadow).
            std::unique_ptr<DataDrivenMaterial> VirtualShadow;

            // What the surface program's params were last written from (ApplyDefaults + the overrides' params):
            // written again only when another overrides object (a new material revision), another surface
            // program (render path, VSM permutation) or a recompiled program (its code generation) reaches the
            // set. Nothing else writes those params, so the row keeps them from frame to frame.
            std::shared_ptr<const MaterialOverrides> AppliedOverrides;
            const DataDrivenMaterial*                AppliedSurface    = nullptr;
            uint64_t                                 AppliedGeneration = 0u;
            // The PrepareFrame (m_PrepareSerial) the set's group opened in, and that group's index: a tile finds
            // its group with no key.
            uint64_t GroupFrame = 0u;
            size_t   GroupIndex = 0u;
        };

        // A frame's terrains sharing one texture set. m_Materials owns the set; the group points at it and at its
        // key (the map is node-based, so both stay put until ReleaseWorldCaches).
        struct FrameGroup
        {
            const std::string*           Key       = nullptr;
            ProgramMaterials*            Materials = nullptr;
            std::vector<glm::vec4>       ParamRows;
            std::vector<TerrainInstance> Instances;
            // The group's layer inputs (m_LayerInputs, keyed by the material's own textures, which the group key
            // contains): resolved once per frame by the tile that opened the group; every other tile of the group
            // and every light's row read them through this, with no TerrainTextureKey of their own.
            TerrainLayerInputs* Layers = nullptr;
        };

        // ONE TILE'S PERSISTENT PROXY (UE: FLandscapeComponentSceneProxy lives in the scene and is rebuilt only when
        // its component changes), by the tile's entity (m_TileProxies). It holds what PrepareFrame used to derive
        // from strings on every frame: the tile's texture set (TerrainTextureKey, spelled only on a rebuild), its
        // layer inputs and the override textures that are samplers of the surface program. Rebuilt when any
        // revision below moved; the frame only reads it. A tile whose heightmap died leaves (owner liveness).
        struct TileProxy
        {
            // The revision: the root's overrides object (one per material revision), the two maps' identity
            // (owner, so a freed and reallocated image at the same address is a new one) and their pixel
            // revisions, the render path and the forward VSM permutation (they pick the surface program).
            std::shared_ptr<const MaterialOverrides> Overrides;
            std::weak_ptr<Image2D>                   Heightmap;
            std::weak_ptr<Image2D>                   Weightmap;
            uint64_t                                 HeightRevision = 0u;
            uint64_t                                 WeightRevision = 0u;
            bool                                     Deferred       = false;
            bool                                     ForwardVirtual = false;

            const std::string*  Key       = nullptr; // m_Materials' key of the set
            ProgramMaterials*   Materials = nullptr;
            TerrainLayerInputs* Layers    = nullptr;
            std::string         LayerTag; // UpdateTerrainLayerInputs' name of the material in an error
            // Indices into Overrides->Textures of the samplers the surface program binds (the layer inputs' maps
            // and layer infos reach the shader through the arrays instead).
            std::vector<uint32_t> Samplers;
        };
        // @p tile's proxy, rebuilt first when one of its revisions moved.
        const TileProxy& ProxyOf( const TerrainDrawData& tile, bool deferred );

        struct FrameDraw
        {
            size_t   Group       = 0;
            uint32_t Row         = 0; // names BOTH the param row and the instance row
            uint32_t VertexCount = 0; // the procedural decode's whole draw (the virtual shadow map's records)
            // What RecordDraws draws instead (TERRAIN-GPU-2): the dice rings procedurally, the other cells through
            // the grid's shared index buffer (m_GridIndices[Split.Cells]).
            LandscapeDrawSplit Split;
        };
        // @p draw with its split for row @p row at grid LOD @p gridLod of a @p quadsPerTile tile, the grid's
        // index buffer made first if this is the first draw at that grid (EnsureGridIndices).
        void SplitDraw( FrameDraw& draw, uint32_t quadsPerTile, uint32_t gridLod, const TerrainInstance& row );
        // The shared index buffer of the @p cells-a-side grid (LandscapeGridIndices), made and uploaded on first
        // use, before any graph records a draw of it; an error is logged and the draws of that grid refuse.
        void EnsureGridIndices( uint32_t cells );

        // One point/spot light's terrain draws (PrepareFrame): the tiles in its reach, each at the LOD the light
        // asks for (its rows follow the camera's in the group buffers), and the key of what they draw.
        struct LocalLightDraws
        {
            LocalShadowCasterLight Light;
            std::vector<FrameDraw> Draws;
            uint64_t               Signature = 0;
        };

        // The draws shadow view @p view records: its light's (a local view), else its cascade's (the camera's
        // tiles and LOD - the receivers' vertices, so a shadow cannot swim against its caster - diced no finer
        // than the cascade's texels resolve; a movable layer view draws its cascade's).
        [[nodiscard]] const std::vector<FrameDraw>& ShadowViewDraws( uint32_t view ) const;

        // The executors of @p program's material of every frame group, in group order: a node drawing the
        // terrain declares one binding block per entry (block firstBlock + group), and RecordDraws records group
        // g's draws through block firstBlock + g. @p shadowSlope (RecordDraws): a light view's slope term (bias,
        // clamp), the push field ShadowSlope of the shadow programs; nullopt for a program without it.
        // The culled records a virtual shadow map level draws the tiles from (RecordVirtualShadow): tile k's at
        // record FirstRecord + k of Arguments.
        struct VirtualShadowIndirect
        {
            RDG::BufferRef Arguments;
            uint32_t       FirstRecord = 0;
        };

        [[nodiscard]] std::vector<const MaterialExecutor*>
        GroupExecutors( std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program ) const;

        [[nodiscard]] Common::BoolResultStr
        RecordDraws( const RDG::PassContext& context, uint32_t firstBlock, const GraphicsPipeline& pipeline,
                     std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program,
                     const glm::mat4& clipFromWorld, const std::vector<FrameDraw>& draws,
                     const std::optional<glm::vec2>&             shadowSlope = std::nullopt,
                     const std::optional<VirtualShadowIndirect>& indirect    = std::nullopt );

        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        // The forward program's DESERT_LOCAL_SHADOW_VSM permutation, built on the first frame whose local shadows
        // are virtual (SceneRenderer::LocalShadowsAreVirtual); a refusal is logged once and kept.
        std::shared_ptr<GraphicsPipeline> m_PipelineVsm;
        bool                              m_PipelineVsmRefused = false;
        // This frame's forward program (PrepareFrame): the VSM permutation while the local shadows are virtual.
        bool m_ForwardVirtual = false;
        bool m_ForwardRefused = false; // VirtualMaps asked for, its permutation refused: no forward terrain
        // Builds m_PipelineVsm on first use; false (said once) when the permutation or its pipeline is refused.
        [[nodiscard]] bool EnsureForwardVsmPipeline();
        using ProgramSlot = std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*;
        [[nodiscard]] ProgramSlot ForwardProgram() const
        {
            return m_ForwardVirtual ? &ProgramMaterials::ForwardVsm : &ProgramMaterials::Forward;
        }
        [[nodiscard]] const std::shared_ptr<GraphicsPipeline>& ForwardPipeline() const
        {
            return m_ForwardVirtual ? m_PipelineVsm : m_Pipeline;
        }
        // The pipeline the base pass records with: ForwardPipeline(), or under the wireframe view its line-polygon
        // twin (built on first use from the same spec; a refusal is said once and the solid pipeline draws).
        [[nodiscard]] GraphicsPipeline* BasePassPipeline();
#if DESERT_DEV_INSTRUMENTS
        bool                              m_Wireframe = false;
        std::shared_ptr<GraphicsPipeline> m_WireframePipeline;    // twin of m_Pipeline
        std::shared_ptr<GraphicsPipeline> m_WireframeVsmPipeline; // twin of m_PipelineVsm
        bool                              m_WireframeRefused = false;
#endif
        std::shared_ptr<GraphicsPipeline> m_GBufferPipeline;
        std::shared_ptr<GraphicsPipeline> m_DepthPipeline; // the forward depth prepass's (SceneDepthLayout)
        std::shared_ptr<GraphicsPipeline> m_ShadowPipeline;
        // m_ShadowPipeline with depth clamp: a sun cascade's terrain, pancaked (EnsureShadowPipeline,
        // RecordShadowView).
        std::shared_ptr<GraphicsPipeline> m_CascadeShadowPipeline;
        // The virtual shadow map's caster: no attachment, no depth test, depth clamp (the mesh renderer's VSM
        // casters' state). Built on the first level that draws the terrain; a refusal is logged once and kept.
        [[nodiscard]] const GraphicsPipeline* VirtualShadowPipeline();
        std::shared_ptr<GraphicsPipeline>     m_VirtualShadowPipeline;
        bool                                  m_VirtualShadowRefused = false;
        ShaderBindingLayoutCache              m_VirtualShadowLayout;

        // Each program's block layout, keyed on the shader of the pipeline its draws record with (kept, not
        // re-derived per frame; a reload re-derives it).
        ShaderBindingLayoutCache m_ForwardLayout;
        ShaderBindingLayoutCache m_ForwardVsmLayout;
        ShaderBindingLayoutCache m_GBufferLayout;
        ShaderBindingLayoutCache m_DepthLayout;
        ShaderBindingLayoutCache m_ShadowLayout;

        std::unordered_map<std::string, ProgramMaterials> m_Materials;
        // The landscape materials' layer inputs (their texture arrays), by the material's texture key: shared
        // by every tile of a landscape, kept between frames so an array is copied only when its sources change.
        std::unordered_map<std::string, TerrainLayerInputs> m_LayerInputs;
        std::shared_ptr<Image2D>                            m_EmptyLayerArray; // CreateTerrainEmptyLayerArray

        std::vector<TerrainDrawData>      m_Queue;
        std::vector<TerrainInstance> m_FrameInstances; // parallel to m_Queue after PrepareFrame
        std::vector<TerrainFrameMaterial> m_FrameMaterials; // parallel to m_Queue after PrepareFrame
        std::vector<FrameGroup>      m_FrameGroups;
        // The previous frame's group slots between ClearQueue and the next PrepareFrame: their vectors keep their
        // capacity, so a frame opens its groups with no allocation.
        std::vector<FrameGroup> m_FrameGroupPool;
        // Every tile's persistent proxy (TileProxy), by entity.
        std::unordered_map<uint32_t, TileProxy> m_TileProxies;
        // Counts PrepareFrame calls: ProgramMaterials::GroupFrame names the frame a set's group opened in.
        uint64_t m_PrepareSerial = 0u;
        std::vector<FrameDraw>       m_FrameDraws;
        // THE CAMERA'S DRAWS (UE: every landscape component is a primitive the view's frustum culls): the tiles of
        // m_FrameDraws whose displaced box meets the camera's frustum, in queue order - what the camera's own
        // rasters draw (TerrainGBuffer, the forward depth prepass and base pass). The casters, the dice pool and
        // the virtual shadow map keep m_FrameDraws: a tile outside the view still casts into it.
        std::vector<FrameDraw>       m_ViewDraws;
        // The triangle count of m_ViewDraws PrepareFrame last logged (TERRAIN-LOD-1's census of the ground).
        uint64_t m_LoggedViewTriangles = 0u;
        // THE VIRTUAL SHADOW MAP'S CASTER (TURN-CACHE-2; UE: a cached VSM page is invalidated by a caster change,
        // never by the camera's step): per queued tile, under Shadows.Method VirtualMaps, the row and draw "VSM:
        // Raster" draws - the camera's tile, LOD and dice rings, with the displacement faded from m_VsmAnchor
        // instead of the eye (LandscapeVirtualShadowRow). Its own dice-pool job evaluates it; its signature
        // (AppendVirtualShadowEntries) moves with the anchor, not the eye. Empty when the frame's shadows are not
        // virtual.
        std::vector<FrameDraw>       m_VsmDraws;
        std::vector<TerrainInstance> m_VsmInstances;
        // The point the virtual shadow map's displacement fade is measured from: the eye where it last left
        // kLandscapeVsmAnchorReachCm of the anchor (PrepareVirtualShadowRows). Every eye within the reach sees a
        // surface at or above the caster's (the shader's lower envelope), so the cached pages hold while it walks.
        glm::vec3 m_VsmAnchor{ 0.0f };
        bool      m_VsmAnchored = false;
        // The rows above for this frame's eye @p eye; @p diceWidthPerCm is DiceWindow.w (DicingRate pixels' width
        // in cm per cm of distance). After PrepareFrame's camera rows, so a group's instance rows past its param
        // rows are casters' only.
        void                         PrepareVirtualShadowRows( const glm::vec3& eye, float diceWidthPerCm );
        std::vector<LocalLightDraws> m_LocalLightDraws; // this frame's point/spot lights, PrepareFrame
        // Per sun cascade view (PrepareLocalShadowViews): the camera's draws at the cascade's dicing; empty for a
        // view with no cascade this frame.
        std::vector<std::vector<FrameDraw>> m_CascadeDraws;

        // THE DICE POOL (AddDicePoolPass): per diced camera row of the frame, what its compute reads; the row's
        // DicePool.x is its first vertex. m_DicePool is this frame's graph buffer, bound by every terrain draw.
        struct DicePoolJob
        {
            TerrainInstance          Row;
            std::shared_ptr<Image2D> Heightmap;
            std::shared_ptr<Image>   Weightmap; // the fallback when the tile has none
            bool                     HasWeightmap = false;
            std::shared_ptr<Image2D> LayerHeights;
            uint32_t                 Vertices = 0;
            // The row's corner nodes (LandscapeDicePoolCornerCount) and the first of them in the frame's corner
            // buffer (AddDicePoolPass).
            uint32_t Corners     = 0;
            uint32_t FirstCorner = 0;
        };
        std::vector<DicePoolJob>         m_DicePoolJobs;
        uint32_t                         m_DicePoolVertices = 0;
        uint32_t                         m_DicePoolCorners  = 0;
        // Puts @p row's pooled vertices (@p pooled > 0, LandscapeDicePoolVertexCount) at the pool's end: sets its
        // DicePool.x and adds its job with tile @p t's maps and @p layers' height array.
        void PushDicePoolJob( TerrainInstance& row, const TerrainDrawData& t, const TerrainLayerInputs& layers,
                              uint32_t pooled );
        // THE GRIDS' SHARED INDEX BUFFERS (UE FLandscapeSharedBuffers), by cells a side: one per grid size any tile
        // has drawn at (a 255-quad landscape's eight LOD grids, 2.1 MB), kept for the renderer's life. Written
        // only in PrepareFrame / PrepareLocalShadowViews, read by RecordDraws.
        std::unordered_map<uint32_t, std::shared_ptr<IndexBuffer>> m_GridIndices;
        RDG::BufferRef                   m_DicePool;
        std::shared_ptr<ComputePipeline> m_DicePoolPipeline;
        ShaderBindingLayoutCache         m_DicePoolLayout;
    };
} // namespace Desert::Graphic::System
