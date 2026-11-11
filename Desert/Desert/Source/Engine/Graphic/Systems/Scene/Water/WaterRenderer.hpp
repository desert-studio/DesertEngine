#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Water/WaterQuadTree.hpp>
#include <Engine/Water/WaterBodyQuery.hpp>
#include <Engine/Assets/Common.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::Graphic::System
{
    // One water zone of this frame, submitted from the ECS (ECS::WaterZoneECSSystem -> WaterZoneCommand ->
    // SceneRenderer::SubmitWaterZone): the zone and the bodies' footprints inside it. Graphic-side, no ECS types.
    // The frame's visible water bodies, as every zone of the frame draws them. Immutable once built and shared by
    // every zone's draw: the collector keeps it across frames and builds a new one only when a body changed.
    struct WaterZoneBodies
    {
        std::vector<Water::WaterQuadTreeBody> Footprints;
        // Each body's water material (ECS::WaterBodyData::WaterMaterial), in Footprints' order.
        std::vector<Assets::AssetHandle> Materials;
        // Each body's wave set (ECS::WaterBodyData::WaterWaves; Null = a flat body) and its WaveOrigin (the
        // entity's X, Z), in Footprints' order.
        std::vector<Assets::AssetHandle> Waves;
        std::vector<glm::vec2>           WaveOrigins;
        // Each body's TargetWaveMaskDepth (cm, UE): the depth below which the GPU waves damp, read against the
        // WaterInfo ground exactly as Water::QueryWaterBody damps the CPU's.
        std::vector<float> WaveMaskDepths;
    };

    struct WaterZoneDraw
    {
        Water::WaterQuadTreeDesc Zone;
        // Never null for a submitted zone: a frame without a visible body submits no zone.
        std::shared_ptr<const WaterZoneBodies> Bodies;
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

        // THE FRAME'S WATER IN THREE STEPS (FPS-ARCH-SETUP3, UE FSceneRenderer task-based view setup), before the
        // render graph records; nothing may draw the water this frame before FinishPrepare ran.
        // PrepareFrame (render thread): the frame's material and program, its waves block (written into the
        // upload's head), the camera's matrices and lens scale.
        void PrepareFrame();
        // PrepareTiles (a setup task, or inline): every zone's retained quadtree walked from the camera
        // PrepareFrame took, its tiles appended to the upload as rows and grouped into the frame's draws. Touches
        // only the zone trees, the traversal scratch, the upload's rows and the draws - no service, no GPU object.
        void PrepareTiles() noexcept;
        // FinishPrepare (render thread, after the task joined): the material's fold, the WaterInfo ground and the
        // rows' upload; the frame's program is set only here.
        void FinishPrepare();

        [[nodiscard]] bool HasDraws() const
        {
            return !m_FrameDraws.empty();
        }

        // Whether this frame's water program samples @p textureName (a scene/view input by its shader name): the
        // shadow gates' question (SceneRenderer ShadowMapReadersOutsideMeshes), answered after FinishPrepare. The
        // water reads the sun's cascades and the point/spot atlas wherever it draws - it is in no opaque depth a
        // screen shadow mask could serve it from.
        [[nodiscard]] bool FrameSamples( std::string_view textureName ) const;

        // The WaterInfo ground (UE WaterInfo texture) FinishPrepare keeps: a persistent image the frame graph
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

        // By reference, as GrassRenderer::Submit: @p zone is the recorded command's, held until ClearQueue.
        void Submit( WaterZoneDraw&& ) = delete; // the queue keeps a reference: a temporary would dangle
        void Submit( const WaterZoneDraw& zone )
        {
            m_Queue.emplace_back( zone );
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

        // What a material resolution read, so a cache of one is current exactly while none of it moved (the
        // LandscapeECSSystem overrides key): the material service, its rebuild stamp, its in-place value edit
        // stamp and the content registry's write serial. Taken once per frame (PrepareFrame).
        struct MaterialStamp
        {
            const void* Service             = nullptr;
            uint32_t    InvalidationVersion = 0;
            uint32_t    ValuesEditSerial    = 0;
            uint64_t    ContentSerial       = 0;
            bool        operator==( const MaterialStamp& ) const = default;
        };

        // THE FOLDED VALUES (UE FMaterialRenderProxy's cached uniform expressions): the `.demat` last folded into
        // a program's material row, re-resolved and re-folded only when another material is drawn with the
        // program or the stamp moved. The textures are re-bound every frame from Overrides (allocation-free
        // lookups): a texture still loading lands the frame it is ready, with no stamp of its own.
        struct FoldedValues
        {
            uint64_t          Material = 0; // the handle, 0 = nothing folded yet
            MaterialStamp     Stamp;
            MaterialOverrides Overrides;
        };

        // One SingleLayerWater program's pipeline and its material (the frame block, the rows and the values of
        // the `.demat` drawn with it), built on first use of the template.
        struct WaterProgram
        {
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            std::unique_ptr<DataDrivenMaterial> Material;
            FoldedValues                        Folded;
            // The same cell under DESERT_LOCAL_SHADOW_VSM (MeshRenderer::ForwardShadowMaskVariant's virtual map),
            // drawn under Shadows.Method VirtualMaps, when no atlas is drawn; built on first such frame. Its own
            // material: the frame's values and rows are written into the program that draws.
            std::unique_ptr<WaterProgram> VirtualLocal;
            bool                          VirtualLocalTried = false;
        };

        // The program of @p material's template, built on first use; null (said once per handle) when the handle
        // names no material, or one whose template is not of the SingleLayerWater domain or does not build.
        WaterProgram* ProgramFor( const Assets::AssetHandle& material, const MaterialStamp& stamp );
        // @p program's DESERT_LOCAL_SHADOW_VSM twin (WaterProgram::VirtualLocal); null (said once per handle) when
        // the permutation does not build - the water is then not drawn under VirtualMaps rather than drawn with
        // every point and spot light unshadowed.
        WaterProgram* VirtualLocalProgramFor( const Assets::AssetHandle& material, WaterProgram& program );
        // The pipeline of @p shader into @p program; the build's refusal otherwise.
        [[nodiscard]] Common::BoolResultStr BuildProgram( const std::shared_ptr<Shader>& shader,
                                                          const std::string& debugName, WaterProgram& program );
        // Folds @p material's values over its program's schema into the program's WaterMaterialUB and ripple maps;
        // the fold is cached in WaterProgram::Folded, so a steady frame resolves nothing and allocates nothing.
        void ApplyMaterial( const Assets::AssetHandle& material, const MaterialStamp& stamp,
                            WaterProgram& program );

        // WARNINGS ONCE PER CAUSE: each (material, cause) is said once. FirstWarning asks (and marks) before any
        // message is built, so a cause that recurs every frame builds no string after its first.
        enum class WaterWarning : uint8_t
        {
            NoTemplate,
            NoWaterCell,
            TemplateDoesNotBuild,
            NoVirtualLocalPermutation,
            VirtualLocalDoesNotBuild,
            MixedMaterials,
            NoWavesService,
            WavesRefused,
            TooManyWaves,
        };
        [[nodiscard]] bool FirstWarning( const Assets::AssetHandle& material, WaterWarning cause );
        void WarnOnce( const Assets::AssetHandle& material, WaterWarning cause, std::string_view why );
        static void Warn( const Assets::AssetHandle& material, std::string_view why );

        std::unordered_map<std::string, WaterProgram> m_Programs;        // by template compile name
        WaterProgram*                                 m_Frame = nullptr; // this frame's program
        ShaderBindingLayoutCache                      m_Layout;
        std::unordered_set<uint64_t>                  m_Warned; // FirstWarning's (material, cause) keys
        // ProgramFor's last answer (UE: the proxy's cached FMaterial), while the material and the stamp stand.
        struct ResolvedProgram
        {
            uint64_t      Material = 0;
            MaterialStamp Stamp;
            WaterProgram* Program = nullptr; // m_Programs' entries are never erased
        };
        ResolvedProgram m_Resolved;

        // The WaterInfo ground uploaded last (WaterZoneDraw::Ground) and its texture.
        std::shared_ptr<const Water::WaterInfoGround> m_GroundBaked;
        std::shared_ptr<Image2D>                      m_WaterInfo;

        std::vector<std::reference_wrapper<const WaterZoneDraw>> m_Queue;
        std::vector<FrameDraw>                                   m_FrameDraws;

        // THE RETAINED QUADTREES (UE: FWaterQuadTree is built once per water body set and only traversed per
        // view): one per submitted zone, in queue order, rebuilt only when that zone's desc or its WaterZoneBodies
        // changed (the collector shares one WaterZoneBodies until a body changes, so the pointer is the body set's
        // identity).
        struct ZoneTree
        {
            Water::WaterQuadTreeDesc               Zone;
            std::shared_ptr<const WaterZoneBodies> Bodies;
            std::optional<Water::WaterQuadTree>    Tree;
        };
        std::vector<ZoneTree> m_ZoneTrees;
        // What PrepareFrame hands the tile task and FinishPrepare (FPS-ARCH-SETUP3): the program the frame draws
        // with (null: no water, the task walks nothing), the material and stamp its fold reads, the ground, the
        // camera the quadtrees are walked from, and the task's row count.
        struct TileWalk
        {
            WaterProgram*                                 Program  = nullptr;
            Assets::AssetHandle                           Material = Assets::AssetHandle::Null();
            MaterialStamp                                 Stamp;
            std::shared_ptr<const Water::WaterInfoGround> Ground;
            glm::mat4                                     View{ 1.0f };
            glm::mat4                                     Projection{ 1.0f };
            glm::vec3                                     Observer{ 0.0f };
            float                                         LensScale      = 1.0f;
            float                                         VerticalExtent = 0.0f;
            uint32_t                                      Rows           = 0;
        };
        TileWalk m_Walk;
        // Per-view scratch the frame reuses, so a steady frame allocates nothing: the traversal's tiles, the
        // upload (the waves block, then the tile rows), and RenderManual's push block.
        Water::WaterTraversalBuffers m_Traversal;
        std::vector<std::byte>       m_Upload;
        std::vector<std::byte>       m_Push;
    };
} // namespace Desert::Graphic::System
