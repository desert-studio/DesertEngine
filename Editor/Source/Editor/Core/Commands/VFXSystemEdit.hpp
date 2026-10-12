#pragma once

/**
 * THE `.dfx` SYSTEM EDITOR'S COMMAND LAYER (VFX-12): every edit of a VFX system is one undo step.
 *
 * The editor window holds a WORKING copy of the system's VFXSystemData. A discrete gesture (add an emitter from a
 * template, add / remove / move / toggle a module, change an input's source) goes through ApplyVFXSystemEdit,
 * which runs the edit on a copy, refuses it by name when the result is not a valid system
 * (Serialization::ValidateVFXSystemData: the one statement of what a `.dfx` may hold), and only then writes the
 * working copy and pushes ONE CommandHistory entry holding the system before and after. A continuous gesture (a
 * slider drag) edits the working copy while the widget is held and is committed as one entry when it is released
 * (CommitVFXSystemGesture, same validation; an invalid result is rolled back). A system is a few kilobytes: a
 * by-value snapshot, no inverse to get wrong. An edit that changes nothing pushes nothing. EditedObject() is the
 * working copy, so the window's destructor forgets its entries with CommandHistory::DropFor(&working).
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace Desert::Editor
{
    /// One undo step of a VFX system: the whole system before and after.
    class VFXSystemEditCommand final : public ICommand
    {
    public:
        VFXSystemEditCommand( Assets::Serialization::VFXSystemData* target,
                              Assets::Serialization::VFXSystemData  before,
                              Assets::Serialization::VFXSystemData after, std::string label )
             : m_Target( target ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
               m_Label( std::move( label ) )
        {
        }

        bool Undo() override
        {
            *m_Target = m_Before;
            return true;
        }

        bool Redo() override
        {
            *m_Target = m_After;
            return true;
        }

        [[nodiscard]] const void* EditedObject() const override
        {
            return m_Target;
        }

        std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        Assets::Serialization::VFXSystemData* m_Target;
        Assets::Serialization::VFXSystemData  m_Before;
        Assets::Serialization::VFXSystemData  m_After;
        std::string                           m_Label;
    };

    /// Runs @p edit on a copy of @p working; a refused edit or an invalid result leaves @p working as it was and
    /// pushes nothing, a valid change is written and pushed to @p history as one entry labelled @p label.
    inline Common::BoolResultStr
    ApplyVFXSystemEdit( Assets::Serialization::VFXSystemData& working, CommandHistory& history,
                        const std::string&                                                                   label,
                        const std::function<Common::BoolResultStr( Assets::Serialization::VFXSystemData& )>& edit )
    {
        Assets::Serialization::VFXSystemData next = working;
        if ( auto done = edit( next ); !done )
            return Common::MakeFormattedError<bool>( "{}: {}", label, done.GetError() );
        if ( auto valid = Assets::Serialization::ValidateVFXSystemData( next ); !valid )
            return Common::MakeFormattedError<bool>( "{}: {}", label, valid.GetError() );
        if ( next == working )
            return BOOLSUCCESS;
        Assets::Serialization::VFXSystemData before = std::move( working );
        working                                     = next;
        history.PushCommand(
             std::make_unique<VFXSystemEditCommand>( &working, std::move( before ), std::move( next ), label ) );
        return BOOLSUCCESS;
    }

    /// Ends a continuous gesture that edited @p working in place since @p before: an invalid result is rolled
    /// back to @p before and refused by name, a valid change is pushed as one entry, no change pushes nothing.
    inline Common::BoolResultStr CommitVFXSystemGesture( Assets::Serialization::VFXSystemData&       working,
                                                         const Assets::Serialization::VFXSystemData& before,
                                                         CommandHistory& history, const std::string& label )
    {
        if ( auto valid = Assets::Serialization::ValidateVFXSystemData( working ); !valid )
        {
            working = before;
            return Common::MakeFormattedError<bool>( "{}: {}", label, valid.GetError() );
        }
        if ( working == before )
            return BOOLSUCCESS;
        history.PushCommand( std::make_unique<VFXSystemEditCommand>( &working, before, working, label ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor
