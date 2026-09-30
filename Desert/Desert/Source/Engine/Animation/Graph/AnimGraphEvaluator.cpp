#include "AnimGraph.hpp"

#include "AnimGraphValidation.hpp"

#include <algorithm>
#include <format>

namespace Desert::Animation::Graph
{
    Evaluator::Evaluator( AnimGraph graph, GraphScope scope ) : m_Graph( std::move( graph ) ), m_Scope( scope )
    {
        Reset();
    }

    void Evaluator::Reset()
    {
        m_Params.clear();
        for ( const auto& p : m_Graph.Parameters )
            m_Params[p.Name] = p.Default;

        CheckStructure();
        EnterMachines();
    }

    void Evaluator::EnterMachines()
    {
        m_Runs.assign( m_Graph.Nodes.size(), MachineRun{} );
        for ( size_t n = 0; n < m_Graph.Nodes.size(); ++n )
        {
            const auto& slot = m_Graph.Nodes[n].Machine;
            if ( !slot || slot->States.empty() )
                continue;
            const StateMachine& machine = *slot;
            int                 entry   = -1;
            for ( size_t i = 0; i < machine.States.size(); ++i )
                if ( machine.States[i].Name == machine.Entry )
                    entry = static_cast<int>( i );
            m_Runs[n].Current = entry < 0 ? 0 : entry; // no entry / a missing one -> the first state
        }
    }

    void Evaluator::SyncGraph( AnimGraph graph )
    {
        // Each machine keeps running the state it was in, matched by node name then state name.
        std::unordered_map<std::string, std::string> running;
        for ( size_t n = 0; n < m_Graph.Nodes.size() && n < m_Runs.size(); ++n )
            if ( const State* state = StateOf( static_cast<int>( n ), m_Runs[n].Current ) )
                running[m_Graph.Nodes[n].Name] = state->Name;
        auto savedParams = m_Params;

        m_Graph = std::move( graph );
        EnterMachines();
        for ( size_t n = 0; n < m_Graph.Nodes.size(); ++n )
        {
            const auto  it      = running.find( m_Graph.Nodes[n].Name );
            const auto& machine = m_Graph.Nodes[n].Machine;
            if ( it == running.end() || !machine )
                continue;
            for ( size_t i = 0; i < machine->States.size(); ++i )
                if ( machine->States[i].Name == it->second )
                    m_Runs[n].Current = static_cast<int>( i );
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
        // The plan first: a graph that cannot be ordered has no evaluation to speak of, and saying so is
        // the whole verdict. Then the conditions (the rule's one spelling lives in AnimGraphValidation).
        // ANY COMPOSITION IS PLAYED: the pose graph is evaluated node by node (PoseGraphInstance), so a
        // blend of blends, an additive over a layered blend or a sequence player in a layer are all legal.
        auto plan = PlanPoseGraph( m_Graph, m_Scope );
        m_Plan.clear();
        m_Output = -1;
        if ( !plan )
        {
            m_StructureError = plan.GetError();
            return;
        }
        m_Plan = plan.ExtractValue();
        if ( const PoseNode* base = BaseSourceNode( m_Graph );
             base != nullptr && static_cast<PoseNodeKind>( base->Kind ) == PoseNodeKind::StateMachine )
            m_Output = static_cast<int>( base - m_Graph.Nodes.data() );
        m_StructureError = UndeclaredConditionParameters( m_Graph );
    }

    float Evaluator::GetFloat( const std::string& name ) const
    {
        const auto it = m_Params.find( name );
        return it == m_Params.end() ? 0.0f : it->second;
    }

    const State* Evaluator::StateOf( int node, int state ) const
    {
        if ( node < 0 || node >= static_cast<int>( m_Graph.Nodes.size() ) )
            return nullptr;
        const auto& machine = m_Graph.Nodes[static_cast<size_t>( node )].Machine;
        if ( !machine || state < 0 || state >= static_cast<int>( machine->States.size() ) )
            return nullptr;
        return &machine->States[static_cast<size_t>( state )];
    }

    const State* Evaluator::CurrentState() const
    {
        return m_Output < 0 ? nullptr : StateOf( m_Output, m_Runs[static_cast<size_t>( m_Output )].Current );
    }

    const State* Evaluator::CurrentState( std::string_view node ) const
    {
        for ( size_t n = 0; n < m_Graph.Nodes.size(); ++n )
            if ( m_Graph.Nodes[n].Name == node )
                return StateOf( static_cast<int>( n ), m_Runs[n].Current );
        return nullptr;
    }

    const State* Evaluator::PreviousState() const
    {
        return m_Output < 0 ? nullptr : StateOf( m_Output, m_Runs[static_cast<size_t>( m_Output )].Previous );
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
        // The plan, in order: every node after the nodes wired into it. Only a state machine node has an
        // Update today; the kinds that take Pose pins (layered blend, linked layer) join this walk.
        Result output;
        for ( const int node : m_Plan )
        {
            const PoseNode& poseNode = m_Graph.Nodes[static_cast<size_t>( node )];
            if ( static_cast<PoseNodeKind>( poseNode.Kind ) != PoseNodeKind::StateMachine || !poseNode.Machine )
                continue;
            // Exit time is measured on the clip Output Pose shows, so only the machine there is given it;
            // a machine further up the graph has no clip time the caller knows.
            const Result result =
                 UpdateMachine( node, *poseNode.Machine, node == m_Output ? normalizedTime : 0.0f );
            if ( node == m_Output )
                output = result;
        }
        return output;
    }

    Evaluator::Result Evaluator::UpdateMachine( int node, const StateMachine& machine, float normalizedTime )
    {
        Result      result;
        MachineRun& run = m_Runs[static_cast<size_t>( node )];

        result.Current = StateOf( node, run.Current );
        if ( !result.Current )
            return result;

        for ( const auto& t : result.Current->Transitions )
        {
            if ( t.HasExitTime && normalizedTime < t.ExitTime )
                continue;

            // Empty condition set is valid: a pure exit-time (or unconditional) transition auto-advances.
            const bool pass = std::all_of( t.Conditions.begin(), t.Conditions.end(),
                                           [this]( const Condition& c ) { return EvaluateCondition( c ); } );
            if ( !pass )
                continue;

            int target = -1;
            for ( size_t i = 0; i < machine.States.size(); ++i )
                if ( machine.States[i].Name == t.To )
                    target = static_cast<int>( i );
            if ( target < 0 || target == run.Current )
                continue; // dangling target / self-loop -> ignore (never "changes")

            // ОТКУДА ПРИШЛИ — запоминается здесь, в единственном месте, где переход срабатывает.
            // Панель могла бы вывести это наблюдением («имя сменилось — значит был переход»), но она
            // тикает, только когда открыта: закрыл окно на время перехода — и наблюдатель пропустил
            // ровно то событие, ради которого он есть. Здесь же это факт, а не догадка.
            run.Previous   = run.Current;
            run.Current    = target;
            result.Current = StateOf( node, run.Current );
            result.Changed = true;
            result.Blend   = t.Blend;
            break; // one transition per tick
        }

        return result;
    }
} // namespace Desert::Animation::Graph
