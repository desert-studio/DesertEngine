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
#include <Engine/Water/FFTOceanSpectrum.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Systems/Scene/Water/WaterZoneDraw.hpp>
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

namespace Desert::Graphic::RDG
{
    class Builder; // Engine/Graphic/RDG/RDGBuilder.hpp: AddWavePasses records into it
} // namespace Desert::Graphic::RDG

namespace Desert::Graphic::System
{
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

        // THE FFT OCEAN (WATER-RIVERS-FFT; Tessendorf 2001, the UE Water plugin's FFT wave source): when the frame's
        // body names a wave set authored as an FFT ocean and Effects.WaterWaveMethod resolves to FFT, PrepareFrame
        // sets the frame's spectrum (uploading its h0 cascades once per set) and the waves block's FFT lane;
        // the frame graph then records AddWavePasses once (SceneRenderer::FrameWaterWaves) before the first water
        // draw, and both WaterMesh cells sample the result as WaterFFTTexture.
        [[nodiscard]] bool FrameWavesAreFFT() const
        {
            return m_FFTFrame != nullptr && !m_Walk.Programs.empty();
        }
        // The h0 cascades of the frame's spectrum: kFFTOceanResolution x 3 kFFTOceanResolution RGBA32F, cascade c
        // in rows [N c, N c + N) (Programs/Water/WaterFFTSpectrum.shader).
        [[nodiscard]] const std::shared_ptr<Image2D>& GetFFTInitialSpectrum() const
        {
            return m_FFTInitial;
        }
        // The persistent two-frame waves atlas (Mesh/Surface/WaterMeshTiles.glslh WaterFFTTexture): N x 12 N
        // RGBA16F, this frame's half written by AddWavePasses, the other half the previous frame's (the velocity).
        [[nodiscard]] const std::shared_ptr<Image2D>& GetFFTWaves() const
        {
            return m_FFTWaves;
        }
        // The waves this frame's water samples, once AddWavePasses recorded them into @p graph; invalid otherwise.
        [[nodiscard]] RDG::TextureRef RecordedWaves( const RDG::Builder& graph ) const
        {
            return m_FrameWavesGraph == &graph ? m_FrameWavesTexture : RDG::TextureRef{};
        }
        // THE OCEAN AT THE VIEW'S TIME (Programs/Water/WaterFFTSpectrum, WaterFFT x2, WaterFFTFinalize): the
        // cascades' spectra from @p initial (the imported GetFFTInitialSpectrum) advanced to @p timeSeconds
        // (cameraUB.Time), inverse-transformed along rows then columns, and unpacked into this frame's half of
        // @p waves (the imported GetFFTWaves). Returns @p waves; invalid when the frame draws no FFT ocean.
        RDG::TextureRef AddWavePasses( RDG::Builder& graph, RDG::TextureRef initial, RDG::TextureRef waves,
                                       float timeSeconds );

        // SETUP of the water node: its one binding block - the material route (frame block, tile rows, material
        // values and ripple maps), the opaque scene's copy @p sceneCopy the surface refracts, and the scene/view
        // inputs of @p view its program has slots for (the opaque depth = the water's bottom, the sky cubes, the
        // view's fog), and the imported WaterInfo ground @p waterInfo (GetWaterInfo) the surf reads its depth
        // from.
        // @p frontReflection / @p frontDepth: this frame's front layer reflection and the depth it belongs to
        // (SceneRenderer::FrameFrontLayer; the graph's black texture for both when no front layer was traced),
        // bound as FrontLayerReflectionTexture / FrontLayerDepthTexture where the cell has the slots
        // (Mesh/Surface/FrontLayerReflection.glslh: the water's traced reflection where it IS the front layer).
        // @p waves: this frame's FFT atlas (SceneRenderer::FrameWaterWaves), the graph's black without one.
        void DeclareDraws( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, RDG::TextureRef waterInfo,
                           RDG::TextureRef waves, RDG::TextureRef frontReflection, RDG::TextureRef frontDepth,
                           const SceneViewInputs& view, const SceneFrameBinding& frame );
        [[nodiscard]] Common::BoolResultStr RenderManual( const RDG::PassContext& context );

        // THE WATER'S FRONT LAYER (UE: single layer water writes the G-buffer its SSR / Lumen reflections trace
        // from): this frame's `WaterMesh.FrontLayer` cell over the same tile rows, drawn into the translucent
        // front layer's targets ("Translucency: Front Layer G-buffer (Water)",
        // SceneRenderer::AddFrameFrontLayerSurface). True after PrepareFrame when the water draws and its
        // template's front layer cell built.
        [[nodiscard]] bool HasFrontLayerDraws() const
        {
            return !m_FrameDraws.empty() && m_FrameFrontLayers > 0u;
        }
        // SETUP of the front layer node: the cell's material route (frame block, tile rows, material row and
        // ripple maps), the scene/view inputs it has slots for and the WaterInfo ground @p waterInfo its waves
        // damp by.
        void DeclareFrontLayerDraws( RDG::PassBuilder& pass, RDG::TextureRef waterInfo, RDG::TextureRef waves,
                                     const SceneViewInputs& view, const SceneFrameBinding& frame );
        [[nodiscard]] Common::BoolResultStr RenderFrontLayerManual( const RDG::PassContext& context );

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
        // One run of rows sharing a grid density, a template and a body material: one instanced draw.
        struct FrameDraw
        {
            uint32_t FirstRow     = 0;
            uint32_t RowCount     = 0;
            uint32_t QuadsPerSide = 0;
            // The tiles' body material row in their program's Materials block (FrameProgram::Materials), the
            // draw's MaterialIndex.
            uint32_t MaterialSlot = 0;
            // The tiles' template (TileWalk::Programs): the program that records the draw.
            uint32_t ProgramSlot = 0;
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
            // One fold per body material drawn with this program (ApplyMaterial), and the material whose values
            // the cell holds now (0: none / refolded).
            std::vector<FoldedValues> Folded;
            uint64_t                  Applied = 0;
            // FPS-MAT-HANDLES3, THE CELL'S BLOCKS (UE: a shader's buffer slots are bound once per shader map): its
            // Materials[] and WaterTiles storage-buffer properties, found by name once per (layout stamp,
            // executor) - the executor owns its properties for its whole life, a recompile moves the stamp - and
            // written through the kept pointers every frame (BlocksOf). Null: the shader declares no such block.
            struct CellBlocks
            {
                MaterialLayoutStamp     Stamp;
                const MaterialExecutor* Executor = nullptr;
                StorageBufferProperty*  Rows     = nullptr;
                StorageBufferProperty*  Tiles    = nullptr;
                bool                    Resolved = false;
            } Blocks;
            // The same cell under DESERT_LOCAL_SHADOW_VSM (MeshRenderer::ForwardShadowMaskVariant's virtual map),
            // drawn under Shadows.Method VirtualMaps, when no atlas is drawn; built on first such frame. Its own
            // material: the frame's values and rows are written into the program that draws.
            std::unique_ptr<WaterProgram> VirtualLocal;
            bool                          VirtualLocalTried = false;
            // The template's `WaterMesh.FrontLayer` cell (FrontLayerProgramFor), built on first use; its own
            // material, written every frame from the same `.demat` and tile rows as the program it belongs to.
            std::unique_ptr<WaterProgram> FrontLayer;
            bool                          FrontLayerTried = false;
            // The template's compile name, the prefix of every cell of it (set by ProgramFor).
            std::string CompileName;
        };

        // The program of @p material's template, built on first use; null (said once per handle) when the handle
        // names no material, or one whose template is not of the SingleLayerWater domain or does not build.
        WaterProgram* ProgramFor( const Assets::AssetHandle& material, const MaterialStamp& stamp );
        // @p program's DESERT_LOCAL_SHADOW_VSM twin (WaterProgram::VirtualLocal); null (said once per handle) when
        // the permutation does not build - the water is then not drawn under VirtualMaps rather than drawn with
        // every point and spot light unshadowed.
        WaterProgram* VirtualLocalProgramFor( const Assets::AssetHandle& material, WaterProgram& program );
        // @p program's front layer cell (WaterProgram::FrontLayer); null (said once per handle) when the template
        // has no such cell or it does not build - the water then reflects its captures and sky without the trace.
        WaterProgram* FrontLayerProgramFor( const Assets::AssetHandle& material, WaterProgram& program );
        // DeclareDraws' / DeclareFrontLayerDraws' binding block of one template's @p program (one per
        // TileWalk::Programs entry; the front layer's only for entries with a front layer cell).
        void DeclareProgramDraws( RDG::PassBuilder& pass, WaterProgram& program, RDG::TextureRef sceneCopy,
                                  RDG::TextureRef waterInfo, RDG::TextureRef waves,
                                  RDG::TextureRef frontReflection, RDG::TextureRef frontDepth,
                                  const SceneViewInputs& view, const SceneFrameBinding& frame );
        void DeclareFrontLayerProgramDraws( RDG::PassBuilder& pass, WaterProgram& front, RDG::TextureRef waterInfo,
                                            RDG::TextureRef waves, const SceneViewInputs& view,
                                            const SceneFrameBinding& frame );
        // The draws of m_FrameDraws of template @p programSlot with @p program, through the pass's binding block
        // @p blockIndex (DeclareDraws declares one block per template), inside the node's open render pass.
        [[nodiscard]] Common::BoolResultStr RecordDraws( const RDG::PassContext& context,
                                                         const WaterProgram& program, uint32_t programSlot,
                                                         uint32_t blockIndex );
        // The pipeline of @p shader into @p program; the build's refusal otherwise.
        [[nodiscard]] Common::BoolResultStr BuildProgram( const std::shared_ptr<Shader>& shader,
                                                          const std::string& debugName, WaterProgram& program );
        // Folds @p material's values over its program's schema into the program's WaterMaterialUB and ripple maps;
        // the fold is cached in WaterProgram::Folded, so a steady frame resolves nothing and allocates nothing.
        // The row is appended to @p rows (the Materials block holds one row per body material, ApplyMaterials).
        void ApplyMaterial( const Assets::AssetHandle& material, const MaterialStamp& stamp, WaterProgram& program,
                            std::vector<glm::vec4>& rows );
        // Every row of @p materials (FrameProgram::Materials) into @p program's Materials block, in order (the
        // tiles' MaterialIndex).
        void ApplyMaterials( WaterProgram& program, const std::vector<Assets::AssetHandle>& materials );
        // @p program's WaterProgram::Blocks, re-resolved by name only when its cell's layout stamp or executor
        // moved (a missing block is reported by Material::Get once per resolve, not per frame).
        static const WaterProgram::CellBlocks& BlocksOf( WaterProgram& program );

        // WARNINGS ONCE PER CAUSE: each (material, cause) is said once. FirstWarning asks (and marks) before any
        // message is built, so a cause that recurs every frame builds no string after its first.
        enum class WaterWarning : uint8_t
        {
            NoTemplate,
            NoWaterCell,
            TemplateDoesNotBuild,
            NoVirtualLocalPermutation,
            VirtualLocalDoesNotBuild,
            NoFrontLayerCell,
            FrontLayerDoesNotBuild,
            MixedWaves,
            NoWavesService,
            WavesRefused,
            TooManyWaves,
            SpectrumRefused,
            FFTPipelinesMissing,
            SpectrumMalformed,
        };
        [[nodiscard]] bool FirstWarning( const Assets::AssetHandle& material, WaterWarning cause );
        void WarnOnce( const Assets::AssetHandle& material, WaterWarning cause, std::string_view why );
        static void Warn( const Assets::AssetHandle& material, std::string_view why );

        std::unordered_map<std::string, WaterProgram> m_Programs;        // by template compile name
        // FinishPrepare ran for this frame's TileWalk::Programs (their folds and rows are written): the programs
        // draw. m_FrameFrontLayers: how many of them have a front layer cell (null FrameProgram::FrontLayer:
        // none).
        bool                                          m_FrameReady       = false;
        uint32_t                                      m_FrameFrontLayers = 0u;
        ShaderBindingLayoutCache                      m_Layout;
        std::unordered_set<uint64_t>                  m_Warned; // FirstWarning's (material, cause) keys
        // ProgramFor's answers (UE: each proxy's cached FMaterial), one per body material, while the stamp stands
        // (a moved stamp drops them all): a frame of several templates resolves each body's once.
        struct ResolvedProgram
        {
            uint64_t      Material = 0;
            MaterialStamp Stamp;
            WaterProgram* Program = nullptr; // m_Programs' entries are never erased
        };
        std::vector<ResolvedProgram> m_Resolved;

        // The WaterInfo ground uploaded last (WaterZoneDraw::Ground) and its texture.
        std::shared_ptr<const Water::WaterInfoGround> m_GroundBaked;
        // The rivers whose flow the uploaded WaterInfo texture carries (TileWalk::Flows when it was composed).
        std::vector<std::shared_ptr<const Water::RiverFlowField>> m_FlowsBaked;
        std::shared_ptr<Image2D>                      m_WaterInfo;

        // THE FFT OCEAN'S STATE (WATER-RIVERS-FFT). The three compute programs, built on the first FFT frame (a
        // failure is said once and the water draws its Gerstner generator); one layout cache per program.
        [[nodiscard]] bool EnsureFFTPipelines();
        // The frame's spectrum and its waves lane, or null/false: the Gerstner rows (PrepareFrame).
        [[nodiscard]] bool PrepareFFTFrame( const Assets::AssetHandle& material, const Assets::AssetHandle& waves,
                                            glm::vec4& header, glm::vec4& fft, float& verticalExtent );
        std::shared_ptr<ComputePipeline> m_FFTSpectrum, m_FFTTransform, m_FFTFinalize;
        ShaderBindingLayoutCache         m_FFTSpectrumLayout, m_FFTTransformLayout, m_FFTFinalizeLayout;
        bool                             m_FFTTried = false;
        // This frame's spectrum (null: Gerstner), the one whose h0 is uploaded, its h0 image and the atlas.
        std::shared_ptr<const Water::FFTOceanSpectrum> m_FFTFrame;
        std::shared_ptr<const Water::FFTOceanSpectrum> m_FFTUploaded;
        std::shared_ptr<Image2D>                       m_FFTInitial;
        std::shared_ptr<Image2D>                       m_FFTWaves;
        // The atlas half (0 or 1) this frame writes, and the one the last recorded frame wrote (-1: none since the
        // spectrum changed - the previous frame is then this frame's own half, a zero velocity for one frame).
        int32_t m_FFTHalf        = 0;
        int32_t m_FFTWrittenHalf = -1;
        // The last flip (PrepareFFTFrame): the view time it was taken at and the half it named as the previous
        // frame's - a second view of the same time keeps both (R3 step 6: one flip per frame, not per view).
        int32_t m_FFTPreviousHalf = 0;
        double  m_FFTFlipTime     = 0.0;
        bool    m_FFTFlipped      = false;
        // The waves recorded into a graph this frame (RecordedWaves), reset by PrepareFrame.
        RDG::TextureRef     m_FrameWavesTexture;
        const RDG::Builder* m_FrameWavesGraph = nullptr;
        // The wave source last said in the log ([Water] waves=...), -1: nothing said yet.
        int32_t m_LoggedWaveSource = -1;

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
        // PER-TEMPLATE PROGRAMS (UE: each water body's own material instance on the one water mesh): one entry per
        // template the frame's bodies use, in first-use order; each draws its tiles through its own binding block.
        struct FrameProgram
        {
            WaterProgram* Base       = nullptr; // ProgramFor's (the identity of the template)
            WaterProgram* Program    = nullptr; // the cell that draws: Base, or its VSM twin under VirtualMaps
            WaterProgram* FrontLayer = nullptr; // FrontLayerProgramFor's, or null
            std::vector<Assets::AssetHandle> Materials; // its body materials: its Materials block, in order
        };
        struct TileWalk
        {
            std::vector<FrameProgram>                     Programs; // empty: no water, the task walks nothing
            MaterialStamp                                 Stamp;
            std::shared_ptr<const Water::WaterInfoGround> Ground;
            std::vector<std::shared_ptr<const Water::RiverFlowField>> Flows;      // the frame's rivers
            glm::vec2                                     InfoMin{ 0.0f };     // WaterInfo square
            float                                         InfoExtent = 0.0f;
            uint32_t                                                  InfoSize   = 0u;
            std::vector<std::vector<glm::vec4>>           Bodies;              // per zone, per body lane
            std::vector<std::vector<uint32_t>>            BodyPrograms; // per zone, per body: Programs slot
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
        std::vector<glm::vec4>       m_Rows; // material rows of every body material, one upload
    };
} // namespace Desert::Graphic::System
