#pragma once

// THE OWN CHARACTER'S PREDICTION, AS NET CARRIES IT (NET-ARCH §5.1; UE FSavedMove_Character + ServerMove +
// ClientAdjustPosition). Net numbers the input steps, keeps the client's saved moves until the server acks them,
// compares the server's state for the acked step with the predicted one and replays the unacked moves on a
// mismatch. WHAT AN INPUT STEP DOES TO THE PAWN IS THE GAME'S, not Net's (NET-GAMEMODE, owner 11-23): the server's
// simulation and the client's prediction both call the game's IPawnMover - Engine/Gameplay/CharacterMoveStep.hpp,
// the one character movement step, linked by the editor's client and by DesertServer - so the same inputs give
// the same state on both and Net reads none of them (Replication.hpp InputStep: the pawn's opaque action set).

#include <Net/Replication.hpp>

#include <cstdint>
#include <deque>
#include <string>

namespace Desert::Net
{
    // THE GAME'S MOVEMENT AS NET SEES IT (UE: the CharacterMovementComponent the net driver calls; Mover 2.0's
    // movement simulation): one fixed step of a pawn's state under one input step, a function of (state, input)
    // alone so the client's replay reproduces the server's step.
    class IPawnMover
    {
    public:
        virtual ~IPawnMover() = default;
        [[nodiscard]] virtual PawnMoveState Step( const PawnMoveState& from, const InputStep& input,
                                                  float dtSeconds ) = 0;
        // The step's action set in the game's words (its declared action names), for the logs Net writes.
        [[nodiscard]] virtual std::string DescribeActions( const InputStep& input ) const = 0;
    };

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
        void Predict( const InputStep& input, IPawnMover& mover, float dtSeconds );
        // The server's state after input step @p ackStep (UE ClientAdjustPosition / ClientAckGoodMove).
        EReconcile Reconcile( std::uint32_t ackStep, const PawnMoveState& server, IPawnMover& mover,
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
        // The last corrected step as it was predicted, and its input: what the server's state is read against.
        [[nodiscard]] const PawnMoveState& LastMispredicted() const
        {
            return m_LastMispredicted;
        }
        [[nodiscard]] const InputStep& LastMispredictedInput() const
        {
            return m_LastMispredictedInput;
        }

    private:
        struct SavedMove
        {
            InputStep     Input;
            PawnMoveState After;
        };
        std::deque<SavedMove> m_Moves;
        PawnMoveState         m_Current, m_Previous, m_LastMispredicted;
        InputStep             m_LastMispredictedInput;
        std::uint32_t         m_Corrections = 0;
        bool                  m_Started     = false;
    };
} // namespace Desert::Net
