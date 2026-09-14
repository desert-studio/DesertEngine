#pragma once

// ONE ARGUMENT LIST MAY NOT BOTH CONSUME AN OBJECT AND READ IT — a census over the repository's source
// text, because no run of this binary can see the defect it is about.
//
// WHY A CENSUS AND NOT A TEST. C++ does not order the evaluation of function arguments. clang picks left
// to right, MSVC picks right to left, and both are conforming. So a line that reads `spec.Name` in one
// argument and does `std::move( spec )` in another computes one thing on this machine and a different
// thing on the other, FOREVER, and every run here agrees with itself. The only instrument that can see
// the shape is the source text. That conclusion is not new — Г24 reached it after `dev` went red on
// Windows only — and this file exists because the census Г24 wrote could not have found the next one.
//
// WHAT Г24's CENSUS COULD SEE, AND WHY THAT WAS NOT ENOUGH. `ShaderGraphDeterminism.
// AtMostOneEmittingCallPerFullExpression` opens ONE file by absolute path
// (Editor/.../NodeGraph/ShaderGraph.cpp) and counts calls to ONE function named in a constant
// (`EmitInput`). It is a census of the site that had already failed, not of the shape that failed there.
// Two independent narrownesses, either of which alone is fatal:
//
//   * its REACH is one file. `SceneRenderer.cpp` was never opened by it, and neither is anything else;
//   * its SUBJECT is a name. Even pointed at the whole tree it would have found nothing, because
//     `RegisterExternalPass` calls no function called `EmitInput` — the consuming side effect there is
//     spelled `std::move`, and the reading side effect is a plain member access.
//
// So the two censuses are complementary and both are kept. Г24's covers a side effect that has a NAME in
// one emitter; this one covers the side effect the language itself spells, everywhere.
//
// WHAT IS FLAGGED, EXACTLY. Inside one call's argument list, one top-level argument contains
// `std::move( X … )` / `std::forward( X … )` and a DIFFERENT top-level argument of the SAME list mentions
// `X`. The scan walks outward through every enclosing call, which is what catches the nesting that the
// live defect had — the move sat two calls deep:
//
//     TrackRenderSystem( ExternalSystemKey( spec.Name ),
//                        std::make_shared<ExternalPassSystem>( this, std::move( spec ) ) );
//
// WHAT IS DELIBERATELY NOT FLAGGED, AND WHY EACH IS SAFE BY THE STANDARD RATHER THAN BY TASTE:
//
//   * a braced initialiser list — `{ std::move( m.Body ), m.Properties }`. [dcl.init.list] sequences its
//     elements left to right. It is the one aggregate form that is not a coin toss;
//   * two mentions inside the SAME argument — `std::forward<decltype( args )>( args )`, or
//     `cond ? std::move( x ) : y`. One argument is one expression and its own operators sequence it;
//   * a subscript or a block, which are not argument lists at all.
//
// WHAT IT CANNOT SEE, said out loud because a census is only honest about its number if it names its
// blind spots: an alias (`auto& s = spec;` then `f( s.Name, std::move( spec ) )`), a side effect reached
// through a function rather than through `std::move`, and a move written as an explicit
// `static_cast<T&&>`. The first two are what Г24's name-based census exists for; the third has no
// instance in this tree and would be caught by review, because nobody writes it by accident.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Desert::Tests::ArgumentOrder
{
    namespace fs = std::filesystem;

    struct Finding
    {
        std::string File; // repository-relative, empty when the scan was handed a string
        int         Line = 0;
        std::string Object;     // the identifier that is both moved and read
        std::string Expression; // the offending call, whitespace-collapsed
    };

    struct ScanCounts
    {
        int Files     = 0;
        int MoveSites = 0; // every std::move/std::forward call the scan looked at
    };

    inline bool IsIdentChar( char c )
    {
        return ConsumerText::IsIdentChar( c );
    }

    /// The identifier that ENDS at @p end (exclusive), or empty.
    inline std::string IdentEndingAt( const std::string& code, std::size_t end )
    {
        std::size_t start = end;
        while ( start > 0 && ConsumerText::IsIdentChar( code[start - 1] ) )
            --start;
        return code.substr( start, end - start );
    }

    inline std::string ReadAll( const fs::path& path )
    {
        std::ifstream     in( path, std::ios::binary );
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    inline std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    /// Index of the bracket closing the one at @p open, or npos. Counts ( [ { as one family on purpose:
    /// the text is already stripped of comments and literals, so the only unbalanced bracket left would be
    /// a template angle, which this never starts on.
    inline std::size_t MatchBracket( const std::string& s, std::size_t open )
    {
        int depth = 0;
        for ( std::size_t i = open; i < s.size(); ++i )
        {
            const char c = s[i];
            if ( c == '(' || c == '[' || c == '{' )
                ++depth;
            else if ( c == ')' || c == ']' || c == '}' )
            {
                --depth;
                if ( depth == 0 )
                    return i;
            }
        }
        return std::string::npos;
    }

    /// The half-open spans of the top-level, comma-separated arguments of @p inner (the text between a
    /// call's parentheses).
    inline std::vector<std::pair<std::size_t, std::size_t>> TopLevelArguments( const std::string& inner )
    {
        std::vector<std::pair<std::size_t, std::size_t>> out;
        int                                              depth = 0;
        std::size_t                                      start = 0;
        for ( std::size_t i = 0; i < inner.size(); ++i )
        {
            const char c = inner[i];
            if ( c == '(' || c == '[' || c == '{' )
                ++depth;
            else if ( c == ')' || c == ']' || c == '}' )
                --depth;
            else if ( c == ',' && depth == 0 )
            {
                out.emplace_back( start, i );
                start = i + 1;
            }
        }
        out.emplace_back( start, inner.size() );
        return out;
    }

    /// Does @p word occur in @p s as a whole identifier?
    inline bool MentionsIdentifier( const std::string& s, const std::string& word )
    {
        return !ConsumerText::WordPositions( s, word ).empty();
    }

    /// The opening parenthesis of the `std::move`/`std::forward` call that begins at @p at, skipping an
    /// optional explicit template argument list. npos when what follows is not a call.
    inline std::size_t CallParenAfter( const std::string& code, std::size_t at, std::size_t nameLength )
    {
        std::size_t i = ConsumerText::SkipSpace( code, at + nameLength );
        if ( i < code.size() && code[i] == '<' )
        {
            int angle = 0;
            for ( ; i < code.size(); ++i )
            {
                if ( code[i] == '(' || code[i] == '[' )
                {
                    const std::size_t close = MatchBracket( code, i );
                    if ( close == std::string::npos )
                        return std::string::npos;
                    i = close;
                    continue;
                }
                if ( code[i] == '<' )
                    ++angle;
                else if ( code[i] == '>' )
                {
                    --angle;
                    if ( angle == 0 )
                    {
                        ++i;
                        break;
                    }
                }
            }
            i = ConsumerText::SkipSpace( code, i );
        }
        return ( i < code.size() && code[i] == '(' ) ? i : std::string::npos;
    }

    inline int LineOf( const std::string& code, std::size_t at )
    {
        return 1 + static_cast<int>(
                        std::count( code.begin(), code.begin() + static_cast<std::ptrdiff_t>( at ), '\n' ) );
    }

    inline std::string Collapse( const std::string& text )
    {
        std::istringstream words( text );
        std::string        out;
        std::string        word;
        while ( words >> word )
        {
            if ( !out.empty() )
                out += ' ';
            out += word;
        }
        return out;
    }

    /// The opening parenthesis of the CALL whose argument list directly contains position @p from, or
    /// npos when there is none before the statement begins. Returns npos — rather than skipping past —
    /// for a brace or a subscript, because [dcl.init.list] sequences a braced list left to right and a
    /// subscript is not an argument list: neither is a coin toss, so neither may be reported.
    inline std::size_t EnclosingCallParen( const std::string& code, std::size_t from )
    {
        int depth = 0;
        for ( std::size_t j = from; j-- > 0; )
        {
            const char c = code[j];
            if ( c == ')' || c == ']' || c == '}' )
            {
                ++depth;
                continue;
            }
            if ( c == '(' || c == '[' || c == '{' )
            {
                if ( depth > 0 )
                {
                    --depth;
                    continue;
                }
                if ( c != '(' )
                    return std::string::npos;

                std::size_t before = j;
                while ( before > 0 && ( code[before - 1] == ' ' || code[before - 1] == '\n' ||
                                        code[before - 1] == '\t' || code[before - 1] == '\r' ) )
                    --before;
                if ( before == 0 )
                    return std::string::npos;
                const char lead = code[before - 1];
                // A call, as opposed to a grouping parenthesis or an `if (` / `for (`: what precedes it
                // closes a name or a template argument list. `if`, `for`, `while` and `switch` are
                // identifier characters too, so they are excluded by name.
                if ( !( IsIdentChar( lead ) || lead == '>' ) )
                    return std::string::npos;
                const std::string word = IdentEndingAt( code, before );
                if ( word == "if" || word == "for" || word == "while" || word == "switch" || word == "catch" ||
                     word == "return" )
                    return std::string::npos;
                return j;
            }
            if ( c == ';' && depth == 0 )
                return std::string::npos;
        }
        return std::string::npos;
    }

    /// Every argument list in @p source that both consumes an object and reads it. @p source is raw text;
    /// comments and literals are blanked here with the one shared implementation.
    inline std::vector<Finding> ScanText( const std::string& source, const std::string& file,
                                          ScanCounts* counts = nullptr )
    {
        const std::string    code = ConsumerText::StripCommentsAndLiterals( source );
        std::vector<Finding> out;

        for ( const char* consumer : { "std::move", "std::forward" } )
        {
            const std::string name( consumer );
            for ( std::size_t at = code.find( name ); at != std::string::npos; at = code.find( name, at + 1 ) )
            {
                if ( at > 0 && ConsumerText::IsIdentChar( code[at - 1] ) )
                    continue;
                const std::size_t open = CallParenAfter( code, at, name.size() );
                if ( open == std::string::npos )
                    continue;
                const std::size_t close = MatchBracket( code, open );
                if ( close == std::string::npos )
                    continue;
                if ( counts != nullptr )
                    ++counts->MoveSites;

                // The object is the ROOT of the moved expression: `spec` of `std::move( spec.Inner )`.
                const std::size_t first  = ConsumerText::SkipSpace( code, open + 1 );
                const std::string object = ConsumerText::IdentAt( code, first );
                if ( object.empty() || object == "std" || object == "static_cast" || object == "const_cast" ||
                     object == "reinterpret_cast" )
                    continue;

                // WALK OUTWARD. `lo`..`hi` is the span already accounted for one level down, so a mention
                // inside it is the move's own text and not a sibling read. Every enclosing call is a
                // separate coin toss, and the live defect sat two levels up from its `std::move`.
                std::size_t lo = at;
                std::size_t hi = close;
                for ( std::size_t scan = at; scan > 0; )
                {
                    const std::size_t opener = EnclosingCallParen( code, scan );
                    if ( opener == std::string::npos )
                        break;
                    const std::size_t enclosing = MatchBracket( code, opener );
                    if ( enclosing == std::string::npos )
                        break;

                    const std::string inner = code.substr( opener + 1, enclosing - opener - 1 );
                    std::string       siblings;
                    for ( const auto& span : TopLevelArguments( inner ) )
                    {
                        const std::size_t absFrom = opener + 1 + span.first;
                        const std::size_t absTo   = opener + 1 + span.second;
                        if ( absFrom <= lo && hi <= absTo )
                            continue; // the argument the move itself lives in
                        siblings += inner.substr( span.first, span.second - span.first );
                        siblings += ' ';
                    }

                    if ( MentionsIdentifier( siblings, object ) )
                    {
                        Finding f;
                        f.File       = file;
                        f.Line       = LineOf( code, at );
                        f.Object     = object;
                        f.Expression = Collapse( code.substr( opener, enclosing - opener + 1 ) );
                        out.push_back( std::move( f ) );
                        break;
                    }

                    lo   = opener;
                    hi   = enclosing;
                    scan = opener;
                }
            }
        }
        return out;
    }

    /// The source trees this census covers. Every directory the build compiles our own C++ from; the list
    /// is asserted to exist rather than merely iterated, because a root that was renamed and silently
    /// stopped being scanned is the exact way Г24's census went blind.
    inline std::vector<std::string> SourceRoots()
    {
        return { "Desert/Desert/Source", "Desert/Common/Source", "Desert/Tests",
                 "Editor/Source",        "Runtime/Source",       "Tools" };
    }

    inline std::vector<Finding> ScanRepository( const std::string& root, ScanCounts& counts )
    {
        std::vector<Finding> out;
        for ( const std::string& tree : SourceRoots() )
        {
            std::error_code ec;
            for ( auto it = fs::recursive_directory_iterator( fs::path( root ) / tree, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                if ( p.string().find( "ThirdParty" ) != std::string::npos ||
                     p.string().find( "lightweightvk" ) != std::string::npos )
                    continue;
                const std::string ext = p.extension().string();
                if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".inl" )
                    continue;

                ++counts.Files;
                std::string relative = p.lexically_relative( fs::path( root ) ).generic_string();
                for ( Finding& f : ScanText( ReadAll( p ), relative, &counts ) )
                    out.push_back( std::move( f ) );
            }
        }
        return out;
    }
} // namespace Desert::Tests::ArgumentOrder
