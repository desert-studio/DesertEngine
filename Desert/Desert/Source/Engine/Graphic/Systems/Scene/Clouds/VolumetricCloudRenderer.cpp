#include "VolumetricCloudRenderer.hpp"

#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialBake.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/RenderGraphSort.hpp>
#include <Engine/Graphic/RenderPhase.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

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
        m_ModellingBakeSignal->Cancelled.store( true, std::memory_order_relaxed );
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
        layerInputs.WindDirection    = m_Data.WindDirection;

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

        // WHERE THE REGION WANTS TO BE. The camera MINUS the accumulated wind, because the march asks the
        // volume about `position - wind`: the sky drifting downwind for an hour must not carry the region
        // away from the camera that is looking at it.
        const glm::vec3 cameraKm = camera->GetPosition() / kCloudWorldUnitsPerKm;
        const glm::vec3 windKm   = m_WindOffset / kCloudWorldUnitsPerKm;

        const glm::vec2 wantedOrigin =
             Assets::CloudProceduralRegionOriginKm( wanted, cameraKm.x - windKm.x, cameraKm.z - windKm.z );

        // ---------------------------------------------------------------------------------------------
        // START ONE IF WHAT IS ON THE DEVICE IS NOT WHAT IS WANTED
        // ---------------------------------------------------------------------------------------------
        // A REFUSAL APPLIES TO THE PARAMETERS THAT EARNED IT, AND IS LIFTED THE MOMENT THEY MOVE. See the
        // member's declaration for what the permanent version of this flag cost: an artist cannot reach a
        // positive thickness without passing through zero, and zero is rejected, so the latch fired during
        // ordinary editing and disabled every cloud knob for the rest of the session.
        if ( m_ModellingFailed && ( m_FailedOriginKm != wantedOrigin ||
                                    !Assets::CloudProceduralParamsEqual( m_FailedParams, wanted ) ) )
            m_ModellingFailed = false;

        if ( !m_ModellingFailed )
        {
            const bool sameSet =
                 m_ModellingValid && m_ProfileSpeciesCount == speciesCount && m_ProfileGeneration == generation;

            bool sameTypes = sameSet;
            for ( uint32_t slot = 0; sameTypes && slot < speciesCount; ++slot )
                sameTypes = m_ProfileTypes[slot] == handles[slot];

            const bool sameRegion = m_ModellingValid && m_ModellingOriginKm == wantedOrigin;
            const bool sameParams =
                 m_ModellingValid && Assets::CloudProceduralParamsEqual( m_ModellingParams, wanted );

            // ── WHAT A BAKE IN FLIGHT IS FOR, AND THE ONE DISTINCTION THAT MATTERS ────────────────────
            //
            // A bake becomes stale for two completely different reasons, and only ONE of them makes it
            // worthless:
            //
            //   * THE SHAPE CHANGED — the artist moved a parameter or dropped a different cloud type in.
            //     The volume in flight is a picture of a sky that no longer exists. Nothing downstream will
            //     ever want it, so finishing it is pure waste and it is CANCELLED.
            //   * ONLY THE REGION MOVED — the camera crossed a snap of the lump lattice. The volume in
            //     flight is a picture of the RIGHT sky, one snap step away, and the field is periodic, so
            //     it is a perfectly usable answer that the next frame can march. It is LET FINISH.
            //
            // THE SECOND CASE IS NOT A CONCESSION, IT IS FORWARD PROGRESS. Cancelling on a moving camera
            // would replace "always one snap behind" with "never finishes at all" whenever the camera keeps
            // crossing snaps faster than a bake completes — a starvation the old always-finish code could
            // not have. The gain the owner asked for lives entirely in the first case: his twenty edits in
            // 1.02 s are twenty shape changes and no region movement whatsoever.
            bool pendingShapeIsWanted = m_ModellingBake.valid() && m_PendingSpeciesCount == speciesCount &&
                                        m_PendingGeneration == generation &&
                                        Assets::CloudProceduralParamsEqual( m_PendingParams, wanted );
            for ( uint32_t slot = 0; pendingShapeIsWanted && slot < speciesCount; ++slot )
                pendingShapeIsWanted = m_PendingTypes[slot] == handles[slot];

            // A bake of the wanted SHAPE is left alone even when its region has since moved, which is also
            // what stops an artist holding a slider still from restarting the same bake sixty times a
            // second: the frame after it lands starts the cheap origin-only rebake if one is still wanted.
            if ( !pendingShapeIsWanted && !( sameTypes && sameRegion && sameParams ) )
            {
                if ( auto valid = Assets::ValidateCloudProceduralParams( wanted ); !valid )
                {
                    m_ModellingFailed = true;
                    m_FailedParams    = wanted;
                    m_FailedOriginKm  = wantedOrigin;
                    LOG_ERROR( "[Clouds] The cloud layer cannot be turned into a modelling volume: {}",
                               valid.GetError() );
                    return m_ModellingValid;
                }

                // GETTING HERE WITH A BAKE IN FLIGHT MEANS ITS SHAPE IS NOT THE WANTED ONE — see the two
                // cases above — so it is told to stop and its result is dropped rather than collected. A
                // third of the delay the owner reported was exactly this: 20 edits in 1.02 s produced two
                // bakes of 4 800 ms and 11 453 ms IN SERIES, because `std::async` has no way to be told the
                // answer is no longer needed, and the second could not start until the first was done.
                //
                // Dropping the future is safe and does not block — a future from JobSystem::Async wraps a
                // packaged_task, whose destructor never waits, where the destructor of a std::async future
                // BLOCKS on the very thread we are trying to stop. That is why the old code could not have
                // done this even if it had had a flag.
                if ( m_ModellingBake.valid() )
                {
                    m_ModellingBakeSignal->Cancelled.store( true, std::memory_order_relaxed );
                    m_ModellingBake = {};
                    ++m_ModellingBakesCancelled;
                }

                m_PendingParams       = wanted;
                m_PendingOriginKm     = wantedOrigin;
                m_PendingSpeciesCount = speciesCount;
                m_PendingGeneration   = generation;
                for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
                    m_PendingTypes[slot] = slot < speciesCount ? handles[slot] : Assets::AssetHandle::Null();

                // ON A WORKER, and the frame does not wait for it. See the note on the declaration: the
                // bake is measured at hundreds of milliseconds to seconds in a Debug build, and a region
                // shift happens once per lattice cell of camera travel.
                m_ModellingBakeStarted = std::chrono::steady_clock::now();

                // A FRESH SIGNAL PER BAKE, not a reset of the old one: the abandoned job still holds the old
                // shared_ptr and is still reading it, so clearing that flag would un-cancel a bake we have
                // already stopped caring about and burn a worker for nothing.
                m_ModellingBakeSignal = std::make_shared<ModellingBakeSignal>();

                // ON THE ENGINE'S POOL AND NOT ON A THREAD OF OUR OWN. Three things follow and all three
                // were live defects: the work is VISIBLE (DESERT_PROFILE_SCOPE below puts it in Optick and
                // in the editor's own Profiler panel, where a whole day was spent inferring this cost from
                // log timestamps because no such row existed); it is BOUNDED (two document windows used to
                // be able to start unrelated bakes with nothing counting them against the machine); and it
                // is CANCELLABLE, which is what the block above needs.
                m_ModellingBake = Common::JobSystem::Get().Async(
                     [params = wanted, origin = wantedOrigin, signal = m_ModellingBakeSignal]()
                     {
                         DESERT_PROFILE_SCOPE( "Clouds: Modelling volume bake" );
                         return Assets::BakeCloudProceduralVolume(
                              params, origin,
                              [&signal]( float fraction )
                              {
                                  signal->Fraction.store( fraction, std::memory_order_relaxed );
                                  return !signal->Cancelled.load( std::memory_order_relaxed );
                              } );
                     } );
            }
        }

        // ---------------------------------------------------------------------------------------------
        // COLLECT A FINISHED BAKE
        // ---------------------------------------------------------------------------------------------
        // THE FIRST BAKE OF A SCENE BLOCKS, AND EVERY LATER ONE DOES NOT. The difference is whether there
        // is anything to march meanwhile.
        //
        // A REBAKE has a previous volume: the camera has left the region it was baked for by at most one
        // snap step, the volume is periodic, so the frame reads the neighbouring tile — the same
        // degenerate far path the sky past the region already uses — and waiting for a better answer would
        // be a hitch in exchange for nothing anybody can see.
        //
        // THE FIRST BAKE has none, and the consequence of not waiting was measured rather than reasoned
        // about: with the pass skipped the frames cost almost nothing, so a headless shot of ninety frames
        // finished BEFORE an 800 ms bake did and wrote an empty sky. In an editor it is the same defect
        // with a friendlier face — the sky appears a second after the scene does, and the temporal resolve
        // then takes ten more frames to converge it.
        if ( m_ModellingBake.valid() && !m_ModellingValid )
            m_ModellingBake.wait();

        if ( m_ModellingBake.valid() &&
             m_ModellingBake.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
        {
            const auto baked = m_ModellingBake.get();

            if ( !baked )
            {
                // The parameters blamed here are the ones the FINISHED bake was started for, which is what
                // m_Pending* hold — `wanted` may already have moved on while the worker ran.
                m_ModellingFailed = true;
                m_FailedParams    = m_PendingParams;
                m_FailedOriginKm  = m_PendingOriginKm;
                LOG_ERROR( "[Clouds] The procedural modelling volume could not be baked: {}", baked.GetError() );
                return m_ModellingValid;
            }

            // A FRESH IMAGE RATHER THAN AN IN-PLACE UPLOAD, which is the choice CloudNoiseService makes
            // and for the same reason: SetData writes into an image that frames still in flight may be
            // sampling. A rebake happens once per lattice cell of camera travel, so the allocation is not
            // worth the synchronisation argument. The old image goes through Image3D's deletion queue
            // when the last reference drops.
            if ( m_ModellingVolume )
                Renderer::GetInstance().WaitDeviceIdle();

            // THE SIDE COMES FROM THE PARAMETERS THE FINISHED BAKE RAN WITH, never from the constant and
            // never from `wanted`. The bake sizes its byte block from its own parameters, so an image built
            // to any other extent would be read past the end of the block or short of it — and `wanted` may
            // have moved to a different budget while this worker ran.
            const uint32_t bakedSide = m_PendingParams.VolumeSideVoxels;

            const Core::Formats::Image3DSpecification spec{
                 .Tag        = "CloudModellingVolume",
                 .Width      = bakedSide,
                 .Height     = Assets::kCloudProceduralVolumeHeight,
                 .Depth      = bakedSide,
                 .Format     = Core::Formats::ImageFormat::RGBA8F,
                 .Data       = baked.GetValue(),
                 .Properties = Core::Formats::Sample,
            };

            m_ModellingVolume = Image3D::Create( spec );
            if ( !m_ModellingVolume )
            {
                // A device allocation failure is about the SIZE, so blaming the pending parameters keeps the
                // retry honest: moving a knob — including the resolution knob — is a fresh attempt, not a
                // loop.
                m_ModellingFailed = true;
                m_FailedParams    = m_PendingParams;
                m_FailedOriginKm  = m_PendingOriginKm;
                LOG_ERROR( "[Clouds] The {}x{}x{} RGBA8 procedural modelling volume could not be created on "
                           "the device; the clouds will not render for this view.",
                           bakedSide, Assets::kCloudProceduralVolumeHeight, bakedSide );
                m_ModellingValid = false;
                return false;
            }

            // WHAT THE SKY'S ENVIRONMENT BAKE ASKS ABOUT, and it is deliberately NOT "the volume was
            // rebuilt": a rebuild happens every time the camera crosses a snap of the lump lattice, and
            // the irradiance cube cannot see the field move. Asked through the same canonical comparison
            // the cache above uses, so the two cannot come to different conclusions about what changed.
            if ( !m_ModellingValid || !Assets::CloudProceduralParamsEqual( m_ModellingParams, m_PendingParams ) )
                ++m_ModellingShapeGeneration;

            m_ModellingParams   = m_PendingParams;
            m_ModellingOriginKm = m_PendingOriginKm;
            m_ModellingValid    = true;

            // FROM THE PENDING SET AND NOT FROM THIS FRAME'S, which is the same statement m_ModellingParams
            // above it makes: what is recorded as "what the volume on the device was built from" has to be
            // what the finished BAKE was started with. It reads identically today — a bake whose types
            // changed under it is cancelled and its future dropped, so a collected one is always the wanted
            // one — but writing `handles` here would leave the cache describing a set the bytes were never
            // built from the day that stops being true, and the symptom would be a cloud type swap that
            // never re-bakes at all. Both ends looked right; the middle link is where it is lost.
            for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
                m_ProfileTypes[slot] = m_PendingTypes[slot];
            m_ProfileSpeciesCount = m_PendingSpeciesCount;
            m_ProfileGeneration   = m_PendingGeneration;

            // THE COST OF A REBAKE IS PRINTED, NOT ASSUMED, and that is the exit criterion this phase was
            // given (ANALYSIS_APPROACH.md §3). It is wall time from starting the worker to collecting it,
            // so it includes whatever else the machine was doing — which is the number that matters,
            // because what it bounds is how far the sky lags the camera.
            const double bakeMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() -
                                                                             m_ModellingBakeStarted )
                                       .count();

            // THE LIFT IS NAMED BESIDE THE ENVELOPE rather than left to be inferred from it. The envelope
            // moves for two unrelated reasons — a different set of types, or this scene's own offset —
            // and a reader who cannot tell them apart is one edit away from blaming the wrong one.
            LOG_INFO( "[Clouds] Modelling volume baked for {} cloud type(s) in {:.0f} ms — region {:.0f} km "
                      "at ({:.1f}, {:.1f}), envelope {:.2f} to {:.2f} km (scene lift {:.2f} km), "
                      "{}x{}x{} RGBA8 ({:.2f} MiB), "
                      "{} lumps, {:.0f} m per voxel, {} stale bake(s) cancelled.",
                      m_ProfileSpeciesCount, bakeMs, m_ModellingParams.RegionSizeKm, m_ModellingOriginKm.x,
                      m_ModellingOriginKm.y, m_ModellingParams.LayerBottomKm,
                      m_ModellingParams.LayerBottomKm + m_ModellingParams.LayerThicknessKm,
                      CloudLayerLiftKm( m_Data ), bakedSide, Assets::kCloudProceduralVolumeHeight, bakedSide,
                      BytesToMiB( static_cast<size_t>( Assets::CloudProceduralVoxelBytes( bakedSide ) ) ),
                      Assets::CountCloudProceduralBlobs( m_ModellingParams, m_ModellingOriginKm ),
                      m_ModellingParams.RegionSizeKm / static_cast<float>( bakedSide ) * 1000.0f,
                      m_ModellingBakesCancelled );

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
        spec.Framebuffer = target;

        // A fullscreen quad has no meaningful depth of its own; occlusion was resolved inside the march,
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

        // Replayed by ExecuteTransparency with a LOAD begin, so the pipeline is built against the
        // framebuffer's LOAD render pass.
        spec.UseLoadRenderPass = true;

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

    bool VolumetricCloudRenderer::EnsureShadowMap( uint32_t resolution )
    {
        if ( m_ShadowMapFailed )
            return false;

        const float scale = CloudQualityFor( m_Quality ).ShadowMapScale;
        if ( m_ShadowMapImage && m_ShadowMapScaleInUse == scale )
            return true;

        // ALLOCATED ON THE FIRST FRAME THE LAYER ACTUALLY CASTS, and reallocated only when the QUALITY
        // TIER changes its size — never when the viewport does. That distinction is the whole reason this
        // is not part of EnsureTraceTargets: the six trace targets ARE the view's size and a resize must
        // throw them away, while this map is a world-space quantity and a resize must leave it standing.
        //
        // The old image may still be referenced by descriptors of frames in flight, exactly as in
        // EnsureTraceTargets, so the device is idled before it is dropped. A tier switch is a rare,
        // human-initiated event; paying a full idle for it is the cheap answer and the safe one.
        if ( m_ShadowMapImage )
            Renderer::GetInstance().WaitDeviceIdle();

        const Core::Formats::Image2DSpecification spec{
             .Tag        = "CloudShadowMap",
             .Width      = resolution,
             .Height     = resolution,
             .Format     = Core::Formats::ImageFormat::RGBA32F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };

        m_ShadowMapImage = Image2D::Create( spec );
        if ( !m_ShadowMapImage )
        {
            LOG_ERROR( "[Clouds] The {}x{} RGBA32F cloud shadow map could not be created on the device; "
                       "the clouds will not shade the world for this view.",
                       resolution, resolution );
            m_ShadowMapFailed = true;
            return false;
        }

        m_ShadowMapScaleInUse = scale;

        const float extentKm = CloudShadowExtentKmForScale( scale );
        LOG_INFO( "[Clouds] Cloud shadow map {}x{} RGBA32F ({:.2f} MiB) — {:.0f} km across the world, "
                  "{:.1f} m per texel, snapped to a {:.0f} km grid, quality scale {:.2f}.",
                  resolution, resolution,
                  BytesToMiB( Core::Formats::CalculateImageSize( resolution, resolution,
                                                                 Core::Formats::ImageFormat::RGBA32F ) ),
                  2.0f * extentKm, 2.0f * extentKm * 1000.0f / static_cast<float>( resolution ),
                  CloudShadowSnapKmForScale( scale ), scale );
        return true;
    }

    float VolumetricCloudRenderer::GetShadowStrength() const
    {
        // ZERO WHEN THE LAYER IS NOT CASTING, so a consumer that reads only this number cannot light the
        // world through a map that was never written. It is the same gate ExecuteShadowMapInFrame uses,
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

        payload = PackCloudParams( m_Data, m_Material, shapes, speciesCount, atmosphere, m_WindOffset,
                                   CloudRegionBinding{ m_ModellingOriginKm, m_ModellingParams.RegionSizeKm },
                                   quality.LightMarchSampleCeiling, quality.StopTransmittanceFloor, m_NoiseSlots );
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
            return bake;

        // Slot A, on the same terms the two in-frame passes build it on. A hero cloud is part of the
        // field, so it lights the world through the environment for the same reason it shades the ground.
        BuildAuthoredPayload( bake.Params );
        bake.Authored = m_AuthoredPayload;

        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
            bake.Noise[slot] = m_NoiseVolume[slot];

        bake.Modelling     = m_ModellingVolume.get();
        bake.AuthoredAtlas = m_AuthoredAtlas.get();

        // THE PREVIOUS FRAME'S VOLUME, and it can be nothing else: this runs before the frame's own
        // dispatch of it. The first bake of a scene therefore marches with the profile-driven occlusion
        // the layer would fall back to anyway, and the fingerprint below carries the flag so that the
        // frame the volume appears asks for one more bake rather than leaving a permanently brighter dome.
        bake.SkyOcclusionValid  = m_SkyOcclusionValid && m_SkyOcclusionVolume != nullptr;
        bake.SkyOcclusionVolume = m_SkyOcclusionVolume.get();

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
        // schema the graph author wrote. Copied for the same reason the variant is — the bake outlives this
        // call by a submit-and-wait — while the IMAGES are borrowed, exactly like the noise volumes beside
        // them, because the texture service owns them and outlives the bake.
        bake.MediumValues = m_MediumValues.Params;
        bake.MediumImages = m_MediumImages;

        bake.Marched = true;
        bake.Fingerprint =
             CloudEnvironmentFingerprint( bake.Params, true, bake.SkyOcclusionValid, m_ModellingShapeGeneration,
                                          m_MediumVariantHash, m_MediumValuesFingerprint );
        return bake;
    }

    void VolumetricCloudRenderer::ExecuteShadowMapInFrame()
    {
        DESERT_PROFILE_PASS( "Clouds: ShadowMap" );

        // Cleared FIRST and set only at the end, so every early return below leaves consumers with "no
        // cloud shadow this frame" rather than with the projection of a frame the sun has since left.
        m_ShadowMapValid = false;

        if ( !m_ShadowMapPipeline || !m_ShadowParamsBuffer )
            return;

        // THE ZERO-COST LADDER, and it is the same one the march has, plus the two fields this task
        // added. A scene with no cloud component, with the clouds off, with casting off or with the
        // strength at zero dispatches nothing and — because the allocation is below this line and not
        // above it — allocates nothing either. The editor builds a SceneRenderer for every asset
        // thumbnail and every mesh preview, and none of them has a sky.
        if ( GetShadowStrength() <= 0.0f )
            return;

        const AtmosphereEnv& atmosphere = m_SceneRenderer->GetAtmosphere();
        if ( !atmosphere.Valid )
            return;

        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return;

        // THE TIER'S THREE NUMBERS, DERIVED FROM ONE SCALE. Resolution, extent and snap move together so
        // that the texel — and therefore the relation against the chord the view march can find, which
        // Desert/Tests/Engine/CloudShadow asserts for every tier — does not move at all. What the cheaper
        // tier buys is a smaller square of world, not a coarser one.
        const CloudQualityScale quality    = CloudQualityFor( m_Quality );
        const uint32_t          resolution = CloudShadowResolutionForScale( quality.ShadowMapScale );
        const float             extentKm   = CloudShadowExtentKmForScale( quality.ShadowMapScale );
        const float             snapKm     = CloudShadowSnapKmForScale( quality.ShadowMapScale );

        if ( !EnsureShadowMap( resolution ) )
            return;

        CloudGpuPayload payload{};
        if ( !BuildFieldPayload( payload ) )
            return;

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
            return;
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

        auto& renderer = Renderer::GetInstance();

        // ONLY THE DISTINCT ONES ARE TRANSITIONED, and only they can be: the barrier is per image, and
        // asking for the same image four times is four barriers on one resource rather than a no-op. The
        // descriptors below are still written four times, because a descriptor is not a barrier.
        for ( uint32_t slot = 0; slot < m_NoiseNeeded; ++slot )
            renderer.ComputeImageBeginRead( m_NoiseVolume[slot] );
        renderer.ComputeImageBeginRead( m_ModellingVolume.get() );
        if ( m_AuthoredAtlas )
            renderer.ComputeImageBeginRead( m_AuthoredAtlas.get() );
        renderer.ComputeImageBeginWrite( m_ShadowMapImage.get() );

        m_ShadowMapPipeline->SetOutput( kCloudShadowOutputBinding, m_ShadowMapImage.get(), 0 );
        m_ShadowMapPipeline->SetStorageBuffer( kCloudShadowParamsBinding, m_ShadowParamsBuffer.get() );
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
            m_ShadowMapPipeline->SetInput( kCloudShadowNoiseBindings[slot], m_NoiseVolume[slot] );
        m_ShadowMapPipeline->SetInput( kCloudShadowModellingBinding, m_ModellingVolume.get() );
        m_ShadowMapPipeline->SetStorageBuffer( kCloudShadowAuthoredBinding, m_ShadowAuthoredBuffer.get() );
        // ALWAYS bound, fallback included — see the note at the march's own binding of it.
        m_ShadowMapPipeline->SetInput(
             kCloudShadowAuthoredAtlasBinding,
             m_AuthoredAtlas
                  ? m_AuthoredAtlas.get()
                  : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get() );
        BindMedium( m_ShadowMapPipeline.get(), m_ShadowMediumParamsBuffer.get() );
        m_ShadowMapPipeline->SetPushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );

        renderer.DispatchComputeInFrame( m_ShadowMapPipeline.get(), GroupCount( resolution, kMarchWorkGroupSize ),
                                         GroupCount( resolution, kMarchWorkGroupSize ), 1 );

        renderer.ComputeImageEndWrite( m_ShadowMapImage.get() );
        if ( m_AuthoredAtlas )
            renderer.ComputeImageEndRead( m_AuthoredAtlas.get() );
        renderer.ComputeImageEndRead( m_ModellingVolume.get() );
        for ( uint32_t slot = m_NoiseNeeded; slot-- > 0; )
            renderer.ComputeImageEndRead( m_NoiseVolume[slot] );

        m_ShadowMapValid = true;
    }

    void VolumetricCloudRenderer::SetCloudSettings( bool present, const ECS::VolumetricCloudData& data,
                                                    const glm::vec3&                      windOffset,
                                                    Common::Settings::CloudQuality        quality,
                                                    const std::vector<HeroCloudInstance>& heroClouds )
    {
        m_Present    = present;
        m_Data       = data;
        m_WindOffset = windOffset;
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
                // has warned about this since it was written (MaterialFactory::ApplyShaderAsset); the
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

        auto* textures = Runtime::ResourceRegistry::GetTextureService();
        auto* images   = Runtime::ResourceRegistry::GetImageService();
        for ( std::size_t slot = 0; slot < m_MediumValues.Textures.size(); ++slot )
        {
            const Assets::AssetHandle handle = m_MediumValues.Textures[slot];
            if ( !handle.IsNull() && textures && images )
            {
                auto* texture = textures->Get( handle );
                if ( auto* image = texture ? static_cast<Image2D*>( images->Resolve( texture->GetImageHandle() ) )
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
            m_MediumImages[slot] = const_cast<Image2D*>( DefaultTextures::Get().Resolve( kind ) );
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

        auto* fallback = FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA32F ).get();
        for ( std::size_t slot = 0; slot < m_MediumImages.size(); ++slot )
        {
            // ResolveMediumValues leaves no null here — an unassigned slot already carries the schema's
            // own default image. The guard is for the one case it cannot cover, a default-texture service
            // that has not come up, and it binds SOMETHING because an unwritten descriptor costs the whole
            // dispatch rather than one sampler.
            pipeline->SetInput( Core::kCloudMediumTextureFirst + static_cast<uint32_t>( slot ),
                                m_MediumImages[slot] ? m_MediumImages[slot] : fallback );
        }
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

    bool VolumetricCloudRenderer::EnsureTraceTargets( uint32_t halfWidth, uint32_t halfHeight )
    {
        if ( m_TargetsFailed )
            return false;

        const bool allocated = m_TraceImage && m_TraceGuideImage && m_HistoryImage[0] && m_HistoryImage[1] &&
                               m_HistoryGuideImage[0] && m_HistoryGuideImage[1];
        if ( allocated && halfWidth == m_HalfWidth && halfHeight == m_HalfHeight )
            return true;

        // The old images may still be referenced by descriptors of frames in flight.
        if ( m_TraceImage || m_HistoryImage[0] )
            Renderer::GetInstance().WaitDeviceIdle();

        const uint32_t traceWidth  = HalfExtent( halfWidth );
        const uint32_t traceHeight = HalfExtent( halfHeight );

        // One helper for all six, because the only thing that differs between them is a size and a name.
        // Written as a lambda taking its parameters rather than capturing them: a parameter-less
        // multi-line lambda is one of the constructs clang-format 18 and 22 disagree about, and the CI
        // gate runs 18.
        auto createTarget = []( const char* tag, uint32_t width, uint32_t height )
        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = tag,
                 .Width      = width,
                 .Height     = height,
                 .Format     = Core::Formats::ImageFormat::RGBA16F,
                 .Mips       = 1u,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            return Image2D::Create( spec );
        };

        // The march's pair, at a QUARTER of the view. RGBA16F for the scatter because radiance is
        // pre-tonemap HDR; RGBA16F for the guide for a reason worth stating rather than discovering: the
        // guide carries TWO meaningful channels and Core::Formats::ImageFormat offers no two-channel float
        // format, so half of every texel is allocated and never written. Half a kilometre of distance
        // resolves to about thirty metres at this precision, two orders finer than the tenth-of-the-
        // distance threshold the composite compares against and three finer than the two-kilometre
        // disocclusion threshold the reconstruction compares against.
        m_TraceImage      = createTarget( "CloudTrace", traceWidth, traceHeight );
        m_TraceGuideImage = createTarget( "CloudTraceGuide", traceWidth, traceHeight );

        for ( uint32_t i = 0; i < 2u; ++i )
        {
            m_HistoryImage[i]      = createTarget( "CloudReconstruction", halfWidth, halfHeight );
            m_HistoryGuideImage[i] = createTarget( "CloudReconstructionGuide", halfWidth, halfHeight );
        }

        const double traceMiB = BytesToMiB(
             Core::Formats::CalculateImageSize( traceWidth, traceHeight, Core::Formats::ImageFormat::RGBA16F ) );
        const double halfMiB = BytesToMiB(
             Core::Formats::CalculateImageSize( halfWidth, halfHeight, Core::Formats::ImageFormat::RGBA16F ) );

        if ( !m_TraceImage || !m_TraceGuideImage || !m_HistoryImage[0] || !m_HistoryImage[1] ||
             !m_HistoryGuideImage[0] || !m_HistoryGuideImage[1] )
        {
            LOG_ERROR( "[Clouds] The reconstruction targets could not be created — trace {}x{} ({:.2f} MiB "
                       "each, scatter {}, guide {}), history {}x{} ({:.2f} MiB each, scatter {}/{}, guide "
                       "{}/{}); the clouds will not render for this view.",
                       traceWidth, traceHeight, traceMiB, m_TraceImage != nullptr, m_TraceGuideImage != nullptr,
                       halfWidth, halfHeight, halfMiB, m_HistoryImage[0] != nullptr, m_HistoryImage[1] != nullptr,
                       m_HistoryGuideImage[0] != nullptr, m_HistoryGuideImage[1] != nullptr );

            // Released together rather than left half standing: the pass writes all six or none, and a
            // trace image with no history beside it is a target nothing may reconstruct.
            m_TraceImage.reset();
            m_TraceGuideImage.reset();
            for ( uint32_t i = 0; i < 2u; ++i )
            {
                m_HistoryImage[i].reset();
                m_HistoryGuideImage[i].reset();
            }
            m_TargetsFailed = true;
            return false;
        }

        m_HalfWidth  = halfWidth;
        m_HalfHeight = halfHeight;

        // Every image is fresh, so whatever the resolve read last frame is gone. Saying so here rather
        // than at the call site is what keeps the flag true to the memory it describes: a resize is the
        // one event that invalidates the history without the camera moving at all.
        m_HistoryValid = false;

        // The cost is announced once, on the allocation, not discovered in a memory graph later. All six
        // are named and counted, because targets of the same size are exactly what a reader of a memory
        // graph would otherwise take for one measured several times.
        LOG_INFO( "[Clouds] Trace targets {}x{} RGBA16F ({:.2f} MiB each, scatter + guide) — a QUARTER of "
                  "the {}x{} view, one jittered sub-pixel per frame.",
                  traceWidth, traceHeight, traceMiB, halfWidth * 2u, halfHeight * 2u );
        LOG_INFO( "[Clouds] Reconstruction targets {}x{} RGBA16F ({:.2f} MiB each, scatter + guide, "
                  "ping-ponged) — {:.2f} MiB total for the pass.",
                  halfWidth, halfHeight, halfMiB, 2.0 * traceMiB + 4.0 * halfMiB );
        return true;
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
        // and they cost nothing in memory, because Assets::AssetPreloader uploads every `.dcnv` in the
        // project at startup whatever any scene names.
        //
        // THE HANDLES ARE THE SPECIES' AND NOT THE SLOTS', which matters when a layer's filled slots have
        // holes in them: ResolveSpecies has already compacted slot 3 down to species 1, and the march
        // indexes SpeciesNoise by the same compacted number. Two statements of that compaction is exactly
        // the defect this task found in the code it replaced.
        Assets::AssetHandle perSpecies[kCloudSpeciesSlots] = {};

        const uint32_t species = std::min( speciesCount, kCloudSpeciesSlots );
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

            m_NoiseVolume[slot] = volume.Get();
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

    void VolumetricCloudRenderer::ExecuteInFrame()
    {
        DESERT_PROFILE_PASS( "Clouds: ExecuteInFrame" );

        m_HasFrameResult = false;

        // Cleared with it, and read by the NEXT frame's environment bake rather than by anything here: a
        // frame that skipped this pass must leave "there is no sky-occlusion volume to read" behind it,
        // not the answer of the last frame that ran.
        m_SkyOcclusionValid = false;

        if ( !m_MarchPipeline || !m_ResolvePipeline || !m_CompositePipeline || !m_ParamsBuffer ||
             !m_ResolveParamsBuffer )
            return;

        if ( !m_Present || !m_Data.Enabled )
            return;

        // Without a sky there is no sun to light the clouds and no ambient to fill their shadowed sides.
        // Marching anyway would draw black cut-outs across the frame, which is worse than drawing nothing
        // and much harder to diagnose — so the layer is simply absent, exactly as the sky's own contract
        // says a consumer must treat Valid == false.
        const AtmosphereEnv& atmosphere = m_SceneRenderer->GetAtmosphere();
        if ( !atmosphere.Valid )
            return;

        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return;

        const auto target = m_TargetFramebuffer.lock();
        if ( !target || target->GetDepthAttachmentCount() == 0 )
            return;

        if ( !EnsureTraceTargets( HalfExtent( target->GetFramebufferWidth() ),
                                  HalfExtent( target->GetFramebufferHeight() ) ) )
            return;

        // RESOLVED AGAIN RATHER THAN CACHED FROM EnsureModellingVolume. Two hash-map probes per slot
        // against handles that have not moved is not worth a member that can disagree with the volume it
        // was built beside — and the one thing that must not diverge is exactly the set whose channels the
        // volume carries. BuildFieldPayload is the one place that sequence is written.
        CloudGpuPayload payload{};
        if ( !BuildFieldPayload( payload ) )
            return;

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
            return;
        }

        const glm::mat4     viewProjection = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        const CloudSubPixel subPixel       = CloudTraceSubPixel( m_FrameIndex );

        auto& renderer = Renderer::GetInstance();

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
            DESERT_PROFILE_PASS( "Clouds: SkyOcclusion" );

            for ( uint32_t slot = 0; slot < m_NoiseNeeded; ++slot )
                renderer.ComputeImageBeginRead( m_NoiseVolume[slot] );
            renderer.ComputeImageBeginRead( m_ModellingVolume.get() );
            if ( m_AuthoredAtlas )
                renderer.ComputeImageBeginRead( m_AuthoredAtlas.get() );
            renderer.ComputeImageBeginWrite( m_SkyOcclusionVolume.get() );

            m_SkyOcclusionPipeline->SetOutput( kCloudSkyOcclusionOutputBinding, m_SkyOcclusionVolume.get(), 0 );
            m_SkyOcclusionPipeline->SetStorageBuffer( kCloudSkyOcclusionParamsBinding, m_ParamsBuffer.get() );
            for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
                m_SkyOcclusionPipeline->SetInput( kCloudSkyOcclusionNoiseBindings[slot], m_NoiseVolume[slot] );
            m_SkyOcclusionPipeline->SetInput( kCloudSkyOcclusionModellingBinding, m_ModellingVolume.get() );
            m_SkyOcclusionPipeline->SetStorageBuffer( kCloudSkyOcclusionAuthoredBinding, m_AuthoredBuffer.get() );
            // The same buffer the march binds, and legitimately so: this dispatch is issued inside
            // ExecuteInFrame between the march's own upload and the march itself, which is exactly the
            // window m_ParamsBuffer is already shared across.
            BindMedium( m_SkyOcclusionPipeline.get(), m_MediumParamsBuffer.get() );
            // ALWAYS bound, fallback included — see the note at the march's own binding of it.
            m_SkyOcclusionPipeline->SetInput(
                 kCloudSkyOcclusionAuthoredAtlasBinding,
                 m_AuthoredAtlas
                      ? m_AuthoredAtlas.get()
                      : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get() );

            // ONE INVOCATION PER COLUMN — the altitude axis is walked inside the shader, because the whole
            // point of the pass is that a column's optical depth accumulates downward and a thread per
            // texel would have to re-integrate everything above it.
            renderer.DispatchComputeInFrame( m_SkyOcclusionPipeline.get(),
                                             GroupCount( kCloudSkyOcclusionResolution, kMarchWorkGroupSize ),
                                             GroupCount( kCloudSkyOcclusionResolution, kMarchWorkGroupSize ), 1 );

            renderer.ComputeImageEndWrite( m_SkyOcclusionVolume.get() );
            if ( m_AuthoredAtlas )
                renderer.ComputeImageEndRead( m_AuthoredAtlas.get() );
            renderer.ComputeImageEndRead( m_ModellingVolume.get() );
            for ( uint32_t slot = m_NoiseNeeded; slot-- > 0; )
                renderer.ComputeImageEndRead( m_NoiseVolume[slot] );

            skyOcclusionReady = true;
        }

        // Published for the NEXT frame's environment bake, which runs before this pass and can therefore
        // only ask about the volume this frame leaves behind.
        m_SkyOcclusionValid = skyOcclusionReady;

        CloudPush push{};
        push.InverseViewProjection = glm::inverse( viewProjection );
        push.CameraPosition = glm::vec4( camera->GetPosition(), static_cast<float>( m_FrameIndex & 0xFFFFu ) );
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

        Image2D* depthImage = target->GetDepthAttachmentImage().get();

        // Present the DEPTH attachment to a compute sampler and hand it back afterwards — its tracked
        // layout is not legal for a combined image sampler, and SetInput binds the tracked layout
        // verbatim.
        renderer.ComputeImageBeginRead( depthImage );
        // Only the DISTINCT volumes, for the reason the shadow pass gives at its own copy of this loop.
        for ( uint32_t slot = 0; slot < m_NoiseNeeded; ++slot )
            renderer.ComputeImageBeginRead( m_NoiseVolume[slot] );
        renderer.ComputeImageBeginRead( m_ModellingVolume.get() );
        if ( m_AuthoredAtlas )
            renderer.ComputeImageBeginRead( m_AuthoredAtlas.get() );
        renderer.ComputeImageBeginWrite( m_TraceImage.get() );
        renderer.ComputeImageBeginWrite( m_TraceGuideImage.get() );

        m_MarchPipeline->SetOutput( kCloudOutputBinding, m_TraceImage.get(), 0 );
        m_MarchPipeline->SetOutput( kCloudGuideOutputBinding, m_TraceGuideImage.get(), 0 );
        m_MarchPipeline->SetStorageBuffer( kCloudParamsBinding, m_ParamsBuffer.get() );
        m_MarchPipeline->SetInput( kCloudSceneDepthBinding, depthImage );
        // ALL FOUR, always, whatever the layer needs — an unbound sampler is an invalid descriptor set and
        // this backend answers one by skipping the dispatch. Slots past the distinct count repeat slot 0's
        // image, which is what ResolveCloudNoiseVolumes filled them with.
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
            m_MarchPipeline->SetInput( kCloudNoiseBindings[slot], m_NoiseVolume[slot] );
        m_MarchPipeline->SetInput( kCloudModellingBinding, m_ModellingVolume.get() );

        // ALWAYS bound, even when the payload's gate says it will not be read: a declared sampler with no
        // image is an invalid descriptor set, not an unused one, and this backend answers an invalid set
        // by skipping the whole dispatch — the clouds would vanish with nothing in the log.
        m_MarchPipeline->SetInput(
             kCloudDistantSkyLightBinding,
             atmosphere.DistantSkyLight
                  ? atmosphere.DistantSkyLight
                  : FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ).get() );

        // The aerial-perspective volume, on the same terms: always bound, read only when the payload's
        // gate says the volume is real. It is what makes a cloud at the horizon the colour of the sky
        // instead of an opaque white wall.
        m_MarchPipeline->SetInput(
             kCloudAerialPerspectiveBinding,
             atmosphere.AerialPerspectiveVolume
                  ? atmosphere.AerialPerspectiveVolume
                  : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get() );

        // Slot A's instance list and the atlas of sculpted bodies it addresses. The image is ALWAYS bound, even
        // when the count is zero and nothing will read it, on exactly the terms the two samplers above
        // are bound on: a declared sampler with no image is an invalid descriptor set, and this backend
        // answers an invalid set by skipping the dispatch — every cloud in the frame would disappear with
        // nothing in the log, which is a rake this subsystem has already stood on.
        m_MarchPipeline->SetStorageBuffer( kCloudAuthoredBinding, m_AuthoredBuffer.get() );
        BindMedium( m_MarchPipeline.get(), m_MediumParamsBuffer.get() );
        m_MarchPipeline->SetInput(
             kCloudAuthoredAtlasBinding,
             m_AuthoredAtlas
                  ? m_AuthoredAtlas.get()
                  : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get() );

        // The sky-light occlusion volume, on the same always-bound terms as the two samplers above and for
        // the same reason. When the layer does not want it the image does not exist at all, so the
        // fallback is what the descriptor points at and push.Frame.x is 0.
        m_MarchPipeline->SetInput(
             kCloudSkyOcclusionBinding,
             skyOcclusionReady
                  ? m_SkyOcclusionVolume.get()
                  : FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F ).get() );

        // The atmosphere's transmittance LUT, again always bound and for the fourth time for the same
        // reason. The handle is the sky's — a scene on the artistic gradient, or one whose LUTs have not
        // been baked, publishes null and gets the fallback, which is exactly the case push.Frame.y is 0 in.
        m_MarchPipeline->SetInput(
             kCloudSunTransmittanceLutBinding,
             atmosphere.TransmittanceLut
                  ? atmosphere.TransmittanceLut
                  : FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ).get() );

        m_MarchPipeline->SetPushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );

        const uint32_t traceWidth  = HalfExtent( m_HalfWidth );
        const uint32_t traceHeight = HalfExtent( m_HalfHeight );

        {
            // The march and the temporal resolve shared ONE scope until GPU timing arrived, which is
            // precisely the split the cost breakdown needed: they are a heavy half-res raymarch and a
            // cheap reprojection, and lumping them hid the ratio.
            DESERT_PROFILE_PASS( "Clouds: March" );
            renderer.DispatchComputeInFrame( m_MarchPipeline.get(), GroupCount( traceWidth, kMarchWorkGroupSize ),
                                             GroupCount( traceHeight, kMarchWorkGroupSize ), 1 );
        }

        renderer.ComputeImageEndWrite( m_TraceGuideImage.get() );
        renderer.ComputeImageEndWrite( m_TraceImage.get() );
        if ( m_AuthoredAtlas )
            renderer.ComputeImageEndRead( m_AuthoredAtlas.get() );
        renderer.ComputeImageEndRead( m_ModellingVolume.get() );
        for ( uint32_t slot = m_NoiseNeeded; slot-- > 0; )
            renderer.ComputeImageEndRead( m_NoiseVolume[slot] );
        renderer.ComputeImageEndRead( depthImage );

        // S2 — THE TEMPORAL RECONSTRUCTION. The slot written alternates with the frame index, so the one
        // written last frame is still intact to be read. Both are real allocations from the first frame
        // onwards; what changes is whether their CONTENT means anything, and that is m_HistoryValid.
        const uint32_t writeIndex = m_FrameIndex & 1u;
        const uint32_t readIndex  = 1u - writeIndex;

        CloudResolveParams resolve{};
        resolve.InverseViewProjection = push.InverseViewProjection;
        resolve.PrevViewProjection    = m_PrevViewProjection;
        resolve.CameraPosition        = camera->GetPosition();
        resolve.HistoryValid          = m_HistoryValid ? 1.0f : 0.0f;
        resolve.SubPixelOffset =
             glm::ivec2( static_cast<int32_t>( subPixel.X ), static_cast<int32_t>( subPixel.Y ) );
        const auto resolveParams =
             m_ResolveParamsBuffer->SetData( &resolve, static_cast<uint32_t>( sizeof( resolve ) ) );
        if ( !resolveParams.IsSuccess() )
        {
            // AND THE HISTORY IS INVALIDATED, which is the part that is not obvious. Skipping the
            // reconstruction leaves m_HistoryImage[writeIndex] holding whatever it held before; next
            // frame would reproject against it as though it were the previous frame's resolve, using a
            // PrevViewProjection that never described it. That is a smear locked to the camera path —
            // the hardest artefact in this subsystem to attribute to its cause. One un-reconstructed
            // frame is visible for one frame; a poisoned history is visible until the camera stops.
            m_HistoryValid = false;
            LOG_ERROR( "[Clouds] the temporal reconstruction is skipped and the history dropped; its "
                       "parameters were not uploaded: {}",
                       resolveParams.GetError() );
            return;
        }

        renderer.ComputeImageBeginWrite( m_HistoryImage[writeIndex].get() );
        renderer.ComputeImageBeginWrite( m_HistoryGuideImage[writeIndex].get() );

        m_ResolvePipeline->SetOutput( kCloudResolveOutputBinding, m_HistoryImage[writeIndex].get(), 0 );
        m_ResolvePipeline->SetOutput( kCloudResolveGuideOutputBinding, m_HistoryGuideImage[writeIndex].get(), 0 );
        m_ResolvePipeline->SetStorageBuffer( kCloudResolveParamsBinding, m_ResolveParamsBuffer.get() );
        m_ResolvePipeline->SetInput( kCloudResolveTraceBinding, m_TraceImage.get() );
        m_ResolvePipeline->SetInput( kCloudResolveTraceGuideBinding, m_TraceGuideImage.get() );

        // THE HISTORY, OR SOMETHING REAL IN ITS PLACE. Before the first reconstruction the read slot has
        // never been written, so its device memory is uninitialised AND its tracked layout is the one it
        // was created in — binding it would be an invalid descriptor, and this backend answers an invalid
        // set by skipping the whole dispatch, which loses the clouds with nothing in the log. The engine's
        // fallback texture is bound instead and CloudResolveParams::HistoryValid tells the shader to
        // ignore it. RGBA8F because FallbackTextures only provides RGBA8F and RGBA32F, and the sampler
        // reads floats either way.
        Image2D* historyScatter = m_HistoryValid ? m_HistoryImage[readIndex].get() : nullptr;
        Image2D* historyGuide   = m_HistoryValid ? m_HistoryGuideImage[readIndex].get() : nullptr;
        Image2D* fallback =
             FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ).get();

        m_ResolvePipeline->SetInput( kCloudResolveHistoryBinding, historyScatter ? historyScatter : fallback );
        m_ResolvePipeline->SetInput( kCloudResolveHistoryGuideBinding, historyGuide ? historyGuide : fallback );

        {
            DESERT_PROFILE_PASS( "Clouds: TemporalResolve" );
            renderer.DispatchComputeInFrame( m_ResolvePipeline.get(),
                                             GroupCount( m_HalfWidth, kMarchWorkGroupSize ),
                                             GroupCount( m_HalfHeight, kMarchWorkGroupSize ), 1 );
        }

        renderer.ComputeImageEndWrite( m_HistoryGuideImage[writeIndex].get() );
        renderer.ComputeImageEndWrite( m_HistoryImage[writeIndex].get() );

        // Recorded AFTER the dispatch that used the previous value, so the matrix always describes the
        // frame whose pixels are now in the history rather than the frame being drawn.
        m_PrevViewProjection = viewProjection;
        m_ResolvedIndex      = writeIndex;
        m_HistoryValid       = true;

        ++m_FrameIndex;
        m_HasFrameResult = true;
    }

    void VolumetricCloudRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        const auto target = m_TargetFramebuffer.lock();
        if ( !target || !m_CompositePipeline )
            return;

        RenderGraphBuilder::PassConfig config;
        config.Name        = "CloudComposite";
        config.Phase       = RenderPhase::Transparency;
        config.ExecuteFunc = [this]()
        {
            // The RECONSTRUCTION, not the trace: the composite upsamples half to full, and the half-res
            // pair is what the resolve wrote this frame.
            if ( !m_HasFrameResult || !m_HistoryImage[m_ResolvedIndex] || !m_HistoryGuideImage[m_ResolvedIndex] ||
                 !m_CompositeMaterial )
                return;

            m_CompositeMaterial->BindInputs( m_HistoryImage[m_ResolvedIndex].get(),
                                             m_HistoryGuideImage[m_ResolvedIndex].get() );
            Renderer::GetInstance().SubmitFullscreenQuad( m_CompositePipeline.get(),
                                                          m_CompositeMaterial->GetMaterialExecutor() );
        };
        config.PipelineSpec      = m_CompositePipeline->GetSpecification();
        config.TargetFramebuffer = target;
        config.Dependencies      = { RenderPassDependency( RenderPhase::Geometry ) };

        // FarField: above the atmospheric fog, below everything else the Transparency phase composites.
        // Stated here, on the pass itself, rather than implied by the order of the RegisterSystem calls —
        // that ordering is a tie-break, not a contract, and it moves when an unrelated system is added.
        config.OrderInPhase = RenderPassOrder::FarField;

        builder.AddPass( config );
    }
} // namespace Desert::Graphic::System
