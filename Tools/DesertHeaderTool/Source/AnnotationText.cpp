#include "AnnotationText.hpp"

#include <cctype>

namespace Desert::HeaderTool
{
    // Reads the attribute's string argument, CONCATENATING adjacent literals ("a" "b" -> "ab").
    // Reading only the first one silently truncated every annotation long enough for clang-format to wrap
    // it at the 115-column limit — the symptom was a tooltip that ended mid-sentence in the generated file
    // ("...NOT a photometric unit (lux/candela): ") with nothing in the source looking wrong.
    // Escapes are preserved verbatim, because the text is re-emitted straight back into a C++ literal.
    std::string ExtractStringLiteral( const std::string& s )
    {
        std::string out;
        size_t      i = 0;
        while ( true )
        {
            const auto a = s.find( '"', i );
            if ( a == std::string::npos )
                break;

            size_t b = a + 1;
            while ( b < s.size() && s[b] != '"' )
                b += ( s[b] == '\\' && b + 1 < s.size() ) ? 2 : 1;
            if ( b >= s.size() )
                break;

            out += s.substr( a + 1, b - a - 1 );

            // Only whitespace may separate adjacent literals; anything else ends this attribute.
            size_t j = b + 1;
            while ( j < s.size() && ( std::isspace( static_cast<unsigned char>( s[j] ) ) != 0 ) )
                ++j;
            if ( j >= s.size() || s[j] != '"' )
                break;
            i = j;
        }
        return out;
    }

    // Splits the contents of PROPERTY( ... ) by top-level commas (ignoring commas inside (), <>, "").
    std::vector<std::string> SplitTopLevel( const std::string& s )
    {
        std::vector<std::string> out;
        std::string              cur;
        int                      paren = 0;
        int                      angle = 0;
        bool                     inStr = false;
        for ( const char c : s )
        {
            if ( inStr )
            {
                cur += c;
                if ( c == '"' )
                    inStr = false;
                continue;
            }
            switch ( c )
            {
                case '"':
                    inStr = true;
                    cur += c;
                    break;
                case '(':
                    ++paren;
                    cur += c;
                    break;
                case ')':
                    --paren;
                    cur += c;
                    break;
                case '<':
                    ++angle;
                    cur += c;
                    break;
                case '>':
                    --angle;
                    cur += c;
                    break;
                case ',':
                    if ( paren == 0 && angle == 0 )
                    {
                        out.push_back( cur );
                        cur.clear();
                    }
                    else
                        cur += c;
                    break;
                default:
                    cur += c;
                    break;
            }
        }
        if ( !cur.empty() )
            out.push_back( cur );
        return out;
    }

    std::string Trimmed( std::string v )
    {
        while ( !v.empty() && ( std::isspace( static_cast<unsigned char>( v.front() ) ) != 0 ) )
            v.erase( v.begin() );
        while ( !v.empty() && ( std::isspace( static_cast<unsigned char>( v.back() ) ) != 0 ) )
            v.pop_back();
        return v;
    }

    // The identifier inside "Word( Ident )", or empty.
    std::string ParenIdent( const std::string& tok )
    {
        const auto open  = tok.find( '(' );
        const auto close = tok.rfind( ')' );
        if ( open == std::string::npos || close == std::string::npos || close <= open )
            return {};
        return Trimmed( tok.substr( open + 1, close - open - 1 ) );
    }
} // namespace Desert::HeaderTool
