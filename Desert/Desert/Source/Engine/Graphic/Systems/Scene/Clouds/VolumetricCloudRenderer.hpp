#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>
#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudMediumValues.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>
#include <Engine/Graphic/Clouds/CloudQuality.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/Clouds/CloudSkyOcclusionPayload.hpp>
#include <Engine/Graphic/Materials/Clouds/MaterialCloudComposite.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Desert::Graphic::System
{
    /**
     * @brief The volumetric cloud pass: a per-pixel march through a spherical shell around the planet,
     *        composited over the scene at sky distance.
     *
     * Three stages. There used to be a fourth in front of them — a compute bake that filled the noise
     * volume from a seed — and it is gone: the volume is an ASSET now (Engine/Assets/CloudNoiseVolume.hpp),
     * generated offline and uploaded once by Runtime::CloudNoiseService, because a volume that only ever
     * existed on the GPU could not be saved, shown, or replaced by one the artist made.
     *
     *
     *   S1  MARCH      compute, RGBA16F at a QUARTER of the target's size. Reconstructs each pixel's ray
     *                  from the camera's inverse view-projection — through the HALF-resolution pixel this
     *                  frame's sub-pixel offset owns, which is the projection jitter — intersects the
     *                  shell, cuts the segment at the scene depth (presented by ComputeImageBeginRead —
     *                  the one path that works in both Forward and Deferred), and integrates.
     *                  Premultiplied radiance in .rgb, transmittance in .a.
     *   S2  RESOLVE    compute, RGBA16F at HALF the target's size. Unreal's VolumetricRenderTarget mode 0
     *                  reconstruction: a half-res pixel this frame traced is taken exactly, and one it did
     *                  not is reprojected from the previous frame's reconstruction through the guide's
     *                  cloud front distance, validated, and blended. Ping-ponged between two history
     *                  targets because it reads last frame's result while writing this frame's.
     *   SM  SHADOW MAP compute, RGBA32F, 512x512 at the reference quality tier and scaled with it,
     *                  marched down the SUN's direction rather than the eye's,
     *                  and issued EARLY — before the render graph, because the deferred lighting pass
     *                  reads it. Each texel holds (frontDepthKm, meanExtinctionPerKm, maxOpticalDepth),
     *                  the triple that reconstructs a correct transmittance for a receiver at any depth
     *                  inside or below the layer with one fetch. See Common/CloudShadowMap.glslh for the
     *                  encoding and Engine/Graphic/Clouds/CloudShadowPayload.hpp for the projection.
     *                  Independent of S1 and S2: it needs no scene depth and no view.
     *   S3  COMPOSITE  a fullscreen quad registered in RenderPhase::Transparency at
     *                  RenderPassOrder::FarField — ABOVE the height fog and BELOW everything else the
     *                  phase composites, so particles land over the clouds rather than under them. It
     *                  upsamples the HALF-resolution reconstruction, unchanged by mode 0.
     *
     * WHERE IT RUNS. S0, S1 and S2 are in-frame compute dispatches and must be issued OUTSIDE an open
     * render pass, after the scene depth is finished. SceneRenderer::ExecuteVolumetricClouds() calls
     * ExecuteInFrame() immediately after ExecuteAtmosphericFog(), so the depth a ray is cut at and the
     * atmosphere a cloud is lit by both belong to the camera this frame was drawn with.
     *
     * PER-VIEW STATE, AND WHY IT IS SAFE. The history targets, the frame counter and the previous frame's
     * view-projection are MEMBERS OF THIS OBJECT, and SceneRenderer constructs one VolumetricCloudRenderer
     * per renderer. That is strictly stronger than the per (frame x renderer slot) rule in
     * Docs/RENDERER_FRAME_STATE.md: an asset thumbnail's renderer cannot see the viewport's history
     * because it does not own it. The one piece of state that lives in the shared shader-resource layer is
     * the parameter buffer, which is created NON-PERSISTENT and therefore already carries a copy per
     * (frame x slot).
     *
     * ZERO COST WHEN ABSENT. A scene with no cloud component, or the clouds disabled, or no atmosphere to
     * light them, dispatches nothing, allocates nothing and composites nothing: the frame is bit for bit
     * what it was before this system existed. That is checked before any allocation rather than after,
     * because the editor builds a SceneRenderer for every asset thumbnail and every mesh preview, and
     * none of them has a sky.
     */
    class VolumetricCloudRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~VolumetricCloudRenderer() override;

        Common::BoolResultStr Initialize() override;
        void                  RegisterPasses( RenderGraphBuilder& builder ) override;

        // Drops the temporal reconstruction's history — see IRenderSystem::OnSceneReplaced, kind 1. Every
        // other cache in this system is content-keyed already (the modelling volume and the noise bakes
        // compare their authored parameters, m_Present is pushed explicitly including the absent case), so
        // the history is the whole of what a scene change invalidates here. It is otherwise dropped only on
        // a resize; without this, the first executed cloud frame of the new scene reprojects the old one.
        void OnSceneReplaced() override
        {
            m_HistoryValid = false;
        }

        /**
         * @brief This frame's cloud layer, from ECS::VolumetricCloudECSSystem.
         *
         * @param present    false when the scene has no cloud component at all. Said explicitly rather
         *                   than by omission — the renderer keeps its state across frames, and a component
         *                   deleted mid-session must take its clouds with it.
         * @param windOffset the drift the ECS accumulated, world units. Owned there because that is where
         *                   the timestep is.
         * @param heroClouds this frame's sculpted bodies — slot A of the seam. Empty is the ordinary case
         *                   and means the march's authored loop does not run at all.
         * @param quality    this machine's cloud quality tier, taken from Common::Settings::MachineSettings each
         *                   BeginScene like every other cost-versus-quality choice. It arrives HERE rather
         *                   than being read from a global because this renderer is one of several live at
         *                   once (Docs/RENDERER_FRAME_STATE.md) and a tier is per-view state like any
         *                   other.
         */
        void SetCloudSettings( bool present, const ECS::VolumetricCloudData& data, const glm::vec3& windOffset,
                               Common::Settings::CloudQuality        quality,
                               const std::vector<HeroCloudInstance>& heroClouds );

        /**
         * @brief Stages S0 and S1. Must be called outside any render pass, after the scene depth is final.
         */
        void ExecuteInFrame();

        /**
         * @brief Stage SM — the cloud shadow map. Must be called outside any render pass, and EARLY:
         *        before the render graph records and before the deferred lighting pass, both of which
         *        read the result. It depends on nothing that the frame produces — no scene depth, no
         *        atmosphere LUT — only on the field, the sun and the camera position, so nothing forces
         *        it later and the one thing that forces it earlier is its consumer.
         *
         * Costs exactly nothing when the layer is absent, disabled, not casting or at zero strength: the
         * image is not even allocated until all four are true.
         */
        void ExecuteShadowMapInFrame();

        /// True when a shadow map was produced for THIS frame and may be sampled. False makes every
        /// consumer fall back to "no cloud shadow" rather than to a map from a frame the sun has since
        /// left.
        bool HasShadowMap() const
        {
            return m_ShadowMapValid;
        }

        /// The map itself. Null unless HasShadowMap(). Borrowed — this renderer owns it.
        Image2D* GetShadowMapImage() const
        {
            return m_ShadowMapImage.get();
        }

        /// This frame's projection and its far depth, for a consumer that has to transform a world
        /// position into the map. Meaningless unless HasShadowMap().
        const CloudShadowMapView& GetShadowMapView() const
        {
            return m_ShadowMapView;
        }

        /// The artist's shadow strength, applied by the CONSUMER rather than baked into the map — see
        /// Common/CloudShadowMap.glslh. Zero when the layer is not casting at all, so a consumer that
        /// only reads this number still gets the right answer.
        float GetShadowStrength() const;

        /**
         * @brief This view's cloud field, prepared for the sky's environment bake.
         *
         * The panorama the IBL chain is built from is marched with the SAME field, the SAME lighting and
         * the SAME quadrature the screen pass uses, so it is this object that has to answer — the
         * parameters, the modelling volume and the noise volumes are its, and SkyboxRenderer owns none of
         * them. See Engine/Graphic/Clouds/CloudEnvironmentBake.hpp for the seam.
         *
         * CALLED OUTSIDE THE FRAME, from SceneRenderer::OnUpdate, before anything this class dispatches.
         * It therefore does the same three steps every dispatch here does — resolve the species, guarantee
         * the volumes they name, pack the block — through the one function all of them share, and blocks
         * on the first modelling bake exactly as the frame does.
         *
         * @return a result whose Marched is false when this view has no clouds to bake: no component, the
         *         layer off, no atmosphere to light it, or a resource that could not be built. The
         *         panorama is then the sky alone, and the caller must still bind fallbacks for every
         *         descriptor the bake declares.
         */
        CloudEnvironmentBake BuildEnvironmentBake();

        /**
         * @brief Is a modelling-volume bake in flight for this view right now?
         *
         * WHAT IT IS FOR, AND IT IS NOT DIAGNOSTICS. About half of a cloud material's parameters are inputs
         * to a bake of a 256 x 32 x 256 volume out of a few thousand bodies, and moving one of those costs
         * SECONDS on a worker while the sky keeps showing the PREVIOUS volume. With no signal out of here
         * the artist's experience of that is "I moved the knob and nothing happened", which is
         * indistinguishable from the dead setting §1.3 of the contract forbids — the owner reported exactly
         * that sentence twice. A view that can say "rebuilding" turns a defect back into a wait.
         *
         * ASKED OF THE RENDERER RATHER THAN REDERIVED BY A PANEL, because this object is the only one that
         * can answer: the decision to re-bake is a comparison (Assets::CloudProceduralParamsEqual) against
         * what is on THIS view's device, and a caller repeating it would need this view's camera, its
         * resolved species and its resolved material — three things it does not have and one of which
         * (the region origin) follows the camera every frame.
         *
         * TRUE ALSO DURING THE FIRST, BLOCKING BAKE, but nobody can observe that: the frame it blocks in is
         * the frame that would have drawn the label.
         */
        bool IsModellingVolumeBaking() const
        {
            return m_ModellingBake.valid();
        }

        /// How far that bake has got, 0..1. Meaningless unless IsModellingVolumeBaking(), and 0 for a view
        /// that has never baked one.
        ///
        /// IT IS A NUMBER AND NOT A SPINNER because the wait it describes is SECONDS long and varies by
        /// four times with the GRID (5 915 ms at 256 against 1 461 ms at 128, this machine, Debug) and by
        /// another four with how much cloud the coverage asks for (3.31 s to 14.05 s over the slider's own
        /// travel, measured before this task) — so "how much longer" is a question the artist genuinely has
        /// and no fixed animation can answer. The bake already
        /// produces the fraction for its own cancellation check (Assets::CloudProceduralBakeProgressFn), so
        /// this costs one relaxed store per XZ slice and nothing at all per voxel.
        float ModellingBakeProgress() const
        {
            return m_ModellingBakeSignal->Fraction.load( std::memory_order_relaxed );
        }

    private:
        bool CreatePipelines();
        // Allocates (or reallocates) all SIX images the pass owns: the quarter-resolution scatter and
        // guide the march writes, and the two half-resolution scatter/guide pairs the resolve ping-pongs
        // between. One function because they are one set — a size is a property of the view, and a guide
        // that outlived a resized scatter, or a history that outlived a resized trace, would hand the next
        // stage last frame's edges. Reallocating also invalidates the history, because the bytes in a
        // freshly created image mean nothing. Returns false having logged the reason and latched the
        // failure.
        bool EnsureTraceTargets( uint32_t halfWidth, uint32_t halfHeight );
        /**
         * Fills m_NoiseVolume with the images the layer's FOUR species name, deduplicated, and
         * m_NoiseSlots with the mapping the packed block sends.
         *
         * ONE VOLUME PER SPECIES AND NOT ONE PER LAYER, which is what this function was rewritten from.
         * The character of a cloud's edge is a property of the KIND of cloud — the shipped Cirrus names
         * the finer of the two volumes and the panel's own tooltip promises that it travels with the type
         * — and until this took four handles rather than the first non-empty one, three of a layer's four
         * slots could name a volume the frame never read.
         *
         * @param handles      the species' cloud type handles, as ResolveSpecies packed them.
         * @param speciesCount how many of them are filled.
         * @return false, having logged the reason, when there is not even a default to fall back on.
         */
        bool EnsureNoiseVolumes( const Assets::AssetHandle ( &handles )[kCloudSpeciesSlots],
                                 uint32_t speciesCount );
        // Allocates the shadow map the first frame the layer actually casts, and REALLOCATES it when the
        // quality tier changes its size. Separate from EnsureTraceTargets because its size is not a
        // property of the view: a viewport resize must not throw it away, where every one of the six trace
        // targets IS the view's size and must. Returns false having logged the reason and latched the
        // failure.
        //
        // @param resolution texels per side for THIS tier, from Graphic::CloudShadowResolutionForScale.
        bool EnsureShadowMap( uint32_t resolution );
        // Allocates the sky-light occlusion volume the first frame a layer asks for it, and never again:
        // its extents are a fixed grid over the modelling volume's region, so neither a viewport resize
        // nor a quality tier can change them. Returns false having logged the reason and latched the
        // failure, and a false answer makes the march fall back to the profile-driven occlusion rather
        // than sampling an image nobody wrote.
        bool EnsureSkyOcclusionVolume();

        /**
         * The three steps every dispatch of this subsystem takes before it can bind anything: resolve the
         * layer's species, guarantee the volumes they name and the modelling volume they are baked into,
         * and pack the parameter block from all of it.
         *
         * ONE FUNCTION AND NOT THREE COPIES OF THE SEQUENCE. The shadow map, the march and the sky's
         * environment bake all need exactly this, in exactly this order, with exactly these arguments —
         * and "both ends of a chain look right, one link in between silently loses something" is the
         * defect shape this repository has recorded seven instances of in one day. The quality tier's two
         * ceilings in particular have to reach every one of them: a shadow ray of two different lengths in
         * one frame is a disagreement no test of either end would find.
         *
         * @param payload written only when this returns true.
         * @return false, having logged the reason where there was one, when the field cannot be built at
         *         all — no atmosphere, no camera, or a volume that could not be created.
         */
        bool BuildFieldPayload( CloudGpuPayload& payload );

        /**
         * The types in this layer's four slots, packed down to a prefix, with the empty ones removed and
         * an all-empty layer answered by ONE built-in default.
         *
         * PACKED AND NOT SPARSE, which is the one decision in this function. A layer with slots 1 and 3
         * filled behaves exactly like a layer with slots 1 and 2 filled: the count is a count, the loop in
         * the march has no holes to skip, and a species' identity is its position in the packed set rather
         * than in the panel. The one thing that DOES follow the packed position is the placement field's
         * decorrelation offset — so moving a type from slot 3 to slot 2 moves its clouds, which is the same
         * thing that happens when the artist changes its scale and is visible for the same reason.
         *
         * @param shapes  filled from the front with the resolved shapes.
         * @param handles filled from the front with the handles they came from — the cache key of the
         *                profile table, and the reason this returns both.
         * @return how many of the two arrays were written, always at least 1.
         */
        uint32_t ResolveSpecies( CloudTypeShape ( &shapes )[kCloudSpeciesSlots],
                                 Assets::AssetHandle ( &handles )[kCloudSpeciesSlots] ) const;
        /**
         * Keeps the procedural MODELLING VOLUME up to date for this view, and collects a finished bake.
         *
         * WHAT IT DECIDES, once a frame and cheaply: whether the parameters that go into the bake have
         * changed, and whether the camera has crossed a snap of the lump lattice. Either answer starts a
         * bake ON A WORKER THREAD; neither blocks the frame. A bake in flight is collected the frame it
         * finishes and uploaded then.
         *
         * WHY A WORKER AND NOT THE FRAME. The bake is measured, not assumed — that is the exit criterion
         * this phase was given (ANALYSIS_APPROACH.md §3) — and Desert/Tests/Engine/CloudProceduralField
         * prints it on every run: 404 / 757 / 1 344 ms for one, two and four species in a Debug build,
         * re-measured after Г10 put the bake's z-slices across the whole pool (it read 803 / 1 584 / 2 529
         * when one worker did all of it). A region shift happens once per lattice cell of camera travel,
         * which at the shipped 3 km cell is rarely; a hitch of even the new figure when it does would be
         * worse than anything the volume buys.
         *
         * WHAT THE FRAME DOES MEANWHILE: it marches the volume it already has. That volume was baked for
         * a region the camera has left by at most one snap step, and it is periodic — so the answer is
         * the neighbouring tile's rather than nothing, which is the same degenerate far path the sky past
         * the region already uses. The first bake of a scene is the one case with no previous volume, and
         * until it lands the species count in the payload is zero and the march composites nothing.
         *
         * @return false, having logged the reason, when the image could not be created at all.
         */
        bool EnsureModellingVolume();

        /// The parameters this view's volume was baked from, as a pure function of the layer and the
        /// resolved species. Separated out because it is asked for twice — once to compare against what
        /// is on the device, once to hand to the bake — and two constructions of one value is the defect
        /// class §2.3.1 names.
        ///
        /// @param shapes / speciesCount the packed set ResolveSpecies produced.
        Assets::CloudProceduralFieldParams BuildProceduralParams( const CloudTypeShape* shapes,
                                                                  uint32_t              speciesCount ) const;

        /// Schema defaults + the `.demat` chain of m_Data.Material -> m_Material, once per frame from
        /// SetCloudSettings. See the definition for why per frame and why the schema is the source.
        void ResolveMaterial();

        /**
         * @brief Turns m_Material.Medium into the compile-time substitution the three in-frame programs
         *        are compiled under, and rebuilds their pipelines when it has changed.
         *
         * ONLY WHEN IT HAS CHANGED, and the comparison is the variant's own content hash rather than the
         * material handle: editing the graph produces new bytes under the same handle, and swapping two
         * materials whose media are the same text must NOT throw three pipelines away. Called once per
         * frame from ResolveMaterial, and does nothing at all in the shipped case, where the hash is 0
         * and stays 0.
         *
         * The FOURTH consumer — the sky panorama the scene is lit by — is not built here: it belongs to
         * the sky renderer and is compiled under the same variant, carried out through
         * CloudEnvironmentBake::Medium.
         */
        void ResolveMedium();

        /**
         * @brief Resolves the medium's OWN properties out of the same `.demat` — its values into
         *        m_MediumValues and its image handles into m_MediumImages.
         *
         * SEPARATE FROM ResolveMaterial's typed resolve, because the two schemas are separate: the shipped
         * one is mirrored field for field onto CloudMaterialValues, and this one is a list whose names the
         * graph author invented. They cannot be read as each other — a medium key carries
         * Core::kCloudMediumOverridePrefix, which no GLSL identifier can — so nothing here can retune a
         * shipped value and nothing there can silently absorb a medium one.
         *
         * @param overrides the flattened material chain ResolveMaterial already asked for; passed rather
         *                  than fetched again so both resolves are certainly of the SAME material.
         */
        void ResolveMediumValues( const MaterialOverrides& overrides );

        /// Writes m_MediumValues into @p buffer and binds it plus every declared image to @p pipeline —
        /// the one statement of "how a medium reaches a dispatch", shared by the march, the shadow map and
        /// the sky-occlusion volume so three call sites cannot come to disagree about a binding number.
        ///
        /// EVERY DECLARED SLOT IS WRITTEN, fallback included: an unwritten descriptor invalidates the whole
        /// set, and this backend answers an invalid set by returning without dispatching — silently.
        void BindMedium( ComputePipeline* pipeline, ShaderResources::StorageBuffer* buffer ) const;

        /// Creates the three pipelines whose programs carry the medium, from the variant currently held
        /// (or from the registered programs when it is default). Split out of CreatePipelines because
        /// ResolveMedium has to do exactly this again when the authored medium changes, and two
        /// constructions of one pipeline set is how they come to disagree.
        bool BuildMediumPipelines();

        /**
         * Turns this frame's hero clouds into the instance buffer the two cloud passes read, and points
         * m_AuthoredAtlas at the image holding their bodies.
         *
         * DISTINCT BODIES GET A SLAB EACH AND REPEATED ONES SHARE, which is where the two limits of this
         * producer part company: kCloudAuthoredSlots caps the INSTANCES, because each costs the march a
         * bounds test at every field sample, and kCloudModellingAtlasMaxSlabs caps the BODIES, because
         * each costs 4.00 MiB. A wood of forty copies of one sculpted tree is one slab.
         *
         * @param payload the layer, already packed. Read for two numbers and neither is recomputed here:
         *                Layer.y, the shell's base altitude, which is what puts an instance into the
         *                field's own frame, and Layer.z, its thickness, which the fit warning is stated
         *                against.
         */
        void BuildAuthoredPayload( const CloudGpuPayload& payload );

        // ---- The authored medium -------------------------------------------------------------------
        //
        // THE VARIANT THREE OF THE FOUR CONSUMERS ARE COMPILED UNDER. Default (empty) is the shipped
        // state and means Generated/CloudMedium.glslh as it sits on disk.
        Core::ShaderVariant m_MediumVariant;
        /// m_MediumVariant.Hash(), kept beside it so the per-frame comparison is one integer and not a
        /// re-hash of the whole body. 0 is the default medium.
        uint64_t m_MediumVariantHash = 0;

        // THE STRONG REFERENCES, and they are what keeps the variant programs alive: ShaderService holds
        // variants only weakly, precisely so that a medium nobody uses any more releases its
        // VkShaderModules instead of accumulating one program per edit of the graph for the whole
        // session. Dropping these three IS the release.
        std::shared_ptr<Shader> m_MarchMediumShader;
        std::shared_ptr<Shader> m_ShadowMapMediumShader;
        std::shared_ptr<Shader> m_SkyOcclusionMediumShader;

        // ---- The authored medium's OWN parameters and images ----------------------------------------
        //
        // THE VALUES ARE NOT IN m_Material AND THAT IS THE DESIGN. m_Material mirrors ONE shader's
        // Properties block field for field; a medium's properties are named by whoever drew the graph, so
        // they are resolved separately out of the same `.demat` and live here. See CloudMediumValues.hpp.
        //
        // Resolved once per frame beside m_Material, for the same reason: an artist dragging a slider in
        // the Material Editor must see the sky move in the frame that follows.
        CloudMediumValues m_MediumValues;

        /// CloudMediumValuesFingerprint( m_MediumValues ) — kept beside them so the environment bake's
        /// trigger is one integer. 0 is "this medium exposes nothing", which is every shipped scene.
        uint64_t m_MediumValuesFingerprint = 0;

        /// The medium's images, resolved from m_MediumValues.Textures once per frame. BORROWED — the
        /// texture service owns them — and one entry per declared slot, null where the material assigned
        /// nothing, because an unassigned slot is still a descriptor that has to be written.
        std::vector<Image2D*> m_MediumImages;

        /// Handles already reported as naming nothing the texture service has, so an unresolvable medium
        /// image is said once rather than once per frame.
        std::unordered_set<uint64_t> m_WarnedMediumImages;

        std::shared_ptr<ComputePipeline>  m_MarchPipeline;
        std::shared_ptr<ComputePipeline>  m_ResolvePipeline;
        std::shared_ptr<ComputePipeline>  m_ShadowMapPipeline;
        std::shared_ptr<ComputePipeline>  m_SkyOcclusionPipeline;
        std::shared_ptr<GraphicsPipeline> m_CompositePipeline;

        std::unique_ptr<MaterialCloudComposite> m_CompositeMaterial;

        // The march's two QUARTER-resolution outputs, allocated and released together because neither is
        // usable alone: the scatter image (premultiplied radiance in .rgb, transmittance in .a) and the
        // depth guide beside it (.x cloud front distance, .y scene distance, both kilometres). Declared
        // adjacently and with no comment between them so the alignment of this block stays one group — a
        // comment inserted mid-group is what clang-format and this repository's style disagree about.
        std::shared_ptr<Image2D>                        m_TraceImage;
        std::shared_ptr<Image2D>                        m_TraceGuideImage;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ParamsBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ResolveParamsBuffer;

        // The half-resolution reconstruction, ping-ponged. The resolve reads index 1 - write and writes
        // index write, so one frame's result is the next frame's history; the composite always samples the
        // one just written. Two images rather than one because a single target would be read and written
        // by the same dispatch at different coordinates, which is a race with no defined answer.
        std::shared_ptr<Image2D> m_HistoryImage[2];
        std::shared_ptr<Image2D> m_HistoryGuideImage[2];

        // THE CLOUD SHADOW MAP and the parameter copy its dispatch reads.
        //
        // A SECOND PARAMETER BUFFER FOR THE SAME BYTES, and it is not duplicated state: both buffers are
        // filled from one call of the pure Graphic::PackCloudParams, on the same m_Data, in the same
        // frame. What forces two of them is the frame, not the data — this pass is issued before the
        // render graph and the march after it, and writing one non-persistent buffer twice between two
        // dispatches is a hazard whose only defence today is that the bytes happen to be equal.
        std::shared_ptr<Image2D>                        m_ShadowMapImage;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ShadowParamsBuffer;

        // This frame's projection, and whether it describes anything. Rebuilt every frame rather than
        // cached: it is a pure function of the camera, the sun and the snap, and a cached matrix is the
        // shape of state that survives a scene reload it should not have.
        CloudShadowMapView m_ShadowMapView{};
        bool               m_ShadowMapValid  = false;
        bool               m_ShadowMapFailed = false;

        // THE SKY-LIGHT OCCLUSION VOLUME — 128 x 16 x 128 RGBA16F, 2.00 MiB, holding at every column and
        // altitude the diffuse transmittance of the cloud STANDING OVER it. It is what the march's ambient
        // term is occluded by when the layer asks for it, and it exists because the alternative —
        // CloudAmbientOcclusion on the sample's own Profile — cannot express "there is cloud above me" at
        // all (Docs/Clouds/DIAGNOSIS_CARTOON.md §1).
        //
        // PER VIEW AND OWNED, exactly like m_ModellingVolume beside it and for a stronger reason than the
        // Docs/RENDERER_FRAME_STATE.md rule requires: its region follows the modelling volume's, which
        // follows THIS view's camera, so a shared one would drag the viewport's shading to wherever a
        // preview's camera happens to be. A renderer whose layer does not want it never allocates it,
        // which is every scene authored before this existed and every asset thumbnail.
        //
        // NO SECOND PARAMETER BUFFER, unlike the shadow map: this pass is dispatched inside
        // ExecuteInFrame between the one write of m_ParamsBuffer and the march that reads it, so both
        // dispatches read one upload rather than racing two.
        std::shared_ptr<Image3D> m_SkyOcclusionVolume;
        bool                     m_SkyOcclusionFailed = false;

        // Whether the volume above holds a reconstruction that may be READ, as distinct from existing.
        // The march has always needed this and carried it as a local; the sky's environment bake needs it
        // too, and that one runs OUTSIDE the frame, before this pass, so it can only ask about the
        // previous frame. Cleared at the top of every ExecuteInFrame so that a frame which skipped the
        // pass leaves "no volume" rather than a stale yes — the bake would otherwise shade a panorama with
        // a transmittance field written for a sky that has since been switched off.
        bool m_SkyOcclusionValid = false;

        // BORROWED, not owned: Runtime::CloudNoiseService owns every noise volume and shares one upload
        // across all views. A raw pointer says that plainly, where a shared_ptr here would suggest this
        // renderer has a say in the image's lifetime and would keep an unloaded volume alive on the device.
        // Refreshed from the service every frame, so a hot reload swaps the image under it with no state of
        // its own to go stale.
        //
        // FOUR OF THEM, one per DISTINCT volume the layer's species name, and entries at or past
        // m_NoiseNeeded repeat the first — every descriptor is written every frame, because an unbound
        // sampler is an invalid descriptor set and this backend answers one by skipping the dispatch.
        Image3D* m_NoiseVolume[kCloudSpeciesSlots] = {};

        // How many of the four are DISTINCT, 1..kCloudSpeciesSlots. It is the number of images the frame
        // has to transition for reading — the barrier is per image, and transitioning the same one four
        // times is four barriers on one resource rather than a no-op.
        uint32_t m_NoiseNeeded = 1;

        // Which of the four each species reads, exactly as it travels to the march in
        // CloudGpuPayload::SpeciesNoise. Kept beside the images so the two cannot be filled from different
        // resolutions of the same question.
        CloudNoiseResolution m_NoiseSlots{};

        // SLOT A. The instance buffer is doubled for the same reason the parameter buffer is — the shadow
        // map dispatches before the render graph and the march after it, and one non-persistent buffer
        // written twice between two dispatches is a hazard whose only defence would be that the bytes
        // happen to be equal. The ATLAS itself is BORROWED from Runtime::CloudModellingService, like the
        // noise volume beside it, and is null in every scene that has no hero cloud in it — in which case
        // the fallback volume is bound instead and the count is zero.
        std::shared_ptr<ShaderResources::StorageBuffer> m_AuthoredBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ShadowAuthoredBuffer;

        // THE AUTHORED MEDIUM'S PARAMETER BLOCK, doubled for exactly the reason the layer's own block and
        // the hero instance list are: two dispatches on opposite sides of the render graph read it, and
        // one non-persistent buffer holds one set of bytes per (frame x renderer slot) — not per pass.
        // Sized for Core::kCloudMediumMaxValues and allocated unconditionally, because a buffer created
        // when the first medium arrives would be created inside the frame, where a failed allocation has
        // nowhere to go but a silently skipped dispatch.
        std::shared_ptr<ShaderResources::StorageBuffer> m_MediumParamsBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ShadowMediumParamsBuffer;

        std::vector<HeroCloudInstance> m_HeroClouds;
        CloudAuthoredPayload           m_AuthoredPayload{};
        // CO-OWNED. This names an image the PROCESS-WIDE CloudModellingService owns, and a second live
        // SceneRenderer asking that service for a different set of bodies destroys it — see
        // CloudModellingAtlasBinding. Holding a handle rather than a pointer is what makes several live
        // renderers legal here, which six renderer slots already make possible. A8-2.
        std::shared_ptr<Image3D> m_AuthoredAtlas;

        // Latched so that a body standing outside its layer is said ONCE per scene rather than sixty
        // times a second, and re-armed the moment the arrangement changes so that fixing it and breaking
        // it again both speak.
        bool m_AuthoredFitWarned    = false;
        bool m_AuthoredCrowdWarned  = false;
        bool m_AuthoredBodiesWarned = false;

        // The procedural MODELLING VOLUME this view marches against — OWNED, unlike the noise volume,
        // because it is GENERATED here rather than resolved from an asset, and because WHERE it is baked
        // depends on THIS view's camera.
        //
        // PER VIEW AND NOT SHARED, and the cost of that is named rather than buried: 8.00 MiB per live
        // renderer where the profile table it replaces was 0.25 MiB. Sharing one volume across views would
        // be the exact defect Docs/RENDERER_FRAME_STATE.md exists to prevent — the region follows a
        // camera, so a second live renderer would drag the viewport's sky to wherever the preview's camera
        // happens to be, and the two would re-bake each other's region every frame. A renderer with no
        // cloud component never allocates it, which is every asset thumbnail and every mesh preview.
        std::shared_ptr<Image3D> m_ModellingVolume;

        // A BAKE IN FLIGHT, on Common::JobSystem. A future rather than a raw thread so the result is
        // collected exactly once — the same arrangement, for the same reason, that the sculpting panel's
        // bake uses.
        //
        // IT USED TO BE `std::async` AND THAT WAS THREE DEFECTS AT ONCE (Г9). The work was invisible to the
        // profiler, so the delay the owner reported had to be measured off log timestamps over a whole day;
        // it obeyed no thread budget, so two document windows could each start bakes with nothing counting
        // them; and it could not be STOPPED, so an already-doomed bake ran to the end while the wanted one
        // waited behind it. There is a fourth, quieter one: the destructor of a future returned by
        // std::async BLOCKS until the task finishes, so simply dropping a stale bake was not available —
        // which is why the old code could only ever have one in flight. A JobSystem future wraps a
        // packaged_task and its destructor waits for nothing, so abandoning one is a move-assignment.
        std::future<Common::ResultStr<std::vector<unsigned char>>> m_ModellingBake;

        /// THE WHOLE CHANNEL BETWEEN A BAKE AND THE VIEW THAT WANTED IT: one flag in, one number out. Both
        /// are read by the worker at the same instant — between XZ slices, through the one progress hook —
        /// so they are ONE object rather than two shared_ptrs that a future edit could give different
        /// lifetimes to.
        struct ModellingBakeSignal
        {
            /// Set by the renderer when this bake's parameters are no longer wanted. The bake returns an
            /// error at its next slice boundary and its result is never collected.
            std::atomic<bool> Cancelled{ false };

            /// 0 at the start, 1 at the end. Written by the worker, read by whatever draws the wait.
            std::atomic<float> Fraction{ 0.0f };
        };

        // A FRESH SIGNAL PER BAKE rather than a reset of the old one: an abandoned job is still reading the
        // object it was started with, so clearing that flag would un-cancel a bake nobody is waiting for and
        // burn a worker to the end of it — and its progress would fight the new bake's for the same number.
        //
        // Never null, so no call site has to ask.
        std::shared_ptr<ModellingBakeSignal> m_ModellingBakeSignal = std::make_shared<ModellingBakeSignal>();

        // How many bakes this view has abandoned. It is in the log line beside the milliseconds because it
        // is the number that says the cancellation is WORKING — "baked in 3 100 ms, 19 stale bake(s)
        // cancelled" is a sentence a slider drag produces and a single edit does not.
        uint32_t m_ModellingBakesCancelled = 0;

        // WHAT THE VOLUME ON THE DEVICE WAS BAKED FROM. Two things, and both have to be asked about: the
        // PARAMETERS, which change when the artist moves a slider or drops a different type into a slot,
        // and the REGION ORIGIN, which changes when the camera crosses a snap of the lump lattice. A cache
        // keyed on one of them alone would either never follow the camera or re-bake on every edit that
        // did not reach the field.
        Assets::CloudProceduralFieldParams m_ModellingParams{};
        glm::vec2                          m_ModellingOriginKm{ 0.0f };
        bool                               m_ModellingValid = false;

        // HOW MANY TIMES THE VOLUME HAS BEEN REBUILT FROM GENUINELY DIFFERENT PARAMETERS — the region's
        // ORIGIN excluded, because that follows the camera. It exists for the sky's environment bake and
        // for nothing else: coverage, the cloud types, the seed, the placement lattice and the painted
        // layout decide WHERE cloud is, none of them is in the packed block the fingerprint hashes, and a
        // trigger that could not see them would leave a scene's ambient describing the weather the sky had
        // when it was opened. Comparing through Assets::CloudProceduralParamsEqual rather than restating
        // which fields matter is what keeps it from drifting from the cache above it, which asks the same
        // question one line later for a different reason.
        uint32_t m_ModellingShapeGeneration = 0;

        // The content hash of the painting already complained about, so a layout the layer cannot honour is
        // named ONCE rather than sixty times a second. MUTABLE because BuildProceduralParams is const and
        // must stay const — it is a pure translation of the component into bake parameters, and the only
        // state it owns is the memory of what it has already said out loud. Zero means "nothing to
        // complain about", which is also the state a usable painting restores it to, so re-fixing a
        // painting and breaking it again reports the second break.
        //
        // ONE PER SLOT, because since O-4 the pattern and the mask are separate inputs that can hold
        // different files. A single latch would let a bad pattern silence the complaint about a bad mask,
        // and the artist would fix one and see nothing about the other.
        mutable uint32_t m_ReportedBadPatternHash = 0u;
        mutable uint32_t m_ReportedBadMaskHash    = 0u;

        /// Drops @p source from @p params, and says so once, when this layer cannot honour it. Static
        /// because it owns nothing: the latch it updates is handed in, so the pattern's memory and the
        /// mask's cannot be confused for one another by a copy-paste.
        static void DropUnusableLayout( Assets::CloudProceduralFieldParams& params, Assets::CloudLayoutTable table,
                                        std::shared_ptr<const Assets::CloudLayoutData>& source,
                                        uint32_t&                                       reportedHash );

        // The region the bake IN FLIGHT is for, so that the frame it lands the payload can be pointed at
        // the region that was actually baked rather than at wherever the camera is by then.
        Assets::CloudProceduralFieldParams m_PendingParams{};
        glm::vec2                          m_PendingOriginKm{ 0.0f };

        // AND WHICH TYPES IT IS FOR, which the pending set did not carry until cancellation arrived. With
        // no way to stop a bake, "is one in flight" was the whole question and the answer was enough; with
        // one, the question becomes "is the one in flight the one I WANT", and a bake started for a
        // different set of cloud types is exactly as stale as one started for different parameters. Without
        // these three the guard would either restart a wanted bake every frame or keep a stale one.
        Assets::AssetHandle m_PendingTypes[kCloudSpeciesSlots]{};
        uint32_t            m_PendingSpeciesCount = 0;
        uint32_t            m_PendingGeneration   = 0;

        // When the bake in flight was started, so the log line that collects it can print what it cost.
        // Wall time and not CPU time: what this number bounds is how far the sky lags the camera.
        std::chrono::steady_clock::time_point m_ModellingBakeStarted{};

        // Latched so a bake that cannot be started, or an image that cannot be created, is said once per
        // scene rather than sixty times a second.
        //
        // THE LATCH IS TIED TO THE PARAMETERS THAT TRIPPED IT, and that is not a refinement — without it
        // the flag never cleared and the whole modelling block below it was skipped for the rest of the
        // session. An artist dragging Layer Thickness, Blend Radius, Profile Depth or Region Size passes
        // through zero on the way to the value they want, and every one of those is rejected by
        // ValidateCloudProceduralParams; Placement Density does the same when dragged past 8. So ONE frame
        // in the middle of an ordinary gesture killed every cloud parameter until the editor was
        // restarted, with nothing on screen to say so — the owner reported it as "I turn the knobs in
        // Details and there is no effect". Remembering WHICH parameters failed keeps the "said once"
        // property for a value that stays broken, while a knob moved to anything else retries.
        bool                               m_ModellingFailed = false;
        Assets::CloudProceduralFieldParams m_FailedParams{};
        glm::vec2                          m_FailedOriginKm{ 0.0f };
        // WHAT THE VOLUME WAS BUILT FROM ON THE ASSET SIDE, and it takes two values rather than one. The
        // handle answers "is this still the type the artist chose"; the generation answers "is the FILE
        // behind that type still the one I read", which is what makes an edit in the Cloud Type panel show
        // up in the viewport without the handle changing at all. A null handle with generation 0 is the
        // "nothing baked yet" state, and it is unreachable as a real answer because a registered type
        // always bumps the generation past zero.
        //
        // FOUR HANDLES, in the packed order ResolveSpecies produced them, plus how many of them were real.
        // Comparing the whole set rather than one handle is what makes dropping a second type into the
        // layer re-bake: the first slot has not changed, and a cache keyed on it alone would show a
        // one-species sky until something else happened to invalidate it.
        Assets::AssetHandle m_ProfileTypes[kCloudSpeciesSlots]{};
        uint32_t            m_ProfileSpeciesCount = 0;
        uint32_t            m_ProfileGeneration   = 0;

        ECS::VolumetricCloudData m_Data{};
        glm::vec3                m_WindOffset{ 0.0f };

        /// The frame's resolved cloud LOOK — CloudRaymarch schema defaults with the `.demat` chain of
        /// m_Data.Material applied over them (O1). Filled by ResolveMaterial() from SetCloudSettings,
        /// read by everything downstream; never read m_Data for a look value again — the component does
        /// not carry them.
        CloudMaterialValues m_Material{};

        /// Material handles already reported as unresolvable, so the warning in ResolveMaterial is said
        /// once per handle and not once per frame.
        std::unordered_set<uint64_t> m_WarnedMissingMaterials;

        /// Material handles already reported as carrying a slot the cloud shader does not declare, on the
        /// same once-per-handle discipline and for the same reason.
        std::unordered_set<uint64_t> m_WarnedUnknownSlots;

        bool                     m_Present = false;

        // This view's quality tier and the numbers it derives. The scale is held rather than re-derived at
        // every use because EnsureShadowMap has to notice when it CHANGES — the map's size is a property
        // of the tier, so switching tiers mid-session reallocates it, which is the one thing the old
        // "allocated once and never again" comment on that function stopped being true about.
        Common::Settings::CloudQuality m_Quality             = Common::Settings::CloudQuality::High;
        float              m_ShadowMapScaleInUse = 0.0f;

        // The HALF-resolution grid, which is what the sub-pixel jitter and the reconstruction are both
        // expressed in. The trace's own extents are this rounded up again and are derived where needed
        // rather than stored, so the two cannot drift apart on an odd viewport.
        uint32_t m_HalfWidth  = 0;
        uint32_t m_HalfHeight = 0;

        // Latched by EnsureTraceTargets and covering ALL SIX images: any one missing means the pass cannot
        // run, and retrying an allocation that already failed once per frame only fills the log.
        bool m_TargetsFailed = false;
        // Set by BuildProceduralParams (which is const, hence mutable) when either layout slot names a
        // painting whose read is in flight; read by EnsureModellingVolume, which is the level that can
        // refuse the frame. TWO MEMBERS rather than one because they answer different questions: one is
        // "is a read outstanding right now", the other is "have I already said so in the log".
        mutable bool m_LayoutPending = false;
        bool         m_LayoutWaiting = false;
        /// Latched while a hero cloud's body is being read, so the waiting line is printed once per wait.
        bool m_HeroWaiting = false;

        bool m_NoiseFailed   = false;
        // Latched while a noise volume's read is in flight, so the "waiting" line is printed once per
        // wait rather than once per frame. SEPARATE FROM m_NoiseFailed on purpose: one of them is a
        // condition that clears itself in a few frames and the other is a scene an artist has to fix,
        // and a single flag would make the log unable to tell them apart -- which is the defect one
        // layer down that this whole change is about.
        bool m_NoiseWaiting = false;

        // Advances once per executed frame. It decides both the march's dither pattern and which of the
        // four sub-pixels this frame traces, and it selects the history target written. Wrapping is
        // harmless: only its low bits reach the hash, the sub-pixel walk and the ping-pong, and all three
        // have a period that divides 2^32.
        uint32_t m_FrameIndex = 0;

        // The view-projection of the frame that WROTE the history — the previous EXECUTED frame, not the
        // previous frame of the application. The two differ whenever the pass is skipped (no component, no
        // atmosphere, an allocation failure), and reprojecting through a matrix from a frame that did not
        // write the history is how a history buffer starts smearing after a scene is reloaded.
        glm::mat4 m_PrevViewProjection{ 1.0f };

        // False until a reconstruction has been written into the targets currently allocated. Reading the
        // history before that is reading uninitialised device memory, so the resolve is handed the
        // engine's fallback texture instead and told to ignore it: an unbound sampler would be an INVALID
        // descriptor set, which this backend answers by skipping the whole dispatch.
        bool m_HistoryValid = false;

        // True while the last ExecuteInFrame actually produced a reconstruction. The composite draws
        // nothing without it, rather than compositing a target from three frames ago over a scene that
        // has since moved.
        bool m_HasFrameResult = false;

        // Which of the two history slots the last executed resolve wrote, and therefore the one the
        // composite must sample. An index rather than a second shared_ptr, so each image has exactly one
        // owner and a resize cannot leave the composite holding a released target.
        uint32_t m_ResolvedIndex = 0;
    };
} // namespace Desert::Graphic::System
