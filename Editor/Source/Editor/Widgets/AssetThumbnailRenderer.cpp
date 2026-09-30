#include "AssetThumbnailRenderer.hpp"
#include <Editor/Widgets/ThumbnailSlots.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <Engine/Geometry/PosedBounds.hpp>

#include <Editor/Widgets/ThumbnailEncode.hpp>
#include <Editor/Widgets/ThumbnailFraming.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/Runtime/Services/Skybox/SkyboxService.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Units.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>

// STB_IMAGE_WRITE_IMPLEMENTATION is already compiled into Desert.lib (stb_image.obj); just declare here.

namespace Desert::Editor
{
    namespace
    {
        // ── THE DOME, and every number here is O5's, taken rather than re-derived ──────────────────────
        //
        // The Material Editor already shows a Volume-domain material as the sky it authors
        // (PreviewViewport::SetVolumeMaterial), and the framing it uses was argued and measured there.
        // Two independent statements of "where the observer stands to look at a cloud material" is how a
        // thumbnail comes to disagree with the pane it is a thumbnail OF, so these are the same values and
        // PreviewViewport.cpp is where the argument for each of them lives.
        //
        // 96 degrees of vertical field puts the horizon, the mid angle and very nearly the zenith in ONE
        // frame — which matters more here than there: nobody can turn a thumbnail, so an empty zenith or
        // mid-elevation streaking would be outside every picture this ever writes.
        constexpr float kDomeFov       = 96.0f;
        constexpr float kDomeNearPlane = 10.0f;                             // 10 cm
        constexpr float kDomeFarPlane  = Common::Units::Metres( 60000.0f ); // 60 km — the layer's own reach
        // Not eye height: from 1.7 m the visible near ground fits inside a fraction of one cloud-shadow
        // texel and renders uniform. The observer stands on a rise.
        constexpr float kDomeEyeHeight = Common::Units::Metres( 150.0f );
        // 30 degrees of elevation: the frame spans -18 to +78, so a fifth of it is horizon and the rest
        // climbs to just under the zenith.
        constexpr float kDomePitch = 0.5236f;
        constexpr float kDomeYaw   = -0.6f;

        // The march's budget for a 512-pixel tile. PreviewViewport's own defaults, for the same reason it
        // has them: a preview pane has no use for the viewport's sample ceiling.
        constexpr int32_t kDomeMaxSteps          = 96;
        constexpr float   kDomeStopTransmittance = 0.03f;
        // 128 and not 256: measured at 229 ms against 961 ms for a bake whose difference from the shipped
        // grid is softer edges (mean 1.1 to 6.2 of 255). It is also the floor
        // Assets::kCloudProceduralVolumeSideMin states, so this sits ON the cheapest grid measured honest.
        constexpr int32_t kDomeVolumeResolution = 128;
    } // namespace

    AssetThumbnailRenderer::~AssetThumbnailRenderer()
    {
        // The worker holds the readback and writes a file: it finishes before the device it reads from goes.
        if ( m_Encode.valid() )
            m_Encode.wait();
        m_Readback.reset();
        if ( !m_Inited )
            return;

        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_Scene.reset();
        m_Renderer.reset();
    }

    void AssetThumbnailRenderer::EnsureInit()
    {
        if ( m_Inited )
            return;

        // NO CASCADES AT ALL, and it has to be said HERE — at construction — rather than by the scene
        // setting below. A thumbnail is a lit object on a backdrop with shadows deliberately off; the
        // cascade framebuffers are allocated once inside Scene::Init() on the line after this one, from
        // the budget the renderer was BUILT with, so `settings.EnableShadows = false` a few lines further
        // down arrives after the money is spent. It was: 320 MiB of shadow maps for a renderer that has
        // never drawn a shadow and never will. See Graphic::ShadowQuality.
        m_Renderer = std::make_unique<Graphic::SceneRenderer>( Graphic::ViewExtent{ kRenderSize, kRenderSize },
                                                               Graphic::kThumbnailViewProfile );
        m_Scene           = std::make_shared<::Desert::Core::Scene>( "ThumbnailPreview", m_Renderer.get() );
        const auto inited = m_Scene->Init();
        if ( !inited.IsSuccess() )
        {
            // `m_Inited` stays false so the next call retries, which is the whole reason this is not a
            // bare `(void)`: with the result dropped the flag was set anyway, the preview was marked
            // ready, and every frame afterwards recorded into a scene that had never initialised.
            LOG_ERROR( "[AssetThumbnailRenderer] preview scene failed to initialise: {}", inited.GetError() );
            m_Scene.reset();
            m_Renderer.reset();
            return;
        }

        // Clean preview: no shadows bleeding into the thumbnail. Keep AA on (FXAA) for smoother edges;
        // supersampling (render 2x, downscale) adds the rest.
        //
        // `settings.EnableShadows = false` USED TO BE HERE and is gone: the shadowless budget above is
        // the same statement made where it is still worth something. Two ways to say one thing is how the
        // next reader ends up switching the one that no longer decides anything — and this one never
        // decided the allocation, only whether the maps it had already paid for were drawn into.
        //
        // The GRID needs no line here any more: it is a property of the VIEW now
        // (Graphic::DebugViewState, all-off by default) and only EditorLayer's main loop ever pushes the
        // editor's flags into a renderer. This used to switch the scene's own ShowGrid off, which worked
        // and said the wrong thing — a thumbnail scene had to know about an editor aid to opt out of it.
        // No PostProcessVolume in this scene, so the grade is Core::PostProcessSettings{} — bloom off.

        // `settings.AA = FXAA` used to stand here and said NOTHING: FXAA is the default, so the line
        // restated it. The mode is machine quality now (К3) and this renderer is simply never pushed to,
        // which leaves it at the schema defaults — the same silence the debug overlays already rely on.

        // Selection outline is an editor-preference now (no longer a scene setting); force it off on this
        // preview renderer so it never bleeds into a thumbnail (the main editor loop pushes it every frame,
        // but this offscreen renderer is never fed, so disable it explicitly).
        m_Renderer->SetOutlineSettings( glm::vec3( 0.0f ), 0.0f, 0.0f, false );

        // WHERE THIS SCENE'S CAPTURE CAMERA COMES FROM, and why this scene has no camera ENTITY.
        //
        // Scene::Init() has already made the camera: it constructs a Core::EditorCamera and hands it to
        // SetActiveCamera, which also publishes it as the scene's MAIN camera — and SceneRenderer::BeginScene
        // captures through `scene.GetMainCamera()`. So the camera that takes the picture is the engine's
        // default editor camera, owned by the engine, positioned by the engine's own defaults.
        //
        // There USED to be a `ThumbCam` entity here carrying a CameraComponent with AutoActivateForPlayer = true,
        // which read as the thing that made the capture work and was in fact inert. A scene
        // CameraComponent is a GAME camera: the only code that turns one into a camera object is
        // Scene::FindMainCamera (which nothing in the repository calls) and Scene::UpdateActiveCameraSource,
        // which consults camera entities only while the scene is in SceneState::Play. This scene is created,
        // rendered and destroyed in Edit, so the component was never read by anything. Measured rather
        // than argued: three material captures taken with the entity and without it are byte-identical
        // 1024px PNGs, against a repeat-run noise floor of zero bytes for this scene. Keeping it was worse
        // than useless — it invited the next reader to "fix" the thumbnail camera by editing a component
        // that does not reach the renderer.
        //
        // The comment that stood here also asserted the engine camera's defaults as literals — "sits at
        // ~(-4.33, 6.12, -4.33) looking at the origin (distance ~8.66), so thumbnails are framed by SCALING
        // the target at the origin to fit that fixed view". Every number in it was true when it was written
        // and false afterwards: the centimetre migration moved the default camera to eye height, focal
        // (0, 200, 0), and a subject left at the world origin then sits 200 units BELOW where the camera
        // aims — 70 degrees off a view axis with a 38-degree half-FOV, entirely outside the frustum. That
        // is Д30: the thumbnail stopped containing its subject at all, and only surfaced when a material
        // Save deleted a pre-migration PNG and forced a re-capture.
        //
        // So FitTarget reads the pose from the camera's OWN matrices (ThumbnailFraming::PlaceInView) and no
        // camera constant is written down anywhere in this file. A pose that is measured cannot go stale.

        // Key light pointing toward the camera-facing hemisphere (DirectionLight stores the *travel*
        // direction in Translation; the shader lights along -Direction). From above + the camera's side.
        auto  light           = m_Scene->CreateNewEntity( "ThumbLight" );
        auto& lightC          = light.AddComponent<ECS::DirectionLightComponent>();
        lightC.Data.Intensity = 3.5f;
        lightC.Data.Color     = { 1.0f, 0.97f, 0.92f }; // warm key
        light.GetComponent<ECS::TransformComponent>().Translation = { 2.0f, -6.0f, 5.0f };

        m_Target = m_Scene->CreateNewEntity( "ThumbTarget" );
        m_Target.AddComponent<ECS::StaticMeshComponent>();

        // THE CAMERA Scene::Init MADE, held by name. An object capture is framed against it, and after a
        // dome capture has pinned its own it can no longer be found by asking the scene.
        m_ObjectCamera = m_Scene->GetActiveCamera();

        m_Scene->AddSystem<ECS::MeshECSSystem>();
        m_Scene->AddSystem<ECS::SkyboxECSSystem>();
        // The Volume domain's system. Added unconditionally and costing nothing until an entity carries a
        // VolumetricCloudComponent: with none in the registry it emits one "no clouds present" command a
        // frame, which is also what takes the dome down again after a cloud capture.
        m_Scene->AddSystem<ECS::VolumetricCloudECSSystem>();

        // Procedural sky entity (drawn by SkyboxECSSystem) — gives a real backdrop gradient. ALSO call the
        // direct SceneRenderer::SetProceduralSky below so the sky is enabled from frame 0 (the ECS command
        // path alone proved insufficient in this minimal scene). Sun dir = the ThumbLight.
        // Through the engine's ONE negation, not a second hand-written one (ECS::Rules::AtmosphereSunDirection):
        // the light's Translation is the direction it TRAVELS, the sky wants the direction toward the sun.
        const glm::vec3 sunDir = ECS::Rules::AtmosphereSunDirection( glm::vec3( 2.0f, -6.0f, 5.0f ) );

        // IMPORTANT: which part of the dome ends up behind the subject is NOT knowable here. The subject is
        // placed on whatever view axis the engine's default camera currently has (see FitTarget), so the
        // backdrop is whatever that camera looks at — and the last time this comment named a pose ("sits
        // ABOVE the object at y=6.12 looking DOWN, so the backdrop samples the ground hemisphere") it was
        // describing a camera that had already moved, which is the mistake Д30 was made of.
        //
        // So the dome is authored to be a cohesive light blue at EVERY angle rather than tuned for one:
        // ground, horizon and zenith are all set, and GroundColor in particular is a soft sky-blue because
        // a dark ground reads as muddy grey after tonemap and looked like "no sky" when it was in shot.
        // That is a property of the backdrop, and it survives the camera moving again.
        auto  skyEnt             = m_Scene->CreateNewEntity( "ThumbSky" );
        m_SkyAtmosphere          = skyEnt;
        auto& skyC               = skyEnt.AddComponent<ECS::SkyAtmosphereComponent>();
        skyC.Data.ZenithColor    = { 0.26f, 0.46f, 0.78f };
        skyC.Data.HorizonColor   = { 0.62f, 0.73f, 0.87f };
        skyC.Data.GroundColor    = { 0.45f, 0.56f, 0.72f }; // visible behind the object (camera looks down)
        skyC.Data.SunColor       = { 1.00f, 0.95f, 0.85f };
        skyC.Data.SkyBrightness  = 1.15f;
        skyC.Data.HorizonFalloff = 0.5f;
        skyC.Data.SunGlow        = 0.8f;
        skyC.Data.StarIntensity  = 0.0f;
        skyC.Data.SunIntensity   = 16.0f;
        // 512x256 rather than the 1024x512 default, exactly as PreviewViewport's pane already does and for
        // the same reason: the panorama's only consumer here is the ambient term on one sphere or one
        // dome, in a picture that ends up 512 pixels wide. Four times the texels buys nothing it can show.
        //
        // AND IT IS NOW PAID OFTEN, which is what moved this line from "would be nice" to part of this
        // change: the dome capture puts a cloud layer in and out of this one scene between captures, and
        // SkyboxRenderer re-bakes whenever the cloud fingerprint changes, so a cold sweep of this
        // repository runs about 125 of these.
        //
        // THE SAVING IS 13 %, NOT 75 %, AND THE NUMBER IS MEASURED RATHER THAN COUNTED FROM THE TEXELS.
        // Two cold sweeps on this machine, one at each resolution, 123 and 125 bakes: median 419.3 ms ->
        // 365.9 ms, minimum 386.2 -> 338.8. Quartering the panorama did NOT quarter the bake, so whatever
        // dominates it is not the pixels — the line stays because 6 s off a cold sweep for a picture
        // nothing can tell apart is still worth having, and the next person to reach for a bigger win here
        // should go looking somewhere other than the resolution.
        skyC.Data.EnvironmentResolution = ECS::SkyEnvironmentResolution::Low;
        skyC.RequestBake                = true;

        // The SAME values via the direct call (enabled from frame 0) — through the one packing helper, so
        // this route and the ECS route cannot describe two different skies. The eight literals above used
        // to be typed a second time here, which is how a field added to the component reached the viewport
        // and not the thumbnails.
        // Default SunLightFx: a thumbnail has no sun light entity to read shafts from, and streaks in a
        // 128px preview would be noise anyway.
        m_Renderer->SetProceduralSky( true, sunDir, /*bakeNow=*/true, Graphic::MakeSkySettings( skyC.Data ),
                                      Graphic::SunLightFx{} );

        // Resize ONCE here (after the camera exists) so the camera projection becomes square. We render at
        // kRenderSize (2x the output) and downscale on write = supersampled anti-aliasing. The renderer was
        // built at this extent, so its half of the call rebuilds nothing; the camera's half is why it is here.
        m_Scene->Resize( kRenderSize, kRenderSize );

        m_Inited = true;
    }

    namespace
    {
        // The asset's thumbnail primitive as the shared primitive mesh drawn for it.
        Geometry::PrimitiveType PrimitiveFor( const Assets::ThumbnailPrimitive primitive )
        {
            switch ( primitive )
            {
                case Assets::ThumbnailPrimitive::Sphere:
                    return Geometry::PrimitiveType::Sphere;
                case Assets::ThumbnailPrimitive::Cube:
                    return Geometry::PrimitiveType::Cube;
                case Assets::ThumbnailPrimitive::Plane:
                    return Geometry::PrimitiveType::Plane;
                case Assets::ThumbnailPrimitive::Cylinder:
                    return Geometry::PrimitiveType::Cylinder;
            }
            return Geometry::PrimitiveType::Sphere;
        }
    } // namespace

    void AssetThumbnailRenderer::FitTarget( const glm::vec3& center, float worldSize )
    {
        auto& tc    = m_Target.GetComponent<ECS::TransformComponent>();
        tc.Rotation = glm::vec3( 0.0f );

        // Frame from the camera's OWN view/projection rather than an assumed pose (see ThumbnailFraming
        // for why: the preview camera's position and look-at both moved in the centimetre migration, which
        // is what made re-captured material thumbnails come out as a wall of colour or an empty sky — Д30).
        auto cam = m_Scene->GetMainCamera().lock();
        if ( !cam )
        {
            // No camera yet (should not happen after EnsureInit): keep the subject visible at unit scale
            // rather than divide framing math by a matrix that is not there.
            tc.Scale       = glm::vec3( 1.0f );
            tc.Translation = -center;
            return;
        }

        const auto placement = ThumbnailFraming::PlaceInView( cam->GetViewMatrix(), cam->GetProjectionMatrix(),
                                                              worldSize, center, m_PendingThumbnail.Orbit );
        tc.Scale       = glm::vec3( placement.Scale );
        tc.Translation = placement.Translation;
        tc.Rotation          = glm::eulerAngles( placement.Rotation );
    }

    void AssetThumbnailRenderer::RecordRender()
    {
        // Records the scene render into the CURRENT editor frame's command buffer. It is NOT submitted yet
        // (that happens when the editor's frame ends), so the readback must wait until a later frame -
        // see Collect().
        // The scene opens and closes its own renderer, so a refusal can no longer leave the editor's
        // frame command buffer holding half a pass — the frame is simply skipped and named.
        if ( const auto frame = m_Scene->OnUpdate( Common::Timestep( 0.016f ) ); !frame.IsSuccess() )
            LOG_ERROR( "[AssetThumbnailRenderer] preview frame skipped: {}", frame.GetError() );
    }

    Common::BoolResultStr AssetThumbnailRenderer::RequestMaterial( const Assets::AssetHandle&   materialHandle,
                                                                   const std::string&           outPng,
                                                                   ThumbnailSubject::Preview    how,
                                                                   const Assets::ThumbnailInfo& thumbnail )
    {
        if ( static_cast<uint64_t>( materialHandle ) == 0 )
            return Common::MakeFormattedError( "no material handle for '{}'", outPng );
        if ( m_Phase != 0 )
            return Common::MakeFormattedError( "a capture is already in flight; '{}' was not queued", outPng );

        // NO "IS THIS MATERIAL REALLY THERE" CHECK, AND THAT IS A DECISION — do not "finish the job" by
        // adding the mirror of RequestMesh's guard below. Reviewed and refused deliberately, teamlead
        // 2026-09-08.
        //
        // The asymmetry is real: a mesh that is not built photographs an empty backdrop, which was measured
        // in this tree. Nothing equivalent was ever observed for a material, and the test that LOOKS like
        // the missing guard does not ask the same question. MaterialService::Get( handle, path, pass )
        // needs a shader path and a render pass to answer at all, so calling it here would ask "can a
        // runtime material be built for the default pass right now" — while the capture builds it for the
        // preview scene's pass, later, in a different renderer. A material that answers no to the first
        // question and yes to the second is a FALSE refusal, and a false refusal here is worse than the
        // hole: it puts a working slot into the per-process failure set, permanently, with a message that
        // reads authoritative.
        //
        // Asset eviction does not open this hole either (checked when A7 landed): eviction parks a runtime
        // material in the graveyard and MaterialService::Get rebuilds it from the shell, so an evicted
        // material is not an unregistered one.
        //
        // WHAT WOULD CHANGE THE ANSWER: a measured case of a material capture producing a wrong picture, or
        // a service question that can be asked in the capture's own terms — not the availability of some
        // check that compiles.
        // RESIDENT BEFORE THE FIRST FRAME OF THE CAPTURE (AL1-8b). The sweep resolved this material a few
        // subjects ago, and AssetEviction may have released its payload since (a thumbnail is no scene root).
        // The cloud layer asks MaterialService::ResolveOverrides, which would then parse it inside the frame:
        // five `*_Clouds.demat` per Starter start. Read on a worker instead, with its closure.
        (void)Runtime::AwaitAssetClosure( materialHandle, Common::Content::ContentKind::Material );

        // A SKYBOX-DOMAIN DOME shows the HDR its cube slot binds; required now (the loader delivers it while
        // the dome settles), refused with the reason when there is none (the route already refused it).
        m_PendingSky.reset();
        if ( how == ThumbnailSubject::Preview::SkyDome )
        {
            auto sky = ThumbnailSubject::DomeSkyboxOf( materialHandle );
            if ( !sky )
                return Common::MakeFormattedError( "'{}' was not queued: {}", outPng, sky.GetError() );
            if ( sky.GetValue() )
            {
                m_PendingSky = Assets::AssetHandle( *sky.GetValue() );
                (void)Runtime::RequireSkybox( *m_PendingSky );
            }
        }
        m_PendingHandle  = materialHandle;
        m_PendingPng     = outPng;
        m_PendingSubject = Subject::Material;
        m_PendingPreview   = how;
        m_PendingThumbnail = thumbnail;
        // Render for several frames before reading back: the first renders after init aren't "warm" yet
        // (GPU mesh buffers + per-frame uniform-buffer ring slots need a few frames to fully populate), so an
        // early readback returns an empty frame. Capture happens on the last count (reads the prior, warm
        // frame's already-submitted render).
        m_Phase = kRenderFrames;
        m_CaptureMainMs = 0.0;
        m_CaptureTicks  = 0;
        // The dome needs its own, much longer window on top of that warm-up — see DomeIsStillSettling.
        m_DomeSettle = ( how == ThumbnailSubject::Preview::SkyDome ) ? kDomeSettleFrames : 0;
        m_DomeFrames = 0;
        m_Staged     = false;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AssetThumbnailRenderer::RequestSkybox( const Assets::AssetHandle& skyboxHandle,
                                                                 const std::string&         outPng )
    {
        if ( static_cast<uint64_t>( skyboxHandle ) == 0 )
            return Common::MakeFormattedError( "no skybox handle for '{}'", outPng );
        if ( m_Phase != 0 )
            return Common::MakeFormattedError( "a capture is already in flight; '{}' was not queued", outPng );
        // Required now: the loader delivers it while the dome settles (DomeIsStillSettling waits for it).
        if ( !Runtime::RequireSkybox( skyboxHandle ) )
            return Common::MakeFormattedError( "'{}' was not queued: the registry has no skybox {}", outPng,
                                               static_cast<uint64_t>( skyboxHandle ) );
        m_PendingSky       = skyboxHandle;
        m_PendingHandle    = skyboxHandle;
        m_PendingPng       = outPng;
        m_PendingSubject   = Subject::Sky;
        m_PendingThumbnail = {};
        m_Phase            = kRenderFrames;
        m_CaptureMainMs    = 0.0;
        m_CaptureTicks     = 0;
        m_DomeSettle       = kDomeSettleFrames;
        m_DomeFrames       = 0;
        m_Staged           = false;
        return Common::MakeSuccess( true );
    }

    namespace
    {
        // The mesh's own material slots, resolved through the material service — or REFUSED, naming the slot.
        // An unresolved slot used to empty the whole list, and an empty list is what the scene draws with its
        // default material: the thumbnail then showed a material the mesh does not have, as its picture. The
        // request is the moment to say so (ResolveMesh awaited the slots' closure, so a registered material
        // answers now; one that does not will not answer on a later frame either).
        Common::ResultStr<std::vector<Assets::AssetHandle>> MeshOwnSlots( const Assets::AssetHandle& meshHandle )
        {
            using Slots = std::vector<Assets::AssetHandle>;
            auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( meshHandle );
            if ( !asset )
                return Common::MakeFormattedError<Slots>( "mesh {} has no asset in the MeshService to read its "
                                                          "material slots from",
                                                          static_cast<uint64_t>( meshHandle ) );
            auto* materials = Runtime::ResourceRegistry::GetMaterialService();
            if ( materials == nullptr )
                return Common::MakeFormattedError<Slots>( "there is no material service to resolve mesh {}'s "
                                                          "material slots",
                                                          static_cast<uint64_t>( meshHandle ) );
            Slots       slots;
            const auto& externals = asset->GetMaterialHandles();
            for ( std::size_t i = 0; i < externals.size(); ++i )
            {
                // Unassigned -> the engine default (null staged); a broken reference -> refused by index.
                auto slot = ThumbnailSlots::SlotMaterial( externals[i], i, [&]( const Common::UUID& ref )
                                                          { return materials->GetAssetHandleByExternal( ref ); } );
                if ( !slot )
                    return Common::MakeFormattedError<Slots>( "mesh {} {}", static_cast<uint64_t>( meshHandle ),
                                                              slot.GetError() );
                slots.push_back( slot.GetValue() );
            }
            return Common::MakeSuccess( std::move( slots ) );
        }

        /// The pose a skinned capture draws, built as PreviewViewport builds it: no clip -> the bind pose
        /// evaluated by a zero step, a clip -> its middle frame. One builder for the frame measured at the
        /// request and the animator staged for the draw, so the two cannot disagree.
        std::unique_ptr<Animation::Animator> BuildPoseAnimator( const Desert::SkinnedMesh&                   mesh,
                                                                const Assets::Asset<Assets::AnimationAsset>& clip )
        {
            auto animator = std::make_unique<Animation::Animator>( mesh.GetSkeleton() );
            if ( clip )
            {
                const auto& c = clip->GetClip();
                animator->Play( c, true );
                animator->SetTick( Animation::FrameTime{ Animation::FrameNumber{ c.DurationTicks.Value / 2 } } );
            }
            else
            {
                animator->Update( Common::Timestep( 0.0f ) ); // the bind pose into the skinning matrices
            }
            return animator;
        }
    } // namespace

    Common::BoolResultStr AssetThumbnailRenderer::RequestMesh( const Assets::AssetHandle&    meshHandle,
                                                               const std::string&            outPng,
                                                               const Assets::ThumbnailOrbit& orbit,
                                                               const Assets::AssetHandle&    material )
    {
        if ( static_cast<uint64_t>( meshHandle ) == 0 )
            return Common::MakeFormattedError( "no mesh handle for '{}'", outPng );
        if ( m_Phase != 0 )
            return Common::MakeFormattedError( "a capture is already in flight; '{}' was not queued", outPng );

        // THE GEOMETRY HAS TO EXIST NOW, not merely be nameable. See the header for the measurement: a
        // handle the MeshService has not built captured the empty backdrop and every layer above read the
        // resulting file as a finished picture. Asked here rather than in Tick() because this is the last
        // moment the caller is still on the stack and can be told; five frames later there is only a PNG.
        auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( meshHandle );
        if ( !mesh )
        {
            return Common::MakeFormattedError(
                 "mesh {} is not built in the MeshService, so a capture would photograph an empty scene "
                 "and write it to '{}' as if it were the asset",
                 static_cast<uint64_t>( meshHandle ), outPng );
        }
        if ( mesh->GetSubmeshes().empty() )
        {
            return Common::MakeFormattedError(
                 "mesh {} is built but has no submeshes, so there is nothing to photograph for '{}'",
                 static_cast<uint64_t>( meshHandle ), outPng );
        }
        // THE FRAME IS THE ASSET'S OWN BOUNDS, measured here where a refusal can still be told (UE frames a
        // thumbnail by the asset's Bounds and takes no picture of an object that has none).
        const auto frame = ThumbnailFraming::MeasureSubmeshes( mesh->GetSubmeshes() );
        if ( !frame.Valid )
        {
            return Common::MakeFormattedError(
                 "mesh {} has {} submesh(es) but every bounding box is empty, so there is no frame to put a "
                 "camera on for '{}'",
                 static_cast<uint64_t>( meshHandle ), mesh->GetSubmeshes().size(), outPng );
        }
        m_PendingFrame = frame;

        // No material worn over every slot -> the mesh's OWN slots, every one resolved or the capture refused.
        m_PendingSlots.clear();
        if ( static_cast<uint64_t>( material ) == 0 )
        {
            auto slots = MeshOwnSlots( meshHandle );
            if ( !slots )
                return Common::MakeFormattedError( "'{}' was not queued: {}", outPng, slots.GetError() );
            m_PendingSlots = std::move( slots.GetValue() );
        }

        m_PendingHandle   = meshHandle;
        m_PendingMaterial        = material;
        m_PendingThumbnail       = Assets::ThumbnailInfo{};
        m_PendingThumbnail.Orbit = orbit;
        m_PendingPng      = outPng;
        m_PendingSubject  = Subject::Mesh;
        m_PendingClip     = nullptr;
        m_Phase           = kRenderFrames;
        m_CaptureMainMs   = 0.0;
        m_CaptureTicks    = 0;
        m_DomeSettle      = 0;
        m_DomeFrames      = 0;
        m_Staged          = false;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AssetThumbnailRenderer::RequestPose( const Assets::AssetHandle&            meshHandle,
                                                               Assets::Asset<Assets::AnimationAsset> clip,
                                                               const std::string&                    outPng ,
                                                               const Assets::ThumbnailOrbit&         orbit)
    {
        // Refused BEFORE the shared checks queue anything: a static mesh posed would stage a skinned
        // component on geometry with no skeleton and photograph nothing.
        if ( static_cast<uint64_t>( meshHandle ) != 0 )
        {
            const auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( meshHandle );
            if ( mesh != nullptr && !mesh->IsSkinned() )
                return Common::MakeFormattedError(
                     "mesh {} is not skinned, so there is no pose to photograph for '{}'",
                     static_cast<uint64_t>( meshHandle ), outPng );
        }
        // EVERY SLOT MUST HAVE A (Skinned x Forward) CELL, asked BEFORE anything is queued and by the rule the
        // scene's material build refuses by (MaterialService::CellOf — the rule
        // CreateSurfaceMaterial builds by). The
        // scene answers a missing cell by substituting its default material; a thumbnail of THAT is a picture
        // of a material the mesh does not have, so the capture is refused with the reason instead (THM1n-10).
        // The pass does not change the answer for a skinned path: a template with a Surface block has every
        // cell, one without has only (Static x Forward).
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return Common::MakeFormattedError( "there is no material service to ask skinned mesh {}'s slots "
                                               "for a (Skinned x Forward) cell, for '{}'",
                                               static_cast<uint64_t>( meshHandle ), outPng );
        const auto ownSlots = MeshOwnSlots( meshHandle );
        if ( !ownSlots )
            return Common::MakeFormattedError( "skinned mesh {} cannot be photographed for '{}': {}",
                                               static_cast<uint64_t>( meshHandle ), outPng, ownSlots.GetError() );
        for ( const auto& slot : ownSlots.GetValue() )
        {
            if ( slot.IsNull() )
                continue; // unassigned: the engine's default skinned material, which every path has
            if ( const auto cell =
                      materials->CellOf( slot, Graphic::MeshVertexPath::Skinned, Graphic::MeshPass::Forward );
                 !cell )
                return Common::MakeFormattedError( "skinned mesh {} cannot be photographed for '{}': {}",
                                                   static_cast<uint64_t>( meshHandle ), outPng, cell.GetError() );
        }

        auto queued = RequestMesh( meshHandle, outPng, orbit );
        if ( !queued.IsSuccess() )
            return queued;

        // A SKINNED MESH IS FRAMED AS IT IS DRAWN. Its submesh boxes are raw-vertex space, which is the drawn
        // mesh only when the bind is identity; TwoJointProbe's boxes said 80 cm while the bind drew it at a
        // different size, and the camera photographed the sky from inside it. The posed vertices cannot lie.
        const auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( meshHandle );
        // IsSkinned() is the mesh's own type tag: a skinned mesh IS a SkinnedMesh.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        const auto* skinnedMesh = static_cast<const Desert::SkinnedMesh*>( mesh );
        const auto  animator    = BuildPoseAnimator( *skinnedMesh, clip );
        const auto  box =
             Geometry::MeasurePosedVertices( skinnedMesh->GetVertices(), animator->GetPose().Matrices );
        const auto frame = ThumbnailFraming::FrameOfBox( box.Min, box.Max );
        if ( !frame.Valid )
        {
            m_Phase = 0; // withdrawn: the request above queued it before the pose could be measured
            return Common::MakeFormattedError(
                 "skinned mesh {} keeps no CPU vertices to measure its pose by, so there is no frame to put a "
                 "camera on for '{}'",
                 static_cast<uint64_t>( meshHandle ), outPng );
        }
        m_PendingFrame   = frame;
        m_PendingSubject = Subject::Pose;
        m_PendingClip    = std::move( clip );
        return queued;
    }

    void AssetThumbnailRenderer::StagePose()
    {
        auto& smc      = m_Target.GetComponent<ECS::StaticMeshComponent>();
        smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        smc.Primitive.reset();
        smc.MaterialSlots.clear();
        smc.RuntimeMaterialInstances.clear();
        ECS::ClearEditableMesh( smc );

        // As PreviewViewport::SetSkinnedMesh + ApplyAnimationTime stage it: this scene runs no
        // AnimationECSSystem either, so the animator is built here and evaluated once.
        auto& skinned         = m_Target.AddComponent<ECS::SkinnedMeshComponent>();
        skinned.MeshHandle    = m_PendingHandle;
        skinned.MaterialSlots = m_PendingSlots; // resolved (or refused) at RequestMesh
        auto& anim            = m_Target.AddComponent<ECS::AnimationComponent>();
        anim.CurrentClip      = m_PendingClip ? m_PendingClip->GetClip().AnimationName : std::string();
        anim.Playing          = false;
        anim.Loop             = true;

        if ( auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( m_PendingHandle );
             mesh != nullptr && mesh->IsSkinned() )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast) -- IsSkinned() is the type tag
            anim.Animator = BuildPoseAnimator( *static_cast<Desert::SkinnedMesh*>( mesh ), m_PendingClip );
        }
        m_FitFrame = m_PendingFrame;
        FitTarget( m_PendingFrame.Center, m_PendingFrame.Extent );
    }

    void AssetThumbnailRenderer::PinDomeCamera()
    {
        if ( !m_DomeCamera )
            m_DomeCamera = std::make_shared<::Desert::Core::GameplayCamera>();

        // PINNED, not merely set active: Scene::OnUpdate re-picks the camera from the play state every
        // frame, so a plain SetActiveCamera survives exactly one frame before the scene's own
        // EditorCamera takes the view back.
        //
        // AND THE PIN IS WHY THE OBJECT CAMERA STOPS MOVING AFTERWARDS. PinActiveCamera also mutes the
        // scene's EditorCamera, which until now polled the global mouse from inside this offscreen
        // scene — so an object capture's subject was framed against wherever the person happened to be
        // flying the real viewport. FitTarget reads the pose from the camera's own matrices, so the
        // picture was right either way; from here it is also REPRODUCIBLE.
        m_DomeCamera->SetFromTransform( glm::vec3( 0.0f, kDomeEyeHeight, 0.0f ),
                                        glm::vec3( kDomePitch, kDomeYaw, 0.0f ), kDomeFov, kDomeNearPlane,
                                        kDomeFarPlane, kRenderSize, kRenderSize );
        m_Scene->PinActiveCamera( m_DomeCamera );
    }

    void AssetThumbnailRenderer::ResetPreviewScene()
    {
        // The subject: a pose capture's two components go, the static mesh component is emptied.
        if ( m_Target.HasComponent<ECS::AnimationComponent>() )
            m_Target.RemoveComponent<ECS::AnimationComponent>();
        if ( m_Target.HasComponent<ECS::SkinnedMeshComponent>() )
            m_Target.RemoveComponent<ECS::SkinnedMeshComponent>();
        auto& smc      = m_Target.GetComponent<ECS::StaticMeshComponent>();
        smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        smc.Primitive.reset();
        smc.MaterialSlots.clear();
        smc.RuntimeMaterialInstances.clear();
        ECS::ClearEditableMesh( smc );

        // The layers: switched OFF rather than destroyed — destroying the deck would give back its 10 MiB
        // and pay for it again on the next cloud material, of which this project has 52.
        if ( m_CloudLayer && m_CloudLayer.HasComponent<ECS::VolumetricCloudComponent>() )
            m_CloudLayer.GetComponent<ECS::VolumetricCloudComponent>().Data.Enabled = false;
        if ( m_SkyboxLayer )
            m_SkyboxLayer.GetComponent<ECS::SkyboxComponent>().SkyboxHandle = Assets::AssetHandle();
        m_SkyAtmosphere.GetComponent<ECS::SkyAtmosphereComponent>().Data.Enabled = true;

        // The camera Scene::Init made, by name. Unpinning alone would leave the DOME camera active until
        // the scene's next OnUpdate — which is after FitTarget has already framed against it.
        if ( m_DomeCamera && m_ObjectCamera )
        {
            m_Scene->PinActiveCamera( nullptr );
            m_Scene->SetActiveCamera( m_ObjectCamera );
        }
        m_FitFrame.reset();
    }

    bool AssetThumbnailRenderer::StageSubject()
    {
        auto& smc = m_Target.GetComponent<ECS::StaticMeshComponent>();

        // ── THE DOME: A MEDIUM, NOT A SURFACE ─────────────────────────────────────────────────────────
        //
        // Nothing rides the mesh path here — a cloud material has no surface to put on a ball, and the
        // mesh path would refuse it by name anyway (MeshRenderer::DrawGenericMeshes). What is photographed
        // is the SKY it authors, from a camera standing on a rise and looking up.
        if ( IsDomeCapture() )
        {
            smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            smc.Primitive.reset();
            smc.MaterialSlots.clear();
            smc.RuntimeMaterialInstances.clear();
            ECS::ClearEditableMesh( smc );

            if ( m_PendingSky )
            {
                // THE SKY IS THE MATERIAL'S HDR, drawn by the scene's skybox from the same ground camera.
                if ( !m_SkyboxLayer )
                {
                    m_SkyboxLayer = m_Scene->CreateNewEntity( "ThumbSkybox" );
                    m_SkyboxLayer.AddComponent<ECS::SkyboxComponent>();
                }
                m_SkyboxLayer.GetComponent<ECS::SkyboxComponent>().SkyboxHandle = *m_PendingSky;
                m_SkyAtmosphere.GetComponent<ECS::SkyAtmosphereComponent>().Data.Enabled = false;
                PinDomeCamera();
                return true;
            }

            if ( !m_CloudLayer )
            {
                m_CloudLayer = m_Scene->CreateNewEntity( "ThumbCloudLayer" );
                m_CloudLayer.AddComponent<ECS::VolumetricCloudComponent>();
            }

            auto& cloud        = m_CloudLayer.GetComponent<ECS::VolumetricCloudComponent>();
            cloud.Data.Enabled = true;
            // A STILL SKY. Measured in the interactive pane at 90.5 ms of a 105.8 ms frame: wind changes
            // the cloud fingerprint every frame, and SkyboxRenderer re-bakes its 485 ms environment
            // panorama whenever that changes. It also shows nothing in a still photograph.
            cloud.Data.WindSpeed         = 0.0f;
            cloud.Data.Material          = m_PendingHandle;
            cloud.Data.MaxSteps          = kDomeMaxSteps;
            cloud.Data.StopTransmittance = kDomeStopTransmittance;
            cloud.Data.VolumeResolution  = kDomeVolumeResolution;

            PinDomeCamera();
            return true;
        }

        // ── NOT THE DOME: the base scene ResetPreviewScene left is already the object scene ─────────────
        if ( m_PendingSubject == Subject::Pose )
        {
            StagePose();
        }
        else if ( m_PendingSubject == Subject::Mesh )
        {
            // Asset mesh, auto-framed by its bounds. Apply the mesh's linked (sidecar) material to every slot
            // if one was provided, so the preview shows the real look instead of a flat default gray.
            ECS::ClearEditableMesh( smc );
            smc.Primitive.reset();
            smc.RuntimeMaterialInstances.clear();
            smc.MeshHandle = m_PendingHandle;

            // Framed by the asset's bounds measured at the request (RequestMesh refused a mesh without them).
            if ( auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( m_PendingHandle ) )
            {
                // Slot count = submesh count; a sidecar material wears every slot. Without one, THE MESH'S OWN
                // SLOTS — the .demat each submesh names by GUID, as an imported mesh carries them and as the
                // scene draws them (MeshECSSystem). Clearing them here photographed every import in the
                // fallback grey. Resolved once at the request, which refuses an unresolved slot by index.
                if ( static_cast<uint64_t>( m_PendingMaterial ) != 0 )
                    smc.MaterialSlots.assign( std::max<size_t>( 1, mesh->GetSubmeshes().size() ),
                                              m_PendingMaterial );
                else
                    smc.MaterialSlots = m_PendingSlots; // resolved (or refused) at RequestMesh
            }
            else
            {
                smc.MaterialSlots.clear();
            }
            m_FitFrame = m_PendingFrame;
            FitTarget( m_PendingFrame.Center, m_PendingFrame.Extent );
        }
        else
        {
            // Material preview. Geometry is built once and reused; clearing the runtime instances forces a
            // rebuild against the current material handle. Always the sphere: a masked material is cut by
            // the mesh path's alpha discard, so its blades show against the backdrop.
            smc.MeshHandle    = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            smc.Primitive     = PrimitiveFor( m_PendingThumbnail.Primitive );
            smc.MaterialSlots = { m_PendingHandle };
            smc.RuntimeMaterialInstances.clear();
            ECS::ClearEditableMesh( smc ); // drop any previously-built primitive so the type change rebuilds

            // MEASURED from the very mesh MeshECSSystem will draw for this component (the process-wide
            // shared primitive), never assumed. The assumption this replaces — the literal `worldSize = 1.0`
            // passed to FitTarget — was minted when primitives were authored at one unit, and survived the
            // centimetre migration that scales every primitive by Common::Units::UnitsPerMetre: the sphere
            // is 100 units across, so the old rule scaled it by 4 instead of 0.04 and drew it 100x too big.
            // Combined with the second half of Д30 (the subject was also left at the world origin, which
            // the migrated camera no longer looks at), the capture came out as the flank of a 400-unit
            // sphere the camera was practically resting on: a wall of albedo under sky, with no sphere in
            // it. See ThumbnailFraming for the framing rule and the measured geometry.
            // No stand-in: a sphere that cannot be measured is not photographed (TickCapture abandons it).
            const auto* prim  = Geometry::PrimitiveMeshFactory::GetShared( *smc.Primitive );
            const auto  frame = prim != nullptr ? ThumbnailFraming::MeasureSubmeshes( prim->GetSubmeshes() )
                                                : ThumbnailFraming::Frame{};
            if ( !frame.Valid )
            {
                LOG_ERROR( "[AssetThumbnailRenderer] the preview sphere has no measurable bounds, so '{}' is not "
                           "captured",
                           m_PendingPng );
                return false;
            }
            m_FitFrame = frame;
            FitTarget( frame.Center, frame.Extent );
        }
        return true;
    }

    bool AssetThumbnailRenderer::DomeIsStillSettling()
    {
        if ( !IsDomeCapture() )
            return false;

        if ( m_DomeFrames >= kDomeMaxSettleFrames )
        {
            // SAID ONCE, AND THE CAPTURE STILL HAPPENS. A dome that is photographed early is a soft or
            // dithered cloud; a capture abandoned here would hold the editor's one slot until the process
            // ended. The first is a worse picture, the second is no pictures at all.
            if ( m_DomeSettle > 0 )
            {
                LOG_WARN( "[AssetThumbnailRenderer] the cloud volume for '{}' was still baking after {} "
                          "frames — capturing anyway, so the tile may show a march that has not converged.",
                          m_PendingPng, kDomeMaxSettleFrames );
                m_DomeSettle = 0;
            }
            return false;
        }
        ++m_DomeFrames;

        // COUNTED FROM THE END OF THE BAKE. The modelling volume is built on a worker while these frames
        // run, and a march through a volume that is not there yet converges on nothing — so seeing a bake
        // in flight restarts the window rather than merely extending it.
        if ( m_Renderer->IsCloudVolumeBaking() )
            m_DomeSettle = kDomeSettleFrames;
        // A skybox dome's HDR still on the loader: nothing to photograph yet, so the window restarts too.
        if ( m_PendingSky )
        {
            auto* skies = Runtime::ResourceRegistry::GetSkyboxService();
            if ( skies != nullptr && !skies->Get( *m_PendingSky ) )
                m_DomeSettle = kDomeSettleFrames;
        }

        if ( m_DomeSettle <= 0 )
            return false;

        --m_DomeSettle;
        return true;
    }

    void AssetThumbnailRenderer::Tick()
    {
        if ( !HasPending() )
            return;
        const auto began = std::chrono::steady_clock::now();
        TickCapture();
        m_CaptureMainMs +=
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count();
        ++m_CaptureTicks;
    }

    void AssetThumbnailRenderer::TickCapture()
    {
        if ( m_Readback )
        {
            AdvanceReadback();
            return;
        }
        if ( m_Phase == 0 )
            return;
        EnsureInit();
        if ( !m_Inited )
        {
            // The scene refused to initialise and EnsureInit has already said why. ABANDON the capture
            // rather than returning with the phase intact: every line below dereferences entities that do
            // not exist, and a phase that never reaches zero holds the editor's one capture slot for the
            // rest of the session. The service sees no PNG appear and reports the failure in its own terms.
            m_Phase = 0;
            return;
        }

        // NOTE ON WHAT IS NOT HERE. This used to add an ECS::MaterialComponent to the preview target and
        // write MaterialComponent::ShaderName into it, to serve a RequestShader() entry point that nothing
        // in the editor ever called. That is the shader-OVERRIDE route, and it is the exact route behind
        // the Stage-1 defect where a thumbnail showed a correct material while the scene showed black —
        // the two paths resolve a material differently, so a preview taken on one proves nothing about the
        // other (Docs/MaterialEditor/STAGE1_END_TO_END.md). Every capture below now goes through
        // MaterialSlots, the per-slot route the scene itself uses.
        // ONCE PER CAPTURE: the scene back to its base, then this capture's subject into it. Every later
        // warm-up tick only re-fits the object to the camera's current matrices.
        if ( !m_Staged )
        {
            ResetPreviewScene();
            if ( !StageSubject() )
            {
                // Nothing measurable to frame, and StageSubject said so: abandoned like a scene that would
                // not initialise, so no picture of the backdrop is written as the asset.
                m_Phase = 0;
                return;
            }
            m_Staged = true;
        }
        else if ( m_FitFrame )
        {
            FitTarget( m_FitFrame->Center, m_FitFrame->Extent );
        }

        // Render this frame (recorded into the editor's in-flight frame, submitted at frame end).
        RecordRender();

        // The dome's frames do not count as warm-up until the volume is there and the march has settled.
        if ( DomeIsStillSettling() )
            return;

        if ( m_Phase > 1 )
        {
            // Still warming up; capture on the last count.
            --m_Phase;
            return;
        }

        // Final count: the PREVIOUS (warm) frame's render is submitted + (after this wait) finished, so the
        // framebuffer readback returns it. (This frame's render isn't submitted yet, so it doesn't interfere.)
        // NO DEVICE IDLE AND NO WAIT HERE (TH3). The copy is recorded behind every frame already on the queue
        // (BeginReadbackRGBA8 carries the whole-queue barrier) and polled by its fence on later Ticks.
        const auto began = std::chrono::steady_clock::now();
        if ( auto finalImage = m_Scene->GetFinalImage() )
        {
            auto readback = finalImage->BeginReadbackRGBA8();
            if ( readback.IsSuccess() )
            {
                m_Readback       = readback.GetValue();
                m_ReadbackPng    = m_PendingPng;
                m_ReadbackBegan  = began;
                m_ReadbackFrames = 0;
                m_ReadbackSubmitMs =
                     std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count();
            }
            else
                LOG_ERROR( "[AssetThumbnailRenderer] readback for '{}' was refused: {} — no thumbnail written.",
                           m_PendingPng, readback.GetError() );
        }
        else
        {
            LOG_ERROR( "[AssetThumbnailRenderer] '{}' has no final image after {} warm-up frames — no "
                       "thumbnail written.",
                       m_PendingPng, kRenderFrames );
        }

        static constexpr bool kDebugLoopForCapture = false;
        m_Phase = kDebugLoopForCapture ? kRenderFrames : 0;
    }

    void AssetThumbnailRenderer::AdvanceReadback()
    {
        ++m_ReadbackFrames;
        if ( !m_Encode.valid() )
        {
            if ( !m_Readback->IsComplete() )
                return;
            // Read, downscale and write on a worker. The readback is shared so the object outlives the job
            // even if this renderer is torn down first (the destructor waits for the job anyway).
            m_Encode = Common::JobSystem::Get().Async(
                 [readback = m_Readback, png = m_ReadbackPng]() -> Encoded
                 {
                     using Clock   = std::chrono::steady_clock;
                     const auto ms = []( Clock::time_point from, Clock::time_point to )
                     { return std::chrono::duration<double, std::milli>( to - from ).count(); };
                     Encoded    out;
                     const auto t0     = Clock::now();
                     auto       pixels = readback->ReadRGBA8();
                     const auto t1     = Clock::now();
                     out.ReadMs        = ms( t0, t1 );
                     if ( !pixels.IsSuccess() )
                     {
                         out.Written = Common::MakeError<bool>( pixels.GetError() );
                         return out;
                     }
                     auto       boxed = ThumbnailEncode::Downscale( pixels.GetValue(), kRenderSize, kSize );
                     const auto t2    = Clock::now();
                     out.BoxMs        = ms( t1, t2 );
                     if ( !boxed.IsSuccess() )
                     {
                         out.Written = Common::MakeError<bool>( boxed.GetError() );
                         return out;
                     }
                     out.Written = ThumbnailEncode::WritePng( boxed.GetValue(), kSize, png );
                     out.PngMs   = ms( t2, Clock::now() );
                     return out;
                 } );
            return;
        }
        if ( m_Encode.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            return;

        const Encoded encoded = m_Encode.get();
        m_Readback.reset(); // staging buffer and fence go back on the device thread
        if ( !encoded.Written.IsSuccess() )
            LOG_ERROR( "[AssetThumbnailRenderer] '{}': {} — no thumbnail written.", m_ReadbackPng,
                       encoded.Written.GetError() );
        LOG_DEBUG( "[Thumbnails] captured '{}' in {:.0f} ms over {} frames (main: {:.1f} over {} ticks, submit "
                   "{:.1f}; worker: read "
                   "{:.0f}, downscale {:.0f}, png {:.0f}) at {}px from a {}px render",
                   std::filesystem::path( m_ReadbackPng ).filename().string(),
                   std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_ReadbackBegan )
                        .count(),
                   m_ReadbackFrames, m_CaptureMainMs, m_CaptureTicks, m_ReadbackSubmitMs, encoded.ReadMs,
                   encoded.BoxMs, encoded.PngMs, kSize, kRenderSize );
    }
} // namespace Desert::Editor
