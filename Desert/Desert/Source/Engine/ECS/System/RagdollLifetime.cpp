#include <Engine/ECS/System/RagdollLifetime.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Physics/RagdollPose.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/gtc/quaternion.hpp>

#include <format>
#include <type_traits>

namespace Desert::ECS
{
    static_assert( std::is_same_v<decltype( RagdollComponent::RuntimeRagdoll ), Physics::RagdollHandle> );

    namespace
    {
        // The world pose: parents composed, as PhysicsECSSystem places a body.
        glm::mat4 EntityWorld( const entt::registry& registry, entt::entity entity )
        {
            glm::mat4    world   = registry.get<TransformComponent>( entity ).GetTransform();
            entt::entity current = entity;
            while ( registry.has<RelationshipComponent>( current ) )
            {
                const auto& relationship = registry.get<RelationshipComponent>( current );
                if ( relationship.Parent == entt::null )
                    break;
                current = relationship.Parent;
                if ( registry.has<TransformComponent>( current ) )
                    world = registry.get<TransformComponent>( current ).GetTransform() * world;
            }
            return world;
        }
    } // namespace

    RagdollLifetime::~RagdollLifetime()
    {
        Detach();
    }

    void RagdollLifetime::Attach( entt::registry& registry )
    {
        if ( m_Registry == &registry )
            return;
        Detach();
        m_Registry = &registry;
        registry.on_destroy<RagdollComponent>().connect<&RagdollLifetime::OnRagdollDestroyed>( *this );
    }

    void RagdollLifetime::Detach()
    {
        if ( m_Registry == nullptr )
            return;
        m_Registry->on_destroy<RagdollComponent>().disconnect<&RagdollLifetime::OnRagdollDestroyed>( *this );
        m_Registry = nullptr;
        m_Refused.clear();
        m_Live.clear();
    }

    void RagdollLifetime::Release( entt::entity entity, uint32_t& runtimeRagdoll )
    {
        m_Live.erase( entity );
        if ( runtimeRagdoll == Physics::kInvalidRagdoll )
            return;
        m_World->RemoveRagdoll( runtimeRagdoll );
        runtimeRagdoll = Physics::kInvalidRagdoll;
    }

    void RagdollLifetime::OnRagdollDestroyed( entt::registry& registry, entt::entity entity )
    {
        m_Refused.erase( entity ); // the id may come back as a new entity
        Release( entity, registry.get<RagdollComponent>( entity ).RuntimeRagdoll );
    }

    void RagdollLifetime::Refuse( entt::entity entity, const std::string& reason )
    {
        m_Refused.insert( entity );
        LOG_ERROR( "[Physics] entity {} has no ragdoll: {}", static_cast<uint32_t>( entity ), reason );
    }

    void RagdollLifetime::DriveBeforeStep( entt::registry& registry, const PhysicsAssetLookup& assets,
                                           const MeshSkeletonLookup& skeletons )
    {
        auto view = registry.view<TransformComponent, RagdollComponent>();
        for ( const auto entity : view )
        {
            auto& ragdoll = view.get<RagdollComponent>( entity );
            if ( m_Refused.contains( entity ) )
                continue;
            if ( !registry.has<AnimationComponent>( entity ) )
            {
                Release( entity, ragdoll.RuntimeRagdoll );
                Refuse( entity, "it has no Animation component: a ragdoll's bodies are the bones of an animated "
                                "skinned mesh" );
                continue;
            }
            auto& anim = registry.get<AnimationComponent>( entity );
            if ( !anim.Animator )
                continue; // the mesh is not resident yet; AnimationECSSystem builds the Animator when it is
            Animation::Animator&       animator = *anim.Animator;
            const Animation::Skeleton& skeleton = animator.GetSkeleton();
            const RagdollData&         data     = ragdoll.Data;

            // A reimported rig (AnimationECSSystem rebuilt the Animator) invalidates the description.
            if ( const auto live = m_Live.find( entity );
                 live != m_Live.end() && live->second.SkeletonSignature != skeleton.GetSignature() )
                Release( entity, ragdoll.RuntimeRagdoll );

            const glm::mat4 world = EntityWorld( registry, entity );
            if ( ragdoll.RuntimeRagdoll == Physics::kInvalidRagdoll )
            {
                if ( data.PhysicsAsset == Assets::AssetHandle::Null() )
                {
                    Refuse( entity, "its Physics Asset is empty: drag a .dephysasset onto it" );
                    continue;
                }
                auto meshSkeleton = skeletons( registry, entity );
                if ( !meshSkeleton.IsSuccess() )
                {
                    Refuse( entity, meshSkeleton.GetError() );
                    continue;
                }
                auto asset = assets( data.PhysicsAsset, meshSkeleton.GetValue(), skeleton.GetName() );
                if ( !asset.IsSuccess() )
                {
                    Refuse( entity, asset.GetError() );
                    continue;
                }
                if ( !asset.GetValue() )
                    continue; // still loading
                auto desc = Physics::BuildRagdollDesc( *asset.GetValue(), skeleton );
                if ( !desc.IsSuccess() )
                {
                    Refuse( entity, desc.GetError() );
                    continue;
                }
                // Placed in the pose being rendered, so the first frame shows no jump.
                auto parts =
                     Physics::RagdollPartsFromPose( desc.GetValue(), skeleton, animator.GetLocalPose(), world );
                if ( !parts.IsSuccess() )
                {
                    Refuse( entity, parts.GetError() );
                    continue;
                }
                auto created = m_World->CreateRagdoll( desc.GetValue(), glm::vec3( world[3] ),
                                                       glm::quat_cast( glm::mat3( world ) ), data.Mode );
                if ( !created.IsSuccess() )
                {
                    Refuse( entity, created.GetError() );
                    continue;
                }
                ragdoll.RuntimeRagdoll = created.GetValue();
                m_World->SetRagdollPose( ragdoll.RuntimeRagdoll, parts.GetValue() );
                m_Live[entity] = Live{ desc.GetValue(), skeleton.GetSignature() };
            }

            const Live& live = m_Live.at( entity );
            // Simulated -> Kinematic snaps to the animation; Kinematic -> Simulated keeps the bodies where the
            // last kinematic step left them (the animated pose) with the velocity it gave them.
            const bool modeChanged = m_World->GetRagdollMotion( ragdoll.RuntimeRagdoll ) != data.Mode;
            if ( !modeChanged && data.Mode == Physics::RagdollMotion::Simulated )
                continue;
            auto parts = Physics::RagdollPartsFromPose( live.Desc, skeleton, animator.GetLocalPose(), world );
            if ( !parts.IsSuccess() )
            {
                Release( entity, ragdoll.RuntimeRagdoll );
                Refuse( entity, parts.GetError() );
                continue;
            }
            if ( modeChanged )
            {
                if ( data.Mode == Physics::RagdollMotion::Kinematic )
                    m_World->SetRagdollPose( ragdoll.RuntimeRagdoll, parts.GetValue() );
                m_World->SetRagdollMotion( ragdoll.RuntimeRagdoll, data.Mode );
            }
            if ( data.Mode == Physics::RagdollMotion::Kinematic )
                m_World->SetRagdollTarget( ragdoll.RuntimeRagdoll, parts.GetValue() );
        }
    }

    void RagdollLifetime::WriteBackAfterStep( entt::registry& registry )
    {
        auto view = registry.view<TransformComponent, RagdollComponent, AnimationComponent>();
        for ( const auto entity : view )
        {
            auto& ragdoll = view.get<RagdollComponent>( entity );
            auto& anim    = view.get<AnimationComponent>( entity );
            if ( ragdoll.RuntimeRagdoll == Physics::kInvalidRagdoll || !anim.Animator ||
                 m_World->GetRagdollMotion( ragdoll.RuntimeRagdoll ) != Physics::RagdollMotion::Simulated )
                continue;
            const auto live = m_Live.find( entity );
            if ( live == m_Live.end() )
                continue;
            Animation::Animator& animator = *anim.Animator;
            m_World->GetRagdollPose( ragdoll.RuntimeRagdoll, m_Parts );
            auto overrides =
                 Physics::RagdollBoneOverrides( live->second.Desc, animator.GetSkeleton(), animator.GetLocalPose(),
                                                EntityWorld( registry, entity ), m_Parts );
            Common::BoolResultStr applied = overrides.IsSuccess()
                                                 ? animator.ApplyPhysicsPose( overrides.GetValue() )
                                                 : Common::MakeError<bool>( overrides.GetError() );
            if ( !applied.IsSuccess() )
            {
                Release( entity, ragdoll.RuntimeRagdoll );
                Refuse( entity, std::format( "its simulated pose does not write back: {}", applied.GetError() ) );
            }
        }
    }
} // namespace Desert::ECS
