#include "AssetThumbnailRenderer.hpp"

#include <Editor/Widgets/ThumbnailFraming.hpp>
#include <Editor/RenderSystems/Passes/EditorCubemapPreviewPass.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Units.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>

// STB_IMAGE_WRITE_IMPLEMENTATION is already compiled into Desert.lib (stb_image.obj); just declare here.
#include <stb_image/stb_image_write.h>

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
        m_Renderer        = std::make_unique<Graphic::SceneRenderer>( Graphic::kNoShadowQuality );
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
        auto& settings       = m_Scene->GetSettings();
        settings.EnableBloom = false;

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
        // There USED to be a `ThumbCam` entity here carrying a CameraComponent with IsMainCamera = true,
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
        // kRenderSize (2x the output) and downscale on write = supersampled anti-aliasing. Resize recreates
        // framebuffers + idles the GPU, so we never call it per render.
        m_Scene->Resize( kRenderSize, kRenderSize );

        m_Inited = true;
    }

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

        const auto placement =
             ThumbnailFraming::PlaceInView( cam->GetViewMatrix(), cam->GetProjectionMatrix(), worldSize, center );
        tc.Scale       = glm::vec3( placement.Scale );
        tc.Translation = placement.Translation;
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

    Common::BoolResultStr AssetThumbnailRenderer::RequestMaterial( const Assets::AssetHandle& materialHandle,
                                                                   const std::string&         outPng,
                                                                   ThumbnailSubject::Preview  how )
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
        m_PendingHandle  = materialHandle;
        m_PendingPng     = outPng;
        m_PendingSubject = Subject::Material;
        m_PendingPreview = how;
        // Render for several frames before reading back: the first renders after init aren't "warm" yet
        // (GPU mesh buffers + per-frame uniform-buffer ring slots need a few frames to fully populate), so an
        // early readback returns an empty frame. Capture happens on the last count (reads the prior, warm
        // frame's already-submitted render).
        m_Phase = kRenderFrames;
        // The dome needs its own, much longer window on top of that warm-up — see DomeIsStillSettling.
        m_DomeSettle = ( how == ThumbnailSubject::Preview::SkyDome ) ? kDomeSettleFrames : 0;
        m_DomeFrames = 0;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AssetThumbnailRenderer::RequestMesh( const Assets::AssetHandle& meshHandle,
                                                               const std::string&         outPng,
                                                               const Assets::AssetHandle& material )
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

        m_PendingHandle   = meshHandle;
        m_PendingMaterial = material;
        m_PendingPng      = outPng;
        m_PendingSubject  = Subject::Mesh;
        m_Phase           = kRenderFrames;
        m_DomeSettle      = 0;
        m_DomeFrames      = 0;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AssetThumbnailRenderer::RequestSkybox( const Assets::AssetHandle& skyboxHandle,
                                                                 const std::string&         outPng )
    {
        if ( static_cast<uint64_t>( skyboxHandle ) == 0 )
            return Common::MakeFormattedError( "no skybox handle for '{}'", outPng );
        if ( m_Phase != 0 )
            return Common::MakeFormattedError( "a capture is already in flight; '{}' was not queued", outPng );

        // THE CUBES HAVE TO EXIST NOW. The same guard, for the same measured reason, as RequestMesh's:
        // without them the ball resolves to nothing, the capture photographs the backdrop, and the PNG is
        // filed as this sky's picture with nothing able to tell it apart from a real one.
        const auto material = Runtime::ResourceRegistry::GetSkyboxService()->Get( skyboxHandle );
        if ( !material )
        {
            return Common::MakeFormattedError(
                 "skybox {} is not registered in the skybox service, so a capture would photograph an "
                 "empty scene and write it to '{}' as if it were the sky",
                 static_cast<uint64_t>( skyboxHandle ), outPng );
        }
        if ( !material->GetEnvironment().RadianceMap.IsValid() )
        {
            return Common::MakeFormattedError(
                 "skybox {} has no radiance cube (its panorama did not bake), so there is nothing to "
                 "photograph for '{}'",
                 static_cast<uint64_t>( skyboxHandle ), outPng );
        }

        m_PendingHandle  = skyboxHandle;
        m_PendingPng     = outPng;
        m_PendingSubject = Subject::Skybox;
        m_Phase          = kRenderFrames;
        m_DomeSettle     = 0;
        m_DomeFrames     = 0;
        return Common::MakeSuccess( true );
    }

    void AssetThumbnailRenderer::StageSubject()
    {
        auto& smc = m_Target.GetComponent<ECS::StaticMeshComponent>();

        // ── THE DOME: A MEDIUM, NOT A SURFACE ─────────────────────────────────────────────────────────
        //
        // Nothing rides the mesh path here — a cloud material has no surface to put on a ball, and the
        // mesh path would refuse it by name anyway (MeshRenderer::DrawGenericMeshes). What is photographed
        // is the SKY it authors, from a camera standing on a rise and looking up.
        if ( m_PendingSubject == Subject::Material && m_PendingPreview == ThumbnailSubject::Preview::SkyDome )
        {
            smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            smc.Primitive.reset();
            smc.MaterialSlots.clear();
            smc.RuntimeMaterialInstances.clear();
            smc.RuntimeMesh.reset();

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
            return;
        }

        // ── THE HDR SKY: A CUBEMAP ON A BALL ──────────────────────────────────────────────────────────
        //
        // Nothing rides the mesh path here either. A cubemap has no surface: the engine's own skybox
        // program shows it BY DIRECTION on the far plane, which is a picture of a background and not of
        // a thing. The ball is the other presentation — the environment wrapped on an object — and it is
        // drawn by the very pass the Material Editor's cubemap pane uses, so the tile and the pane cannot
        // show two different skies.
        if ( m_PendingSubject == Subject::Skybox )
        {
            smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            smc.Primitive.reset();
            smc.MaterialSlots.clear();
            smc.RuntimeMaterialInstances.clear();
            smc.RuntimeMesh.reset();

            if ( !m_CubemapPass )
            {
                auto pass = std::make_unique<Render::EditorCubemapPreviewPass>();
                if ( const auto installed = pass->Install( m_Scene ); !installed )
                {
                    // ABANDON, NAMED. Returning with the phase intact would photograph an empty scene
                    // five frames later and file it as this sky.
                    LOG_ERROR( "[AssetThumbnailRenderer] '{}' not captured: the cubemap pass is "
                               "unavailable: {}",
                               m_PendingPng, installed.GetError() );
                    m_Phase = 0;
                    return;
                }
                m_CubemapPass = std::move( pass );
            }

            // RE-RESOLVED EVERY FRAME through the handle, never captured as a pointer: the cubes are
            // rebuilt whenever the sky's authored look changes (MaterialSkybox::EnsureBaked), and a
            // pointer taken here would outlive the image it names by exactly one rebake.
            const Assets::AssetHandle handle = m_PendingHandle;
            m_CubemapPass->SetSource(
                 [handle]() -> const Graphic::ImageCube*
                 {
                     const auto material = Runtime::ResourceRegistry::GetSkyboxService()->Get( handle );
                     if ( !material )
                         return nullptr;
                     const auto& environment = material->GetEnvironment();
                     if ( !environment.RadianceMap.IsValid() )
                         return nullptr;
                     return static_cast<Graphic::ImageCube*>(
                          Runtime::ResourceRegistry::GetImageService()->Resolve( environment.RadianceMap ) );
                 },
                 kSkyboxBallRadius );

            // The ball's DIAMETER is what has to fit the frame, and FitTarget takes the subject's world
            // size. Passing the radius here would frame half the ball and crop the rest — the same
            // half-vs-whole slip the mesh branch's AABB extent had to be measured out of.
            FitTarget( glm::vec3( 0.0f ), kSkyboxBallRadius * 2.0f );
            return;
        }

        // ── NOT THE DOME OR THE BALL: TAKE THEM DOWN ──────────────────────────────────────────────────
        //
        // The ball is an external pass with its own source, so switching subject must clear it or every
        // material captured after one sky would be photographed standing behind that sky's ball.
        if ( m_CubemapPass )
            m_CubemapPass->ClearSource();

        // ── NOT THE DOME: TAKE IT DOWN ────────────────────────────────────────────────────────────────
        //
        // The scene is shared between the three pictures, so the layer has to be switched off rather than
        // merely not switched on: a live deck would otherwise sit over every material and mesh captured
        // after the first cloud one. Off rather than destroyed, because destroying it would give back the
        // 10 MiB and then pay for it again on the next cloud material, of which this project has 52.
        if ( m_CloudLayer && m_CloudLayer.HasComponent<ECS::VolumetricCloudComponent>() )
            m_CloudLayer.GetComponent<ECS::VolumetricCloudComponent>().Data.Enabled = false;

        // The camera Scene::Init made, by name. Unpinning alone would leave the DOME camera active until
        // the scene's next OnUpdate — which is after FitTarget has already framed against it.
        if ( m_DomeCamera && m_ObjectCamera )
        {
            m_Scene->PinActiveCamera( nullptr );
            m_Scene->SetActiveCamera( m_ObjectCamera );
        }

        if ( m_PendingSubject == Subject::Mesh )
        {
            // Asset mesh, auto-framed by its bounds. Apply the mesh's linked (sidecar) material to every slot
            // if one was provided, so the preview shows the real look instead of a flat default gray.
            smc.RuntimeMesh.reset();
            smc.Primitive.reset();
            smc.RuntimeMaterialInstances.clear();
            smc.MeshHandle = m_PendingHandle;

            glm::vec3 center( 0.0f );
            float     extent = 1.0f;
            if ( auto* mesh = Runtime::ResourceRegistry::GetMeshService()->Get( m_PendingHandle ) )
            {
                // Union of the submesh AABBs in MESH space — ThumbnailFraming::MeasureSubmeshes, shared
                // with the material branch below so both frame what is actually drawn.
                if ( const auto frame = ThumbnailFraming::MeasureSubmeshes( mesh->GetSubmeshes() ); frame.Valid )
                {
                    center = frame.Center;
                    extent = frame.Extent;
                }

                // Slot count = submesh count; fill with the linked material (or leave default if none).
                if ( static_cast<uint64_t>( m_PendingMaterial ) != 0 )
                    smc.MaterialSlots.assign( std::max<size_t>( 1, mesh->GetSubmeshes().size() ),
                                              m_PendingMaterial );
                else
                    smc.MaterialSlots.clear();
            }
            else
            {
                smc.MaterialSlots.clear();
            }
            FitTarget( center, extent );
        }
        else
        {
            // Material preview. Geometry is built once and reused; clearing the runtime instances forces a
            // rebuild against the current material handle. Foliage/cutout materials (a grass-card atlas) wrap
            // and garble on a sphere, so those preview on a flat PLANE turned to face the fixed camera.
            smc.MeshHandle    = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            const bool flat   = m_PendingPreview == ThumbnailSubject::Preview::Card;
            smc.Primitive     = flat ? Geometry::PrimitiveType::Plane : Geometry::PrimitiveType::Sphere;
            smc.MaterialSlots = { m_PendingHandle };
            smc.RuntimeMaterialInstances.clear();
            smc.RuntimeMesh.reset(); // drop any previously-built primitive so the type change rebuilds

            // MEASURED from the very mesh MeshECSSystem will draw for this component (the process-wide
            // shared primitive), never assumed. The assumption this replaces — the literal `worldSize = 1.0`
            // passed to FitTarget — was minted when primitives were authored at one unit, and survived the
            // centimetre migration that scales every primitive by Common::Units::UnitsPerMetre: the sphere
            // is 100 units across, so the old rule scaled it by 4 instead of 0.04 and drew it 100x too big.
            // Combined with the second half of Д30 (the subject was also left at the world origin, which
            // the migrated camera no longer looks at), the capture came out as the flank of a 400-unit
            // sphere the camera was practically resting on: a wall of albedo under sky, with no sphere in
            // it. See ThumbnailFraming for the framing rule and the measured geometry.
            glm::vec3 matCenter( 0.0f );
            float     matExtent = 1.0f;
            if ( auto* prim = Geometry::PrimitiveMeshFactory::GetShared( *smc.Primitive ) )
            {
                if ( const auto frame = ThumbnailFraming::MeasureSubmeshes( prim->GetSubmeshes() ); frame.Valid )
                {
                    matCenter = frame.Center;
                    matExtent = frame.Extent;
                }
            }
            FitTarget( matCenter, matExtent );

            if ( flat )
            {
                // Turn the card to face the camera. Through ThumbnailFraming::FacingYaw, which takes BOTH
                // points as arguments — the card is no longer at the world origin, and the eye is no longer
                // a constant anyone may write down here.
                auto& tc = m_Target.GetComponent<ECS::TransformComponent>();
                if ( auto cam = m_Scene->GetMainCamera().lock() )
                    tc.Rotation = glm::vec3(
                         0.0f, ThumbnailFraming::FacingYaw( cam->GetPosition(), tc.Translation ), 0.0f );
            }
        }
    }

    bool AssetThumbnailRenderer::DomeIsStillSettling()
    {
        if ( m_PendingSubject != Subject::Material || m_PendingPreview != ThumbnailSubject::Preview::SkyDome )
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

        if ( m_DomeSettle <= 0 )
            return false;

        --m_DomeSettle;
        return true;
    }

    void AssetThumbnailRenderer::Tick()
    {
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
        StageSubject();

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
        const auto captureBegan = std::chrono::steady_clock::now();
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        const auto idled = std::chrono::steady_clock::now();

        std::chrono::steady_clock::time_point read    = idled;
        std::chrono::steady_clock::time_point boxed   = idled;
        std::chrono::steady_clock::time_point written = idled;

        if ( auto finalImage = m_Scene->GetFinalImage() )
        {
            // THE SIZE MISMATCH BELOW USED TO BE THE ONLY REPORT, and it was a report about the wrong
            // thing: a refused readback also came back as an empty vector, so "the staging buffer could
            // not be allocated" was printed as "returned 0 bytes, expected 4194304". The readback now
            // says why, and the size check keeps its own, separate meaning.
            const auto readback = finalImage->ReadPixelsRGBA8();
            read                = std::chrono::steady_clock::now();
            if ( !readback.IsSuccess() )
            {
                LOG_ERROR( "[AssetThumbnailRenderer] readback for '{}' was refused: {} — no thumbnail "
                           "written.",
                           m_PendingPng, readback.GetError() );
            }
            else if ( const std::vector<uint8_t>& src = readback.GetValue();
                      src.size() == static_cast<size_t>( kRenderSize ) * kRenderSize * 4 )
            {
                // Supersample downscale kRenderSize -> kSize via NxN box filter (clean anti-aliased edges).
                // Supersample downscale kRenderSize -> kSize (NxN box filter). The framebuffer readback is
                // already upright, so write rows in order and do NOT stbi-flip (a single flip — which used to
                // be on — made asymmetric meshes like the statue come out head-down; symmetric previews hid it).
                constexpr uint32_t   F = kRenderSize / kSize;
                std::vector<uint8_t> out( static_cast<size_t>( kSize ) * kSize * 4 );
                for ( uint32_t y = 0; y < kSize; ++y )
                    for ( uint32_t x = 0; x < kSize; ++x )
                        for ( uint32_t c = 0; c < 4; ++c )
                        {
                            uint32_t sum = 0;
                            for ( uint32_t sy = 0; sy < F; ++sy )
                                for ( uint32_t sx = 0; sx < F; ++sx )
                                    sum += src[( ( ( y * F + sy ) * kRenderSize + ( x * F + sx ) ) * 4 ) + c];
                            out[( ( y * kSize + x ) * 4 ) + c] = static_cast<uint8_t>( sum / ( F * F ) );
                        }

                boxed = std::chrono::steady_clock::now();

                std::error_code ec;
                std::filesystem::create_directories( std::filesystem::path( m_PendingPng ).parent_path(), ec );
                stbi_flip_vertically_on_write( 0 ); // readback is already upright — no flip

                // WRITE ASIDE, THEN RENAME. stbi_write_png streams straight into the destination, so an
                // editor that dies mid-write leaves a truncated PNG at the cache path — a file that is
                // "fresh" by modification time and cannot be decoded, which is precisely the state the
                // freshness rule cannot repair (it would say "show it" forever). A rename is atomic on
                // every filesystem this runs on, so the destination only ever holds a complete image.
                const std::string temp = m_PendingPng + ".part";
                if ( stbi_write_png( temp.c_str(), kSize, kSize, 4, out.data(), kSize * 4 ) )
                {
                    std::filesystem::rename( temp, m_PendingPng, ec );
                    if ( ec )
                    {
                        LOG_ERROR( "[AssetThumbnailRenderer] '{}' was rendered but could not be moved into "
                                   "place from '{}': {}. No thumbnail was written.",
                                   m_PendingPng, temp, ec.message() );
                        std::error_code cleanupEc;
                        std::filesystem::remove( temp, cleanupEc );
                    }
                }
                else
                {
                    LOG_ERROR( "[AssetThumbnailRenderer] stbi_write_png refused to write '{}' ({}x{} RGBA8). "
                               "No thumbnail was written.",
                               temp, kSize, kSize );
                }
                written = std::chrono::steady_clock::now();
            }
            else
            {
                // A readback whose size does not match the framebuffer we asked for is the one way this
                // path can silently produce nothing, and it used to do so in complete silence: no PNG, no
                // line, and the service one level up reporting "no preview produced" with no reason.
                LOG_ERROR( "[AssetThumbnailRenderer] readback for '{}' returned {} bytes, expected {} "
                           "({}x{} RGBA8) — no thumbnail written.",
                           m_PendingPng, src.size(), static_cast<size_t>( kRenderSize ) * kRenderSize * 4,
                           kRenderSize, kRenderSize );
            }
        }
        else
        {
            LOG_ERROR( "[AssetThumbnailRenderer] '{}' has no final image after {} warm-up frames — no "
                       "thumbnail written.",
                       m_PendingPng, kRenderFrames );
        }

        // THE COST OF ONE CAPTURE, split, because the four parts are not comparable and the budget
        // question ("can the whole project be pre-rendered?") is entirely decided by which of them
        // dominates. Debug level: one line per asset is right for a log file and wrong for the console.
        const auto ms = []( auto from, auto to )
        { return std::chrono::duration<double, std::milli>( to - from ).count(); };
        LOG_DEBUG( "[Thumbnails] captured '{}' in {:.0f} ms (device idle {:.0f}, readback {:.0f}, "
                   "downscale {:.0f}, png {:.0f}) at {}px from a {}px render",
                   std::filesystem::path( m_PendingPng ).filename().string(), ms( captureBegan, written ),
                   ms( captureBegan, idled ), ms( idled, read ), ms( read, boxed ), ms( boxed, written ), kSize,
                   kRenderSize );

        // DEBUG: when true, never finish — keep re-rendering THIS thumbnail every frame so its passes
        // (SkyboxPass etc.) appear in every editor frame and can be grabbed with RenderDoc (F12). The
        // preview render is recorded into the editor's in-flight frame, so the capture includes it.
        // TEMPORARY — set back to false (or delete) after the capture.
        static constexpr bool kDebugLoopForCapture = false;
        m_Phase = kDebugLoopForCapture ? kRenderFrames : 0;
    }
} // namespace Desert::Editor
