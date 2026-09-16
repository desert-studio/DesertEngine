#include "AnimGraph.hpp"

#include "AnimGraphValidation.hpp"

#include <algorithm>

namespace Desert::Animation::Graph
{
    const char* TypeName( ParamType type )
    {
        switch ( type )
        {
            case ParamType::Bool:
                return "Bool";
            case ParamType::Int:
                return "Int";
            case ParamType::Float:
                return "Float";
        }
        return "?";
    }

    std::string DeclaredParameterList( const AnimGraph& graph )
    {
        std::string declared;
        for ( const auto& p : graph.Parameters )
        {
            declared += declared.empty() ? "" : ", ";
            declared += fmt::format( "'{}' ({})", p.Name, TypeName( static_cast<ParamType>( p.Type ) ) );
        }
        return declared.empty() ? std::string( "none at all" ) : declared;
    }

    Evaluator::Evaluator( AnimGraph graph ) : m_Graph( std::move( graph ) )
    {
        Reset();
    }

    void Evaluator::Reset()
    {
        m_Params.clear();
        for ( const auto& p : m_Graph.Parameters )
            m_Params[p.Name] = p.Default;

        m_Current = m_Graph.Entry.empty() ? ( m_Graph.States.empty() ? -1 : 0 ) : FindState( m_Graph.Entry );
        if ( m_Current < 0 && !m_Graph.States.empty() )
        {
            m_Current = 0; // entry named a missing state -> fall back to the first
        }

        CheckStructure();
    }

    void Evaluator::SyncGraph( AnimGraph graph )
    {
        const std::string currentName = CurrentState() ? CurrentState()->Name : std::string();
        auto              savedParams = m_Params;

        m_Graph = std::move( graph );

        // Keep running the same state if it still exists; otherwise re-enter (entry / first).
        m_Current = FindState( currentName );
        if ( m_Current < 0 )
        {
            m_Current = m_Graph.Entry.empty() ? ( m_Graph.States.empty() ? -1 : 0 ) : FindState( m_Graph.Entry );
            if ( m_Current < 0 && !m_Graph.States.empty() )
                m_Current = 0;
        }

        // Preserve live parameter values; seed only parameters that are new.
        for ( const auto& p : m_Graph.Parameters )
            if ( savedParams.find( p.Name ) == savedParams.end() )
                savedParams[p.Name] = p.Default;
        m_Params = std::move( savedParams );

        // The graph was replaced, so the previous verdict describes a graph that is gone.
        CheckStructure();
    }

    const Parameter* Evaluator::FindParameter( const std::string& name ) const
    {
        const auto it = std::find_if( m_Graph.Parameters.begin(), m_Graph.Parameters.end(),
                                      [&name]( const Parameter& p ) { return p.Name == name; } );
        return it == m_Graph.Parameters.end() ? nullptr : &*it;
    }

    Common::BoolResultStr Evaluator::RefuseUnknown( const std::string& name ) const
    {
        return Common::MakeFormattedError<bool>(
             "AnimGraph '{}' has no parameter called '{}'. It declares: {}. Setting it would have created a "
             "parameter that holds the value, is read by no condition, and says nothing -- which is "
             "indistinguishable from the state machine working.",
             m_Graph.Name, name, DeclaredParameterList( m_Graph ) );
    }

    Common::BoolResultStr Evaluator::SetBool( const std::string& name, bool value )
    {
        const Parameter* declared = FindParameter( name );
        if ( declared == nullptr )
        {
            return RefuseUnknown( name );
        }
        if ( static_cast<ParamType>( declared->Type ) != ParamType::Bool )
        {
            return Common::MakeFormattedError<bool>(
                 "AnimGraph '{}' declares parameter '{}' as {}, and a bool was set on it. The graph is the "
                 "one place that says what a parameter IS; a caller that could override that would be a "
                 "second answer to the same question.",
                 m_Graph.Name, name, TypeName( static_cast<ParamType>( declared->Type ) ) );
        }
        m_Params[name] = value ? 1.0f : 0.0f;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Evaluator::SetInt( const std::string& name, int value )
    {
        const Parameter* declared = FindParameter( name );
        if ( declared == nullptr )
        {
            return RefuseUnknown( name );
        }
        if ( static_cast<ParamType>( declared->Type ) != ParamType::Int )
        {
            return Common::MakeFormattedError<bool>(
                 "AnimGraph '{}' declares parameter '{}' as {}, and an int was set on it.", m_Graph.Name, name,
                 TypeName( static_cast<ParamType>( declared->Type ) ) );
        }
        m_Params[name] = static_cast<float>( value );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Evaluator::SetFloat( const std::string& name, float value )
    {
        const Parameter* declared = FindParameter( name );
        if ( declared == nullptr )
        {
            return RefuseUnknown( name );
        }
        if ( static_cast<ParamType>( declared->Type ) != ParamType::Float )
        {
            return Common::MakeFormattedError<bool>(
                 "AnimGraph '{}' declares parameter '{}' as {}, and a float was set on it.", m_Graph.Name, name,
                 TypeName( static_cast<ParamType>( declared->Type ) ) );
        }
        m_Params[name] = value;
        return Common::MakeSuccess( true );
    }

    void Evaluator::CheckStructure()
    {
        // ONE SPELLING OF THE RULE, AND IT LIVES IN AnimGraphValidation. This used to be the only place
        // that knew which conditions name an undeclared parameter, so the panel had no way to draw the
        // same fact without writing it a second time -- and two spellings of one rule is a strip and a
        // log that one day disagree about the same graph. The check still happens HERE, once, because
        // the place the conditions are read cannot refuse; what moved is where the sentence is written.
        m_StructureError = UndeclaredConditionParameters( m_Graph );
    }

    float Evaluator::GetFloat( const std::string& name ) const
    {
        const auto it = m_Params.find( name );
        return it == m_Params.end() ? 0.0f : it->second;
    }

    int Evaluator::FindState( const std::string& name ) const
    {
        for ( size_t i = 0; i < m_Graph.States.size(); ++i )
            if ( m_Graph.States[i].Name == name )
                return static_cast<int>( i );
        return -1;
    }

    const State* Evaluator::CurrentState() const
    {
        return ( m_Current >= 0 && m_Current < static_cast<int>( m_Graph.States.size() ) )
                    ? &m_Graph.States[m_Current]
                    : nullptr;
    }

    bool Evaluator::EvaluateCondition( const Condition& c ) const
    {
        const float v = GetFloat( c.Parameter );
        switch ( static_cast<CompareOp>( c.Op ) )
        {
            case CompareOp::Greater:
                return v > c.Value;
            case CompareOp::Less:
                return v < c.Value;
            case CompareOp::GreaterEqual:
                return v >= c.Value;
            case CompareOp::LessEqual:
                return v <= c.Value;
            case CompareOp::Equals:
                return v == c.Value;
            case CompareOp::NotEquals:
                return v != c.Value;
            case CompareOp::IsTrue:
                return v != 0.0f;
            case CompareOp::IsFalse:
                return v == 0.0f;
        }
        return false;
    }

    Evaluator::Result Evaluator::Update( float normalizedTime )
    {
        Result result;

        if ( m_Current < 0 )
            m_Current = m_Graph.States.empty() ? -1 : 0;
        result.Current = CurrentState();
        if ( !result.Current )
            return result;

        for ( const auto& t : result.Current->Transitions )
        {
            if ( t.HasExitTime && normalizedTime < t.ExitTime )
                continue;

            // Empty condition set is valid: a pure exit-time (or unconditional) transition auto-advances.
            bool pass = true;
            for ( const auto& c : t.Conditions )
            {
                if ( !EvaluateCondition( c ) )
                {
                    pass = false;
                    break;
                }
            }
            if ( !pass )
                continue;

            const int target = FindState( t.To );
            if ( target < 0 || target == m_Current )
                continue; // dangling target / self-loop -> ignore (never "changes")

            m_Current      = target;
            result.Current = CurrentState();
            result.Changed = true;
            result.Blend   = t.Blend;
            break; // one transition per tick
        }

        return result;
    }
} // namespace Desert::Animation::Graph
