#pragma once

/**
 * ONE UNDO PATH FOR EVERY EDIT OF AN ENHANCED INPUT ASSET (UE: the FScopedTransaction the Input Action and
 * Input Mapping Context editors open on their UObject for every property change).
 *
 * The shape is AnimGraphEdit.hpp's: a by-value snapshot of the asset's data before, one after, AT MOST ONE
 * entry of the editor's ONE CommandHistory per interaction (a drag of a Hold time over forty frames is one
 * entry, a click that changed nothing is none). Both data types are a few hundred bytes and have a defaulted
 * operator==, so "did this interaction change anything" is `!=` over every field — a field added to either
 * struct cannot fall out of it.
 *
 * TWO DRIVERS, ONE BASELINE. `Edit` runs one of the operations below (an add, a remove, a reorder, a key
 * picked) as one entry; `Observe`, called once per frame after the widgets, turns a settled frame whose data
 * moved (a drag released, a checkbox clicked) into one entry. A move the history made itself (an Undo, a
 * Redo) is not an edit: the baseline follows it.
 *
 * THE OWNER RESOLVES AT EVERY USE, so an entry whose window was closed resolves to nothing and the history
 * discards it, instead of writing into freed memory.
 *
 * The operations are free functions over the data and touch no ImGui and no device: what the editors do is
 * what Tests/Editor/InputAssetEdit pins.
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/Serialization/InputAssets.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Desert::Editor
{
    namespace InputEdit
    {
        using Assets::Serialization::InputActionData;
        using Assets::Serialization::InputMappingContextData;
        using Assets::Serialization::InputModifierData;
        using Assets::Serialization::InputModifierType;
        using Assets::Serialization::InputTriggerData;
        using Assets::Serialization::InputTriggerType;
        using Assets::Serialization::InputValueType;

        /// A modifier of @p type with the parameters of that type present (UE's defaults) and no others.
        [[nodiscard]] InputModifierData DefaultModifier( InputModifierType type );
        /// A trigger of @p type; a Hold carries InputHoldParams, every other type none.
        [[nodiscard]] InputTriggerData DefaultTrigger( InputTriggerType type );

        Common::BoolResultStr SetValueType( InputActionData& action, InputValueType type );
        Common::BoolResultStr SetConsumeInput( InputActionData& action, bool consume );

        /// Appends a mapping of @p action on @p key. Refused: a key InputKeyFromName does not know, an action
        /// reference without a well-formed GUID.
        Common::BoolResultStr AddMapping( InputMappingContextData& context, const Assets::AssetGuidRef& action,
                                          const std::string& key );
        Common::BoolResultStr RemoveMapping( InputMappingContextData& context, std::size_t mapping );
        Common::BoolResultStr SetMappingKey( InputMappingContextData& context, std::size_t mapping,
                                             const std::string& key );
        Common::BoolResultStr SetMappingAction( InputMappingContextData& context, std::size_t mapping,
                                                const Assets::AssetGuidRef& action );

        /// Modifiers apply in list order (UE), so the order is an edit of its own.
        Common::BoolResultStr AddModifier( InputMappingContextData& context, std::size_t mapping,
                                           InputModifierType type );
        Common::BoolResultStr RemoveModifier( InputMappingContextData& context, std::size_t mapping,
                                              std::size_t modifier );
        Common::BoolResultStr MoveModifier( InputMappingContextData& context, std::size_t mapping,
                                            std::size_t from, std::size_t to );

        Common::BoolResultStr AddTrigger( InputMappingContextData& context, std::size_t mapping,
                                          InputTriggerType type );
        Common::BoolResultStr RemoveTrigger( InputMappingContextData& context, std::size_t mapping,
                                             std::size_t trigger );
        /// Changes the trigger's type keeping its threshold; Hold parameters appear or go with the type.
        Common::BoolResultStr SetTriggerType( InputMappingContextData& context, std::size_t mapping,
                                              std::size_t trigger, InputTriggerType type );
        /// Refused: a trigger that is not a Hold, a time that is not above zero.
        Common::BoolResultStr SetHoldSeconds( InputMappingContextData& context, std::size_t mapping,
                                              std::size_t trigger, float seconds );
    } // namespace InputEdit

    /**
     * @brief Whose data an edit is on, resolved at every use (the open editor's working copy).
     */
    template <typename Data>
    struct InputAssetOwner
    {
        std::string            Label; ///< "Input Action 'IA_Jump'", for the history's entry
        std::function<Data*()> Resolve;
        std::function<void()>  AfterRestore;
        bool                   Volatile = true;
    };

    /**
     * @brief ONE interaction's net effect on one input asset's data.
     */
    template <typename Data>
    class InputAssetEditCommand final : public ICommand
    {
    public:
        InputAssetEditCommand( InputAssetOwner<Data> owner, Data before, Data after )
             : m_Owner( std::move( owner ) ), m_Before( std::move( before ) ), m_After( std::move( after ) )
        {
        }

        bool Undo() override
        {
            return Apply( m_Before );
        }
        bool Redo() override
        {
            return Apply( m_After );
        }
        [[nodiscard]] bool IsVolatile() const override
        {
            return m_Owner.Volatile;
        }
        [[nodiscard]] std::string GetLabel() const override
        {
            return std::format( "Edit {}", m_Owner.Label );
        }

    private:
        [[nodiscard]] bool Apply( const Data& value ) const
        {
            Data* data = m_Owner.Resolve ? m_Owner.Resolve() : nullptr;
            if ( data == nullptr )
                return false; // the window is closed: the history discards the entry
            *data = value;
            if ( m_Owner.AfterRestore )
                m_Owner.AfterRestore();
            return true;
        }

        InputAssetOwner<Data> m_Owner;
        Data                  m_Before;
        Data                  m_After;
    };

    /**
     * @brief The interaction boundary of one editor window: at most one InputAssetEditCommand per interaction.
     */
    template <typename Data>
    class InputAssetEditTransaction
    {
    public:
        using Operation = std::function<Common::BoolResultStr( Data& )>;

        /// Runs @p operation on the owner's data as ONE entry. A refused operation leaves the data as it was
        /// and pushes nothing; one that changed nothing pushes nothing.
        Common::BoolResultStr Edit( const InputAssetOwner<Data>& owner, const Operation& operation )
        {
            Data* data = owner.Resolve ? owner.Resolve() : nullptr;
            if ( data == nullptr )
                return Common::MakeFormattedError<bool>( "{} is not open", owner.Label );
            Settle( owner, *data );
            const Data before = *data;
            if ( auto done = operation( *data ); !done )
            {
                *data = before;
                return done;
            }
            Push( owner, before, *data );
            return Common::MakeSuccess( true );
        }

        /// Once per frame, after every widget wrote. @p held: an item is still being dragged or typed into, so
        /// the interaction has not ended. Returns the entries pushed (0 or 1).
        uint32_t Observe( const InputAssetOwner<Data>& owner, const bool held )
        {
            Data* data = owner.Resolve ? owner.Resolve() : nullptr;
            if ( data == nullptr )
                return 0;
            if ( !m_Baseline || CommandHistory::Get().Revision() != m_HistorySeen )
            {
                Rebase( *data );
                return 0;
            }
            if ( held || *data == *m_Baseline )
                return 0;
            const Data before = *m_Baseline;
            return Push( owner, before, *data );
        }

    private:
        // The baseline is the data as of the last settled frame; an Undo/Redo since moved it, so it follows.
        void Settle( const InputAssetOwner<Data>& owner, const Data& data )
        {
            if ( !m_Baseline || CommandHistory::Get().Revision() != m_HistorySeen )
                Rebase( data );
            else if ( data != *m_Baseline )
                Push( owner, Data( *m_Baseline ), data ); // a widget's write not yet observed is its own entry
        }

        uint32_t Push( const InputAssetOwner<Data>& owner, const Data& before, const Data& after )
        {
            if ( before == after )
            {
                Rebase( after );
                return 0;
            }
            CommandHistory::Get().PushCommand(
                 std::make_unique<InputAssetEditCommand<Data>>( owner, before, after ) );
            Rebase( after );
            return 1;
        }

        void Rebase( const Data& data )
        {
            m_Baseline    = data;
            m_HistorySeen = CommandHistory::Get().Revision();
        }

        std::optional<Data> m_Baseline;
        uint64_t            m_HistorySeen = 0;
    };

    using InputActionEditTransaction  = InputAssetEditTransaction<Assets::Serialization::InputActionData>;
    using InputContextEditTransaction = InputAssetEditTransaction<Assets::Serialization::InputMappingContextData>;
} // namespace Desert::Editor
