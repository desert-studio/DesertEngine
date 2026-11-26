#include <Engine/ECS/System/CharacterMovement.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/RelationshipComponent.hpp>
#include <Engine/ECS/SkinnedMeshComponent.hpp>
#include <Engine/ECS/StaticMeshComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/Gameplay/CharacterMoveStep.hpp>
#include <Engine/Gameplay/PawnPrefabMovement.hpp>
#include <Engine/Gameplay/PhysicsPawnMoveWorld.hpp>
#include <Engine/Physics/CharacterControllerComponent.hpp>

#include <algorithm>

namespace Desert::ECS::CharacterMovement
{
    float CylinderHalfHeight( float height, float radius )
    {
        return std::max( ( height - 2.0f * radius ) * 0.5f, 1.0f );
    }

    Common::ResultStr<Physics::CharacterHandle>
    CreateCharacter( CharacterControllerComponent& cc, Physics::PhysicsWorld& world, const glm::vec3& center )
    {
        const CharacterControllerData& data = cc.Data;
        Physics::CharacterDesc         desc;
        desc.Radius      = data.Radius;
        desc.HalfHeight  = CylinderHalfHeight( data.Height, data.Radius );
        desc.Position    = center;
        desc.MaxSlopeDeg = data.MaxSlopeDeg;
        auto profile     = world.GetCollisionProfiles().Resolve( data.CollisionProfile );
        if ( !profile )
            return Common::MakeError<Physics::CharacterHandle>( profile.GetError() );
        desc.Profile = profile.GetValue();
        auto created = world.CreateCharacter( desc );
        if ( !created )
            return created;
        cc.RuntimeCharacter = created.GetValue();
        cc.Velocity         = glm::vec3( 0.0f );
        cc.CurrentSpeed     = 0.0f;
        cc.IsCrouched       = false;
        cc.JumpHeld         = false;
        return created;
    }

    void Step( CharacterControllerComponent& cc, Physics::PhysicsWorld& world, const Net::InputStep& input,
               float dt )
    {
        if ( cc.RuntimeCharacter == Physics::kInvalidCharacter || dt <= 0.0f )
            return;
        Gameplay::PawnMovement movement = Gameplay::PrefabMovement::ToPawnMovement( cc.Data );
        // The scene's gravity (Scene Settings), the one the world presses the floor's bodies with.
        movement.Settings.GravityScale = cc.Data.GravityScale; // times the scene's (PhysicsPawnMoveWorld::Gravity)

        // The feet under the capsule the world holds (its record of the shape is the one; the step's crouch
        // changes it through the move), not under one re-derived from IsCrouched.
        Gameplay::PhysicsPawnMoveWorld moveWorld( world, cc.RuntimeCharacter );
        const Gameplay::PawnCapsule&   capsule = moveWorld.Capsule();
        const glm::vec3                center  = world.GetCharacterPosition( cc.RuntimeCharacter );
        Net::PawnMoveState             from;
        from.Position = { center.x, center.y - ( capsule.HalfHeight + capsule.Radius ), center.z };
        from.Velocity = { cc.Velocity.x, cc.Velocity.y, cc.Velocity.z };
        from.Grounded = world.IsCharacterOnGround( cc.RuntimeCharacter );
        from.Mode     = static_cast<std::uint8_t>( ( cc.IsCrouched ? Gameplay::kModeCrouched : 0u ) |
                                                   ( cc.Swimming ? Gameplay::kModeSwimming : 0u ) |
                                                   ( cc.JumpHeld ? Gameplay::kModeJumpHeld : 0u ) );
        const Net::NetVec3       root{ cc.RootMotionVelocity.x, cc.RootMotionVelocity.y, cc.RootMotionVelocity.z };
        const Net::PawnMoveState to =
             Gameplay::StepPawnMove( from, input, movement, moveWorld, dt, cc.HasRootMotion ? &root : nullptr );
        ApplyMoveState( cc, to );
    }

    void ApplyMoveState( CharacterControllerComponent& cc, const Net::PawnMoveState& state )
    {
        cc.IsCrouched   = ( state.Mode & Gameplay::kModeCrouched ) != 0;
        cc.Swimming     = ( state.Mode & Gameplay::kModeSwimming ) != 0;
        cc.JumpHeld     = ( state.Mode & Gameplay::kModeJumpHeld ) != 0;
        cc.OnGround     = state.Grounded;
        cc.Velocity     = glm::vec3( state.Velocity.X, state.Velocity.Y, state.Velocity.Z );
        cc.CurrentSpeed = glm::length( glm::vec2( cc.Velocity.x, cc.Velocity.z ) );
    }

    void PlaceDrawnPawn( entt::registry& registry, entt::entity pawn, CharacterControllerComponent& cc,
                         const glm::vec3& center, float meshLift )
    {
        ECS::SetLocalTranslation( registry, pawn, center );
        const float delta = meshLift - cc.MeshLift;
        if ( delta == 0.0f )
            return;
        cc.MeshLift = meshLift;
        const auto* family = registry.try_get<RelationshipComponent>( pawn );
        if ( !family )
            return;
        for ( const entt::entity child : family->Children )
        {
            if ( !registry.valid( child ) || !registry.has<TransformComponent>( child ) )
                continue;
            if ( !registry.has<StaticMeshComponent>( child ) && !registry.has<SkinnedMeshComponent>( child ) )
                continue;
            LocalTransform local = registry.get<TransformComponent>( child ).GetLocal();
            local.Translation.y += delta;
            ECS::SetLocalTransform( registry, child, local );
        }
    }

    void PlaceDrawnCharacter( entt::registry& registry, entt::entity pawn, CharacterControllerComponent& cc,
                              const Physics::PhysicsWorld& world )
    {
        const Physics::CharacterCapsule held  = world.GetCharacterCapsule( cc.RuntimeCharacter );
        const Gameplay::DrawnPawn       drawn = Gameplay::DrawnPawnOf(
             Gameplay::PrefabMovement::ToPawnMoveSettings( cc.Data ),
             Gameplay::PawnCapsule{ held.Radius, held.HalfHeight } );
        PlaceDrawnPawn( registry, pawn, cc, world.GetInterpolatedCharacterPosition( cc.RuntimeCharacter ),
                        drawn.MeshLift );
    }

    void PublishAnimGraphParameters( const CharacterControllerComponent& cc, AnimationComponent& anim )
    {
        if ( !anim.Graph )
            return;

        const auto publish = [&anim]( const char* name, float value )
        {
            const auto& declared = anim.Graph->Parameters;
            if ( std::none_of( declared.begin(), declared.end(),
                               [name]( const Animation::Graph::Parameter& p ) { return p.Name == name; } ) )
                return;
            auto&      pending = anim.PendingGraphParams;
            const auto queued  = std::find_if( pending.begin(), pending.end(),
                                               [name]( const AnimationComponent::PendingGraphParam& p )
                                               { return p.Name == name; } );
            if ( queued != pending.end() )
                queued->Value = value;
            else
                pending.push_back( { name, value } );
        };
        publish( kAnimParamSpeed, cc.CurrentSpeed );
        publish( kAnimParamIsFalling, !cc.OnGround && !cc.Swimming ? 1.0f : 0.0f );
        publish( kAnimParamIsCrouched, cc.IsCrouched ? 1.0f : 0.0f );
    }
} // namespace Desert::ECS::CharacterMovement
