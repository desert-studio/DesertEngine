#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <unordered_map>

namespace Desert::ECS
{
    // UE-style socket attachment. For every entity with a SocketAttachmentComponent, follow a BONE of the
    // target (skinned) entity: take the bone's model-space transform from the target's animator pose, lift it
    // to world space with the target's world matrix, apply the local offset, and write the result into this
    // entity's TransformComponent. Runs AFTER AnimationECSSystem (the pose must be current this frame) and
    // before rendering, so the weapon-in-hand never lags a frame.
    // DOES NOT HONOUR VisibilityComponent, AND MUST NOT: it writes a TransformComponent, so a hidden weapon
    // that stopped following its bone would snap to a stale pose the moment it is shown again.
    // Verdict and mutation gate: Desert/Tests/Engine/VisibilityHonoured.
    class AttachmentSystem final : public System
    {
    public:
        explicit AttachmentSystem( Core::Scene* scene ) : m_Scene( scene )
        {
        }

        ~AttachmentSystem() override
        {
            if ( m_HookedRegistry != nullptr )
                m_HookedRegistry->on_destroy<SocketAttachmentComponent>().disconnect( this );
        }

        AttachmentSystem( const AttachmentSystem& )            = delete;
        AttachmentSystem& operator=( const AttachmentSystem& ) = delete;

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ) override
        {
            EnsureDestroyHook( registry );

            auto view = registry.view<SocketAttachmentComponent, TransformComponent>();
            for ( auto entity : view )
            {
                auto& socket = view.get<SocketAttachmentComponent>( entity );
                if ( socket.Target.IsNull() || socket.SocketName.empty() )
                    continue;

                // Resolve the target (skinned) entity + its live animator.
                auto targetRef = m_Scene->FindEntityByID( socket.Target );
                if ( !targetRef )
                    continue;
                ECS::Entity target = targetRef->get();
                if ( !target.HasComponent<AnimationComponent>() )
                    continue;
                const auto& anim = target.GetComponent<AnimationComponent>();
                if ( !anim.Animator )
                    continue; // pose not built yet (e.g. not playing) — leave the weapon where it is

                // THE NAME IS A SOCKET OF THE RIG FIRST, THEN A BONE (UE USkinnedMeshComponent::GetSocketTransform).
                // A socket gives its bone and its local transform, so every attachment naming it follows the one
                // grip authored on the skeleton. Resolved once per (name, rig, the rig's socket set) by
                // Rules::ResolveAttachPoint, so "no name authored" (detached, filtered above) and "a name the rig has neither as a socket
                // nor as a bone" (the attachment stops following, and says why) stay two things.
                const Animation::Skeleton& skeleton = anim.Animator->GetSkeleton();
                Resolved&                  resolved = m_Resolved[entity];
                if ( resolved.Name != socket.SocketName || resolved.Signature != skeleton.GetSignature() ||
                     resolved.AuthoringRevision != skeleton.GetAuthoringRevision() )
                {
                    resolved.Name              = socket.SocketName;
                    resolved.Signature         = skeleton.GetSignature();
                    resolved.AuthoringRevision = skeleton.GetAuthoringRevision();
                    resolved.Point = Rules::ResolveAttachPoint( skeleton, socket.SocketName );
                    if ( !resolved.Point )
                    {
                        LOG_ERROR( "[Attachment] '{}' is neither a socket nor a bone of the target's rig "
                                   "(signature {}, {} bones, {} sockets); this entity will not follow anything.",
                                   socket.SocketName, skeleton.GetSignature(), skeleton.GetBones().size(),
                                   skeleton.GetSockets().size() );
                    }
                }
                if ( !resolved.Point )
                    continue;
                const uint32_t boneIdx = resolved.Point->Bone;

                // The TransformComponent stores a LOCAL transform, so a parented weapon must come back
                // into its parent's space or it doubles the parent's motion. The composition + decompose
                // live in SystemRules.hpp, where a test can reach them without a Scene.
                glm::mat4 parentWorld( 1.0f );
                if ( registry.has<RelationshipComponent>( entity ) )
                {
                    const auto parent = registry.get<RelationshipComponent>( entity ).Parent;
                    if ( parent != entt::null )
                    {
                        ECS::Entity parentEnt{ parent, registry };
                        parentWorld = parentEnt.GetWorldTransform();
                    }
                }

                const glm::mat4 local = Rules::SocketLocalTransform(
                     target.GetWorldTransform(), anim.Animator->GetBoneModelMatrix( boneIdx ) * resolved.Point->SocketLocal,
                     socket.OffsetTranslation, socket.OffsetRotation, socket.OffsetScale, parentWorld );

                const Rules::DecomposedTransform decomposed = Rules::DecomposeTransform( local );

                auto& tc       = view.get<TransformComponent>( entity );
                tc.Translation = decomposed.Translation;
                tc.Rotation    = decomposed.Rotation;
                tc.Scale       = decomposed.Scale;
            }
        }

    private:
        // The two caches below are keyed by the socket entity and used to be erased by nothing: every socket
        // ever destroyed stayed in them for the life of the scene, which streaming turns from a few stale
        // rows into one per unloaded weapon. Same listener shape as ScriptSystem::EnsureDestroyHook.
        void EnsureDestroyHook( entt::registry& registry )
        {
            if ( m_HookedRegistry == &registry )
                return;
            if ( m_HookedRegistry != nullptr )
                m_HookedRegistry->on_destroy<SocketAttachmentComponent>().disconnect( this );
            registry.on_destroy<SocketAttachmentComponent>().connect<&AttachmentSystem::OnSocketDestroyed>( this );
            m_HookedRegistry = &registry;
        }

        void OnSocketDestroyed( entt::registry&, entt::entity entity )
        {
            m_Resolved.erase( entity );
        }

        Core::Scene*    m_Scene          = nullptr;
        entt::registry* m_HookedRegistry = nullptr;

        // Per socket entity: what its name resolved to, and against what. The key is the name, the rig's signature
        // and its authoring revision (a socket edited or a rig re-read at the same address moves it): re-resolving
        // on every frame would defeat the cache, never re-resolving would hand back a bone or a socket transform
        // the target's rig no longer has.
        struct Resolved
        {
            std::string        Name;
            uint64_t           Signature         = 0;
            uint64_t           AuthoringRevision = 0;
            std::optional<Rules::AttachPoint> Point; ///< nullopt = neither a socket nor a bone of the rig
        };
        std::unordered_map<entt::entity, Resolved> m_Resolved;
    };
} // namespace Desert::ECS
