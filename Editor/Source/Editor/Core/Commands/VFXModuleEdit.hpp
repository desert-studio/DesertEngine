#pragma once

// VFX-MODULE-ASSET. THE EDITS A `.dfxmod` WINDOW MAKES (VFXModuleDocument): one command layer, so the window owns no
// rule about what a module may hold - every edit is applied to a copy, the copy must pass the format's own rules
// (VFX::ValidateVFXModule: its canonical text reads back as the same module), and only then becomes the working
// copy and one CommandHistory entry. A refused edit changes nothing and its refusal is the reader's.

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/VFX/VFXModuleFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace Desert::Editor
{
    /// The whole definition before and after one edit (a module is small: a snapshot is the simplest exact undo).
    class VFXModuleEditCommand final : public ICommand
    {
    public:
        VFXModuleEditCommand( VFX::VFXModuleDefinition* target, VFX::VFXModuleDefinition before,
                              VFX::VFXModuleDefinition after, std::string label )
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
        VFX::VFXModuleDefinition* m_Target;
        VFX::VFXModuleDefinition  m_Before;
        VFX::VFXModuleDefinition  m_After;
        std::string               m_Label;
    };

    /// Applies @p edit to a copy of @p working; a valid, different result becomes @p working and one entry of
    /// @p history labelled @p label. The GUID is not the edit's to change.
    inline Common::BoolResultStr
    ApplyVFXModuleEdit( VFX::VFXModuleDefinition& working, CommandHistory& history, const std::string& label,
                        const std::function<Common::BoolResultStr( VFX::VFXModuleDefinition& )>& edit )
    {
        VFX::VFXModuleDefinition next = working;
        if ( auto done = edit( next ); !done )
            return Common::MakeFormattedError<bool>( "{}: {}", label, done.GetError() );
        if ( next.Guid != working.Guid )
            return Common::MakeFormattedError<bool>( "{}: a module's GUID is its identity and is not edited", label );
        if ( auto valid = VFX::ValidateVFXModule( next ); !valid )
            return Common::MakeFormattedError<bool>( "{}: {}", label, valid.GetError() );
        if ( next == working )
            return BOOLSUCCESS;
        VFX::VFXModuleDefinition before = std::move( working );
        working                         = next;
        history.PushCommand(
             std::make_unique<VFXModuleEditCommand>( &working, std::move( before ), std::move( next ), label ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor
