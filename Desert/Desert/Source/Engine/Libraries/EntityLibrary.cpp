#include <Engine/Libraries/EntityLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/Ray.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Desert::Libraries
{
    namespace
    {
        template <class T>
        T* Get( const ECS::Entity& entity )
        {
            return entity ? entity.GetRegistry()->try_get<T>( entity.GetHandle() ) : nullptr;
        }

        // The world of the running script; nullptr (logged) outside a script call.
        const Core::WorldContext* World( const char* function )
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            if ( context == nullptr || context->World == nullptr )
            {
                LOG_ERROR( "[Script] World.{}: no world — called outside a script call", function );
                return nullptr;
            }
            return context;
        }
    } // namespace

    // ── EntityLibrary ──

    std::string EntityLibrary::Name( ECS::Entity entity )
    {
        const auto* tag = Get<ECS::TagComponent>( entity );
        return tag != nullptr ? tag->Tag : std::string();
    }

    void EntityLibrary::AttachTo( ECS::Entity entity, ECS::Entity target, const std::string& bone )
    {
        if ( !entity )
            return;
        Common::UUID targetId;
        if ( const auto* id = Get<ECS::UUIDComponent>( target ) )
            targetId = id->UUID;
        auto& attachment =
             entity.GetRegistry()->get_or_emplace<ECS::SocketAttachmentComponent>( entity.GetHandle() );
        attachment.Target   = targetId;
        attachment.BoneName = bone;
    }

    void EntityLibrary::Detach( ECS::Entity entity )
    {
        if ( entity )
            entity.GetRegistry()->remove<ECS::SocketAttachmentComponent>( entity.GetHandle() );
    }

    glm::vec3 EntityLibrary::GetPosition( ECS::Entity entity )
    {
        const auto* t = Get<ECS::TransformComponent>( entity );
        return t != nullptr ? t->Translation : glm::vec3( 0.0f );
    }

    void EntityLibrary::SetPosition( ECS::Entity entity, const glm::vec3& position )
    {
        if ( auto* t = Get<ECS::TransformComponent>( entity ) )
            t->Translation = position;
    }

    void EntityLibrary::Translate( ECS::Entity entity, const glm::vec3& offset )
    {
        if ( auto* t = Get<ECS::TransformComponent>( entity ) )
            t->Translation += offset;
    }

    glm::vec3 EntityLibrary::GetRotation( ECS::Entity entity )
    {
        const auto* t = Get<ECS::TransformComponent>( entity );
        return t != nullptr ? t->Rotation : glm::vec3( 0.0f );
    }

    void EntityLibrary::SetRotation( ECS::Entity entity, const glm::vec3& euler )
    {
        if ( auto* t = Get<ECS::TransformComponent>( entity ) )
            t->Rotation = euler;
    }

    glm::vec3 EntityLibrary::GetScale( ECS::Entity entity )
    {
        const auto* t = Get<ECS::TransformComponent>( entity );
        return t != nullptr ? t->Scale : glm::vec3( 1.0f );
    }

    void EntityLibrary::SetScale( ECS::Entity entity, const glm::vec3& scale )
    {
        if ( auto* t = Get<ECS::TransformComponent>( entity ) )
            t->Scale = scale;
    }

    glm::vec3 EntityLibrary::Forward( ECS::Entity entity )
    {
        const auto*     t = Get<ECS::TransformComponent>( entity );
        const glm::vec3 forward( 0.0f, 0.0f, -1.0f );
        return t != nullptr ? glm::quat( t->Rotation ) * forward : forward;
    }

    glm::vec3 EntityLibrary::Right( ECS::Entity entity )
    {
        const auto*     t = Get<ECS::TransformComponent>( entity );
        const glm::vec3 right( 1.0f, 0.0f, 0.0f );
        return t != nullptr ? glm::quat( t->Rotation ) * right : right;
    }

    float EntityLibrary::DistanceTo( ECS::Entity entity, ECS::Entity other )
    {
        const auto* a = Get<ECS::TransformComponent>( entity );
        const auto* b = Get<ECS::TransformComponent>( other );
        return ( a != nullptr && b != nullptr ) ? glm::distance( a->Translation, b->Translation ) : -1.0f;
    }

    // ── CharacterLibrary ──

    void CharacterLibrary::Move( ECS::Entity entity, float forward, float right, float speed )
    {
        if ( auto* cc = Get<ECS::CharacterControllerComponent>( entity ) )
        {
            cc->MoveInput    = { right, forward };
            cc->DesiredSpeed = speed;
        }
    }

    void CharacterLibrary::Jump( ECS::Entity entity, float strength )
    {
        if ( auto* cc = Get<ECS::CharacterControllerComponent>( entity ) )
        {
            cc->JumpRequested = true;
            cc->JumpStrength  = strength;
        }
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
        if ( auto* t = Get<ECS::TransformComponent>( entity ) )
            t->Rotation.y += radians;
    }

    void CharacterLibrary::AddCameraPitch( ECS::Entity entity, float radians )
    {
        const auto* rel = Get<ECS::RelationshipComponent>( entity );
        if ( rel == nullptr )
            return;
        entt::registry& reg = *entity.GetRegistry();
        for ( entt::entity child : rel->Children )
        {
            if ( reg.has<ECS::CameraComponent>( child ) && reg.has<ECS::TransformComponent>( child ) )
            {
                auto& rot = reg.get<ECS::TransformComponent>( child ).Rotation;
                rot.x     = glm::clamp( rot.x + radians, glm::radians( -85.0f ), glm::radians( 85.0f ) );
                break;
            }
        }
    }

    // ── MaterialLibrary ──

    namespace
    {
        void UpsertComponentParam( ECS::MaterialComponent& mc, const std::string& name, const glm::vec4& v )
        {
            for ( auto& p : mc.Params )
                if ( p.Name == name )
                {
                    p.Value = v;
                    return;
                }
            mc.Params.push_back( { name, v } );
        }

        // The mesh's live material instance (slot 0), or nullptr.
        std::shared_ptr<Graphic::MaterialInstance> LiveInstance( const ECS::Entity& entity )
        {
            const auto* smc = Get<ECS::StaticMeshComponent>( entity );
            if ( smc == nullptr || smc->RuntimeMaterialInstances.empty() )
                return nullptr;
            return smc->RuntimeMaterialInstances[0];
        }
    } // namespace

    void MaterialLibrary::SetMaterialParam( ECS::Entity entity, const std::string& name, const glm::vec4& value )
    {
        if ( !entity )
            return;
        if ( auto* mc = Get<ECS::MaterialComponent>( entity ); mc != nullptr && !mc->ShaderName.empty() )
        {
            UpsertComponentParam( *mc, name, value );
            return;
        }
        if ( const auto instance = LiveInstance( entity ) )
        {
            instance->SetParamFromVec4( name, value );
            return;
        }
        UpsertComponentParam( entity.GetRegistry()->get_or_emplace<ECS::MaterialComponent>( entity.GetHandle() ),
                              name, value );
    }

    glm::vec4 MaterialLibrary::GetMaterialParam( ECS::Entity entity, const std::string& name )
    {
        if ( const auto* mc = Get<ECS::MaterialComponent>( entity ) )
            for ( const auto& p : mc->Params )
                if ( p.Name == name )
                    return p.Value;
        if ( const auto instance = LiveInstance( entity ) )
        {
            for ( const auto& [pname, prop] : instance->GetPropertySet().GetProperties() )
            {
                if ( pname != name || !prop.bIsOverridden )
                    continue;
                if ( const auto* f = std::get_if<float>( &prop.Value ) )
                    return { *f, 0.0f, 0.0f, 0.0f };
                if ( const auto* v2 = std::get_if<glm::vec2>( &prop.Value ) )
                    return { v2->x, v2->y, 0.0f, 0.0f };
                if ( const auto* v3 = std::get_if<glm::vec3>( &prop.Value ) )
                    return { v3->x, v3->y, v3->z, 1.0f };
                if ( const auto* v4 = std::get_if<glm::vec4>( &prop.Value ) )
                    return *v4;
            }
        }
        return glm::vec4( 0.0f );
    }

    void MaterialLibrary::ClearMaterialParams( ECS::Entity entity )
    {
        if ( auto* mc = Get<ECS::MaterialComponent>( entity ) )
        {
            mc->Params.clear();
            mc->Textures.clear();
        }
        if ( const auto instance = LiveInstance( entity ) )
            instance->ResetOverrides();
    }

    void MaterialLibrary::SetShader( ECS::Entity entity, const std::string& shader )
    {
        if ( entity )
            entity.GetRegistry()->get_or_emplace<ECS::MaterialComponent>( entity.GetHandle() ).ShaderName = shader;
    }

    std::string MaterialLibrary::GetShader( ECS::Entity entity )
    {
        const auto* mc = Get<ECS::MaterialComponent>( entity );
        return mc != nullptr ? mc->ShaderName : std::string();
    }

    // ── WorldLibrary ──

    ECS::Entity WorldLibrary::Find( const std::string& name )
    {
        const Core::WorldContext* context = World( "find" );
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
        const Core::WorldContext* context = World( "spawn" );
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
        const Core::WorldContext* context = World( "spawnMarker" );
        if ( context == nullptr )
            return {};
        ECS::Entity e                                        = context->World->CreateNewEntity( "Marker" );
        e.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Sphere;
        auto& t                                              = e.GetComponent<ECS::TransformComponent>();
        t.Translation                                        = position;
        t.Scale                                              = glm::vec3( scale <= 0.0f ? 0.15f : scale );
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
        auto& mc      = e.AddComponent<ECS::MaterialComponent>();
        mc.ShaderName = template_->GetMetadata().Filepath.stem().string();
        mc.Params.push_back( ECS::MaterialParamOverride{ "Color", glm::vec4( color, 1.0f ) } );
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
        const Core::WorldContext* context = World( "raycast" );
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
        const Core::WorldContext* context = World( "cameraRay" );
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
