#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Water/WaterQuadTree.hpp>

#include <glm/glm.hpp>

#include <cstdint>
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
    };

    // THE WATER SURFACE RENDERER (WATER-W6; UE FWaterMeshSceneProxy + its water vertex factory). Each frame it
    // walks every submitted zone's quadtree from the main camera (Water::WaterQuadTree::Traverse: LOD by distance,
    // frustum-culled), packs the selected tiles as rows of one storage buffer, and draws them in the deferred
    // path's "Deferred: Water" node — after the scene-colour copy (the scene without water, which WATER-W7's
    // refraction reads) and before the glass — as one vertexless instanced draw per grid density
    // (WaterSurface.shader).
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

        // SETUP of the water node: its one binding block, with the frame's fog inputs (the surface fogs itself).
        void                                DeclareDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
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

        std::shared_ptr<GraphicsPipeline>   m_Pipeline;
        ShaderBindingLayoutCache            m_Layout;
        std::unique_ptr<DataDrivenMaterial> m_Material;

        std::vector<WaterZoneDraw> m_Queue;
        std::vector<FrameDraw>     m_FrameDraws;
    };
} // namespace Desert::Graphic::System
