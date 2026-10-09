#include "InputAssetEdit.hpp"

#include <Engine/Input/InputKey.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <algorithm>
#include <iterator>

namespace Desert::Editor::InputEdit
{
    namespace S = Assets::Serialization;

    namespace
    {
        Common::BoolResultStr CheckKey( const std::string& key )
        {
            if ( !Input::InputKeyFromName( key ) )
                return Common::MakeFormattedError<bool>( "'{}' is not a key this engine knows", key );
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr CheckAction( const Assets::AssetGuidRef& action )
        {
            const auto guid = Common::Content::AssetGuidFromText( action.Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeFormattedError<bool>( "action '{}' has no asset GUID ('{}')", action.Path,
                                                         action.Guid );
            return Common::MakeSuccess( true );
        }

        Common::ResultStr<S::InputKeyMappingData*> MappingAt( S::InputMappingContextData& context,
                                                              const std::size_t           mapping )
        {
            if ( mapping >= context.Mappings.size() )
                return Common::MakeFormattedError<S::InputKeyMappingData*>(
                     "mapping {} does not exist (the context has {})", mapping, context.Mappings.size() );
            return Common::MakeSuccess( &context.Mappings[mapping] );
        }
    } // namespace

    S::InputModifierData DefaultModifier( const S::InputModifierType type )
    {
        S::InputModifierData modifier;
        modifier.Type = type;
        switch ( type )
        {
            case S::InputModifierType::Negate:
                modifier.Negate = S::InputNegateParams{};
                break;
            case S::InputModifierType::Swizzle:
                modifier.Swizzle = S::InputSwizzleParams{};
                break;
            case S::InputModifierType::DeadZone:
                modifier.DeadZone = S::InputDeadZoneParams{};
                break;
            case S::InputModifierType::Scalar:
                modifier.Scalar = S::InputScalarParams{};
                break;
        }
        return modifier;
    }

    S::InputTriggerData DefaultTrigger( const S::InputTriggerType type )
    {
        S::InputTriggerData trigger;
        trigger.Type = type;
        if ( type == S::InputTriggerType::Hold )
            trigger.Hold = S::InputHoldParams{};
        return trigger;
    }

    Common::BoolResultStr SetValueType( S::InputActionData& action, const S::InputValueType type )
    {
        action.ValueType = type;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetConsumeInput( S::InputActionData& action, const bool consume )
    {
        action.ConsumeInput = consume;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AddMapping( S::InputMappingContextData& context, const Assets::AssetGuidRef& action,
                                      const std::string& key )
    {
        if ( auto ok = CheckKey( key ); !ok )
            return ok;
        if ( auto ok = CheckAction( action ); !ok )
            return ok;
        S::InputKeyMappingData mapping;
        mapping.Action = action;
        mapping.Key    = key;
        context.Mappings.push_back( std::move( mapping ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveMapping( S::InputMappingContextData& context, const std::size_t mapping )
    {
        if ( auto found = MappingAt( context, mapping ); !found )
            return Common::MakeError( found.GetError() );
        context.Mappings.erase( context.Mappings.begin() + static_cast<std::ptrdiff_t>( mapping ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetMappingKey( S::InputMappingContextData& context, const std::size_t mapping,
                                         const std::string& key )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        if ( auto ok = CheckKey( key ); !ok )
            return ok;
        found.GetValue()->Key = key;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetMappingAction( S::InputMappingContextData& context, const std::size_t mapping,
                                            const Assets::AssetGuidRef& action )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        if ( auto ok = CheckAction( action ); !ok )
            return ok;
        found.GetValue()->Action = action;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AddModifier( S::InputMappingContextData& context, const std::size_t mapping,
                                       const S::InputModifierType type )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        found.GetValue()->Modifiers.push_back( DefaultModifier( type ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveModifier( S::InputMappingContextData& context, const std::size_t mapping,
                                          const std::size_t modifier )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        auto& modifiers = found.GetValue()->Modifiers;
        if ( modifier >= modifiers.size() )
            return Common::MakeFormattedError<bool>( "mapping {} has no modifier {}", mapping, modifier );
        modifiers.erase( modifiers.begin() + static_cast<std::ptrdiff_t>( modifier ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr MoveModifier( S::InputMappingContextData& context, const std::size_t mapping,
                                        const std::size_t from, const std::size_t to )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        auto& modifiers = found.GetValue()->Modifiers;
        if ( from >= modifiers.size() || to >= modifiers.size() )
            return Common::MakeFormattedError<bool>( "mapping {} has {} modifiers; cannot move {} to {}", mapping,
                                                     modifiers.size(), from, to );
        S::InputModifierData moved = std::move( modifiers[from] );
        modifiers.erase( modifiers.begin() + static_cast<std::ptrdiff_t>( from ) );
        modifiers.insert( modifiers.begin() + static_cast<std::ptrdiff_t>( to ), std::move( moved ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr AddTrigger( S::InputMappingContextData& context, const std::size_t mapping,
                                      const S::InputTriggerType type )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        found.GetValue()->Triggers.push_back( DefaultTrigger( type ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveTrigger( S::InputMappingContextData& context, const std::size_t mapping,
                                         const std::size_t trigger )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        auto& triggers = found.GetValue()->Triggers;
        if ( trigger >= triggers.size() )
            return Common::MakeFormattedError<bool>( "mapping {} has no trigger {}", mapping, trigger );
        triggers.erase( triggers.begin() + static_cast<std::ptrdiff_t>( trigger ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetTriggerType( S::InputMappingContextData& context, const std::size_t mapping,
                                          const std::size_t trigger, const S::InputTriggerType type )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        auto& triggers = found.GetValue()->Triggers;
        if ( trigger >= triggers.size() )
            return Common::MakeFormattedError<bool>( "mapping {} has no trigger {}", mapping, trigger );
        S::InputTriggerData& edited = triggers[trigger];
        if ( edited.Type == type )
            return Common::MakeSuccess( true );
        const float threshold     = edited.ActuationThreshold;
        edited                    = DefaultTrigger( type );
        edited.ActuationThreshold = threshold;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetHoldSeconds( S::InputMappingContextData& context, const std::size_t mapping,
                                          const std::size_t trigger, const float seconds )
    {
        auto found = MappingAt( context, mapping );
        if ( !found )
            return Common::MakeError( found.GetError() );
        auto& triggers = found.GetValue()->Triggers;
        if ( trigger >= triggers.size() )
            return Common::MakeFormattedError<bool>( "mapping {} has no trigger {}", mapping, trigger );
        S::InputTriggerData& edited = triggers[trigger];
        if ( edited.Type != S::InputTriggerType::Hold || !edited.Hold )
            return Common::MakeFormattedError<bool>( "trigger {} of mapping {} is not a Hold", trigger, mapping );
        if ( !( seconds > 0.0f ) )
            return Common::MakeFormattedError<bool>( "a Hold needs a time above zero, not {} s", seconds );
        edited.Hold->HoldTimeSeconds = seconds;
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::InputEdit
