#pragma once

// A SCRIPTED INPUT SOURCE FOR A PAWN (NET-GAMEMODE; COUPLING-AUDIT finding 15: the bot walker lived inside the
// net driver and set engine-fixed sprint / jump bits). It stands in for a player's hands on a test client: it
// walks a direction until the pawn has travelled a distance, holds the pawn's declared Sprint action and presses
// its Jump action every N seconds - by the names the pawn's prefab declares (CharacterMoveStep.hpp PawnActions).
// It yields what a player's input yields (the move intent and the action bits), so the net driver sends it and
// the prediction runs it without knowing it is scripted (UE: a bot's AIController drives the same pawn input).

#include <Engine/Gameplay/CharacterMoveStep.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Gameplay
{
    struct ScriptedWalk
    {
        float     DistanceCm = 0.0f; // > 0: walk until the pawn's server position is this far from its first
        glm::vec3 Direction{ 1.0f, 0.0f, 0.0f };
        bool      HoldSprint       = false; // hold the pawn's declared Sprint action while walking
        float     JumpEverySeconds = 0.0f;  // > 0: press the pawn's declared Jump action this often
    };

    class ScriptedPawnInput
    {
    public:
        struct Frame
        {
            glm::vec3     Move{ 0.0f };
            std::uint32_t Actions = 0;
        };

        explicit ScriptedPawnInput( ScriptedWalk walk ) : m_Walk( walk )
        {
        }
        // The pawn starts (or restarts after a correction's reset): the jump clock starts with it.
        void Start( double clock )
        {
            m_NextJump = clock + m_Walk.JumpEverySeconds;
        }
        [[nodiscard]] bool Walking() const
        {
            return m_Walking;
        }
        // One step's input at @p clock (seconds). @p travelledCm: how far the pawn's newest SERVER position is
        // from its first one (nullopt until known) - the walk ends on the server's word, not on the prediction's.
        [[nodiscard]] Frame Next( double clock, std::optional<float> travelledCm, const PawnActions& actions )
        {
            if ( m_Walking && travelledCm && *travelledCm >= m_Walk.DistanceCm )
                m_Walking = false;
            Frame frame;
            if ( !m_Walking )
                return frame; // the walk is over: the pawn stands
            frame.Move =
                 glm::length( m_Walk.Direction ) > 1e-4f ? glm::normalize( m_Walk.Direction ) : glm::vec3( 0.0f );
            if ( m_Walk.HoldSprint )
                frame.Actions |= actions.Bit( actions.Sprint );
            if ( m_Walk.JumpEverySeconds > 0.0f && clock >= m_NextJump )
            {
                frame.Actions |= actions.Bit( actions.Jump );
                m_NextJump = clock + m_Walk.JumpEverySeconds;
            }
            return frame;
        }

    private:
        ScriptedWalk m_Walk;
        bool         m_Walking  = true;
        double       m_NextJump = 0.0;
    };
} // namespace Desert::Gameplay
