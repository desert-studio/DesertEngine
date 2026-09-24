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
                if ( socket.Target.IsNull() || socket.BoneName.empty() )
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

                // A BoneRef, so the two ways this can fail stay two things. `if ( !FindBoneIndex ) continue;`
                // collapsed them: a socket with no name authored (detached — correct, say nothing) and a
                // socket naming a bone the target's rig does not have (the weapon silently stops following,
                // and nothing anywhere says why) took the same branch. The empty name is already filtered
                // above; what is left here is only the second case, and it now has a voice.
                const Animation::Skeleton& skeleton = anim.Animator->GetSkeleton();
                auto&                      ref      = m_BoneRefs[entity];
                if ( ref.GetName() != socket.BoneName || m_RefRig[entity] != skeleton.GetSignature() )
                {
                    ref.SetName( socket.BoneName );
                    m_RefRig[entity] = skeleton.GetSignature();
                    if ( !ref.Resolve( skeleton ) )
                    {
                        LOG_ERROR( "[Attachment] socket bone '{}' is not a bone of the target's rig "
                                   "(signature {}, {} bones); this entity will not follow anything.",
                                   socket.BoneName, skeleton.GetSignature(), skeleton.GetBones().size() );
                    }
                }
                if ( !ref.IsResolved() )
                    continue;
                const uint32_t boneIdx = ref.GetIndex();

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
                     target.GetWorldTransform(), anim.Animator->GetBoneModelMatrix( boneIdx ),
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
            m_BoneRefs.erase( entity );
            m_RefRig.erase( entity );
        }

        Core::Scene*    m_Scene          = nullptr;
        entt::registry* m_HookedRegistry = nullptr;

        // The cached bone index per socket entity, plus the rig signature it was resolved against. The
        // signature is the invalidation key: re-resolving on every frame would defeat the cache, and never
        // re-resolving would hand back an index from a rig the target no longer has.
        std::unordered_map<entt::entity, Animation::BoneRef> m_BoneRefs;
        std::unordered_map<entt::entity, uint64_t>           m_RefRig;
    };
} // namespace Desert::ECS
