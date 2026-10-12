#include "Utf8.hpp"

namespace Desert::Text
{
    namespace
    {
        bool IsContinuation( unsigned char b )
        {
            return ( b & 0xC0u ) == 0x80u;
        }
    } // namespace

    uint32_t Utf8Next( const std::string& s, size_t& i )
    {
        if ( i >= s.size() )
            return 0;

        const unsigned char b0 = static_cast<unsigned char>( s[i] );
        if ( b0 < 0x80u ) // ASCII
        {
            ++i;
            return b0;
        }

        // Sequence length from the lead byte. 0xC0/0xC1 and > 0xF4 are never valid leads.
        int      extra = 0;
        uint32_t cp    = 0;
        if ( ( b0 & 0xE0u ) == 0xC0u && b0 >= 0xC2u )
            extra = 1, cp = b0 & 0x1Fu;
        else if ( ( b0 & 0xF0u ) == 0xE0u )
            extra = 2, cp = b0 & 0x0Fu;
        else if ( ( b0 & 0xF8u ) == 0xF0u && b0 <= 0xF4u )
            extra = 3, cp = b0 & 0x07u;
        else
        {
            ++i; // stray continuation byte or an invalid lead
            return kReplacementChar;
        }

        if ( i + static_cast<size_t>( extra ) >= s.size() ) // truncated at the end of the string
        {
            ++i;
            return kReplacementChar;
        }

        for ( int k = 1; k <= extra; ++k )
        {
            const unsigned char bn = static_cast<unsigned char>( s[i + static_cast<size_t>( k )] );
            if ( !IsContinuation( bn ) )
            {
                ++i; // resync on the offending byte rather than swallowing it
                return kReplacementChar;
            }
            cp = ( cp << 6 ) | ( bn & 0x3Fu );
        }
        i += static_cast<size_t>( extra ) + 1;

        // Reject surrogates and out-of-range values; overlong forms are already excluded by the lead checks.
        if ( ( cp >= 0xD800u && cp <= 0xDFFFu ) || cp > 0x10FFFFu )
            return kReplacementChar;
        return cp;
    }

    std::vector<uint32_t> Utf8Decode( const std::string& s )
    {
        std::vector<uint32_t> out;
        out.reserve( s.size() );
        for ( size_t i = 0; i < s.size(); )
            out.push_back( Utf8Next( s, i ) );
        return out;
    }

    void Utf8Append( std::string& s, uint32_t cp )
    {
        if ( cp < 0x80u )
        {
            s += static_cast<char>( cp );
        }
        else if ( cp < 0x800u )
        {
            s += static_cast<char>( 0xC0u | ( cp >> 6 ) );
            s += static_cast<char>( 0x80u | ( cp & 0x3Fu ) );
        }
        else if ( cp < 0x10000u )
        {
            s += static_cast<char>( 0xE0u | ( cp >> 12 ) );
            s += static_cast<char>( 0x80u | ( ( cp >> 6 ) & 0x3Fu ) );
            s += static_cast<char>( 0x80u | ( cp & 0x3Fu ) );
        }
        else
        {
            s += static_cast<char>( 0xF0u | ( cp >> 18 ) );
            s += static_cast<char>( 0x80u | ( ( cp >> 12 ) & 0x3Fu ) );
            s += static_cast<char>( 0x80u | ( ( cp >> 6 ) & 0x3Fu ) );
            s += static_cast<char>( 0x80u | ( cp & 0x3Fu ) );
        }
    }

    void Utf8PopBack( std::string& s )
    {
        while ( !s.empty() && IsContinuation( static_cast<unsigned char>( s.back() ) ) )
            s.pop_back();
        if ( !s.empty() )
            s.pop_back();
    }
} // namespace Desert::Text
