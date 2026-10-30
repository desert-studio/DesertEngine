#include <UI/Text/UILuaTokenizer.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace Desert::UI
{
    namespace
    {
        // llex.c luaX_tokens, the reserved words (Lua 5.4).
        constexpr std::array<std::string_view, 22> kLuaKeywords{
             "and", "break", "do",  "else", "elseif", "end",    "false",  "for",  "function", "goto",  "if",
             "in",  "local", "nil", "not",  "or",     "repeat", "return", "then", "true",     "until", "while",
        };

        constexpr std::size_t kNone = std::string_view::npos;

        bool IsIdentStart( char c )
        {
            return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
        }
        bool IsDigit( char c )
        {
            return c >= '0' && c <= '9';
        }
        bool IsIdent( char c )
        {
            return IsIdentStart( c ) || IsDigit( c );
        }
        bool IsHexDigit( char c )
        {
            return IsDigit( c ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
        }

        // llex.c skip_sep: at @p at, a long bracket "[" "="* "[" opens — its level (the count of '='), or -1.
        int LongOpenLevel( std::string_view line, std::size_t at )
        {
            if ( at >= line.size() || line[at] != '[' )
                return -1;
            std::size_t j = at + 1;
            while ( j < line.size() && line[j] == '=' )
                ++j;
            return j < line.size() && line[j] == '[' ? static_cast<int>( j - at - 1 ) : -1;
        }

        // From @p from: one past the "]" "="*level "]" that closes a long bracket of @p level, or kNone.
        std::size_t LongCloseEnd( std::string_view line, std::size_t from, int level )
        {
            for ( std::size_t i = from; i < line.size(); ++i )
            {
                if ( line[i] != ']' )
                    continue;
                std::size_t j      = i + 1;
                int         equals = 0;
                while ( j < line.size() && line[j] == '=' )
                {
                    ++equals;
                    ++j;
                }
                if ( equals == level && j < line.size() && line[j] == ']' )
                    return j + 1;
            }
            return kNone;
        }

        // The line state of an open long bracket: its level + 1 above bit 0, bit 0 set for a comment. Never 0.
        UICodeLineState LongState( int level, bool comment )
        {
            return ( static_cast<UICodeLineState>( level + 1 ) << 1u ) | ( comment ? 1u : 0u );
        }

        // llex.c read_numeral: one past the numeral starting at @p at (a digit, or '.' before a digit).
        std::size_t NumeralEnd( std::string_view line, std::size_t at )
        {
            std::size_t j   = at;
            bool        hex = false;
            if ( line[j] == '0' && j + 1 < line.size() && ( line[j + 1] == 'x' || line[j + 1] == 'X' ) )
            {
                hex = true;
                j += 2;
            }
            while ( j < line.size() )
            {
                const char c        = line[j];
                const bool exponent = hex ? ( c == 'p' || c == 'P' ) : ( c == 'e' || c == 'E' );
                if ( exponent )
                {
                    ++j;
                    if ( j < line.size() && ( line[j] == '+' || line[j] == '-' ) )
                        ++j;
                }
                else if ( c == '.' || ( hex ? IsHexDigit( c ) : IsDigit( c ) ) )
                    ++j;
                else
                    break;
            }
            return j;
        }
    } // namespace

    UICodeLineState UILuaTokenizer::Tokenize( std::string_view line, UICodeLineState entry,
                                              std::vector<UICodeToken>& out ) const
    {
        auto push = [&out]( std::size_t begin, std::size_t end, UICodeTokenKind kind )
        {
            if ( end > begin )
                out.push_back( { begin, end, kind } );
        };

        std::size_t i = 0;
        // A long comment or string left open by an earlier line runs until its closing bracket.
        if ( entry != 0 )
        {
            const int             level = static_cast<int>( entry >> 1u ) - 1;
            const UICodeTokenKind kind  = ( entry & 1u ) != 0 ? UICodeTokenKind::Comment : UICodeTokenKind::String;
            const std::size_t     close = LongCloseEnd( line, 0, level );
            if ( close == kNone )
            {
                push( 0, line.size(), kind );
                return entry;
            }
            push( 0, close, kind );
            i = close;
        }

        while ( i < line.size() )
        {
            const char c = line[i];
            if ( c == '-' && i + 1 < line.size() && line[i + 1] == '-' )
            {
                // --[[ … ]] (any level) is a long comment; any other -- runs to the end of the line.
                const int level = LongOpenLevel( line, i + 2 );
                if ( level < 0 )
                {
                    push( i, line.size(), UICodeTokenKind::Comment );
                    return 0;
                }
                const std::size_t close =
                     LongCloseEnd( line, i + 2 + static_cast<std::size_t>( level ) + 2, level );
                if ( close == kNone )
                {
                    push( i, line.size(), UICodeTokenKind::Comment );
                    return LongState( level, true );
                }
                push( i, close, UICodeTokenKind::Comment );
                i = close;
                continue;
            }
            if ( c == '[' )
            {
                const int level = LongOpenLevel( line, i );
                if ( level < 0 )
                {
                    ++i; // an index bracket
                    continue;
                }
                const std::size_t close = LongCloseEnd( line, i + static_cast<std::size_t>( level ) + 2, level );
                if ( close == kNone )
                {
                    push( i, line.size(), UICodeTokenKind::String );
                    return LongState( level, false );
                }
                push( i, close, UICodeTokenKind::String );
                i = close;
                continue;
            }
            if ( c == '"' || c == '\'' )
            {
                // llex.c read_string: to the same quote; a backslash takes the next byte with it.
                std::size_t j = i + 1;
                while ( j < line.size() && line[j] != c )
                    j += ( line[j] == '\\' && j + 1 < line.size() ) ? 2 : 1;
                j = std::min( j + 1, line.size() );
                push( i, j, UICodeTokenKind::String );
                i = j;
                continue;
            }
            if ( IsDigit( c ) || ( c == '.' && i + 1 < line.size() && IsDigit( line[i + 1] ) ) )
            {
                const std::size_t end = NumeralEnd( line, i );
                push( i, end, UICodeTokenKind::Number );
                i = end;
                continue;
            }
            if ( IsIdentStart( c ) )
            {
                std::size_t j = i + 1;
                while ( j < line.size() && IsIdent( line[j] ) )
                    ++j;
                const std::string_view word = line.substr( i, j - i );
                if ( std::ranges::find( kLuaKeywords, word ) != kLuaKeywords.end() )
                    push( i, j, UICodeTokenKind::Keyword );
                i = j; // a name is Text, digits inside it included
                continue;
            }
            ++i;
        }
        return 0;
    }
} // namespace Desert::UI
