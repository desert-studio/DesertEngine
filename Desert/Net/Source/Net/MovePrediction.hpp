#pragma once

// THE OWN CHARACTER'S PREDICTION, AS NET CARRIES IT (NET-ARCH §5.1; UE FSavedMove_Character + ServerMove +
// ClientAdjustPosition). Net numbers the input steps, keeps the client's saved moves until the server acks them,
// compares the server's state for the acked step with the predicted one and replays the unacked moves on a
// mismatch. WHAT AN INPUT STEP DOES TO THE PAWN IS THE GAME'S, not Net's (NET-GAMEMODE, owner 11-23): the server's
// simulation and the client's prediction both call the game's IPawnMover - Engine/Gameplay/CharacterMoveStep.hpp,
// the one character movement step, linked by the editor's client and by DesertServer - so the same inputs give
// the same state on both and Net reads none of them (Replication.hpp InputStep: the pawn's opaque action set).
//
// NET-TELEPORT-VELOCITY: the server acks the SAME step again whenever it moved the pawn without a move of ours (a
// teleport, a held placement, a handover) while the client sends at its stationary rate and no newer step has been
// processed. The predictor keeps the last acked move (UE FNetworkPredictionData_Client_Character::LastAckedMove) and
// reads a re-ack with a changed state as a correction: the server's state is taken and every unacked move replayed on
// it, as for any other correction. A re-acked step whose state is the one already taken changes nothing.

#include <Net/Replication.hpp>

#include <cstdint>
#include <deque>
#include <optional>
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
    // produced, kept until the server acknowledges it; the last acknowledged one is kept too (LastAckedMove), so a
    // second ack of that step with another state is read as the server's correction it is.
    class MovePredictor
    {
    public:
        enum class EReconcile : std::uint8_t
        {
            Matched,   // the server's state for the acked step is the one predicted (or the one already taken for a
                       // re-acked step): nothing changes
            Corrected, // it was not: the server's state was taken and the unacked moves replayed on it - for a saved
                       // step or for the last acked one (LastCorrectionReacked)
            Unknown,   // the acked step is not saved and is not the last acked one (older than both, or before the
                       // first prediction)
        };

        void               Reset( const PawnMoveState& state );
        [[nodiscard]] bool Started() const
        {
            return m_Started;
        }
        // Predicts one input step; the input's Step is its number on the wire.
        void Predict( const InputStep& input, IPawnMover& mover, float dtSeconds );
        // The server's state after input step @p ackStep (UE ClientAdjustPosition / ClientAckGoodMove). The acked
        // move, saved or the last acked, is compared with @p server by MoveStatesDiffer; on a difference the server's
        // state replaces it and every move after @p ackStep is replayed on it. The last acked move becomes
        // (@p ackStep, @p server) whenever the step is saved or re-acked.
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
        // True when the last Corrected came from a re-acked step (the server moved the pawn without a move of
        // ours: a teleport, a held placement, a handover); false after a correction of a saved step.
        [[nodiscard]] bool LastCorrectionReacked() const
        {
            return m_LastCorrectionReacked;
        }
        // The step the server last acknowledged, with the state it acknowledged (the server's when it differed, the
        // predicted one when it matched); nullopt before the first ack of this prediction.
        [[nodiscard]] std::optional<std::uint32_t> LastAckedStep() const
        {
            return m_LastAcked ? std::optional<std::uint32_t>( m_LastAcked->Input.Step ) : std::nullopt;
        }

    private:
        struct SavedMove
        {
            InputStep     Input;
            PawnMoveState After;
        };
        std::deque<SavedMove>    m_Moves;
        std::optional<SavedMove> m_LastAcked; // UE LastAckedMove: the newest step the server has answered
        PawnMoveState            m_Current, m_Previous, m_LastMispredicted;
        InputStep                m_LastMispredictedInput;
        std::uint32_t            m_Corrections            = 0;
        bool                     m_Started                = false;
        bool                     m_LastCorrectionReacked = false;
    };
} // namespace Desert::Net
