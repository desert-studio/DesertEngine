#include "VolumetricCloudRenderer.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <utility>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialBake.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RenderConfig.hpp> // GlobalTextureFilterSampler, VolumeSampler
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace Desert::Graphic::System
{
    namespace
    {
        // 8x8 for the screen-space march, matching the LocalSize the shader declares. 64 invocations is
        // inside every implementation's guaranteed maximum and the dispatch bounds-checks, so any target
        // size is fine.
        constexpr uint32_t kMarchWorkGroupSize = 8;

        // One spelling with the material schema's own — the march program IS the cloud material shader.
        constexpr const char* kMarchShaderName        = kCloudMaterialShaderName;
        constexpr const char* kResolveShaderName      = "CloudTemporalResolve";
        constexpr const char* kCompositeShaderName    = "CloudComposite";
        constexpr const char* kShadowMapShaderName    = "CloudShadowMap";
        constexpr const char* kSkyOcclusionShaderName = "CloudSkyOcclusionVolume";

        constexpr uint32_t GroupCount( uint32_t extent, uint32_t groupSize )
        {
            return ( extent + groupSize - 1 ) / groupSize;
        }

        // Half resolution, rounded UP. Rounding down would leave the right and bottom column of the frame
        // uncovered by any cloud texel, which the composite's filter then stretches — a one-pixel smear
        // along two edges of the screen that is very hard to attribute. Applied TWICE to reach the trace's
        // quarter resolution, and written that way rather than as a single round-up by four because the
        // invariant the jitter depends on is "the trace grid covers the HALF grid in 2x2 blocks". Chaining
        // the same function states that invariant; a separate QuarterExtent would be a second expression
        // that has to keep agreeing with this one.
        //
        // THE TWO APPLICATIONS ARE NOT A DOUBLE-APPLICATION, and the difference has now cost one
        // measurement, so it is written down. The call at ExecuteInFrame produces the HALF extent — the
        // reconstruction the composite upsamples from. EnsureTraceTargets applies it again to produce the
        // QUARTER extent — the trace the march writes. They are two targets at two sizes, both bound at
        // their own size, and the march is told the half size explicitly through CloudPush::Trace.zw
        // because imageSize() reports only the quarter one. Removing either call does not "undo a
        // doubling"; it moves the whole pyramid up an octave.
        //
        // AND THAT OCTAVE WAS MEASURED AND REFUSED. Р0 (Docs/Clouds/DIAGNOSIS_CARTOON.md §4.5, §8) found
        // that over cloud pixels our fine-scale energy is about half the UE reference's and named the
        // quarter-resolution trace as the last un-eliminated suspect. Р6 raised the pyramid one octave —
        // trace at half, reconstruction at native, so EVERY displayed pixel gets its own traced ray and
        // no resolution deficit remains — and shot the six protocol points at 90 and 3 frames on a
        // measured zero noise floor (Clouds_Protocol, 1280x766, --play, Debug/MoltenVK):
        //
        //     E1 over cloud pixels    quarter (shipped)   half     UE reference
        //     zenith away, 42 deg          0.00165       0.00167      0.00318
        //     mid away, 24 deg             0.00198       0.00204      0.00375
        //
        // +1.2 % and +3.0 % of a quantity short by ~48 %, contrast unchanged to 0.001 at all six points,
        // and the frames are indistinguishable except for a marginal crispening of the far-field band at
        // 7 deg. The price, per-pass GPU self time, minimum of six interleaved runs on a SHARED machine:
        // Clouds: March 12.695 -> 35.907 ms (2.83x, +23.2 ms); pass memory 8.42 -> 33.66 MiB, which at
        // 1920x1080 is 71.2 MiB and exceeds decision D-9's whole 64 MB subsystem budget on its own.
        //
        // So the quarter is a budget and not a defect, and the fine-scale deficit is NOT resolution — a
        // native-resolution march does not close it. What limits the surface is still open; §4.5's
        // signature (our frames surviving a 4x round trip better than the reference) barely moves at
        // native resolution too, 58.7 -> 58.2 % and 60.8 -> 59.4 % against the reference's 45.7 / 53.8 %,
        // so that statistic was reading the smoothness of the cloud itself, not the sampling grid.
        // What would change the answer: a subject-matched reference (§8's near cumulus deck), or a
        // mechanism that adds surface rather than sampling it more finely.
        constexpr uint32_t HalfExtent( uint32_t extent )
        {
            return ( extent + 1u ) / 2u;
        }

        double BytesToMiB( uint64_t bytes )
        {
            return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
        }
    } // namespace

    VolumetricCloudRenderer::~VolumetricCloudRenderer()
    {
        // TELL THE BAKE TO STOP, AND DO NOT WAIT FOR IT. It was `= default` while the future came from
        // std::async, and that was correct only by accident: ~future of an async future BLOCKS, so closing a
        // material document mid-bake stalled the editor for the remainder of a bake nobody would see. On the
        // JobSystem the future's destructor waits for nothing, so the closing is instant — and the flag is
        // what keeps that from leaking a worker for seconds afterwards. Nothing the job touches belongs to
        // this object: the parameters, the origin and the flag are all captured BY VALUE, which is what
        // makes not waiting safe rather than merely fast.
        m_Clipmap.CancelAll();
    }

    Common::BoolResultStr VolumetricCloudRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError( "VolumetricCloudRenderer: the cloud shaders could not be resolved "
                                      "(CloudRaymarch / CloudTemporalResolve / CloudComposite)" );

        // Non-persistent, so the backend keeps one copy per (frame x recording renderer slot) — the
        // Docs/RENDERER_FRAME_STATE.md rule. A shared buffer would let an asset-thumbnail renderer
        // overwrite the viewport's cloud parameters halfway through a frame.
        m_ParamsBuffer = ShaderResources::StorageBuffer::Create( "CloudParams", kCloudPayloadBytes,
                                                                 kCloudParamsBinding, /*persistent=*/false );
        if ( !m_ParamsBuffer )
            return Common::MakeError( "VolumetricCloudRenderer: could not create the cloud parameter buffer" );

        // The reconstruction's camera matrices, on the same terms and for the same reason. It is a buffer
        // rather than a push constant because two 4x4 matrices already fill the 128 bytes Vulkan
        // guarantees for push constants — see the comment on Graphic::CloudResolveParams.
        m_ResolveParamsBuffer = ShaderResources::StorageBuffer::Create(
             "CloudResolveParams", kCloudResolveParamsBytes, kCloudResolveParamsBinding, /*persistent=*/false );
        if ( !m_ResolveParamsBuffer )
            return Common::MakeError(
                 "VolumetricCloudRenderer: could not create the cloud reconstruction parameter buffer" );

        // The shadow map's own copy of the same block, on the same non-persistent terms. Two buffers
        // because two dispatches on opposite sides of the render graph read them, not because there are
        // two sets of numbers — see the member's comment.
        m_ShadowParamsBuffer = ShaderResources::StorageBuffer::Create(
             "CloudShadowParams", kCloudPayloadBytes, kCloudShadowParamsBinding, /*persistent=*/false );
        if ( !m_ShadowParamsBuffer )
            return Common::MakeError(
                 "VolumetricCloudRenderer: could not create the cloud shadow parameter buffer" );

        // Slot A's instance list, doubled for the same reason the parameter block is: two dispatches on
        // opposite sides of the render graph read it. Non-persistent, so the backend keeps one copy per
        // (frame x recording renderer slot) — the Docs/RENDERER_FRAME_STATE.md rule.
        //
        // ALLOCATED UNCONDITIONALLY, 656 bytes each, and that is the one place this feature costs a scene
        // that does not use it. A buffer created lazily would have to be created inside the dispatch path,
        // where a failure has nowhere to go but a silent skip — and the descriptor has to exist anyway,
        // because a declared storage block with no buffer is the same invalid descriptor set a missing
        // sampler is.
        m_AuthoredBuffer = ShaderResources::StorageBuffer::Create( "CloudAuthored", kCloudAuthoredPayloadBytes,
                                                                   kCloudAuthoredBinding, /*persistent=*/false );
        if ( !m_AuthoredBuffer )
            return Common::MakeError( "VolumetricCloudRenderer: could not create the hero cloud instance buffer" );

        m_ShadowAuthoredBuffer =
             ShaderResources::StorageBuffer::Create( "CloudShadowAuthored", kCloudAuthoredPayloadBytes,
                                                     kCloudShadowAuthoredBinding, /*persistent=*/false );
        if ( !m_ShadowAuthoredBuffer )
            return Common::MakeError(
                 "VolumetricCloudRenderer: could not create the hero cloud instance buffer for the shadow map" );

        // THE AUTHORED MEDIUM'S OWN PARAMETER BLOCK, on the same non-persistent terms and doubled for the
        // same reason as the two above. 256 bytes each, allocated whether or not any material in the scene
        // authors a medium: a buffer created on the frame the first medium arrives would be created inside
        // the frame, and a failed device allocation there has nowhere to report itself but a dispatch that
        // silently does not happen.
        m_MediumParamsBuffer = ShaderResources::StorageBuffer::Create(
             "CloudMediumParams", Core::kCloudMediumParamsBytes, Core::kCloudMediumParamsBinding,
             /*persistent=*/false );
        if ( !m_MediumParamsBuffer )
            return Common::MakeError(
                 "VolumetricCloudRenderer: could not create the authored medium's parameter buffer" );

        m_ShadowMediumParamsBuffer = ShaderResources::StorageBuffer::Create(
             "CloudShadowMediumParams", Core::kCloudMediumParamsBytes, Core::kCloudMediumParamsBinding,
             /*persistent=*/false );
        if ( !m_ShadowMediumParamsBuffer )
            return Common::MakeError( "VolumetricCloudRenderer: could not create the authored medium's "
                                      "parameter buffer for the shadow map" );

        m_CompositeMaterial = std::make_unique<MaterialCloudComposite>();
        return BOOLSUCCESS;
    }

    uint32_t VolumetricCloudRenderer::ResolveSpecies( CloudTypeShape ( &shapes )[kCloudSpeciesSlots],
                                                      Assets::AssetHandle ( &handles )[kCloudSpeciesSlots] ) const
    {
        // The component has four type fields and the payload has four species channels, and this is the
        // one place both are indexed by the same number. If they ever part, every array below overflows
        // by exactly as much as they differ.
        static_assert( ECS::kCloudTypeSlots == kCloudSpeciesSlots,
                       "a layer's cloud type slots and the payload's species slots are indexed together" );

        auto* types = Runtime::ResourceRegistry::GetCloudTypeService();

        // WHICH SLOTS BECOME SPECIES IS STATED ONCE, IN ECS::ResolveCloudSpecies, and it stopped being the
        // renderer's private business the moment a painted layout indexed its channels by SPECIES: the
        // Cloud Layout panel has to name the type behind channel 2, and a panel that compacted the slots
        // for itself would name a different one the day this rule moved. Empty slots are skipped and a
        // repeated type is dropped there, for the reason given there.
        Assets::AssetHandle authored[ECS::kCloudTypeSlots];
        m_Material.TypeSlots( authored );

        const ECS::CloudSpeciesResolution resolved = ECS::ResolveCloudSpecies( authored );

        uint32_t speciesCount = resolved.Count;

        if ( resolved.BuiltInDefault )
        {
            // ALL FOUR EMPTY IS A DOCUMENTED ANSWER, and it is the SAME answer an empty single slot gave
            // before there were four: one built-in cumulus congestus. A scene nobody has authored a type
            // for still has to have a sky.
            handles[0]   = Assets::AssetHandle::Null();
            shapes[0]    = Assets::CloudTypeDefaultShape();
            speciesCount = 1;
        }
        else
        {
            for ( uint32_t species = 0; species < speciesCount; ++species )
            {
                handles[species] = authored[resolved.AuthoredSlot[species]];
                shapes[species]  = types->GetShape( handles[species] );
            }
        }

        // THE SCENE'S OWN LIFT, APPLIED HERE AND NOWHERE ELSE — which is the only reason it needs no
        // second number kept in agreement with anything. Both readers of this array reach it through this
        // one function: BuildProceduralParams builds the bake's shell and its bodies from it, and
        // BuildPayload packs the march's shell from it. Lifting the SHAPES rather than either shell is
        // what makes the two move together by construction; see Graphic::CloudLiftSpeciesSet for what
        // lifting only the shell would empty. The built-in default is lifted too: a scene with no type in
        // its slots is still a scene, and a knob that worked only once a type had been dropped in would
        // be the worst kind of dead — dead in exactly the state a new user is in.
        CloudLiftSpeciesSet( m_Data, shapes, speciesCount );

        return speciesCount;
    }

    Assets::CloudProceduralFieldParams
    VolumetricCloudRenderer::BuildProceduralParams( const CloudTypeShape* shapes, uint32_t speciesCount ) const
    {
        Assets::CloudProceduralFieldParams params;

        // EVERYTHING THE BAKE TAKES FROM THE MATERIAL IS APPLIED BY ONE FREE FUNCTION, and the reason it is
        // not written out here is the census that stands on it. Graphic::ApplyCloudMaterialToBakeParams is
        // callable with no ResourceRegistry, so Desert/Tests/Engine/CloudMaterialTiming can perturb one
        // material field at a time and ask Assets::CloudProceduralParamsEqual — this renderer's OWN rebake
        // decision, unchanged — whether the volume has to be built again. That is what makes the schema's
        // `Timing(Rebake)` a checked fact rather than a comment: while this code lived inside a renderer
        // method, nothing could measure which knob cost 14 seconds and which cost none, and the owner
        // reported the layer as "not updating" twice.
        CloudBakeLayerInputs layerInputs;
        layerInputs.RegionSize = m_Data.RegionSize;
        // THIS VIEW'S BAKE BUDGET. It is a property of the VIEW rather than of the layer's look — a preview
        // and a viewport of the same scene legitimately want different answers — which is why it travels
        // through the component beside Max Steps rather than through the material.
        layerInputs.VolumeResolution = m_Data.VolumeResolution;
        layerInputs.MaxSteps         = m_Data.MaxSteps;
        layerInputs.WindDirection    = m_WindDirection;

        ApplyCloudMaterialToBakeParams( m_Material, layerInputs, shapes, speciesCount, params );

        // THE PAINTED LAYOUT'S TWO SOURCES. Resolved through the service exactly as the cloud types above
        // are, so the renderer never learns how to read a file and three viewports resolve one asset once.
        // The numbers that POSITION a painting were applied by the call above; only the handles need a
        // service, which is precisely why they are the two lines that stayed here.
        //
        // TWO SLOTS, RESOLVED INDEPENDENTLY — Unreal's `Layout_CloudGlobalPattern` and
        // `Layout_GlobalCloudMask` are two texture parameters and since O-4 so are ours. Pointing both at
        // one `.dclayout` is the ordinary case and the service returns the same shared object twice.
        //
        // A NULL HERE IS THE SHIPPED STATE AND NOT A FAILURE — every scene in this repository carries empty
        // slots, and the bake reads null as "there is no painting" and places the sky exactly as it did
        // before these fields existed. A handle that names a layout nobody registered is logged by the
        // service, once, and also arrives here as null.
        auto*      layouts = Runtime::ResourceRegistry::GetCloudLayoutService();
        const auto pattern = layouts->Require( m_Material.LayoutPattern );
        const auto mask    = layouts->Require( m_Material.LayoutMask );

        // PENDING IS NOT "NO PAINTING", AND HERE THE TWO ARE THE SAME POINTER.
        //
        // An empty slot and a painting still being read both produce a null `shared_ptr`, and the bake
        // reads null as "place the clouds procedurally". For an empty slot that is correct and shipped.
        // For a painting in flight it is the quiet degradation this tier exists to remove: the layer the
        // artist painted would be placed procedurally, baked, and then silently re-baked a few frames
        // later when the pixels arrived — two different skies for one scene, neither of them reported.
        //
        // So the WHOLE bake waits. It is the caller of this function that can refuse (EnsureModellingVolume
        // returns false and the layer draws nothing this frame), which is why this const function records
        // the fact rather than acting on it.
        m_LayoutPending = pattern.IsPending() || mask.IsPending();

        params.PatternSource = pattern.Share();
        params.MaskSource    = mask.Share();

        // WHEN A PAINTING CANNOT BE HONOURED IT IS DROPPED, NOT THE SKY, AND NOT THE OTHER SLOT. The narrow
        // per-table validator is the one called here on purpose, twice — handed the whole one, a mistyped
        // patch tile would have dropped the artist's painting and blamed the painting for it, and a pattern
        // too coarse for the lattice would have taken the mask down with it.
        //
        // Said out loud, because a painting that silently stopped applying is the least diagnosable thing
        // these slots can do — and said ONCE per painting rather than once per frame, because this function
        // runs every frame and a message at sixty hertz is a log nobody reads.
        DropUnusableLayout( params, Assets::CloudLayoutTable::Pattern, params.PatternSource,
                            m_ReportedBadPatternHash );
        DropUnusableLayout( params, Assets::CloudLayoutTable::Mask, params.MaskSource, m_ReportedBadMaskHash );

        return params;
    }

    void VolumetricCloudRenderer::DropUnusableLayout( Assets::CloudProceduralFieldParams&             params,
                                                      Assets::CloudLayoutTable                        table,
                                                      std::shared_ptr<const Assets::CloudLayoutData>& source,
                                                      uint32_t& reportedHash )
    {
        if ( !source )
            return;

        if ( const auto usable = Assets::ValidateCloudProceduralLayoutTable( params, table ); !usable )
        {
            if ( reportedHash != source->ContentHash )
            {
                reportedHash = source->ContentHash;
                LOG_ERROR( "[Clouds] The layout bound to the cloud {} input is not usable and that input "
                           "will contribute nothing: {}",
                           Assets::CloudLayoutTableName( table ), usable.GetError() );
            }
            source = nullptr;
            return;
        }

        reportedHash = 0u;
    }

    bool VolumetricCloudRenderer::EnsureModellingVolume()
    {
        auto* types = Runtime::ResourceRegistry::GetCloudTypeService();

        CloudTypeShape      shapes[kCloudSpeciesSlots]{};
        Assets::AssetHandle handles[kCloudSpeciesSlots]{};
        const uint32_t      speciesCount = ResolveSpecies( shapes, handles );

        const uint32_t generation = types->GetGeneration();

        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return false;

        const Assets::CloudProceduralFieldParams wanted = BuildProceduralParams( shapes, speciesCount );

        // A PAINTING THIS LAYER NAMES IS STILL BEING READ. Baking now would bake the procedural placement
        // and then have to throw it away — fourteen seconds of volume work on some tiers — and in between
        // the frame would show a sky the artist did not paint. Refuse the frame instead; the host holds
        // its loading overlay up for exactly this.
        if ( m_LayoutPending )
        {
            if ( !m_LayoutWaiting )
            {
                m_LayoutWaiting = true;
                LOG_INFO( "[Clouds] Waiting for this layer's painted layout; the read is on a worker and "
                          "the placement bake does not run until it lands." );
            }
            return false;
        }
        m_LayoutWaiting = false;

        // WHERE THE LEVELS WANT TO BE. The camera MINUS the accumulated wind, because the march asks the
        // volume about `position - wind`: the sky drifting downwind for an hour must not carry the clipmap
        // away from the camera that is looking at it.
        const glm::vec3 cameraKm = camera->GetPosition() / kCloudWorldUnitsPerKm;
        const glm::vec3 windKm   = m_WindOffset / kCloudWorldUnitsPerKm;
        const glm::vec2 clipCameraKm( cameraKm.x - windKm.x, cameraKm.z - windKm.z );

        // A REFUSAL APPLIES TO THE PARAMETERS THAT EARNED IT, AND IS LIFTED THE MOMENT THEY MOVE: an artist
        // cannot reach a positive thickness without passing through zero, and zero is rejected.
        if ( m_ModellingFailed && !Assets::CloudProceduralParamsEqual( m_FailedParams, wanted ) )
            m_ModellingFailed = false;
        if ( m_ModellingFailed )
            return m_ModellingValid;

        // A NEW SHAPE (parameters, types or the files behind them) re-bakes every level whole; a moving
        // camera only scrolls them, inside CloudProceduralClipmap::Update. A shape in flight that is still
        // the wanted one is left alone, so a slider held still does not restart it every frame; one that is
        // not is cancelled by BeginShape.
        {
            const bool sameSet =
                 m_ModellingValid && m_ProfileSpeciesCount == speciesCount && m_ProfileGeneration == generation;
            bool sameTypes = sameSet;
            for ( uint32_t slot = 0; sameTypes && slot < speciesCount; ++slot )
                sameTypes = m_ProfileTypes[slot] == handles[slot];
            const bool sameParams =
                 m_ModellingValid && Assets::CloudProceduralParamsEqual( m_ModellingParams, wanted );

            bool pendingShapeIsWanted = m_Clipmap.IsShapeInFlight() && m_PendingSpeciesCount == speciesCount &&
                                        m_PendingGeneration == generation &&
                                        Assets::CloudProceduralParamsEqual( m_PendingParams, wanted );
            for ( uint32_t slot = 0; pendingShapeIsWanted && slot < speciesCount; ++slot )
                pendingShapeIsWanted = m_PendingTypes[slot] == handles[slot];

            if ( !pendingShapeIsWanted && !( sameTypes && sameParams ) )
            {
                if ( auto valid = Assets::ValidateCloudProceduralParams( wanted ); !valid )
                {
                    m_ModellingFailed = true;
                    m_FailedParams    = wanted;
                    LOG_ERROR( "[Clouds] The cloud layer cannot be turned into a modelling volume: {}",
                               valid.GetError() );
                    return m_ModellingValid;
                }
                m_PendingParams       = wanted;
                m_PendingSpeciesCount = speciesCount;
                m_PendingGeneration   = generation;
                for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
                    m_PendingTypes[slot] = slot < speciesCount ? handles[slot] : Assets::AssetHandle::Null();
                m_Clipmap.BeginShape( wanted, clipCameraKm );
            }
        }

        // COLLECT, WRITE, SCROLL. The first shape of a view blocks (nothing to march meanwhile — a headless
        // shot of ninety frames once finished before an 800 ms bake and wrote an empty sky); later ones do
        // not, and the previous shape stays on the device until the new one has landed whole.
        auto updated = m_Clipmap.Update( clipCameraKm, /*block=*/!m_ModellingValid );
        if ( !updated )
        {
            m_ModellingFailed = true;
            m_FailedParams    = m_PendingParams;
            LOG_ERROR( "[Clouds] The procedural modelling clipmap could not be baked or written: {}",
                       updated.GetError() );
            return m_ModellingValid;
        }

        if ( updated.GetValue() )
        {
            const CloudProceduralClipmap::ShapeLanded& landed    = *updated.GetValue();
            const uint32_t                             bakedSide = m_PendingParams.VolumeSideVoxels;

            // WHAT THE SKY'S ENVIRONMENT BAKE ASKS ABOUT, and it is deliberately NOT "the clipmap scrolled".
            if ( !m_ModellingValid || !Assets::CloudProceduralParamsEqual( m_ModellingParams, m_PendingParams ) )
                ++m_ModellingShapeGeneration;

            m_ModellingParams = m_PendingParams;
            m_ModellingValid  = true;

            if ( !EnsureFarWeatherMap() || !EnsureLayoutTextures() )
            {
                m_ModellingValid = false;
                return false;
            }

            // FROM THE PENDING SET AND NOT FROM THIS FRAME'S: what is recorded as "what the volume on the
            // device was built from" has to be what the finished shape was started with.
            for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
                m_ProfileTypes[slot] = m_PendingTypes[slot];
            m_ProfileSpeciesCount = m_PendingSpeciesCount;
            m_ProfileGeneration   = m_PendingGeneration;

            if ( !landed.CacheWriteError.empty() )
                LOG_WARN( "[Clouds] The modelling clipmap was baked but not all levels were cached: {}",
                          landed.CacheWriteError );
            const glm::vec2 regionOrigin = m_Clipmap.RegionOriginKm();
            LOG_INFO(
                 "[Clouds] Modelling clipmap for {} cloud type(s) in {:.0f} ms ({} of {} levels from the "
                 "DDC) — region {:.0f} km at ({:.1f}, {:.1f}), envelope {:.2f} to {:.2f} km (scene lift "
                 "{:.2f} km), {}x{}x{} RGBA8 ({:.2f} MiB), {} lumps in the region, {:.0f} m per voxel on "
                 "the finest level, {} stale bake(s) cancelled.",
                 m_ProfileSpeciesCount, landed.Ms, landed.LevelsFromCache, CloudProceduralClipmap::kLevels,
                 m_ModellingParams.RegionSizeKm, regionOrigin.x, regionOrigin.y, m_ModellingParams.LayerBottomKm,
                 m_ModellingParams.LayerBottomKm + m_ModellingParams.LayerThicknessKm, CloudLayerLiftKm( m_Data ),
                 bakedSide, Assets::kCloudProceduralVolumeHeight * CloudProceduralClipmap::kLevels, bakedSide,
                 BytesToMiB( static_cast<size_t>( Assets::CloudProceduralVoxelBytes( bakedSide ) ) *
                             CloudProceduralClipmap::kLevels ),
                 Assets::CountCloudProceduralBlobs( m_ModellingParams, regionOrigin ),
                 Assets::CloudProceduralLevelVoxelKm( m_ModellingParams, 0u ) * 1000.0f,
                 m_Clipmap.CancelledBakes() );

            // THE GRID WAS RAISED, AND THE READER IS TOLD SO AT THE PLACE THE SIDE IS ALREADY PRINTED.
            // Graphic::CloudBakeSideForSpecies only ever raises, and only when the asked-for grid could not
            // express a type's authored placement cell — at which point the cheaper grid does not draw a
            // coarser sky, it draws a DIFFERENT one, so the budget has to give. Saying nothing is what let
            // the asset preview place altocumulus on a 1.50 km lattice while the level used 0.90 km.
            const uint32_t askedSide = static_cast<uint32_t>( std::clamp(
                 m_Data.VolumeResolution, static_cast<int32_t>( Assets::kCloudProceduralVolumeSideMin ),
                 static_cast<int32_t>( Assets::kCloudProceduralVolumeSide ) ) );
            if ( bakedSide > askedSide )
            {
                float smallestCellKm = 0.0f;
                for ( const Assets::CloudProceduralSpecies& one : m_ModellingParams.Species )
                    if ( smallestCellKm <= 0.0f || one.CellKm < smallestCellKm )
                        smallestCellKm = one.CellKm;

                LOG_INFO( "[Clouds] The bake grid was raised from the {} voxels this view asked for to {}: "
                          "the finest type places cells {:.2f} km apart and a {} grid can only express "
                          "{:.2f} km, which would have placed the clouds on a different lattice rather than "
                          "drawn the same ones more coarsely.",
                          askedSide, bakedSide, smallestCellKm, askedSide,
                          4.0f * m_ModellingParams.RegionSizeKm / static_cast<float>( askedSide ) );
            }
        }

        return m_ModellingValid;
    }

    bool VolumetricCloudRenderer::BuildMediumPipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return false;

        // THE VARIANT DECIDES WHICH OBJECT, NOT WHICH FILE. All three names are the shipped programs'
        // own; what changes with an authored medium is the bytes one of their includes was compiled
        // from, so a variant program is the same `.shader` under a different substitution. Asking for
        // the registered program when the variant is default is deliberate: a default variant must be
        // ONE object shared with everything else that draws it, not a private copy per renderer.
        const auto acquire = [&]( const char* name, std::shared_ptr<Shader>& hold ) -> std::shared_ptr<Shader>
        {
            if ( m_MediumVariant.IsDefault() )
            {
                hold.reset(); // releases the previous variant's modules, if there was one
                const auto shipped = shaderService->GetByName( name );
                if ( !shipped )
                {
                    LOG_ERROR( "[Clouds] Compute shader '{}' is not registered. Expected "
                               "Editor/Resources/Shaders/Programs/Clouds/{}.shader.",
                               name, name );
                }
                return shipped;
            }

            hold = shaderService->AcquireVariant( name, m_MediumVariant );
            if ( !hold )
            {
                // Named, and then the SHIPPED program is used: an authored medium that will not compile
                // must not take the sky away with it. The material editor's own error is the place the
                // artist reads why; here the frame keeps drawing.
                LOG_ERROR( "[Clouds] the authored medium could not be compiled into '{}' — that program "
                           "falls back to the shipped medium, so this frame's sky is judged by two "
                           "different fields.",
                           name );
                return shaderService->GetByName( name );
            }
            return hold;
        };

        const auto marchShader = acquire( kMarchShaderName, m_MarchMediumShader );
        if ( !marchShader )
            return false;
        // THE REFUSAL IS NOW REACHABLE, AND IT WAS NOT. `if ( !m_MarchPipeline )` stood here against a
        // Create() that returned `make_shared`, i.e. a branch that could not run — which is exactly what
        // made the empty-stage crash look handled. The authored medium is the case it is for: an artist's
        // graph that does not compile reaches this line.
        const auto march = ComputePipeline::Create( { .Shader = marchShader, .DebugName = kMarchShaderName } );
        if ( !march )
        {
            LOG_ERROR( "[Clouds] the march pipeline was not built: {}", march.GetError() );
            return false;
        }
        m_MarchPipeline = march.GetValue();

        const auto shadowShader = acquire( kShadowMapShaderName, m_ShadowMapMediumShader );
        if ( !shadowShader )
            return false;
        const auto shadow =
             ComputePipeline::Create( { .Shader = shadowShader, .DebugName = kShadowMapShaderName } );
        if ( !shadow )
        {
            LOG_ERROR( "[Clouds] the shadow-map pipeline was not built: {}", shadow.GetError() );
            return false;
        }
        m_ShadowMapPipeline = shadow.GetValue();

        // THE SKY-LIGHT OCCLUSION VOLUME'S PRODUCER. Created unconditionally, like the two above, even
        // though the default layer never dispatches it: a pipeline is created once per renderer and a
        // pipeline created lazily inside the dispatch path is a failure with nowhere to go but a silent
        // skip — which is precisely the class of defect this subsystem's zero-cost ladder is written to
        // avoid. What is conditional is the IMAGE and the DISPATCH, and both are below the layer's flag.
        const auto skyOcclusionShader = acquire( kSkyOcclusionShaderName, m_SkyOcclusionMediumShader );
        if ( !skyOcclusionShader )
            return false;
        const auto skyOcclusion =
             ComputePipeline::Create( { .Shader = skyOcclusionShader, .DebugName = kSkyOcclusionShaderName } );
        if ( !skyOcclusion )
        {
            LOG_ERROR( "[Clouds] the sky-occlusion pipeline was not built: {}", skyOcclusion.GetError() );
            return false;
        }
        m_SkyOcclusionPipeline = skyOcclusion.GetValue();

        return true;
    }

    void VolumetricCloudRenderer::ResolveMedium()
    {
        Core::ShaderVariant next;
        if ( !m_Material.Medium.IsNull() )
        {
            if ( const auto shaderService = Runtime::ResourceRegistry::GetShaderService() )
            {
                // Read from the asset's current content every frame: hot reload re-reads it in place, and
                // a cached copy here would be the one thing standing between an edited graph and a
                // changed sky. A refusal (missing asset, or a shader with no Medium block) comes back
                // empty and is logged there, which leaves `next` default — the shipped medium.
                if ( std::string source = shaderService->MediumSourceOf( m_Material.Medium ); !source.empty() )
                    next.VirtualSources.push_back( { std::string( kCloudMediumInclude ), std::move( source ) } );
            }
        }

        const uint64_t hash = next.Hash();
        if ( hash == m_MediumVariantHash )
            return;

        // COMPARED BY CONTENT AND NOT BY HANDLE. Editing the graph produces new bytes under the same
        // handle — which a handle comparison would miss entirely — and swapping two materials whose media
        // are the same text must NOT throw three pipelines away and pay three compiles for a picture that
        // cannot change.
        LOG_INFO( "[Clouds] authored medium {:016x} -> {:016x} ({} bytes); rebuilding the march, the "
                  "shadow map and the sky-occlusion volume against it.",
                  m_MediumVariantHash, hash,
                  next.IsDefault() ? std::size_t{ 0 } : next.VirtualSources.front().Source.size() );

        // THE DEVICE FIRST. The three pipelines below are replaced, and the previous objects may still be
        // referenced by command buffers in flight; this is the same wait the shader hot-reload path takes
        // before invalidating a pipeline cache, and for the same reason.
        Renderer::GetInstance().WaitDeviceIdle();

        m_MediumVariant     = std::move( next );
        m_MediumVariantHash = hash;

        if ( !BuildMediumPipelines() )
        {
            LOG_ERROR( "[Clouds] the medium changed but its pipelines could not be rebuilt — the layer "
                       "will not march until this is fixed." );
        }
    }

    bool VolumetricCloudRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return false;

        // The three programs that CARRY THE MEDIUM are built by their own function, because ResolveMedium
        // has to build exactly the same three again the moment an authored medium arrives — and two
        // constructions of one pipeline set is how they come to disagree about which medium they hold.
        if ( !BuildMediumPipelines() )
            return false;

        const auto resolveShader = shaderService->GetByName( kResolveShaderName );
        if ( !resolveShader )
        {
            LOG_ERROR( "[Clouds] Compute shader '{}' is not registered. Expected "
                       "Editor/Resources/Shaders/Programs/Clouds/{}.shader.",
                       kResolveShaderName, kResolveShaderName );
            return false;
        }
        const auto resolve =
             ComputePipeline::Create( { .Shader = resolveShader, .DebugName = kResolveShaderName } );
        if ( !resolve )
        {
            LOG_ERROR( "[Clouds] the resolve pipeline was not built: {}", resolve.GetError() );
            return false;
        }
        m_ResolvePipeline = resolve.GetValue();

        const auto target = m_TargetFramebuffer.lock();
        if ( !target )
            return false;

        const auto compositeShader = shaderService->GetByName( kCompositeShaderName );
        if ( !compositeShader )
        {
            LOG_ERROR( "[Clouds] Graphics shader '{}' is not registered.", kCompositeShaderName );
            return false;
        }

        GraphicsPipelineSpecification spec;
        spec.DebugName   = kCompositeShaderName;
        spec.Shader      = compositeShader;
        spec.TargetLayout = SceneTargetLayout();

        // A fullscreen triangle has no meaningful depth of its own; occlusion was resolved inside the march,
        // which cut every ray at the distance the depth attachment reported.
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.CullMode          = CullMode::None;
        spec.Topology          = PrimitiveTopology::Triangles;

        // scene = cloud.rgb * One + scene * cloud.a — the premultiplied over-operator with alpha carrying
        // TRANSMITTANCE. The march emits exactly that pair.
        spec.BlendEnable         = true;
        spec.SrcColorBlendFactor = BlendFactor::One;
        spec.DstColorBlendFactor = BlendFactor::SrcAlpha;

        const auto composite = GraphicsPipeline::Create( spec );
        if ( !composite )
        {
            LOG_ERROR( "[Clouds] the composite pipeline was not built: {}", composite.GetError() );
            return false;
        }
        m_CompositePipeline = composite.GetValue();

        // The final `return m_MarchPipeline && ... && m_CompositePipeline;` that stood here is gone: each
        // of those five members is assigned from a Result that was checked at the line above it, so the
        // conjunction could not be false and hid nothing. Each refusal now names WHICH pipeline it was.
        return true;
    }

    bool VolumetricCloudRenderer::EnsureSkyOcclusionVolume()
    {
        if ( m_SkyOcclusionFailed )
            return false;
        if ( m_SkyOcclusionVolume )
            return true;

        // ALLOCATED ON THE FIRST FRAME THE LAYER ACTUALLY ASKS FOR IT, and never reallocated: unlike the
        // six trace targets its size is not a property of the view, and unlike the shadow map its size is
        // not a property of the quality tier either. It is a fixed grid over the modelling volume's own
        // region, so nothing a session can do changes how many texels it needs.
        //
        // NO TIER LEVER, and that is a decision rather than an omission. The cost of this pass is
        // resolution^2 columns of a fixed sample count and it is already a fraction of the shadow map's;
        // halving the grid would buy a fraction of a fraction while coarsening a term that is applied to
        // every cloud pixel in the frame. If it ever needs one, the lever is this constant and the
        // relation in Common/CloudLighting.glslh says what it must stay inside.
        const Core::Formats::Image3DSpecification spec{
             .Tag        = "CloudSkyOcclusionVolume",
             .Width      = kCloudSkyOcclusionResolution,
             .Height     = kCloudSkyOcclusionSlices,
             .Depth      = kCloudSkyOcclusionResolution,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };

        m_SkyOcclusionVolume = Image3D::Create( spec );
        if ( !m_SkyOcclusionVolume )
        {
            LOG_ERROR( "[Clouds] The {}x{}x{} RGBA16F sky-light occlusion volume could not be created on the "
                       "device; the clouds will fall back to occluding their ambient by the sample's own "
                       "profile for this view.",
                       kCloudSkyOcclusionResolution, kCloudSkyOcclusionSlices, kCloudSkyOcclusionResolution );
            m_SkyOcclusionFailed = true;
            return false;
        }

        LOG_INFO( "[Clouds] Sky-light occlusion volume {}x{}x{} RGBA16F ({:.2f} MiB) — {:.0f} km across the "
                  "world at {:.0f} m per texel, {} altitude slices over the shell.",
                  kCloudSkyOcclusionResolution, kCloudSkyOcclusionSlices, kCloudSkyOcclusionResolution,
                  BytesToMiB( kCloudSkyOcclusionBytes ), m_ModellingParams.RegionSizeKm,
                  m_ModellingParams.RegionSizeKm / static_cast<float>( kCloudSkyOcclusionResolution ) * 1000.0f,
                  kCloudSkyOcclusionSlices );
        return true;
    }

    float VolumetricCloudRenderer::GetShadowStrength() const
    {
        // ZERO WHEN THE LAYER IS NOT CASTING, so a consumer that reads only this number cannot light the
        // world through a map that was never written. It is the same gate DeclareShadowMapNodes uses,
        // stated once here rather than repeated at every call site.
        if ( !m_Present || !m_Data.Enabled || !m_Data.CastShadows )
            return 0.0f;
        return std::clamp( m_Data.ShadowStrength, 0.0f, 1.0f );
    }

    bool VolumetricCloudRenderer::BuildFieldPayload( CloudGpuPayload& payload )
    {
        // Without a sky there is no sun to light the clouds and no ambient to fill their shadowed sides.
        const AtmosphereEnv& atmosphere = m_SceneRenderer->GetAtmosphere();
        if ( !atmosphere.Valid )
            return false;

        // THE SPECIES COME FIRST, and the order is load-bearing rather than tidy: the volumes a layer
        // binds are named by its TYPES, so the set has to be resolved before there is anything to look
        // them up with.
        CloudTypeShape      shapes[kCloudSpeciesSlots]{};
        Assets::AssetHandle handles[kCloudSpeciesSlots]{};
        const uint32_t      speciesCount = ResolveSpecies( shapes, handles );

        if ( !EnsureNoiseVolumes( handles, speciesCount ) )
            return false;
        if ( !EnsureModellingVolume() )
            return false;

        // THE TIER'S TWO CEILINGS COME FROM ONE CALL for every consumer, because the passes march the same
        // field and a shadow ray of two different lengths in one frame is the class of disagreement
        // §2.3.1 of the contract is about.
        const CloudQualityScale quality = CloudQualityFor( m_Quality );

        payload = PackCloudParams(
             m_Data, m_Material, shapes, speciesCount, atmosphere, m_WindOffset,
             CloudRegionBinding{ m_Clipmap.LevelUniforms(), Assets::CloudFarWeatherUniform( m_ModellingParams ),
                                 Assets::CloudLayoutPlaceUniform( m_ModellingParams ),
                                 Assets::CloudLayoutStrengthUniform( m_ModellingParams ),
                                 m_Clipmap.FarStatistics().Presence, m_Clipmap.FarStatistics().Profile },
             quality.LightMarchSampleCeiling, quality.StopTransmittanceFloor, m_NoiseSlots );
        return true;
    }

    bool VolumetricCloudRenderer::EnsureFarWeatherMap()
    {
        // KEYED ON WHAT THE MAP IS A FUNCTION OF, and nothing else: the seed and the shortest wave. The
        // cover and the strength are read by the march through CloudGpuPayload::Weather, so moving the
        // Coverage slider re-uploads nothing here.
        const uint32_t seed   = Assets::CloudFarWeatherSeed( m_ModellingParams );
        const float    tileKm = m_ModellingParams.PatchTileKm;

        if ( m_FarWeatherMap && !m_FarWeatherStale && m_FarWeatherSeed == seed && m_FarWeatherTileKm == tileKm )
            return true;

        const std::vector<float> weather = Assets::BakeCloudFarWeatherMap( seed, tileKm );

        // RGBA16F WITH W IN .r: a half keeps W (|W| < 5) to 2e-3, a thousandth of the weather map W's sensitivity
        // band, and is filterable on every device this engine targets where R32F is not.
        std::vector<std::byte> texels( weather.size() * 4u * sizeof( uint16_t ), std::byte{ 0 } );
        for ( size_t i = 0; i < weather.size(); ++i )
        {
            const uint16_t half = static_cast<uint16_t>( glm::packHalf1x16( weather[i] ) );
            std::memcpy( texels.data() + i * 4u * sizeof( uint16_t ), &half, sizeof( half ) );
        }

        // CREATED EMPTY: the texels reach it on the frame graph (AddInputUploads), recorded on the frame's own
        // command buffer, so a seed change costs no submit-and-wait.
        const Core::Formats::Image2DSpecification spec{
             .Tag    = "CloudFarWeatherMap",
             .Width  = Assets::kCloudFarWeatherMapSide,
             .Height = Assets::kCloudFarWeatherMapSide,
             .Format = Core::Formats::ImageFormat::RGBA16F,
             .Usage  = Core::Formats::Image2DUsage::Image2D,
             // LINEAR ALWAYS: the weather map reads W between texels (1.95 km apart); under the user's "Nearest"
             // the cut would follow texel-block edges — straight walls in the sky.
             .Properties = Core::Formats::Sample | Core::Formats::AlgorithmicLinear,
        };

        // NO DEVICE IDLE before dropping the old map: the allocator defers an image's destruction by frame index
        // (VulkanAllocator::RT_DestroyImage), and a sky capture still marching it co-owns it (CloudBakeBinding).
        m_FarWeatherMap = Image2D::Create( spec );
        if ( !m_FarWeatherMap )
        {
            LOG_ERROR( "[Clouds] The {}^2 world weather map could not be created on the device; the clouds will "
                       "not render for this view.",
                       Assets::kCloudFarWeatherMapSide );
            return false;
        }

        m_FarWeatherSeed   = seed;
        m_FarWeatherTileKm = tileKm;
        m_FarWeatherStale  = false;
        std::erase_if( m_PendingUploads, []( const PendingUpload& upload )
                       { return std::string_view( upload.Name ) == "Clouds.FarWeather"; } );
        m_PendingUploads.push_back(
             PendingUpload{ m_FarWeatherMap, std::move( texels ), "Clouds.FarWeather", &m_FarWeatherStale } );
        return true;
    }

    bool VolumetricCloudRenderer::EnsureLayoutTextures()
    {
        // KEYED ON THE PAINTING'S CONTENT HASH, the number Assets::CloudProceduralParamsEqual compares it by;
        // strength and placement reach the march through CloudGpuPayload, so moving them uploads nothing.
        const Assets::CloudLayoutData* pattern     = m_ModellingParams.PatternSource.get();
        const Assets::CloudLayoutData* mask        = m_ModellingParams.MaskSource.get();
        const bool                     hasPattern  = pattern != nullptr && pattern->HasPattern();
        const bool                     hasMask     = mask != nullptr && mask->HasMask();
        const uint32_t                 patternHash = hasPattern ? pattern->ContentHash : 0u;
        const uint32_t                 maskHash    = hasMask ? mask->ContentHash : 0u;

        if ( m_LayoutPatternTexture && m_LayoutMaskTexture && !m_LayoutStale &&
             m_LayoutPatternHash == patternHash && m_LayoutMaskHash == maskHash )
            return true;

        // CREATED EMPTY, like the weather map: the texels reach them on the frame graph (AddInputUploads).
        const auto create = []( const char* tag, uint32_t side )
        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag    = tag,
                 .Width  = side,
                 .Height = side,
                 .Format = Core::Formats::ImageFormat::RGBA8F,
                 .Usage  = Core::Formats::Image2DUsage::Image2D,
                 // LINEAR ALWAYS and REPEAT: the CPU's BilinearWrapped is exactly this filter, and the
                 // painting tiles the region by construction.
                 .Properties = Core::Formats::Sample | Core::Formats::AlgorithmicLinear,
            };
            return Image2D::Create( spec );
        };

        // THE STAND-INS ARE THE NEUTRALS: white (the slider's own sky) and a mask at 128 (adds nothing).
        std::vector<unsigned char> patternTexels =
             hasPattern ? pattern->Pattern : std::vector<unsigned char>{ 255u, 255u, 255u, 255u };
        std::vector<unsigned char> maskTexels;
        if ( hasMask )
        {
            maskTexels.assign( mask->Mask.size() * 4u, 0u );
            for ( size_t i = 0; i < mask->Mask.size(); ++i )
                maskTexels[i * 4u] = mask->Mask[i];
        }
        else
            maskTexels = { Assets::kCloudLayoutMaskNeutral, 0u, 0u, 0u };

        // No device idle, for the reason EnsureFarWeatherMap gives.
        m_LayoutPatternTexture = create( "CloudLayoutPattern", hasPattern ? pattern->Resolution : 1u );
        m_LayoutMaskTexture    = create( "CloudLayoutMask", hasMask ? mask->Resolution : 1u );
        if ( !m_LayoutPatternTexture || !m_LayoutMaskTexture )
        {
            LOG_ERROR( "[Clouds] The painted layout's textures could not be created on the device; the clouds "
                       "will not render for this view." );
            return false;
        }

        m_LayoutPatternHash = patternHash;
        m_LayoutMaskHash    = maskHash;
        m_LayoutStale       = false;
        const auto bytes    = []( const std::vector<unsigned char>& texels )
        {
            std::vector<std::byte> out( texels.size() );
            std::memcpy( out.data(), texels.data(), texels.size() );
            return out;
        };
        std::erase_if( m_PendingUploads,
                       []( const PendingUpload& upload )
                       {
                           const std::string_view name( upload.Name );
                           return name == "Clouds.LayoutPattern" || name == "Clouds.LayoutMask";
                       } );
        m_PendingUploads.push_back( PendingUpload{ m_LayoutPatternTexture, bytes( patternTexels ),
                                                   "Clouds.LayoutPattern", &m_LayoutStale } );
        m_PendingUploads.push_back(
             PendingUpload{ m_LayoutMaskTexture, bytes( maskTexels ), "Clouds.LayoutMask", &m_LayoutStale } );
        return true;
    }

    CloudEnvironmentBake VolumetricCloudRenderer::BuildEnvironmentBake()
    {
        DESERT_PROFILE_SCOPE( "Clouds: BuildEnvironmentBake" );

        CloudEnvironmentBake bake{};

        // THE SAME ZERO-COST LADDER THE TWO DISPATCHES HAVE, and it is checked before any allocation
        // rather than after: the editor builds a SceneRenderer for every asset thumbnail and every mesh
        // preview, none of them has a sky, and none of them may pay for a modelling volume here.
        if ( !m_Present || !m_Data.Enabled )
            return bake;

        if ( !BuildFieldPayload( bake.Params ) )
        {
            // WAITING IS NOT FAILING. A read still on a worker or the first modelling bake still running
            // will complete; a refused resource will not, and that layer bakes as sky alone.
            bake.InputsPending = m_NoiseWaiting || m_LayoutWaiting || m_Clipmap.IsShapeInFlight();
            return bake;
        }

        bake.InputsPending = m_Data.SkyOcclusionVolume && !m_SkyOcclusionDecided;

        // Slot A, on the same terms the two in-frame passes build it on. A hero cloud is part of the
        // field, so it lights the world through the environment for the same reason it shades the ground.
        BuildAuthoredPayload( bake.Params );
        bake.Authored = m_AuthoredPayload;

        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
            bake.Noise[slot] = m_NoiseVolume[slot];

        bake.Modelling     = m_Clipmap.GetImage();
        bake.FarWeather    = m_FarWeatherMap;
        bake.LayoutPattern = m_LayoutPatternTexture;
        bake.LayoutMask    = m_LayoutMaskTexture;
        bake.AuthoredAtlas = m_AuthoredAtlas;

        // THE PREVIOUS FRAME'S VOLUME, and it can be nothing else: this runs before the frame's own
        // dispatch of it. The first bake of a scene therefore marches with the profile-driven occlusion
        // the layer would fall back to anyway, and the fingerprint below carries the flag so that the
        // frame the volume appears asks for one more bake rather than leaving a permanently brighter dome.
        bake.SkyOcclusionValid  = m_SkyOcclusionValid && m_SkyOcclusionVolume != nullptr;
        bake.SkyOcclusionVolume = m_SkyOcclusionVolume;

        // WHAT THE BLOCK ABOVE MEANS, asked of the same function PackCloudParams asked. BuildFieldPayload
        // has just chosen which illuminance SunColour carries; the bake marches that block and has to
        // apply the matching transmittance, or the panorama is lit by the sun as seen from space.
        bake.PerSampleSunTransmittance =
             CloudUsesPerSampleSunTransmittance( m_Data, m_SceneRenderer->GetAtmosphere() );

        // THE AUTHORED MEDIUM CROSSES HERE, into the one consumer that is not this renderer's. Copied
        // rather than pointed at: the bake outlives this call by a submit-and-wait, and the variant is a
        // few kilobytes of text against three quarters of a second of device time.
        bake.Medium = m_MediumVariant;

        // AND ITS VALUES, which are NOT in the packed block above and never will be: they belong to a
        // schema the graph author wrote. Copied for the same reason the variant is -- the capture outlives
        // this call by its slices -- and the IMAGES are shared, exactly like the noise volumes beside them,
        // so a material edit mid-capture cannot free one a later slice samples.
        bake.MediumValues = m_MediumValues.Params;
        bake.MediumImages = m_MediumImages;

        bake.Marched = true;
        bake.Fingerprint =
             CloudEnvironmentFingerprint( bake.Params, true, bake.SkyOcclusionValid, m_ModellingShapeGeneration,
                                          m_MediumVariantHash, m_MediumValuesFingerprint );
        return bake;
    }

    namespace
    {
        // A cloud compute node's one binding block (block 0): the pipeline's shader layout, the pipeline route
        // the node's setup filled with the pipeline's setters, and the push block its exec gives. The exec opens
        // it with RDG::PassBindings( context, context.GetBindingBlock( 0 ) ) and only records.
        // @p layout is the one the renderer keeps for @p pipeline (keyed on its shader, not re-derived per frame).
        auto DeclareComputeBlock( RenderPassDeclaration& declared, const ComputePipeline* pipeline,
                                  ShaderBindingLayoutCache& layout, const uint32_t pushBytes )
        {
            auto block = declared.Bindings( layout.Get( pipeline->GetSpecification().Shader ),
                                            Renderer::GetPipelineRouteFill( *pipeline ) );
            block.PushConstantBytes( pushBytes );
            return block;
        }
    } // namespace

    std::vector<ComputeNodeDeclaration> VolumetricCloudRenderer::DeclareShadowMapNodes( RDG::Builder& graph )
    {
        std::vector<ComputeNodeDeclaration> nodes;
        // Cleared FIRST and set only at the end, so every early return below leaves consumers with "no
        // cloud shadow this frame" rather than with the projection of a frame the sun has since left.
        m_ShadowMapValid   = false;
        m_ShadowMapPending = false;
        m_ShadowMap        = {};

        if ( !m_ShadowMapPipeline || !m_ShadowParamsBuffer )
            return nodes;

        // THE ZERO-COST LADDER, and it is the same one the march has, plus the two fields this task
        // added. A scene with no cloud component, with the clouds off, with casting off or with the
        // strength at zero dispatches nothing and — because the transient is declared below this line and
        // not above it — the graph places no memory for it either. The editor builds a SceneRenderer for every
        // asset thumbnail and every mesh preview, and none of them has a sky.
        if ( GetShadowStrength() <= 0.0f )
            return nodes;

        const AtmosphereEnv& atmosphere = m_SceneRenderer->GetAtmosphere();
        if ( !atmosphere.Valid )
            return nodes;

        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return nodes;

        // THE TIER'S THREE NUMBERS, DERIVED FROM ONE SCALE. Resolution, extent and snap move together so
        // that the texel — and therefore the relation against the chord the view march can find, which
        // Desert/Tests/Engine/CloudShadow asserts for every tier — does not move at all. What the cheaper
        // tier buys is a smaller square of world, not a coarser one.
        const CloudQualityScale quality    = CloudQualityFor( m_Quality );
        const uint32_t          resolution = CloudShadowResolutionForScale( quality.ShadowMapScale );
        const float             extentKm   = CloudShadowExtentKmForScale( quality.ShadowMapScale );
        const float             snapKm     = CloudShadowSnapKmForScale( quality.ShadowMapScale );

        CloudGpuPayload payload{};
        if ( !BuildFieldPayload( payload ) )
            return nodes;

        // A TRANSIENT OF THE FRAME GRAPH: written here, read by the lit passes of the same frame, never by a later
        // one (the projection is rebuilt every frame). Its size is the tier's, not the view's; announced when the
        // tier changes it.
        if ( m_ShadowMapScaleInUse != quality.ShadowMapScale )
        {
            m_ShadowMapScaleInUse = quality.ShadowMapScale;
            LOG_INFO( "[Clouds] Cloud shadow map {}x{} RGBA32F ({:.2f} MiB, a frame-graph transient) — {:.0f} km "
                      "across the world, {:.1f} m per texel, snapped to a {:.0f} km grid, quality scale {:.2f}.",
                      resolution, resolution,
                      BytesToMiB( Core::Formats::CalculateImageSize( resolution, resolution,
                                                                     Core::Formats::ImageFormat::RGBA32F ) ),
                      2.0f * extentKm, 2.0f * extentKm * 1000.0f / static_cast<float>( resolution ), snapKm,
                      quality.ShadowMapScale );
        }
        const RDG::TextureRef shadowMap = graph.CreateTexture(
             RDG::TextureDesc{ .Size   = RDG::Extent3D{ .Width = resolution, .Height = resolution },
                               .Format = Core::Formats::ImageFormat::RGBA32F },
             "Clouds.ShadowMap" );

        // BUILD TIME: whether the node exists depends on these uploads, and they fill this frame's slot of the
        // buffers before the graph executes.
        const auto shadowParams =
             m_ShadowParamsBuffer->SetData( &payload, static_cast<uint32_t>( sizeof( payload ) ) );

        // Slot A, for the shadow map as well as for the eye: a hero cloud shades the ground under it
        // because it IS the cloud field, not because anything was added to the deferred pass.
        BuildAuthoredPayload( payload );
        const auto shadowAuthored = m_ShadowAuthoredBuffer->SetData(
             &m_AuthoredPayload, static_cast<uint32_t>( sizeof( m_AuthoredPayload ) ) );

        // NO BLOCK, NO DISPATCH — the same answer this function already gives four lines up when
        // BuildFieldPayload refuses, extended to the upload of what it built. A march that runs against
        // the PREVIOUS frame's parameters does not merely look stale: the layer altitude, the planet
        // radius and the map extent it reads must agree with the m_ShadowMapView computed below from the
        // payload in hand, and a shadow map centred on one sphere while the eye march intersects another
        // is the "seam at 30 km" this subsystem has already paid for once.
        if ( !shadowParams.IsSuccess() || !shadowAuthored.IsSuccess() )
        {
            LOG_ERROR( "[Clouds] the shadow map is not rendered this frame; its parameters were not "
                       "uploaded. field: {} | authored: {}",
                       shadowParams.IsSuccess() ? "ok" : shadowParams.GetError(),
                       shadowAuthored.IsSuccess() ? "ok" : shadowAuthored.GetError() );
            return nodes;
        }

        // THE PLANET RADIUS IS TAKEN FROM THE PACKED BLOCK, not from the component, because the packer is
        // where it is floored — and a map centred on a different sphere than the one the march intersects
        // is a shadow displaced by the curvature.
        m_ShadowMapView = CloudBuildShadowMapView( camera->GetPosition(), atmosphere.SunDirection, payload.Layer.x,
                                                   extentKm, snapKm, static_cast<float>( resolution ) );

        CloudShadowPush push{};
        push.MapToWorld = m_ShadowMapView.MapToWorld;
        push.Trace      = glm::vec4( m_ShadowMapView.LightDirection, m_ShadowMapView.SampleCount );
        // The far plane the producer encodes its depths against, taken from the projection it is handed
        // rather than compiled from a constant — see Graphic::CloudShadowPush.
        push.Depth = glm::vec4( m_ShadowMapView.FarDepthKm, 0.0f, 0.0f, 0.0f );

        // One Compute node: it samples the cloud volumes and writes the shadow map the lit passes sample
        // (as a scene view input, SceneViewInputsOf).
        ComputeNodeDeclaration shadow;
        shadow.Name = "Clouds: ShadowMap";
        // SETUP: the shadow map it writes is a block entry (the graph imports, orders and barriers it); the
        // volumes and buffers it samples are the renderer's own, bound by the pipeline's setters here, so the
        // node's block is complete when it is declared; the exec only records.
        m_ShadowMapPipeline->SetStorageBuffer( kCloudShadowParamsBinding, m_ShadowParamsBuffer.get() );
        m_ShadowMapPipeline->SetStorageBuffer( kCloudShadowAuthoredBinding, m_ShadowAuthoredBuffer.get() );
        BindMedium( m_ShadowMapPipeline.get(), m_ShadowMediumParamsBuffer.get() );
        SampledMedium(
             SampledVolumes( DeclareComputeBlock( shadow.Access, m_ShadowMapPipeline.get(), m_ShadowMapLayout,
                                                  static_cast<uint32_t>( sizeof( CloudShadowPush ) ) ) ) )
             .Storage( "u_CloudShadowMap", shadowMap, RDG::Access::StorageWrite );
        shadow.Record = [this, push, resolution]( RDG::PassContext& context,
                                                  const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            DESERT_PROFILE_PASS( "Clouds: ShadowMap" );
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            return Renderer::DispatchCompute( bindings, *m_ShadowMapPipeline,
                                              GroupCount( resolution, kMarchWorkGroupSize ),
                                              GroupCount( resolution, kMarchWorkGroupSize ), 1 );
        };
        nodes.push_back( std::move( shadow ) );

        m_ShadowMap        = shadowMap;
        m_ShadowMapPending = true;
        return nodes;
    }

    void VolumetricCloudRenderer::SettleShadowMapNodes( bool accepted )
    {
        // BUILD TIME: the lit passes declared after this read HasShadowMap() to decide whether they import the
        // map.
        m_ShadowMapValid   = m_ShadowMapPending && accepted;
        m_ShadowMapPending = false;
        if ( !m_ShadowMapValid )
            m_ShadowMap = {};
    }

    void VolumetricCloudRenderer::SetCloudSettings( bool present, const ECS::VolumetricCloudData& data,
                                                    const glm::vec3& windOffset, const glm::vec3& windDirection,
                                                    Common::Settings::CloudQuality        quality,
                                                    const std::vector<HeroCloudInstance>& heroClouds )
    {
        m_Present    = present;
        m_Data       = data;
        m_WindOffset    = windOffset;
        m_WindDirection = windDirection;
        m_Quality    = quality;

        // RE-ARM THE WARNINGS WHEN THE ARRANGEMENT CHANGES. A latch that is never released says a thing
        // once and then lies for the rest of the session: an artist who moves a body back inside the
        // layer and out again would hear nothing the second time. Comparing the count is the cheap half
        // of "the arrangement changed" and it is the half that matters — adding or removing a hero cloud
        // is what re-opens both questions.
        if ( heroClouds.size() != m_HeroClouds.size() )
        {
            m_AuthoredFitWarned = false;
            m_AuthoredCrowdWarned = false;
        }

        m_HeroClouds = heroClouds;

        ResolveMaterial();
    }

    void VolumetricCloudRenderer::ResolveMaterial()
    {
        // THE LOOK IS RESOLVED HERE, ONCE PER FRAME, AND NOWHERE ELSE (O1, D-35). The march schema —
        // CloudRaymarch's own Properties block — supplies every default, the `.demat` chain overwrites by
        // name, and everything downstream (the packer, the bake decision, the species resolve) reads the
        // one m_Material this fills. Resolving per frame is what the terrain does for the same reason:
        // an artist dragging a slider in the Material Editor must see the sky move the same frame, and
        // the whole resolve is two vector copies plus a name map.
        const Core::Formats::ShaderProgramMeta* schema = nullptr;
        if ( const auto shaderService = Runtime::ResourceRegistry::GetShaderService() )
        {
            if ( const auto marchShader = shaderService->GetByName( kMarchShaderName ) )
                schema = &marchShader->GetProgramMeta();
        }
        // A missing march shader is already a loud failure in CreatePipelines — nothing marches at all —
        // so no second message here; BuildCloudMaterialValues degrades to its pinned mirror of the schema.

        MaterialOverrides overrides;
        if ( static_cast<uint64_t>( m_Data.Material ) != 0 )
        {
            auto* materialService = Runtime::ResourceRegistry::GetMaterialService();
            if ( !materialService || !materialService->ResolveOverrides( m_Data.Material, overrides ) )
            {
                // Not a silent default: a handle that resolves to nothing is a material the scene names
                // and the asset database does not have — the sky then renders the schema defaults, which
                // LOOKS like an unauthored layer rather than a broken one. Said once per handle; this
                // runs every frame.
                const uint64_t raw = static_cast<uint64_t>( m_Data.Material );
                if ( m_WarnedMissingMaterials.insert( raw ).second )
                {
                    LOG_WARN( "[Clouds] material handle {} does not resolve to a registered material — "
                              "the layer renders with the CloudRaymarch schema defaults.",
                              raw );
                }
            }
            else if ( schema && !schema->Params.empty() )
            {
                // A NAME THE SHADER DOES NOT DECLARE IS DROPPED, AND UNTIL NOW SILENTLY. The surface path
                // has warned about this since it was written (MaterialService's ApplySurfaceAsset); the
                // cloud path never did, because BuildCloudMaterialValues skips an unknown key by design —
                // a `.demat` may be a shader revision ahead of this binary. That is right for a VALUE and
                // wrong for an ASSET reference: a dropped number falls back to a default that still looks
                // authored, but a dropped layout is a painting that stops applying with nothing anywhere
                // to say so. O-4 renamed `CloudLayout` to `LayoutPattern` and `LayoutMask`, and this is
                // what makes a material nobody ran the migrator over report itself instead of rendering a
                // sky with the painting quietly missing.
                //
                // Once per material handle, on the same latch as the miss above: this runs every frame.
                const uint64_t raw = static_cast<uint64_t>( m_Data.Material );
                for ( const auto& texture : overrides.Textures )
                {
                    // A MEDIUM'S IMAGE IS NOT AN UNKNOWN SLOT, and without this line every medium that
                    // declared one would report itself as a material somebody forgot to migrate. The two
                    // schemas share this map and are told apart by the prefix, which is the whole reason
                    // the prefix exists — see Core::kCloudMediumOverridePrefix.
                    if ( Core::IsCloudMediumOverrideKey( texture.first ) )
                        continue;

                    const bool declared = std::any_of( schema->Params.begin(), schema->Params.end(),
                                                       [&texture]( const Core::Formats::ShaderParam& p )
                                                       { return p.Name == texture.first; } );
                    if ( declared )
                        continue;

                    if ( m_WarnedUnknownSlots.insert( raw ).second )
                        LOG_WARN( "[Clouds] material handle {} carries a value for '{}', which the cloud "
                                  "shader does not declare, so it is ignored. The slot was renamed or "
                                  "removed since this material was authored — run Tools/SceneMigrator over "
                                  "the .demat to bring it forward.",
                                  raw, texture.first );
                }
            }
        }

        // THE LAYER'S DYNAMIC PARAMETERS LAST (UE's material instance dynamic on the cloud material): what the
        // world sets at runtime — the weather's Coverage (Core::WeatherState) — by the SAME names the `.demat`
        // authors, through the same BuildCloudMaterialValues, so there is no second coverage path; last write
        // wins.
        for ( const auto& param : m_Data.DynamicParams )
            overrides.Params.push_back( param );
        m_Material = BuildCloudMaterialValues( schema, overrides );

        // AND THE ONE VALUE OF THE MATERIAL THAT IS NOT A NUMBER: the authored medium, which is a body of
        // code and therefore reaches the frame through the shader compiler rather than through the packed
        // parameter block. Costs one integer comparison per frame in the shipped case.
        ResolveMedium();

        // AND THE MEDIUM'S OWN VALUES, out of the SAME overrides — after ResolveMedium, so a frame in which
        // the medium changed resolves the values of the medium the pipelines were just rebuilt for rather
        // than of the one they were rebuilt from.
        ResolveMediumValues( overrides );
    }

    namespace
    {
        /// The DefaultTexture of the @p slot-th IMAGE property of @p schema. Counted over the schema in
        /// its own order because that order IS the binding layout — the same walk BuildCloudMediumValues
        /// makes, so slot i here and Textures[i] there are the same property.
        Core::Formats::DefaultTextureKind
        DefaultTextureOfSlot( const std::vector<Core::Formats::ShaderParam>& schema, std::size_t slot )
        {
            std::size_t seen = 0;
            for ( const Core::Formats::ShaderParam& p : schema )
            {
                if ( !p.IsTexture )
                    continue;
                if ( seen++ == slot )
                    return p.DefaultTexture;
            }
            return Core::Formats::DefaultTextureKind::White;
        }
    } // namespace

    void VolumetricCloudRenderer::ResolveMediumValues( const MaterialOverrides& overrides )
    {
        const std::vector<Core::Formats::ShaderParam>* schema = nullptr;
        if ( !m_Material.Medium.IsNull() )
        {
            if ( const auto shaderService = Runtime::ResourceRegistry::GetShaderService() )
                schema = shaderService->MediumSchemaOf( m_Material.Medium );
            // A null schema is not said here: MediumSourceOf was asked about the same handle a moment ago
            // in ResolveMedium and latches the one message for it. Two lines for one mistake is how a log
            // stops being read.
        }

        m_MediumValues            = schema ? BuildCloudMediumValues( *schema, overrides ) : CloudMediumValues{};
        m_MediumValuesFingerprint = CloudMediumValuesFingerprint( m_MediumValues );

        // THE IMAGES, RESOLVED EVERY FRAME AND BORROWED, one entry per DECLARED slot and NEVER NULL. The
        // count is what all four consumers bind, and a short vector would leave a declared sampler
        // unwritten — which invalidates the descriptor set and costs the whole dispatch, in silence.
        //
        // AN UNASSIGNED SLOT GETS THE SCHEMA'S OWN DEFAULT TEXTURE, not "some fallback". White is the
        // multiplicative identity and is what a Texture2D property declares unless its author said
        // otherwise, so a medium whose image the artist has not chosen yet draws the sky it drew before
        // the slot existed. A generic backend fallback would be a different colour by accident.
        m_MediumImages.assign( m_MediumValues.Textures.size(), nullptr );
        m_MediumTextureNames.clear();
        if ( schema != nullptr )
        {
            for ( const Core::Formats::ShaderParam& p : *schema )
            {
                if ( p.IsTexture )
                    m_MediumTextureNames.push_back( p.Name );
            }
        }

        auto* textures = Runtime::ResourceRegistry::GetTextureService();
        auto* images   = Runtime::ResourceRegistry::GetImageService();
        for ( std::size_t slot = 0; slot < m_MediumValues.Textures.size(); ++slot )
        {
            const Assets::AssetHandle handle = m_MediumValues.Textures[slot];
            if ( !handle.IsNull() && textures && images )
            {
                auto* texture = textures->Get( handle );
                if ( auto image = texture != nullptr ? std::static_pointer_cast<Image2D>(
                                                            images->Share( texture->GetImageHandle() ) )
                                                     : nullptr )
                {
                    m_MediumImages[slot] = image;
                    continue;
                }

                // NAMED, ONCE PER HANDLE. The slot still gets the declared default below — a medium must
                // keep drawing — but "the artist assigned a texture that is not there" and "the artist
                // assigned nothing" are the same picture, and this is the only place they differ.
                if ( m_WarnedMediumImages.insert( static_cast<uint64_t>( handle ) ).second )
                    LOG_ERROR( "[Clouds] the authored medium's image slot {} names texture {}, which does "
                               "not resolve to an image. That slot reads the schema's default texture in "
                               "all four programs until it does.",
                               slot, static_cast<uint64_t>( handle ) );
            }

            const Core::Formats::DefaultTextureKind kind = schema && slot < schema->size()
                                                                ? DefaultTextureOfSlot( *schema, slot )
                                                                : Core::Formats::DefaultTextureKind::White;
            m_MediumImages[slot]                         = DefaultTextures::Get().Share( kind );
        }
    }

    void VolumetricCloudRenderer::BindMedium( ComputePipeline*                pipeline,
                                              ShaderResources::StorageBuffer* buffer ) const
    {
        if ( !pipeline )
            return;

        // NOTHING IS BOUND FOR A MEDIUM THAT DECLARES NOTHING, and that is not an optimisation. The emitter
        // only declares a block for properties the five functions actually READ, so a medium with none
        // declares none — and this backend writes a descriptor for every binding a caller names, whether
        // the shader's layout has it or not. Binding into a layout that does not declare the slot is a
        // validation error, not a no-op.
        if ( !m_MediumValues.Params.empty() && buffer )
        {
            const auto uploaded =
                 buffer->SetData( m_MediumValues.Params.data(),
                                  static_cast<uint32_t>( m_MediumValues.Params.size() * sizeof( glm::vec4 ) ) );
            if ( !uploaded )
            {
                LOG_ERROR( "[Clouds] the authored medium's {} value(s) were not uploaded: {}. This "
                           "dispatch reads whatever the previous frame left in that buffer.",
                           m_MediumValues.Params.size(), uploaded.GetError() );
            }
            pipeline->SetStorageBuffer( Core::kCloudMediumParamsBinding, buffer );
        }
    }

    RenderPassDeclaration::BlockDeclaration
    VolumetricCloudRenderer::SampledMedium( RenderPassDeclaration::BlockDeclaration block ) const
    {
        // ResolveMediumValues leaves no null here — an unassigned slot already carries the schema's own
        // default image. The fallback is for the one case it cannot cover, a default-texture service that has
        // not come up. Each image is a separate texture read through the generated medium's one shared state,
        // u_CloudMediumSampler (ShaderGraph's Volume-domain emitter), with the global texture filter, REPEAT.
        const std::shared_ptr<Image> fallback =
             FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F );
        const std::size_t named = std::min( m_MediumImages.size(), m_MediumTextureNames.size() );
        for ( std::size_t slot = 0; slot < named; ++slot )
        {
            block.Texture( m_MediumTextureNames[slot],
                           m_MediumImages[slot] ? std::shared_ptr<Image>( m_MediumImages[slot] ) : fallback,
                           RDG::Access::SampledCompute, std::format( "Clouds.Medium{}", slot ) )
                 .Sampler( "u_CloudMediumSampler", GlobalTextureFilterSampler() );
        }
        return block;
    }

    void VolumetricCloudRenderer::BuildAuthoredPayload( const CloudGpuPayload& payload )
    {
        m_AuthoredPayload = CloudAuthoredPayload{};
        m_AuthoredAtlas.reset();

        if ( m_HeroClouds.empty() )
            return;

        auto* service = Runtime::ResourceRegistry::GetCloudModellingService();

        // WHICH BODIES THIS FRAME NEEDS, before a single instance is packed. The atlas has to exist before
        // an instance can say where in it to look, and TWO ENTITIES NAMING ONE `.dcmv` SHARE A SLAB —
        // which is what makes a scene of forty copies of one sculpted body cost 4.00 MiB and not 160.
        std::vector<Assets::AssetHandle> bodies;
        bodies.reserve( kCloudModellingAtlasMaxSlabs );

        for ( const HeroCloudInstance& hero : m_HeroClouds )
        {
            const auto body = service->RequireBody( hero.Data.Volume );

            // A BODY STILL BEING READ STOPS THE WHOLE ATLAS, not just its own slab. Building an atlas
            // without it would upload up to twelve megabytes and then have to upload them again on the
            // frame the body landed — and in between the scene would show some of its hero clouds and not
            // others, which looks like a scene with missing content rather than a scene still loading.
            if ( body.IsPending() )
            {
                if ( !m_HeroWaiting )
                {
                    m_HeroWaiting = true;
                    LOG_INFO( "[Clouds] Waiting for hero cloud '{}' body {}; the read is on a worker and "
                              "no hero cloud draws until the whole set has landed.",
                              hero.Name, static_cast<uint64_t>( body.Handle() ) );
                }
                return;
            }

            if ( !body.IsValid() )
                continue; // empty slot, or already logged by the service with the handle in the message

            if ( std::find( bodies.begin(), bodies.end(), hero.Data.Volume ) != bodies.end() )
                continue;

            if ( bodies.size() >= kCloudModellingAtlasMaxSlabs )
            {
                // NOT a silent drop, and the number that ran out is named: it is the ATLAS and not the
                // instance list, so the fix is fewer DIFFERENT bodies rather than fewer entities.
                if ( !m_AuthoredBodiesWarned )
                {
                    LOG_WARN( "[Clouds] Hero cloud '{}' names the {}th different modelling volume in this "
                              "scene and the atlas holds {}; it is not drawn. Instances that SHARE a .dcmv "
                              "are free — it is the number of DIFFERENT bodies that is capped, by the "
                              "4.00 MiB each of them costs.",
                              hero.Name, bodies.size() + 1, kCloudModellingAtlasMaxSlabs );
                    m_AuthoredBodiesWarned = true;
                }
                continue;
            }

            bodies.push_back( hero.Data.Volume );
        }

        m_HeroWaiting = false;

        if ( bodies.empty() )
            return;

        // A CANONICAL SLAB ORDER, so that the atlas is a function of WHICH bodies are live and not of the
        // order the ECS happened to hand them over in. The service rebuilds when the list changes, and a
        // list that reorders itself would rebuild — twelve megabytes of upload — on a frame where nothing
        // about the sky moved.
        //
        // MEASURED RATHER THAN FEARED: without this sort the engine's own log line already appeared
        // exactly ONCE in a 900-frame render, so EnTT's view order is stable in practice. Sorting makes
        // it stable by construction, which is a cheaper guarantee than the observation — at most eight
        // handles, once a frame.
        std::sort( bodies.begin(), bodies.end() );

        const Runtime::CloudModellingAtlasBinding atlas = service->EnsureAtlas( bodies );
        if ( !atlas.Volume )
            return; // already logged by the service

        m_AuthoredAtlas             = atlas.Volume;
        m_AuthoredPayload.SlabCount = static_cast<int32_t>( atlas.SlabCount );

        for ( const HeroCloudInstance& hero : m_HeroClouds )
        {
            const auto slab = std::find( bodies.begin(), bodies.end(), hero.Data.Volume );
            if ( slab == bodies.end() )
                continue; // unregistered, or past the atlas — both already said above

            const uint32_t slot = static_cast<uint32_t>( std::distance( bodies.begin(), slab ) );

            const CloudAuthoredPackResult packed = PackCloudAuthoredInstance(
                 hero.WorldTransform, service->GetSizeKm( hero.Data.Volume ), payload.Layer.y, hero.Data,
                 CloudAuthoredAtlasSlabBaseW( slot, atlas.SlabCount ) );

            if ( !packed.Valid )
            {
                LOG_ERROR( "[Clouds] Hero cloud '{}' has a degenerate transform — a scale of zero on some "
                           "axis leaves no way back from the world into the body — so it is not drawn.",
                           hero.Name );
                continue;
            }

            // THE RELATION, STATED RATHER THAN HOPED FOR. The march only samples between the two shells,
            // so a body whose top is above the layer's top is not clipped by anything the artist can see
            // — it is simply never sampled there, and the symptom is a cumulus with its crown sliced flat
            // by an altitude nobody set. Both numbers are in the message, which is the house pattern for
            // two values obliged to agree (desert-engine-verify section 4).
            if ( !CloudAuthoredInstanceFitsLayer( packed.Instance, payload.Layer.z ) && !m_AuthoredFitWarned )
            {
                LOG_WARN( "[Clouds] Hero cloud '{}' spans {:.2f} to {:.2f} km above the layer's base, and the "
                          "layer is {:.2f} km thick starting at {:.2f} km. The part outside the shell is "
                          "never marched, so the body will look cut off there. Move the entity, or give the "
                          "layer a cloud type whose altitudes contain it.",
                          hero.Name, packed.Instance.BoundsMin.y, packed.Instance.BoundsMax.y, payload.Layer.z,
                          payload.Layer.y );
                m_AuthoredFitWarned = true;
            }

            m_AuthoredPayload.Instances[m_AuthoredPayload.Count] = packed.Instance;
            ++m_AuthoredPayload.Count;

            if ( static_cast<uint32_t>( m_AuthoredPayload.Count ) >= kCloudAuthoredSlots )
            {
                // The OTHER limit, and it is worth telling them apart: this one is the march's, paid at
                // every field sample by every instance, where the atlas's is memory.
                if ( m_HeroClouds.size() > kCloudAuthoredSlots && !m_AuthoredCrowdWarned )
                {
                    LOG_WARN( "[Clouds] {} hero clouds are live and the march carries {}; the rest are not "
                              "drawn. Each instance costs a bounds test at every sample of every view ray "
                              "AND of every shadow ray, which is what the limit is for.",
                              m_HeroClouds.size(), kCloudAuthoredSlots );
                    m_AuthoredCrowdWarned = true;
                }
                break;
            }
        }

        // THE RELATION BETWEEN THE PAYLOAD AND THE IMAGE, asserted where both are in hand. An instance
        // packed for one atlas and dispatched against another reads a body at the wrong depth and draws a
        // cloud nobody sculpted, silently — see Graphic::CloudAuthoredPayloadIsBindable.
        if ( !CloudAuthoredPayloadIsBindable( m_AuthoredPayload, atlas.SlabCount ) )
        {
            LOG_ERROR( "[Clouds] The authored payload ({} instances over {} slabs) does not address the "
                       "atlas that is about to be bound ({} slabs); no hero cloud is drawn this frame.",
                       m_AuthoredPayload.Count, m_AuthoredPayload.SlabCount, atlas.SlabCount );
            m_AuthoredPayload = CloudAuthoredPayload{};
            m_AuthoredAtlas.reset();
        }
    }

    void VolumetricCloudRenderer::UpdateTraceExtent( uint32_t halfWidth, uint32_t halfHeight )
    {
        if ( halfWidth == m_HalfWidth && halfHeight == m_HalfHeight )
            return;
        m_HalfWidth  = halfWidth;
        m_HalfHeight = halfHeight;

        // The cost is announced once per size, not discovered in a memory graph later. Nothing is allocated here:
        // the trace pair is a transient of the frame graph, the reconstruction pair a transient extracted into
        // the view (SceneViewState::Clouds) that holds two of them across frames. RGBA16F for the guide because
        // Core::Formats offers no two-channel float; half a kilometre resolves to about thirty metres.
        const uint32_t traceWidth  = HalfExtent( halfWidth );
        const uint32_t traceHeight = HalfExtent( halfHeight );
        const double   traceMiB    = BytesToMiB(
             Core::Formats::CalculateImageSize( traceWidth, traceHeight, ViewTargetFormats::kCloudTrace ) );
        const double halfMiB = BytesToMiB(
             Core::Formats::CalculateImageSize( halfWidth, halfHeight, ViewTargetFormats::kCloudTrace ) );
        LOG_INFO( "[Clouds] Trace {}x{} RGBA16F ({:.2f} MiB each, scatter + guide, frame-graph transients) — a "
                  "QUARTER of the {}x{} view, one jittered sub-pixel per frame.",
                  traceWidth, traceHeight, traceMiB, halfWidth * 2u, halfHeight * 2u );
        LOG_INFO( "[Clouds] Reconstruction {}x{} RGBA16F ({:.2f} MiB each, scatter + guide) — the view's history: "
                  "{:.2f} MiB held across frames, {:.2f} MiB with this frame's pair.",
                  halfWidth, halfHeight, halfMiB, 2.0 * halfMiB, 4.0 * halfMiB );
    }

    bool VolumetricCloudRenderer::EnsureNoiseVolumes( const Assets::AssetHandle ( &handles )[kCloudSpeciesSlots],
                                                      uint32_t speciesCount )
    {
        auto* service = Runtime::ResourceRegistry::GetCloudNoiseService();
        auto* types   = Runtime::ResourceRegistry::GetCloudTypeService();

        // Asked EVERY frame rather than cached behind a "have I got one" flag, because the answer changes
        // for three independent reasons now — the artist picks a different cloud TYPE, the type they
        // already picked is edited to name a different volume, or the volume itself is re-baked and
        // hot-reloaded. A flag would answer none of them. Every lookup is a hash-map probe against a
        // handle; they are not worth a cache that can be wrong.
        //
        // THE VOLUME COMES THROUGH THE TYPE, which is the one structural change T1 made to this path: the
        // character of a cloud's edge is a property of what kind of cloud it is, so the type names it and
        // the layer does not. An empty slot on either side lands on the built-in default volume.
        //
        // ONE VOLUME PER SPECIES, AND THIS FUNCTION USED TO TAKE THE FIRST NON-EMPTY SLOT'S AND GIVE IT TO
        // THE WHOLE LAYER. That was the programme's last recorded debt, and it was a dead setting: three
        // of a layer's four slots could name a volume the frame never read, silently, while the Cloud Type
        // panel's own tooltip promised the opposite. What paid for it is four descriptors instead of one —
        // and they cost only the volumes the layer names: a `.dcnv` is read when named (AL1-2).
        //
        // THE HANDLES ARE THE SPECIES' AND NOT THE SLOTS', which matters when a layer's filled slots have
        // holes in them: ResolveSpecies has already compacted slot 3 down to species 1, and the march
        // indexes SpeciesNoise by the same compacted number. Two statements of that compaction is exactly
        // the defect this task found in the code it replaced.
        Assets::AssetHandle perSpecies[kCloudSpeciesSlots] = {};

        const uint32_t species = std::min( speciesCount, kCloudSpeciesSlots );
        // A TYPE STILL BEING READ (AL1-7) names no volume yet; asking now would answer "the default volume"
        // and start a read of it that the type may be about to override. Not drawn, like a pending volume.
        for ( uint32_t k = 0; k < species; ++k )
            if ( types->IsPending( handles[k] ) )
                return false;
        for ( uint32_t k = 0; k < species; ++k )
            perSpecies[k] = types->GetNoiseVolume( handles[k] );

        m_NoiseSlots  = ResolveCloudNoiseVolumes( perSpecies, species );
        m_NoiseNeeded = m_NoiseSlots.DistinctCount;

        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
        {
            const Assets::AssetRef<Image3D> volume = service->Require( m_NoiseSlots.Volume[slot] );

            // PENDING IS NOT MISSING, AND THIS BRANCH IS THE WHOLE POINT OF THE THREE-STATE ANSWER.
            //
            // Until the cloud kinds became demand-driven, a null from the service could only mean "the
            // scene names a .dcnv that is not in the project", so one branch was enough and it logged an
            // error. Now the same absence also means "a worker is reading it", which happens on every
            // scene load and is not a defect. Taking the old branch for it would put an error in the log
            // for normal loading, latch this renderer's failure flag over a condition that clears
            // itself, and teach the reader to ignore the one line that reports a genuinely broken
            // reference.
            //
            // The ACTION is the same — do not draw this layer — and that is not an argument for merging
            // the states: it is why merging them was survivable for so long and why nobody would have
            // noticed. The log is the difference, and the log is what the next investigation reads.
            if ( volume.IsPending() )
            {
                if ( !m_NoiseWaiting )
                {
                    m_NoiseWaiting = true;
                    LOG_INFO( "[Clouds] Waiting for noise volume {} for slot {} of this layer; the read is "
                              "on a worker and this view draws no clouds until it lands.",
                              static_cast<uint64_t>( volume.Handle() ), slot );
                }
                return false;
            }

            if ( !volume.IsValid() )
            {
                // The service has already logged which volume is missing and why. Latched so a scene with a
                // broken reference does not print once per frame forever.
                if ( !m_NoiseFailed )
                {
                    m_NoiseFailed = true;
                    LOG_ERROR( "[Clouds] No noise volume could be resolved for slot {} of this layer (of {} "
                               "distinct volumes over {} species); the clouds will not render for this view "
                               "until one is registered.",
                               slot, m_NoiseNeeded, species );
                }
                return false;
            }

            m_NoiseVolume[slot] = volume.Share();
        }

        // Cleared as soon as the volumes do resolve: the failure above is a state of the SCENE, not of this
        // renderer, and dropping a project's clouds for the rest of the session because one scene was
        // opened with a stale reference is the kind of latch that reads as a broken build. The waiting
        // latch clears for the same reason and one step earlier — a volume that arrives must be able to
        // put the log back to quiet, or the next wait says nothing.
        m_NoiseFailed  = false;
        m_NoiseWaiting = false;

        return true;
    }

    std::vector<ComputeNodeDeclaration>
    VolumetricCloudRenderer::DeclareFrameNodes( RDG::Builder& graph, const ViewFrame& frame,
                                                VolumetricCloudHistory& history )
    {
        std::vector<ComputeNodeDeclaration> nodes;
        m_FrameResult = {};
        m_PendingResult.reset();

        // Cleared with it, and read by the NEXT frame's environment bake rather than by anything here: a
        // frame that skipped this pass must leave "there is no sky-occlusion volume to read" behind it,
        // not the answer of the last frame that ran.
        m_SkyOcclusionValid = false;

        if ( !m_MarchPipeline || !m_ResolvePipeline || !m_CompositePipeline || !m_ParamsBuffer ||
             !m_ResolveParamsBuffer )
            return nodes;

        if ( !m_Present || !m_Data.Enabled )
            return nodes;

        // Without a sky there is no sun to light the clouds and no ambient to fill their shadowed sides.
        // Marching anyway would draw black cut-outs across the frame, which is worse than drawing nothing
        // and much harder to diagnose — so the layer is simply absent, exactly as the sky's own contract
        // says a consumer must treat Valid == false.
        const AtmosphereEnv& atmosphere = m_SceneRenderer->GetAtmosphere();
        if ( !atmosphere.Valid )
            return nodes;

        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return nodes;

        const auto target = m_TargetFramebuffer.lock();
        if ( !target || target->GetDepthAttachmentCount() == 0 )
            return nodes;

        UpdateTraceExtent( HalfExtent( target->GetFramebufferWidth() ),
                           HalfExtent( target->GetFramebufferHeight() ) );

        // RESOLVED AGAIN RATHER THAN CACHED FROM EnsureModellingVolume. Two hash-map probes per slot
        // against handles that have not moved is not worth a member that can disagree with the volume it
        // was built beside — and the one thing that must not diverge is exactly the set whose channels the
        // volume carries. BuildFieldPayload is the one place that sequence is written.
        CloudGpuPayload payload{};
        if ( !BuildFieldPayload( payload ) )
            return nodes;

        // From here on this frame decides the sky-occlusion volume, whichever way it goes.
        m_SkyOcclusionDecided = true;

        // BUILD TIME: the march exists only when these uploads land, and they fill this frame's buffer slot.
        const auto traceParams = m_ParamsBuffer->SetData( &payload, static_cast<uint32_t>( sizeof( payload ) ) );

        // Slot A. Rebuilt here rather than reused from the shadow map's call: the two dispatches sit on
        // opposite sides of the render graph and the shadow map may not have run at all this frame (no
        // casting, no strength, a failed allocation), so a payload built there is a payload that might
        // not exist. It is a handful of matrix inversions for at most four entities.
        BuildAuthoredPayload( payload );
        const auto traceAuthored =
             m_AuthoredBuffer->SetData( &m_AuthoredPayload, static_cast<uint32_t>( sizeof( m_AuthoredPayload ) ) );

        // NO BLOCK, NO MARCH. Both dispatches below — the sky-light occlusion volume and the trace
        // itself — read this one upload, so half of it arriving is worse than none: the occlusion volume
        // would be built from this frame's field and the march from last frame's, and the two are then
        // asserting different clouds at the same time.
        if ( !traceParams.IsSuccess() || !traceAuthored.IsSuccess() )
        {
            LOG_ERROR( "[Clouds] the cloud march does not run this frame; its parameters were not "
                       "uploaded. field: {} | authored: {}",
                       traceParams.IsSuccess() ? "ok" : traceParams.GetError(),
                       traceAuthored.IsSuccess() ? "ok" : traceAuthored.GetError() );
            return nodes;
        }

        const CloudSubPixel subPixel       = CloudTraceSubPixel( m_FrameIndex );

        // ---- THE SKY-LIGHT OCCLUSION VOLUME ----------------------------------------------------------
        //
        // HERE AND NOT IN ITS OWN Execute*, and the placement is the whole of its correctness. It is
        // dispatched AFTER m_ParamsBuffer was written just above and BEFORE the march below, so the two
        // dispatches read ONE upload of one block — where the shadow map, which sits on the far side of
        // the render graph, needs a second buffer for exactly the bytes this one shares. It writes an
        // image the march then samples, which fixes the order and forbids reordering them.
        //
        // ZERO COST WHEN OFF: no allocation, no dispatch, and the march's gate is a push constant of 0, so
        // a scene without the flag is the frame it was before this pass existed.
        const bool wantsSkyOcclusion = m_Data.SkyOcclusionVolume;
        bool       skyOcclusionReady = false;

        if ( wantsSkyOcclusion && EnsureSkyOcclusionVolume() )
        {

            ComputeNodeDeclaration occlusion;
            occlusion.Name = "Clouds: SkyOcclusion";
            // SETUP: the pipeline route (the renderer's own sampled volumes and buffers) and the node's block over
            // it; the volume it writes is the block's entry.
            m_SkyOcclusionPipeline->SetStorageBuffer( kCloudSkyOcclusionParamsBinding, m_ParamsBuffer.get() );
            m_SkyOcclusionPipeline->SetStorageBuffer( kCloudSkyOcclusionAuthoredBinding, m_AuthoredBuffer.get() );
            // The same buffer the march binds, and legitimately so: this dispatch is issued inside
            // ExecuteInFrame between the march's own upload and the march itself, which is exactly the
            // window m_ParamsBuffer is already shared across.
            BindMedium( m_SkyOcclusionPipeline.get(), m_MediumParamsBuffer.get() );
            SampledMedium( SampledVolumes( DeclareComputeBlock( occlusion.Access, m_SkyOcclusionPipeline.get(),
                                                                m_SkyOcclusionLayout, 0 ) ) )
                 .Storage( "u_CloudSkyOcclusion", m_SkyOcclusionVolume, RDG::Access::StorageWrite,
                           "Clouds.SkyOcclusion" );
            occlusion.Record = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
            {
                DESERT_PROFILE_PASS( "Clouds: SkyOcclusion" );
                // ONE INVOCATION PER COLUMN — the altitude axis is walked inside the shader, because the whole
                // point of the pass is that a column's optical depth accumulates downward and a thread per
                // texel would have to re-integrate everything above it.
                const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                return Renderer::DispatchCompute( bindings, *m_SkyOcclusionPipeline,
                                                  GroupCount( kCloudSkyOcclusionResolution, kMarchWorkGroupSize ),
                                                  GroupCount( kCloudSkyOcclusionResolution, kMarchWorkGroupSize ),
                                                  1 );
            };
            nodes.push_back( std::move( occlusion ) );

            skyOcclusionReady = true;
        }

        // Published for the NEXT frame's environment bake, which runs before this pass and can therefore
        // only ask about the volume this frame leaves behind.
        m_SkyOcclusionValid = skyOcclusionReady;

        CloudPush push{};
        // The jittered inverse: the march's depth test reads the scene depth rasterised with the jittered matrix,
        // and its ray directions jitter with the geometry they are resolved together with (ViewFrame.hpp).
        push.InverseViewProjection = frame.InvJitteredViewProjection;
        push.CameraPosition = glm::vec4( frame.CameraPosition, static_cast<float>( m_FrameIndex & 0xFFFFu ) );
        push.Trace          = glm::vec4( static_cast<float>( subPixel.X ), static_cast<float>( subPixel.Y ),
                                         static_cast<float>( m_HalfWidth ), static_cast<float>( m_HalfHeight ) );
        // THE GATES ARE "WAS IT WRITTEN", not "was it asked for". A layer whose flag is on but whose volume
        // failed to allocate must fall back to the profile term, not read an image nobody filled; and a
        // layer that asked for per-sample sun transmittance under an artistic-gradient sky has no LUT to
        // read at all.
        //
        // THE SECOND GATE IS NOT DECIDED HERE. CloudUsesPerSampleSunTransmittance is what PackCloudParams
        // asked when it chose which illuminance to put in the block, and asking it again with the same two
        // arguments is what keeps the two answers from being two answers.
        const bool perSampleSun = CloudUsesPerSampleSunTransmittance( m_Data, atmosphere );

        // The radii are written whatever the gate says. They are two numbers on a wire that is paid for
        // either way — a push constant is laid out in vec4s — and a lane that changes meaning with a gate
        // is harder to read than one that is simply ignored.
        push.Frame =
             glm::vec4( skyOcclusionReady ? 1.0f : 0.0f, perSampleSun ? 1.0f : 0.0f,
                        atmosphere.TransmittanceLutBottomRadiusKm, atmosphere.TransmittanceLutTopRadiusKm );
        push.Visualize = glm::vec4( static_cast<float>( RenderConfig::CloudVisualize.load() ), 0.0f, 0.0f, 0.0f );

        // The march: samples the scene depth, the cloud volumes, the sky occlusion written above and the
        // atmosphere's LUTs; writes the half-resolution trace and its guide.
        // Single-sample: the scene depth at MSAA 1, SceneDepthResolved at MSAA > 1 (a sampler2D over a
        // multisampled image is invalid). With a depth target it always exists: at MSAA > 1 the resolve is part
        // of the renderer, and a resolve that failed to build said so at startup and is said again here.
        const std::shared_ptr<Image2D> depth = m_SceneRenderer->GetComputeSceneDepth();
        if ( !depth )
        {
            LOG_ERROR( "[Clouds] the cloud march has no single-sample scene depth: SceneDepthResolve was not "
                       "built for the multisampled scene target (see the startup error)" );
            return {};
        }
        const uint32_t traceWidth  = HalfExtent( m_HalfWidth );
        const uint32_t traceHeight = HalfExtent( m_HalfHeight );
        // The march's two QUARTER-resolution outputs live this frame only: the scatter (premultiplied radiance
        // in .rgb, transmittance in .a) and the depth guide beside it (.x cloud front distance, .y scene
        // distance, both kilometres). RGBA16F for the guide because Core::Formats offers no two-channel float.
        const RDG::TextureDesc traceDesc{ .Size   = RDG::Extent3D{ .Width = traceWidth, .Height = traceHeight },
                                          .Format = ViewTargetFormats::kCloudTrace };
        const RDG::TextureRef  trace      = graph.CreateTexture( traceDesc, "Clouds.Trace" );
        const RDG::TextureRef  traceGuide = graph.CreateTexture( traceDesc, "Clouds.TraceGuide" );
        ComputeNodeDeclaration march;
        march.Name = "Clouds: March";
        // SETUP: everything but the graph's two transients is the renderer's own (or the sky's), bound by the
        // pipeline's setters here; the two transients are the block's entries, each the declaration of its write.
        m_MarchPipeline->SetStorageBuffer( kCloudParamsBinding, m_ParamsBuffer.get() );
        // The noise, modelling and atlas volumes are the block's entries (SampledVolumes).

        // Slot A's instance list and the atlas of sculpted bodies it addresses. The image is ALWAYS bound,
        // even when the count is zero and nothing will read it, on exactly the terms the two samplers above
        // are bound on: a declared sampler with no image is an invalid descriptor set, and this backend
        // answers an invalid set by skipping the dispatch — every cloud in the frame would disappear with
        // nothing in the log, which is a rake this subsystem has already stood on.
        m_MarchPipeline->SetStorageBuffer( kCloudAuthoredBinding, m_AuthoredBuffer.get() );
        BindMedium( m_MarchPipeline.get(), m_MediumParamsBuffer.get() );

        // The sky's three images, entries of the block on the same always-bound terms: a declared sampler with
        // no image is an invalid descriptor set, so a null handle (the artistic gradient, LUTs not baked, a
        // sky switched off) is the engine fallback, and the payload's gates (Ambient.w, Aerial.z,
        // push.Frame.y) say it is never read. The distant sky light is what lights a cloud from the physical
        // sky; the aerial-perspective volume is what makes a cloud at the horizon the colour of the sky
        // instead of an opaque white wall; the transmittance LUT re-evaluates the sun at each sample.
        const std::shared_ptr<Image> fallback2D =
             FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F );
        const std::shared_ptr<Image> distantSkyLight =
             atmosphere.DistantSkyLight ? std::shared_ptr<Image>( atmosphere.DistantSkyLight ) : fallback2D;
        const std::shared_ptr<Image> aerialPerspective =
             atmosphere.AerialPerspectiveVolume
                  ? std::shared_ptr<Image>( atmosphere.AerialPerspectiveVolume )
                  : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F );
        const std::shared_ptr<Image> sunTransmittanceLut =
             atmosphere.TransmittanceLut ? std::shared_ptr<Image>( atmosphere.TransmittanceLut ) : fallback2D;
        // The block's entries: the two transients it writes, the scene depth and the sky-light occlusion volume,
        // on the same always-bound terms as the samplers above -- when the layer does not want it the image does
        // not exist at all, so the fallback is the entry and push.Frame.x is 0. Each is read with the sampler the
        // image carries as its own (depth: the global texture filter, REPEAT; a volume: VolumeSampler).
        const std::shared_ptr<Image> skyOcclusion =
             skyOcclusionReady
                  ? std::shared_ptr<Image>( m_SkyOcclusionVolume )
                  : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F );
        SampledMedium( SampledVolumes( DeclareComputeBlock( march.Access, m_MarchPipeline.get(), m_MarchLayout,
                                                            static_cast<uint32_t>( sizeof( CloudPush ) ) ) ) )
             .Storage( "u_CloudScatter", trace, RDG::Access::StorageWrite )
             .Storage( "u_CloudGuide", traceGuide, RDG::Access::StorageWrite )
             .Texture( "u_SceneDepth", depth, RDG::Access::SampledCompute, "SceneDepth.Compute" )
             .Sampler( "u_GlobalFilterSampler", GlobalTextureFilterSampler() )
             .Texture( "u_CloudSkyOcclusionVolume", skyOcclusion, RDG::Access::SampledCompute,
                       skyOcclusionReady ? "Clouds.SkyOcclusion" : "Clouds.SkyOcclusionFallback" )
             .Sampler( "u_LinearRepeatSampler", VolumeSampler() )
             .Texture( "u_DistantSkyLight", distantSkyLight, RDG::Access::SampledCompute,
                       atmosphere.DistantSkyLight ? "Sky.DistantLight" : "Clouds.DistantSkyLightFallback" )
             .Texture( "u_CloudAerialPerspective", aerialPerspective, RDG::Access::SampledCompute,
                       atmosphere.AerialPerspectiveVolume ? "Sky.AerialPerspectiveLut"
                                                          : "Clouds.AerialPerspectiveFallback" )
             .Texture( "u_CloudSunTransmittanceLut", sunTransmittanceLut, RDG::Access::SampledCompute,
                       atmosphere.TransmittanceLut ? "Sky.TransmittanceLut"
                                                   : "Clouds.SunTransmittanceLutFallback" );
        march.Record = [this, push, traceWidth, traceHeight]( RDG::PassContext& context,
                                                              const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            DESERT_PROFILE_PASS( "Clouds: March" );
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            return Renderer::DispatchCompute( bindings, *m_MarchPipeline,
                                              GroupCount( traceWidth, kMarchWorkGroupSize ),
                                              GroupCount( traceHeight, kMarchWorkGroupSize ), 1 );
        };
        nodes.push_back( std::move( march ) );

        // S2 — THE TEMPORAL RECONSTRUCTION, through the VIEW's history (SceneViewState::Clouds): last frame's
        // pair registered back into this graph, this frame's a transient pair extracted for the next. Readable
        // only when the view says its previous frame may be reprojected at all (a cut, a scene change, a
        // resize: ViewFrame::HistoryValid) and the view still holds the pair its previous frame resolved.
        const ViewExtent                   halfExtent{ .Width = m_HalfWidth, .Height = m_HalfHeight };
        const VolumetricCloudHistory::Refs previous = history.RegisterPrevious( graph, halfExtent );
        bool historyReadable = frame.HistoryValid() && previous.Scatter.IsValid() && previous.Guide.IsValid();

        CloudResolveParams resolve{};
        resolve.InverseViewProjection = push.InverseViewProjection;
        resolve.PrevViewProjection    = frame.PrevViewProjection;
        resolve.CameraPosition        = frame.CameraPosition;
        resolve.HistoryValid          = historyReadable ? 1.0f : 0.0f;
        resolve.SubPixelOffset =
             glm::ivec2( static_cast<int32_t>( subPixel.X ), static_cast<int32_t>( subPixel.Y ) );
        // BUILD TIME, like the trace's upload: the resolve node exists only when it lands.
        const auto resolveParams =
             m_ResolveParamsBuffer->SetData( &resolve, static_cast<uint32_t>( sizeof( resolve ) ) );
        if ( !resolveParams.IsSuccess() )
        {
            // AND THE HISTORY GOES WITH IT: no pair is created this frame, so the view's EndFrame keeps none and
            // the next frame restarts the reconstruction rather than reprojecting a pair its previous matrices
            // never described.
            LOG_ERROR( "[Clouds] the temporal reconstruction is skipped and the history dropped; its "
                       "parameters were not uploaded: {}",
                       resolveParams.GetError() );
            return nodes;
        }

        // The temporal resolve: samples this frame's trace and, when valid, last frame's history; writes this
        // frame's history slot, which the composite (CloudComposite, a Transparency raster node) samples.
        ComputeNodeDeclaration temporal;
        temporal.Name = "Clouds: TemporalResolve";
        // SETUP: the parameters are the renderer's own buffer, bound by the pipeline's setter here; this frame's
        // trace pair, last frame's history (or the fallback) and this frame's history slot are the block's
        // entries.
        m_ResolvePipeline->SetStorageBuffer( kCloudResolveParamsBinding, m_ResolveParamsBuffer.get() );
        // THE HISTORY, OR SOMETHING REAL IN ITS PLACE. With no previous pair the sampler still needs an image
        // (a declared sampler with no image is an invalid descriptor set, which this backend answers by
        // skipping the whole dispatch): the engine's fallback, and CloudResolveParams::HistoryValid tells the
        // shader to ignore it. RGBA8F because FallbackTextures only provides RGBA8F and RGBA32F, and the
        // sampler reads floats either way.
        const std::shared_ptr<Image> fallback =
             FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F );
        // This frame's pair, written by the resolve and sampled by the composite: transients of this graph that
        // the view extracts for the next frame.
        const VolumetricCloudHistory::Refs current = history.CreateCurrent( graph, halfExtent );
        // This frame's trace pair, by shader name. Linear + clamp: the reconstruction texelFetches the texel it
        // owns and bilinearly upsamples (texture(..., traceUv)) the ones it does not.
        RenderPassDeclaration::BlockDeclaration resolveBlock =
             DeclareComputeBlock( temporal.Access, m_ResolvePipeline.get(), m_ResolveLayout, 0 );
        resolveBlock.Texture( "u_CloudTrace", trace, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
             .Texture( "u_CloudTraceGuide", traceGuide, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             // The march's depth image again: the resolve gives every half-res pixel its OWN scene distance rather
             // than the one the jitter cycle's traced sub-pixel saw (CloudTemporalResolve.shader, u_SceneDepth).
             .Texture( "u_SceneDepth", depth, RDG::Access::SampledCompute, "SceneDepth.Compute" )
             .Sampler( "u_GlobalFilterSampler", GlobalTextureFilterSampler() )
             .Storage( "u_ReconstructedScatter", current.Scatter, RDG::Access::StorageWrite )
             .Storage( "u_ReconstructedGuide", current.Guide, RDG::Access::StorageWrite );
        if ( historyReadable )
            resolveBlock
                 .Texture( "u_CloudHistory", previous.Scatter, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Texture( "u_CloudHistoryGuide", previous.Guide, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() );
        else
            resolveBlock
                 .Texture( "u_CloudHistory", fallback, RDG::Access::SampledCompute, "Clouds.HistoryFallback" )
                 .Texture( "u_CloudHistoryGuide", fallback, RDG::Access::SampledCompute,
                           "Clouds.HistoryFallback" );
        temporal.Record = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            DESERT_PROFILE_PASS( "Clouds: TemporalResolve" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::DispatchCompute( bindings, *m_ResolvePipeline,
                                              GroupCount( m_HalfWidth, kMarchWorkGroupSize ),
                                              GroupCount( m_HalfHeight, kMarchWorkGroupSize ), 1 );
        };
        nodes.push_back( std::move( temporal ) );

        // Published by SettleFrameNodes, i.e. once the graph accepted the nodes.
        m_PendingResult = current;
        return nodes;
    }

    void VolumetricCloudRenderer::SettleFrameNodes( bool accepted )
    {
        const std::optional<FrameResult> pending = std::exchange( m_PendingResult, std::nullopt );
        if ( !accepted )
        {
            // Nothing of this frame is recorded: no occlusion volume and no resolve. The view's pair has no
            // writer (the caller sets none), so its EndFrame keeps no history of this frame.
            m_SkyOcclusionValid = false;
            m_FrameResult       = {};
            return;
        }
        if ( !pending )
            return;
        m_FrameResult = *pending;
        ++m_FrameIndex;
    }

    void VolumetricCloudRenderer::AddInputUploads( RDG::Builder& graph, FrameTextures& textures )
    {
        // UE: a texture upload recorded on the graph (QueueBufferUpload into a staging buffer, then a copy into
        // the texture) — never a submit-and-wait. The image is imported through @p textures, the one registration
        // of it this graph has, so the block entries and the sky capture that sample it later in the graph read
        // the same graph texture and are ordered after this copy.
        for ( PendingUpload& upload : m_PendingUploads )
        {
            const RDG::TextureRef target = textures.Import( upload.Image, upload.Name );
            if ( !target.IsValid() )
            {
                // Import already said why; the input is re-baked next frame rather than sampled unwritten.
                *upload.Stale = true;
                continue;
            }
            const RDG::BufferRef staging = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = upload.Texels.size() },
                                                               std::format( "{}.Texels", upload.Name ) );
            graph.QueueBufferUpload( staging, upload.Texels );
            graph.AddPass(
                 std::format( "Clouds: Upload {}", upload.Name ), RDG::PassFlags::Copy,
                 [staging, target]( RDG::PassBuilder& pass )
                 {
                     pass.Read( staging, RDG::Access::CopySrc );
                     pass.Write( target, RDG::Access::CopyDst );
                 },
                 [staging, target]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Clouds: Upload" );
                     return Renderer::CopyGraphBufferToTexture( context, staging, target );
                 } );
            // A fault that removes the copy leaves the image unwritten: its owner re-bakes it next frame.
            bool* stale = upload.Stale;
            textures.MarkHistory( target, [stale]() { *stale = true; } );
        }
        m_PendingUploads.clear();
    }

    RenderPassDeclaration::BlockDeclaration
    VolumetricCloudRenderer::SampledVolumes( RenderPassDeclaration::BlockDeclaration block ) const
    {
        // ALL FOUR noise slots, always, whatever the layer needs: slots past the distinct count repeat slot 0's
        // image (ResolveCloudNoiseVolumes), which the frame imports once, so two entries read one graph texture
        // in one state. The atlas is ALWAYS an entry, the 3D fallback while there is none -- a declared sampler
        // with no image is an invalid descriptor set. Each with the sampler the volume carries as its own.
        static constexpr const char* kNoiseNames[kCloudSpeciesSlots] = { "u_CloudNoise", "u_CloudNoise1",
                                                                         "u_CloudNoise2", "u_CloudNoise3" };
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
            block.Texture( kNoiseNames[slot], m_NoiseVolume[slot], RDG::Access::SampledCompute,
                           std::format( "Clouds.Noise{}", slot ) )
                 .Sampler( "u_LinearRepeatSampler", VolumeSampler() );
        const std::shared_ptr<Image> atlas =
             m_AuthoredAtlas ? std::shared_ptr<Image>( m_AuthoredAtlas )
                             : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F );
        // The clipmap's one image (three toroidal levels), the world weather map and the painted layout's two
        // textures: always present once EnsureInputs has run, each LINEAR and REPEAT -- the weather and the
        // layout tile over their own periods, and a "Nearest" read would cut the sky along texel edges.
        block.Texture( "u_CloudModelling", m_Clipmap.GetImage(), RDG::Access::SampledCompute, "Clouds.Modelling" )
             .Sampler( "u_LinearRepeatSampler", VolumeSampler() )
             .Texture( "u_CloudFarWeather", m_FarWeatherMap, RDG::Access::SampledCompute, "Clouds.FarWeather" )
             .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
             .Texture( "u_CloudLayoutPattern", m_LayoutPatternTexture, RDG::Access::SampledCompute,
                       "Clouds.LayoutPattern" )
             .Texture( "u_CloudLayoutMask", m_LayoutMaskTexture, RDG::Access::SampledCompute, "Clouds.LayoutMask" )
             .Texture( "u_CloudAuthoredAtlas", atlas, RDG::Access::SampledCompute,
                       m_AuthoredAtlas ? "Clouds.AuthoredAtlas" : "Clouds.AuthoredAtlasFallback" );
        return block;
    }

    SystemRasterPass VolumetricCloudRenderer::CompositePass()
    {
        SystemRasterPass config;
        const auto       target = m_TargetFramebuffer.lock();
        if ( !target || !m_CompositePipeline )
            return config;

        config.Name        = "CloudComposite";
        config.ExecuteFunc = [this]( RDG::PassContext&     context,
                                     const FrameGraphRefs& refs ) -> Common::BoolResultStr
        {
            if ( !refs.Transients.CloudScatter.IsValid() || !refs.Transients.CloudGuide.IsValid() ||
                 !m_CompositeMaterial )
                return BOOLSUCCESS;
            // Through the block Declare declared below.
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_CompositePipeline,
                                                           m_CompositeMaterial->GetMaterialExecutor() );
        };
        config.TargetFramebuffer = target;

        // The composite samples the reconstruction the resolve node wrote this frame (m_ResolvedIndex is decided
        // when the cloud nodes are declared, before this runs).
        config.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            // The RECONSTRUCTION, not the trace: the composite upsamples half to full, and the half-res pair is
            // what the resolve wrote this frame (imported by SceneRenderer).
            const RDG::TextureRef scatter = refs.Transients.CloudScatter;
            const RDG::TextureRef guide   = refs.Transients.CloudGuide;
            if ( !scatter.IsValid() || !guide.IsValid() || !m_CompositeMaterial )
                return;
            // The composite's one block: the shader's layout, the material route, and both halves from ONE slot
            // (the shader indexes them with one set of coordinates) with the sampler the material route sampled
            // them with (the image's own: linear, REPEAT). Each entry is the read's declaration.
            const MaterialExecutor& executor = *m_CompositeMaterial->GetMaterialExecutor();
            declared
                 .Bindings( m_CompositeLayout.Get( m_CompositePipeline->GetSpecification().Shader ),
                            executor.GetRouteFill() )
                 .Texture( "u_CloudScatter", scatter, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
                 .Texture( "u_CloudGuide", guide, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() );
        };

        return config;
    }
} // namespace Desert::Graphic::System
