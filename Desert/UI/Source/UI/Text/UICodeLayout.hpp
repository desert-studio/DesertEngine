#pragma once

#include <UI/Text/UICodeTokenizer.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// THE LINES OF A CODE FIELD — the layout half of SMultiLineEditableText (UE's FTextLayout + the marshaller
// that fills it), kept apart from the text model (UITextEditState: caret, selection, undo) and from the
// language (IUICodeTokenizer). It knows where every line starts and what each line inherits from the one
// before it (the tokenizer's line state); it lays out and tokenizes nothing else, so a frame touches only the
// lines it shows (VisibleLines -> Runs), however long the text.
//
// No pixels: line height and scroll come from the widget, which owns the font.
namespace Desert::UI
{
    class UICodeLayout
    {
    public:
        // Re-cut @p text into lines and carry the tokenizer's state down them — only when the text or the
        // tokenizer differs from the last call (O(text) then, a compare otherwise). @p tokenizer may be null:
        // every run is then Text.
        void Update( const std::string& text, const IUICodeTokenizer* tokenizer );

        // Always >= 1: an empty text is one empty line.
        [[nodiscard]] std::size_t LineCount() const
        {
            return m_Starts.size();
        }
        // The byte line @p line starts at, and the byte it ends at (its '\n', or the text's end).
        [[nodiscard]] std::size_t LineStart( std::size_t line ) const;
        [[nodiscard]] std::size_t LineEnd( std::size_t line ) const;
        // The line byte @p offset is on.
        [[nodiscard]] std::size_t LineOf( std::size_t offset ) const;

        // Line @p line cut into runs that cover it end to end (what the tokenizer did not single out is Text),
        // in TEXT offsets. Appends to @p out. The text is the one last passed to Update.
        void Runs( std::size_t line, std::vector<UICodeToken>& out ) const;

        // [first, last) — the lines a box @p heightPx tall shows when scrolled down by @p scrollY at @p lineH
        // per line, partly shown ones included.
        [[nodiscard]] std::pair<std::size_t, std::size_t> VisibleLines( float scrollY, float heightPx,
                                                                        float lineH ) const;

    private:
        std::string                  m_Text;
        const IUICodeTokenizer*      m_Tokenizer = nullptr;
        bool                         m_Valid     = false;
        std::vector<std::size_t>     m_Starts{ 0 };
        std::vector<UICodeLineState> m_Entry{ 0 }; // the state each line starts in
    };
} // namespace Desert::UI
