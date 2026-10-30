#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Water/WaterQuadTree.hpp>
#include <Engine/Water/WaterBodyQuery.hpp>
#include <Engine/Assets/Common.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
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
        // Each body's wave set (ECS::WaterBodyData::WaterWaves; Null = a flat body) and its WaveOrigin (the
        // entity's X, Z), in Bodies' order.
        std::vector<Assets::AssetHandle> BodyWaves;
        std::vector<glm::vec2>           BodyWaveOrigins;
        // Each body's TargetWaveMaskDepth (cm, UE): the depth below which the GPU waves damp, read against the
        // WaterInfo ground exactly as Water::QueryWaterBody damps the CPU's.
        std::vector<float> BodyWaveMaskDepths;
        // The zone's bottom seen from above (UE WaterInfo ground depth); null over no landscape.
        std::shared_ptr<const Water::WaterInfoGround> Ground;
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

        // Whether this frame's water program samples @p textureName (a scene/view input by its shader name): the
        // shadow gates' question (SceneRenderer ShadowMapReadersOutsideMeshes), answered after PrepareFrame. The
        // water reads the sun's cascades and the point/spot atlas wherever it draws - it is in no opaque depth a
        // screen shadow mask could serve it from.
        [[nodiscard]] bool FrameSamples( std::string_view textureName ) const;

        // The WaterInfo ground (UE WaterInfo texture) PrepareFrame keeps: a persistent image the frame graph
        // imports and the water pass samples as WaterInfoTexture - a pass input, not a material property.
        [[nodiscard]] const std::shared_ptr<Image2D>& GetWaterInfo() const
        {
            return m_WaterInfo;
        }

        // SETUP of the water node: its one binding block - the material route (frame block, tile rows, material
        // values and ripple maps), the opaque scene's copy @p sceneCopy the surface refracts, and the scene/view
        // inputs of @p view its program has slots for (the opaque depth = the water's bottom, the sky cubes, the
        // view's fog), and the imported WaterInfo ground @p waterInfo (GetWaterInfo) the surf reads its depth from.
        void DeclareDraws( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, RDG::TextureRef waterInfo,
                           const SceneViewInputs& view, const SceneFrameBinding& frame );
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
            // The same cell under DESERT_LOCAL_SHADOW_VSM (MeshRenderer::ForwardShadowMaskVariant's virtual map),
            // drawn under Shadows.Method VirtualMaps, when no atlas is drawn; built on first such frame. Its own
            // material: the frame's values and rows are written into the program that draws.
            std::unique_ptr<WaterProgram> VirtualLocal;
            bool                          VirtualLocalTried = false;
        };

        // The program of @p material's template, built on first use; null (said once per handle) when the handle
        // names no material, or one whose template is not of the SingleLayerWater domain or does not build.
        WaterProgram* ProgramFor( const Assets::AssetHandle& material );
        // @p program's DESERT_LOCAL_SHADOW_VSM twin (WaterProgram::VirtualLocal); null (said once per handle) when
        // the permutation does not build - the water is then not drawn under VirtualMaps rather than drawn with
        // every point and spot light unshadowed.
        WaterProgram* VirtualLocalProgramFor( const Assets::AssetHandle& material, WaterProgram& program );
        // The pipeline of @p shader into @p program; the build's refusal otherwise.
        [[nodiscard]] Common::BoolResultStr BuildProgram( const std::shared_ptr<Shader>& shader,
                                                          const std::string& debugName, WaterProgram& program );
        // Folds @p material's values over its program's schema into the program's WaterMaterialUB and ripple maps.
        void ApplyMaterial( const Assets::AssetHandle& material, WaterProgram& program );
        void WarnOnce( const Assets::AssetHandle& material, const std::string& why );

        std::unordered_map<std::string, WaterProgram> m_Programs;        // by template compile name
        WaterProgram*                                 m_Frame = nullptr; // this frame's program
        ShaderBindingLayoutCache                      m_Layout;
        std::unordered_set<uint64_t>                  m_Warned;

        // The WaterInfo ground uploaded last (WaterZoneDraw::Ground) and its texture.
        std::shared_ptr<const Water::WaterInfoGround> m_GroundBaked;
        std::shared_ptr<Image2D>                      m_WaterInfo;

        std::vector<WaterZoneDraw> m_Queue;
        std::vector<FrameDraw>     m_FrameDraws;
    };
} // namespace Desert::Graphic::System
