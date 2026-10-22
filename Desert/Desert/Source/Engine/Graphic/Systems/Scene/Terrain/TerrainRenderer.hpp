#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Graphic/Systems/Scene/ShadowCaster.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainLayerInputs.hpp>

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
        // name (e.g. "Tint"). Unset parameters keep the shader's schema defaults.
        Graphic::MaterialOverrides Overrides;
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
        // matrices — the same view every other pass of the frame draws for — with @p scene's lights, shadow
        // block and cloud shadow on the forward program (MeshRenderer::CaptureFrameState). The instance rows go
        // up in PrepareLocalShadowViews, which must follow it: nothing may draw a terrain before both ran.
        void PrepareFrame( const ViewFrame& frame, const SceneFrameBinding& scene );

        // After PrepareFrame AND MeshRenderer::UpdateLocalShadows (the lights get their views there, later in the
        // frame than PrepareFrame, whose rows the TLAS and the motion rows read first): for each of @p localLights
        // (MeshRenderer::GetLocalShadowCasterLights) the tiles in its reach at the LOD its own position asks for,
        // with their cache key (LocalShadowSignature), then the instance rows of every group are uploaded.
        void PrepareLocalShadowViews( const std::vector<LocalShadowCasterLight>& localLights );

        // Deferred path: draws the frame's terrain into the G-buffer, in a LOAD pass after the meshes'. SETUP of
        // that node: one binding block per G-buffer material of the frame's groups (the G-buffer shader samples
        // no scene/view input); the exec records through them.
        void                                DeclareGBufferDraws( RDG::PassBuilder& pass );
        [[nodiscard]] Common::BoolResultStr RenderGBufferManual( const RDG::PassContext& context );

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
        [[nodiscard]] uint32_t DeclareShadowView( RenderPassDeclaration& declared, uint32_t view ) override;
        [[nodiscard]] Common::BoolResultStr RecordShadowView( const RDG::PassContext& context, uint32_t view,
                                                              uint32_t firstBlock, const glm::mat4& casterViewProj,
                                                              const CascadeCasterBias& bias ) override;

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

        void ClearQueue()
        {
            m_Queue.clear();
            m_FrameInstances.clear();
            m_FrameMaterials.clear();
            m_FrameGroups.clear();
            m_FrameDraws.clear();
            m_LocalLightDraws.clear();
        }

    private:
        // One texture set's materials, one per program. Textures are the materials' identity (see
        // TerrainTextureKey), so each program needs its own copy of the set.
        struct ProgramMaterials
        {
            std::unique_ptr<DataDrivenMaterial> Forward;
            std::unique_ptr<DataDrivenMaterial> GBuffer;
            std::unique_ptr<DataDrivenMaterial> Shadow;
            // TerrainVirtualShadow.shader: the virtual shadow map's caster (the same vertex stage as Shadow).
            std::unique_ptr<DataDrivenMaterial> VirtualShadow;
        };

        // A frame's terrains sharing one texture set. Named by key, not by pointer: m_Materials owns them.
        struct FrameGroup
        {
            std::string                  Key;
            std::vector<glm::vec4>       ParamRows;
            std::vector<TerrainInstance> Instances;
        };

        struct FrameDraw
        {
            size_t   Group       = 0;
            uint32_t Row         = 0; // names BOTH the param row and the instance row
            uint32_t VertexCount = 0;
        };

        // One point/spot light's terrain draws (PrepareFrame): the tiles in its reach, each at the LOD the light
        // asks for (its rows follow the camera's in the group buffers), and the key of what they draw.
        struct LocalLightDraws
        {
            LocalShadowCasterLight Light;
            std::vector<FrameDraw> Draws;
            uint64_t               Signature = 0;
        };

        // The draws shadow view @p view records: its light's (a local view), else the camera's (a cascade: the
        // receivers' vertices, so a shadow cannot swim against its caster).
        [[nodiscard]] const std::vector<FrameDraw>& ShadowViewDraws( uint32_t view ) const;

        // The executors of @p program's material of every frame group, in group order: a node drawing the
        // terrain declares one binding block per entry (block firstBlock + group), and RecordDraws records group
        // g's draws through block firstBlock + g.
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
                     const std::optional<VirtualShadowIndirect>& indirect = std::nullopt );

        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        std::shared_ptr<GraphicsPipeline> m_GBufferPipeline;
        std::shared_ptr<GraphicsPipeline> m_ShadowPipeline;
        // The virtual shadow map's caster: no attachment, no depth test, depth clamp (the mesh renderer's VSM
        // casters' state). Built on the first level that draws the terrain; a refusal is logged once and kept.
        [[nodiscard]] const GraphicsPipeline* VirtualShadowPipeline();
        std::shared_ptr<GraphicsPipeline>     m_VirtualShadowPipeline;
        bool                                  m_VirtualShadowRefused = false;
        ShaderBindingLayoutCache              m_VirtualShadowLayout;

        // Each program's block layout, keyed on the shader of the pipeline its draws record with (kept, not
        // re-derived per frame; a reload re-derives it).
        ShaderBindingLayoutCache m_ForwardLayout;
        ShaderBindingLayoutCache m_GBufferLayout;
        ShaderBindingLayoutCache m_ShadowLayout;

        std::unordered_map<std::string, ProgramMaterials> m_Materials;
        // The landscape materials' layer inputs (their texture arrays), by the material's texture key: shared
        // by every tile of a landscape, kept between frames so an array is copied only when its sources change.
        std::unordered_map<std::string, TerrainLayerInputs> m_LayerInputs;
        std::shared_ptr<Image2D>                            m_EmptyLayerArray; // CreateTerrainEmptyLayerArray

        std::vector<TerrainDrawData> m_Queue;
        std::vector<TerrainInstance> m_FrameInstances; // parallel to m_Queue after PrepareFrame
        std::vector<TerrainFrameMaterial> m_FrameMaterials; // parallel to m_Queue after PrepareFrame
        std::vector<FrameGroup>      m_FrameGroups;
        std::vector<FrameDraw>       m_FrameDraws;
        std::vector<LocalLightDraws> m_LocalLightDraws; // this frame's point/spot lights, PrepareFrame
    };
} // namespace Desert::Graphic::System
