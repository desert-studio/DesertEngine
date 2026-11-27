// BuildPool platform, Windows: text conversion, error text and command-line quoting.
#include "Win.hpp"

#include <vector>

namespace buildpool::win
{
    std::wstring Widen( std::string_view text )
    {
        if ( text.empty() )
        {
            return {};
        }
        const int size =
             MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
        std::wstring wide( static_cast<size_t>( size > 0 ? size : 0 ), L'\0' );
        if ( size > 0 )
        {
            MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), wide.data(), size );
        }
        return wide;
    }

    std::string Narrow( std::wstring_view text )
    {
        if ( text.empty() )
        {
            return {};
        }
        const int size = WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0,
                                              nullptr, nullptr );
        std::string narrow( static_cast<size_t>( size > 0 ? size : 0 ), '\0' );
        if ( size > 0 )
        {
            WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), narrow.data(), size,
                                 nullptr, nullptr );
        }
        return narrow;
    }

    std::string ErrorText( std::string_view what, DWORD code )
    {
        wchar_t*    buffer = nullptr;
        const DWORD length = FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                                  FORMAT_MESSAGE_IGNORE_INSERTS,
                                             nullptr, code, 0, reinterpret_cast<wchar_t*>( &buffer ), 0, nullptr );
        std::string text( what );
        text += ": ";
        if ( length > 0 && buffer != nullptr )
        {
            std::wstring_view message( buffer, length );
            while ( !message.empty() &&
                    ( message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ' ) )
            {
                message.remove_suffix( 1 );
            }
            text += Narrow( message );
        }
        if ( buffer != nullptr )
        {
            LocalFree( buffer );
        }
        text += " (error " + std::to_string( code ) + ")";
        return text;
    }

    namespace
    {
        // The rules CommandLineToArgvW and the MSVC CRT parse by: backslashes are literal unless they precede a
        // quote, where each pair becomes one backslash and an odd one escapes the quote.
        void AppendQuoted( std::wstring& line, const std::wstring& argument )
        {
            if ( !argument.empty() && argument.find_first_of( L" \t\n\v\"" ) == std::wstring::npos )
            {
                line += argument;
                return;
            }
            line.push_back( L'"' );
            size_t backslashes = 0;
            for ( const wchar_t c : argument )
            {
                if ( c == L'\\' )
                {
                    ++backslashes;
                    continue;
                }
                if ( c == L'"' )
                {
                    line.append( backslashes * 2 + 1, L'\\' );
                }
                else
                {
                    line.append( backslashes, L'\\' );
                }
                backslashes = 0;
                line.push_back( c );
            }
            line.append( backslashes * 2, L'\\' );
            line.push_back( L'"' );
        }
    } // namespace

    std::wstring QuoteCommandLine( const std::vector<std::string>& argv )
    {
        std::wstring line;
        for ( size_t i = 0; i < argv.size(); ++i )
        {
            if ( i > 0 )
            {
                line.push_back( L' ' );
            }
            AppendQuoted( line, Widen( argv[i] ) );
        }
        return line;
    }
} // namespace buildpool::win
