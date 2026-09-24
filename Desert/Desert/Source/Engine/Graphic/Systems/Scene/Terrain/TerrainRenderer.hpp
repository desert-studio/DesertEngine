#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp>

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Graphic::System
{
    // One terrain entity's render parameters, submitted per-frame from the ECS (TerrainECSSystem ->
    // SceneRenderer::SubmitTerrain). Mirrors the ECS TerrainData but stays Graphic-side (no ECS dep).
    struct TerrainDrawData
    {
        glm::mat4 Transform      = glm::mat4( 1.0f );
        float     Size           = 50.0f;
        int       Resolution     = 64;    // patch grid subdivisions per side (clamped at draw time)
        float     HeightScale    = 5.0f;  // Stage 2 (displacement)
        float     NoiseFrequency = 0.08f; // Stage 2
        int       Seed           = 1337;  // Stage 2

        // Per-layer splat mode (grass, rock, snow): 0 = Auto (rules), 1 = Manual (splat map), 2 = Off.
        glm::vec3 LayerModes = glm::vec3( 0.0f );

        // Per-terrain splat map (R=grass,G=rock,B=snow weights), painted by the editor brush. Non-owning;
        // null => the white fallback is used (Manual layers show everywhere until painted).
        Image2D* SplatMap = nullptr;

        // A landscape tile's R16 heightmap (LandscapeECSSystem's GPU copy of the tile's samples). Non-null
        // selects the heightmap path: Size/Resolution/HeightScale/NoiseFrequency/Seed/Transform are then
        // not read, and Landscape says where the tile sits. Null is the procedural TerrainComponent path.
        Image2D*          Heightmap = nullptr;
        LandscapeTileDraw Landscape;

        // Material param + texture overrides from the entity's MaterialComponent, applied generically to the
        // DataDrivenMaterial by name (params e.g. "Tint"/"DetailTiling"; textures = the splat layers
        // u_GrassTex/u_RockTex/...). Unset samplers keep the backend white fallback.
        Graphic::MaterialOverrides Overrides;
    };

    // GPU terrain renderer. Draws a tessellated patch grid into the scene framebuffer's Geometry phase
    // (vertexless patch-list draw -> TCS LOD -> TES displacement). Driven by the ECS: each TerrainComponent
    // entity is submitted as a TerrainDrawData every frame. Stage 1 keeps the surface flat (validates the
    // tessellation pipeline); later stages add compute-heightmap displacement + PBR shading.
    class TerrainRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;
        void                          RegisterPasses( RenderGraphBuilder& builder ) override;

        void Submit( const TerrainDrawData& data )
        {
            m_Queue.push_back( data );
        }

        void ClearQueue()
        {
            m_Queue.clear();
        }

    private:
        std::shared_ptr<GraphicsPipeline> m_Pipeline;

        // ONE MATERIAL PER TEXTURE SET, keyed by TerrainTextureKey (TerrainBatch.hpp), never one for the
        // whole queue: a sampler is a descriptor, a descriptor set belongs to the material, and the
        // set is written at most once per frame BEFORE its first bind — so with one shared material the
        // first terrain's textures were the frame's textures and every later SetTexture was silently
        // swallowed. Same rule and key shape as MeshRenderer::m_GenericMaterials. Keys persist across
        // frames (a material owns per-frame GPU state and must outlive the frames in flight); a scene's
        // terrain texture sets are few and stable, so the map does not grow in practice.
        std::unordered_map<std::string, std::unique_ptr<DataDrivenMaterial>> m_Materials;

        std::vector<TerrainDrawData> m_Queue;
    };
} // namespace Desert::Graphic::System
