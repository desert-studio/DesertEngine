#include "TwinCheck.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>

namespace Desert::ShaderProtocolGen
{
    namespace
    {
        struct Token
        {
            std::string Text;
            uint32_t    Line = 0;
        };

        bool IsIdentStart( const char c )
        {
            return std::isalpha( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        bool IsIdentChar( const char c )
        {
            return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        bool IsIdentifier( const std::string_view text )
        {
            return !text.empty() && IsIdentStart( text.front() );
        }

        // Comments and preprocessor lines become spaces, newlines stay: a token keeps its source line.
        std::string StripNonCode( const std::string_view text )
        {
            std::string out( text );
            size_t      i           = 0;
            bool        lineStarted = false;
            while ( i < out.size() )
            {
                const char c = out[i];
                if ( c == '\n' )
                {
                    lineStarted = false;
                    ++i;
                    continue;
                }
                if ( c == '/' && i + 1 < out.size() && out[i + 1] == '/' )
                {
                    while ( i < out.size() && out[i] != '\n' )
                        out[i++] = ' ';
                    continue;
                }
                if ( c == '/' && i + 1 < out.size() && out[i + 1] == '*' )
                {
                    while ( i < out.size() && !( out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/' ) )
                    {
                        if ( out[i] != '\n' )
                            out[i] = ' ';
                        ++i;
                    }
                    for ( size_t k = 0; k < 2 && i < out.size(); ++k )
                        out[i++] = ' ';
                    continue;
                }
                if ( c == '#' && !lineStarted )
                {
                    // A directive runs to the end of the line, continued by a trailing backslash.
                    while ( i < out.size() )
                    {
                        if ( out[i] == '\n' )
                        {
                            size_t back = i;
                            while ( back > 0 && ( out[back - 1] == ' ' || out[back - 1] == '\r' ) )
                                --back;
                            if ( back == 0 || out[back - 1] != '\\' )
                                break;
                            ++i;
                            continue;
                        }
                        out[i++] = ' ';
                    }
                    continue;
                }
                if ( c != ' ' && c != '\t' && c != '\r' )
                    lineStarted = true;
                ++i;
            }
            return out;
        }

        std::vector<Token> Tokenize( const std::string_view code )
        {
            std::vector<Token> tokens;
            uint32_t           line = 1;
            size_t             i    = 0;
            while ( i < code.size() )
            {
                const char c = code[i];
                if ( c == '\n' )
                {
                    ++line;
                    ++i;
                    continue;
                }
                if ( std::isspace( static_cast<unsigned char>( c ) ) != 0 )
                {
                    ++i;
                    continue;
                }
                const size_t start = i;
                if ( IsIdentChar( c ) )
                {
                    while ( i < code.size() && ( IsIdentChar( code[i] ) || code[i] == '.' ) )
                        ++i;
                }
                else
                    ++i;
                tokens.push_back( { std::string( code.substr( start, i - start ) ), line } );
            }
            return tokens;
        }

        bool IsBlockSugar( const std::string_view text )
        {
            return text == "Uniform" || text == "ReadBuffer" || text == "WriteBuffer" || text == "Buffer";
        }

        // The name of the struct or block whose body opens at tokens[open], or empty when the brace opens anything
        // else (a function body, an initialiser list, a control statement).
        std::string_view CandidateName( const std::vector<Token>& tokens, const size_t open )
        {
            if ( open < 2 || !IsIdentifier( tokens[open - 1].Text ) )
                return {};
            const std::string_view before = tokens[open - 2].Text;
            if ( before == "struct" || before == "uniform" || before == "buffer" )
                return tokens[open - 1].Text;
            if ( before != ")" )
                return {};
            int depth = 0;
            for ( size_t k = open - 2;; --k )
            {
                if ( tokens[k].Text == ")" )
                    ++depth;
                else if ( tokens[k].Text == "(" && --depth == 0 )
                    return k > 0 && IsBlockSugar( tokens[k - 1].Text ) ? std::string_view( tokens[open - 1].Text )
                                                                       : std::string_view();
                if ( k == 0 )
                    return {};
            }
        }

        // Member names of the body opening at tokens[open]: each declarator's last identifier outside () and [].
        // @p close receives the index of the matching '}'.
        std::vector<std::string> MemberNames( const std::vector<Token>& tokens, const size_t open, size_t& close )
        {
            std::vector<std::string> members;
            std::string_view         last;
            int                      braces = 1;
            int                      nested = 0;
            size_t                   k      = open + 1;
            for ( ; k < tokens.size(); ++k )
            {
                const std::string_view t = tokens[k].Text;
                if ( t == "{" )
                {
                    ++braces;
                    continue;
                }
                if ( t == "}" )
                {
                    if ( --braces == 0 )
                        break;
                    continue;
                }
                if ( braces != 1 )
                    continue;
                if ( t == "(" || t == "[" )
                    ++nested;
                else if ( t == ")" || t == "]" )
                    nested = std::max( 0, nested - 1 );
                else if ( nested == 0 && ( t == "," || t == ";" ) )
                {
                    if ( !last.empty() )
                        members.emplace_back( last );
                    last = {};
                }
                else if ( nested == 0 && IsIdentifier( t ) )
                    last = t;
            }
            if ( !last.empty() )
                members.emplace_back( last );
            close = k;
            return members;
        }

        std::string Normalised( const std::string_view name )
        {
            std::string out;
            for ( const char c : name )
                out += static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
            if ( out.starts_with( "m_" ) )
                out.erase( 0, 2 );
            return out;
        }

        // LCS / max(length) of two ordered name sequences.
        double SequenceMatch( const std::vector<std::string>& a, const std::vector<std::string>& b )
        {
            std::vector<size_t> row( b.size() + 1, 0 );
            for ( const std::string& x : a )
            {
                size_t diagonal = 0;
                for ( size_t j = 1; j <= b.size(); ++j )
                {
                    const size_t up = row[j];
                    row[j]          = x == b[j - 1] ? diagonal + 1 : std::max( row[j], row[j - 1] );
                    diagonal        = up;
                }
            }
            return static_cast<double>( row.back() ) / static_cast<double>( std::max( a.size(), b.size() ) );
        }

        std::string Trimmed( std::string_view text )
        {
            while ( !text.empty() && std::isspace( static_cast<unsigned char>( text.front() ) ) != 0 )
                text.remove_prefix( 1 );
            while ( !text.empty() && std::isspace( static_cast<unsigned char>( text.back() ) ) != 0 )
                text.remove_suffix( 1 );
            return std::string( text );
        }
    } // namespace

    std::vector<std::string> ReadAllowList( const std::filesystem::path& file, std::vector<TwinAllow>& out )
    {
        std::vector<std::string> errors;
        std::ifstream            in( file, std::ios::binary );
        if ( !in )
            return { std::format( "{}: cannot read the twin allow-list", file.generic_string() ) };
        std::string line;
        uint32_t    number = 0;
        while ( std::getline( in, line ) )
        {
            ++number;
            const std::string text = Trimmed( line );
            if ( text.empty() || text.starts_with( '#' ) )
                continue;
            // `path:Name - reason` (an em dash or a hyphen between spaces).
            const size_t colon = text.find( ':' );
            size_t       dash  = text.find( " \xE2\x80\x94 " );
            size_t       skip  = 5;
            if ( dash == std::string::npos )
            {
                dash = text.find( " - " );
                skip = 3;
            }
            if ( colon == std::string::npos || dash == std::string::npos || dash < colon )
            {
                errors.push_back(
                     std::format( "{}:{}: expected `path:Name - reason`", file.generic_string(), number ) );
                continue;
            }
            TwinAllow allow;
            allow.Path   = Trimmed( std::string_view( text ).substr( 0, colon ) );
            allow.Name   = Trimmed( std::string_view( text ).substr( colon + 1, dash - colon - 1 ) );
            allow.Reason = Trimmed( std::string_view( text ).substr( dash + skip ) );
            allow.Line   = number;
            if ( allow.Path.empty() || !IsIdentifier( allow.Name ) || allow.Reason.empty() )
            {
                errors.push_back( std::format( "{}:{}: a path, a GLSL name and a reason are all required",
                                               file.generic_string(), number ) );
                continue;
            }
            out.push_back( std::move( allow ) );
        }
        return errors;
    }

    std::vector<TwinFinding> FindTwins( const std::filesystem::path&          shaderRoot,
                                        const std::span<const GeneratedShape> shapes,
                                        std::vector<TwinAllow>&               allowList )
    {
        std::vector<std::vector<std::string>> shapeMembers;
        for ( const GeneratedShape& shape : shapes )
        {
            std::vector<std::string> normalised;
            for ( const std::string& member : shape.Members )
                normalised.push_back( Normalised( member ) );
            shapeMembers.push_back( std::move( normalised ) );
        }

        std::vector<std::filesystem::path> files;
        std::error_code                    error;
        for ( auto it = std::filesystem::recursive_directory_iterator( shaderRoot, error );
              !error && it != std::filesystem::recursive_directory_iterator(); it.increment( error ) )
        {
            if ( it->is_directory() && it.depth() == 0 && it->path().filename() == "Generated" )
            {
                it.disable_recursion_pending();
                continue;
            }
            const std::filesystem::path extension = it->path().extension();
            if ( it->is_regular_file() &&
                 ( extension == ".glslh" || extension == ".shader" || extension == ".glsl" ) )
                files.push_back( it->path() );
        }
        std::ranges::sort( files );

        std::vector<TwinFinding> twins;
        for ( const std::filesystem::path& file : files )
        {
            std::ifstream     in( file, std::ios::binary );
            const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            const std::vector<Token> tokens   = Tokenize( StripNonCode( text ) );
            const std::string        relative = std::filesystem::relative( file, shaderRoot ).generic_string();

            for ( size_t open = 0; open < tokens.size(); ++open )
            {
                if ( tokens[open].Text != "{" )
                    continue;
                const std::string_view name = CandidateName( tokens, open );
                if ( name.empty() )
                    continue;
                size_t                         close   = open;
                const std::vector<std::string> members = MemberNames( tokens, open, close );
                std::vector<std::string>       normalised;
                for ( const std::string& member : members )
                    normalised.push_back( Normalised( member ) );

                const GeneratedShape* match    = nullptr;
                bool                  sameName = false;
                for ( const GeneratedShape& shape : shapes )
                    if ( shape.Name == name )
                    {
                        match    = &shape;
                        sameName = true;
                        break;
                    }
                for ( size_t s = 0; match == nullptr && s < shapes.size(); ++s )
                    if ( normalised.size() >= 2 && shapeMembers[s].size() >= 2 &&
                         SequenceMatch( normalised, shapeMembers[s] ) >= 0.8 )
                        match = &shapes[s];
                const uint32_t line = tokens[open - 1].Line; // the name, not a brace on its own line
                open                = close;
                if ( match == nullptr )
                    continue;

                const auto allowed = std::ranges::find_if( allowList, [&]( const TwinAllow& a )
                                                           { return a.Path == relative && a.Name == name; } );
                if ( allowed != allowList.end() )
                {
                    allowed->Used = true;
                    continue;
                }
                twins.push_back( { relative, line, std::string( name ), std::string( match->Path ),
                                   std::string( match->SourceHeader ), sameName } );
            }
        }
        return twins;
    }
} // namespace Desert::ShaderProtocolGen
