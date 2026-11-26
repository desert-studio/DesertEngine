#include <Engine/Core/Scene.hpp>
#include <Engine/Core/RenderTask.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/DebugDraw.hpp>
#include <Engine/Core/GameMode.hpp>
#include <Engine/Core/LevelTravel.hpp>
#include <UI/UIWorld.hpp>
#include <Common/Core/CrashWorlds.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Core/WeatherState.hpp>
#include <Engine/Core/WorldParameterInstance.hpp>
#include <Engine/Core/WorldTime.hpp>
#include <Engine/Core/Serialize/EntityPackages.hpp>
#include <Engine/ECS/ReflectedComponents.hpp>
#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/PostProcessVolumeComponent.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/GameplayDebugger/GameplayDebugger.hpp>
#include <Engine/Graphic/ExtensionPass.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/World/Deformation/DeformationWorld.hpp>
#include <Engine/World/Decals/DecalImpactPool.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Carry.hpp>
#include <Engine/Runtime/Factory/PrefabFactory.hpp>
#include <Engine/ECS/EditorHLODComponent.hpp>

#include <Engine/Assets/Serialization/ShaderGraph.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Core/PlayerStart.hpp>

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <Engine/ECS/AtmosphereSunLight.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/System/SystemRules.hpp>
#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Input/LocalPlayerInput.hpp>
#endif
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>

#include <algorithm>
#include <cfloat>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <typeinfo>

#include <Engine/Geometry/PosedBounds.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>
#include <Engine/Runtime/Services/WorldParameters/WorldParametersService.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/AudioSourceComponent.hpp>
#include <Engine/ECS/CameraComponent.hpp>
#include <Engine/ECS/ControlRigComponent.hpp>
#include <Engine/ECS/DecalComponent.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Serialize/EngineComponentProvider.hpp>
#include <Engine/ECS/DirectionLightComponent.hpp>
#include <Engine/ECS/InstancedStaticMeshComponent.hpp>
#include <Engine/ECS/LevelSequenceComponent.hpp>
#include <Engine/ECS/MaterialComponent.hpp>
#include <Engine/ECS/MorphComponent.hpp>
#include <Engine/ECS/PlayerStartComponent.hpp>
#include <Engine/ECS/PointLightComponent.hpp>
#include <Engine/ECS/ProceduralFoliageComponent.hpp>
#include <Engine/ECS/RagdollComponent.hpp>
#include <Engine/ECS/RectLightComponent.hpp>
#include <Engine/ECS/RelationshipComponent.hpp>
#include <Engine/ECS/RetargetComponent.hpp>
#include <Engine/ECS/SkinnedMeshComponent.hpp>
#include <Engine/ECS/SkyboxComponent.hpp>
#include <Engine/ECS/SocketAttachmentComponent.hpp>
#include <Engine/ECS/SpotLightComponent.hpp>
#include <Engine/ECS/SpringArmComponent.hpp>
#include <Engine/ECS/StaticMeshComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/TextComponent.hpp>
#include <Engine/ECS/TimeOfDayComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/ECS/TwoBoneIKComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/ECS/VisibilityComponent.hpp>
#include <Engine/ECS/WindSourceComponent.hpp>
#include <Engine/Physics/CharacterControllerComponent.hpp>
#include <Engine/Physics/ColliderComponent.hpp>
#include <Engine/Physics/RigidBodyComponent.hpp>
#include <Engine/Scripting/ScriptComponent.hpp>
#include <Engine/UI/Ecs/UIAnimComponent.hpp>
#include <Engine/World/Foliage/FoliageComponent.hpp>
#include <Engine/World/Partition/HiddenByDataLayersComponent.hpp>
#include <Engine/World/Partition/StreamingSourceComponent.hpp>
#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/ECS/HeterogeneousVolumeComponent.hpp>
#include <Engine/ECS/LocalFogVolumeComponent.hpp>

namespace Desert::Core
{
    namespace
    {
        // Resolve a static-mesh component to its renderable Mesh (handle / runtime-edited / shared primitive).
        // This used to live in the editor's ViewportPanel (engine logic that leaked into the editor) — it
        // belongs here, where Raycast and the mesh systems can share it.
        ::Desert::Mesh* ResolveMesh( const ECS::StaticMeshComponent& c )
        {
            if ( c.GetMeshHandle() )
                return Runtime::ResourceRegistry::GetMeshService()->Get( c.GetMeshHandle() );
            if ( c.GetRuntimeMesh() )
                return c.GetRuntimeMesh().get();
            if ( c.GetPrimitive().has_value() )
                return Geometry::PrimitiveMeshFactory::GetShared( c.GetPrimitive().value() );
            return nullptr;
        }

        // A static mesh's CPU triangles, from the same source ResolveMesh draws (handle / runtime-edited /
        // primitive): the parsed asset keeps its arrays while it is resident. nullopt when none is readable.
        struct MeshTriangles
        {
            std::span<const Vertex>  Vertices;
            std::span<const Index>   Indices;
            std::span<const Submesh> Submeshes;
        };
        std::optional<MeshTriangles> StaticTriangles( const ECS::StaticMeshComponent& c )
        {
            if ( c.GetMeshHandle() )
            {
                const auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( c.GetMeshHandle() );
                const auto* data  = dynamic_cast<const Assets::StaticMeshAsset*>( asset );
                if ( !data )
                    return std::nullopt;
                return MeshTriangles{ data->GetVertices(), data->GetIndices(), data->GetSubmeshes() };
            }
            const DynamicMesh* mesh = c.GetRuntimeMesh() ? c.GetRuntimeMesh().get()
                                      : c.GetPrimitive().has_value()
                                           ? Geometry::PrimitiveMeshFactory::GetShared( c.GetPrimitive().value() )
                                           : nullptr;
            if ( !mesh )
                return std::nullopt;
            return MeshTriangles{ mesh->GetVertices(), mesh->GetIndices(), mesh->GetSubmeshes() };
        }

        // Moller-Trumbore, both sides: the ray parameter of a hit in front of the origin.
        bool RayTriangle( const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b,
                          const glm::vec3& c, float& outT )
        {
            const glm::vec3 e1  = b - a;
            const glm::vec3 e2  = c - a;
            const glm::vec3 p   = glm::cross( d, e2 );
            const float     det = glm::dot( e1, p );
            if ( std::abs( det ) < 1e-12f )
                return false;
            const float     inv = 1.0f / det;
            const glm::vec3 tv  = o - a;
            const float     u   = glm::dot( tv, p ) * inv;
            if ( u < 0.0f || u > 1.0f )
                return false;
            const glm::vec3 q = glm::cross( tv, e1 );
            const float     v = glm::dot( d, q ) * inv;
            if ( v < 0.0f || u + v > 1.0f )
                return false;
            outT = glm::dot( e2, q ) * inv;
            return outT > 0.0f;
        }

        // Editor-built runtime rig takes priority over the cooked asset (mirrors the render path).
        ::Desert::Mesh* ResolveSkinnedMesh( const ECS::SkinnedMeshComponent& c )
        {
            if ( c.GetRuntimeMesh() )
                return c.GetRuntimeMesh().get();
            if ( c.GetMeshHandle() )
                return Runtime::ResourceRegistry::GetMeshService()->Get( c.GetMeshHandle() );
            return nullptr;
        }

        // Mesh-local AABB of a skinned mesh deformed by `skin`: the stored submesh box is raw-vertex space, so
        // picking measures the posed vertices (Geometry::MeasurePosedVertices, shared with the pose thumbnail).
        Common::Math::AABB SkinnedLocalBounds( const SkinnedMesh& mesh, const std::vector<glm::mat4>& skin )
        {
            const auto box = Geometry::MeasurePosedVertices( mesh.GetVertices(), skin );
            return { box.Min, box.Max };
        }
    } // namespace

    bool Scene::Raycast( const Common::Math::Ray& ray, RaycastHit& outHit ) const
    {
        return Raycast( ray, outHit, {} );
    }

    // Every module's surfaces as they are now (ModuleHooks().RaySurfaces). A module refreshing its table
    // (the landscape's drawn tiles) updates a cache, not the scene: the const_cast is the registry-ctx
    // equivalent of a `mutable` cache.
    Scene::RaycastSurfaceSet Scene::GatherRaycastSurfaces() const
    {
        RaycastSurfaceSet set;
        for ( const RaySurfaceGather gather : ModuleHooks().RaySurfaces )
            if ( RaySurfaceTrace trace = gather( const_cast<entt::registry&>( m_Registry ) ) )
                set.Surfaces.push_back( std::move( trace ) );
        return set;
    }

    bool Scene::Raycast( const Common::Math::Ray& ray, RaycastHit& outHit,
                         const std::function<bool( const Common::UUID& )>& accept ) const
    {
        return Raycast( ray, outHit, accept, GatherRaycastSurfaces() );
    }

    bool Scene::Raycast( const Common::Math::Ray& ray, RaycastHit& outHit,
                         const std::function<bool( const Common::UUID& )>& accept,
                         const RaycastSurfaceSet&                          surfaces ) const
    {
        return Trace( ray, outHit, accept, surfaces, false );
    }

    bool Scene::RaycastComplex( const Common::Math::Ray& ray, RaycastHit& outHit,
                                const std::function<bool( const Common::UUID& )>& accept ) const
    {
        return Trace( ray, outHit, accept, GatherRaycastSurfaces(), true );
    }

    bool Scene::RaycastComplex( const Common::Math::Ray& ray, RaycastHit& outHit,
                                const std::function<bool( const Common::UUID& )>& accept,
                                const RaycastSurfaceSet&                          surfaces ) const
    {
        return Trace( ray, outHit, accept, surfaces, true );
    }

    bool Scene::Trace( const Common::Math::Ray& ray, RaycastHit& outHit,
                       const std::function<bool( const Common::UUID& )>& accept, const RaycastSurfaceSet& surfaces,
                       bool complex ) const
    {
        float              closest = std::numeric_limits<float>::max();
        glm::mat4          bestXf( 1.0f );
        Common::Math::AABB bestAABB;
        Common::Math::Ray  bestLocal  = ray;
        float              bestLocalT = 0.0f;
        Common::UUID       bestUUID;
        std::optional<glm::vec3> bestFace; // the crossed triangle's normal (complex trace), world space
        bool               hit = false;

        // A hit on a box (a static mesh's submesh box, a skinned mesh's posed bounds), nearer than the best.
        const auto boxHit = [&]( const ECS::Entity& entity, const glm::mat4& xf, const Common::Math::Ray& localRay,
                                 const Common::Math::AABB& box )
        {
            float t = 0.0f;
            if ( !localRay.IntersectsAABB( box, t ) || t <= 0.0f )
                return;
            // Compared in WORLD units: a local t is in the entity's own (scaled) units.
            if ( const float d = ray.WorldDistanceOf( localRay, t, xf ); d > 0.0f && d < closest )
            {
                closest    = d;
                bestXf     = xf;
                bestAABB   = box;
                bestLocal  = localRay;
                bestLocalT = t;
                bestUUID   = entity.GetComponent<ECS::UUIDComponent>().UUID;
                bestFace.reset();
                hit        = true;
            }
        };
        // The world distance at which the ray enters @p box (0 when it starts inside); nullopt when it misses.
        const auto boxEntry = [&]( const glm::mat4& xf, const Common::Math::Ray& localRay,
                                   const Common::Math::AABB& box ) -> std::optional<float>
        {
            float t = 0.0f;
            if ( !localRay.IntersectsAABB( box, t ) )
                return std::nullopt;
            return std::max( ray.WorldDistanceOf( localRay, std::max( t, 0.0f ), xf ), 0.0f );
        };

        // The complex trace's broad phase (UE: the scene query's bounds before a mesh's triangles): every static
        // mesh whose submesh boxes the ray enters is a candidate, met on its triangles AFTER the boxes, nearest
        // entry first, and none whose boxes the ray enters beyond the nearest hit found so far. Reading a mesh's
        // triangles is an explicit CPU-access request that keeps its arrays resident (StaticMeshAsset::
        // GetVertices), so only a mesh the ray can still meet first is asked: a foliage dab, the hover ring of
        // every frame, does not make the scene's meshes resident nor walk their triangles.
        struct TriangleCandidate
        {
            const ECS::Entity* Entity = nullptr;
            glm::mat4          Xf     = glm::mat4( 1.0f );
            Common::Math::Ray  Local;
            float              Entry  = 0.0f;
        };
        std::vector<TriangleCandidate> candidates;

        for ( const auto& entity : GetAllEntities() )
        {
            // AN EDITOR HLOD IS NOT THE WORLD'S (ECS/EditorHLODComponent.hpp; UE's editor HLODs are neither
            // selectable nor traced): no trace meets one, drawn or hidden - a hidden one stands exactly where the
            // held cell's own entities stand, and would take their clicks and the foliage brush's hits.
            if ( entity.HasComponent<ECS::EditorHLODComponent>() )
                continue;
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
            if ( accept && !accept( entity.GetComponent<ECS::UUIDComponent>().UUID ) )
                continue;

            const glm::mat4 xf       = entity.GetWorldTransform();
            const auto      localRay = ray.ToLocalSpace( xf );

            if ( complex && !skinned )
            {
                std::optional<float> entry;
                for ( const auto& sm : mesh->GetSubmeshes() )
                    if ( const auto at = boxEntry( xf, localRay, sm.BoundingBox ); at && ( !entry || *at < *entry ) )
                        entry = at;
                if ( entry )
                    candidates.push_back( TriangleCandidate{ &entity, xf, localRay, *entry } );
            }
            else if ( !skinned )
            {
                for ( const auto& sm : mesh->GetSubmeshes() )
                    boxHit( entity, xf, localRay, sm.BoundingBox );
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
                boxHit( entity, xf, localRay, SkinnedLocalBounds( *sk, skin ) );
            }
        }

        std::sort( candidates.begin(), candidates.end(),
                   []( const TriangleCandidate& a, const TriangleCandidate& b ) { return a.Entry < b.Entry; } );
        for ( const auto& candidate : candidates )
        {
            if ( candidate.Entry > closest )
                break; // every later mesh is entered beyond the nearest hit
            const ECS::Entity&       entity   = *candidate.Entity;
            const glm::mat4&         xf       = candidate.Xf;
            const Common::Math::Ray& localRay = candidate.Local;
            const auto triangles = StaticTriangles( entity.GetComponent<ECS::StaticMeshComponent>() );
            if ( !triangles )
            {
                // No readable CPU triangles (StaticTriangles names when): the mesh is met on its boxes.
                for ( const auto& sm : ResolveMesh( entity.GetComponent<ECS::StaticMeshComponent>() )->GetSubmeshes() )
                    boxHit( entity, xf, localRay, sm.BoundingBox );
                continue;
            }
            // Every submesh the ray enters before the nearest hit (from inside too: a camera within a room's box
            // still meets its walls), then its triangles, in the entity's own space.
            for ( const auto& sm : triangles->Submeshes )
            {
                if ( const auto at = boxEntry( xf, localRay, sm.BoundingBox ); !at || *at > closest )
                    continue;
                const uint32_t first = sm.IndexOffset / 3;
                const uint32_t count = sm.IndexCount / 3;
                if ( static_cast<uint64_t>( first ) + count > triangles->Indices.size() )
                    continue;
                for ( uint32_t k = first; k < first + count; ++k )
                {
                    const auto&    tri = triangles->Indices[k];
                    const uint64_t a   = static_cast<uint64_t>( sm.VertexOffset ) + tri.V1;
                    const uint64_t b   = static_cast<uint64_t>( sm.VertexOffset ) + tri.V2;
                    const uint64_t c   = static_cast<uint64_t>( sm.VertexOffset ) + tri.V3;
                    if ( std::max( { a, b, c } ) >= triangles->Vertices.size() )
                        continue;
                    const glm::vec3& pa = triangles->Vertices[a].Position;
                    const glm::vec3& pb = triangles->Vertices[b].Position;
                    const glm::vec3& pc = triangles->Vertices[c].Position;
                    float            tt = 0.0f;
                    if ( !RayTriangle( localRay.Origin, localRay.Direction, pa, pb, pc, tt ) )
                        continue;
                    if ( const float d = ray.WorldDistanceOf( localRay, tt, xf ); d > 0.0f && d < closest )
                    {
                        // A normal is carried by the inverse transpose (a non-uniform scale tilts it).
                        glm::vec3 n =
                             glm::transpose( glm::inverse( glm::mat3( xf ) ) ) * glm::cross( pb - pa, pc - pa );
                        if ( glm::dot( n, ray.Direction ) > 0.0f )
                            n = -n;
                        closest  = d;
                        bestUUID = entity.GetComponent<ECS::UUIDComponent>().UUID;
                        bestFace = glm::normalize( n );
                        hit      = true;
                    }
                }
            }
        }

        // The module surfaces after the meshes, against the nearest distance so far: a rock standing on a hill is
        // picked when the ray meets the rock first, the hill otherwise.
        bool surfaceHit = false;
        for ( const RaySurfaceTrace& trace : surfaces.Surfaces )
        {
            const float limit = hit || surfaceHit ? closest : std::numeric_limits<float>::max();
            if ( const auto met = trace( ray, limit, accept ); met && met->Distance < limit )
            {
                outHit.Hit      = true;
                outHit.Entity   = met->Entity;
                outHit.Point    = met->Point;
                outHit.Normal   = met->Normal;
                outHit.Distance = met->Distance;
                closest         = met->Distance;
                surfaceHit      = true;
            }
        }
        if ( surfaceHit )
            return true;

        outHit.Hit = hit;
        if ( !hit )
            return false;

        outHit.Entity   = bestUUID;
        outHit.Distance = closest;
        outHit.Point    = ray.GetPoint( closest );
        if ( bestFace )
        {
            outHit.Normal = *bestFace;
            return true;
        }

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

    struct Scene::FileState
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> AssetHeader;
        std::optional<Common::Json::TextDocument>                 LoadedDocument;
        std::optional<WorldPartitionSerialized>                   WorldPartition;
        std::string                                               Path; // see GetFilePath
        Common::Crash::CrashWorld                                 Crash;
    };

    struct Scene::FrameRendererInfos
    {
        std::array<Graphic::SceneRenderer::UpdateInfo, 2> Slots; // index-matched to m_SystemCommandArenas
    };

    const std::vector<Scene*>& Scene::LiveScenes()
    {
        return LiveSceneList();
    }

    Scene::Scene( std::string sceneName, Graphic::SceneRenderer* sceneRenderer, WorldKind kind )
         : m_Kind( kind ), m_ExtensionPasses( std::make_unique<Graphic::ExtensionPassRegistry>() ),
           m_DebugDraw( std::make_unique<DebugDrawWorld>() ),
#if DESERT_DEV_INSTRUMENTS
           m_GameplayDebugger( std::make_unique<Debug::GameplayDebugger>() ),
#endif
           m_WorldTime( std::make_unique<WorldTime>() ),
           m_WorldParameters( std::make_unique<WorldParameterInstance>() ),
           m_Deformation( std::make_unique<World::DeformationWorld>() ),
           m_DecalImpacts( std::make_unique<World::DecalImpactPool>() ),
           m_Weather( std::make_unique<WeatherState>() ), m_GameMode( std::make_unique<GameMode>() ),
           m_FrameRendererInfos( std::make_unique<FrameRendererInfos>() ),
           m_Settings( std::make_unique<SceneSettings>() ), m_SceneName( std::move( sceneName ) ),
           m_Packages( std::make_shared<EntityPackages>() ), m_File( std::make_unique<FileState>() ),
           m_Travel( std::make_unique<LevelTravel>() )
    {
        m_Registry.set<UI::UIWorld>();
        // The worlds a crash report names: the played ones. A preview or a bake is the tool's, not the user's.
        if ( m_Kind == WorldKind::Game || m_Kind == WorldKind::Document )
            m_File->Crash = Common::Crash::CrashWorld( m_Kind == WorldKind::Game ? "game" : "document" );
        ReportToCrashHandler();

        // Engine's component serializers (Core's registry): this reference links the provider in.
        Serialize::LinkEngineComponentProvider();
        StartupEngineModules();
        if ( const SceneRenderStateMaker makeRenderState = ModuleHooks().SceneRenderState )
            m_RenderState = makeRenderState( m_LedgerInstance );
        for ( const SceneModuleStateMaker make : ModuleHooks().SceneStates )
            m_ModuleStates.push_back( make( m_Registry ) );
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

        m_Subsystems.Begin();
    }

    void Scene::SetState( SceneState state )
    {
        if ( m_State == SceneState::Edit && state == SceneState::Play )
        {
            m_WorldTime->Reset();
            for ( const auto& moduleState : m_ModuleStates )
                moduleState->Reset();
        }
        else if ( m_State != SceneState::Edit && state == SceneState::Edit )
            for ( const auto& moduleState : m_ModuleStates )
                moduleState->Reset();
        m_State = state;
        if ( state == SceneState::Edit )
            m_Simulating = false; // a simulation ends with the session it ran in
        m_Subsystems.SetPlaying( state != SceneState::Edit );
        if ( state != SceneState::Paused )
        {
            m_SingleFramePending = false; // a step belongs to the pause it was asked in
        }
        ReportToCrashHandler();
    }

    void Scene::ReportToCrashHandler() noexcept
    {
        m_File->Crash.Update( m_File->Path, m_State == SceneState::Play     ? "play"
                                            : m_State == SceneState::Paused ? "paused"
                                                                            : "edit" );
    }

    const std::string& Scene::GetFilePath() const
    {
        return m_File->Path;
    }

    void Scene::SetFilePath( std::string path )
    {
        m_File->Path = std::move( path );
        ReportToCrashHandler();
    }

    LevelTravel& Scene::Travel()
    {
        return *m_Travel;
    }

    UI::UIWorld& Scene::GetUIWorld()
    {
        return m_Registry.ctx<UI::UIWorld>();
    }

    const std::optional<Common::Content::TextAssetHeaderSerialized>& Scene::GetAssetHeader() const
    {
        return m_File->AssetHeader;
    }

    void Scene::SetAssetHeader( std::optional<Common::Content::TextAssetHeaderSerialized> header )
    {
        m_File->AssetHeader = std::move( header );
    }

    const std::optional<Common::Json::TextDocument>& Scene::GetLoadedDocument() const
    {
        return m_File->LoadedDocument;
    }

    void Scene::SetLoadedDocument( Common::Json::TextDocument document )
    {
        m_File->LoadedDocument = std::move( document );
    }

    const std::optional<WorldPartitionSerialized>& Scene::GetWorldPartition() const
    {
        return m_File->WorldPartition;
    }

    void Scene::SetWorldPartition( std::optional<WorldPartitionSerialized> partition )
    {
        m_File->WorldPartition = std::move( partition );
    }

    void Scene::MarkModified( Common::UUID id ) const
    {
        m_Packages->MarkModified( id );
    }

    void Scene::MarkModifiedAll() const
    {
        m_Packages->TouchAll();
    }

    Scene::~Scene()
    {
        // A job of this world's views may still be recording (RenderTask): it reads this scene's buffers.
        ::Desert::Engine::RenderTask::Get().Join();
        m_Subsystems.End();

        // Erased in the destructor and nowhere else, so an entry cannot outlive the object it points at.
        // That is the whole reason the list holds raw pointers rather than weak_ptrs: a Scene is in it for
        // exactly its own lifetime and there is no window in which a reader could see a dead one.
        auto& scenes = LiveSceneList();
        scenes.erase( std::remove( scenes.begin(), scenes.end(), this ), scenes.end() );
    }

    NO_DISCARD Common::BoolResultStr Scene::Init( InitPurpose purpose )
    {
        // A world nobody made systems for would draw an empty frame with no error anywhere.
        if ( m_Systems.empty() )
            return Common::MakeError<bool>(
                 std::format( "world '{}' has no systems: CreateWorldSystems was not called", m_SceneName ) );

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
        if ( purpose == InitPurpose::World )
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

    Common::ResultStr<entt::entity> Scene::ResolveViewTarget()
    {
        m_ViewTarget = entt::null;
        // A camera driven from OUTSIDE (a headless shot's pinned view, a preview) is the view already;
        // Play must not refuse over a question nobody is asking.
        if ( m_CameraPinned )
            return Common::MakeSuccess( entt::entity( entt::null ) );

        // (1) The pawn's own camera: root first, then its subtree depth-first in child order.
        entt::entity pawnCamera = entt::null;
        const entt::entity playerPawn = GetPlayerPawn();
        if ( playerPawn != entt::null && m_Registry.valid( playerPawn ) )
        {
            std::vector<entt::entity> stack{ playerPawn };
            while ( !stack.empty() && pawnCamera == entt::null )
            {
                const entt::entity e = stack.back();
                stack.pop_back();
                if ( m_Registry.has<ECS::CameraComponent, ECS::TransformComponent>( e ) )
                    pawnCamera = e;
                else if ( const auto* rel = m_Registry.try_get<ECS::RelationshipComponent>( e ) )
                    stack.insert( stack.end(), rel->Children.rbegin(), rel->Children.rend() );
            }
        }

        std::vector<entt::entity> autoCams;
        std::vector<std::string>  autoNames;
        for ( const auto e : m_Registry.view<ECS::CameraComponent, ECS::TransformComponent>() )
        {
            if ( !m_Registry.get<ECS::CameraComponent>( e ).Data.AutoActivateForPlayer )
                continue;
            autoCams.push_back( e );
            const auto* tag = m_Registry.try_get<ECS::TagComponent>( e );
            autoNames.push_back( tag != nullptr ? tag->Tag : std::string( "<unnamed>" ) );
        }

        const auto choice = ChooseViewTarget( { .Level               = m_SceneName,
                                                .PawnSpawned         = playerPawn != entt::null,
                                                .PawnHasCamera       = pawnCamera != entt::null,
                                                .AutoActivateCameras = autoNames,
                                                .PlayFromHere        = m_PlayFromHere } );
        if ( !choice )
            return Common::MakeError<entt::entity>( choice.GetError() );
        switch ( choice.GetValue().Kind )
        {
            case ViewTargetKind::PawnCamera:
                m_ViewTarget = pawnCamera;
                break;
            case ViewTargetKind::AutoActivateCamera:
                m_ViewTarget = autoCams[choice.GetValue().AutoIndex];
                break;
            case ViewTargetKind::EditorCamera:
                m_ViewTarget = entt::null;
                break;
        }
        return Common::MakeSuccess( entt::entity( m_ViewTarget ) );
    }

    std::optional<glm::mat4> Scene::PlayerViewPoint()
    {
        if ( m_State != SceneState::Play && m_State != SceneState::Paused )
            return std::nullopt;
        if ( m_ViewTarget != entt::null && m_Registry.valid( m_ViewTarget ) &&
             m_Registry.has<ECS::TransformComponent>( m_ViewTarget ) )
            return ECS::Entity( m_ViewTarget, m_Registry ).GetWorldTransform();
        const std::shared_ptr<Camera> camera = GetActiveCamera();
        if ( !camera )
            return std::nullopt;
        return glm::inverse( camera->GetViewMatrix() );
    }

    void Scene::UpdateActiveCameraSource()
    {
        // Camera source follows the play state: Edit/Paused -> EditorCamera; Play -> the view target
        // ResolveViewTarget chose at BeginPlay (driven into a GameplayCamera each frame so moving the
        // camera entity moves the view), or the editor camera when Play from Here resolved to it.
        if ( m_State == SceneState::Play )
        {
            const entt::entity mainEntity =
                 ( m_ViewTarget != entt::null && m_Registry.valid( m_ViewTarget ) &&
                   m_Registry.has<ECS::CameraComponent, ECS::TransformComponent>( m_ViewTarget ) )
                      ? m_ViewTarget
                      : entt::entity( entt::null );
            const ECS::CameraComponent* mainCam =
                 mainEntity != entt::null ? &m_Registry.get<ECS::CameraComponent>( mainEntity ) : nullptr;

            if ( mainCam && mainEntity != entt::null )
            {
                // WORLD transform of the camera entity (the cached one), so a camera parented to a moving
                // entity (e.g. a child of the character controller) follows it — 1st/3rd person is just the
                // child's local offset.
                const glm::mat4 world = m_Registry.get<ECS::TransformComponent>( mainEntity ).GetWorldTransform();
                if ( !m_GameplayCamera )
                    m_GameplayCamera = std::make_shared<GameplayCamera>();
                static_cast<GameplayCamera*>( m_GameplayCamera.get() )
                     ->SetView(
                          CameraEntityViewOf( world, mainCam->Data.FOV, mainCam->Data.Near, mainCam->Data.Far ),
                          m_ViewportWidth, m_ViewportHeight, static_cast<uint32_t>( mainEntity ) );
                if ( GetActiveCamera() != m_GameplayCamera )
                    SetActiveCamera( m_GameplayCamera );
            }
            else if ( GetActiveCamera() != m_EditorCamera )
            {
                SetActiveCamera( m_EditorCamera ); // Play from Here without a game camera
            }
        }
        else if ( m_EditorCamera && GetActiveCamera() != m_EditorCamera )
        {
            SetActiveCamera( m_EditorCamera );
        }
    }

    Common::BoolResultStr Scene::SyncWorldParameters()
    {
        const Common::AssetHandle named = m_Settings->WorldParameters;
        if ( named == Common::AssetHandle{} )
        {
            m_WorldParameters->Adopt( named, nullptr );
            return BOOLSUCCESS;
        }
        const auto data = Runtime::ResourceRegistry::GetWorldParametersService()->Get( named );
        if ( !data )
        {
            m_WorldParameters->Adopt( Common::AssetHandle{}, nullptr );
            return Common::MakeFormattedError<bool>( "the scene's world parameters {}: {}",
                                                     static_cast<uint64_t>( named ), data.GetError() );
        }
        // A Texture row names its .detex by path; the world holds the handle, each view its bindless slot.
        std::string unresolved;
        m_WorldParameters->Adopt(
             named, data.GetValue(),
             [&unresolved]( const std::string& path )
             {
                 const auto handle = Runtime::ResourceRegistry::GetTextureService()->HandleForPath( path );
                 if ( !handle )
                 {
                     unresolved = handle.GetError();
                     return Common::AssetHandle{};
                 }
                 return Common::AssetHandle( handle.GetValue() );
             },
             // A SurfaceLayer row names its .dgraph by content path: the layer's name and the
             // textures it samples come from the graph itself, so the collection names the
             // layer and nothing about its look.
             [&unresolved]( const std::string& path )
             {
                 const auto text =
                      Common::Utils::FileSystem::ReadFileContent( Common::Constants::Path::ASSETS_PATH / path );
                 if ( !text )
                 {
                     unresolved = std::format( "surface layer '{}': {}", path, text.GetError() );
                     return Core::SurfaceLayerSource{};
                 }
                 auto doc = Assets::Serialization::ShaderGraph::ParseShaderGraph( text.GetValue() );
                 if ( !doc )
                 {
                     unresolved = std::format( "surface layer '{}': {}", path, doc.GetError() );
                     return Core::SurfaceLayerSource{};
                 }
                 if ( doc.GetValue().DomainEnum() != Assets::Serialization::ShaderGraph::Domain::SurfaceLayer )
                 {
                     unresolved = std::format( "'{}' is not a Surface Layer graph", path );
                     return Core::SurfaceLayerSource{};
                 }
                 return Core::SurfaceLayerSource{
                      doc.GetValue().Name,
                      Assets::Serialization::ShaderGraph::SurfaceLayerTextures( doc.GetValue() ) };
             } );
        if ( !unresolved.empty() )
            return Common::MakeFormattedError<bool>( "the scene's world parameters {}: {}",
                                                     static_cast<uint64_t>( named ), unresolved );
        return BOOLSUCCESS;
    }

    NO_DISCARD Common::BoolResultStr Scene::OnUpdate( const Common::Timestep& ts )
    {
        // THE TICK RUNS PAST THIS WORLD'S VIEWS OF THE PREVIOUS FRAME (UE: the game thread ticks N+1 while the
        // rendering thread draws N). Every job this update enqueues carries this world as its owner; another
        // world's views still recording are joined here, this world's by the first view's bracket below
        // (SceneRenderer::BeginScene; the surface cache, retire queues and captures are handed over,
        // CPU-ONE-SYNC). What the tick writes and a view job reads is guarded at its writer (Scene.hpp
        // m_SystemCommandArenas: the other arena; the deformation log and the rest: by value at BeginScene).
        const ::Desert::Engine::RenderTask::ScopedOwner owner( this );
        ::Desert::Engine::RenderTask::Get().JoinUnlessOwnedBy( this );

        // The world's parameters before anything reads them: the scripts below set them, every view after them
        // snapshots them. The service logged a refusal once; the instance then holds none.
        {
            DESERT_PROFILE_SCOPE( "Scene: SyncWorldParameters" );
            (void)SyncWorldParameters();
        }

        // THE RENDER-STATE FRAME BOUNDARY: what the previous frame (its systems, its editor edits, its views)
        // dirtied is latched for scene.renderDirty and the next frame starts from an empty set.
        {
            std::lock_guard lock( m_RenderStateDirtyMutex );
            m_RenderStateDirtyLastFrame = m_RenderStateDirty.Size();
            m_RenderStateDirty.Clear();
        }
        m_RenderStateDelta.FrameBoundary();

        // PACKED PREFAB INSTANCES follow their root's world and their prefab's packed form (UE: a packed level
        // actor's ISMs move with the actor and are rebuilt with the source level). Before anything reads the
        // instances, after the boundary, so the re-placed ISMs are this frame's render-state changes.
        {
            DESERT_PROFILE_SCOPE( "Scene: Packed prefab instances" );
            ::Desert::Runtime::Factory::PrefabFactory::RefreshPackedInstances( *this );
        }

        // A pinned camera is driven from OUTSIDE the scene (the Details preview orbits its own), so the
        // play-state rule must not hand the view back to the EditorCamera behind its back — that is a
        // per-frame overwrite, and it is why the preview rendered through the input-driven editor camera
        // one frame after being told not to.
        if ( !m_CameraPinned )
        {
            DESERT_PROFILE_SCOPE( "Scene: Active camera source" );
            UpdateActiveCameraSource();
        }

        // The first view that refused, reported after every other view has had its frame: one broken
        // viewport must not silently cost the others theirs.
        std::string firstError;

        // THE WORLD'S CLOCK TICKS FIRST, once, before anything reads it: the systems below, and every
        // view's renderer after them, see the same Delta and GameTime this frame.
        m_WorldTime->Tick( ts.GetSeconds(), WorldTime::ClockModeFor(
                                                TicksGameplay(), m_State == SceneState::Paused && !TicksGameplay(),
                                                m_PreviewRealtime ) );

        // The renderers step by the world's Delta; the wall-clock step goes beside it for what is about
        // the machine rather than the world (the sky's re-bake debounce).
        // The slot of the arena this update records into (flipped below, "ECS Systems"; m_FrameRendererInfos):
        // reset to its defaults, its light list's capacity kept. The views and the captures read it by reference.
        const std::size_t                   frameSlot = ( m_RecordingArena + 1 ) % m_SystemCommandArenas.size();
        Graphic::SceneRenderer::UpdateInfo& sceneRendererInfo = m_FrameRendererInfos->Slots[frameSlot];
        {
            auto lights = std::move( sceneRendererInfo.DirLights.DirectionLights );
            lights.clear();
            sceneRendererInfo                           = Graphic::SceneRenderer::UpdateInfo{};
            sceneRendererInfo.DirLights.DirectionLights = std::move( lights );
        }
        sceneRendererInfo.Timestep     = Common::Timestep( m_WorldTime->GetDeltaSeconds() );
        sceneRendererInfo.RealTimestep = ts;

        // Gameplay systems (animation, physics, scripts) advance only when the world TICKS GAMEPLAY — Play,
        // or the one stepped frame of a pause (TicksGameplay) — and by the world's Delta, so a pause or a
        // time dilation reaches them from the same clock as the picture. Systems still RUN every frame (they
        // collect render data); they see a zero step otherwise. The world's preview time reaches its
        // consumers through SetWorldTime below; the editor-world animation preview through SetEditorTick.
        const Common::Timestep gameplayTs( TicksGameplay() ? m_WorldTime->GetDeltaSeconds() : 0.0f );

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
        for ( auto& system : m_Systems )
            system->SetWorldTime( *m_WorldTime );

        // THE EDITOR WORLD'S PREVIEW STEP, separate from gameplay's (WorldTime::EditorPreviewSeconds). A system
        // previews in the editor only on the author's request (UE bUpdateAnimationInEditor), so it needs to know
        // which world this is — a zero gameplay timestep alone cannot tell Edit from Paused. With Realtime off
        // the character holds still with the wind, the particles and the material Time.
        const Common::Timestep editorTs( m_WorldTime->EditorPreviewSeconds( m_State == SceneState::Edit ) );
        for ( auto& system : m_Systems )
            system->SetEditorTick( editorTs );

        // The modules' world state before the systems (ModuleHookLists::PreSystems): prefab foliage's instances
        // are entities, and the systems below must see them where the field's transforms say they stand this
        // frame — after a stroke, an undo or a cell streaming in. Main thread: it creates and destroys entities.
        {
            DESERT_PROFILE_SCOPE( "Module pre-systems" );
            for ( const ScenePreSystems preSystems : ModuleHooks().PreSystems )
                preSystems( *this );
        }

        // THE WEATHER before the systems read the fog, the clouds and the wind: advanced by gameplay time (a
        // script's Weather.set lands next frame's tick), and written at the active camera. In Edit the authored
        // values show — the weather is world state, and the editor world does not run it.
        if ( TicksGameplay() )
        {
            DESERT_PROFILE_SCOPE( "Weather" );
            m_Weather->Tick( m_Registry, gameplayTs.GetSeconds() );
            if ( const auto observer = GetActiveCamera() )
                m_Weather->Apply( m_Registry, *m_WorldParameters, observer->GetPosition() );
        }
        // THE DEBUG DRAWING AGES before the systems draw this frame's (scripts run in them): last frame's
        // one-frame items go unless the world is paused, timed items count down by the scene's time - the editor's
        // step in Edit, gameplay's in Play, nothing while paused - as the effects clock below.
        {
            DESERT_PROFILE_SCOPE( "Scene: DebugDraw Tick" );
            m_DebugDraw->Tick( m_State == SceneState::Edit ? ts.GetSeconds() : gameplayTs.GetSeconds(),
                              m_State != SceneState::Paused || TicksGameplay() );
        }

        {
            DESERT_PROFILE_SCOPE( "ECS Systems" );
            // THE OTHER ARENA (m_SystemCommandArenas): the previous update's views may still replay from theirs.
            m_RecordingArena = frameSlot;
            ExecuteSystems( gameplayTs );
        }

#if DESERT_DEV_INSTRUMENTS
        // THE GAMEPLAY DEBUGGER after the systems: the scripts' switches and the player's keys of this frame (the
        // script system ticked the local player's input) are in, and what it draws is one-frame DebugDraw, aged
        // with the rest at the top of the next update. Keys only while played: in Edit the player has no input.
        {
            DESERT_PROFILE_SCOPE( "Gameplay Debugger" );
            const Input::LocalPlayerInput* playerInput = nullptr;
            if ( m_State != SceneState::Edit )
                if ( auto* scripts = FindSystem<Core::ScriptHostSystem>() )
                    playerInput = &scripts->Host().PlayerInput();
            m_GameplayDebugger->Tick( *this, playerInput,
                                     m_State == SceneState::Edit ? ts.GetSeconds() : gameplayTs.GetSeconds(),
                                     m_State != SceneState::Paused || TicksGameplay() );
        }
#endif

        // THE MODULES' PER-SCENE STATE (the VFX module's effects clock among them), once per scene update and
        // never per view: every SceneRenderer of this scene reads the plan made here. Its time is the scene's -
        // the editor's delta in Edit (effects preview while authoring, as Niagara does), the gameplay delta in
        // Play, nothing while paused - and never the wall clock, so a `--play` run is the same frames every time.
        {
            DESERT_PROFILE_SCOPE( "Scene: Module States" );
            const Common::Timestep moduleTs = m_State == SceneState::Edit ? ts : gameplayTs;
            for ( const auto& moduleState : m_ModuleStates )
                moduleState->Tick( *this, m_Registry, static_cast<double>( moduleTs.GetSeconds() ) );
        }

        // Dir lights
        {
            DESERT_PROFILE_SCOPE( "Scene: DirLights" );

            // THE SUN, AND ONLY THE SUN. The engine renders exactly one directional light
            // (DirectionLightsUB is a single struct), and that light is the one the sky stands on:
            // ECS::PickAtmosphereSun is the rule the sky collector, the shadow settings and the clock all
            // call. This loop used to push EVERY light in group order and keep the first, so with two lights
            // the ground was lit and shadowed by one while the atmosphere, the fog's lobe and the clouds
            // took the other.
            const ECS::AtmosphereSunPick pick = ECS::PickAtmosphereSun( m_Registry, ECS::SunCandidacy::Lit );
            if ( const auto sun = pick.Sun() )
            {
                const auto&     light  = m_Registry.get<ECS::DirectionLightComponent>( *sun );
                const glm::vec3 travel = ECS::DirectionalLightTravel( m_Registry, *sun );
                sceneRendererInfo.SunVolumetricScatteringIntensity = light.Data.VolumetricScatteringIntensity;
                // w = the sine of the disc's angular radius (UE SourceAngle is its full diameter): the sun's
                // specular is the disc's (Mesh/LightSources.glslh DesertSunLight).
                sceneRendererInfo.DirLights.DirectionLights.push_back(
                     { glm::vec4( travel, std::sin( glm::radians( light.Data.SourceAngle ) * 0.5f ) ),
                       glm::vec4( light.Data.Color, light.Data.Intensity ) } );
            }

            // ATMOSPHERE LIGHT 1 (UE's Atmosphere Sun Light Index 1, the moon): lit by the deferred pass beside
            // the sun, never a stand-in for it — it has no fallback. When it casts shadows it gets cascades of its
            // own (SceneResources slot 1, MeshRenderer::UpdateCascades), fitted from these same fields.
            static_assert( static_cast<int>( Graphic::SceneResources::kMaxShadowedDirectionalLights ) ==
                                ECS::Rules::kAtmosphereLightCount,
                           "a cascade slot per atmosphere light: the slot IS the Atmosphere Sun Light Index" );
            const ECS::AtmosphereSunPick secondPick =
                 ECS::PickAtmosphereLight( m_Registry, ECS::SunCandidacy::Lit, 1 );
            const auto second = secondPick.Sun();
            sceneRendererInfo.AtmosphereLight1.reset();
            if ( second )
            {
                const auto&     light  = m_Registry.get<ECS::DirectionLightComponent>( *second );
                const glm::vec3 travel = ECS::DirectionalLightTravel( m_Registry, *second );
                sceneRendererInfo.AtmosphereLight1 = Graphic::SceneRenderer::AtmosphereLight1Frame{
                     .Light = Graphic::ShaderProtocols::DirectionLightPayload{
                          glm::vec4( travel, std::sin( glm::radians( light.Data.SourceAngle ) * 0.5f ) ),
                          glm::vec4( light.Data.Color, light.Data.Intensity ) },
                     .SourceAngleDegrees = light.Data.SourceAngle,
                     .CastShadows        = light.Data.CastShadows,
                     .ShadowBias         = { .BiasCm       = light.Data.ShadowBias,
                                             .SlopeBiasCm  = light.Data.ShadowSlopeBias,
                                             .Distribution = light.Data.ShadowCascadeBiasDistribution },
                     .CascadeSplitLambda = light.Data.CascadeSplitLambda };
            }

            // WHICH LIGHTS EMIT NOTHING, AND WHY ANYONE SHOULD HEAR ABOUT IT: every visible directional
            // light that is not the sun contributes nothing at all. Hidden lights are not in the pick: a
            // light the artist deliberately hid is not one they mis-authored.
            const auto nameOf = [&]( entt::entity entity ) -> std::string
            {
                return m_Registry.has<ECS::TagComponent>( entity )
                            ? m_Registry.get<ECS::TagComponent>( entity ).Tag
                            : std::string( "<untagged>" );
            };
            // Latched on the STATE (which lights are unused, whether there is a sun), not on a flag: this runs
            // every frame, so an unlatched LOG_ERROR here would bury the log at 60 lines a second, and a plain
            // once-only flag would go quiet about the SECOND state. The state is compared as entity handles in a
            // reused buffer; the names are formatted only when it changes - including back to empty, which is
            // the line that tells a user their fix worked.
            m_DirLightsUnusedScratch.clear();
            for ( size_t i = 0; i < pick.Candidates.size(); ++i )
                if ( pick.Selection.Chosen != i && pick.Entities[i] != second )
                    m_DirLightsUnusedScratch.push_back( pick.Entities[i] );
            const bool hasSun = static_cast<bool>( pick.Sun() );
            if ( !m_DirLightsLatched || hasSun != m_DirLightsSunReported ||
                 m_DirLightsUnusedScratch != m_DirLightsUnusedReported )
            {
                m_DirLightsLatched     = true;
                m_DirLightsSunReported = hasSun;
                m_DirLightsUnusedReported = m_DirLightsUnusedScratch; // reuses its capacity
                std::string unused;
                for ( const entt::entity entity : m_DirLightsUnusedScratch )
                    std::format_to( std::back_inserter( unused ), "{}{}", unused.empty() ? "" : ", ",
                                    nameOf( entity ) );
                if ( !unused.empty() && pick.Sun() )
                    LOG_ERROR( "[Scene] directional light(s) {} emit NOTHING: the engine renders the two "
                               "atmosphere lights only (the sun '{}' at index 0, a moon at index 1). Remove them, "
                               "or mark them as Atmosphere Sun Light at a free index.",
                               unused, nameOf( *pick.Sun() ) );
                if ( !pick.Sun() && m_Registry.view<ECS::DirectionLightComponent>().size() > 0 )
                    LOG_ERROR( "[Scene] no visible directional light: the scene has NO sun — no direct light, "
                               "no shadows, no atmosphere sun." );
            }
        }

        // Every view's camera is ticked, not just view 0's: an EditorCamera integrates its own input and
        // a view whose camera never updated would be frozen in place while its window says otherwise.
        {
            DESERT_PROFILE_SCOPE( "Scene: View cameras" );
            for ( auto& view : m_Views.All() )
            {
                if ( view.Camera )
                    view.Camera->OnUpdate( ts );
            }
        }

        // ── THE LINE BETWEEN "ONCE" AND "PER VIEW" ───────────────────────────────────────────────────
        //
        // Everything above this point read the world: ExecuteSystems walked the ECS and recorded draw
        // commands into m_SystemCommandArenas, and the directional-light group was collected. It runs
        // ONCE no matter how many views are open, which is the point of the view list — a second angle
        // on one world must cost a second set of GPU passes, not a second walk of every component pool.
        //
        // Everything below is per view, replaying the arena this update's systems recorded into
        // (m_SystemCommandArenas). RenderCommandBuffer::ExecuteAll does not consume the buffer (the
        // arena is rewound by Clear, which happens after the last view), so replaying the SAME recording
        // into a second renderer is exactly what it already supports — the draws are identical because
        // the world is identical; only the camera each renderer was opened with differs.
        const SystemCommandArena& arena = m_SystemCommandArenas[m_RecordingArena];
        // NO FENCE HERE (CPU-ONE-SYNC; UE: the game thread never joins the rendering thread per frame, it enqueues
        // render commands). This world's views of the previous frame may still be recording, and the surface
        // cache, the retire queues and the reflection captures below are what they read, so nothing here writes
        // them: the surface cache's frame is a render command behind those views, the retire queues and the
        // captures are deferred to the fence the first view's bracket takes (SceneRenderer::BeginScene joins and
        // runs the deferred commands, RenderTask::AfterJoin), and each writer names a write under a recording
        // view (RenderTask::CheckRenderStateWrite). THE MATERIAL HAND-OFF (MaterialInstance, THE RENDER COPY) is
        // BeginScene's too, after that fence, before any view of this update reads a parameter.
        const auto&    frames         = Engine::FrameManager::GetInstance();
        const uint64_t absoluteFrame  = frames.GetAbsoluteFrameCount();
        const uint32_t framesInFlight = frames.GetMaxFramesInFlight();
        // THE INTAKE OF THIS UPDATE'S RECORDING (CPU-ONE-SYNC step 4, REVIEW-FPS14 BUG 1; UE: a proxy's resources
        // are initialised before the rendering thread draws it). The replays - the capture faces at the fence and
        // every view on the render task - only read (RenderCommand::Execute); what they need from a mesh the
        // commands point at (its device buffers, Mesh::EnsureGpuResources) is made HERE, on the game thread, at
        // the fence: the previous frame's views that read those buffers are done, it runs before the captures
        // below (deferred commands run in order) and before this update's first view is enqueued (that view's
        // BeginScene joins first). Serial, or nothing in flight: it runs now.
        // THE VIEWS ARE ITS WHOLE FENCE, and the captures' and the retire queues' below (RenderTask::AfterViews,
        // CPU-GT-OVERLAP): what the three write - the meshes' device geometry and render revision, the cubes,
        // the parked caches - is read by view jobs only, so BeginScene's JoinViews runs them while the previous
        // frame's interface draw, submit and present and this frame's begin still record on the render task.
        {
            const SystemCommandArena* const prepared = &arena;
            ::Desert::Engine::RenderTask::Get().AfterViews(
                 [prepared]()
                 {
                     DESERT_PROFILE_SCOPE( "Scene: Prepare recorded draws (fence)" );
                     ::Desert::Engine::RenderTask::CheckRenderStateWrite( "Scene: Prepare recorded draws" );
                     for ( const auto& buffer : *prepared )
                         buffer->PrepareAll();
                 } );
        }
        {
            // THE SCENE'S SURFACE CACHE, ONCE FOR EVERY VIEW (UE: the Lumen scene update gathers every view origin
            // of the family before any view renders): the frame's view origins - the cameras just ticked above -
            // and the scene's budget and card tracer are stated before the first view opens, so the one update the
            // first view to reach the cache runs captures the cards in front of every viewport, not its own.
            DESERT_PROFILE_SCOPE( "Scene: Surface cache + retire" );
            // The parked caches and captures die at the fence, the same frame numbers as before (stamped here);
            // the registry is read on the game thread, the surface cache's command takes the values
            // (ISceneRenderState::BeginFrame).
            if ( m_RenderState )
            {
                std::vector<glm::vec3> origins;
                for ( const auto& view : m_Views.All() )
                    if ( view.Camera )
                        origins.push_back( view.Camera->GetPosition() );
                m_RenderState->BeginFrame( m_Registry, m_Settings->RenderingPath == RenderPath::Deferred,
                                           absoluteFrame, framesInFlight, std::move( origins ) );
            }
        }
        {
            // THE SCENE'S REFLECTION CAPTURES FIRST, ONCE: their face views are frames of their own, closed before
            // any view opens, fed the same recorded draws. Only when a view reads them; the sky light they are lit
            // by is the one the first such view holds, lent to the faces (every view bakes the one sky of this
            // scene; a face bakes none). A scope of their own, so "Scene: Views" stays the cost of the views
            // alone. AT THE FENCE (RenderTask::AfterJoin): the driver reads the world and writes what the views in
            // flight read (the cubes, the round's state). It runs before the first view of this update opens -
            // that view's BeginScene joins first - and the recorded arena is cleared only behind this update's
            // views.
            DESERT_PROFILE_SCOPE( "Scene: Reflection Captures" );
            const SystemCommandArena* const                 recorded = &arena;
            const Graphic::SceneRenderer::UpdateInfo* const info     = &sceneRendererInfo;
            ::Desert::Engine::RenderTask::Get().AfterViews(
                 [this, recorded, info]()
                 {
                     DESERT_PROFILE_SCOPE( "Scene: Reflection Captures (fence)" );
                     for ( auto& view : m_Views.All() )
                     {
                         if ( !m_RenderState || !view.Renderer->ReadsReflectionCaptures() )
                             continue;
                         // ONE LIGHTING STATE, NOT EACH INPUT: a sun the sky light is being (or will be) captured
                         // again for is rendered into the captures with that sky, not under the old one. Index 0
                         // is the atmosphere sun (ECS::Rules::AtmosphereSunDirection, the one negation).
                         const auto& suns = info->DirLights.DirectionLights;
                         const bool  skyFollows =
                              !suns.empty() &&
                              view.Renderer->SkyLightRecapturesFor(
                                   ECS::Rules::AtmosphereSunDirection( glm::vec3( suns.front().Direction ) ) );
                         const std::optional<Graphic::Environment> environment = view.Renderer->GetEnvironment();
                         m_RenderState->RenderReflectionCaptures(
                              *this,
                              [recorded]( Graphic::SceneRenderer& face )
                              {
                                  for ( const auto& buffer : *recorded )
                                      buffer->ExecuteAll( face );
                              },
                              info->Timestep, info->RealTimestep, info->DirLights, environment, skyFollows );
                         break;
                     }
                 } );
        }
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
                const auto begun = [&]
                {
                    DESERT_PROFILE_SCOPE( "SceneRenderer: BeginScene" );
                    return view.Renderer->BeginScene( *this, view.Camera.get() );
                }();
                if ( !begun )
                {
                    firstError = begun.GetError();
                    continue; // do NOT record into a renderer that refused to open
                }

                // THE HALF THAT NO LONGER READS THE WORLD goes to the render task (RENDER-TASK-1; UE: the game
                // thread enqueues the view family and goes on to the interface). BeginScene reads the registry
                // and the live camera; from here the renderer works from its own copies (the frozen camera, the
                // recorded draws) and from `info`, this update's slot of m_FrameRendererInfos, by reference (it
                // lives as long as the arena). The next view's BeginScene joins this job first, so
                // two brackets are never open at once. Its failure is reported by the render task.
                //
                // THE REPLAY IS THE JOB'S (CPU-ONE-SYNC step 3; UE: the mesh draw commands are built on the
                // rendering thread from the proxies the game thread handed over). The recorded commands hold
                // values AND raw pointers to meshes and materials; the replay only READS them (RenderCommand::
                // Execute): the one write it used to make - a stale mesh's upload - is the intake's, done at the
                // fence above (RenderCommand::Prepare). The arena is this update's one: the next update records
                // into the other arena, and this one is cleared only behind this update's views ("Scene: CmdBuffer
                // Clear"). What the replay writes - the renderer's per-frame draw lists - is render-owned, so it
                // is written on the render task, after BeginScene's state and before OnUpdate reads it. A
                // game-thread edit of a drawn mesh while the job runs (DynamicMesh::Update from an editor tool,
                // a system ticking past the previous frame's views) writes only what no replay reads: its CPU
                // arrays and a pending flag (Mesh::MarkGpuStale). The revision, the BLAS and the device
                // buffers change at the next fence (SceneRenderer::PrepareMesh, CPU-ONE-SYNC step 5).
                // ECS::SetEditableMesh builds a new mesh and retires the old one behind the views
                // (SetStaticMesh, RetireRenderCache). Registration order - NOT
                // completion order - so the frame's submission order is the old sequential path's.
                auto* const                     renderer = &*view.Renderer;
                const SystemCommandArena* const                 replayed = &arena;
                const Graphic::SceneRenderer::UpdateInfo* const info     = &sceneRendererInfo;
                ::Desert::Engine::RenderTask::Get().EnqueueView(
                     "Scene view",
                     [renderer, replayed, info]() -> Common::BoolResultStr
                     {
                         {
                             DESERT_PROFILE_SCOPE( "Scene: System command buffers" );
                             for ( const auto& buffer : *replayed )
                                 buffer->ExecuteAll( *renderer );
                         }
                         renderer->OnUpdate( *info );
                         DESERT_PROFILE_SCOPE( "SceneRenderer: EndScene" );
                         return renderer->EndScene();
                     } );
            }
        }
        // Behind the last view, on the same queue: the replayed draws may point into THIS update's arena until
        // their view has recorded. The next update records into the other arena; the one after it, into this one
        // again, only after its views joined this job (m_SystemCommandArenas).
        ::Desert::Engine::RenderTask::Get().Enqueue( "Scene: CmdBuffer Clear",
                                                     [cleared = &arena]() -> Common::BoolResultStr
                                                     {
                                                         DESERT_PROFILE_SCOPE( "Scene: CmdBuffer Clear" );
                                                         for ( const auto& buffer : *cleared )
                                                             buffer->Clear();
                                                         return BOOLSUCCESS;
                                                     } );

        // The stepped frame of a pause is THIS update, whatever it reported: consumed here, after every
        // system has asked TicksGameplay(), so the world holds again from the next frame.
        m_SingleFramePending = false;

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
        //      in cmake/DesertWorkspace.cmake precisely so that `value++` is not a plain read-modify-write
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
        r.prepare<ECS::HiddenByDataLayersComponent>();
        r.prepare<ECS::StreamingPendingComponent>(); // ECS::IsHidden reads it from every collector
        r.prepare<ECS::StreamingOutComponent>();     // and this one (DestroyEntityAtFence)

        // Renderables.
        r.prepare<ECS::StaticMeshComponent>();
        r.prepare<ECS::SkinnedMeshComponent>();
        r.prepare<ECS::InstancedStaticMeshComponent>();
        // MeshECSSystem asks `try_get<FoliageComponent>` of every instanced mesh for its cull distance and wind,
        // so a level with no foliage field would otherwise create this pool inside the parallel phase.
        r.prepare<ECS::FoliageComponent>();
        r.prepare<ECS::MaterialComponent>();
        // MeshECSSystem asks `has<MorphComponent>` of every skinned mesh with morph targets (LEG-MORPH).
        r.prepare<ECS::MorphComponent>();
        r.prepare<ECS::AnimationComponent>();
        // Skeletal controls. AnimationECSSystem asks `has<TwoBoneIKComponent>` for every animated entity,
        // and the ask itself is a first touch — so a rig with no IK on it would have created this pool from
        // inside the parallel phase. Found by Tests/Engine/ComponentPools, which is the census that reads
        // the system headers rather than trusting this list.
        r.prepare<ECS::TwoBoneIKComponent>();
        r.prepare<ECS::ControlRigComponent>();
        r.prepare<ECS::RetargetComponent>();
        // LevelSequenceSystem views every LevelSequenceComponent; a level with no sequence actor would
        // otherwise create this pool inside the parallel phase.
        r.prepare<ECS::LevelSequenceComponent>();
        r.prepare<ECS::TextComponent>();

        // Lights and sky.
        r.prepare<ECS::DirectionLightComponent>();
        r.prepare<ECS::PointLightComponent>();
        r.prepare<ECS::SpotLightComponent>();
        r.prepare<ECS::RectLightComponent>();
        r.prepare<ECS::SkyboxComponent>();
        r.prepare<ECS::SkyAtmosphereComponent>();
        r.prepare<ECS::TimeOfDayComponent>();
        r.prepare<ECS::ExponentialHeightFogComponent>();
        r.prepare<ECS::LocalFogVolumeComponent>();
        r.prepare<ECS::HeterogeneousVolumeComponent>();
        r.prepare<ECS::PostProcessVolumeComponent>();
        r.prepare<ECS::VolumetricCloudComponent>();
        r.prepare<ECS::WindSourceComponent>();
        r.prepare<ECS::DecalComponent>();
        r.prepare<ECS::HeroCloudComponent>();
        r.prepare<ECS::ProceduralFoliageComponent>();
        r.prepare<ECS::ProceduralFoliageFieldComponent>();

        // Gameplay -- serial systems today, and prepared all the same: what makes a type safe is that its
        // pool exists before the first parallel group, not which system happens to be serial this week.
        r.prepare<ECS::ColliderComponent>();
        r.prepare<ECS::RigidBodyComponent>();
        r.prepare<ECS::RagdollComponent>();
        // The feature modules' pools (Destruction: the Fracture preview, viewed by its stand-in draw inside
        // MeshECSSystem's parallel group, and the field components; Water: its bodies and zones).
        for ( const RegistryPrepare prepare : ModuleHooks().Prepares )
            prepare( r );
        r.prepare<ECS::CharacterControllerComponent>();
        r.prepare<ECS::SpringArmComponent>();
        r.prepare<ECS::PlayerStartComponent>();
        r.prepare<ECS::StreamingSourceComponent>();
        r.prepare<ECS::ScriptComponent>();
        r.prepare<ECS::AudioSourceComponent>();

        // Every module's reflected component (Core/Serialize/ComponentRegistration.hpp) — a module's component
        // is prepared from its own generated registration, not named here: DestructibleComponent left this list
        // with BUILD-MOD-5 so a field added to it no longer recompiles the engine's Scene.cpp. Preparing a pool
        // twice is a no-op, so the rows also named above stay correct.
        for ( const ECS::ReflectedComponent& row : ECS::AllReflectedComponents() )
            row.Prepare( r );
        r.prepare<ECS::UIAnimComponent>(); // AudioECSSystem reads the UI clips' Audio voices
        r.prepare<ECS::SocketAttachmentComponent>();
    }

    void Scene::SetQuality( const Common::Scalability::ResolvedQuality& quality )
    {
        for ( auto& system : m_Systems )
            system->SetQuality( quality );
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
        SystemCommandArena& arena = m_SystemCommandArenas[m_RecordingArena];
        while ( arena.size() < m_Systems.size() )
            arena.emplace_back( std::make_unique<Graphic::Render::RenderCommandBuffer>() );
        // The dereference is bound to a reference first because `typeid` on an expression WITH SIDE EFFECTS
        // evaluates it - `*system` is `unique_ptr::operator*`, a function call. Same dynamic type.
        while ( m_SystemScopeNames.size() < m_Systems.size() )
        {
            [[maybe_unused]] const auto& systemRef = *m_Systems[m_SystemScopeNames.size()];
            m_SystemScopeNames.push_back( DESERT_PROFILE_INTERN_NAME( typeid( systemRef ).name() ) );
        }

        const auto runOne = [&]( size_t index )
        {
            const auto& system = m_Systems[index];
            // Per-system timing (named by the system's type, interned above) so every ECS system is
            // individually visible in the profiler — no per-system edits.
            DESERT_PROFILE_SCOPE_INTERNED( m_SystemScopeNames[index] );
            system->Update( m_Registry, *arena[index], gameplayTs );
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
            {
                // The modules' shared tables are brought up to date HERE, serially, so the group's readers
                // only read them (ModuleHooks().BeforeParallelGroup; the landscape's tile table, which
                // WaterZoneECSSystem reads).
                for ( const RegistryPrepare refresh : ModuleHooks().BeforeParallelGroup )
                    refresh( m_Registry );
                Common::JobSystem::Get().ParallelFor( count,
                                                      [&]( size_t local ) { runOne( i + local ); } );
            }
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

    void Scene::RegisterExtensionPass( Graphic::ExtensionPass&& pass )
    {
        DESERT_VERIFY( !pass.Name.empty() && pass.Execute );
        // A view of this scene may be recording on the render task, walking this list and calling into the passes
        // (SceneRenderer::AddExtensionPoint): the list changes, and a replaced pass's owner may be freed, only
        // after it.
        ::Desert::Engine::RenderTask::Get().JoinOnGameThread();
        m_ExtensionPasses->Register( std::move( pass ) );
    }

    void Scene::UnregisterExtensionPass( const std::string& name )
    {
        // Same fence as Register: the owner calls this from its destructor, and its members (the state the pass's
        // Declare/Execute read) are destroyed right after.
        ::Desert::Engine::RenderTask::Get().JoinOnGameThread();
        m_ExtensionPasses->Unregister( name );
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
        // yet. The extension passes need nothing: the renderer reads them from this scene every frame.
        if ( m_Initialized )
        {
            renderer->Init();
            renderer->Resize( m_ViewportWidth, m_ViewportHeight );
            camera->UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
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

    void Scene::ReleaseParkedRenderers()
    {
        // The render task is joined FIRST (a view of another world may be recording the draws that name its own,
        // and the parked objects' destructors free into the allocators that view records with); the device is
        // idle. Then the parked caches and captures die, and the render caches this world's Clear parked as its
        // components died (ECS::RetireRenderCachesOf) go too, with any other world's.
        ::Desert::Engine::RenderTask::Get().JoinOnGameThread();
        ::Desert::Engine::RenderTask::CheckRenderStateWrite( "Scene::ReleaseParkedRenderers" );
        if ( m_RenderState )
            m_RenderState->ReleaseParked();
        ::Desert::Engine::FrameManager::GetInstance().ReleaseRetiredNow();
    }

    void Scene::Clear()
    {
        m_Subsystems.End();
        m_Registry.clear(); // the context stays: the world's UI state is emptied below, not remade
        GetUIWorld().Reset();
        m_File->Path.clear();
        ReportToCrashHandler();
        // A reload empties this object and refills it: a new world at the same address. Entity ids restart, so
        // a view's history keyed by (generation, entity) must not survive it (SceneRenderer::BeginScene).
        m_Generation   = NextSceneGeneration();
        // THE OLD WORLD'S RENDERER STATE GOES WITH IT (UE: the FScene is destroyed with its world, and the next
        // world's is built empty): what frames in flight still read is parked from this frame
        // (Graphic::SceneRenderState::Clear); the opener drains the device queue before the new world loads
        // (SceneWorkspace).
        if ( m_RenderState )
            m_RenderState->Clear( Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
        m_Players.clear();
        m_ViewTarget   = entt::null;
        m_GameMode->Reset();
        m_PlayFromHere = false;
        m_Simulating   = false;

        m_Entities.Clear();
        for ( const auto& moduleState : m_ModuleStates )
            moduleState->Clear();

        SetupRegistryCallbacks();
        m_Subsystems.Begin();
    }

    namespace
    {
        // A destroyed component's render caches outlive it by the frames in flight (ECS::RetireRenderCache): it
        // is removed by the interface while the render task records the draws that name them by raw pointer.
        template <typename Component>
        void RetireRenderCachesOnDestroy( entt::registry& registry, entt::entity entity )
        {
            ECS::RetireRenderCachesOf( registry.get<Component>( entity ) );
        }
    } // namespace

    void Scene::SetupRegistryCallbacks()
    {
        // Disconnect first — Clear() calls this on every scene reload, and entt keeps signal connections
        // across registry.clear(), so re-connecting without this accumulates duplicate handler invocations.
        m_Registry.on_construct<ECS::CameraComponent>().disconnect( this );
        m_Registry.on_construct<ECS::CameraComponent>().connect<&Scene::OnEntityCreated_Camera>( this );

        // The render-state dirty set (MarkRenderStateDirty), fed at the source.
        m_Registry.on_construct<ECS::TransformComponent>().disconnect( this );
        m_Registry.on_update<ECS::TransformComponent>().disconnect( this );
        m_Registry.on_destroy<ECS::TransformComponent>().disconnect( this );
        m_Registry.on_construct<ECS::RelationshipComponent>().disconnect( this );
        m_Registry.on_update<ECS::RelationshipComponent>().disconnect( this );
        m_Registry.on_destroy<ECS::RelationshipComponent>().disconnect( this );
        // The hierarchy stamp (ECS::HierarchyRevision): a relationship emplaced (OnRelationshipConstructed),
        // replaced or destroyed; Attach / Detach edit it in place and bump it themselves.
        m_Registry.on_update<ECS::RelationshipComponent>().connect<&Scene::OnRelationshipChanged>( this );
        m_Registry.on_destroy<ECS::RelationshipComponent>().connect<&Scene::OnRelationshipChanged>( this );
        // The world-transform cache first, so the render-state listeners below already see the composed world.
        m_Registry.on_construct<ECS::TransformComponent>().connect<&Scene::OnTransformWorldPending>( this );
        m_Registry.on_update<ECS::TransformComponent>().connect<&Scene::OnTransformWorldPending>( this );
        m_Registry.on_construct<ECS::RelationshipComponent>().connect<&Scene::OnRelationshipConstructed>( this );
        m_Registry.on_construct<ECS::TransformComponent>().connect<&Scene::OnTransformRenderStateChanged>( this );
        m_Registry.on_update<ECS::TransformComponent>().connect<&Scene::OnTransformRenderStateChanged>( this );
        m_Registry.on_destroy<ECS::TransformComponent>().connect<&Scene::OnTransformRenderStateChanged>( this );
        const auto connect = [this]<typename Component>()
        {
            m_Registry.on_construct<Component>().disconnect( this );
            m_Registry.on_update<Component>().disconnect( this );
            m_Registry.on_destroy<Component>().disconnect( this );
            m_Registry.on_construct<Component>().template connect<&Scene::OnRenderStateChanged>( this );
            m_Registry.on_update<Component>().template connect<&Scene::OnRenderStateChanged>( this );
            m_Registry.on_destroy<Component>().template connect<&Scene::OnRenderStateChanged>( this );
        };
        connect.template operator()<ECS::StaticMeshComponent>();
        connect.template operator()<ECS::SkinnedMeshComponent>();
        connect.template operator()<ECS::InstancedStaticMeshComponent>();
        connect.template operator()<ECS::MaterialComponent>();
        connect.template operator()<ECS::VisibilityComponent>();
        // The editor's data-layer hiding (UE bHiddenEdLayer): ECS::IsHidden reads it beside VisibilityComponent.
        connect.template operator()<ECS::HiddenByDataLayersComponent>();
        // A field's foliage type: the ISM record's fade and wind source (MeshECSSystem's Ism channel).
        connect.template operator()<ECS::FoliageComponent>();
        // Every component that holds render caches parks them as it dies (UE: the proxy's RHI objects are
        // deleted once no frame in flight names them, never at the component's unregister).
        const auto retireOnDestroy = [this]<typename Component>()
        {
            m_Registry.on_destroy<Component>().template disconnect<&RetireRenderCachesOnDestroy<Component>>();
            m_Registry.on_destroy<Component>().template connect<&RetireRenderCachesOnDestroy<Component>>();
        };
        retireOnDestroy.template operator()<ECS::StaticMeshComponent>();
        retireOnDestroy.template operator()<ECS::SkinnedMeshComponent>();
        retireOnDestroy.template operator()<ECS::InstancedStaticMeshComponent>();
        retireOnDestroy.template operator()<ECS::TextComponent>();
        {
            std::lock_guard lock( m_RenderStateDirtyMutex );
            m_RenderStateDirty.Clear();
            m_RenderStateDirtyLastFrame = 0;
        }
        // The retained render cache rebuilds from the registry as it now stands (MeshECSSystem).
        m_RenderStateDelta.RequestFullRebuild();
        m_Registry.ctx_or_set<ECS::RenderStateDeltaLink>().Delta = &m_RenderStateDelta;
        // A reload is a new hierarchy: every cache derived from the old one is dropped.
        m_HierarchyRevision.Bump();
        m_Registry.ctx_or_set<ECS::HierarchyRevisionLink>().Revision = &m_HierarchyRevision;
        // The scene's settings (gravity, ...) for the readers that have only the registry (Core::SceneSettingsLink).
        m_Registry.ctx_or_set<Core::SceneSettingsLink>().Settings = m_Settings.get();
    }

    void Scene::OnTransformWorldPending( entt::registry& registry, entt::entity entity )
    {
        // SetLocalTransform composed the world inside its patch; only an emplace or a replace is pending.
        if ( ECS::WorldTransformPending( registry.get<ECS::TransformComponent>( entity ) ) )
            ECS::UpdateWorldTransforms( registry, entity );
    }

    void Scene::OnRelationshipConstructed( entt::registry& registry, entt::entity entity )
    {
        // Unconditional: a root emplaced with its Children already listed (scene load restores children before
        // their parent's relationship) has to push its world down to them as well.
        ECS::UpdateWorldTransforms( registry, entity );
        m_HierarchyRevision.Bump();
    }

    void Scene::OnRelationshipChanged( entt::registry&, entt::entity )
    {
        m_HierarchyRevision.Bump();
    }

    void Scene::OnTransformRenderStateChanged( entt::registry&, entt::entity entity )
    {
        MarkRenderStateDirty( entity, true );
    }

    void Scene::OnRenderStateChanged( entt::registry&, entt::entity entity )
    {
        MarkRenderStateDirty( entity, false );
    }

    void Scene::MarkRenderStateDirty( entt::entity entity, bool subtree )
    {
        std::lock_guard lock( m_RenderStateDirtyMutex );
        if ( !subtree )
        {
            (void)m_RenderStateDirty.Mark( entity );
            m_RenderStateDelta.Mark( entity );
            return;
        }
        // Iterative, so a deep hierarchy cannot overflow the stack; the stack is a member kept under the lock,
        // so a transform event allocates nothing once it has grown. An entity already in the set had its
        // subtree dirtied when it entered — unless it entered alone (a mesh / material / visibility change),
        // so the walk goes on below every entity regardless.
        std::vector<entt::entity>& pending = m_RenderStateWalk;
        pending.clear();
        pending.push_back( entity );
        while ( !pending.empty() )
        {
            const entt::entity current = pending.back();
            pending.pop_back();
            (void)m_RenderStateDirty.Mark( current );
            m_RenderStateDelta.Mark( current );
            if ( !m_Registry.valid( current ) )
                continue;
            if ( const auto* relation = m_Registry.try_get<ECS::RelationshipComponent>( current ) )
                pending.insert( pending.end(), relation->Children.begin(), relation->Children.end() );
        }
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
        m_HierarchyRevision.Bump(); // edited in place: no signal carries it
        // A new parent is a new world transform for the child and everything under it.
        ECS::UpdateWorldTransforms( m_Registry, child.GetHandle() );
        MarkRenderStateDirty( child.GetHandle(), true );
    }

    void Scene::ArrangeRoots( const std::vector<ECS::Entity>& roots )
    {
        std::vector<entt::entity> handles;
        handles.reserve( roots.size() );
        for ( const ECS::Entity& root : roots )
            handles.push_back( root.GetHandle() );
        m_Entities.Arrange( handles );
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
        m_HierarchyRevision.Bump(); // edited in place: no signal carries it
        ECS::UpdateWorldTransforms( m_Registry, child.GetHandle() );
        MarkRenderStateDirty( child.GetHandle(), true );
    }

    void Scene::SetVisibleRecursive( ECS::Entity entity, bool visible )
    {
        if ( !entity ) return;

        entity.SetVisible( visible );
        if ( entity.HasComponent<ECS::UUIDComponent>() )
            MarkModified( entity.GetComponent<ECS::UUIDComponent>().UUID );

        if ( entity.HasComponent<ECS::RelationshipComponent>() )
        {
            auto& rel = entity.GetComponent<ECS::RelationshipComponent>();
            for ( auto childHandle : rel.Children )
                SetVisibleRecursive( ECS::Entity( childHandle, m_Registry ), visible );
        }
    }

    void Scene::MarkModifiedSubtree( ECS::Entity root ) const
    {
        if ( !root )
            return;
        // Walked with an explicit stack over the const registry: a deep hierarchy cannot overflow the call stack.
        std::vector<entt::entity> pending{ root.GetHandle() };
        while ( !pending.empty() )
        {
            const entt::entity entity = pending.back();
            pending.pop_back();
            if ( const auto* uuid = m_Registry.try_get<ECS::UUIDComponent>( entity ) )
                MarkModified( uuid->UUID );
            if ( const auto* rel = m_Registry.try_get<ECS::RelationshipComponent>( entity ) )
                for ( const auto child : rel->Children )
                    if ( m_Registry.valid( child ) )
                        pending.push_back( child );
        }
    }

    void Scene::DestroyEntity( ECS::Entity entity )
    {
        // A destroy on the game thread while the views record waits for them: what the entity owned may be
        // what a replayed draw points at (RenderTask).
        ::Desert::Engine::RenderTask::Get().Join();
        if ( !entity )
            return;
        // The algorithm lives beside the index it maintains — see SceneEntityIndex.hpp for why it is O(1)
        // per entity and what that did to the order of GetAllEntities().
        DestroyEntityTree( m_Registry, m_Entities, entity.GetHandle() );
    }

    void Scene::DestroyEntityAtFence( ECS::Entity entity )
    {
        if ( !entity || !m_Registry.valid( entity.GetHandle() ) )
            return;
        const entt::entity root = entity.GetHandle();
        // Hidden and unnamed now, the whole subtree: a prefab instance's inner entities go with its root.
        std::vector<entt::entity> walk{ root };
        while ( !walk.empty() )
        {
            const entt::entity next = walk.back();
            walk.pop_back();
            if ( !m_Registry.valid( next ) || m_Registry.has<ECS::StreamingOutComponent>( next ) )
                continue;
            m_Registry.emplace<ECS::StreamingOutComponent>( next );
            m_Entities.ForgetId( next );
            if ( const auto* relationship = m_Registry.try_get<ECS::RelationshipComponent>( next ) )
                walk.insert( walk.end(), relationship->Children.begin(), relationship->Children.end() );
        }
        MarkRenderStateDirty( root, true );
        m_DestroyAtFence.push_back( root );
        if ( m_DestroyAtFence.size() > 1 )
            return; // the command queued for the first root destroys this one too
        // Serial, or nothing in flight: it runs now.
        ::Desert::Engine::RenderTask::Get().AfterViews(
             [this]()
             {
                 DESERT_PROFILE_SCOPE( "Scene: Destroy streamed-out entities (fence)" );
                 std::vector<entt::entity> roots = std::move( m_DestroyAtFence );
                 m_DestroyAtFence.clear();
                 // A root destroyed since (a script, its parent's tree) is no longer valid; DestroyEntityTree
                 // skips it, and a recycled handle carries another version.
                 for ( const entt::entity doomed : roots )
                     DestroyEntityTree( m_Registry, m_Entities, doomed );
             } );
    }

} // namespace Desert::Core