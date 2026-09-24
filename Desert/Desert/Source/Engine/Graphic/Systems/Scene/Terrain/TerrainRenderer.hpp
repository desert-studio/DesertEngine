#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Systems/Scene/ShadowCaster.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp>

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Graphic::System
{
    // One landscape tile's render parameters, submitted per frame from the ECS (LandscapeECSSystem ->
    // DrawLandscapeTileCommand -> SceneRenderer::SubmitLandscapeTile). Graphic-side, no ECS dependency.
    struct TerrainDrawData
    {
        // The tile's R16 heightmap (LandscapeECSSystem's GPU copy of the tile's samples), and where the tile
        // sits in its root's frame.
        Image2D*          Heightmap = nullptr;
        LandscapeTileDraw Landscape;

        // Per-layer mode (grass, rock, snow) from the root's LandscapeMaterial: 0 = Auto (rules), 1 = Off.
        glm::vec3 LayerModes = glm::vec3( 0.0f );

        // The root's material, as param + texture overrides applied generically to the DataDrivenMaterial by
        // name (params e.g. "Tint"/"DetailTiling"; textures = the layers u_GrassTex/u_RockTex/...). Unset
        // samplers keep the backend white fallback.
        Graphic::MaterialOverrides Overrides;
    };

    // GPU landscape renderer. Draws each tile as a tessellated patch grid into the scene framebuffer's
    // Geometry phase (vertexless patch-list draw -> TCS LOD -> TES displacement from the tile's heightmap).
    // Driven by the ECS: every drawn LandscapeTileComponent is submitted as a TerrainDrawData each frame.
    // Draws every terrain of the frame, in whichever pass the render path shades opaque geometry with, and
    // into the sun's cascades. One frame's data — materials, param rows, TerrainInstances — is resolved ONCE
    // by PrepareFrame, and every pass that follows draws from it:
    //   - Forward:  the "TerrainPass" graph pass (Terrain.shader, lit by itself);
    //   - Deferred: RenderGBufferManual, after the meshes' G-buffer fill (TerrainGBuffer.shader, lit by the
    //     deferred composite like every other opaque surface — so it receives the cascaded shadows);
    //   - both:     RecordShadowCascade, from inside the mesh renderer's cascade passes (TerrainShadow.shader).
    // The three programs share their patch stages (Programs/Terrain/*.glslh) and differ in what the
    // fragment writes and in the matrix pushed per draw (camera or cascade).
    class TerrainRenderer final : public RenderSystem, public IShadowCaster
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;
        void                          RegisterPasses( RenderGraphBuilder& builder ) override;

        // The caster pipeline is built against the cascade targets, which the mesh renderer owns and makes.
        Common::BoolResultStr CreateShadowPipeline( const std::shared_ptr<Framebuffer>& cascadeFramebuffer );

        // Before the render graph records: resolves every queued terrain's material, packs the rows and
        // uploads them. Nothing may draw a terrain this frame before it ran.
        void PrepareFrame();

        // Deferred path: draws the frame's terrain into the G-buffer, in a LOAD pass after the meshes'.
        void RenderGBufferManual();

        void RecordShadowCascade( uint32_t cascade, const glm::mat4& cascadeViewProj ) override;

        void Submit( const TerrainDrawData& data )
        {
            m_Queue.push_back( data );
        }

        void ClearQueue()
        {
            m_Queue.clear();
            m_FrameGroups.clear();
            m_FrameDraws.clear();
        }

    private:
        // One texture set's materials, one per program. Textures are the materials' identity (see
        // TerrainTextureKey), so each program needs its own copy of the set.
        struct ProgramMaterials
        {
            std::unique_ptr<DataDrivenMaterial> Forward;
            std::unique_ptr<DataDrivenMaterial> GBuffer;
            std::unique_ptr<DataDrivenMaterial> Shadow;
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

        void RecordDraws( GraphicsPipeline*                   pipeline,
                          std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program,
                          const glm::mat4&                                       clipFromWorld );

        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        std::shared_ptr<GraphicsPipeline> m_GBufferPipeline;
        std::shared_ptr<GraphicsPipeline> m_ShadowPipeline;

        std::unordered_map<std::string, ProgramMaterials> m_Materials;

        std::vector<TerrainDrawData> m_Queue;
        std::vector<FrameGroup>      m_FrameGroups;
        std::vector<FrameDraw>       m_FrameDraws;
    };
} // namespace Desert::Graphic::System
