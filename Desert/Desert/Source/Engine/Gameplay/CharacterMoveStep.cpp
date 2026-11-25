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

        // UE UCharacterMovementComponent::CalcVelocity on the planar velocity.
        Planar CalcVelocity( Planar velocity, Planar acceleration, float dt, float maxSpeed, float friction,
                             float brakingFrictionFactor, float brakingDeceleration )
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
                const float newMax = Length( velocity ) > maxSpeed * 1.01f ? Length( velocity ) : maxSpeed;
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
        for ( const std::string* name : { &controller.JumpAction, &controller.SprintAction } )
            if ( !name->empty() && !actions.IndexOf( *name ) )
                actions.Names.push_back( *name );
        actions.Jump   = controller.JumpAction;
        actions.Sprint = controller.SprintAction;
        return actions;
    }

    PawnMoveState StepPawnMove( const PawnMoveState& from, const InputStep& input, const PawnMovement& movement,
                                IPawnMoveWorld& world, float dtSeconds )
    {
        const PawnMoveSettings& settings = movement.Settings;
        PawnMoveState state = from;
        Planar        wish{ input.Move.X, input.Move.Z };
        if ( const float length = Length( wish ); length > 1.0f )
            wish = Scaled( wish, 1.0f / length );
        // The game's reading of the step's action set: the pawn's declared Sprint / Jump actions (PawnActions).
        const float maxSpeed =
             movement.Actions.Held( input, movement.Actions.Sprint ) ? settings.MaxSprintSpeed : settings.MaxWalkSpeed;
        Planar velocity{ state.Velocity.X, state.Velocity.Z };
        if ( state.Grounded )
        {
            velocity = CalcVelocity( velocity, Scaled( wish, settings.MaxAcceleration ), dtSeconds, maxSpeed,
                                     settings.GroundFriction, settings.BrakingFrictionFactor,
                                     settings.BrakingDecelerationWalking );
            state.Velocity.Y = 0.f;
            if ( movement.Actions.Held( input, movement.Actions.Jump ) )
            {
                state.Velocity.Y = settings.JumpZVelocity; // UE ACharacter::Jump -> DoJump
                state.Grounded   = false;
            }
        }
        else
        {
            // UE falling: lateral friction and braking 0, acceleration times AirControl.
            velocity = CalcVelocity( velocity, Scaled( wish, settings.MaxAcceleration * settings.AirControl ),
                                     dtSeconds, maxSpeed, 0.f, 0.f, 0.f );
        }
        if ( !state.Grounded )
            state.Velocity.Y -= settings.Gravity * dtSeconds;
        state.Velocity.X = velocity.X;
        state.Velocity.Z = velocity.Z;
        // A grounded walk keeps pressing the floor (UE: the walking move follows the floor; Jolt sticks the
        // capsule to it), so a step off a ledge is a fall from the next move on, not a hover. A grounded pawn
        // with no planar velocity does not press: UE PhysWalking moves nothing along a walkable floor and only
        // re-finds it, while a press into a slope would slide down it (the collide-and-slide keeps the press's
        // tangential part) - an idle pawn on a walkable slope stays put on both sides. A floor that went away
        // still ends the stand: the move's contacts find no floor and the pawn falls from the next step.
        const bool                    idleOnFloor = state.Grounded && state.Velocity.Y == 0.f &&
                                                    velocity.X == 0.f && velocity.Z == 0.f;
        const float                   pressY      = idleOnFloor ? 0.f : -settings.Gravity * dtSeconds;
        const NetVec3                 moveVelocity{ state.Velocity.X, state.Grounded ? pressY : state.Velocity.Y,
                                    state.Velocity.Z };
        const IPawnMoveWorld::Outcome moved  = world.Move( state, moveVelocity, settings, dtSeconds );
        const bool                    rising = !state.Grounded && state.Velocity.Y > 0.f;
        state.Position                       = moved.Position;
        state.Velocity.X                     = moved.Velocity.X;
        state.Velocity.Z                     = moved.Velocity.Z;
        if ( state.Grounded )
            state.Velocity.Y = 0.f;
        else if ( moved.Velocity.Y < state.Velocity.Y * 0.5f && rising )
            state.Velocity.Y = moved.Velocity.Y; // a ceiling stopped the jump (UE HandleImpact on the way up)
        // UE ProcessLanded: a walkable floor under a falling capsule ends the fall; a grounded capsule whose floor
        // went away starts falling (UE StartFalling from PhysWalking).
        if ( moved.Grounded && !( rising && state.Velocity.Y > 0.f ) )
        {
            state.Velocity.Y = 0.f;
            state.Grounded   = true;
        }
        else if ( !moved.Grounded )
            state.Grounded = false;
        state.Yaw = input.Yaw;
        return state;
    }

} // namespace Desert::Gameplay

