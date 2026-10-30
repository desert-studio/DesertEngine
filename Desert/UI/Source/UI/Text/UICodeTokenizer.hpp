#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// WHAT A CODE FIELD'S HIGHLIGHTING IS — UE's ITextLayoutMarshaller as FSyntaxHighlighterTextLayoutMarshaller
// specialises it (Slate/Public/Framework/Text/SyntaxHighlighterTextLayoutMarshaller.h): the text is cut into
// runs, and each run carries a style. Here the run's style is a KIND, and the widget (Widgets::CodeEditor)
// turns a kind into a colour from its theme, so a language says what its words ARE and never how they look.
//
// One line at a time, with a state carried across lines (UE's tokenizer carries the same thing in its
// FTokenizedLine / ParseState): a multi-line construct — a Lua long comment `--[[ … ]]`, a long string
// `[==[ … ]==]`, a C block comment — is open at the end of one line and still open at the start of the next.
// That is what lets the layout re-tokenize only the lines it shows: a line's tokens depend on its own text
// and its entry state alone (UICodeLayout keeps every line's entry state).
namespace Desert::UI
{
    enum class UICodeTokenKind : std::uint8_t
    {
        Text, // anything the language does not single out: names, operators, white space
        Keyword,
        String,
        Number,
        Comment,
    };

    // [Begin, End) bytes of ONE line (no '\n' inside), and what they are.
    struct UICodeToken
    {
        std::size_t     Begin = 0;
        std::size_t     End   = 0;
        UICodeTokenKind Kind  = UICodeTokenKind::Text;
    };

    // What is still open where a line ends — the tokenizer's own encoding; 0 is "nothing open", the state the
    // first line starts in.
    using UICodeLineState = std::uint32_t;

    class IUICodeTokenizer
    {
    public:
        virtual ~IUICodeTokenizer() = default;

        // Append @p line's tokens to @p out in order, none overlapping and none empty; a byte no token covers
        // is Text. @p entry is what was open where the line starts; the return is what is open where it ends.
        // @p line holds no '\n'.
        virtual UICodeLineState Tokenize( std::string_view line, UICodeLineState entry,
                                          std::vector<UICodeToken>& out ) const = 0;
    };
} // namespace Desert::UI
