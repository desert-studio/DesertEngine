#include <Engine/Libraries/EntityLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/Ray.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Libraries/ScriptWorld.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Reflection/FunctionThunk.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <format>

namespace Desert::Libraries
{
    namespace
    {
        template <class T>
        T* Get( const ECS::Entity& entity )
        {
            return entity ? entity.GetRegistry()->try_get<T>( entity.GetHandle() ) : nullptr;
        }

    } // namespace

    // ── EntityLibrary ──

    std::string EntityLibrary::Name( ECS::Entity entity )
    {
        const auto* tag = Get<ECS::TagComponent>( entity );
        return tag != nullptr ? tag->Tag : std::string();
    }

    void EntityLibrary::AttachTo( ECS::Entity entity, ECS::Entity target, const std::string& socket )
    {
        if ( !entity )
            return;
        Common::UUID targetId;
        if ( const auto* id = Get<ECS::UUIDComponent>( target ) )
            targetId = id->UUID;
        auto& attachment =
             entity.GetRegistry()->get_or_emplace<ECS::SocketAttachmentComponent>( entity.GetHandle() );
        attachment.Target     = targetId;
        attachment.SocketName = socket;
    }

    void EntityLibrary::Detach( ECS::Entity entity )
    {
        if ( entity )
            entity.GetRegistry()->remove<ECS::SocketAttachmentComponent>( entity.GetHandle() );
    }

    glm::vec3 EntityLibrary::GetPosition( ECS::Entity entity )
    {
        const auto* t = Get<ECS::TransformComponent>( entity );
        return t != nullptr ? t->GetTranslation() : glm::vec3( 0.0f );
    }

    void EntityLibrary::SetPosition( ECS::Entity entity, const glm::vec3& position )
    {
        if ( Get<ECS::TransformComponent>( entity ) != nullptr )
            entity.SetLocalTranslation( position );
    }

    void EntityLibrary::Translate( ECS::Entity entity, const glm::vec3& offset )
    {
        if ( const auto* t = Get<ECS::TransformComponent>( entity ) )
            entity.SetLocalTranslation( t->GetTranslation() + offset );
    }

    glm::vec3 EntityLibrary::GetRotation( ECS::Entity entity )
    {
        const auto* t = Get<ECS::TransformComponent>( entity );
        return t != nullptr ? t->GetRotation() : glm::vec3( 0.0f );
    }

    void EntityLibrary::SetRotation( ECS::Entity entity, const glm::vec3& euler )
    {
        if ( Get<ECS::TransformComponent>( entity ) != nullptr )
            entity.SetLocalRotation( euler );
    }

    glm::vec3 EntityLibrary::GetScale( ECS::Entity entity )
    {
        const auto* t = Get<ECS::TransformComponent>( entity );
        return t != nullptr ? t->GetScale() : glm::vec3( 1.0f );
    }

    void EntityLibrary::SetScale( ECS::Entity entity, const glm::vec3& scale )
    {
        if ( Get<ECS::TransformComponent>( entity ) != nullptr )
            entity.SetLocalScale( scale );
    }

    glm::vec3 EntityLibrary::Forward( ECS::Entity entity )
    {
        const auto*     t = Get<ECS::TransformComponent>( entity );
        const glm::vec3 forward( 0.0f, 0.0f, -1.0f );
        return t != nullptr ? glm::quat( t->GetRotation() ) * forward : forward;
    }

    glm::vec3 EntityLibrary::Right( ECS::Entity entity )
    {
        const auto*     t = Get<ECS::TransformComponent>( entity );
        const glm::vec3 right( 1.0f, 0.0f, 0.0f );
        return t != nullptr ? glm::quat( t->GetRotation() ) * right : right;
    }

    float EntityLibrary::DistanceTo( ECS::Entity entity, ECS::Entity other )
    {
        const auto* a = Get<ECS::TransformComponent>( entity );
        const auto* b = Get<ECS::TransformComponent>( other );
        return ( a != nullptr && b != nullptr ) ? glm::distance( a->GetTranslation(), b->GetTranslation() ) : -1.0f;
    }

    // ── CharacterLibrary ──

    void CharacterLibrary::Move( ECS::Entity entity, float forward, float right )
    {
        if ( auto* cc = Get<ECS::CharacterControllerComponent>( entity ) )
            cc->MoveInput = { right, forward };
    }

    void CharacterLibrary::Jump( ECS::Entity entity )
    {
        if ( auto* cc = Get<ECS::CharacterControllerComponent>( entity ) )
            cc->JumpRequested = true;
    }

    void CharacterLibrary::Crouch( ECS::Entity entity, bool crouch )
    {
        if ( auto* cc = Get<ECS::CharacterControllerComponent>( entity ) )
            cc->CrouchRequested = crouch;
    }

    bool CharacterLibrary::IsCrouched( ECS::Entity entity )
    {
        const auto* cc = Get<ECS::CharacterControllerComponent>( entity );
        return cc != nullptr && cc->IsCrouched;
    }

    bool CharacterLibrary::IsOnGround( ECS::Entity entity )
    {
        const auto* cc = Get<ECS::CharacterControllerComponent>( entity );
        return cc != nullptr && cc->OnGround;
    }

    void CharacterLibrary::SetSwimming( ECS::Entity entity, bool swimming )
    {
        if ( auto* cc = Get<ECS::CharacterControllerComponent>( entity ) )
            cc->Swimming = swimming;
    }

    void CharacterLibrary::Swim( ECS::Entity entity, float vertical )
    {
        if ( auto* cc = Get<ECS::CharacterControllerComponent>( entity ) )
            cc->SwimVertical = vertical;
    }

    bool CharacterLibrary::IsSwimming( ECS::Entity entity )
    {
        const auto* cc = Get<ECS::CharacterControllerComponent>( entity );
        return cc != nullptr && cc->Swimming;
    }

    void CharacterLibrary::AddYaw( ECS::Entity entity, float radians )
    {
        if ( const auto* t = Get<ECS::TransformComponent>( entity ) )
        {
            glm::vec3 rotation = t->GetRotation();
            rotation.y += radians;
            entity.SetLocalRotation( rotation );
        }
    }

    void CharacterLibrary::AddCameraPitch( ECS::Entity entity, float radians )
    {
        const auto* rel = Get<ECS::RelationshipComponent>( entity );
        if ( rel == nullptr )
            return;
        entt::registry& reg = *entity.GetRegistry();
        for ( entt::entity const child : rel->Children )
        {
            // A spring arm takes the pitch (UE: the arm follows the control rotation) and swings its camera
            // with it; without one the camera child itself tilts.
            if ( ( reg.has<ECS::SpringArmComponent>( child ) || reg.has<ECS::CameraComponent>( child ) ) &&
                 reg.has<ECS::TransformComponent>( child ) )
            {
                glm::vec3 rot = reg.get<ECS::TransformComponent>( child ).GetRotation();
                rot.x         = glm::clamp( rot.x + radians, glm::radians( -85.0f ), glm::radians( 85.0f ) );
                ECS::SetLocalRotation( reg, child, rot );
                break;
            }
        }
    }

    // ── ProjectileLibrary ──

    void ProjectileLibrary::Launch( ECS::Entity entity, const glm::vec3& velocity, ECS::Entity instigator )
    {
        auto* projectile = Get<ECS::ProjectileComponent>( entity );
        if ( projectile == nullptr )
            return;
        projectile->Data.Velocity = velocity;
        projectile->Stopped       = false;
        if ( instigator )
            projectile->Instigator = instigator.GetHandle();
    }

    glm::vec3 ProjectileLibrary::ProjectileVelocity( ECS::Entity entity )
    {
        const auto* projectile = Get<ECS::ProjectileComponent>( entity );
        return projectile != nullptr ? projectile->Data.Velocity : glm::vec3( 0.0f );
    }

    bool ProjectileLibrary::HasStopped( ECS::Entity entity )
    {
        const auto* projectile = Get<ECS::ProjectileComponent>( entity );
        return projectile != nullptr && projectile->Stopped;
    }

    // ── MaterialLibrary ──

    namespace
    {
        void UpsertParam( std::vector<ECS::MaterialParamOverride>& params, const std::string& name,
                          const glm::vec4& v )
        {
            for ( auto& p : params )
                if ( p.Name == name )
                {
                    p.Value = v;
                    return;
                }
            params.push_back( { name, v } );
        }

        // The Shader Override component, when the entity has one naming a shader: its Params are that draw.
        const ECS::MaterialComponent* ShaderOverrideOf( const ECS::Entity& entity )
        {
            const auto* mc = Get<ECS::MaterialComponent>( entity );
            return mc != nullptr && !mc->GetShaderName().empty() ? mc : nullptr;
        }
    } // namespace

    // Two routes, one per thing the entity draws with (UE: UMeshComponent::SetScalar/VectorParameterValue-
    // OnMaterials, MeshComponent.cpp:549-628, whose MID is created on demand):
    //   * a Shader Override (MaterialComponent naming a shader) - the component's Params are that draw;
    //   * a static mesh's slot 0 - the write joins the mesh's ScriptSlotParams and, when the slot-0 instance
    //     exists, lands on it now. Before the instance is built (OnCreate runs first) the list alone holds it,
    //     and MeshECSSystem applies the list to every slot-0 instance it builds.
    // An entity with neither has no material to write, and the call fails naming it.
    void MaterialLibrary::SetMaterialParam( ECS::Entity entity, const std::string& name, const glm::vec4& value )
    {
        if ( const auto* mc = ShaderOverrideOf( entity ) )
        {
            ECS::MaterialDesc desc = mc->GetDesc();
            UpsertParam( desc.Params, name, value );
            ECS::SetMaterial( *entity.GetRegistry(), entity.GetHandle(), desc );
            return;
        }
        auto* smc = Get<ECS::StaticMeshComponent>( entity );
        if ( smc == nullptr )
        {
            Reflection::RaiseCallError(
                 std::format( "setMaterialParam('{}'): the entity has no static mesh and no "
                              "Shader Override - there is no material to write",
                              name ) );
            return;
        }
        // Through the component's one writer (ECS::SetStaticMesh): the slots are unchanged, so the runtime
        // instances survive and take the value directly below.
        ECS::StaticMeshDesc meshDesc = smc->GetDesc();
        UpsertParam( meshDesc.ScriptSlotParams, name, value );
        ECS::SetStaticMesh( *entity.GetRegistry(), entity.GetHandle(), meshDesc );
        smc = Get<ECS::StaticMeshComponent>( entity );
        if ( !smc->RuntimeMaterialInstances().empty() && smc->RuntimeMaterialInstances()[0] )
            smc->RuntimeMaterialInstances()[0]->SetParamFromVec4( name, value );
    }

    // The value a read sees is the one the write route above keeps: the override's Params, else the script's
    // slot-0 list, else the slot-0 instance's own override (a material-instance asset's authored value).
    glm::vec4 MaterialLibrary::GetMaterialParam( ECS::Entity entity, const std::string& name )
    {
        if ( const auto* mc = ShaderOverrideOf( entity ) )
        {
            for ( const auto& p : mc->GetParams() )
                if ( p.Name == name )
                    return p.Value;
            return glm::vec4( 0.0f );
        }
        if ( const auto* smc = Get<ECS::StaticMeshComponent>( entity ) )
        {
            for ( const auto& p : smc->GetScriptSlotParams() )
                if ( p.Name == name )
                    return p.Value;
            if ( !smc->RuntimeMaterialInstances().empty() && smc->RuntimeMaterialInstances()[0] )
                if ( const auto authored = smc->RuntimeMaterialInstances()[0]->GetOverrideAsVec4( name ) )
                    return *authored;
        }
        return glm::vec4( 0.0f );
    }

    // Undo every script-set param. On a static mesh the slot instances are DROPPED and rebuilt by MeshECSSystem
    // next frame from the authored slots: resetting the live instance's overrides would also wipe a
    // material-instance asset's own overrides, which the instance carries the same way.
    void MaterialLibrary::ClearMaterialParams( ECS::Entity entity )
    {
        if ( const auto* mc = ShaderOverrideOf( entity ) )
        {
            ECS::MaterialDesc desc = mc->GetDesc();
            desc.Params.clear();
            desc.Textures.clear();
            ECS::SetMaterial( *entity.GetRegistry(), entity.GetHandle(), desc );
        }
        if ( auto* smc = Get<ECS::StaticMeshComponent>( entity );
             smc != nullptr && !smc->GetScriptSlotParams().empty() )
        {
            ECS::StaticMeshDesc desc = smc->GetDesc();
            desc.ScriptSlotParams.clear();
            ECS::SetStaticMesh( *entity.GetRegistry(), entity.GetHandle(), desc );
            // The script overrides were baked into the runtime instances; the writer drops those only on a
            // slot change, so they are dropped here and rebuilt from the slots on the next draw.
            Get<ECS::StaticMeshComponent>( entity )->RuntimeMaterialInstances().clear();
        }
    }

    // A surface shader by name; "" drops the Shader Override and the mesh draws its lit slot materials again
    // (the override's Params belonged to that shader and go with it).
    void MaterialLibrary::SetShader( ECS::Entity entity, const std::string& shader )
    {
        if ( !entity )
            return;
        if ( shader.empty() )
            entity.GetRegistry()->remove<ECS::MaterialComponent>( entity.GetHandle() );
        else
        {
            const auto*       mc   = Get<ECS::MaterialComponent>( entity );
            ECS::MaterialDesc desc = mc != nullptr ? mc->GetDesc() : ECS::MaterialDesc{};
            desc.ShaderName        = shader;
            ECS::SetMaterial( *entity.GetRegistry(), entity.GetHandle(), desc );
        }
    }

    std::string MaterialLibrary::GetShader( ECS::Entity entity )
    {
        const auto* mc = Get<ECS::MaterialComponent>( entity );
        return mc != nullptr ? mc->GetShaderName() : std::string();
    }

    // ── WorldLibrary ──

    ECS::Entity WorldLibrary::Find( const std::string& name )
    {
        const Core::WorldContext* context = ScriptWorldContext( "World", "find" );
        if ( context == nullptr )
            return {};
        entt::registry& registry = context->World->GetRegistry();
        auto            view     = registry.view<ECS::TagComponent>();
        for ( auto e : view )
            if ( view.get<ECS::TagComponent>( e ).Tag == name )
                return ECS::Entity( e, registry );
        return {};
    }

    ECS::Entity WorldLibrary::Spawn( const std::string& prefabPath, const glm::vec3& position )
    {
        const Core::WorldContext* context = ScriptWorldContext( "World", "spawn" );
        if ( context == nullptr )
            return {};
        if ( context->Assets == nullptr )
        {
            LOG_ERROR( "[Script] World.spawn: no AssetManager bound" );
            return {};
        }
        auto prefab = context->Assets->FindByPath<Assets::PrefabAsset>( prefabPath );
        if ( !prefab )
            prefab = context->Assets->CreateAsset<Assets::PrefabAsset>( prefabPath );
        if ( !prefab )
        {
            LOG_ERROR( "[Script] World.spawn: prefab not found '{}'", prefabPath );
            return {};
        }
        const auto placed = prefab->Instantiate( context->World, *context->Assets, {}, &position );
        if ( !placed )
        {
            LOG_ERROR( "[Script] World.spawn: {}", placed.GetError() );
            return {};
        }
        return placed.GetValue();
    }

    ECS::Entity WorldLibrary::SpawnMarker( const glm::vec3& position, float scale, const glm::vec3& color )
    {
        const Core::WorldContext* context = ScriptWorldContext( "World", "spawnMarker" );
        if ( context == nullptr )
            return {};
        ECS::Entity         e = context->World->CreateNewEntity( "Marker" );
        ECS::StaticMeshDesc marker;
        marker.Primitive = Geometry::PrimitiveType::Sphere;
        ECS::SetStaticMesh( *e.GetRegistry(), e.GetHandle(), marker );
        ECS::LocalTransform t = e.GetComponent<ECS::TransformComponent>().GetLocal();
        t.Translation         = position;
        t.Scale               = glm::vec3( scale <= 0.0f ? 0.15f : scale );
        e.SetLocalTransform( t );
        const auto debugColor =
             context->Assets != nullptr
                  ? Assets::FindTemplateByRole( *context->Assets, Common::Content::kDebugColorRole )
                  : Common::MakeError<Common::AssetHandle>( std::string( "no asset manager is bound" ) );
        const auto template_ =
             debugColor ? context->Assets->FindByHandle<Assets::ShaderAsset>( debugColor.GetValue() ) : nullptr;
        if ( template_ == nullptr )
        {
            LOG_ERROR( "[Script] World.spawnMarker: {} — the marker draws its mesh's own material",
                       debugColor ? std::string( "the DebugColor template is not loaded" )
                                  : debugColor.GetError() );
            return e;
        }
        ECS::MaterialDesc desc;
        desc.ShaderName = template_->GetMetadata().Filepath.stem().string();
        desc.Params.push_back( ECS::MaterialParamOverride{ "Color", glm::vec4( color, 1.0f ) } );
        ECS::SetMaterial( *e.GetRegistry(), e.GetHandle(), desc );
        return e;
    }

    HitResult WorldLibrary::Raycast( const glm::vec3& origin, const glm::vec3& direction, float maxDistance )
    {
        HitResult   result;
        const float length = glm::length( direction );
        if ( length <= 0.0f )
        {
            LOG_ERROR( "[Script] World.raycast: the direction is a zero vector" );
            return result;
        }
        const glm::vec3           unit    = direction / length;
        const Core::WorldContext* context = ScriptWorldContext( "World", "raycast" );
        if ( context == nullptr )
            return result;

        Core::RaycastHit hit;
        if ( !context->World->Raycast( Common::Math::Ray( origin, unit ), hit ) || hit.Distance > maxDistance )
            return result;
        result.bBlockingHit = true;
        result.Distance     = hit.Distance;
        result.Location     = hit.Point;
        result.Normal       = hit.Normal;
        if ( auto found = context->World->FindEntityByID( hit.Entity ) )
            result.Entity = ECS::Entity( found->get().GetHandle(), context->World->GetRegistry() );
        return result;
    }

    WorldRay WorldLibrary::CameraRay()
    {
        WorldRay                  ray;
        const Core::WorldContext* context = ScriptWorldContext( "World", "cameraRay" );
        if ( context == nullptr )
            return ray;
        if ( auto camera = context->World->GetActiveCamera() )
        {
            const glm::mat4 inverse = glm::inverse( camera->GetViewMatrix() );
            ray.Origin              = glm::vec3( inverse[3] );
            ray.Direction           = -glm::normalize( glm::vec3( inverse[2] ) );
        }
        return ray;
    }
} // namespace Desert::Libraries
