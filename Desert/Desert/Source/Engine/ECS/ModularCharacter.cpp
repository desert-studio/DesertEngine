#include <Engine/ECS/ModularCharacter.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/SkeletonReference.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/SkinnedMeshComponent.hpp>
#include <Engine/ECS/SocketAttachmentComponent.hpp>
#include <Engine/ECS/StaticMeshComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/ECS/System/SystemRules.hpp>
#include <Engine/ECS/System/WindField.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>

#include <algorithm>
#include <string>

namespace Desert::ECS
{
    namespace
    {
        // The parsed skinned mesh asset behind @p handle, or why not.
        Common::ResultStr<const Assets::SkinnedMeshAsset*> SkinnedAssetOf( Common::AssetHandle handle,
                                                                           const char*         role ) noexcept
        {
            using R       = const Assets::SkinnedMeshAsset*;
            auto* service = Runtime::ResourceRegistry::GetMeshService();
            if ( service == nullptr )
                return Common::MakeFormattedError<R>( "the {} mesh cannot be resolved: no mesh service", role );
            const auto* asset = dynamic_cast<const Assets::SkinnedMeshAsset*>( service->GetAsset( handle ) );
            if ( asset == nullptr )
            {
                return Common::MakeFormattedError<R>( "the {} mesh {} is not a parsed skinned mesh", role,
                                                      static_cast<uint64_t>( handle ) );
            }
            return Common::MakeSuccess( asset );
        }

        Animation::SkeletonAssetRef SkeletonRefOf( const Assets::SkinnedMeshAsset& asset ) noexcept
        {
            return { asset.GetSkeleton(), asset.GetMetadata().Filepath.string() };
        }

        // The rig the renderer skins @p handle with (the built SkinnedMesh's skeleton), or why not.
        Common::ResultStr<const Animation::Skeleton*> RigOf( Common::AssetHandle handle ) noexcept
        {
            using R               = const Animation::Skeleton*;
            auto*         service = Runtime::ResourceRegistry::GetMeshService();
            Desert::Mesh* mesh    = service != nullptr ? service->Get( handle ) : nullptr;
            if ( mesh == nullptr || mesh->GetType() != Desert::MeshType::Skinned )
            {
                return Common::MakeFormattedError<R>( "the leader's mesh {} is not a built skinned mesh",
                                                      static_cast<uint64_t>( handle ) );
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): MeshType::Skinned = a SkinnedMesh
            const Animation::Skeleton* rig = static_cast<Desert::SkinnedMesh*>( mesh )->GetSkeletonMutable();
            if ( rig == nullptr )
            {
                return Common::MakeFormattedError<R>( "the leader's mesh {} has no skeleton",
                                                      static_cast<uint64_t>( handle ) );
            }
            return Common::MakeSuccess( rig );
        }

        std::string LeaderName( const entt::registry& registry, entt::entity leader ) noexcept
        {
            const auto* tag = registry.try_get<TagComponent>( leader );
            return tag != nullptr ? tag->Tag : std::to_string( static_cast<uint32_t>( leader ) );
        }

        template <class TList>
        auto FindId( TList& list, uint32_t id ) noexcept
        {
            return std::find_if( list.begin(), list.end(), [id]( const auto& entry ) { return entry.Id == id; } );
        }
    } // namespace

    ModularCharacter::ModularCharacter( Core::Scene& scene, entt::entity leader ) noexcept
         : m_Registry( scene.GetRegistry() ), m_Scene( &scene ), m_Leader( leader )
    {
    }

    ModularCharacter::ModularCharacter( entt::registry& registry, entt::entity leader ) noexcept
         : m_Registry( registry ), m_Leader( leader )
    {
    }

    Common::ResultStr<ModularCharacterState*> ModularCharacter::State() noexcept
    {
        using R = ModularCharacterState*;
        if ( !m_Registry.valid( m_Leader ) )
            return Common::MakeError<R>( "the leader entity is not alive" );
        const auto* mesh = m_Registry.try_get<SkinnedMeshComponent>( m_Leader );
        if ( mesh == nullptr )
            return Common::MakeFormattedError<R>( "'{}' has no Skinned Mesh to lead",
                                                  LeaderName( m_Registry, m_Leader ) );
        if ( mesh->GetRuntimeMesh() )
        {
            return Common::MakeFormattedError<R>( "'{}' is an in-editor rig (Convert to Skinned) with no skeleton "
                                                  "asset; a follower names the leader's skeleton by GUID",
                                                  LeaderName( m_Registry, m_Leader ) );
        }
        if ( const auto* link = m_Registry.try_get<Animation::LeaderPoseLink>( m_Leader );
             link != nullptr && link->Follows() )
        {
            return Common::MakeFormattedError<R>( "'{}' is itself a follower: a leader is never a follower (one "
                                                  "level, as UE's leader pose)",
                                                  LeaderName( m_Registry, m_Leader ) );
        }
        return Common::MakeSuccess( &m_Registry.get_or_emplace<ModularCharacterState>( m_Leader ) );
    }

    Common::ResultStr<Core::Scene*> ModularCharacter::SceneOf( const char* call ) const noexcept
    {
        if ( m_Scene == nullptr )
            return Common::MakeFormattedError<Core::Scene*>( "{}: this view of the character only steps its pose",
                                                             call );
        return Common::MakeSuccess( m_Scene );
    }

    Common::ResultStr<Animation::FollowerMeshId>
    ModularCharacter::AttachFollowerMesh( Common::AssetHandle skinnedMesh )
    {
        using R    = Animation::FollowerMeshId;
        auto scene = SceneOf( "AttachFollowerMesh" );
        auto state = State();
        if ( !scene )
            return Common::MakeError<R>( scene.GetError() );
        if ( !state )
            return Common::MakeError<R>( state.GetError() );

        const auto& leaderMesh   = m_Registry.get<SkinnedMeshComponent>( m_Leader );
        const bool  castShadows  = leaderMesh.GetCastShadows();
        auto        leaderAsset  = SkinnedAssetOf( leaderMesh.GetMeshHandle(), "leader" );
        auto        followerMesh = SkinnedAssetOf( skinnedMesh, "follower" );
        if ( !leaderAsset )
            return Common::MakeError<R>( leaderAsset.GetError() );
        if ( !followerMesh )
            return Common::MakeError<R>( followerMesh.GetError() );
        // A follower is skinned with the leader's pose INDEX FOR INDEX: it must name the leader's skeleton
        // itself (the playback rule with no compatible list - a compatible skeleton is another index space).
        if ( auto match = Animation::ClipPlaysOnMesh( SkeletonRefOf( *followerMesh.GetValue() ),
                                                      SkeletonRefOf( *leaderAsset.GetValue() ), {} );
             !match )
        {
            return Common::MakeFormattedError<R>(
                 "follower '{}' cannot follow '{}': {} (a follower is skinned with the leader's bones index for "
                 "index, so both meshes must reference the same skeleton)",
                 followerMesh.GetValue()->GetMetadata().Filepath.string(), LeaderName( m_Registry, m_Leader ),
                 match.GetError() );
        }

        ECS::Entity follower =
             scene.GetValue()->CreateNewEntity( LeaderName( m_Registry, m_Leader ) + ".Follower" );
        SkinnedMeshDesc desc;
        desc.MeshHandle  = skinnedMesh;
        desc.CastShadows = castShadows;
        SetSkinnedMesh( m_Registry, follower.GetHandle(), desc );
        m_Registry.emplace_or_replace<Animation::LeaderPoseLink>(
             follower.GetHandle(), Animation::LeaderPoseLink{ static_cast<uint32_t>( m_Leader ) } );
        // Identity local transform under the leader: the follower's world transform IS the leader's, updated in
        // the same transform pass (never a frame behind), and it is destroyed with the leader's subtree.
        scene.GetValue()->Attach( ECS::Entity( m_Leader, m_Registry ), follower );

        auto&                           s = m_Registry.get<ModularCharacterState>( m_Leader );
        const Animation::FollowerMeshId id{ ++s.NextId };
        s.Followers.push_back( { id.Value, follower.GetHandle() } );
        return Common::MakeSuccess( id );
    }

    Common::BoolResultStr ModularCharacter::DetachFollowerMesh( Animation::FollowerMeshId follower )
    {
        auto scene = SceneOf( "DetachFollowerMesh" );
        auto state = State();
        if ( !scene )
            return Common::MakeError<bool>( scene.GetError() );
        if ( !state )
            return Common::MakeError<bool>( state.GetError() );
        ModularCharacterState& s  = *state.GetValue();
        const auto             it = FindId( s.Followers, follower.Value );
        if ( it == s.Followers.end() )
            return Common::MakeFormattedError<bool>( "unknown follower {}", follower.Value );
        // Its cloth first (the contract's order): a simulation never outlives the mesh it dresses.
        std::erase_if( s.Cloths,
                       [&]( const ModularCharacterState::Cloth& c ) { return c.Follower == follower.Value; } );
        const entt::entity entity = it->Entity;
        s.Followers.erase( it );
        if ( m_Registry.valid( entity ) )
            scene.GetValue()->DestroyEntity( ECS::Entity( entity, m_Registry ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<Animation::SocketAttachmentId>
    ModularCharacter::AttachToSocket( const Animation::SocketAttachmentDesc& desc )
    {
        using R    = Animation::SocketAttachmentId;
        auto scene = SceneOf( "AttachToSocket" );
        auto state = State();
        if ( !scene )
            return Common::MakeError<R>( scene.GetError() );
        if ( !state )
            return Common::MakeError<R>( state.GetError() );
        auto rig = RigOf( m_Registry.get<SkinnedMeshComponent>( m_Leader ).GetMeshHandle() );
        if ( !rig )
            return Common::MakeError<R>( rig.GetError() );
        if ( !Rules::ResolveAttachPoint( *rig.GetValue(), desc.SocketName ) )
        {
            return Common::MakeFormattedError<R>(
                 "'{}' is neither a socket nor a bone of '{}' ({} bones, {} sockets)", desc.SocketName,
                 LeaderName( m_Registry, m_Leader ), rig.GetValue()->GetBones().size(),
                 rig.GetValue()->GetSockets().size() );
        }
        const auto* uuid = m_Registry.try_get<UUIDComponent>( m_Leader );
        if ( uuid == nullptr )
            return Common::MakeFormattedError<R>( "'{}' has no UUID to be a socket target",
                                                  LeaderName( m_Registry, m_Leader ) );
        const Common::UUID target = uuid->UUID;

        ECS::Entity attached =
             scene.GetValue()->CreateNewEntity( LeaderName( m_Registry, m_Leader ) + "." + desc.SocketName );
        StaticMeshDesc mesh;
        mesh.MeshHandle = desc.Mesh;
        SetStaticMesh( m_Registry, attached.GetHandle(), mesh );
        SocketAttachmentComponent socket;
        socket.Target            = target;
        socket.SocketName        = desc.SocketName;
        socket.OffsetTranslation = desc.OffsetTranslation;
        socket.OffsetRotation    = desc.OffsetRotation;
        socket.OffsetScale       = desc.OffsetScale;
        m_Registry.emplace_or_replace<SocketAttachmentComponent>( attached.GetHandle(), socket );
        // A child of the leader so it goes with it; AttachmentSystem composes the socket in the parent's space.
        scene.GetValue()->Attach( ECS::Entity( m_Leader, m_Registry ), attached );

        auto&                               s = m_Registry.get<ModularCharacterState>( m_Leader );
        const Animation::SocketAttachmentId id{ ++s.NextId };
        s.Sockets.push_back( { id.Value, attached.GetHandle() } );
        return Common::MakeSuccess( id );
    }

    Common::BoolResultStr ModularCharacter::DetachFromSocket( Animation::SocketAttachmentId attachment )
    {
        auto scene = SceneOf( "DetachFromSocket" );
        auto state = State();
        if ( !scene )
            return Common::MakeError<bool>( scene.GetError() );
        if ( !state )
            return Common::MakeError<bool>( state.GetError() );
        ModularCharacterState& s  = *state.GetValue();
        const auto             it = FindId( s.Sockets, attachment.Value );
        if ( it == s.Sockets.end() )
            return Common::MakeFormattedError<bool>( "unknown socket attachment {}", attachment.Value );
        const entt::entity entity = it->Entity;
        s.Sockets.erase( it );
        if ( m_Registry.valid( entity ) )
            scene.GetValue()->DestroyEntity( ECS::Entity( entity, m_Registry ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ModularCharacter::SetBodySectionVisible( uint32_t section, bool visible )
    {
        auto state = State();
        if ( !state )
            return Common::MakeError<bool>( state.GetError() );
        auto asset = SkinnedAssetOf( m_Registry.get<SkinnedMeshComponent>( m_Leader ).GetMeshHandle(), "leader" );
        if ( !asset )
            return Common::MakeError<bool>( asset.GetError() );
        const size_t sections = asset.GetValue()->GetSubmeshes().size();
        if ( section >= sections )
            return Common::MakeFormattedError<bool>( "section {} >= the body's {} sections", section, sections );
        if ( section >= 64 )
            return Common::MakeFormattedError<bool>( "section {}: a draw's section mask holds 64", section );
        auto&          hidden = m_Registry.get_or_emplace<Animation::HiddenSections>( m_Leader );
        const uint64_t bit    = 1ull << section;
        hidden.Mask           = visible ? ( hidden.Mask & ~bit ) : ( hidden.Mask | bit );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<Animation::ClothInstanceId>
    ModularCharacter::CreateCloth( Animation::FollowerMeshId follower, Common::AssetHandle clothAsset,
                                   std::string_view backend )
    {
        using R    = Animation::ClothInstanceId;
        auto state = State();
        if ( !state )
            return Common::MakeError<R>( state.GetError() );
        if ( FindId( state.GetValue()->Followers, follower.Value ) == state.GetValue()->Followers.end() )
            return Common::MakeFormattedError<R>( "unknown follower {}", follower.Value );
        const ClothServices* services = m_Registry.try_ctx<ClothServices>();
        if ( services == nullptr || services->Backends == nullptr || !services->FindAsset )
        {
            return Common::MakeError<R>( "this world has no cloth services (a backend registry and cloth assets, "
                                         "put in the registry context by the cloth backend's owner)" );
        }
        auto factory = services->Backends->Find( backend );
        if ( !factory )
            return Common::MakeError<R>( factory.GetError() );
        auto asset = services->FindAsset( clothAsset );
        if ( !asset )
            return Common::MakeError<R>( asset.GetError() );
        if ( asset.GetValue() == nullptr )
            return Common::MakeFormattedError<R>( "cloth asset {} resolved to nothing",
                                                  static_cast<uint64_t>( clothAsset ) );
        const Physics::Cloth::ClothingAsset& cloth = *asset.GetValue();
        if ( auto supported = factory.GetValue()->SupportsAsset( cloth ); !supported )
            return Common::MakeError<R>( supported.GetError() );
        auto rig = RigOf( m_Registry.get<SkinnedMeshComponent>( m_Leader ).GetMeshHandle() );
        if ( !rig )
            return Common::MakeError<R>( rig.GetError() );
        std::vector<uint32_t> bones;
        bones.reserve( cloth.UsedBoneNames.size() );
        for ( const std::string& name : cloth.UsedBoneNames )
        {
            const auto bone = rig.GetValue()->FindBoneIndex( name );
            if ( !bone )
            {
                return Common::MakeFormattedError<R>( "cloth '{}' uses bone '{}', which '{}' does not have",
                                                      cloth.Name, name, LeaderName( m_Registry, m_Leader ) );
            }
            bones.push_back( static_cast<uint32_t>( *bone ) );
        }
        auto simulation = factory.GetValue()->CreateSimulation(
             cloth, ECS::Entity( m_Leader, m_Registry ).GetWorldTransform() );
        if ( !simulation )
            return Common::MakeError<R>( simulation.GetError() );

        auto&                            s = m_Registry.get<ModularCharacterState>( m_Leader );
        const Animation::ClothInstanceId id{ ++s.NextId };
        s.Cloths.push_back( { id.Value, follower.Value, std::move( bones ), simulation.ExtractValue() } );
        return Common::MakeSuccess( id );
    }

    Common::BoolResultStr ModularCharacter::DestroyCloth( Animation::ClothInstanceId cloth )
    {
        auto state = State();
        if ( !state )
            return Common::MakeError<bool>( state.GetError() );
        if ( std::erase_if( state.GetValue()->Cloths,
                            [&]( const ModularCharacterState::Cloth& c ) { return c.Id == cloth.Value; } ) == 0 )
            return Common::MakeFormattedError<bool>( "unknown cloth instance {}", cloth.Value );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ModularCharacter::ApplyLeaderPose( const Animation::LeaderPose& pose,
                                                             float                        fixedDeltaSeconds )
    {
        if ( !( fixedDeltaSeconds > 0.0f ) )
            return Common::MakeFormattedError<bool>( "a step of {} s: the cloth step is fixed and positive",
                                                     fixedDeltaSeconds );
        auto state = State();
        if ( !state )
            return Common::MakeError<bool>( state.GetError() );
        ModularCharacterState& s = *state.GetValue();
        for ( ModularCharacterState::Cloth& cloth : s.Cloths )
        {
            s.ScratchClothBones.clear();
            for ( const uint32_t bone : cloth.Bones )
            {
                if ( bone >= pose.BoneModelTransforms.size() )
                {
                    return Common::MakeFormattedError<bool>( "cloth {} needs bone {}, the pose has {}", cloth.Id,
                                                             bone, pose.BoneModelTransforms.size() );
                }
                s.ScratchClothBones.push_back( pose.BoneModelTransforms[bone] );
            }
            Physics::Cloth::ClothStepContext context;
            context.FixedDeltaSeconds   = fixedDeltaSeconds;
            context.ComponentToWorld    = pose.ComponentToWorld;
            context.BoneModelTransforms = s.ScratchClothBones;
            context.Gravity             = pose.Gravity;
            context.WindVelocity        = pose.WindVelocity;
            if ( auto stepped = cloth.Simulation->Step( context ); !stepped )
                return Common::MakeFormattedError<bool>( "cloth {}: {}", cloth.Id, stepped.GetError() );
        }
        return Common::MakeSuccess( true );
    }

    void ModularCharacter::StepAll( entt::registry& registry, float fixedDeltaSeconds,
                                    float gravityCmPerS2 ) noexcept
    {
        auto characters = registry.view<ModularCharacterState, AnimationComponent>();
        for ( const entt::entity leader : characters )
        {
            ModularCharacterState& state = characters.get<ModularCharacterState>( leader );
            if ( state.Cloths.empty() )
                continue;
            const auto& animation = characters.get<AnimationComponent>( leader );
            if ( !animation.Animator )
                continue; // no pose yet: the cloth stays in the reference pose it was created in
            const size_t boneCount = animation.Animator->GetSkeleton().GetBones().size();
            state.ScratchPose.resize( boneCount );
            for ( size_t i = 0; i < boneCount; ++i )
                state.ScratchPose[i] = animation.Animator->GetBoneModelMatrix( static_cast<uint32_t>( i ) );

            Animation::LeaderPose pose;
            pose.BoneModelTransforms = state.ScratchPose;
            pose.ComponentToWorld    = ECS::Entity( leader, registry ).GetWorldTransform();
            pose.Gravity             = { 0.0f, -gravityCmPerS2, 0.0f };
            pose.WindVelocity        = WindAt( registry, glm::vec3( pose.ComponentToWorld[3] ) ).Velocity;

            ModularCharacter              stepper( registry, leader );
            Animation::IModularCharacter& character = stepper;
            if ( auto applied = character.ApplyLeaderPose( pose, fixedDeltaSeconds ); !applied )
                LOG_ERROR( "[ModularCharacter] '{}': {}", LeaderName( registry, leader ), applied.GetError() );
        }
    }
} // namespace Desert::ECS
