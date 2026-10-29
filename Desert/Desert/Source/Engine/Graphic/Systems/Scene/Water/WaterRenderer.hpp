#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Water/WaterQuadTree.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>

namespace Desert::Graphic::System
{
    // One water zone of this frame, submitted from the ECS (ECS::WaterZoneECSSystem -> WaterZoneCommand ->
    // SceneRenderer::SubmitWaterZone): the zone and the bodies' footprints inside it. Graphic-side, no ECS types.
    struct WaterZoneDraw
    {
        Water::WaterQuadTreeDesc              Zone;
        std::vector<Water::WaterQuadTreeBody> Bodies;
        // Each body's water material (ECS::WaterBodyData::WaterMaterial), in Bodies' order.
        std::vector<Assets::AssetHandle> BodyMaterials;
    };

    // THE WATER SURFACE RENDERER (WATER-W6; UE FWaterMeshSceneProxy + its water vertex factory). Each frame it
    // walks every submitted zone's quadtree from the main camera (Water::WaterQuadTree::Traverse: LOD by distance,
    // frustum-culled), packs the selected tiles as rows of one storage buffer, and draws them in the deferred
    // path's "Deferred: Water" node — after the scene-colour copy (the scene without water, which the refraction
    // reads) and before the glass — as one vertexless instanced draw per grid density.
    //
    // THE LOOK IS AN ASSET (UE UWaterBodyComponent::WaterMaterial): each body names a `.demat` whose template is a
    // Surface-domain template of ShadingModel SingleLayerWater with Usage WaterMesh (WaterSurface.shader is the
    // engine's); this renderer draws that template's `WaterMesh.SingleLayerWater` cell (the water mesh vertex
    // factory + the single layer water pass), its material row folded from the `.demat` over the template's schema
    // (MaterialService::ResolveOverrides) and its ripple maps in the bindless slots the row names.
    class WaterRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override;

        // Before the render graph records: traverses every zone and uploads the rows. Nothing may draw the water
        // this frame before it ran.
        void PrepareFrame();

        [[nodiscard]] bool HasDraws() const
        {
            return !m_FrameDraws.empty();
        }

        // SETUP of the water node: its one binding block - the material route (frame block, tile rows, material
        // values and ripple maps), the opaque scene's copy @p sceneCopy the surface refracts, and the scene/view
        // inputs of @p view its program has slots for (the opaque depth = the water's bottom, the sky cubes).
        void DeclareDraws( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, const SceneViewInputs& view,
                           const SceneFrameBinding& frame );
        [[nodiscard]] Common::BoolResultStr RenderManual( const RDG::PassContext& context );

        void Submit( WaterZoneDraw zone )
        {
            m_Queue.push_back( std::move( zone ) );
        }

        void ClearQueue()
        {
            m_Queue.clear();
            m_FrameDraws.clear();
        }

    private:
        // One run of rows sharing a grid density: one instanced draw.
        struct FrameDraw
        {
            uint32_t FirstRow     = 0;
            uint32_t RowCount     = 0;
            uint32_t QuadsPerSide = 0;
        };

        // One SingleLayerWater program's pipeline and its material (the frame block, the rows and the values of
        // the `.demat` drawn with it), built on first use of the template.
        struct WaterProgram
        {
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            std::unique_ptr<DataDrivenMaterial> Material;
        };

        // The program of @p material's template, built on first use; null (said once per handle) when the handle
        // names no material, or one whose template is not of the SingleLayerWater domain or does not build.
        WaterProgram* ProgramFor( const Assets::AssetHandle& material );
        // Folds @p material's values over its program's schema into the program's WaterMaterialUB and ripple maps.
        void ApplyMaterial( const Assets::AssetHandle& material, WaterProgram& program );
        void WarnOnce( const Assets::AssetHandle& material, const std::string& why );

        std::unordered_map<std::string, WaterProgram> m_Programs;        // by template compile name
        WaterProgram*                                 m_Frame = nullptr; // this frame's program
        ShaderBindingLayoutCache                      m_Layout;
        std::unordered_set<uint64_t>                  m_Warned;

        std::vector<WaterZoneDraw> m_Queue;
        std::vector<FrameDraw>     m_FrameDraws;
    };
} // namespace Desert::Graphic::System
