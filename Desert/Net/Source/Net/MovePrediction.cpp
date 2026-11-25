#include <Net/MovePrediction.hpp>

#include <algorithm>

namespace Desert::Net
{
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
               distanceSquared( a.Velocity, b.Velocity ) > kMaxVelocityErrorSquared || a.Grounded != b.Grounded ||
               a.Mode != b.Mode;
    }

    void MovePredictor::Reset( const PawnMoveState& state )
    {
        m_Moves.clear();
        m_Current = m_Previous = state;
        m_Started              = true;
    }

    void MovePredictor::Predict( const InputStep& input, IPawnMover& mover, float dtSeconds )
    {
        m_Previous = m_Current;
        m_Current  = mover.Step( m_Current, input, dtSeconds );
        m_Moves.push_back( { input, m_Current } );
        while ( m_Moves.size() > InputRing::kCapacity )
            m_Moves.pop_front();
    }

    MovePredictor::EReconcile MovePredictor::Reconcile( std::uint32_t ackStep, const PawnMoveState& server,
                                                        IPawnMover& mover, float dtSeconds )
    {
        const auto acked = std::find_if( m_Moves.begin(), m_Moves.end(),
                                         [&]( const SavedMove& move ) { return move.Input.Step == ackStep; } );
        if ( acked == m_Moves.end() )
            return EReconcile::Unknown;
        const bool differ = MoveStatesDiffer( acked->After, server );
        if ( differ )
        {
            m_LastMispredicted      = acked->After;
            m_LastMispredictedInput = acked->Input;
        }
        m_Moves.erase( m_Moves.begin(), std::next( acked ) );
        if ( !differ )
            return EReconcile::Matched;
        // UE ClientAdjustPosition + ClientUpdatePositionAfterServerUpdate: take the server's state, replay.
        PawnMoveState state = server, previous = server;
        for ( SavedMove& move : m_Moves )
        {
            previous   = state;
            state      = mover.Step( state, move.Input, dtSeconds );
            move.After = state;
        }
        m_Previous = m_Moves.empty() ? server : previous;
        m_Current  = state;
        ++m_Corrections;
        return EReconcile::Corrected;
    }
} // namespace Desert::Net
