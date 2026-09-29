#pragma once

namespace Desert::Editor
{
    // THE QUESTION A DIRTY DOCUMENT ASKS BEFORE IT CLOSES (UE: "Save changes to ...?" with Save / Don't Save /
    // Cancel). Asked only when the person closed the window (its x, a tab, the palette's "Close <name>", Close
    // All); a close the editor makes on its own (the subject was deleted, view memory is short) cannot ask
    // anyone and does not. Pure rule, so the suite drives it without a window.
    enum class UnsavedCloseChoice
    {
        Save,    // write the file, then close - and stay open if the write failed
        Discard, // put the file's content back into memory (DiscardEdits), then close
        Cancel   // keep the window, change nothing
    };

    // Does this close ask? Only a document that KNOWS it differs from its file does: Untracked cannot tell,
    // and a question it cannot justify is noise (IPanel.hpp, DiskState).
    [[nodiscard]] constexpr bool CloseAsksFirst( const bool dirty, const bool closedByThePerson ) noexcept
    {
        return dirty && closedByThePerson;
    }

    // Does the window close after the answer? @p saved is whether SaveDocument actually wrote the file.
    [[nodiscard]] constexpr bool CloseAfterAnswer( const UnsavedCloseChoice choice, const bool saved ) noexcept
    {
        switch ( choice )
        {
            case UnsavedCloseChoice::Save:
                return saved;
            case UnsavedCloseChoice::Discard:
                return true;
            case UnsavedCloseChoice::Cancel:
                return false;
        }
        return false;
    }
} // namespace Desert::Editor
