#pragma once

/**
 * THE `.dfx` SYSTEM EDITOR'S UNDO STEP (VFX-12c): every edit of a VFX system is one CommandHistory entry.
 *
 * The VFX System document holds a WORKING copy of the system's VFXSystemData. A gesture - a drag of a stack input,
 * a typed number, a `set` from the control socket - changes the working copy, and when it ends the document pushes
 * ONE entry holding the whole system before and after (a system is a few emitters of a few modules: a by-value
 * snapshot, no inverse to get wrong; UE's FScopedTransaction around a stack edit records the whole UNiagaraSystem
 * the same way). EditedObject() is the working copy, so the document's destructor forgets its entries with
 * CommandHistory::DropFor(&working).
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <string>
#include <utility>

namespace Desert::Editor
{
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
} // namespace Desert::Editor
