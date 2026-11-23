#include <Net/PawnMovement.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Net
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

    PawnMoveState StepPawnMove( const PawnMoveState& from, const InputStep& input,
                                const PawnMoveSettings& settings, IPawnMoveWorld& world, float dtSeconds )
    {
        PawnMoveState state = from;
        Planar        wish{ input.Move.X, input.Move.Z };
        if ( const float length = Length( wish ); length > 1.0f )
            wish = Scaled( wish, 1.0f / length );
        const float maxSpeed =
             ( input.Buttons & kInputSprint ) != 0 ? settings.MaxSprintSpeed : settings.MaxWalkSpeed;
        Planar velocity{ state.Velocity.X, state.Velocity.Z };
        if ( state.Grounded )
        {
            velocity = CalcVelocity( velocity, Scaled( wish, settings.MaxAcceleration ), dtSeconds, maxSpeed,
                                     settings.GroundFriction, settings.BrakingFrictionFactor,
                                     settings.BrakingDecelerationWalking );
            state.Velocity.Y = 0.f;
            if ( ( input.Buttons & kInputJump ) != 0 )
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
        // capsule to it), so a step off a ledge is a fall from the next move on, not a hover.
        const NetVec3                 moveVelocity{ state.Velocity.X,
                                    state.Grounded ? -settings.Gravity * dtSeconds : state.Velocity.Y,
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

    bool MoveStatesDiffer( const PawnMoveState& a, const PawnMoveState& b )
    {
        const auto distanceSquared = []( const NetVec3& p, const NetVec3& q )
        {
            const float x = p.X - q.X, y = p.Y - q.Y, z = p.Z - q.Z;
            return x * x + y * y + z * z;
        };
        constexpr float kMaxPositionErrorSquared = 3.0f * 3.0f; // UE MAXPOSITIONERRORSQUARED: 3 cm
        constexpr float kMaxVelocityErrorSquared = 10.0f * 10.0f;
        return distanceSquared( a.Position, b.Position ) > kMaxPositionErrorSquared ||
               distanceSquared( a.Velocity, b.Velocity ) > kMaxVelocityErrorSquared || a.Grounded != b.Grounded;
    }

    void MovePredictor::Reset( const PawnMoveState& state )
    {
        m_Moves.clear();
        m_Current = m_Previous = state;
        m_Started              = true;
    }

    void MovePredictor::Predict( const InputStep& input, const PawnMoveSettings& settings, IPawnMoveWorld& world,
                                 float dtSeconds )
    {
        m_Previous = m_Current;
        m_Current  = StepPawnMove( m_Current, input, settings, world, dtSeconds );
        m_Moves.push_back( { input, m_Current } );
        while ( m_Moves.size() > InputRing::kCapacity )
            m_Moves.pop_front();
    }

    MovePredictor::EReconcile MovePredictor::Reconcile( std::uint32_t ackStep, const PawnMoveState& server,
                                                        const PawnMoveSettings& settings, IPawnMoveWorld& world,
                                                        float dtSeconds )
    {
        const auto acked = std::find_if( m_Moves.begin(), m_Moves.end(),
                                         [&]( const SavedMove& move ) { return move.Input.Step == ackStep; } );
        if ( acked == m_Moves.end() )
            return EReconcile::Unknown;
        const bool differ = MoveStatesDiffer( acked->After, server );
        m_Moves.erase( m_Moves.begin(), std::next( acked ) );
        if ( !differ )
            return EReconcile::Matched;
        // UE ClientAdjustPosition + ClientUpdatePositionAfterServerUpdate: take the server's state, replay.
        PawnMoveState state = server, previous = server;
        for ( SavedMove& move : m_Moves )
        {
            previous   = state;
            state      = StepPawnMove( state, move.Input, settings, world, dtSeconds );
            move.After = state;
        }
        m_Previous = m_Moves.empty() ? server : previous;
        m_Current  = state;
        ++m_Corrections;
        return EReconcile::Corrected;
    }
} // namespace Desert::Net
