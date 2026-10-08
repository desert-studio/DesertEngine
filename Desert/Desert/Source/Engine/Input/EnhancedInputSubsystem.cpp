#include <Engine/Input/EnhancedInputSubsystem.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Input
{
    using namespace Assets::Serialization;

    namespace
    {

        float RemapDeadZone( const float magnitude, const InputDeadZoneParams& dz )
        {
            return std::clamp( ( magnitude - dz.LowerThreshold ) / ( dz.UpperThreshold - dz.LowerThreshold ), 0.0f,
                               1.0f );
        }

        glm::vec3 ApplyOne( const glm::vec3 v, const InputValueType type, const InputModifierData& m )
        {
            switch ( m.Type )
            {
                case InputModifierType::Negate:
                    return { m.Negate->X ? -v.x : v.x, m.Negate->Y ? -v.y : v.y, m.Negate->Z ? -v.z : v.z };
                case InputModifierType::Swizzle:
                    switch ( m.Swizzle->Order )
                    {
                        case InputSwizzleOrder::YXZ:
                            return { v.y, v.x, v.z };
                        case InputSwizzleOrder::ZYX:
                            return { v.z, v.y, v.x };
                        case InputSwizzleOrder::XZY:
                            return { v.x, v.z, v.y };
                        case InputSwizzleOrder::YZX:
                            return { v.y, v.z, v.x };
                        case InputSwizzleOrder::ZXY:
                            return { v.z, v.x, v.y };
                    }
                    return v;
                case InputModifierType::DeadZone:
                {
                    const InputDeadZoneParams& dz = *m.DeadZone;
                    if ( dz.Type == InputDeadZoneType::Axial )
                    {
                        glm::vec3 out;
                        for ( int c = 0; c < 3; ++c )
                            out[c] = std::copysign( RemapDeadZone( std::abs( v[c] ), dz ), v[c] );
                        return out;
                    }
                    const float length = glm::length( v );
                    if ( length <= 0.0f )
                        return v;
                    return v / length * RemapDeadZone( length, dz );
                }
                case InputModifierType::Scalar:
                    // UE: a scalar does nothing to a Bool (it would stop being 0/1).
                    return type == InputValueType::Bool ? v : v * m.Scalar->Scalar;
            }
            return v;
        }

        bool IsActuated( const glm::vec3& v, const float threshold )
        {
            return glm::dot( v, v ) >= threshold * threshold;
        }

        TriggerState Stronger( const TriggerState a, const TriggerState b )
        {
            return static_cast<uint8_t>( a ) >= static_cast<uint8_t>( b ) ? a : b;
        }
    } // namespace

    glm::vec3 ShapeToValueType( const glm::vec3 value, const InputValueType type )
    {
        switch ( type )
        {
            case InputValueType::Bool:
                return { ( value.x != 0.0f || value.y != 0.0f || value.z != 0.0f ) ? 1.0f : 0.0f, 0.0f, 0.0f };
            case InputValueType::Axis1D:
                return { value.x, 0.0f, 0.0f };
            case InputValueType::Axis2D:
                return { value.x, value.y, 0.0f };
            case InputValueType::Axis3D:
                return value;
        }
        return value;
    }

    glm::vec3 ApplyModifiers( glm::vec3 value, const InputValueType type,
                              const std::vector<InputModifierData>& modifiers )
    {
        value = ShapeToValueType( value, type );
        for ( const InputModifierData& m : modifiers )
            value = ShapeToValueType( ApplyOne( value, type, m ), type );
        return value;
    }

    TriggerEvents EventsForTransition( const TriggerState last, const TriggerState now )
    {
        TriggerEvents e;
        switch ( last )
        {
            case TriggerState::None:
                e.Started   = now != TriggerState::None;
                e.Triggered = now == TriggerState::Triggered;
                break;
            case TriggerState::Ongoing:
                e.Ongoing   = now == TriggerState::Ongoing;
                e.Triggered = now == TriggerState::Triggered;
                e.Canceled  = now == TriggerState::None;
                break;
            case TriggerState::Triggered:
                e.Triggered = now == TriggerState::Triggered;
                e.Ongoing   = now == TriggerState::Ongoing;
                e.Completed = now == TriggerState::None;
                break;
        }
        return e;
    }

    void EnhancedInputSubsystem::RegisterAction( const Common::Content::AssetGuid& guid,
                                                 const InputActionData&            action )
    {
        m_Actions[Common::Content::AssetGuidToText( guid )].Data = action;
        RebuildMappings();
    }

    Common::BoolResultStr EnhancedInputSubsystem::AddMappingContext( const InputMappingContextData& context,
                                                                     const int                      priority )
    {
        if ( !context.Header )
            return Common::MakeFormattedError<bool>( "a mapping context without a header has no identity" );
        const auto guid = Common::Content::AssetGuidFromText( context.Header->Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeFormattedError<bool>( "mapping context GUID '{}' is not an asset GUID",
                                                     context.Header->Guid );
        if ( auto valid = ValidateInputMappingContext( context ); !valid )
            return valid;
        for ( const InputKeyMappingData& mapping : context.Mappings )
        {
            const auto action = Common::Content::AssetGuidFromText( mapping.Action.Guid );
            if ( !m_Actions.contains( Common::Content::AssetGuidToText( action.GetValue() ) ) )
                return Common::MakeFormattedError<bool>(
                     "mapping context '{}' maps key '{}' to action '{}' ({}), which is not registered",
                     context.Header->Guid, mapping.Key, mapping.Action.Guid, mapping.Action.Path );
        }

        const std::string text = Common::Content::AssetGuidToText( guid.GetValue() );
        const auto        it   = std::find_if( m_Contexts.begin(), m_Contexts.end(),
                                               [&]( const ActiveContext& c ) { return c.GuidText == text; } );
        if ( it != m_Contexts.end() )
        {
            it->Data     = context;
            it->Priority = priority;
        }
        else
            m_Contexts.push_back( ActiveContext{ text, context, priority, m_NextOrder++ } );
        RebuildMappings();
        return BOOLSUCCESS;
    }

    bool EnhancedInputSubsystem::RemoveMappingContext( const Common::Content::AssetGuid& contextGuid )
    {
        const std::string text = Common::Content::AssetGuidToText( contextGuid );
        const auto        it   = std::find_if( m_Contexts.begin(), m_Contexts.end(),
                                               [&]( const ActiveContext& c ) { return c.GuidText == text; } );
        if ( it == m_Contexts.end() )
            return false;
        m_Contexts.erase( it );
        RebuildMappings();
        return true;
    }

    void EnhancedInputSubsystem::RebuildMappings()
    {
        std::vector<const ActiveContext*> order;
        for ( const ActiveContext& c : m_Contexts )
            order.push_back( &c );
        std::sort( order.begin(), order.end(), []( const ActiveContext* a, const ActiveContext* b )
                   { return a->Priority != b->Priority ? a->Priority > b->Priority : a->Order < b->Order; } );

        m_Mappings.clear();
        std::vector<InputKey> consumed; // by the contexts above the one being walked
        for ( const ActiveContext* context : order )
        {
            std::vector<InputKey> consumedHere;
            for ( const InputKeyMappingData& mapping : context->Data.Mappings )
            {
                const InputKey key = *InputKeyFromName( mapping.Key );
                if ( std::find( consumed.begin(), consumed.end(), key ) != consumed.end() )
                    continue;
                const std::string actionKey = Common::Content::AssetGuidToText(
                     Common::Content::AssetGuidFromText( mapping.Action.Guid ).GetValue() );
                // AddMappingContext refused a context naming an unregistered action, and actions are never
                // unregistered, so every mapped action is here.
                const auto     action = m_Actions.find( actionKey );
                MappingRuntime runtime;
                runtime.ActionKey = actionKey;
                runtime.ValueType = action->second.Data.ValueType;
                runtime.Key       = key;
                runtime.Modifiers = mapping.Modifiers;
                for ( const InputTriggerData& trigger : mapping.Triggers )
                    runtime.Triggers.push_back( TriggerRuntime{ trigger } );
                m_Mappings.push_back( std::move( runtime ) );
                if ( action->second.Data.ConsumeInput )
                    consumedHere.push_back( key );
            }
            consumed.insert( consumed.end(), consumedHere.begin(), consumedHere.end() );
        }
    }

    TriggerState EnhancedInputSubsystem::EvaluateTriggers( MappingRuntime& mapping, const glm::vec3& value,
                                                           const float deltaSeconds )
    {
        if ( mapping.Triggers.empty() )
            return glm::dot( value, value ) > 0.0f ? TriggerState::Triggered : TriggerState::None;

        TriggerState best = TriggerState::None;
        for ( TriggerRuntime& trigger : mapping.Triggers )
        {
            const float  threshold = trigger.Data.ActuationThreshold;
            const bool   actuated  = IsActuated( value, threshold );
            const bool   was       = IsActuated( mapping.LastValue, threshold );
            TriggerState state     = TriggerState::None;
            switch ( trigger.Data.Type )
            {
                case InputTriggerType::Down:
                    state = actuated ? TriggerState::Triggered : TriggerState::None;
                    break;
                case InputTriggerType::Pressed:
                    state = actuated && !was ? TriggerState::Triggered : TriggerState::None;
                    break;
                case InputTriggerType::Released:
                    state =
                         actuated ? TriggerState::Ongoing : ( was ? TriggerState::Triggered : TriggerState::None );
                    break;
                case InputTriggerType::Hold:
                {
                    if ( !actuated )
                    {
                        trigger.HeldDuration = 0.0f;
                        break;
                    }
                    const float holdTime = trigger.Data.Hold->HoldTimeSeconds;
                    const bool  first    = trigger.HeldDuration < holdTime; // not reached before this frame
                    trigger.HeldDuration += deltaSeconds;
                    state = TriggerState::Ongoing;
                    if ( trigger.HeldDuration >= holdTime )
                        state = ( first || !trigger.Data.Hold->IsOneShot ) ? TriggerState::Triggered
                                                                           : TriggerState::None;
                    break;
                }
            }
            trigger.LastState = state;
            best              = Stronger( best, state );
        }
        return best;
    }

    void EnhancedInputSubsystem::Tick( const RawInputFrame& frame, const float deltaSeconds )
    {
        struct Accumulated
        {
            TriggerState State = TriggerState::None;
            glm::vec3    Value{ 0.0f };
        };
        std::map<std::string, Accumulated> frameState;

        for ( MappingRuntime& mapping : m_Mappings )
        {
            const glm::vec3 value =
                 ApplyModifiers( RawKeyValue( mapping.Key, frame ), mapping.ValueType, mapping.Modifiers );
            const TriggerState state = EvaluateTriggers( mapping, value, deltaSeconds );
            mapping.LastValue        = value;

            Accumulated& acc = frameState[mapping.ActionKey];
            acc.State        = Stronger( acc.State, state );
            if ( state != TriggerState::None )
                for ( int c = 0; c < 3; ++c )
                    if ( std::abs( value[c] ) > std::abs( acc.Value[c] ) )
                        acc.Value[c] = value[c];
        }

        for ( auto& [key, action] : m_Actions )
        {
            const auto         found = frameState.find( key );
            const TriggerState now   = found != frameState.end() ? found->second.State : TriggerState::None;
            action.Events            = EventsForTransition( action.State, now );
            action.TriggeredSeconds =
                 now == TriggerState::Triggered
                      ? ( action.State == TriggerState::Triggered ? action.TriggeredSeconds + deltaSeconds : 0.0f )
                      : 0.0f;
            action.State = now;
            action.Value = now != TriggerState::None ? found->second.Value : glm::vec3( 0.0f );
        }
    }

    glm::vec3 EnhancedInputSubsystem::GetActionValue( const Common::Content::AssetGuid& action ) const
    {
        const auto it = m_Actions.find( Common::Content::AssetGuidToText( action ) );
        return it != m_Actions.end() ? it->second.Value : glm::vec3( 0.0f );
    }

    TriggerState EnhancedInputSubsystem::GetTriggerState( const Common::Content::AssetGuid& action ) const
    {
        const auto it = m_Actions.find( Common::Content::AssetGuidToText( action ) );
        return it != m_Actions.end() ? it->second.State : TriggerState::None;
    }

    TriggerEvents EnhancedInputSubsystem::GetTriggerEvents( const Common::Content::AssetGuid& action ) const
    {
        const auto it = m_Actions.find( Common::Content::AssetGuidToText( action ) );
        return it != m_Actions.end() ? it->second.Events : TriggerEvents{};
    }

    float EnhancedInputSubsystem::GetTriggeredSeconds( const Common::Content::AssetGuid& action ) const
    {
        const auto it = m_Actions.find( Common::Content::AssetGuidToText( action ) );
        return it != m_Actions.end() ? it->second.TriggeredSeconds : 0.0f;
    }
} // namespace Desert::Input
