#include "PreviewViewport.hpp"

#include <Editor/RenderSystems/Passes/EditorCubemapPreviewPass.hpp>
#include <Editor/RenderSystems/Passes/EditorGridPass.hpp>

#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>

#include "UIHelper/ImGuiUI.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/Graphic/SkyPresets.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Desert::Editor
{
    // A Desert::ImGui also exists (the engine's own UI namespace), so unqualified ImGui:: would resolve
    // there in this TU. Same alias the other editor panels use.
    namespace ImGui = ::ImGui;

    namespace
    {
        constexpr float kFov        = 35.0f; // degrees; a longer lens than the viewport's = less distortion
        constexpr float kNearPlane  = 1.0f;  // centimetres (see Common/Core/Units.hpp)
        constexpr float kFarPlane   = 100000.0f;
        constexpr float kPitchLimit = 1.45f; // just shy of straight down/up, so the orbit never gimbals
        constexpr float kFitMargin  = 1.05f; // a little air around the fitted sphere

        // ── The sky dome (Fill::SkyDome) ──────────────────────────────────────────────────────────────
        //
        // A WIDE VERTICAL LENS IS THE WHOLE POINT of the dome and not a taste: the horizon is the most
        // forgiving angle in the sky (a grazing ray crosses dozens of weather cells) and this engine has
        // twice shipped a sky whose failure was invisible from it — an empty zenith above ~20 degrees and
        // vertical streaking at mid elevation. 96 degrees of vertical field puts the horizon, the mid
        // angle and very nearly the zenith in ONE frame, so a person who never touches the camera still
        // sees all three.
        constexpr float kDomeFov       = 96.0f;
        constexpr float kDomeNearPlane = 10.0f;                             // 10 cm
        constexpr float kDomeFarPlane  = Common::Units::Metres( 60000.0f ); // 60 km — the layer's own reach
        // EYE HEIGHT, AND IT IS NOT 1.7 m, WHICH IS WHAT IT WAS FIRST BUILT AS. Standing on the ground
        // was the obvious choice and it made the ground useless: the cloud shadow map is 30 km across at
        // 117 m per texel (VolumetricCloudRenderer's own log line), and from head height the whole visible
        // near ground fits inside a fraction of ONE of those texels, so it renders perfectly uniform. The
        // deck's shadow is the entire reason there is ground in this frame, so the observer stands on a
        // rise instead — at 150 m the ground band spans roughly 0.5 to 10 km, which is tens of texels and
        // the 1-3 km shadow features are readable in it.
        constexpr float kDomeEyeHeight = Common::Units::Metres( 150.0f );
        // The default elevation: the frame then spans -18 to +78 degrees, so a fifth of it is ground and
        // the rest climbs to just under the zenith.
        constexpr float kDomeDefaultPitch = 0.5236f; // 30 degrees, radians
        constexpr float kDomeDefaultYaw   = -0.6f;
        // THE GRADE A CLOUD SKY IS LOOKED AT THROUGH IN THIS REPOSITORY. Counted rather than chosen: of the
        // 51 scenes under Resources/Assets/Scenes carrying a VolumetricCloud component, 50 author
        // Exposure 0.26 and one (Clouds_Sunset) authors 1.0. The dome takes the modal value so that the
        // material is tuned at the exposure it will be shipped at; the row on the Preview Scene tab is
        // there for the level that disagrees.
        constexpr float kDomeExposure = 0.26f;
        // The ground, as a Plane primitive scaled until its edge is past anything the eye reads as a
        // distance. 20 km at 1.7 m of eye height is well beyond where the atmosphere takes over.
        constexpr float kDomeGroundSize = Common::Units::Metres( 20000.0f );

        // A primitive's true half-SIZE per axis. Primitives are generated one metre = 100 units across
        // (PrimitiveMeshFactory::kPrimitiveSize), so this is 50 on each axis a shape actually occupies.
        glm::vec3 HalfExtentOfPrimitive( PreviewViewport::Shape shape )
        {
            constexpr float kHalf = 50.0f;
            switch ( shape )
            {
                case PreviewViewport::Shape::Plane:
                    // A CARD, THIN ON Z. This read `{ kHalf, 0.5f, kHalf }` — thin on Y — which describes
                    // a floor tile, and the Plane primitive is not one: PrimitiveMeshFactory::CreatePlane
                    // builds a unit quad in the XY plane with normal +Z, deliberately, so a foliage or
                    // decal material faces the camera. The corner fit reads this half-extent, so the card
                    // was being framed as if it were lying flat: at the default orbit that is a 100-unit
                    // body seen edge-on where a 1-unit one was described, and the camera sat closer than
                    // the shape's own diagonal. Nothing crashed and nothing warned; the card was simply
                    // framed by the wrong solid, which is why it survived.
                    return glm::vec3( kHalf, kHalf, 0.5f );
                case PreviewViewport::Shape::Cylinder:
                case PreviewViewport::Shape::Cube:
                case PreviewViewport::Shape::Sphere:
                default:
                    return glm::vec3( kHalf );
            }
        }

        // Its bounding-SPHERE radius: the distance from the centre to the furthest point ON THE SHAPE. A
        // sphere's is its own radius; a box's is its half-diagonal. Kept separate from the half-extent on
        // purpose — feeding a radius in where a per-axis half-size belongs describes a body 1.73x too big,
        // which is exactly what put a 100-unit primitive 517 units from the camera.
        float RadiusOfPrimitive( PreviewViewport::Shape shape )
        {
            constexpr float kHalf = 50.0f;
            switch ( shape )
            {
                case PreviewViewport::Shape::Cube:
                    return kHalf * 1.732f; // corner-to-centre
                case PreviewViewport::Shape::Plane:
                    return kHalf * 1.415f;
                case PreviewViewport::Shape::Cylinder:
                    // Round about Y and flat-capped: the furthest point is a rim corner, at
                    // sqrt(r^2 + h^2) from the centre with r == h == kHalf.
                    return kHalf * 1.415f;
                case PreviewViewport::Shape::Sphere:
                default:
                    return kHalf; // a sphere IS its radius — its box would be 1.73x too far
            }
        }

        // For measured geometry there is no shape to know, so the bound is the box's half-diagonal.
        float RadiusOfHalfExtent( const glm::vec3& halfExtent )
        {
            return glm::length( halfExtent );
        }

        Geometry::PrimitiveType ToPrimitive( PreviewViewport::Shape shape )
        {
            switch ( shape )
            {
                case PreviewViewport::Shape::Cube:
                    return Geometry::PrimitiveType::Cube;
                case PreviewViewport::Shape::Plane:
                    return Geometry::PrimitiveType::Plane;
                case PreviewViewport::Shape::Cylinder:
                    return Geometry::PrimitiveType::Cylinder;
                case PreviewViewport::Shape::Sphere:
                default:
                    return Geometry::PrimitiveType::Sphere;
            }
        }
    } // namespace

    glm::vec3 PreviewViewport::SceneSetup::LightTravel() const
    {
        // Yaw is a compass bearing about +Y, pitch the sun's ELEVATION above the horizon; this is the
        // direction TOWARD the sun. What a directional light's Translation stores is where the light
        // GOES, so the vector is negated exactly once, here — the engine's own rule
        // (ECS::Rules::AtmosphereSunDirection) negates back to get the sun again, and two negations in
        // two places is how a sky ends up lit from below.
        const float     yaw   = glm::radians( SunYawDegrees );
        const float     pitch = glm::radians( SunPitchDegrees );
        const float     cp    = std::cos( pitch );
        const glm::vec3 towardSun{ cp * std::sin( yaw ), std::sin( pitch ), cp * std::cos( yaw ) };

        // Scaled rather than unit: nothing downstream needs a length, but a Translation that reads as a
        // position in the outliner should not sit inside the object it lights.
        return -towardSun * 600.0f;
    }

    PreviewViewport::SceneSetup& PreviewViewport::Setup()
    {
        return m_Setup;
    }

    const PreviewViewport::SceneSetup& PreviewViewport::Setup() const
    {
        return m_Setup;
    }

    // Defaulted HERE rather than in the header: see the declaration for why an inline default constructor
    // would drag the complete EditorCubemapPreviewPass type into every panel that creates a preview.
    PreviewViewport::PreviewViewport() = default;

    PreviewViewport::~PreviewViewport()
    {
        if ( !m_Inited )
            return;

        // Closing a scene view (or quitting) can destroy this while the last submitted frame is still
        // executing against our pipelines and descriptor pools. Idle, then release the cubemap pass
        // while the scene it registered with is still alive (its dtor unregisters by name), then the
        // scene before the renderer that owns its passes.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_CubemapPass.reset();
        m_GridPass.reset(); // same rule: it unregisters from the scene by name in its own dtor
        m_Scene.reset();
        m_Renderer.reset();
    }

    void PreviewViewport::EnsureInit( const Graphic::ViewExtent& extent )
    {
        if ( m_Inited )
            return;

        // ONE CASCADE AT 1024 OVER 10 m, and the argument goes to the constructor because the cascade
        // framebuffers are allocated inside Scene::Init() below. Shadows were switched off in this scene
        // outright, and the reason was memory rather than taste: four 2048 cascades are 335 MB of
        // attachments per renderer and this editor allows six live ones. See Graphic::ShadowQuality.
        m_Renderer        = std::make_unique<Graphic::SceneRenderer>( extent, Graphic::kPreviewViewProfile );
        m_Scene    = std::make_shared<::Desert::Core::Scene>( "DetailsPreview", m_Renderer.get() );
        const auto inited = m_Scene->Init();
        if ( !inited.IsSuccess() )
        {
            // `m_Inited` stays false so the next call retries, which is the whole reason this is not a
            // bare `(void)`: with the result dropped the flag was set anyway, the preview was marked
            // ready, and every frame afterwards recorded into a scene that had never initialised.
            LOG_ERROR( "[PreviewViewport] preview scene failed to initialise: {}", inited.GetError() );
            m_Scene.reset();
            m_Renderer.reset();
            return;
        }

        // Clean preview: no bloom to muddy a small image. FXAA keeps the silhouette smooth at inspector
        // sizes (there is no supersampling here — this renders live).
        //
        // SHADOWS ARE NO LONGER OFF, and that line's absence is the point of the change. It used to read
        // `EnableShadows = false` with "the expensive half" beside it, and it was true: on the budget
        // every renderer used, this scene would have allocated 335 MB of cascade attachments to shadow one
        // ball. It now allocates one cascade at 1024 over 10 m (Graphic::kPreviewShadowQuality, passed to
        // the SceneRenderer above), which is 21 MB and is what makes a floor with a shadow under it
        // affordable at all. Whether they are ON is ApplySetup's answer, from the floor's own row.
        //
        // The cloud quality tier drops with them, for the same reason and by the same argument: a
        // 512-pixel pane marching at quarter resolution has no use for the viewport's sample ceiling.
        //
        // AND THE GRID NEEDS NO LINE HERE ANY MORE: debug overlays left SceneSettings for the RENDERER
        // (Graphic::DebugViewState), default to off, and only the main editor loop pushes a user's flags
        // into one. Two tasks met in this block — one turned the shadows on, the other took the overlay
        // out — and both belong: the preview owns its lighting budget, and it no longer owns the overlay.
        auto& settings       = m_Scene->GetSettings();
        settings.EnableBloom = false;

        // The three quality lines that used to stand here are gone: two of them (`AA = FXAA`) merely
        // restated the default, and the third (`CloudQualityTier = Low`) was a real override of a value
        // that has since stopped being scene data. The tier still drops — see PushQuality(), called every
        // frame from Update() rather than once from here, so a machine-quality change reaches this pane
        // in the same frame it reaches the viewport.

        // The selection outline is pushed by the editor loop every frame; this renderer is never fed by it,
        // so disable it explicitly or a stale outline could bleed into the preview.
        m_Renderer->SetOutlineSettings( glm::vec3( 0.0f ), 0.0f, 0.0f, false );

        // OUR OWN camera, unlike AssetThumbnailRenderer: a scene's auto-created main camera is the
        // input-driven EditorCamera (it would fight the real viewport for the mouse and ignores transforms),
        // so the thumbnail renderer has to frame by scaling the object. A GameplayCamera we drive by hand
        // from the orbit state gives real orbit + zoom and leaves the target's transform at identity.
        //
        // PINNED, not merely set active: Scene::OnUpdate re-picks the camera from the play state every
        // frame, so a plain SetActiveCamera survived exactly one frame before the scene's own EditorCamera
        // took the view back — and that camera reads the global mouse, so the preview flew along with the
        // viewport and lost the mesh.
        m_Camera = std::make_shared<::Desert::Core::GameplayCamera>();
        m_Scene->PinActiveCamera( m_Camera );

        m_Light               = m_Scene->CreateNewEntity( "PreviewLight" );
        auto& lightC          = m_Light.AddComponent<ECS::DirectionLightComponent>();
        lightC.Data.Intensity = m_Setup.LightIntensity;
        lightC.Data.Color     = m_Setup.LightColor;
        m_Light.GetComponent<ECS::TransformComponent>().Translation = m_Setup.LightTravel();

        m_Target = m_Scene->CreateNewEntity( "PreviewTarget" );
        m_Target.AddComponent<ECS::StaticMeshComponent>();

        // The floor: a Plane primitive with no material of its own, so it takes the engine's default and
        // stays a neutral surface for a shadow to land on rather than a second thing to look at. It is
        // created switched OFF (no primitive in its mesh component) and ApplySetup turns it on — an entity
        // that exists but draws nothing costs a component walk, while creating and destroying it on every
        // toggle would churn the mesh service for a checkbox.
        m_Floor = m_Scene->CreateNewEntity( "PreviewFloor" );
        m_Floor.AddComponent<ECS::StaticMeshComponent>();

        m_Scene->AddSystem<ECS::MeshECSSystem>();
        m_Scene->AddSystem<ECS::SkyboxECSSystem>();
        // The Volume domain's system. Added unconditionally and costing nothing until an entity carries a
        // VolumetricCloudComponent: with none in the registry it emits one "no clouds present" command a
        // frame, which is what stops a deleted layer leaving its clouds behind.
        m_Scene->AddSystem<ECS::VolumetricCloudECSSystem>();

        // Procedural sky as the backdrop. Unlike the thumbnails (fixed camera looking down, so only the
        // ground hemisphere showed) this camera can point anywhere, so the whole dome is kept cohesive.
        // The palette comes from the SHARED preset table, which exists precisely so this backdrop and the
        // one an artist can pick in Details are the same set of values rather than two copies drifting
        // apart; the default is Studio Neutral, deliberately dark so nothing competes with the asset.
        m_Sky      = m_Scene->CreateNewEntity( "PreviewSky" );
        auto& skyC = m_Sky.AddComponent<ECS::SkyAtmosphereComponent>();
        Graphic::ApplySkyPreset( m_Setup.Sky, skyC.Data );
        skyC.Data.ActivePreset = m_Setup.Sky;
        // A PREVIEW BAKES WHEN IT IS TOLD TO, and never on its own. The automatic rebake exists for a
        // level whose sun crosses the sky over minutes; here the only things that move the sky are on the
        // Preview Scene tab, and ApplySetup already asks for a bake when one of them changes. Leaving it
        // automatic is what let a moving cloud deck re-bake a 485 ms panorama every few frames.
        skyC.Data.AutoRebakeEnvironment = false;
        // 512x256 rather than the 1024x512 default. The panorama's only consumer here is the ambient term
        // on one object or one ground plane in a 512-pixel pane; four times the texels buys nothing it can
        // show, and it is paid per live renderer.
        skyC.Data.EnvironmentResolution = ECS::SkyEnvironmentResolution::Low;
        skyC.RequestBake                = true;

        // Same values through the direct call so the sky is enabled from frame 0 (the ECS command path alone
        // proved insufficient in a minimal scene — see AssetThumbnailRenderer). One packing helper, one
        // negation: the palette is read off the component and the sun comes from the light's travel vector.
        m_Renderer->SetProceduralSky( true, ECS::Rules::AtmosphereSunDirection( m_Setup.LightTravel() ),
                                      /*bakeNow=*/true, Graphic::MakeSkySettings( skyC.Data ),
                                      Graphic::SunLightFx{} );

        m_Inited = true;

        // Everything above wrote the setup's own values, so record that as applied — otherwise the first
        // ApplySetup would see a difference that is not one and ask for a second environment bake on the
        // first frame of every preview that opens.
        m_AppliedSetup      = m_Setup;
        m_AppliedSetupValid = true;
    }

    void PreviewViewport::ApplySetup()
    {
        if ( !m_Inited )
            return;

        const bool changed = !m_AppliedSetupValid || !( m_Setup == m_AppliedSetup );

        // ── The key light, which is also the sun ───────────────────────────────────────────────────────
        auto& lightC          = m_Light.GetComponent<ECS::DirectionLightComponent>();
        lightC.Data.Intensity = m_Setup.LightIntensity;
        lightC.Data.Color     = m_Setup.LightColor;
        m_Light.GetComponent<ECS::TransformComponent>().Translation = m_Setup.LightTravel();

        // ── The environment ───────────────────────────────────────────────────────────────────────────
        //
        // The palette is re-applied from the shared table every frame rather than only on a change. It is
        // thirteen field copies, and it means a preset row is the ONLY thing that can be showing: there is
        // no state here that a switch could fail to overwrite.
        auto& skyC = m_Sky.GetComponent<ECS::SkyAtmosphereComponent>();
        Graphic::ApplySkyPreset( m_Setup.Sky, skyC.Data );
        skyC.Data.ActivePreset = m_Setup.Sky;
        skyC.Data.SkyBrightness *= m_Setup.SkyIntensity;

        // A RE-BAKE, NOT A REDRAW, and only when something moved and nobody is dragging. The sky pass
        // evaluates the palette and the sun every frame, so the dome itself follows a drag with no help;
        // what the bake produces is the IBL pair behind the AMBIENT, and running that per frame while the
        // sun is being swung around would be the most expensive thing in the window by a wide margin.
        if ( changed && !m_DraggingLight )
            skyC.RequestBake = true;

        // ── The floor ─────────────────────────────────────────────────────────────────────────────────
        auto& floorMesh = m_Floor.GetComponent<ECS::StaticMeshComponent>();
        if ( m_Setup.ShowFloor )
        {
            if ( floorMesh.Primitive != Geometry::PrimitiveType::Plane )
            {
                floorMesh.Primitive = Geometry::PrimitiveType::Plane;
                ECS::ClearEditableMesh( floorMesh );
            }
            floorMesh.CastShadows    = m_Setup.FloorCastsShadow;
            floorMesh.ReceiveShadows = m_Setup.FloorReceivesShadow;

            // THE FLOOR'S OWN LOOK, WRITTEN ON ITS RUNTIME INSTANCE — deliberately NOT through
            // ECS::MaterialComponent, which is the shader-OVERRIDE route this widget is forbidden to touch
            // (Desert/Tests/Editor/MaterialPreviewRoute, and it caught the first version of this line). The
            // ban is about the SUBJECT — the override route re-seeds schema defaults, so a material shown
            // through it is not the material the scene draws — but a rule with an exception carved into it
            // for a prop is a rule that stops guarding the thing it was written for. A per-instance write
            // is what an authored slot produces anyway, so the floor needs no exception.
            //
            // Every frame, because MeshECSSystem owns those instances and rebuilds them whenever the slot
            // count changes; three setter calls on one instance is cheaper than watching for that.
            if ( !floorMesh.RuntimeMaterialInstances.empty() && floorMesh.RuntimeMaterialInstances[0] )
            {
                auto& inst = floorMesh.RuntimeMaterialInstances[0];
                inst->SetParamFromVec4( "AlbedoColor", glm::vec4( m_Setup.FloorColour, 1.0f ) );
                // Fully rough and non-metallic: the floor is there to catch a shadow, and a glossy one
                // would throw a sun highlight across the frame that reads as part of the asset.
                inst->SetParamFromVec4( "RoughnessFactor", glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) );
                inst->SetParamFromVec4( "MetallicFactor", glm::vec4( 0.0f ) );
            }

            // The plane primitive is 100 units across, so the authored size in world units is the scale.
            // In dome mode it is not a floor at all but the GROUND, and its size is fixed at a distance
            // the eye reads as a horizon rather than left to a control that means nothing there.
            const float size = ( m_Fill == Fill::SkyDome ) ? kDomeGroundSize : m_Setup.FloorSize;
            auto&       tc   = m_Floor.GetComponent<ECS::TransformComponent>();
            tc.Scale         = glm::vec3( size / 100.0f );
            // LAID DOWN. The Plane primitive is a vertical CARD (normal +Z), which is what a foliage
            // material wants and is not what a floor is; without this quarter turn the "floor" was a
            // 20 km wall standing on edge, and the ground in the first dome frame was the procedural
            // sky's own ground hemisphere rather than anything this scene drew. Radians, negative, so
            // the card's +Z normal ends up pointing at +Y.
            tc.Rotation = glm::vec3( -glm::half_pi<float>(), 0.0f, 0.0f );
            // Just below the subject rather than at its centre: the primitives are generated about the
            // origin, so a floor at y=0 would cut every one of them in half.
            tc.Translation = glm::vec3( 0.0f, ( m_Fill == Fill::SkyDome ) ? 0.0f : -50.0f, 0.0f );
        }
        else if ( floorMesh.Primitive.has_value() )
        {
            floorMesh.Primitive.reset();
            ECS::ClearEditableMesh( floorMesh );
            floorMesh.RuntimeMaterialInstances.clear();
        }

        // ── What the scene renderer is asked for ──────────────────────────────────────────────────────
        //
        // SHADOWS FOLLOW THE FLOOR, because the floor is the only thing in this scene that can receive
        // one. Switching them on with no floor present would allocate nothing extra (the cascades exist
        // from Init either way) but would spend a cascade pass per frame drawing a shadow onto the sky.
        //
        // AND THE DOME HAS NONE, which is not the same statement. The dome's ground DOES receive a shadow
        // — the deck's — but that comes from the cloud shadow map, a separate output of the cloud renderer
        // and not from a cascade. There is no object in a dome to cast a cascade shadow, and the preview
        // budget's cascade reaches 10 m into a scene whose nearest ground is several hundred metres away:
        // it would render an empty depth map every frame and every fragment would fall outside it.
        auto& settings         = m_Scene->GetSettings();
        settings.EnableShadows = m_Fill != Fill::SkyDome && m_Setup.ShowFloor && m_Setup.FloorReceivesShadow;
        // THE GRADE, EVERY FRAME AND FROM THE SETUP, so the pane follows the row instead of holding
        // whatever SceneSettings was constructed with. See SceneSetup::Exposure for the census behind the
        // dome's value: a cloud material used to be authored four stops off every level that ships it.
        settings.Exposure = m_Setup.Exposure;
        // THE GRID IS NOT A SCENE SETTING ANY MORE. К2 moved every debug overlay onto the RENDERER
        // (Graphic::DebugViewState), because a view preference in a level file is one person's opinion
        // travelling through git — and six of those flags reached the Runtime, one of them forcing the
        // forward render path. The preview owns its own view, so it pushes its own state rather than
        // inheriting the editor's: a floor grid under a material ball is the preview's business, not the
        // level's.
        Graphic::DebugViewState debugView;
        debugView.ShowGrid = m_Setup.ShowGrid;
        m_Renderer->SetDebugView( debugView );

        // THE FLAG NEEDS A READER, and in this scene there was none. SceneSettings::ShowGrid is consumed
        // by EditorGridPass, which the editor installs on the MAIN scene through its RenderRegistry and
        // has never installed on a preview scene — so a Show Grid row here would have been a checkbox
        // wired to nothing, which is the dead setting the delivery contract returns work for. Installed on
        // first use, for the reason given on the member.
        if ( m_Setup.ShowGrid && !m_GridPass )
        {
            auto grid = std::make_unique<Render::EditorGridPass>();
            if ( const auto result = grid->Install( m_Scene ); !result )
            {
                // Named once and the flag put back, so the checkbox does not sit ticked over a scene with
                // no grid in it — a silent fallback is exactly what this branch exists to avoid.
                LOG_ERROR( "[Preview] the grid pass could not be installed, so the preview has no grid: {}",
                           result.GetError() );
                m_Setup.ShowGrid   = false;
                debugView.ShowGrid = false;
                m_Renderer->SetDebugView( debugView );
            }
            else
            {
                m_GridPass = std::move( grid );
            }
        }

        // ── The cloud layer, when there is one ────────────────────────────────────────────────────────
        if ( m_CloudLayer && m_CloudLayer.HasComponent<ECS::VolumetricCloudComponent>() )
        {
            auto& cloud                  = m_CloudLayer.GetComponent<ECS::VolumetricCloudComponent>();
            cloud.Data.MaxSteps          = m_Setup.CloudMaxSteps;
            cloud.Data.StopTransmittance = m_Setup.CloudStopTransmittance;
            cloud.Data.VolumeResolution  = m_Setup.CloudVolumeResolution;
        }

        m_AppliedSetup      = m_Setup;
        m_AppliedSetupValid = true;
    }

    void PreviewViewport::SetForcedLOD( const int lod )
    {
        if ( !m_Target || !m_Target.HasComponent<ECS::StaticMeshComponent>() )
            return;
        m_Target.GetComponent<ECS::StaticMeshComponent>().ForcedLOD = lod;
    }

    void PreviewViewport::SetMesh( const Assets::AssetHandle&              mesh,
                                   const std::vector<Assets::AssetHandle>& materials )
    {
        if ( static_cast<uint64_t>( mesh ) == 0 )
        {
            Clear();
            return;
        }

        EnsureInit();

        auto& smc = m_Target.GetComponent<ECS::StaticMeshComponent>();
        ECS::ClearEditableMesh( smc );
        smc.Primitive.reset();
        smc.RuntimeMaterialInstances.clear();
        smc.MeshHandle    = mesh;
        smc.MaterialSlots = materials;
        smc.ForcedLOD     = -1;

        // Upright, like the material preview sets it: the target entity is reused across previews, so a
        // rotation left by an earlier one would tilt this mesh for no reason.
        m_Target.GetComponent<ECS::TransformComponent>().Rotation = glm::vec3( 0.0f );

        // One kind of content at a time (same line as in SetMaterial).
        if ( m_CubemapPass )
            m_CubemapPass->ClearSource();

        m_MeshHandle  = mesh;
        m_Fill            = Fill::Object;
        m_HasContent  = true;
        m_Focus       = glm::vec3( 0.0f );
        m_FrameHalfExtent = glm::vec3( 50.0f ); // stand-in until the bounds are known (see TryFrameMesh)
        m_FrameRadius     = RadiusOfHalfExtent( m_FrameHalfExtent );
        ResetView();
        m_Framed = TryFrameMesh();
    }

    bool PreviewViewport::HasContent() const
    {
        // See the header for why this is a question about the RUNTIME mesh and not about m_Framed.
        if ( !m_HasContent )
            return false;

        // Materials and primitives hand over something drawable at assignment time; there is nothing here
        // to look up for them, and no mesh handle to look it up with.
        if ( static_cast<uint64_t>( m_MeshHandle ) == 0 )
            return true;

        // Get() builds on a miss from the registered shell, so this both answers the question and is the
        // lazy path that makes the answer become true once the mesh is available. Submeshes, not the
        // pointer: a Mesh that exists with none of them draws exactly nothing, which is the state the
        // Details row must fall back to its thumbnail for.
        const auto* runtime = Runtime::ResourceRegistry::GetMeshService()->Get( m_MeshHandle );
        return runtime && !runtime->GetSubmeshes().empty();
    }

    bool PreviewViewport::TryFrameMesh()
    {
        // Bounds from the ASSET'S OWN VERTICES when they are there, and only then from the submesh AABBs.
        // A stored AABB is whatever the importer wrote: it can be stale, empty, or in raw-vertex space that
        // does not match what is drawn (Scene.cpp works around the same thing for picking). Framing off a
        // wrong box is exactly how a preview ends up with the camera inside the model or the model in a
        // corner — the vertices cannot lie.
        glm::vec3 mn( 1e9f );
        glm::vec3 mx( -1e9f );
        bool      haveBounds   = false;
        bool      usedVertices = false;

        if ( const auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( m_MeshHandle ) )
        {
            if ( const auto* staticAsset = dynamic_cast<const Assets::StaticMeshAsset*>( asset ) )
            {
                for ( const auto& v : staticAsset->GetVertices() )
                {
                    mn           = glm::min( mn, v.Position );
                    mx           = glm::max( mx, v.Position );
                    haveBounds   = true;
                    usedVertices = true;
                }
            }
        }

        if ( !haveBounds )
        {
            auto* meshAsset = Runtime::ResourceRegistry::GetMeshService()->Get( m_MeshHandle );
            if ( !meshAsset )
                return false;

            // Union the submesh AABBs in MESH space, applying each submesh transform to its 8 corners —
            // meshes that keep their offset in a submesh transform frame wrongly otherwise.
            for ( const auto& sm : meshAsset->GetSubmeshes() )
            {
                const glm::vec3 lo = sm.BoundingBox.Min, hi = sm.BoundingBox.Max;
                for ( int corner = 0; corner < 8; ++corner )
                {
                    const glm::vec3 p( ( corner & 1 ) ? hi.x : lo.x, ( corner & 2 ) ? hi.y : lo.y,
                                       ( corner & 4 ) ? hi.z : lo.z );
                    const glm::vec3 w = glm::vec3( sm.Transform * glm::vec4( p, 1.0f ) );
                    mn                = glm::min( mn, w );
                    mx                = glm::max( mx, w );
                    haveBounds        = true;
                }
            }
        }

        const glm::vec3 extent = mx - mn;

        // Say what was measured, ONCE per mesh. Three attempts at this framing were made blind because the
        // only symptom available was "it looks wrong"; the numbers that decide the camera — where they came
        // from, and what they are — belong in the log where they can be read.
        static Assets::AssetHandle s_ReportedFor;
        if ( s_ReportedFor != m_MeshHandle )
        {
            s_ReportedFor = m_MeshHandle;
            if ( haveBounds )
            {
                LOG_INFO( "[Preview] mesh {} framed from {}: extent {:.1f} x {:.1f} x {:.1f}, centre "
                          "({:.1f}, {:.1f}, {:.1f})",
                          static_cast<uint64_t>( m_MeshHandle ), usedVertices ? "vertices" : "submesh AABBs",
                          extent.x, extent.y, extent.z, ( mn.x + mx.x ) * 0.5f, ( mn.y + mx.y ) * 0.5f,
                          ( mn.z + mx.z ) * 0.5f );
            }
            else
            {
                LOG_WARN( "[Preview] mesh {} has nothing to measure yet (no CPU vertices, no submesh "
                          "bounds) — the preview keeps its stand-in framing and retries",
                          static_cast<uint64_t>( m_MeshHandle ) );
            }
        }

        if ( !haveBounds || extent.x < 0.0f || glm::length( extent ) < 1e-4f )
            return false; // nothing measurable yet — keep the stand-in and retry next frame

        m_Focus = ( mn + mx ) * 0.5f;

        // Half the LARGEST EXTENT, the same convention RadiusOfPrimitive uses for the material preview
        // (a 100-unit cube gives 50, not the 87 a diagonal would).
        // Half-size per axis is the measurement; the radius is derived from it, never the other way round.
        m_FrameHalfExtent = glm::max( extent * 0.5f, glm::vec3( 0.01f ) );
        m_FrameRadius     = std::max( RadiusOfHalfExtent( m_FrameHalfExtent ), 1.0f );
        m_FrameIsRound    = false; // a measured mesh is fitted by its box, whatever it looks like

        ResetView();
        return true;
    }

    void PreviewViewport::SetMaterial( const Assets::AssetHandle& material, Shape shape )
    {
        EnsureInit();

        auto& smc      = m_Target.GetComponent<ECS::StaticMeshComponent>();
        smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        // Clearing the runtime instances forces a rebuild against the new handle; dropping the runtime mesh
        // makes a shape change rebuild the geometry.
        smc.RuntimeMaterialInstances.clear();
        ECS::ClearEditableMesh( smc );
        smc.Primitive     = ToPrimitive( shape );
        smc.MaterialSlots = { material };

        // A flat card must face the camera to be readable (a grass atlas garbles on a sphere), so the plane
        // preview keeps a fixed front-on view instead of an orbit start angle.
        auto& tc    = m_Target.GetComponent<ECS::TransformComponent>();
        tc.Rotation = glm::vec3( 0.0f );

        m_MeshHandle  = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        m_Framed      = true; // a primitive's size is known up front
        m_Focus       = glm::vec3( 0.0f );
        m_FrameHalfExtent = HalfExtentOfPrimitive( shape );
        m_FrameRadius     = RadiusOfPrimitive( shape );
        m_FrameIsRound    = ( shape == Shape::Sphere );
        m_Fill            = Fill::Object;
        m_HasContent  = true;

        // One kind of content at a time: a window whose material moved from the cubemap domain to the
        // surface one must not keep the ball behind its new primitive.
        if ( m_CubemapPass )
            m_CubemapPass->ClearSource();

        ResetView();
    }

    void PreviewViewport::SetCubemapMaterial( std::function<Graphic::SampledCube()> resolveCube )
    {
        EnsureInit();

        // Nothing rides the mesh path in this mode — the ball is the external pass's draw (see the
        // header for why it is not a primitive with a scratch material).
        auto& smc      = m_Target.GetComponent<ECS::StaticMeshComponent>();
        smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        smc.Primitive.reset();
        smc.MaterialSlots.clear();
        smc.RuntimeMaterialInstances.clear();
        ECS::ClearEditableMesh( smc );

        if ( !m_CubemapPass )
        {
            auto pass = std::make_unique<Render::EditorCubemapPreviewPass>();
            if ( const auto result = pass->Install( m_Scene ); !result )
            {
                // Named, once: an empty pane with no message is the silent fallback the contract
                // forbids, and the panel above will keep showing "Starting the preview...".
                LOG_ERROR( "[Preview] cubemap pass unavailable: {}", result.GetError() );
                return;
            }
            m_CubemapPass = std::move( pass );
        }

        // Same size as the sphere primitive, so the two domains' balls frame identically and the
        // orbit/zoom muscle memory carries over.
        constexpr float kBallRadius = 50.0f;
        m_CubemapPass->SetSource( std::move( resolveCube ), kBallRadius );

        // No floor under a cubemap ball: the ball IS an environment, and a floor under it would be a
        // surface lit by the very map the pane is showing, standing in front of it.
        m_Setup.ShowFloor = false;

        m_MeshHandle      = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        m_Framed          = true;
        m_Focus           = glm::vec3( 0.0f );
        m_FrameHalfExtent = glm::vec3( kBallRadius );
        m_FrameRadius     = kBallRadius;
        m_FrameIsRound    = true;
        m_Fill            = Fill::Cubemap;
        m_HasContent      = true;
        ResetView();
    }

    void PreviewViewport::SetCubemapBackdrop( bool cubeIsBackdrop )
    {
        if ( m_Fill != Fill::Cubemap || !m_CubemapPass )
        {
            LOG_ERROR( "[Preview] SetCubemapBackdrop without a cubemap on show — call SetCubemapMaterial first." );
            return;
        }
        m_CubemapPass->SetBackdrop( cubeIsBackdrop );
    }

    void PreviewViewport::SetVolumeMaterial( const Assets::AssetHandle& material )
    {
        EnsureInit();
        if ( !m_Inited )
            return;

        // Nothing rides the mesh path: a medium has no surface to put on a primitive (see the header).
        auto& smc      = m_Target.GetComponent<ECS::StaticMeshComponent>();
        smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        smc.Primitive.reset();
        smc.MaterialSlots.clear();
        smc.RuntimeMaterialInstances.clear();
        ECS::ClearEditableMesh( smc );

        if ( m_CubemapPass )
            m_CubemapPass->ClearSource();

        // CREATED ON DEMAND, not with the scene. The layer brings a modelling volume of 8 MiB and a
        // sky-occlusion volume of 2 MiB with it, and the FIRST bake of the modelling volume blocks the
        // frame — so a preview of a wood material must not be paying for one by existing.
        if ( !m_CloudLayer )
        {
            m_CloudLayer = m_Scene->CreateNewEntity( "PreviewCloudLayer" );
            m_CloudLayer.AddComponent<ECS::VolumetricCloudComponent>();
        }

        auto& cloud        = m_CloudLayer.GetComponent<ECS::VolumetricCloudComponent>();
        cloud.Data.Enabled = true;

        // A STILL SKY, and this is not a preference — it is the difference between a 10 ms frame and a
        // 106 ms one. Measured on this machine, Debug, with a cloud material document open and the wind at
        // its 30 m/s default: `Clouds: BuildEnvironmentBake` 90.5 ms of a 105.8 ms frame, 85.6 % of it, the
        // editor at 9 FPS. The mechanism is not the march — the march is quarter-resolution of a 512-pixel
        // pane and costs almost nothing — it is the IBL panorama: SkyboxRenderer re-bakes the environment
        // whenever the cloud FINGERPRINT changes, and a moving deck changes it every frame, so a 485 ms
        // bake was running continuously. Wind also shows nothing in a still frame, so the animation was
        // paying that for a picture nobody could see.
        cloud.Data.WindSpeed = 0.0f;
        // THE HANDLE, resolved by the layer every frame through MaterialService — which is what makes an
        // edit in the parameter table show here without an Apply, exactly as the ball does for a surface.
        cloud.Data.Material          = material;
        cloud.Data.MaxSteps          = m_Setup.CloudMaxSteps;
        cloud.Data.StopTransmittance = m_Setup.CloudStopTransmittance;
        cloud.Data.VolumeResolution  = m_Setup.CloudVolumeResolution;

        // The ground comes on with the dome and is not a preference. The deck's shadow and the light it
        // throws down are part of what a cloud material looks like; a dome over nothing would be showing
        // half of the thing being edited, and this engine has already shipped a defect (Р21) where that
        // shadow reached one lit path and not the other two.
        m_Setup.ShowFloor           = true;
        m_Setup.FloorReceivesShadow = true;
        m_Setup.FloorCastsShadow    = false;
        // A dark studio dome over a cloudscape reads as night. The clouds are the subject here, so the
        // backdrop stops being deliberately uninteresting and becomes the sky they live in.
        if ( m_Setup.Sky == ECS::SkyPreset::StudioNeutral )
        {
            m_Setup.Sky = ECS::SkyPreset::ClearNoon;
            // AN OUTDOOR SUN, NOT A STUDIO KEY, and this is what decides whether the deck's shadow is
            // visible at all. The cloud shadow removes only the DIRECT term; at the asset preview's 3.5
            // the sky's diffuse contribution dominates a lit ground, so a shadow that removes all of the
            // sun still moves the pixel very little and the frame reads as a uniform slab. 22 is the value
            // the engine's own outdoor reference scene authors for this sun
            // (Resources/Assets/Scenes/Clouds_ShadowsOnGround.desce), taken rather than derived — the sky's
            // SunIntensity is a radiance and this is an illuminance, and Components.hpp is explicit that
            // the two are different quantities that must not be computed from one another.
            m_Setup.LightIntensity = 22.0f;
            // AND THE GRADE THAT SUN IS SEEN THROUGH, on the same terms and from the same file: 0.26 is
            // what fifty of the fifty-one cloud scenes in this repository author, Clouds_ShadowsOnGround
            // among them. The pane's own default of 1.0 is Core::SceneSettings' struct default and was
            // never a decision; leaving it there made the preview 78 of 255 brighter on average than any
            // level that would ship the material. See SceneSetup::Exposure.
            m_Setup.Exposure = kDomeExposure;
        }

        m_Fill       = Fill::SkyDome;
        m_MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        m_Framed     = true; // there is nothing to frame — the camera stands still and looks around
        m_HasContent = true;
        ResetView();
    }

    void PreviewViewport::InvalidatePipelines( const void* shader )
    {
        if ( !m_Inited || !m_Renderer || !shader )
            return;

        // Idle first: a pipeline this cache is about to drop may still be executing in a submitted frame.
        // The main scene's hot-reload path does the same before its own InvalidateByShader.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_Renderer->GetPipelineCache().InvalidateByShader( shader );
    }

    void PreviewViewport::Clear()
    {
        m_HasContent = false;
        m_MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        m_Framed     = false;
        m_Fill       = Fill::Empty;
        if ( !m_Inited )
            return;

        auto& smc      = m_Target.GetComponent<ECS::StaticMeshComponent>();
        smc.MeshHandle = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        smc.Primitive.reset();
        smc.MaterialSlots.clear();
        smc.RuntimeMaterialInstances.clear();
        ECS::ClearEditableMesh( smc );

        if ( m_CubemapPass )
            m_CubemapPass->ClearSource();
    }

    void PreviewViewport::SetOrbit( float yawRadians, float pitchRadians )
    {
        m_Yaw   = yawRadians;
        m_Pitch = std::clamp( pitchRadians, -kPitchLimit, kPitchLimit );
    }

    void PreviewViewport::ResetView()
    {
        // THE DOME DOES NOT ORBIT AND HAS NOTHING TO FIT. The camera stands on the ground and turns; yaw
        // and pitch are the direction it LOOKS, not a position around a subject, and there is no distance
        // to solve for. Returning here rather than letting the fit run on a stand-in radius is what stops
        // "reset view" quietly putting the observer 1.8 m above a 20 km ground plane.
        if ( m_Fill == Fill::SkyDome )
        {
            m_Yaw      = kDomeDefaultYaw;
            m_Pitch    = kDomeDefaultPitch;
            m_Focus    = glm::vec3( 0.0f, kDomeEyeHeight, 0.0f );
            m_Distance = 0.0f;
            LOG_TRACE( "[Preview] dome view reset: yaw {:.1f} deg, elevation {:.1f} deg, {:.0f} deg vertical "
                       "field (horizon, mid and zenith in one frame)",
                       glm::degrees( m_Yaw ), glm::degrees( m_Pitch ), kDomeFov );
            return;
        }

        m_Yaw   = -0.6f;
        m_Pitch = 0.4f;

        const float halfFov = glm::radians( kFov ) * 0.5f;

        if ( m_FrameIsRound )
        {
            // A ball is bounded by its own radius from every direction, so the tightest distance at which
            // it is fully visible follows straight from the frustum: sin(fov/2) = R / d.
            m_Distance = ( m_FrameRadius / std::sin( halfFov ) ) * kFitMargin;
        }
        else
        {
            // Everything else is fitted by the CORNERS of its box, in perspective. Fitting such a shape by
            // its bounding sphere instead wastes the frame: a cube's sphere is 1.73x its half-size, so the
            // camera sits ~10% further back than it needs to and the corners never reach the edges. The
            // corner fit is exact — for the 100-unit cube it is 276 against the sphere fit's 302, and the
            // silhouette actually touches the frame.
            //
            //   corner depth  = d + dot(c, forward)
            //   inside while |dot(c, right)| <= depth * tan(fov/2)
            //   => d >= |dot(c, right)| / tan(fov/2) - dot(c, forward)
            const float tanHalf = std::tan( halfFov );

            const float     cp = std::cos( m_Pitch );
            const glm::vec3 eyeDir{ cp * std::sin( m_Yaw ), std::sin( m_Pitch ), cp * std::cos( m_Yaw ) };
            const glm::vec3 forward = -eyeDir;
            const glm::vec3 right   = glm::normalize( glm::cross(
                 forward, std::abs( forward.y ) > 0.99f ? glm::vec3( 0, 0, 1 ) : glm::vec3( 0, 1, 0 ) ) );
            const glm::vec3 up      = glm::normalize( glm::cross( right, forward ) );

            float needed = 0.0f;
            for ( int corner = 0; corner < 8; ++corner )
            {
                const glm::vec3 c( ( corner & 1 ) ? m_FrameHalfExtent.x : -m_FrameHalfExtent.x,
                                   ( corner & 2 ) ? m_FrameHalfExtent.y : -m_FrameHalfExtent.y,
                                   ( corner & 4 ) ? m_FrameHalfExtent.z : -m_FrameHalfExtent.z );

                const float lateral = std::max( std::abs( glm::dot( c, right ) ), std::abs( glm::dot( c, up ) ) );
                needed              = std::max( needed, lateral / tanHalf - glm::dot( c, forward ) );
            }

            // The preview is square, so one tan covers both axes. Never inside the content's own sphere.
            m_Distance = std::max( needed * kFitMargin, m_FrameRadius );
        }

        LOG_TRACE( "[Preview] fit: {} radius {:.1f} (half-extent {:.1f} x {:.1f} x {:.1f}) -> distance {:.1f}",
                   m_FrameIsRound ? "round," : "boxed,", m_FrameRadius, m_FrameHalfExtent.x, m_FrameHalfExtent.y,
                   m_FrameHalfExtent.z, m_Distance );
    }

    void PreviewViewport::ApplyCamera( uint32_t width, uint32_t height )
    {
        // The dome's camera is the opposite of the orbit's: it does not move, it turns. m_Pitch is the
        // elevation it LOOKS at, so the camera's own pitch Euler takes it unnegated — the orbit's negation
        // is there because a camera ABOVE a subject looks DOWN at it, and there is no subject here.
        if ( m_Fill == Fill::SkyDome )
        {
            m_Camera->SetFromTransform( glm::vec3( 0.0f, kDomeEyeHeight, 0.0f ), glm::vec3( m_Pitch, m_Yaw, 0.0f ),
                                        kDomeFov, kDomeNearPlane, kDomeFarPlane, width, height );
            return;
        }

        // Spherical orbit around m_Focus. Pitch is the camera's ELEVATION (positive = above the target); the
        // Euler the camera wants is its own pitch, which is the opposite sign.
        const float     cp = std::cos( m_Pitch );
        const glm::vec3 offset{ cp * std::sin( m_Yaw ), std::sin( m_Pitch ), cp * std::cos( m_Yaw ) };
        const glm::vec3 position = m_Focus + offset * m_Distance;

        m_Camera->SetFromTransform( position, glm::vec3( -m_Pitch, m_Yaw, 0.0f ), kFov, kNearPlane, kFarPlane,
                                    width, height );
    }

    void PreviewViewport::Update( uint32_t width, uint32_t height )
    {
        if ( !m_HasContent || width == 0 || height == 0 )
            return;

        EnsureInit( Graphic::ViewExtent{ width, height } );

        // A mesh requested before the MeshService had it: keep trying so it gets framed the frame it lands.
        if ( !m_Framed && static_cast<uint64_t>( m_MeshHandle ) != 0 )
        {
            m_Framed = TryFrameMesh();
        }

        // Resize recreates framebuffers and idles the GPU, so only on an actual change.
        if ( width != m_Width || height != m_Height )
        {
            m_Scene->Resize( width, height );
            m_Width  = width;
            m_Height = height;
        }

        // The preview world, written onto the entities before the scene records. Unconditionally: see
        // ApplySetup for why there is no dirty flag.
        ApplySetup();

        // THE QUALITY THIS PANE RENDERS AT, applied to a COPY of the machine's answer (К3). A preview is
        // a few hundred pixels marching at quarter resolution and has no use for the viewport's cloud
        // budget, so the tier drops here — on the way in, never in the store. That is К10's rule and the
        // reason it exists: a view's transient idea of what it needs must not become the user's permanent
        // one. Pushed every frame, like EditorLayer pushes the viewport's, so the pane follows a change
        // made in Scene Settings instead of holding whatever the machine said when the panel opened.
        Common::Settings::MachineSettings quality = Common::Settings::MachineSettings::Get();
        quality.CloudQualityTier                  = Common::Settings::CloudQuality::Low;
        m_Renderer->SetQuality( quality );

        ApplyCamera( width, height );

        // Recorded into the editor's current frame command buffer, submitted when the frame ends. This is
        // why Update() must run from OnPreUpdate() and never from OnUIRender(). The scene opens and closes
        // its own renderer inside this call, so a refusal cannot leave that buffer holding half a pass.
        if ( const auto frame = m_Scene->OnUpdate( Common::Timestep( 0.016f ) ); !frame.IsSuccess() )
            LOG_ERROR( "[PreviewViewport] preview frame skipped: {}", frame.GetError() );
    }

    bool PreviewViewport::IsSkyRebuilding() const
    {
        // FILL::SKYDOME IS PART OF THE QUESTION, not an optimisation of it. Only the volume domain creates
        // the cloud layer, so a mesh or material preview can never be baking — and asking the renderer
        // anyway would make this widget's answer depend on a system it has no cloud entity for.
        return m_Fill == Fill::SkyDome && m_Renderer && m_Renderer->IsCloudVolumeBaking();
    }

    PreviewInputResult PreviewViewport::Draw( UI::UIHelper& uiHelper, const ImVec2& size, PreviewInteraction mode )
    {
        const ImVec2 drawSize( std::max( size.x, 16.0f ), std::max( size.y, 16.0f ) );
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // The interactive item comes FIRST and the image is painted into it, because UIHelper::Image is not
        // an ImGui item and so can't be hovered or dragged.
        ImGui::InvisibleButton( "##preview_viewport", drawSize,
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();

        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const ImVec2 end( origin.x + drawSize.x, origin.y + drawSize.y );

        const void* texId = nullptr;
        if ( m_HasContent && m_Width > 0 )
        {
            if ( const auto image = m_Scene->GetFinalImage() )
                texId = uiHelper.GetTextureID( image );
        }

        constexpr float kRounding = 4.0f;
        if ( texId )
        {
            dl->AddImageRounded( reinterpret_cast<ImTextureID>( const_cast<void*>( texId ) ), origin, end,
                                 ImVec2( 0, 0 ), ImVec2( 1, 1 ), IM_COL32_WHITE, kRounding );
        }
        else
        {
            dl->AddRectFilled( origin, end, IM_COL32( 28, 30, 34, 255 ), kRounding );
            const char*  label = m_HasContent ? "Rendering..." : "No preview";
            const ImVec2 ts    = ImGui::CalcTextSize( label );
            dl->AddText(
                 ImVec2( origin.x + ( drawSize.x - ts.x ) * 0.5f, origin.y + ( drawSize.y - ts.y ) * 0.5f ),
                 IM_COL32( 130, 135, 145, 255 ), label );
        }
        dl->AddRect( origin, end, ImGui::GetColorU32( ImGuiCol_Border ), kRounding );

        // ── "REBUILDING THE SKY", ON THE PICTURE THAT IS STALE ─────────────────────────────────────────
        //
        // WHY HERE AND NOT IN A STATUS LINE. The thing the artist is looking at while they wait IS this
        // image, and the sentence they need is about this image: the sky in it is the one from before the
        // edit. A badge somewhere else is a badge they are not looking at — the owner reported this defect
        // twice as "the cloud preview still doesn't update straight away", which is what a stale picture
        // with no label says to a person.
        //
        // IT IS ALSO THE ONE PLACE THAT NEEDS NO COOPERATION. This widget owns the renderer that knows the
        // answer and owns the rectangle the answer is about, so nothing has to be pushed anywhere and no
        // panel can forget to draw it.
        if ( IsSkyRebuilding() )
        {
            // WITH A PERCENTAGE, because the wait is SECONDS and varies by four times with the coverage:
            // 5.9 s for a 256 grid and 1.5 s for the 128 this pane uses, measured on this machine. "Working"
            // and "40% of the way through a six-second job" are different sentences to somebody deciding
            // whether to keep dragging, and the bake already computes the fraction for its own cancellation
            // check, so it costs one relaxed load here.
            char label[64];
            std::snprintf( label, sizeof( label ), "Rebuilding the sky... %.0f%%",
                           m_Renderer->CloudVolumeBakeProgress() * 100.0f );

            constexpr float kPad = 6.0f;
            const ImVec2    ts   = ImGui::CalcTextSize( label );
            const ImVec2    boxMin( origin.x + kPad, origin.y + kPad );
            const ImVec2    boxMax( boxMin.x + ts.x + kPad * 2.0f, boxMin.y + ts.y + kPad );

            // Painted over the image rather than blended into it, and DARK: the badge has to be legible
            // over a bright sky, which is the only content this pane ever shows while it is up.
            dl->AddRectFilled( boxMin, boxMax, IM_COL32( 18, 20, 24, 205 ), kRounding );
            dl->AddText( ImVec2( boxMin.x + kPad, boxMin.y + kPad * 0.5f ), IM_COL32( 235, 200, 120, 255 ),
                         label );
        }

        if ( !m_HasContent )
            return {};

        const bool dome = ( m_Fill == Fill::SkyDome );

        PreviewInputEvents events;
        events.Hovered       = hovered;
        events.Active        = active;
        events.RightDown     = ImGui::IsMouseDown( ImGuiMouseButton_Right );
        events.DoubleClicked = ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left );
        events.Dome          = dome;
        events.MouseDelta    = { ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y };
        events.Wheel         = ImGui::GetIO().MouseWheel;
        // HOLD L AND DRAG TO MOVE THE SUN — UE's binding, and the one control in this widget that changes
        // what the material looks like rather than where it is looked at from. It is the whole reason a
        // preview is worth opening twice: a material reads completely differently under a low sun, and
        // being able to swing the key light with the mouse is worth more than any number of fields.
        //
        // It moves ONE thing, and that is deliberate: the key light and the sun in the sky are the same
        // vector here (SceneSetup::SunYawDegrees), so the object's shading, its cast shadow and the dome
        // behind it all turn together. UE also binds K to the environment, and this widget does not — see
        // the report; with a preset sky an environment rotation IS the sun's azimuth, and the engine has
        // no rotation for a cubemap environment to offer instead.
        events.LightKeyDown = ImGui::IsKeyDown( ImGuiKey_L );

        const PreviewInputResult input = PreviewInput( mode, events );

        m_DraggingLight = ( input.SunDelta.x != 0.0f || input.SunDelta.y != 0.0f );
        if ( m_DraggingLight )
        {
            // Degrees per pixel, and slower vertically: the elevation has a quarter of the yaw's range to
            // travel, so a shared rate would make the sun jump from noon to sunset in a few pixels.
            constexpr float kSunYawPerPixel   = 0.45f;
            constexpr float kSunPitchPerPixel = 0.25f;
            m_Setup.SunYawDegrees -= input.SunDelta.x * kSunYawPerPixel;
            // Wrapped rather than clamped: a bearing has no ends, and a sun that stuck at 180 would be an
            // artist dragging against a wall halfway round the compass.
            if ( m_Setup.SunYawDegrees > 180.0f )
                m_Setup.SunYawDegrees -= 360.0f;
            if ( m_Setup.SunYawDegrees < -180.0f )
                m_Setup.SunYawDegrees += 360.0f;

            // Clamped just above the horizon rather than at it: a sun AT zero elevation is the degenerate
            // case the sky pass warns about (below the horizon it renders night), and stopping at 1 degree
            // keeps every drag inside a lit sky.
            m_Setup.SunPitchDegrees =
                 std::clamp( m_Setup.SunPitchDegrees - input.SunDelta.y * kSunPitchPerPixel, 1.0f, 89.0f );
        }

        if ( input.PanDelta.x != 0.0f || input.PanDelta.y != 0.0f )
        {
            // Screen-proportional: one pixel moves the focus by the same fraction of the framed object at
            // any zoom, so panning never feels different when you are close in. Panning moves the orbit's
            // focus in the camera's own screen plane.
            const float     cp = std::cos( m_Pitch );
            const glm::vec3 forward{ -cp * std::sin( m_Yaw ), -std::sin( m_Pitch ), -cp * std::cos( m_Yaw ) };
            const glm::vec3 right = glm::normalize( glm::cross( forward, glm::vec3( 0, 1, 0 ) ) );
            const glm::vec3 up    = glm::normalize( glm::cross( right, forward ) );

            const float speed = m_Distance / std::max( drawSize.y, 1.0f );
            m_Focus += ( -right * input.PanDelta.x + up * input.PanDelta.y ) * speed;
        }

        if ( input.OrbitDelta.x != 0.0f || input.OrbitDelta.y != 0.0f )
        {
            // In the dome this is not an orbit but a turn of the head; the arithmetic is the same and the
            // sign convention is handled where the camera is built (ApplyCamera).
            constexpr float kOrbitSpeed = 0.008f; // radians per pixel
            m_Yaw -= input.OrbitDelta.x * kOrbitSpeed;
            m_Pitch = std::clamp( m_Pitch + input.OrbitDelta.y * kOrbitSpeed, -kPitchLimit, kPitchLimit );
        }

        if ( input.Wheel != 0.0f )
        {
            // Multiplicative so the zoom feels the same at every distance, clamped so the asset can neither
            // be swallowed by the near plane nor lost to a dot.
            m_Distance = std::clamp( m_Distance * std::exp( -input.Wheel * 0.12f ), m_FrameRadius * 0.6f,
                                     m_FrameRadius * 20.0f );
        }

        if ( input.Reframe )
            ResetView();

        if ( hovered )
        {
            ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
            // One sentence per camera, because they genuinely do different things: a promise of panning and
            // zoom in a view that has neither would be describing a different widget, and a Static row that
            // advertised dragging would be describing the editor window it opens.
            if ( mode == PreviewInteraction::Static )
                ImGui::SetTooltip( "Double-click to open" );
            else
                ImGui::SetTooltip( dome ? "Drag to look around - hold L and drag to move the sun - double-click "
                                          "to reset"
                                        : "Drag to orbit - right-drag to pan - wheel to zoom - hold L and drag "
                                          "to move the sun - double-click to reset" );
        }

        return input;
    }
} // namespace Desert::Editor
