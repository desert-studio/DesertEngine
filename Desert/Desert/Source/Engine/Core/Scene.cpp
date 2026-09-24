#include <Engine/Core/Scene.hpp>

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/ECS/System/SystemRules.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/World/Landscape/LandscapeRaycast.hpp>

#include <algorithm>
#include <cfloat>
#include <functional>
#include <limits>
#include <typeinfo>

#include <Engine/Core/Projection.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>

namespace Desert::Core
{
    namespace
    {
        // Resolve a static-mesh component to its renderable Mesh (handle / runtime-edited / shared primitive).
        // This used to live in the editor's ViewportPanel (engine logic that leaked into the editor) — it
        // belongs here, where Raycast and the mesh systems can share it.
        ::Desert::Mesh* ResolveMesh( const ECS::StaticMeshComponent& c )
        {
            if ( c.MeshHandle )
                return Runtime::ResourceRegistry::GetMeshService()->Get( c.MeshHandle );
            if ( c.RuntimeMesh )
                return c.RuntimeMesh.get();
            if ( c.Primitive.has_value() )
                return Geometry::PrimitiveMeshFactory::GetShared( c.Primitive.value() );
            return nullptr;
        }

        // Editor-built runtime rig takes priority over the cooked asset (mirrors the render path).
        ::Desert::Mesh* ResolveSkinnedMesh( const ECS::SkinnedMeshComponent& c )
        {
            if ( c.RuntimeMesh )
                return c.RuntimeMesh.get();
            if ( c.MeshHandle )
                return Runtime::ResourceRegistry::GetMeshService()->Get( c.MeshHandle );
            return nullptr;
        }

        // Mesh-local AABB of a skinned mesh deformed by `skin` (linear blend). A skinned submesh's stored
        // BoundingBox is in RAW-vertex space (which only matches the rendered mesh when bind == identity), so
        // picking must deform the retained CPU vertices by the current pose instead of using that box.
        Common::Math::AABB SkinnedLocalBounds( const SkinnedMesh& mesh, const std::vector<glm::mat4>& skin )
        {
            glm::vec3 mn( FLT_MAX ), mx( -FLT_MAX );
            for ( const auto& sv : mesh.GetVertices() )
            {
                glm::vec3 pos( 0.0f );
                float     wsum = 0.0f;
                for ( size_t j = 0; j < SkinnedVertex::MAX_BONE_INFLUENCES; ++j )
                {
                    const float w = sv.BoneWeights[j];
                    if ( w <= 0.0f )
                        continue;
                    const uint32_t b = sv.BoneIDs[j];
                    if ( b < skin.size() )
                        pos += w * glm::vec3( skin[b] * glm::vec4( sv.StaticVertex.Position, 1.0f ) );
                    wsum += w;
                }
                if ( wsum > 1e-5f )
                    pos /= wsum; // weighted average (robust to weights that don't sum to exactly 1)
                else
                    pos = sv.StaticVertex.Position;
                mn = glm::min( mn, pos );
                mx = glm::max( mx, pos );
            }
            return { mn, mx };
        }
        // Every landscape tile that is drawn: loaded heights, under a loaded root they match. A tile the
        // renderer refuses (LandscapeECSSystem warns why) is not on screen, so it is not pickable either.
        struct PickableTile
        {
            Common::UUID                       Entity;
            World::Landscape::LandscapeRayTile Ray;
        };

        std::vector<PickableTile> PickableLandscapeTiles( const Scene& scene )
        {
            std::vector<PickableTile> tiles;
            for ( const auto& entity : scene.GetAllEntities() )
            {
                if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                const auto& tile = entity.GetComponent<ECS::LandscapeTileComponent>();
                if ( !tile.Heights.has_value() )
                    continue;
                const auto rootEntity = scene.FindEntityByID( tile.Landscape );
                if ( !rootEntity.has_value() || !rootEntity->get().HasComponent<ECS::LandscapeComponent>() )
                    continue;
                const auto root = ECS::LandscapeRootOf( rootEntity->get() );
                // The renderer refuses the same tile (and says so once); a pick is an explicit request, so
                // it is told why the tile under the cursor cannot answer.
                const auto fits = World::Landscape::CheckTileMatchesRoot( *tile.Heights, root );
                if ( !fits.IsSuccess() )
                {
                    LOG_WARN( "[Landscape] tile ({}, {}) is not pickable: {}", tile.TileX, tile.TileZ,
                              fits.GetError() );
                    continue;
                }
                PickableTile pick;
                pick.Entity      = entity.GetComponent<ECS::UUIDComponent>().UUID;
                pick.Ray.Heights = &*tile.Heights;
                pick.Ray.Frame   = World::Landscape::LandscapeTileFrame( root, tile.TileX, tile.TileZ );
                tiles.push_back( pick );
            }
            return tiles;
        }
    } // namespace

    bool Scene::Raycast( const Common::Math::Ray& ray, RaycastHit& outHit ) const
    {
        float              closest = std::numeric_limits<float>::max();
        glm::mat4          bestXf( 1.0f );
        Common::Math::AABB bestAABB;
        Common::Math::Ray  bestLocal  = ray;
        float              bestLocalT = 0.0f;
        Common::UUID       bestUUID;
        bool               hit = false;

        for ( const auto& entity : GetAllEntities() )
        {
            ::Desert::Mesh* mesh    = nullptr;
            bool            skinned = false;
            if ( entity.HasComponent<ECS::StaticMeshComponent>() )
            {
                mesh = ResolveMesh( entity.GetComponent<ECS::StaticMeshComponent>() );
            }
            else if ( entity.HasComponent<ECS::SkinnedMeshComponent>() )
            {
                mesh    = ResolveSkinnedMesh( entity.GetComponent<ECS::SkinnedMeshComponent>() );
                skinned = true;
            }
            if ( !mesh )
                continue;

            const glm::mat4 xf       = entity.GetWorldTransform();
            const auto      localRay = ray.ToLocalSpace( xf );

            if ( !skinned )
            {
                for ( const auto& sm : mesh->GetSubmeshes() )
                {
                    float t = 0.0f;
                    if ( !localRay.IntersectsAABB( sm.BoundingBox, t ) || t <= 0.0f )
                        continue;
                    // Compared in WORLD units: a local t is in the entity's own (scaled) units.
                    if ( const float d = ray.WorldDistanceOf( localRay, t, xf ); d > 0.0f && d < closest )
                    {
                        closest    = d;
                        bestXf     = xf;
                        bestAABB   = sm.BoundingBox;
                        bestLocal  = localRay;
                        bestLocalT = t;
                        bestUUID   = entity.GetComponent<ECS::UUIDComponent>().UUID;
                        hit        = true;
                    }
                }
            }
            else
            {
                // Skinned: test the POSED mesh bounds (animator pose if any, else bind) — the stored submesh
                // box is raw-vertex space and would miss/mis-hit an imported mesh whose bind != identity.
                auto*                  sk = static_cast<SkinnedMesh*>( mesh );
                std::vector<glm::mat4> skin;
                if ( entity.HasComponent<ECS::AnimationComponent>() )
                {
                    const auto& anim = entity.GetComponent<ECS::AnimationComponent>();
                    if ( anim.Animator )
                        skin = anim.Animator->GetPose().Matrices;
                }
                if ( skin.empty() )
                    sk->GetSkeleton().WriteBindSkinningMatrices( skin );

                const Common::Math::AABB bounds = SkinnedLocalBounds( *sk, skin );
                float                    t      = 0.0f;
                if ( !localRay.IntersectsAABB( bounds, t ) || t <= 0.0f )
                    continue;
                if ( const float d = ray.WorldDistanceOf( localRay, t, xf ); d > 0.0f && d < closest )
                {
                    closest    = d;
                    bestXf     = xf;
                    bestAABB   = bounds;
                    bestLocal  = localRay;
                    bestLocalT = t;
                    bestUUID   = entity.GetComponent<ECS::UUIDComponent>().UUID;
                    hit        = true;
                }
            }
        }

        // The landscape after the meshes, against the nearest mesh distance: a rock standing on a hill is
        // picked when the ray meets the rock first, the hill otherwise.
        const auto landscape = PickableLandscapeTiles( *this );
        if ( !landscape.empty() )
        {
            std::vector<World::Landscape::LandscapeRayTile> rayTiles;
            rayTiles.reserve( landscape.size() );
            for ( const auto& tile : landscape )
                rayTiles.push_back( tile.Ray );
            const float limit = hit ? closest : std::numeric_limits<float>::max();
            if ( const auto terrain =
                      World::Landscape::RaycastLandscape( rayTiles, ray.Origin, ray.Direction, limit );
                 terrain && terrain->Distance < limit )
            {
                outHit.Hit      = true;
                outHit.Entity   = landscape[terrain->Tile].Entity;
                outHit.Point    = terrain->Point;
                outHit.Normal   = terrain->Normal;
                outHit.Distance = terrain->Distance;
                return true;
            }
        }

        outHit.Hit = hit;
        if ( !hit )
            return false;

        outHit.Entity   = bestUUID;
        outHit.Distance = closest;
        outHit.Point    = ray.GetPoint( closest );

        // Box-face normal from the local hit (dominant axis of the offset from the AABB centre).
        const glm::vec3 lp = bestLocal.GetPoint( bestLocalT );
        const glm::vec3 c  = ( bestAABB.Min + bestAABB.Max ) * 0.5f;
        const glm::vec3 he = glm::max( ( bestAABB.Max - bestAABB.Min ) * 0.5f, glm::vec3( 1e-4f ) );
        const glm::vec3 dd = ( lp - c ) / he;
        const glm::vec3 ad = glm::abs( dd );
        const glm::vec3 ln = ( ad.x >= ad.y && ad.x >= ad.z ) ? glm::vec3( glm::sign( dd.x ), 0.0f, 0.0f )
                             : ( ad.y >= ad.z )                ? glm::vec3( 0.0f, glm::sign( dd.y ), 0.0f )
                                                               : glm::vec3( 0.0f, 0.0f, glm::sign( dd.z ) );
        outHit.Normal = glm::normalize( glm::mat3( bestXf ) * ln );
        return true;
    }
    namespace
    {
        // The live-scene list. A function-local static so it is constructed on first use whatever the
        // static-initialisation order is — a Scene can be built from another translation unit's static.
        std::vector<Scene*>& LiveSceneList()
        {
            static std::vector<Scene*> scenes;
            return scenes;
        }
    } // namespace

    const std::vector<Scene*>& Scene::LiveScenes()
    {
        return LiveSceneList();
    }

    Scene::Scene()
    {
        LiveSceneList().push_back( this );
    }

    Scene::Scene( std::string&& sceneName, Graphic::SceneRenderer* sceneRenderer )
         : m_SceneName( std::move( sceneName ) )
    {
        LiveSceneList().push_back( this );
        SetupRegistryCallbacks();

        // View 0, the one every existing caller means by "the scene's renderer". The camera it will be
        // aimed with is made by Init() — a Scene is constructed before the device work Init() does, and
        // a camera handed out before then would be a second one to keep in step.
        //
        // The refusal is LOGGED rather than dropped: a null renderer here used to be stored as-is into a
        // raw pointer that nothing ever checked, and the first frame dereferenced it.
        if ( sceneRenderer != nullptr && !m_Views.Add( sceneRenderer, nullptr ) )
            LOG_ERROR( "[Scene] '{}' refused its own renderer as view 0.", m_SceneName );
    }

    Scene::~Scene()
    {
        // Erased in the destructor and nowhere else, so an entry cannot outlive the object it points at.
        // That is the whole reason the list holds raw pointers rather than weak_ptrs: a Scene is in it for
        // exactly its own lifetime and there is no window in which a reader could see a dead one.
        auto& scenes = LiveSceneList();
        scenes.erase( std::remove( scenes.begin(), scenes.end(), this ), scenes.end() );
    }

    NO_DISCARD Common::BoolResultStr Scene::Init()
    {
        for ( auto& view : m_Views.All() )
            view.Renderer->Init();

        // Every scene gets a persistent EditorCamera as its Edit-mode view, so the viewport works
        // immediately (no "add a camera" requirement) and the editor view is independent of any scene
        // CameraComponent.
        if ( !m_EditorCamera )
            m_EditorCamera = std::make_shared<EditorCamera>();
        if ( m_State != SceneState::Play && !m_CameraPinned )
            SetActiveCamera( m_EditorCamera );

        // Views opened BEFORE the first Init (there is always at least view 0) get a camera here; a view
        // opened after it is given one by AddView. Only view 0 takes the scene's shared EditorCamera —
        // see SetActiveCamera for why the others must not.
        for ( size_t i = 1; i < m_Views.Count(); ++i )
        {
            if ( auto* view = m_Views.At( i ); view && !view->Camera )
                view->Camera = std::make_shared<EditorCamera>();
        }

        m_Initialized = true;

        // A WORLD HAS JUST CHANGED, SO THE ANSWER TO "WHAT IS STILL NEEDED" HAS CHANGED. Asked for rather
        // than done here: the scene being replaced may still be alive at this instant (the caller usually
        // drops it after the new one is up), and a sweep that saw it would find its assets reachable and
        // release nothing — which is exactly how "evict on scene change" quietly does nothing. The frame
        // loop runs it at the start of the next frame, by which time the old scene is gone and no command
        // buffer is open. See Assets/AssetEviction.hpp.
        Assets::AssetEvictionSchedule::Request( "scene '" + m_SceneName + "' was initialised" );

        return BOOLSUCCESS;
    }

    void Scene::PinActiveCamera( const std::shared_ptr<Core::Camera>& camera )
    {
        if ( !camera )
        {
            m_CameraPinned = false;
            return;
        }

        m_CameraPinned = true;
        SetActiveCamera( camera );

        // The scene still owns an EditorCamera (Init() always makes one). It polls the global mouse and
        // keyboard directly, so leaving it live in an offscreen scene means it flies along with the real
        // viewport — which is what made the Details preview follow the scene camera.
        if ( auto* editorCam = dynamic_cast<EditorCamera*>( m_EditorCamera.get() ) )
            editorCam->SetInputEnabled( false );
    }

    void Scene::UpdateActiveCameraSource()
    {
        // Camera source follows the play state: Edit/Paused -> EditorCamera; Play -> the main
        // CameraComponent (driven into a GameplayCamera each frame so moving the camera entity moves the
        // view). If Play has no camera entity, fall back to the editor camera so you still see the scene.
        if ( m_State == SceneState::Play )
        {
            const ECS::CameraComponent* mainCam    = nullptr;
            entt::entity                mainEntity = entt::null;
            auto camView = m_Registry.view<ECS::CameraComponent, ECS::TransformComponent>();
            for ( auto entity : camView )
            {
                const auto& cc = camView.get<ECS::CameraComponent>( entity );
                if ( !mainCam || cc.Data.IsMainCamera ) // prefer an IsMainCamera, else the first one
                {
                    mainCam    = &cc;
                    mainEntity = entity;
                    if ( cc.Data.IsMainCamera )
                        break;
                }
            }

            if ( mainCam && mainEntity != entt::null )
            {
                // WORLD transform of the camera entity (walk the parent chain), so a camera parented to a
                // moving entity (e.g. a child of the character controller) follows it — 1st/3rd person is
                // just the child's local offset.
                glm::mat4    world = m_Registry.get<ECS::TransformComponent>( mainEntity ).GetTransform();
                entt::entity cur   = mainEntity;
                while ( m_Registry.has<ECS::RelationshipComponent>( cur ) )
                {
                    const auto& rel = m_Registry.get<ECS::RelationshipComponent>( cur );
                    if ( rel.Parent == entt::null )
                        break;
                    cur = rel.Parent;
                    if ( m_Registry.has<ECS::TransformComponent>( cur ) )
                        world = m_Registry.get<ECS::TransformComponent>( cur ).GetTransform() * world;
                }
                if ( !m_GameplayCamera )
                    m_GameplayCamera = std::make_shared<GameplayCamera>();
                static_cast<GameplayCamera*>( m_GameplayCamera.get() )
                     ->SetView(
                          CameraEntityViewOf( world, mainCam->Data.FOV, mainCam->Data.Near, mainCam->Data.Far ),
                          m_ViewportWidth, m_ViewportHeight );
                if ( GetActiveCamera() != m_GameplayCamera )
                    SetActiveCamera( m_GameplayCamera );
            }
            else if ( GetActiveCamera() != m_EditorCamera )
            {
                SetActiveCamera( m_EditorCamera ); // no game camera -> keep the editor view
            }
        }
        else if ( m_EditorCamera && GetActiveCamera() != m_EditorCamera )
        {
            SetActiveCamera( m_EditorCamera );
        }
    }

    NO_DISCARD Common::BoolResultStr Scene::OnUpdate( const Common::Timestep& ts )
    {
        // A pinned camera is driven from OUTSIDE the scene (the Details preview orbits its own), so the
        // play-state rule must not hand the view back to the EditorCamera behind its back — that is a
        // per-frame overwrite, and it is why the preview rendered through the input-driven editor camera
        // one frame after being told not to.
        if ( !m_CameraPinned )
            UpdateActiveCameraSource();

        // The first view that refused, reported after every other view has had its frame: one broken
        // viewport must not silently cost the others theirs.
        std::string firstError;

        Graphic::SceneRenderer::UpdateInfo sceneRendererInfo;
        sceneRendererInfo.Timestep = ts;

        // Gameplay time only advances in Play (Edit/Paused freeze it -> animation/physics/scripts hold).
        // Systems still RUN every frame (they collect render data); they just see a zero timestep when not
        // playing. The editor camera below uses the real ts so you can fly around while paused/editing.
        const Common::Timestep gameplayTs =
             ( m_State == SceneState::Play ) ? ts : Common::Timestep( 0.0f );

        // Push the active-camera snapshot to systems that lay out camera-relative geometry (billboarded
        // text). Done on the main thread before ExecuteSystems so the parallel system group reads it
        // race-free (SetCameraSnapshot is a no-op for every other system).
        //
        // VIEW 0's CAMERA, FOR EVERY VIEW, AND THAT IS A NAMED COST OF TRAVERSING THE ECS ONCE. The only
        // consumer is camera-relative LAYOUT (billboarded text turns to face the viewer), and that
        // layout is produced by the single ECS pass below — so with two views open, text in the second
        // one faces the first one's camera. Making it per-view means running the collectors per view,
        // which is the whole cost this change exists to remove. Named here rather than discovered later.
        if ( const auto primary = GetActiveCamera() )
        {
            const glm::mat4 camView = primary->GetViewMatrix();
            const glm::vec3 camPos  = primary->GetPosition();
            for ( auto& system : m_Systems )
                system->SetCameraSnapshot( camView, camPos );
        }

        {
            DESERT_PROFILE_SCOPE( "ECS Systems" );
            ExecuteSystems( gameplayTs );
        }

        // Dir lights
        {
            DESERT_PROFILE_SCOPE( "Scene: DirLights" );
            auto dirLightGroup =
                 m_Registry.group<ECS::DirectionLightComponent>( entt::get<ECS::TransformComponent> );

            // WHICH LIGHTS WERE THROWN AWAY, AND WHY ANYONE SHOULD HEAR ABOUT IT.
            //
            // A DirectionLightComponent takes its direction from its TRANSLATION, not its rotation
            // (nothing reads a sun's Rotation anywhere in this engine), so a sun authored the way a sun
            // looks like it should be authored — drop one in, aim it with the rotation gizmo — has a
            // Translation of (0,0,0), is not a direction, and cannot be collected. It then sits in the
            // outliner with an intensity slider, casting nothing, and this loop used to say NOTHING
            // about it. Five lines below, the "too many directional lights" case goes to the trouble of
            // naming the offending entities; the case where a light contributes zero got silence.
            std::string degenerate;
            dirLightGroup.each(
                 [&]( entt::entity entity, const auto& light, const auto& transform )
                 {
                     // A HIDDEN SUN CONTRIBUTES NOTHING, and it is dropped BEFORE the degenerate-direction
                     // report below: a light the artist deliberately hid is not a light they mis-authored,
                     // and naming it in that error every frame would train them to ignore the one message
                     // that tells them a real sun is aimed wrong. SkyboxECSSystem drops the same entity
                     // from its sun candidates — one hidden light, both of its effects gone.
                     if ( ECS::IsHidden( m_Registry, entity ) )
                         return;

                     // ONE gate, shared with the sky (SystemRules.hpp): this site used to open-code
                     // `length > 0.001f`, ten times kSunDirectionEpsilon, so a light in between was a
                     // sun to the atmosphere and no light at all to the renderer.
                     if ( const auto travel = ECS::Rules::DirectionalLightTravel( transform.Translation ) )
                     {
                         sceneRendererInfo.DirLights.DirectionLights.push_back(
                              { glm::vec4( *travel, 0.0f ),
                                glm::vec4( light.Data.Color, light.Data.Intensity ) } );
                         return;
                     }
                     if ( m_Registry.has<ECS::TagComponent>( entity ) )
                         degenerate +=
                              ( degenerate.empty() ? "" : ", " ) + m_Registry.get<ECS::TagComponent>( entity ).Tag;
                 } );

            // Latched on the MESSAGE, not on a flag: this runs every frame, so an unlatched LOG_ERROR
            // here would bury the log at 60 lines a second, and a plain once-only flag would go quiet
            // about the SECOND light to go degenerate. Comparing the text reports each distinct state
            // once and re-reports when the set changes — including back to empty, which is the line that
            // tells a user their fix worked.
            if ( degenerate != m_DegenerateDirLightsReported )
            {
                m_DegenerateDirLightsReported = degenerate;
                if ( !degenerate.empty() )
                    LOG_ERROR( "[Scene] directional light(s) {} have a Translation shorter than {} and "
                               "emit NOTHING. A directional light takes its direction from its "
                               "TRANSLATION (the direction the light travels), not from its Rotation.",
                               degenerate, ECS::Rules::kSunDirectionEpsilon );
            }

            // The engine supports EXACTLY ONE directional light (DirectionLightsUB is a single
            // struct — a second payload overflows every PBR material's UB and aborts). Truncate
            // loudly instead of crashing; name the extras so the offending entity is findable.
            if ( sceneRendererInfo.DirLights.DirectionLights.size() > 1 )
            {
                std::string names;
                dirLightGroup.each(
                     [&]( entt::entity entity, const auto&, const auto& )
                     {
                         if ( m_Registry.has<ECS::TagComponent>( entity ) )
                             names += ( names.empty() ? "" : ", " ) +
                                      m_Registry.get<ECS::TagComponent>( entity ).Tag;
                     } );
                LOG_ERROR( "[Scene] {} directional lights collected ({}) — only ONE is supported; "
                           "using the first.",
                           sceneRendererInfo.DirLights.DirectionLights.size(), names );
                sceneRendererInfo.DirLights.DirectionLights.resize( 1 );
            }
        }

        // Every view's camera is ticked, not just view 0's: an EditorCamera integrates its own input and
        // a view whose camera never updated would be frozen in place while its window says otherwise.
        for ( auto& view : m_Views.All() )
        {
            if ( view.Camera )
                view.Camera->OnUpdate( ts );
        }

        // ── THE LINE BETWEEN "ONCE" AND "PER VIEW" ───────────────────────────────────────────────────
        //
        // Everything above this point read the world: ExecuteSystems walked the ECS and recorded draw
        // commands into m_SystemCommandBuffers, and the directional-light group was collected. It runs
        // ONCE no matter how many views are open, which is the point of the view list — a second angle
        // on one world must cost a second set of GPU passes, not a second walk of every component pool.
        //
        // Everything below is per view. RenderCommandBuffer::ExecuteAll does not consume the buffer (the
        // arena is rewound by Clear, which happens after the last view), so replaying the SAME recording
        // into a second renderer is exactly what it already supports — the draws are identical because
        // the world is identical; only the camera each renderer was opened with differs.
        {
            DESERT_PROFILE_SCOPE( "Scene: Views" );
            for ( auto& view : m_Views.All() )
            {
                // ONE VIEW, ONE COMPLETE SEQUENCE — open, record, render, close — AND THE SEQUENCES MUST
                // NOT INTERLEAVE. The scene used to expose three separate frame phases, each of which
                // looped over the views, so two views gave open(A) open(B) … render(A) render(B) …
                // close(A) close(B). MEASURED: the second view's HDR target came out BLACK — not dark,
                // not mis-lit, empty — while its inputs were provably identical (same atmosphere sun
                // (0.351, 0.902, 0.251), same directional light, its own 960x584 framebuffer, its own
                // G-buffer, a camera at the same position as the view that worked). Driving each view as
                // one contiguous sequence, changing nothing else, made it render.
                //
                // A renderer's per-frame state is BRACKETED by its open/close pair, and two brackets
                // must never be open at once. The offscreen renderers that have always worked
                // (PreviewViewport, AssetThumbnailRenderer) work because they were already shaped this
                // way, which is why one renderer per scene never exposed it — and it is why the three
                // phases were collapsed into this one function rather than left as an ordering rule for
                // the next caller to get right.
                if ( auto begun = view.Renderer->BeginScene( *this, view.Camera.get() ); !begun )
                {
                    firstError = begun.GetError();
                    continue; // do NOT record into a renderer that refused to open
                }

                // Registration order — NOT completion order — so the frame's submission order is
                // identical to the old single-buffer sequential path.
                for ( const auto& buffer : m_SystemCommandBuffers )
                    buffer->ExecuteAll( *view.Renderer );

                view.Renderer->OnUpdate( sceneRendererInfo );

                if ( auto ended = view.Renderer->EndScene(); !ended && firstError.empty() )
                    firstError = ended.GetError();
            }
        }
        {
            DESERT_PROFILE_SCOPE( "Scene: CmdBuffer Clear" );
            for ( const auto& buffer : m_SystemCommandBuffers )
                buffer->Clear();
        }

        // The buffers are cleared FIRST and the failure reported after: a frame that refused still has to
        // leave the arena rewound, or the next frame records on top of this one's commands.
        if ( !firstError.empty() )
            return Common::MakeError( firstError );
        return BOOLSUCCESS;
    }

    void Scene::PrepareComponentPools()
    {
        // WHY THIS EXISTS, AND WHY IT IS NOT OPTIONAL.
        //
        // EnTT creates a component pool on the FIRST touch of a type, and every path that touches one --
        // `view<T...>()`, `has<T>()`, `get<T>()`, `try_get<T>()` -- goes through `basic_registry::assure<T>()`.
        // assure MUTATES, and it does so through the CONST overload too, because `pools` is `mutable`
        // (ThirdParty/entt/include/entt/entt.hpp). One first touch does three unsynchronised writes:
        //
        //   1. `type_index<T>::value()` calls `internal::type_index::next()`, whose counter is
        //      `ENTT_MAYBE_ATOMIC(id_type) value{}` -- and ENTT_USE_ATOMIC is defined for this workspace
        //      in BuildScripts/Workspace.lua precisely so that `value++` is not a plain read-modify-write
        //      on a global. Without it two types can be handed the SAME index and then read each other's
        //      storage;
        //   2. `pools.resize( index + 1 )` -- a std::vector grow;
        //   3. `pools[index].pool.reset( ... )`.
        //
        // ExecuteSystems below runs maximal runs of CanRunParallel() systems on several threads at once,
        // so two collectors that first-touch any type in the same frame race on all three. The observed
        // symptom is `std::length_error: vector` thrown out of a vector grow that read a torn size --
        // roughly one headless run in fifty, always in the first frame, because that is the only frame in
        // which pools are created at all.
        //
        // THE FIX IS TO CREATE EVERY POOL SERIALLY, HERE, BEFORE ANY GROUP OPENS. After this runs,
        // assure<T>() is a bounds check and a pointer test for every type below -- it does not write, so
        // there is nothing left to race on.
        //
        // THE LIST MUST COVER EVERY TYPE ANY SYSTEM TOUCHES. Desert/Tests/Engine/ComponentPools asserts
        // exactly that against the system headers, so a `has<NewComponent>` added to a collector without
        // a line here turns that suite red rather than reopening the race.
        auto& r = m_Registry;

        // Hierarchy and identity -- touched by every collector that composes a world transform.
        r.prepare<ECS::RelationshipComponent>();
        r.prepare<ECS::TransformComponent>();
        r.prepare<ECS::TagComponent>();
        r.prepare<ECS::UUIDComponent>();
        r.prepare<ECS::VisibilityComponent>();

        // Renderables.
        r.prepare<ECS::StaticMeshComponent>();
        r.prepare<ECS::SkinnedMeshComponent>();
        r.prepare<ECS::InstancedStaticMeshComponent>();
        r.prepare<ECS::MaterialComponent>();
        r.prepare<ECS::AnimationComponent>();
        // Skeletal controls. AnimationECSSystem asks `has<TwoBoneIKComponent>` for every animated entity,
        // and the ask itself is a first touch — so a rig with no IK on it would have created this pool from
        // inside the parallel phase. Found by Tests/Engine/ComponentPools, which is the census that reads
        // the system headers rather than trusting this list.
        r.prepare<ECS::TwoBoneIKComponent>();
        r.prepare<ECS::ControlRigComponent>();
        r.prepare<ECS::RetargetComponent>();
        r.prepare<ECS::TextComponent>();
        r.prepare<ECS::LandscapeMaterialComponent>();

        // Lights and sky.
        r.prepare<ECS::DirectionLightComponent>();
        r.prepare<ECS::PointLightComponent>();
        r.prepare<ECS::SpotLightComponent>();
        r.prepare<ECS::SkyboxComponent>();
        r.prepare<ECS::SkyAtmosphereComponent>();
        r.prepare<ECS::ExponentialHeightFogComponent>();
        r.prepare<ECS::VolumetricCloudComponent>();
        r.prepare<ECS::HeroCloudComponent>();

        // Gameplay -- serial systems today, and prepared all the same: what makes a type safe is that its
        // pool exists before the first parallel group, not which system happens to be serial this week.
        r.prepare<ECS::ColliderComponent>();
        r.prepare<ECS::RigidBodyComponent>();
        r.prepare<ECS::CharacterControllerComponent>();
        r.prepare<ECS::LocomotionComponent>();
        r.prepare<ECS::ScriptComponent>();
        r.prepare<ECS::AudioSourceComponent>();
        r.prepare<ECS::SocketAttachmentComponent>();
    }

    void Scene::ExecuteSystems( const Common::Timestep& gameplayTs )
    {
        // BEFORE ANY PARALLEL GROUP OPENS. See PrepareComponentPools for the race this closes; it is
        // called per frame rather than once because a pool that already exists costs a bounds check, and
        // a flag that has to be reset correctly on every path that rebuilds a scene is the more expensive
        // of the two mistakes.
        PrepareComponentPools();

        // One command buffer per system, created on first use (AddSystem is a header template — the
        // buffers are built here where the type is complete).
        while ( m_SystemCommandBuffers.size() < m_Systems.size() )
            m_SystemCommandBuffers.emplace_back( std::make_unique<Graphic::Render::RenderCommandBuffer>() );

        const auto runOne = [&]( size_t index )
        {
            const auto& system = m_Systems[index];
            // Per-system timing (named by the system's type) so every ECS system is individually
            // visible in the profiler — no per-system edits.
            // The dereference is bound to a reference first because `typeid` on an expression WITH SIDE
            // EFFECTS evaluates it — `*system` is `unique_ptr::operator*`, a function call, so the operand
            // is not the plain lvalue it looks like. Same dynamic type, and the intent is now stated.
            const auto& systemRef = *system;
            DESERT_PROFILE_SCOPE_DYNAMIC( typeid( systemRef ).name() );
            system->Update( m_Registry, *m_SystemCommandBuffers[index], gameplayTs );
        };

        // Sequential systems run in registration order; a maximal RUN of CanRunParallel() systems is
        // one parallel group (they have no cross-dependencies by contract — see System::CanRunParallel).
        // ParallelFor blocks until the group finishes, so the following sequential system still sees
        // every effect of the group — the schedule is semantically identical to the sequential loop.
        size_t i = 0;
        while ( i < m_Systems.size() )
        {
            if ( !m_Systems[i]->CanRunParallel() )
            {
                runOne( i );
                ++i;
                continue;
            }

            size_t groupEnd = i + 1;
            while ( groupEnd < m_Systems.size() && m_Systems[groupEnd]->CanRunParallel() )
                ++groupEnd;

            if ( const size_t count = groupEnd - i; count == 1 )
                runOne( i );
            else
                Common::JobSystem::Get().ParallelFor( count,
                                                      [&]( size_t local ) { runOne( i + local ); } );
            i = groupEnd;
        }
    }

    std::optional<Graphic::Environment> Scene::GetEnvironment() const
    {
        auto* renderer = GetSceneRenderer();
        return renderer ? renderer->GetEnvironment() : std::nullopt;
    }

    Desert::ECS::Entity& Scene::CreateNewEntity( std::string&& entityName )
    {
        const auto enttID = m_Registry.create();

        const ECS::Entity entity( enttID, m_Registry );

        entity.AddComponent<ECS::TagComponent>( std::move( entityName ) );
        entity.AddComponent<ECS::UUIDComponent>();
        entity.AddComponent<ECS::TransformComponent>();
        entity.AddComponent<ECS::RelationshipComponent>();

        return m_Entities.Add( entity.GetComponent<ECS::UUIDComponent>().UUID, enttID, m_Registry );
    }

    Desert::ECS::Entity& Scene::CreateEntityWithUUID( const Common::UUID& uuid, const std::string& name )
    {
        const auto enttID = m_Registry.create();

        const ECS::Entity entity( enttID, m_Registry );

        entity.AddComponent<ECS::TagComponent>( name );
        entity.AddComponent<ECS::UUIDComponent>( uuid );
        entity.AddComponent<ECS::TransformComponent>();
        entity.AddComponent<ECS::RelationshipComponent>();

        return m_Entities.Add( uuid, enttID, m_Registry );
    }

    const std::shared_ptr<Desert::Graphic::Image2D> Scene::GetFinalImage() const
    {
        return GetFinalImage( 0 );
    }

    std::shared_ptr<Desert::Graphic::Image2D> Scene::GetFinalImage( size_t viewIndex ) const
    {
        const auto* view = m_Views.At( viewIndex );
        return view ? view->Renderer->GetFinalImage() : nullptr;
    }

    void Scene::Resize( const uint32_t width, const uint32_t height ) const
    {
        ResizeView( 0, width, height );
    }

    void Scene::ResizeView( size_t viewIndex, const uint32_t width, const uint32_t height ) const
    {
        const auto* view = m_Views.At( viewIndex );
        if ( !view )
            return;

        view->Renderer->Resize( width, height );
        if ( view->Camera )
            view->Camera->UpdateProjectionMatrix( width, height );

        // The scene's own idea of "the viewport size" is VIEW 0's, and only view 0 writes it. It feeds
        // screen-space work that has one answer per document (picking rays built outside a panel); a
        // second viewport of a different size must not overwrite it, which is the middle-link defect
        // this split exists to avoid.
        if ( viewIndex == 0 )
        {
            m_ViewportWidth  = width;
            m_ViewportHeight = height;
        }
    }

    std::optional<std::reference_wrapper<const Desert::ECS::Entity>>
    Scene::FindEntityByID( const Common::UUID& uuid ) const
    {
        if ( const ECS::Entity* entity = m_Entities.Find( uuid ) ) [[likely]]
        {
            return std::cref( *entity );
        }
        else [[unlikely]]
        {
            return std::nullopt;
        }
    }

    Common::BoolResultStr Scene::Serialize( const Assets::AssetManager* assetManager,
                                            const Common::Filepath&     path ) const
    {
        SceneSerializer serializer( this, assetManager );
        return serializer.SaveToFile( path );
    }

    void Scene::RegisterExternalPass( Graphic::ExternalPassSpecification&& spec )
    {
        // Replace, don't append, on a repeated name: SceneRenderer keys its render systems by name and a
        // second registration evicts the first there, so an order form that kept both would hand a view
        // opened later a pass the live renderers no longer run.
        const std::string name = spec.Name;
        UnregisterExternalPass( name );

        m_ExternalPasses.push_back( std::move( spec ) );
        for ( auto& view : m_Views.All() )
            view.Renderer->RegisterExternalPass( Graphic::ExternalPassSpecification( m_ExternalPasses.back() ) );
    }

    void Scene::UnregisterExternalPass( const std::string& name )
    {
        m_ExternalPasses.erase( std::remove_if( m_ExternalPasses.begin(), m_ExternalPasses.end(),
                                                [&name]( const Graphic::ExternalPassSpecification& spec )
                                                { return spec.Name == name; } ),
                                m_ExternalPasses.end() );

        for ( auto& view : m_Views.All() )
            view.Renderer->UnregisterExternalPass( name );
    }

    std::optional<size_t> Scene::AddView( Graphic::SceneRenderer* renderer )
    {
        auto camera = std::static_pointer_cast<Core::Camera>( std::make_shared<EditorCamera>() );

        const auto index = m_Views.Add( renderer, camera );
        if ( !index )
        {
            LOG_ERROR( "[Scene] '{}' refused a view: the renderer is null or already a view of this scene.",
                       m_SceneName );
            return std::nullopt;
        }

        // A view opened after Init() has to be caught up by hand — its renderer has no render systems
        // yet, and it carries none of the editor passes the document installed before it existed.
        if ( m_Initialized )
        {
            renderer->Init();
            renderer->Resize( m_ViewportWidth, m_ViewportHeight );
            camera->UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
            for ( const auto& spec : m_ExternalPasses )
                renderer->RegisterExternalPass( Graphic::ExternalPassSpecification( spec ) );
        }

        LOG_INFO( "[Scene] '{}' opened view #{} ({} view(s) on this world).", m_SceneName, *index,
                  m_Views.Count() );
        return index;
    }

    bool Scene::RemoveView( const Graphic::SceneRenderer* renderer )
    {
        if ( !m_Views.Remove( renderer ) )
            return false;

        LOG_INFO( "[Scene] '{}' closed a view ({} view(s) left on this world).", m_SceneName, m_Views.Count() );
        return true;
    }

    std::shared_ptr<Core::Camera> Scene::GetViewCamera( size_t viewIndex ) const
    {
        const auto* view = m_Views.At( viewIndex );
        return view ? view->Camera : nullptr;
    }

    void Scene::SetViewCamera( size_t viewIndex, const std::shared_ptr<Core::Camera>& camera )
    {
        if ( auto* view = m_Views.At( viewIndex ) )
            view->Camera = camera;
    }

    Graphic::SceneRenderer* Scene::GetViewRenderer( size_t viewIndex ) const
    {
        const auto* view = m_Views.At( viewIndex );
        return view ? view->Renderer : nullptr;
    }

    void Scene::SetActiveCamera( const std::shared_ptr<Core::Camera>& camera )
    {
        if ( auto* view = m_Views.At( 0 ) )
            view->Camera = camera;
    }

    std::shared_ptr<Core::Camera> Scene::GetActiveCamera() const
    {
        const auto* view = m_Views.At( 0 );
        return view ? view->Camera : nullptr;
    }

    void Scene::OnEntityCreated_Camera()
    {
        // Intentionally a no-op now: a scene CameraComponent is a GAME camera, NOT the editor view. The
        // editor renders through its own EditorCamera (set via SetActiveCamera); Play mode switches to the
        // main CameraComponent via a GameplayCamera. The Play-mode lookup is UpdateActiveCameraSource's
        // own; FindMainCamera, which claimed to be that lookup, had no caller anywhere in the repository
        // and wrote the one scene-wide camera handle the view list replaced.
    }

    const std::shared_ptr<Desert::Graphic::Framebuffer> Scene::GetTargetFramebuffer() const
    {
        auto* renderer = GetSceneRenderer();
        return renderer ? renderer->GetTargetFramebuffer() : nullptr;
    }

    void Scene::Clear()
    {
        m_Registry.clear();

        m_Entities.Clear();

        SetupRegistryCallbacks();
    }

    void Scene::SetupRegistryCallbacks()
    {
        // Disconnect first — Clear() calls this on every scene reload, and entt keeps signal connections
        // across registry.clear(), so re-connecting without this accumulates duplicate handler invocations.
        m_Registry.on_construct<ECS::CameraComponent>().disconnect( this );
        m_Registry.on_construct<ECS::CameraComponent>().connect<&Scene::OnEntityCreated_Camera>( this );
    }

    void Scene::Attach( ECS::Entity parent, ECS::Entity child )
    {
        if ( !parent || !child || parent.GetHandle() == child.GetHandle() )
            return;

        // Refuse to attach an entity to its own descendant — that would make the hierarchy a cycle
        // (every parent-chain walk in the engine would spin forever).
        for ( entt::entity cur = parent.GetHandle(); cur != entt::null;
              cur = m_Registry.has<ECS::RelationshipComponent>( cur )
                        ? m_Registry.get<ECS::RelationshipComponent>( cur ).Parent
                        : entt::null )
        {
            if ( cur == child.GetHandle() )
                return;
        }

        auto& parentRel = parent.GetComponent<ECS::RelationshipComponent>();
        auto& childRel  = child.GetComponent<ECS::RelationshipComponent>();

        if ( childRel.Parent == parent.GetHandle() )
            return;
        if ( childRel.Parent != entt::null )
            Detach( child ); // reparent: without this the old parent kept a stale Children entry

        childRel.Parent = parent.GetHandle();
        parentRel.Children.push_back( child.GetHandle() );
    }

    void Scene::Detach( ECS::Entity child )
    {
        if ( !child || !child.HasComponent<ECS::RelationshipComponent>() )
            return;

        auto& childRel = child.GetComponent<ECS::RelationshipComponent>();
        if ( childRel.Parent == entt::null )
            return;

        if ( m_Registry.valid( childRel.Parent ) && m_Registry.has<ECS::RelationshipComponent>( childRel.Parent ) )
        {
            auto& siblings = m_Registry.get<ECS::RelationshipComponent>( childRel.Parent ).Children;
            siblings.erase( std::remove( siblings.begin(), siblings.end(), child.GetHandle() ),
                            siblings.end() );
        }
        childRel.Parent = entt::null;
    }

    void Scene::SetVisibleRecursive( ECS::Entity entity, bool visible )
    {
        if ( !entity ) return;

        if ( entity.HasComponent<ECS::VisibilityComponent>() )
            entity.GetComponent<ECS::VisibilityComponent>().Visible = visible;
        else
            entity.AddComponent<ECS::VisibilityComponent>().Visible = visible;

        if ( entity.HasComponent<ECS::RelationshipComponent>() )
        {
            auto& rel = entity.GetComponent<ECS::RelationshipComponent>();
            for ( auto childHandle : rel.Children )
                SetVisibleRecursive( ECS::Entity( childHandle, m_Registry ), visible );
        }
    }

    void Scene::DestroyEntity( ECS::Entity entity )
    {
        if ( !entity )
            return;
        // The algorithm lives beside the index it maintains — see SceneEntityIndex.hpp for why it is O(1)
        // per entity and what that did to the order of GetAllEntities().
        DestroyEntityTree( m_Registry, m_Entities, entity.GetHandle() );
    }

} // namespace Desert::Core