#include <Engine/ECS/System/CharacterMovement.hpp>

#include <algorithm>

namespace Desert::ECS::CharacterMovement
{
    namespace
    {
        // UE ApplyVelocityBraking's sub-step (MaxSimulationTimeStep's braking twin, BrakingSubStepTime = 1/33).
        constexpr float kBrakingSubStep = 1.0f / 33.0f;

        // UE ApplyVelocityBraking: velocity-proportional friction plus a constant deceleration opposing the
        // velocity at the start of the step; never reverses the velocity, and stops it below
        // kBrakeToStopVelocity.
        glm::vec3 ApplyBraking( glm::vec3 velocity, float friction, float deceleration, float dt )
        {
            const float speed = glm::length( velocity );
            if ( speed < 1e-4f || dt <= 0.0f || ( friction <= 0.0f && deceleration <= 0.0f ) )
                return velocity;

            const glm::vec3 old      = velocity;
            const glm::vec3 revAccel = deceleration > 0.0f ? -velocity / speed * deceleration : glm::vec3( 0.0f );
            const bool      noFriction = friction <= 0.0f;
            float           remaining  = dt;
            while ( remaining >= 1e-6f )
            {
                const float step = ( remaining > kBrakingSubStep && !noFriction )
                                        ? std::min( kBrakingSubStep, remaining * 0.5f )
                                        : remaining;
                remaining -= step;
                velocity += ( -friction * velocity + revAccel ) * step;
                if ( glm::dot( velocity, old ) <= 0.0f )
                    return glm::vec3( 0.0f );
            }
            if ( glm::length( velocity ) < kBrakeToStopVelocity )
                return glm::vec3( 0.0f );
            return velocity;
        }

        float StandingHalfHeight( const CharacterControllerData& data )
        {
            return CylinderHalfHeight( data.Height, data.Radius );
        }

        float CrouchedHalfHeight( const CharacterControllerData& data )
        {
            return std::min( CylinderHalfHeight( data.CrouchedHeight, data.Radius ), StandingHalfHeight( data ) );
        }

        // UE Crouch / UnCrouch with bCrouchMaintainsBaseLocation on the ground: the feet stay, the centre moves
        // by the half-height difference; in the air the centre stays.
        void UpdateCrouch( CharacterControllerComponent& cc, Physics::PhysicsWorld& world )
        {
            const CharacterControllerData& data   = cc.Data;
            const bool                     wants  = cc.CrouchRequested && !cc.Swimming;
            const float                    delta  = StandingHalfHeight( data ) - CrouchedHalfHeight( data );
            const bool                     ground = world.IsCharacterOnGround( cc.RuntimeCharacter );
            const glm::vec3                center = world.GetCharacterPosition( cc.RuntimeCharacter );
            const glm::vec3                shift( 0.0f, ground ? delta : 0.0f, 0.0f );

            if ( wants && !cc.IsCrouched )
            {
                if ( !world.SetCharacterCapsule( cc.RuntimeCharacter, data.Radius, CrouchedHalfHeight( data ) ) )
                    return;
                world.SetCharacterPosition( cc.RuntimeCharacter, center - shift );
                cc.IsCrouched = true;
            }
            else if ( !wants && cc.IsCrouched )
            {
                if ( !CanUncrouch( cc, world ) )
                    return; // UE: stays crouched while bWantsToCrouch is false, and retries every frame
                world.SetCharacterPosition( cc.RuntimeCharacter, center + shift );
                if ( !world.SetCharacterCapsule( cc.RuntimeCharacter, data.Radius, StandingHalfHeight( data ) ) )
                {
                    world.SetCharacterPosition( cc.RuntimeCharacter, center );
                    return;
                }
                cc.IsCrouched = false;
            }
        }
    } // namespace

    float CylinderHalfHeight( float height, float radius )
    {
        return std::max( ( height - 2.0f * radius ) * 0.5f, 1.0f );
    }

    void CreateCharacter( CharacterControllerComponent& cc, Physics::PhysicsWorld& world, const glm::vec3& center )
    {
        const CharacterControllerData& data = cc.Data;
        Physics::CharacterDesc         desc;
        desc.Radius         = data.Radius;
        desc.HalfHeight     = StandingHalfHeight( data );
        desc.Position       = center;
        desc.MaxSlopeDeg    = data.MaxSlopeDeg;
        cc.RuntimeCharacter = world.CreateCharacter( desc );
        cc.Velocity         = glm::vec3( 0.0f );
        cc.CurrentSpeed     = 0.0f;
        cc.IsCrouched       = false;
    }

    float MaxSpeed( const CharacterControllerComponent& cc )
    {
        const CharacterControllerData& data = cc.Data;
        return cc.IsCrouched ? data.MaxWalkSpeedCrouched : data.MaxWalkSpeed;
    }

    glm::vec3 CalcVelocity( const glm::vec3& velocity, const glm::vec3& acceleration, float maxSpeed,
                            float analogInput, float friction, float brakingFriction, float brakingDeceleration,
                            float dt )
    {
        glm::vec3   v         = velocity;
        const float accelLen  = glm::length( acceleration );
        const bool  zeroAccel = accelLen < 1e-4f;
        const float speed     = glm::length( v );
        const bool  overMax   = speed > maxSpeed * 1.01f;

        if ( zeroAccel || overMax )
        {
            const glm::vec3 old = v;
            v                   = ApplyBraking( v, brakingFriction, brakingDeceleration, dt );
            // UE: braking must not take a speed that was over the max (and still pushed along) below the max.
            if ( overMax && !zeroAccel && glm::length( v ) < maxSpeed && glm::dot( acceleration, old ) > 0.0f )
                v = old / speed * maxSpeed;
        }
        else
        {
            // Turning friction: the velocity swings toward the acceleration's direction, its size kept.
            const glm::vec3 dir = acceleration / accelLen;
            v -= ( v - dir * speed ) * std::min( dt * friction, 1.0f );
        }

        if ( !zeroAccel )
        {
            const float maxInputSpeed = maxSpeed * glm::clamp( analogInput, 0.0f, 1.0f );
            v += acceleration * dt;
            const float len = glm::length( v );
            if ( len > maxInputSpeed && len > 0.0f )
                v *= maxInputSpeed / len;
        }
        return v;
    }

    bool CanUncrouch( const CharacterControllerComponent& cc, const Physics::PhysicsWorld& world )
    {
        const CharacterControllerData& data   = cc.Data;
        const float                    stand  = StandingHalfHeight( data );
        const float                    delta  = stand - CrouchedHalfHeight( data );
        const bool                     ground = world.IsCharacterOnGround( cc.RuntimeCharacter );
        const glm::vec3                center =
             world.GetCharacterPosition( cc.RuntimeCharacter ) + glm::vec3( 0.0f, ground ? delta : 0.0f, 0.0f );
        return !world.OverlapsCapsule( center, std::max( data.Radius - kUncrouchProbeInset, 1.0f ),
                                       std::max( stand - kUncrouchProbeInset, 0.01f ) );
    }

    void Step( CharacterControllerComponent& cc, Physics::PhysicsWorld& world, const glm::vec3& worldInput,
               float dt )
    {
        if ( cc.RuntimeCharacter == Physics::kInvalidCharacter || dt <= 0.0f )
            return;
        const CharacterControllerData& data = cc.Data;

        UpdateCrouch( cc, world ); // UE UpdateCharacterStateBeforeMovement

        const bool onGround = world.IsCharacterOnGround( cc.RuntimeCharacter );
        cc.OnGround         = onGround;
        const float gravity = world.GetGravity() * data.GravityScale;

        glm::vec3   input( worldInput.x, 0.0f, worldInput.z );
        const float inputLen = glm::length( input );
        if ( inputLen > 1.0f )
            input /= inputLen;
        const float analog = std::min( inputLen, 1.0f );

        glm::vec3 planar( cc.Velocity.x, 0.0f, cc.Velocity.z );
        float     vertical = cc.Velocity.y;
        if ( cc.Swimming )
        {
            planar   = input * data.MaxSwimSpeed;
            vertical = glm::clamp( cc.SwimVertical, -1.0f, 1.0f ) * data.MaxSwimSpeed;
        }
        else if ( onGround )
        {
            planar = CalcVelocity( planar, input * data.MaxAcceleration, MaxSpeed( cc ), analog,
                                   data.GroundFriction, data.GroundFriction * data.BrakingFrictionFactor,
                                   data.BrakingDecelerationWalking, dt );
            // UE CanJump: not while crouched. Otherwise one frame of gravity keeps the capsule on the floor.
            vertical = ( cc.JumpRequested && !cc.IsCrouched ) ? data.JumpZVelocity : -gravity * dt;
        }
        else
        {
            // MOVE_Falling: no lateral friction and no braking (UE FallingLateralFriction = 0,
            // BrakingDecelerationFalling = 0), the input's acceleration scaled by Air Control.
            planar = CalcVelocity( planar, input * ( data.MaxAcceleration * data.AirControl ), data.MaxWalkSpeed,
                                   analog, 0.0f, 0.0f, 0.0f, dt );
            vertical -= gravity * dt;
        }
        cc.JumpRequested = false; // one-shot, consumed

        const glm::vec3 before = world.GetCharacterPosition( cc.RuntimeCharacter );
        world.UpdateCharacter( cc.RuntimeCharacter, glm::vec3( planar.x, vertical, planar.z ), dt );
        const glm::vec3 moved = ( world.GetCharacterPosition( cc.RuntimeCharacter ) - before ) / dt;

        // What the world let happen (a wall stops the planar part, the floor or a ceiling the vertical) — the
        // velocity the next frame accelerates from, as UE's Velocity after SafeMoveUpdatedComponent.
        cc.Velocity = glm::vec3( moved.x, onGround && !cc.Swimming && vertical < 0.0f ? 0.0f : moved.y, moved.z );
        cc.CurrentSpeed = glm::length( glm::vec2( cc.Velocity.x, cc.Velocity.z ) );
    }
} // namespace Desert::ECS::CharacterMovement
