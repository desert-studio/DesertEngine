#pragma once

// THE OWN CHARACTER'S MOVE, SHARED BY THE SERVER AND THE PREDICTING CLIENT (NET-ARCH §5.1; UE
// UCharacterMovementComponent saved moves + ServerMove + ClientAdjustPosition).
//
// StepPawnMove is the one movement step both sides run: the server for every input step it receives (and only
// then - a pawn whose input has not arrived holds, so the state after step N is a function of the inputs up to
// N alone), the client for every input step it sends. Same inputs, same float path, same state: the client's
// prediction matches the server bit for bit unless something only the server knows happened (a teleport, a lost
// input, another authority's push) - then the server's state for the acked step replaces the client's, and the
// client replays its unacked moves on top of it (MovePredictor::Reconcile).
//
// The math is UE's CharacterMovementComponent walking / falling (CalcVelocity with ground friction and braking,
// JumpZVelocity, AirControl, gravity) on world centimetres, Y up. The world under the pawn is a floor plane at the
// state's FloorY: the dedicated server has no collision world yet (it holds cell records as data, ServerWorld),
// so a fence is not an obstacle on either side; the server's collision world is the next step of this file.

#include <Net/Replication.hpp>

#include <cstdint>
#include <deque>

namespace Desert::Net
{
    // UE CharacterMovementComponent defaults (cm, cm/s, cm/s^2), the same as CharacterControllerData's.
    struct PawnMoveSettings
    {
        float MaxWalkSpeed               = 600.0f;
        float MaxSprintSpeed             = 1000.0f; // the speed while the Sprint button is held
        float MaxAcceleration            = 2048.0f;
        float BrakingDecelerationWalking = 2048.0f;
        float GroundFriction             = 8.0f;
        float BrakingFrictionFactor      = 2.0f;
        float JumpZVelocity              = 700.0f;
        float AirControl                 = 0.35f;
        float Gravity                    = 980.0f;
    };

    // One fixed step of the own character (StepPawnMove's input is the player's InputStep).
    [[nodiscard]] PawnMoveState StepPawnMove( const PawnMoveState& from, const InputStep& input,
                                              const PawnMoveSettings& settings, float dtSeconds );

    // True when two states differ by more than the reconciliation tolerance (UE MAXPOSITIONERRORSQUARED: the
    // server corrects only an error the client could see).
    [[nodiscard]] bool MoveStatesDiffer( const PawnMoveState& a, const PawnMoveState& b );

    // THE CLIENT'S SAVED MOVES (UE FSavedMove_Character ring): every predicted input step with the state it
    // produced, kept until the server acknowledges it.
    class MovePredictor
    {
    public:
        enum class EReconcile : std::uint8_t
        {
            Matched,   // the server's state for the acked step is the one predicted: nothing changes
            Corrected, // it was not: the server's state was taken and the unacked moves replayed on it
            Unknown,   // the acked step is not saved (older than the ring, or before the first prediction)
        };

        void               Reset( const PawnMoveState& state );
        [[nodiscard]] bool Started() const
        {
            return m_Started;
        }
        // Predicts one input step; the input's Step is its number on the wire.
        void Predict( const InputStep& input, const PawnMoveSettings& settings, float dtSeconds );
        // The server's state after input step @p ackStep (UE ClientAdjustPosition / ClientAckGoodMove).
        EReconcile Reconcile( std::uint32_t ackStep, const PawnMoveState& server, const PawnMoveSettings& settings,
                              float dtSeconds );

        [[nodiscard]] const PawnMoveState& Current() const
        {
            return m_Current;
        }
        // The state one step before Current: the renderer draws between the two (fixed-step interpolation).
        [[nodiscard]] const PawnMoveState& Previous() const
        {
            return m_Previous;
        }
        [[nodiscard]] std::uint32_t Corrections() const
        {
            return m_Corrections;
        }
        [[nodiscard]] std::size_t Unacked() const
        {
            return m_Moves.size();
        }

    private:
        struct SavedMove
        {
            InputStep     Input;
            PawnMoveState After;
        };
        std::deque<SavedMove> m_Moves;
        PawnMoveState         m_Current, m_Previous;
        std::uint32_t         m_Corrections = 0;
        bool                  m_Started     = false;
    };
} // namespace Desert::Net
