#pragma once

#include <Engine/Graphic/Systems/Scene/SingleLayerWaterPass.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Water/Public/WaterQuadTree.hpp>
#include <Water/Public/WaterBodyQuery.hpp>
#include <Water/Public/FFTOceanSpectrum.hpp>
#include <Water/Public/WaterCaustics.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Water/Public/WaterZoneDraw.hpp>
#include <Water/Public/WaterShoreField.hpp>
#include <Water/Public/WaterFoamClipmap.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Graphic::RDG
{
    class Builder; // Engine/Graphic/RDG/RDGBuilder.hpp: AddFramePasses records into it
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
    class WaterRenderer final : public ISingleLayerWaterPass
    {
    public:
        using ISingleLayerWaterPass::ISingleLayerWaterPass;

        Common::BoolResultStr Initialize() override;

        // THE FRAME'S WATER IN THREE STEPS (FPS-ARCH-SETUP3, UE FSceneRenderer task-based view setup), before the
        // render graph records; nothing may draw the water this frame before FinishPrepare ran.
        // PrepareFrame (render thread): the frame's material and program, its waves block (written into the
        // upload's head), the camera's matrices and lens scale.
        void PrepareFrame() override;
        // PrepareTiles (a setup task, or inline): every zone's retained quadtree walked from the camera
        // PrepareFrame took, its tiles appended to the upload as rows and grouped into the frame's draws. Touches
        // only the zone trees, the traversal scratch, the upload's rows and the draws - no service, no GPU object.
        void PrepareTiles() noexcept override;
        // FinishPrepare (render thread, after the task joined): the material's fold, the rivers' image and the
        // rows' upload; the frame's program is set only here.
        void FinishPrepare() override;

        [[nodiscard]] bool HasDraws() const override
        {
            return !m_FrameDraws.empty();
        }

        // Whether this frame's water program samples @p textureName (a scene/view input by its shader name): the
        // shadow gates' question (SceneRenderer ShadowMapReadersOutsideMeshes), answered after FinishPrepare. The
        // water reads the sun's cascades and the point/spot atlas wherever it draws - it is in no opaque depth a
        // screen shadow mask could serve it from.
        [[nodiscard]] bool FrameSamples( std::string_view textureName ) const override;

        // The frame's rivers (FinishPrepare keeps the image).
        [[nodiscard]] const std::shared_ptr<Image2D>& GetRiverFlow() const override
        {
            return m_RiverImage;
        }

        // WATER-CAUSTICS (WaterCaustics.hpp): this frame's waves block - the upload's head, WaterMeshTiles.glslh
        // WaterTiles' leading members (the header rows and every wave set of the frame) - and every body that
        // draws this frame (its footprint and still level) with its wave lane (WaterTileRow.Body.z: 1 + its set in
        // WaterWaveSets, 0 = flat; parallel to GetFrameBodies), after PrepareFrame; all empty when no water draws.
        static constexpr std::size_t kWavesBlockBytes = ( 17 + 4 * ( 2 + 2 * 32 ) ) * 4 * sizeof( float );
        [[nodiscard]] std::span<const std::byte> GetFrameWavesBlock() const
        {
            if ( m_Walk.Programs.empty() || m_Upload.size() < kWavesBlockBytes )
                return {};
            return std::span<const std::byte>( m_Upload.data(), kWavesBlockBytes );
        }
        [[nodiscard]] std::span<const Water::WaterQuadTreeBody> GetFrameBodies() const
        {
            if ( m_Walk.Programs.empty() )
                return {};
            return m_FrameBodies;
        }
        [[nodiscard]] std::span<const float> GetFrameBodyWaveLanes() const
        {
            if ( m_Walk.Programs.empty() )
                return {};
            return m_FrameBodyWaveLanes;
        }
        // Its material's caustics: x = onset depth, y = fade depth (cm), z = strength; parallel to GetFrameBodies.
        [[nodiscard]] std::span<const glm::vec3> GetFrameBodyCaustics() const
        {
            if ( m_Walk.Programs.empty() )
                return {};
            return m_FrameBodyCaustics;
        }
        // THE FFT OCEAN (Tessendorf 2001): when a body of the frame names a wave set authored as an FFT ocean and
        // Effects.WaterWaveMethod resolves to FFT, PrepareFrame sets the frame's spectrum, uploading its h0 cascades
        // once per set.
        [[nodiscard]] bool FrameWavesAreFFT() const override
        {
            return m_FFTFrame != nullptr && !m_Walk.Programs.empty();
        }
        // The h0 cascades of the frame's spectrum: kFFTOceanResolution wide RGBA32F, cascade c in rows
        // [N c, N c + N) (Programs/Water/WaterFFTSpectrum.shader).
        [[nodiscard]] const std::shared_ptr<Image2D>& GetFFTInitialSpectrum() const override
        {
            return m_FFTInitial;
        }
        // The foam atlases (WaterFoamClipmap.hpp): the one this frame reads and the one it writes; null: no foam.
        [[nodiscard]] const std::shared_ptr<Image2D>& GetFoamHistory() const override
        {
            return m_Foam[m_FoamWritten];
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetFoamTarget() const override
        {
            return m_Foam[m_FoamWritten ^ 1u];
        }
        [[nodiscard]] ShoreFoamUniforms GetShoreFoamUniforms() const override;
        // The frame's water, once AddFramePasses recorded it into @p graph; invalid otherwise.
        [[nodiscard]] WaterFrameRefs RecordedFrame( const RDG::Builder& graph ) const override
        {
            return m_FrameWaterGraph == &graph ? m_FrameWaterRefs : WaterFrameRefs{};
        }
        // The ocean at the view's time, its mips and the foam (WaterWaveSimulation.cpp, WaterFoamSimulation.cpp).
        // Recorded once per graph; invalid when a program or an import is missing (said once).
        WaterFrameRefs AddFramePasses( RDG::Builder& graph, const WaterFrameImports& imports,
                                       float timeSeconds ) override;

        // SETUP of the water node: its one binding block - the material route (frame block, tile rows, material
        // values and ripple maps), the opaque scene's copy @p sceneCopy the surface refracts, and the scene/view
        // inputs of @p view its program has slots for (the opaque depth = the water's bottom, the sky cubes, the
        // view's fog), and the frame's water @p water (AddFramePasses).
        // @p frontReflection / @p frontDepth: this frame's front layer reflection and the depth it belongs to
        // (SceneRenderer::FrameFrontLayer; the graph's black texture for both when no front layer was traced),
        // bound as FrontLayerReflectionTexture / FrontLayerDepthTexture where the cell has the slots
        // (Mesh/Surface/FrontLayerReflection.glslh: the water's traced reflection where it IS the front layer).
        void DeclareDraws( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, const WaterFrameRefs& water,
                           RDG::TextureRef frontReflection, RDG::TextureRef frontDepth,
                           const SceneViewInputs& view, const SceneFrameBinding& frame ) override;
        [[nodiscard]] Common::BoolResultStr RenderManual( const RDG::PassContext& context ) override;

        // THE WATER'S FRONT LAYER (UE: single layer water writes the G-buffer its SSR / Lumen reflections trace
        // from): this frame's `WaterMesh.FrontLayer` cell over the same tile rows, drawn into the translucent
        // front layer's targets ("Translucency: Front Layer G-buffer (Water)",
        // SceneRenderer::AddFrameFrontLayerSurface). True after PrepareFrame when the water draws and its
        // template's front layer cell built.
        [[nodiscard]] bool HasFrontLayerDraws() const override
        {
            return !m_FrameDraws.empty() && m_FrameFrontLayers > 0u;
        }
        // SETUP of the front layer node: the cell's material route (frame block, tile rows, material row and
        // ripple maps), the scene/view inputs it has slots for and the frame's water.
        void DeclareFrontLayerDraws( RDG::PassBuilder& pass, const WaterFrameRefs& water,
                                     const SceneViewInputs& view, const SceneFrameBinding& frame ) override;
        [[nodiscard]] Common::BoolResultStr RenderFrontLayerManual( const RDG::PassContext& context ) override;

        // THE UNDERWATER VOLUME (WATER-UNDERWATER; UE Water: the body's underwater post process volume, its
        // material reading the body's own scattering / absorption). PrepareFrame finds the body the camera is
        // under (the waves' surface at the camera's (X, Z) above the camera, or within the near plane's reach
        // below it: the waterline crossing) and reads the medium from that body's `.demat` (its
        // ScatteringCoefficients / AbsorptionCoefficients / PhaseG, the values its surface's volume uses). True
        // after PrepareFrame when such a body was found - whether or not any tile of it is in view.
        [[nodiscard]] bool HasUnderwater() const override
        {
            return m_Underwater.Active;
        }
        // SETUP of the underwater node ("Deferred: Underwater", drawn on the scene target BEFORE the water
        // surface): the opaque scene's copy @p sceneCopy (the target as it is: no feedback), the G-buffer depth
        // and the sky's diffuse cube of @p view, and @p uniforms (UploadUnderwater) as UnderwaterUB.
        [[nodiscard]] RDG::BufferRef UploadUnderwater( RDG::Builder&            graph,
                                                       const SceneFrameBinding& frame ) override;
        void DeclareUnderwater( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, const SceneViewInputs& view,
                                RDG::BufferRef uniforms ) override;
        [[nodiscard]] Common::BoolResultStr RenderUnderwaterManual( const RDG::PassContext& context ) override;

        // THE CAUSTICS (WaterCaustics.hpp), this module's own system, made and initialised with the pass
        // (Initialize; a caustics program that does not build leaves the water drawing without them, said once).
        void SetCausticsEnabled( bool enabled ) noexcept override
        {
            m_CausticsEnabled = enabled;
            if ( m_Caustics )
                m_Caustics->SetEnabled( enabled );
        }
        [[nodiscard]] bool CausticsEnabled() const noexcept override
        {
            return m_Caustics != nullptr && m_Caustics->IsEnabled();
        }
        [[nodiscard]] bool FrameHasWaves() const override
        {
            return !GetFrameWavesBlock().empty();
        }
        [[nodiscard]] std::optional<CausticsFrame> AddCausticsPasses( RDG::Builder&         graph,
                                                                      const CausticsInputs& inputs ) override;

        // By reference, as GrassRenderer::Submit: @p zone is the recorded command's, held until ClearQueue.
        void Submit( WaterZoneDraw&& ) = delete; // the queue keeps a reference: a temporary would dangle
        void Submit( const WaterZoneDraw& zone )
        {
            m_Queue.emplace_back( zone );
        }

        void ClearQueue() override
        {
            m_Queue.clear();
            m_FrameDraws.clear();
        }

    private:
        std::unique_ptr<WaterCaustics> m_Caustics; // null: its programs did not build
        bool                            m_CausticsEnabled = false;

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
            const void* Service                                  = nullptr;
            uint32_t    InvalidationVersion                      = 0;
            uint32_t    ValuesEditSerial                         = 0;
            uint64_t    ContentSerial                            = 0;
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
            // The template file the cells belong to (set by ProgramFor): every cell is asked of the shader service
            // as that file's own (GetByNameOwnedBy), never by the bare name.
            Assets::AssetHandle Template = Assets::AssetHandle::Null();
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
                                  const WaterFrameRefs& water, RDG::TextureRef frontReflection, RDG::TextureRef frontDepth,
                                  const SceneViewInputs& view, const SceneFrameBinding& frame );
        void DeclareFrontLayerProgramDraws( RDG::PassBuilder& pass, WaterProgram& front, const WaterFrameRefs& water,
                                            const SceneViewInputs& view,
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
            TooManyWaves,
            FFTPipelinesMissing,
            FFTReadbackUnreadable,
            UnderwaterMediumMissing,
            UnderwaterPipelineMissing,
            FoamMissing,
            CausticsMissing,
        };
        [[nodiscard]] bool FirstWarning( const Assets::AssetHandle& material, WaterWarning cause );
        void        WarnOnce( const Assets::AssetHandle& material, WaterWarning cause, std::string_view why );
        static void Warn( const Assets::AssetHandle& material, std::string_view why );

        std::unordered_map<Assets::AssetHandle, WaterProgram> m_Programs; // by template file
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

        // THE SHORE FIELD'S MAP (WaterShoreUpload.cpp): the pooled texture, and the revision of the bake it holds,
        // written by the upload that completed (0: none).
        [[nodiscard]] RDG::TextureRef          AddShorePass( RDG::Builder& graph );
        std::shared_ptr<ComputePipeline>       m_ShoreUpload;
        ShaderBindingLayoutCache               m_ShoreUploadLayout;
        bool                                   m_ShoreUploadTried = false;
        RDG::ExternalTexture                   m_ShoreImage;
        std::shared_ptr<std::atomic<uint64_t>> m_ShoreHolds;
        // The rivers whose flow the river image carries (TileWalk::Flows when it was composed), and the image.
        std::vector<std::shared_ptr<const Water::RiverFlowField>> m_FlowsBaked;
        std::shared_ptr<Image2D>                                  m_RiverImage;

        // THE FFT OCEAN'S STATE. The compute programs, built on the first FFT frame (a failure is said once and the
        // water draws its Gerstner generator); one layout cache per program.
        [[nodiscard]] std::shared_ptr<ComputePipeline>
             BuildWavePipeline( const char* name, ShaderBindingLayoutCache& layout,
                                std::span<const RDG::ShaderParameter> parameters );
        [[nodiscard]] bool EnsureFFTPipelines();
        // Makes what drawing @p spectrum takes (programs, the uploaded spectrum); whether this renderer can.
        [[nodiscard]] bool PrepareFFTFrame( const Assets::AssetHandle&                            waves,
                                            const std::shared_ptr<const Water::FFTOceanSpectrum>& spectrum );
        [[nodiscard]] RDG::TextureRef AddWavePasses( RDG::Builder& graph, RDG::TextureRef initial, float timeSeconds );
        [[nodiscard]] RDG::TextureRef AddStillWaves( RDG::Builder& graph );
        std::shared_ptr<ComputePipeline> m_FFTSpectrum, m_FFTTransform, m_FFTFinalize, m_FFTMips, m_WavesStill;
        ShaderBindingLayoutCache         m_FFTSpectrumLayout, m_FFTTransformLayout, m_FFTFinalizeLayout,
             m_FFTMipsLayout, m_WavesStillLayout;
        bool m_FFTTried        = false;
        bool m_WavesStillTried = false;
        // This frame's spectrum (null: Gerstner), the one whose h0 is uploaded and its h0 image.
        std::shared_ptr<const Water::FFTOceanSpectrum> m_FFTFrame;
        std::shared_ptr<Water::WaterFFTSurface>        m_FFTSurface; // the frame's world's, beside m_FFTFrame
        std::shared_ptr<const Water::FFTOceanSpectrum> m_FFTUploaded;
        std::shared_ptr<Image2D>                       m_FFTInitial;
        // The waves' array (Water/WaterFFT.glslh), and 1 + the frame whose graph completed it last with the uploaded
        // sea (0: none); made with the sea.
        RDG::ExternalTexture                   m_WavesImage;
        std::shared_ptr<std::atomic<uint64_t>> m_WavesHold;

        // THE FOAM (WaterFoamSimulation.cpp). The frame's parameters, set by PrepareFoam when the foam runs.
        // One body's foam (Programs/Water/WaterFoamUpdate.shader WaterFoamBody, std430): each texel of the map
        // follows the body whose footprint holds it.
        struct FoamBody
        {
            glm::vec4 Footprint{ 0.0f }; // min X, min Z, max X, max Z
            glm::vec4 Level{ 0.0f };     // x = still Y, y = its wave set (-1: none), z = 1 + its river of the frame, w = its beach's slope
            glm::vec4 Source{ 0.0f };    // x = jacobian threshold, y = softness, z = whitecap gain
            glm::vec4 Decay{ 0.0f };     // x = foam, y = bubbles, z = wetness (s)
            glm::vec4 Drift{ 0.0f };     // xy = the wind's surface drift (cm/s)
        };
        // One fall's plunge (WaterFoamUpdate.shader WaterFoamPlunge, std430; Water::FallPlunge): the white boil the
        // fall lands in and the outflow that spreads its foam.
        struct FoamPlunge
        {
            glm::vec4 FootRadius{ 0.0f }; // xyz = Foot, w = Radius (cm)
            glm::vec4 AcrossHalf{ 0.0f }; // xy = Across.xz, z = HalfWidth (cm), w = OutflowSpeed (cm/s)
            glm::vec4 Landing{ 0.0f };    // xy = Downstream, z = LandingSpeed (cm/s), w = 0
        };
        struct FoamFrame
        {
            std::array<glm::ivec2, Water::kFoamLevels> Origin{};
            std::vector<FoamBody>                      Bodies;
            // Every plunge of the frame's rivers whose boil reaches the outer window (no cap).
            std::vector<FoamPlunge> Plunges;
        };
        // Places the windows around @p eye, reads every body's foam values and writes the waves block's foam lanes.
        // @p fftSet: the wave set the FFT ocean draws (its wind drifts the foam), -1 none. @p rivers: the frame's
        // river bakes, whose falls' plunges boil in the foam.
        void PrepareFoam( const MaterialStamp& stamp, const glm::vec3& eye, int32_t fftSet,
                          const std::shared_ptr<const Water::WaterShoreField>&           shore,
                          std::span<const std::shared_ptr<const Water::RiverFlowField>> rivers,
                          std::array<glm::vec4, 3>& origin, glm::vec4& shape );
        // What the passes read of a body's material (its look is the surface shader's own).
        struct BodyMaterialValues
        {
            Water::WaterFoamParams Foam;
            glm::vec3              Caustics{ 1.0f, 1.0f, 0.0f }; // a template that states none has none
        };
        // Read again only when the stamp moved; valid until the next call.
        [[nodiscard]] const BodyMaterialValues& MaterialValuesOf( const Assets::AssetHandle& material,
                                                                  const MaterialStamp&       stamp );
        [[nodiscard]] bool EnsureFoam( uint32_t side );
        [[nodiscard]] RDG::TextureRef AddFoamPass( RDG::Builder& graph, const WaterFrameRefs& water,
                                                   const WaterFrameImports& imports, float timeSeconds );
        std::optional<FoamFrame>                 m_FoamFrame;
        Water::WaterFoamClipmapState             m_FoamState;
        std::array<std::shared_ptr<Image2D>, 2>  m_Foam;
        uint32_t                                 m_FoamWritten = 0;
        uint32_t                                 m_FoamSide    = 0;
        // An image replaced while frames in flight still read it.
        void RetireImage( std::shared_ptr<Image2D> image );
        Assets::FrameRetireQueue<std::shared_ptr<Image2D>> m_RetiredImages;
        std::shared_ptr<GraphicsPipeline>        m_FoamPipeline;
        ShaderBindingLayoutCache                 m_FoamLayout;
        // What this frame's pass wrote: the history from the next PrepareFoam on.
        std::optional<std::array<glm::ivec2, Water::kFoamLevels>> m_FoamRecorded;
        float                                    m_FoamRecordedTime = 0.0f;
        float                                    m_FoamTime         = 0.0f; // the history's
        uint64_t                                 m_FoamShore        = 0;    // the floor it lies on: its revision
        // The rectangle of the world (min X, min Z, max X, max Z) whose floor moved under the history.
        std::optional<glm::vec4>                 m_FoamChanged;
        bool                                     m_FoamTried = false;
        // The materials read since the stamp moved.
        MaterialStamp                                    m_BodyMaterialStamp;
        std::unordered_map<uint64_t, BodyMaterialValues> m_BodyMaterials;
        // THE CPU MIRROR's read-back (WATER-RIVERS-FFT pass 7; Water::FFTSurfaceMirror): one host-readable buffer
        // per frame in flight the finalize pass fills with every kFFTReadbackStride-th displacement texel, read
        // when its slot comes round again (the frame start waited that slot's fence: no GPU wait), with the wave
        // time and the sea it was written for. Published to the world's WaterFFTSurface.
        struct FFTReadbackSlot
        {
            RDG::ExternalBuffer                            Buffer;
            std::shared_ptr<const Water::FFTOceanSpectrum> Spectrum; // null: nothing extracted into this slot
            std::shared_ptr<Water::WaterFFTSurface>        Surface;  // the world it is of
            double                                         Time  = 0.0;
            uint64_t                                       Frame = 0; // the frame whose graph extracted into it
        };
        std::vector<FFTReadbackSlot> m_FFTReadback;
        // Reads the slot this frame reuses and publishes it (AddWavePasses, before the slot is extracted into).
        void PublishFFTReadback( uint32_t slot );
        // The frame's water recorded into a graph (RecordedFrame), reset by PrepareFrame.
        WaterFrameRefs      m_FrameWaterRefs;
        const RDG::Builder* m_FrameWaterGraph = nullptr;
        // The wave source last said in the log ([Water] waves=...), -1: nothing said yet.
        int32_t m_LoggedWaveSource = -1;

        std::vector<std::reference_wrapper<const WaterZoneDraw>> m_Queue;
        std::vector<FrameDraw>                                   m_FrameDraws;

        // THE UNDERWATER VOLUME of this frame (PrepareUnderwater; HasUnderwater): the body the camera is under,
        // its surface height at the camera and its medium (1/m), the view's pre-exposure and jittered inverse.
        struct UnderwaterFrame
        {
            bool      Active   = false;
            float     SurfaceY = 0.0f;
            glm::vec3 Camera{ 0.0f };
            glm::vec3 Scattering{ 0.0f };
            glm::vec3 Absorption{ 0.0f };
            float     PhaseG      = 0.0f;
            float     PreExposure = 1.0f;
            glm::mat4 InvViewProjection{ 1.0f };
            // The frame's sun and sky look, taken by DeclareUnderwater (the frame binding the surface reads).
            glm::vec4 ToSun{ 0.0f, 1.0f, 0.0f, 0.0f };
            glm::vec4 SunRadiance{ 0.0f };
            glm::vec4 SkyYawCosSin{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec4 SkyGain{ 1.0f };
        };
        UnderwaterFrame m_Underwater;
        // Each body the camera is over, as it last answered ready: its waves' height over its plane, kept while it
        // answers not ready (its read-back has not landed). The record's Surface names the world.
        struct SurfaceHold
        {
            std::shared_ptr<const Water::WaterFFTSurface> Surface;
            uint32_t                                      Body       = 0;    // WaterBodyState::Id
            float                                         WaveHeight = 0.0f; // cm
        };
        std::vector<SurfaceHold> m_SurfaceHolds;
        std::vector<SurfaceHold> m_LastSurfaceHolds; // the last frame's
        // The medium read from a body `.demat` (its named values), re-read only when that material or the
        // material stamp moved (UE: the underwater post process MID's parameters copied from the water MID).
        struct UnderwaterMedium
        {
            uint64_t      Material = 0;
            MaterialStamp Stamp;
            bool          Readable = false;
            glm::vec3     Scattering{ 0.0f };
            glm::vec3     Absorption{ 0.0f };
            float         PhaseG = 0.0f;
        };
        UnderwaterMedium                  m_UnderwaterMedium;
        std::shared_ptr<GraphicsPipeline> m_UnderwaterPipeline;
        bool                              m_UnderwaterPipelineTried = false;
        void                              PrepareUnderwater( const glm::vec3& camera, const MaterialStamp& stamp );
        [[nodiscard]] bool ReadUnderwaterMedium( const Assets::AssetHandle& material, const MaterialStamp& stamp );
        // THE NAMED NUMERIC VALUES of @p material as its template draws them: each name's schema default (the
        // template ProgramFor builds for it), then the `.demat` chain's stated values over it (ResolveOverrides
        // returns only what the files state). False when the material resolves to no template, or a name is not a
        // parameter of it; @p out is written in @p names' order.
        [[nodiscard]] bool ReadMaterialValues( const Assets::AssetHandle& material, const MaterialStamp& stamp,
                                               std::span<const std::string_view> names, std::span<glm::vec4> out );

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
        // One body's record on the GPU (UE FWaterBodyRenderData, indexed by the tile's WaterBodyIndex):
        // PrepareFrame builds it once per body from WaterZoneBodies, the tile walk copies it onto each of the
        // body's tiles (WaterMeshTiles.glslh WaterTileRow.Body / .Medium), so two bodies of one frame never share
        // a value.
        struct WaterBodyLane
        {
            // x = its material's row (-1 = not drawn), y = 1 for a river, z = 1 + its wave set, w = its index
            glm::vec4 Body{ -1.0f, 0.0f, 0.0f, 0.0f };
            // y = its river's two-phase flow period (s; 0 off a river); a river's x = its mouth's radius (cm, 0 = no
            // receiver's waves) and zw = the mouth's world (X, Z) (Water::RiverMouth); a fall's row's zw = its speeds
            glm::vec4 Medium{ 0.0f };
        };
        struct TileWalk
        {
            std::vector<FrameProgram>                     Programs; // empty: no water, the task walks nothing
            MaterialStamp                                 Stamp;
            std::shared_ptr<const Water::WaterShoreField> Shore;
            // The frame's rivers (the river image is laid out from them).
            std::vector<std::shared_ptr<const Water::RiverFlowField>> Flows;
            std::vector<std::vector<WaterBodyLane>>                   Bodies; // per zone, per body lane
            std::vector<std::vector<uint32_t>>            BodyPrograms;       // per zone, per body: Programs slot
            glm::mat4                                     View{ 1.0f };
            glm::mat4                                     Projection{ 1.0f };
            glm::vec3                                     Observer{ 0.0f };
            float                                         LensScale      = 1.0f;
            float                                         VerticalExtent = 0.0f;
            uint32_t                                      Rows           = 0;
        };
        TileWalk m_Walk;
        // Per-view scratch the frame reuses, so a steady frame allocates nothing: the traversal's tiles, the
        // upload (the waves block, then the tile rows). RecordDraws' push block is NOT here: the surface and the
        // front layer record on two graph chunks at once (a thread's scratch, WaterRenderer.cpp).
        Water::WaterTraversalBuffers m_Traversal;
        std::vector<std::byte>                m_Upload;
        std::vector<glm::vec4>                m_Rows;        // material rows of every body material, one upload
        std::vector<Water::WaterQuadTreeBody> m_FrameBodies; // every body drawn this frame (GetFrameBodies)
        std::vector<float>                    m_FrameBodyWaveLanes; // its wave lane (GetFrameBodyWaveLanes)
        struct FrameBodyLook
        {
            Assets::AssetHandle Material;
            int32_t             River = -1; // its index in the frame's rivers, -1: not a river, -2: a river with no bake
        };
        std::vector<FrameBodyLook> m_FrameBodyLooks;    // parallel to m_FrameBodies
        std::vector<glm::vec3>     m_FrameBodyCaustics; // its material's (GetFrameBodyCaustics)
    };
} // namespace Desert::Graphic::System
