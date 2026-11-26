#include <Engine/Gameplay/CharacterMoveStep.hpp>

#include <format>

#include <algorithm>
#include <cmath>

namespace Desert::Gameplay
{
    namespace
    {
        struct Planar
        {
            float X = 0.f, Z = 0.f;
        };
        float Length( Planar v )
        {
            return std::sqrt( v.X * v.X + v.Z * v.Z );
        }
        Planar Scaled( Planar v, float s )
        {
            return { v.X * s, v.Z * s };
        }
        Planar SafeNormal( Planar v )
        {
            const float length = Length( v );
            return length > 1e-6f ? Scaled( v, 1.0f / length ) : Planar{};
        }

        // UE UCharacterMovementComponent::ApplyVelocityBraking (sub-stepped at 1/33 s, stops below 10 cm/s).
        Planar ApplyBraking( Planar velocity, float dt, float friction, float deceleration )
        {
            if ( Length( velocity ) < 1e-6f || ( friction == 0.f && deceleration == 0.f ) )
                return velocity;
            const Planar    old         = velocity;
            const Planar    reverse     = Scaled( SafeNormal( velocity ), -deceleration );
            constexpr float kMaxSubstep = 1.0f / 33.0f;
            float           remaining   = dt;
            while ( remaining >= 1e-6f )
            {
                const float step = remaining > kMaxSubstep ? std::min( kMaxSubstep, remaining * 0.5f ) : remaining;
                remaining -= step;
                velocity.X += ( -friction * velocity.X + reverse.X ) * step;
                velocity.Z += ( -friction * velocity.Z + reverse.Z ) * step;
                if ( velocity.X * old.X + velocity.Z * old.Z <= 0.f )
                    return {};
            }
            constexpr float kBrakeToStop = 10.0f;
            return Length( velocity ) < kBrakeToStop ? Planar{} : velocity;
        }

        // UE UCharacterMovementComponent::CalcVelocity on the planar velocity; @p analog is the stick's length (UE
        // AnalogInputModifier: the input speed is the max speed times it).
        Planar CalcVelocity( Planar velocity, Planar acceleration, float dt, float maxSpeed, float analog,
                             float friction, float brakingFrictionFactor, float brakingDeceleration )
        {
            const bool zeroAcceleration = Length( acceleration ) < 1e-6f;
            const bool overMax          = Length( velocity ) > maxSpeed * 1.01f;
            if ( zeroAcceleration || overMax )
            {
                const Planar old = velocity;
                velocity = ApplyBraking( velocity, dt, friction * brakingFrictionFactor, brakingDeceleration );
                if ( overMax && Length( velocity ) < maxSpeed &&
                     acceleration.X * old.X + acceleration.Z * old.Z > 0.f )
                    velocity = Scaled( SafeNormal( old ), maxSpeed );
            }
            else
            {
                const Planar direction = SafeNormal( acceleration );
                const float  speed     = Length( velocity );
                const float  turn      = std::min( dt * friction, 1.0f );
                velocity.X -= ( velocity.X - direction.X * speed ) * turn;
                velocity.Z -= ( velocity.Z - direction.Z * speed ) * turn;
            }
            if ( !zeroAcceleration )
            {
                const float maxInputSpeed = maxSpeed * std::clamp( analog, 0.0f, 1.0f );
                const float newMax =
                     Length( velocity ) > maxInputSpeed * 1.01f ? Length( velocity ) : maxInputSpeed;
                velocity.X += acceleration.X * dt;
                velocity.Z += acceleration.Z * dt;
                if ( const float speed = Length( velocity ); speed > newMax )
                    velocity = Scaled( velocity, newMax / speed );
            }
            return velocity;
        }
    } // namespace

    std::optional<std::size_t> PawnActions::IndexOf( std::string_view name ) const
    {
        for ( std::size_t i = 0; i < Names.size(); ++i )
            if ( Names[i] == name )
                return i;
        return std::nullopt;
    }

    std::uint32_t PawnActions::Bit( std::string_view name ) const
    {
        const auto index = IndexOf( name );
        return index ? 1u << *index : 0u;
    }

    bool PawnActions::Held( const InputStep& input, std::string_view name ) const
    {
        return ( input.Actions & Bit( name ) ) != 0;
    }

    std::string PawnActions::Describe( const InputStep& input ) const
    {
        std::string text;
        for ( std::size_t i = 0; i < Names.size(); ++i )
            if ( ( input.Actions >> i ) & 1u )
                text += std::format( "{}{}#{}", text.empty() ? "" : " ", Names[i], i );
        for ( std::size_t i = Names.size(); i < Net::kMaxInputActions; ++i )
            if ( ( input.Actions >> i ) & 1u )
                text += std::format( "{}undeclared#{}", text.empty() ? "" : " ", i );
        return text.empty() ? std::string( "none" ) : text;
    }

    PawnActions PawnActionsOf( const ECS::CharacterControllerData& controller )
    {
        PawnActions actions;
        for ( const std::string* name :
              { &controller.JumpAction, &controller.SprintAction, &controller.CrouchAction } )
            if ( !name->empty() && !actions.IndexOf( *name ) )
                actions.Names.push_back( *name );
        actions.Jump   = controller.JumpAction;
        actions.Sprint = controller.SprintAction;
        actions.Crouch = controller.CrouchAction;
        return actions;
    }

    PawnCapsule CapsuleOf( const PawnMoveSettings& settings, bool crouched )
    {
        return { settings.CapsuleRadius, crouched ? settings.CrouchedHalfHeight : settings.CapsuleHalfHeight };
    }

    PawnMoveState StepPawnMove( const PawnMoveState& from, const InputStep& input, const PawnMovement& movement,
                                IPawnMoveWorld& world, float dtSeconds, const NetVec3* rootMotionVelocity )
    {
        const PawnMoveSettings& settings = movement.Settings;
        const PawnActions&      actions  = movement.Actions;
        PawnMoveState           state    = from;
        const bool              swimming = ( state.Mode & kModeSwimming ) != 0;
        bool                    crouched = ( state.Mode & kModeCrouched ) != 0;

        // UE UpdateCharacterStateBeforeMovement: crouch while the Crouch action is held (not swimming), stand up
        // once it is released and the standing capsule fits. On the floor the feet stay
        // (bCrouchMaintainsBaseLocation); in the air the capsule's centre stays, so the feet move by the
        // half-height difference.
        const float halfDelta   = settings.CapsuleHalfHeight - settings.CrouchedHalfHeight;
        const bool  wantsCrouch = actions.Held( input, actions.Crouch ) && !swimming;
        if ( wantsCrouch && !crouched )
        {
            crouched = true;
            if ( !state.Grounded )
                state.Position.Y += halfDelta;
        }
        else if ( !wantsCrouch && crouched )
        {
            const NetVec3 standing{ state.Position.X, state.Position.Y - ( state.Grounded ? 0.f : halfDelta ),
                                    state.Position.Z };
            if ( world.Fits( standing, CapsuleOf( settings, false ) ) )
            {
                crouched       = false;
                state.Position = standing;
            } // UE: stays crouched under a low ceiling, and retries every step
        }

        Planar wish{ input.Move.X, input.Move.Z };
        float  analog = Length( wish );
        if ( analog > 1.0f )
        {
            wish   = Scaled( wish, 1.0f / analog );
            analog = 1.0f;
        }
        // The game's reading of the step's action set: the pawn's declared Sprint / Jump actions (PawnActions).
        // A jump is the Jump action's press (UE bPressedJump, cleared on landing): held on, it does not repeat.
        const bool  jumpHeld    = actions.Held( input, actions.Jump );
        const bool  jumpPressed = jumpHeld && ( state.Mode & kModeJumpHeld ) == 0;
        const float maxSpeed    = crouched                                ? settings.MaxWalkSpeedCrouched
                                  : actions.Held( input, actions.Sprint ) ? settings.MaxSprintSpeed
                                                                          : settings.MaxWalkSpeed;
        Planar      velocity{ state.Velocity.X, state.Velocity.Z };
        NetVec3     moveVelocity;
        if ( swimming )
        {
            // UE PhysSwimming, as the swim intent drives it: the input's direction at Max Swim Speed, the swim
            // axis (+1 up, -1 down) for the vertical; no gravity.
            const float vertical = input.Axes.size() > kSwimVerticalAxis
                                        ? std::clamp( input.Axes[kSwimVerticalAxis], -1.0f, 1.0f )
                                        : 0.f;
            velocity             = Scaled( wish, settings.MaxSwimSpeed );
            state.Velocity.Y     = vertical * settings.MaxSwimSpeed;
            moveVelocity         = { velocity.X, state.Velocity.Y, velocity.Z };
        }
        else
        {
            if ( state.Grounded )
            {
                velocity = CalcVelocity( velocity, Scaled( wish, settings.MaxAcceleration ), dtSeconds, maxSpeed,
                                         analog, settings.GroundFriction, settings.BrakingFrictionFactor,
                                         settings.BrakingDecelerationWalking );
                state.Velocity.Y = 0.f;
                if ( jumpPressed && !crouched ) // UE CanJump: not while crouched
                {
                    state.Velocity.Y = settings.JumpZVelocity; // UE ACharacter::Jump -> DoJump
                    state.Grounded   = false;
                }
            }
            else
            {
                // UE falling: lateral friction and braking 0, acceleration times AirControl.
                velocity = CalcVelocity( velocity, Scaled( wish, settings.MaxAcceleration * settings.AirControl ),
                                         dtSeconds, maxSpeed, analog, 0.f, 0.f, 0.f );
            }
            // UE PhysWalking / PhysFalling with HasAnimRootMotion: the animation's root velocity IS the planar
            // velocity; the vertical stays gravity's and the jump's.
            if ( rootMotionVelocity != nullptr )
                velocity = { rootMotionVelocity->X, rootMotionVelocity->Z };
            if ( !state.Grounded )
                state.Velocity.Y -= ( world.Gravity() * settings.GravityScale ) * dtSeconds;
            // A grounded walk keeps pressing the floor (UE: the walking move follows the floor; Jolt sticks the
            // capsule to it), so a step off a ledge is a fall from the next move on, not a hover. A grounded pawn
            // with no planar velocity does not press: UE PhysWalking moves nothing along a walkable floor and only
            // re-finds it, while a press into a slope would slide down it (the collide-and-slide keeps the
            // press's tangential part) - an idle pawn on a walkable slope stays put on every side. A floor that
            // went away still ends the stand: the move's contacts find no floor and the pawn falls next step.
            const bool idleOnFloor =
                 state.Grounded && state.Velocity.Y == 0.f && velocity.X == 0.f && velocity.Z == 0.f;
            const float pressY = idleOnFloor ? 0.f : -( world.Gravity() * settings.GravityScale ) * dtSeconds;
            moveVelocity       = { velocity.X, state.Grounded ? pressY : state.Velocity.Y, velocity.Z };
        }
        state.Velocity.X = velocity.X;
        state.Velocity.Z = velocity.Z;

        const IPawnMoveWorld::Outcome moved  = world.Move( state, moveVelocity, CapsuleOf( settings, crouched ),
                                                           world.Gravity() * settings.GravityScale,
                                                          swimming ? 0.f : settings.MaxStepHeight, dtSeconds );
        const bool rising = !state.Grounded && state.Velocity.Y > 0.f;
        state.Position    = moved.Position;
        state.Velocity.X  = moved.Velocity.X;
        state.Velocity.Z  = moved.Velocity.Z;
        if ( swimming )
        {
            state.Velocity.Y = moved.Velocity.Y; // what the water's floor and ceiling let the swim do
            state.Grounded   = moved.Grounded;
        }
        else
        {
            if ( state.Grounded )
                state.Velocity.Y = 0.f;
            else if ( moved.Velocity.Y < state.Velocity.Y * 0.5f && rising )
                state.Velocity.Y = moved.Velocity.Y; // a ceiling stopped the jump (UE HandleImpact on the way up)
            // UE ProcessLanded: a walkable floor under a falling capsule ends the fall; a grounded capsule whose
            // floor went away starts falling (UE StartFalling from PhysWalking).
            if ( moved.Grounded && !( rising && state.Velocity.Y > 0.f ) )
            {
                state.Velocity.Y = 0.f;
                state.Grounded   = true;
            }
            else if ( !moved.Grounded )
                state.Grounded = false;
        }
        state.Mode =
             static_cast<std::uint8_t>( ( crouched ? kModeCrouched : 0u ) | ( swimming ? kModeSwimming : 0u ) |
                                        ( jumpHeld ? kModeJumpHeld : 0u ) );
        state.Yaw = input.Yaw;
        return state;
    }

} // namespace Desert::Gameplay
