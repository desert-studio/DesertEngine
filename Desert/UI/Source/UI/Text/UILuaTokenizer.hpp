#pragma once

#include <UI/Text/UICodeTokenizer.hpp>

// LUA'S WORDS for a code field (UICodeTokenizer.hpp), after the Lua 5.4 lexer (llex.c: llex, read_long_string,
// read_string, read_numeral): the 22 reserved words, short strings '…' / "…" with backslash escapes, long
// strings [[…]] / [==[…]==] and long comments --[[…]] of any level (open across lines: the line state is the
// open bracket's level and whether it is a comment), line comments --, and numerals (decimal with fraction and
// exponent, hexadecimal with fraction and binary exponent).
//
// Not a validator: a malformed numeral or an unterminated short string is coloured as far as it goes, as an
// editor shows code being typed. A short string ending in a backslash-newline ('\' then a line break, or \z)
// is not carried to the next line — the next line starts outside any string.
namespace Desert::UI
{
    class UILuaTokenizer final : public IUICodeTokenizer
    {
    public:
        UICodeLineState Tokenize( std::string_view line, UICodeLineState entry,
                                  std::vector<UICodeToken>& out ) const override;
    };
} // namespace Desert::UI
