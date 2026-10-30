#include "FuzzyMatch.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace Desert::Editor
{
    namespace
    {
        char Lower( char c )
        {
            return static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        }

        bool IsSeparator( char c )
        {
            return c == ' ' || c == '/' || c == '\\' || c == '_' || c == '-' || c == '.' || c == ':';
        }
    } // namespace

    bool FuzzyMatch( std::string_view query, std::string_view text, int& outScore )
    {
        outScore = 0;
        if ( query.empty() )
            return true;
        if ( text.empty() )
            return false;

        constexpr int kSeq = 15, kSep = 10, kCamel = 10, kFirst = 15;

        int         score       = 100;
        std::size_t qi          = 0;
        int         firstMatch  = -1;
        bool        prevMatched = false;

        for ( std::size_t ti = 0; ti < text.size() && qi < query.size(); ++ti )
        {
            if ( Lower( text[ti] ) != Lower( query[qi] ) )
            {
                prevMatched = false;
                continue;
            }

            if ( firstMatch < 0 )
                firstMatch = static_cast<int>( ti );

            if ( ti == 0 )
            {
                score += kFirst;
            }
            else
            {
                const char prev = text[ti - 1];
                if ( IsSeparator( prev ) )
                    score += kSep;
                else if ( std::islower( static_cast<unsigned char>( prev ) ) &&
                          std::isupper( static_cast<unsigned char>( text[ti] ) ) )
                    score += kCamel;
            }
            if ( prevMatched )
                score += kSeq;

            prevMatched = true;
            ++qi;
        }

        if ( qi != query.size() )
            return false;

        score -= std::min( firstMatch, 10 );                              // later start = worse
        score -= static_cast<int>( std::min<std::size_t>( text.size(), 50 ) ); // prefer shorter text
        outScore = score;
        return true;
    }
} // namespace Desert::Editor
