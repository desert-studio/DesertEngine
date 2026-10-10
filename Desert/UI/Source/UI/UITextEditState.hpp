#pragma once

#include <UI/Args/UIControlArgs.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// THE EDITING OF ONE TEXT FIELD — caret, selection, undo, clipboard — as a value with no font, no draw list
// and no platform in it. Ported from UE's FSlateEditableTextLayout (Slate/Public/Widgets/Text/
// SlateEditableTextLayout.h:37): the same split between the text (owned by the field's data) and the edit
// state (owned by the view), the same operations (MoveCursor :356, HandleTypeChar :293, HandleBackspace
// :287, HandleDelete :290, SelectAllText :317, SelectWordAt :320, Cut/Copy/Paste :335-347, Undo/Redo
// :464-470 over FUndoState snapshots :458), in the project's style: std::string UTF-8, byte offsets that
// always sit on a codepoint boundary.
//
// The widget (UI/Widgets/InputField.cpp) owns the pixels: it turns a click into an offset and an offset
// into a caret x. Everything here is decidable from the string alone, which is what makes the edit rules
// testable without a font (suite UITextEdit).

namespace Desert::UI
{
    struct UIKeyEvent;

    // The system clipboard as the UI sees it. The HOST supplies one through UIInput::Clipboard (the
    // runtime and the editor wrap their Desert::Window); null means "no clipboard", and copy/cut/paste
    // then do nothing — the UI never reaches for a platform on its own.
    struct IUIClipboard
    {
        virtual ~IUIClipboard()                                = default;
        virtual std::string GetText() const                    = 0;
        virtual void        SetText( const std::string& text ) = 0;
    };

    // The authored rules an edit is checked against — the field's data, read once per frame.
    struct UITextEditRules
    {
        bool             MultiLine = false;
        bool             Password  = false;
        std::size_t      MaxLength = 0; // codepoints; 0 = unlimited
        UITextCharFilter Filter    = UITextCharFilter::Any;
        UITextNewLineKey NewLine   = UITextNewLineKey::ShiftEnter;

        static UITextEditRules Of( const UIInputFieldData& f );
    };

    // UE's ECursorMoveMethod + the granularity of FMoveCursor, flattened.
    enum class UITextMove : uint8_t
    {
        Left,
        Right,
        WordLeft,
        WordRight,
        LineStart,
        LineEnd,
        Start,
        End,
        Up,   // previous line, same codepoint column (multi-line)
        Down, // next line, same codepoint column (multi-line)
    };

    // What one frame of keys did to the field.
    struct UITextEditOutcome
    {
        bool Changed   = false; // the text differs from before
        bool Committed = false; // Enter (the committing one) was pressed
    };

    class UITextEditState
    {
    public:
        // --- Where the caret is ---------------------------------------------------------------------
        std::size_t Caret() const
        {
            return m_Caret;
        }
        std::size_t Anchor() const
        {
            return m_Anchor;
        }
        bool HasSelection() const
        {
            return m_Caret != m_Anchor;
        }
        // [lo, hi) of the selection, in bytes.
        std::pair<std::size_t, std::size_t> Selection() const;
        std::string                         SelectedText( const std::string& text ) const;

        // The text was written by someone else (a binding, a script, the Details panel): pull the caret and
        // anchor back inside it, onto a codepoint boundary. Called every frame before anything reads them.
        void Sync( const std::string& text );

        // Place the caret at @p offset (snapped to a boundary); @p extend keeps the anchor (Shift+click).
        void SetCaret( const std::string& text, std::size_t offset, bool extend );
        void Move( const std::string& text, UITextMove move, bool extend );
        void SelectAll( const std::string& text );
        void SelectWordAt( const std::string& text, std::size_t offset );

        // --- Edits: each one is a single undo step and returns whether the text changed ----------------
        // Insert @p typed at the caret, replacing the selection. Characters the rules refuse are dropped
        // (control characters; line breaks in a single-line field; the filter's rejects); the result is cut
        // at MaxLength codepoints.
        bool Insert( std::string& text, std::string_view typed, const UITextEditRules& rules );
        bool Backspace( std::string& text, bool word );
        bool Delete( std::string& text, bool word );
        // Copy and cut refuse a password field (UE: CanExecuteCopy/CanExecuteCut).
        bool Copy( const std::string& text, const UITextEditRules& rules, IUIClipboard& clipboard ) const;
        bool Cut( std::string& text, const UITextEditRules& rules, IUIClipboard& clipboard );
        bool Paste( std::string& text, const UITextEditRules& rules, const IUIClipboard& clipboard );
        bool Undo( std::string& text );
        bool Redo( std::string& text );
        bool CanUndo() const
        {
            return !m_Undo.empty();
        }
        bool CanRedo() const
        {
            return !m_Redo.empty();
        }

        // One frame of a focused field: @p typed first (it was typed before the keys that end the frame),
        // then every key event in arrival order. @p clipboard may be null.
        UITextEditOutcome Apply( std::string& text, const UITextEditRules& rules, std::string_view typed,
                                 std::span<const UIKeyEvent> keys, IUIClipboard* clipboard );

        // --- The view's side, kept here so the field has ONE cell of runtime state ------------------
        float  ScrollX       = 0.0f;  // px the text is shifted left so the caret stays inside (single-line)
        float  ScrollY       = 0.0f;  // px the text is shifted up (multi-line)
        bool   HadFocus      = false; // focused last frame: losing it commits (UE ETextCommit::OnUserMovedFocus)
        bool   Dragging      = false; // a press landed in the field and the button is still held
        double LastClickTime = -1.0;  // for the double-click word select

        // Most snapshots kept; the oldest goes first.
        static constexpr std::size_t kMaxUndo = 128;

    private:
        struct Snapshot
        {
            std::string Text;
            std::size_t Caret  = 0;
            std::size_t Anchor = 0;
        };
        // Called BEFORE an edit: what Undo returns to. Clears the redo branch.
        void PushUndo( const std::string& text );
        // Remove the selection (no undo push — the caller made it); returns whether anything went.
        bool EraseSelection( std::string& text );

        std::size_t           m_Caret  = 0;
        std::size_t           m_Anchor = 0;
        std::vector<Snapshot> m_Undo;
        std::vector<Snapshot> m_Redo;
    };

    // --- Offsets of the DRAWN string --------------------------------------------------------------------
    // A password field draws one bullet per codepoint; everything else draws its own text.
    std::string UITextDisplayString( const std::string& text, bool password );
    // The byte offset in UITextDisplayString( text, password ) that @p offset of @p text lands on.
    std::size_t UITextDisplayOffset( const std::string& text, std::size_t offset, bool password );
    // Line @p offset is on (0-based, '\n' separated) and the byte where that line starts.
    std::size_t UITextLineOf( const std::string& text, std::size_t offset );
    std::size_t UITextLineStart( const std::string& text, std::size_t offset );
} // namespace Desert::UI
