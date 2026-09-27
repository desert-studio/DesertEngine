// THE MACROS WINDOWS DEFINES AWAY, AND WHY THIS IS A TEST RATHER THAN A CONVENTION.
//
// windef.h still carries `#define far` and `#define near` — empty, for 16-bit memory models nobody has
// targeted since 1995. Any translation unit that reaches windows.h therefore turns `const float far = x;`
// into `const float = x;`, and MSVC reports "no variable declared before '='" followed by a cascade of
// C2059/C2065/C2440 that reads like a broken test rather than an eaten identifier.
//
// Three suites had it at once (CloudShadow, CloudLighting, CloudGeometry) plus LensFlare, all copied from
// one example comment in Units.hpp that used `far` as its variable name. None of it is visible on macOS or
// Linux. The cost of finding it the other way is a Windows CI job: ~35 minutes to first error, and it
// blocks every other merge behind it because a red Windows is no evidence for anything.
//
// So the check runs HERE, on every platform, in milliseconds, over the source text — the same technique
// SettingConsumers uses. It reads the tree rather than compiling it, which is the only way to assert
// something about a compiler nobody in this repository runs locally.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // Walk up until the marker is found: the test binary's working directory is not fixed.
    fs::path RepoRoot()
    {
        fs::path p = fs::current_path();
        for ( int i = 0; i < 8; ++i )
        {
            if ( fs::exists( p / "Desert" / "Common" ) && fs::exists( p / "Editor" ) )
                return p;
            p = p.parent_path();
        }
        return {};
    }

    std::vector<fs::path> SourceFiles( const fs::path& root )
    {
        std::vector<fs::path> out;
        std::error_code       ec;
        // Tools too: DesertCtl, DomeSheet and the crash reporter are built by the same Windows jobs.
        for ( const fs::path& sub :
              { fs::path( "Desert" ), fs::path( "Editor" ), fs::path( "Runtime" ), fs::path( "Tools" ) } )
        {
            for ( const auto& e : fs::recursive_directory_iterator( root / sub, ec ) )
            {
                if ( !e.is_regular_file() )
                    continue;
                const std::string ext = e.path().extension().string();
                if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" )
                    continue;
                // Generated code is not authored here, and ThirdParty is not ours to rename.
                const std::string s = e.path().generic_string();
                if ( s.find( "/Generated/" ) != std::string::npos ||
                     s.find( "/ThirdParty/" ) != std::string::npos )
                    continue;
                out.push_back( e.path() );
            }
        }
        return out;
    }

    // A DECLARATION of one of these names, not a mention. Comments and string literals are left alone on
    // purpose — prose says "the far side" legitimately, and rewriting words inside comments is its own
    // defect in this repository.
    //
    // THE TYPE IS NOT ENUMERATED, AND THAT IS THE WHOLE POINT OF THIS VERSION. The first one listed the
    // types it knew — const/float/int/auto/vec3/glm::… — and MISSED `std::vector<Scored> near;` in the
    // control channel, which Windows then rejected an hour downstream. That is the same mistake twice: my
    // original hand grep also required `const <one lowercase word>` and could not see `const glm::vec3 far`.
    // A guard that enumerates what it knows about only ever catches what somebody already thought of.
    //
    // So: ANY identifier-ish text, possibly template/namespace/pointer/reference decorated, immediately
    // followed by the reserved name and then by something a declaration ends with. The first token is
    // deliberately loose — `x = near;` is caught too, and a false positive here costs one rename while a
    // miss costs a Windows CI job.
    // A STRING LITERAL IS PROSE, on exactly the terms a trailing comment is, and leaving it in cost a
    // red sweep: `<< "the pose came back near, not equal"` matched as a declaration, because `back`
    // then a space then `near` then a comma is indistinguishable from `float* near,` to a regex that
    // cannot see quotes. The rule this file already states about comments — a guard that reports prose
    // teaches people to ignore it — does not stop at `//`.
    //
    // Naive on purpose: it does not model escapes or raw strings, because the question is only "is this
    // text the compiler will read as code". A literal containing `\"` collapses one quote early, which
    // can only ever REMOVE text from the scan; a census that misses one line is recoverable, a census
    // people switch off is not.
    std::string WithoutStringLiterals( const std::string& line )
    {
        std::string out;
        bool        inString = false;
        for ( std::size_t i = 0; i < line.size(); ++i )
        {
            if ( line[i] == '"' && ( i == 0 || line[i - 1] != '\\' ) )
            {
                inString = !inString;
                continue;
            }
            if ( !inString )
                out += line[i];
        }
        return out;
    }

    const std::regex& DeclarationOfReservedName()
    {
        static const std::regex re(
             R"([A-Za-z_][A-Za-z0-9_:<>,& \t]*[ \t*&>][ \t]*(far|near)[ \t]*(=|;|\)|,|\[))" );
        return re;
    }
} // namespace

// Every authored source file, one regex. Fails with the file, the line and the name.
TEST( ReservedIdentifiers, NoSourceDeclaresAVariableWindowsWillEat )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from " << fs::current_path();

    const auto files = SourceFiles( root );
    // Guards the guard: a wrong working directory would otherwise make this a green pass over zero files.
    ASSERT_GT( files.size(), 200u ) << "only " << files.size() << " source files found — the sweep is wrong";

    std::vector<std::string> offenders;
    for ( const fs::path& file : files )
    {
        std::ifstream in( file );
        std::string   line;
        int           number = 0;
        while ( std::getline( in, line ) )
        {
            ++number;
            const std::string trimmed = line.substr(
                 line.find_first_not_of( " \t" ) == std::string::npos ? 0 : line.find_first_not_of( " \t" ) );
            if ( trimmed.rfind( "//", 0 ) == 0 || trimmed.rfind( "*", 0 ) == 0 )
                continue;
            // Cheap reject first. std::regex over every line of every source file costs 70+ seconds; the
            // substring test throws away >99% of them and brings the whole suite under two. A guard nobody
            // wants to run is a guard that gets excluded from the sweep.
            if ( line.find( "far" ) == std::string::npos && line.find( "near" ) == std::string::npos )
                continue;

            // A TRAILING COMMENT IS PROSE AND MUST NOT BE SCANNED. `glm::vec4 GhostParams; // z = size
            // near, w = size far` is a lens-flare comment, and the loosened pattern above matched it — a
            // guard that reports prose teaches people to ignore it, and "rewriting words inside comments"
            // is already a filed defect here. Cut at `//` and match only what the compiler will see.
            const std::string code = WithoutStringLiterals( line.substr( 0, line.find( "//" ) ) );
            if ( std::regex_search( code, DeclarationOfReservedName() ) )
                offenders.push_back( fs::relative( file, root ).generic_string() + ":" + std::to_string( number ) +
                                     "  " + trimmed );
        }
    }

    std::string report;
    for ( const std::string& o : offenders )
        report += "\n  " + o;

    EXPECT_TRUE( offenders.empty() )
         << offenders.size()
         << " declaration(s) use a name windef.h #defines away. MSVC will read the type with no variable "
            "after it and report a syntax error nowhere near the cause. Rename them (farKm, nearPlane, "
            "reach...):"
         << report;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// THE SEPARATOR, WHICH IS THE SAME DEFECT AS `far` WEARING DIFFERENT CLOTHES.
//
// `std::filesystem::path::string()` returns the NATIVE spelling: forward slashes on macOS and Linux,
// **backslashes on Windows**. So a census that skips part of the tree with
// `entry.path().string().find( "/build/" )` silently skips NOTHING on Windows — the substring cannot
// occur — and the walk then reads whatever lives there.
//
// Measured 2026-09-16, and it cost a red `dev` on the primary target. `AnimationClipCorpus` walks the
// repository asserting every `.anim` is at the current generation, and excluded `/build/` and
// `/.claude/` this way. On Windows neither exclusion matched, the walk reached
// `build/TestScratch/AnimationClipFormat/desert_anim_clip_write/_Walk.anim` — a scratch file ANOTHER
// SUITE had just written — and failed on it at generation 0. The verdict therefore depended on which
// suites had run before it, which is not a property of the repository at all.
//
// It was FIVE call sites across four censuses, all written the same way, none of them wrong on the
// machine they were written on. That is why this is an assertion and not five fixes: `generic_string()`
// is forward-slashed on every platform and is the only spelling a path filter may use.
TEST( ReservedIdentifiers, NoPathFilterUsesTheNativeSpelling )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const auto files = SourceFiles( root );
    ASSERT_FALSE( files.empty() ) << "no sources walked — this census examined nothing";

    // `.string()` followed by a search for a slash-delimited fragment. The fragment is what makes it a
    // PATH FILTER rather than any other use of `.string()`, which is why the pattern requires the quote
    // and the leading slash rather than flagging every `.string()` in the tree.
    const std::regex nativeFilter( R"(\.string\(\)\s*\.find\(\s*"/)" );

    std::vector<std::string> offenders;
    for ( const fs::path& file : files )
    {
        std::ifstream in( file );
        std::string   line;
        int           number = 0;
        while ( std::getline( in, line ) )
        {
            ++number;
            // A COMMENT IS NOT CODE, and a census that reddens on prose is a census someone will
            // silence. This very file explains the defect by quoting it, and the first run flagged
            // that explanation — so the rule is stated once here instead of being worked around by
            // rewording every paragraph that has to name the thing it forbids.
            const std::size_t firstGlyph = line.find_first_not_of( " \t" );
            if ( firstGlyph != std::string::npos &&
                 ( line.compare( firstGlyph, 2, "//" ) == 0 || line.compare( firstGlyph, 1, "*" ) == 0 ) )
            {
                continue;
            }
            // Cheap reject first: std::regex over every line costs seconds per census in a debug build.
            if ( line.find( ".string()" ) != std::string::npos && std::regex_search( line, nativeFilter ) )
            {
                offenders.push_back( fs::relative( file, root ).generic_string() + ":" +
                                     std::to_string( number ) );
            }
        }
    }

    EXPECT_TRUE( offenders.empty() )
         << "A path filter is searching the NATIVE spelling for a forward-slashed fragment, so it "
            "matches nothing on Windows and the exclusion silently does not happen. Use "
            "`generic_string()`, which is forward-slashed on every platform.\n"
         << [&offenders]
    {
        std::string all;
        for ( const std::string& o : offenders )
        {
            all += "  " + o + "\n";
        }
        return all;
    }();
}

TEST( ReservedIdentifiers, EveryPosixProcessPipeHasItsWindowsSpellingBesideIt )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const auto files = SourceFiles( root );
    ASSERT_FALSE( files.empty() ) << "no sources walked — this census examined nothing";

    // THIS REACHED `dev` TWICE. The second time it was `Common/Content/ContentScan.cpp`, the file that
    // asks git what it tracks — so the gate written to stop machine-local answers could not COMPILE on
    // the platform the game ships for, while every macOS suite stayed green. MSVC spells these
    // `_popen`/`_pclose` and declares no unprefixed alias.
    //
    // THE ASSERTION IS A RELATION, NOT AN ABSENCE, and the first draft got that wrong: forbidding
    // `popen` outright reddened on all four files that had ALREADY been split correctly, the remedy
    // included. A gate that fires on its own fix is a gate someone deletes. What must hold is that a
    // file naming the POSIX spelling also names the Windows one — i.e. somebody thought about the
    // platform split — which is checkable per file and cannot be satisfied by deleting the guard.
    //
    // It does not try to parse `#if` nesting. A file that mentions both spellings but wires them to
    // the wrong branches is a defect this census cannot see; the COMPILER sees that one, which is
    // exactly the division of labour a census should keep.
    const std::regex posixPipe( R"((^|[^A-Za-z0-9_])(popen|pclose)\s*\()" );

    std::vector<std::string> offenders;
    for ( const fs::path& file : files )
    {
        std::ifstream in( file );
        std::string   line;
        int           firstUse           = 0;
        int           number             = 0;
        bool          hasWindowsSpelling = false;
        while ( std::getline( in, line ) )
        {
            ++number;
            // A COMMENT IS NOT CODE — same rule, and same reason, as the census above: this very file
            // has to name what it forbids.
            const std::size_t firstGlyph = line.find_first_not_of( " \t" );
            if ( firstGlyph != std::string::npos &&
                 ( line.compare( firstGlyph, 2, "//" ) == 0 || line.compare( firstGlyph, 1, "*" ) == 0 ) )
            {
                continue;
            }
            if ( line.find( "_popen" ) != std::string::npos || line.find( "_pclose" ) != std::string::npos )
            {
                hasWindowsSpelling = true;
            }
            if ( firstUse == 0 &&
                 ( line.find( "popen" ) != std::string::npos || line.find( "pclose" ) != std::string::npos ) &&
                 std::regex_search( line, posixPipe ) )
            {
                firstUse = number;
            }
        }
        if ( firstUse != 0 && !hasWindowsSpelling )
        {
            offenders.push_back( fs::relative( file, root ).generic_string() + ":" + std::to_string( firstUse ) );
        }
    }

    EXPECT_TRUE( offenders.empty() )
         << "`popen`/`pclose` are POSIX and MSVC does not declare them, so this is a Windows BUILD "
            "failure that no macOS sweep can see. Guard with `#if defined( DESERT_PLATFORM_WINDOWS )` "
            "and call `_popen`/`_pclose` there — and remember cmd does not honour single quotes, so "
            "the argument quoting needs the same split or the command silently returns nothing.\n"
         << [&offenders]
    {
        std::string all;
        for ( const std::string& o : offenders )
        {
            all += "  " + o + "\n";
        }
        return all;
    }();
}

// ---------------------------------------------------------------------------------------------------------
// THE REST OF THE MSVC-ONLY CLASSES, ADDED 2026-09-27 (CIW5) AFTER WINDOWS WAS RED FOR TWO DAYS.
//
// Every census below reads the text the compiler will see: comments and string literals are blanked
// first (newlines kept, so line numbers stay true), because this file and many others have to NAME
// what they forbid. Each one asserts a RELATION where the defect has a Windows counterpart — "the
// file that uses the POSIX spelling has thought about Windows" — and an absence only where no
// counterpart exists at all.
// ---------------------------------------------------------------------------------------------------------
namespace
{
    std::string ReadAll( const fs::path& file )
    {
        std::ifstream in( file, std::ios::binary );
        return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    }

    bool IsWordChar( char c )
    {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
    }

    // Comments, string literals (raw ones included) and character literals become spaces; newlines stay.
    std::string CodeOnly( const std::string& text )
    {
        std::string out   = text;
        auto        blank = [&out]( std::size_t from, std::size_t to )
        {
            for ( std::size_t k = from; k < to && k < out.size(); ++k )
            {
                if ( out[k] != '\n' )
                    out[k] = ' ';
            }
        };
        std::size_t i = 0;
        while ( i < text.size() )
        {
            if ( text.compare( i, 2, "//" ) == 0 )
            {
                const std::size_t end = text.find( '\n', i );
                const std::size_t to  = end == std::string::npos ? text.size() : end;
                blank( i, to );
                i = to;
            }
            else if ( text.compare( i, 2, "/*" ) == 0 )
            {
                const std::size_t end = text.find( "*/", i + 2 );
                const std::size_t to  = end == std::string::npos ? text.size() : end + 2;
                blank( i, to );
                i = to;
            }
            else if ( text.compare( i, 2, "R\"" ) == 0 && ( i == 0 || !IsWordChar( text[i - 1] ) ) )
            {
                const std::size_t open = text.find( '(', i + 2 );
                if ( open == std::string::npos )
                    break;
                const std::string close = ")" + text.substr( i + 2, open - i - 2 ) + "\"";
                const std::size_t end   = text.find( close, open );
                const std::size_t to    = end == std::string::npos ? text.size() : end + close.size();
                blank( i + 1, to );
                i = to;
            }
            else if ( text[i] == '"' || ( text[i] == '\'' && ( i == 0 || !IsWordChar( text[i - 1] ) ) ) )
            {
                // A quote preceded by a word character is a digit separator (1'000), not a literal.
                const char  quote = text[i];
                std::size_t j     = i + 1;
                while ( j < text.size() && text[j] != quote && text[j] != '\n' )
                    j += text[j] == '\\' ? 2 : 1;
                blank( i + 1, j );
                i = j + 1;
            }
            else
            {
                ++i;
            }
        }
        return out;
    }

    int LineOf( const std::string& text, std::size_t offset )
    {
        return 1 + static_cast<int>(
                        std::count( text.begin(), text.begin() + static_cast<std::ptrdiff_t>( offset ), '\n' ) );
    }

    struct Source
    {
        std::string Name; // repository-relative, forward-slashed
        std::string Text; // as on disk
        std::string Code; // CodeOnly( Text )
    };

    const std::vector<Source>& Sources()
    {
        static const std::vector<Source> all = []
        {
            std::vector<Source> out;
            const fs::path      root = RepoRoot();
            for ( const fs::path& file : SourceFiles( root ) )
            {
                Source s;
                s.Name = fs::relative( file, root ).generic_string();
                s.Text = ReadAll( file );
                s.Code = CodeOnly( s.Text );
                out.push_back( std::move( s ) );
            }
            return out;
        }();
        return all;
    }

    std::string Listed( const std::vector<std::string>& offenders )
    {
        std::string all;
        for ( const std::string& o : offenders )
            all += "\n  " + o;
        return all;
    }

    // A platform conditional: some `#if` on the stack names a platform. The `#else` of `#if _WIN32` counts
    // — whoever wrote it split the platforms, which is the relation these censuses assert.
    const std::regex& PlatformCondition()
    {
        static const std::regex re(
             R"(_WIN32|_WIN64|WINDOWS|_MSC_VER|__APPLE__|__MACH__|__linux__|__unix__|POSIX|PLATFORM_MACOS|PLATFORM_LINUX)" );
        return re;
    }

    // For every line of `code`, whether it sits inside a platform conditional.
    std::vector<bool> PlatformGuardedLines( const std::string& code )
    {
        std::vector<bool> guarded;
        // Per open `#if`: whether it or anything enclosing it names a platform.
        std::vector<bool>  platformDepth;
        std::istringstream in( code );
        std::string        line;
        const std::regex   directive( R"(^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*))" );
        while ( std::getline( in, line ) )
        {
            std::smatch m;
            if ( line.find( '#' ) != std::string::npos && std::regex_search( line, m, directive ) )
            {
                const std::string kind     = m[1];
                const bool        platform = std::regex_search( std::string( m[2] ), PlatformCondition() );
                const bool        outer    = !platformDepth.empty() && platformDepth.back();
                if ( kind == "if" || kind == "ifdef" || kind == "ifndef" )
                    platformDepth.push_back( outer || platform );
                else if ( kind == "elif" && !platformDepth.empty() )
                    platformDepth.back() = platformDepth.back() || platform;
                else if ( kind == "endif" && !platformDepth.empty() )
                    platformDepth.pop_back();
            }
            guarded.push_back( !platformDepth.empty() && platformDepth.back() );
        }
        return guarded;
    }

    std::vector<std::string> EachMatch( const Source& s, const std::regex& re )
    {
        std::vector<std::string> out;
        for ( auto it = std::sregex_iterator( s.Code.begin(), s.Code.end(), re ); it != std::sregex_iterator();
              ++it )
        {
            out.push_back( s.Name + ":" +
                           std::to_string( LineOf( s.Code, static_cast<std::size_t>( it->position() ) ) ) + "  " +
                           it->str() );
        }
        return out;
    }
} // namespace

// Found on Windows CI 2026-09-26 (B19 gate, run 36277577121) in TWO suites at once: MSVC's
// preprocessor mis-lexes a raw string literal followed by an escaped quote when both sit inside ONE
// macro argument, and reports C2017 "illegal escape sequence" at a line that is valid C++. Clang
// and GCC accept it, so no macOS sweep can see it. The remedy is to name the expectation first
// (`const std::string expected = ...;`) and pass the variable to the macro.
TEST( ReservedIdentifiers, NoMacroArgumentMixesARawStringWithAnEscapedQuote )
{
    const std::regex         macroCall( R"(\b[A-Z][A-Z0-9_]{2,}\s*\()" );
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        const std::string& t = s.Text;
        // Cheap reject first: only a file holding both a raw string and an escaped quote can offend.
        if ( t.find( "R\"(" ) == std::string::npos || t.find( "\\\"" ) == std::string::npos )
            continue;
        for ( auto it = std::sregex_iterator( s.Code.begin(), s.Code.end(), macroCall );
              it != std::sregex_iterator(); ++it )
        {
            // Walk the ORIGINAL text to the matching parenthesis, stepping over literals so a ')' inside
            // one does not end the argument list.
            const std::size_t start       = static_cast<std::size_t>( it->position() );
            std::size_t       j           = start + static_cast<std::size_t>( it->length() );
            int               depth       = 1;
            bool              sawRaw      = false;
            bool              sawEscQuote = false;
            while ( j < t.size() && depth > 0 )
            {
                if ( t.compare( j, 3, "R\"(" ) == 0 )
                {
                    const std::size_t end = t.find( ")\"", j + 3 );
                    sawRaw                = true;
                    j                     = end == std::string::npos ? t.size() : end + 2;
                    continue;
                }
                if ( t[j] == '"' )
                {
                    ++j;
                    while ( j < t.size() && t[j] != '"' && t[j] != '\n' )
                    {
                        if ( t[j] == '\\' && j + 1 < t.size() && t[j + 1] == '"' )
                            sawEscQuote = true;
                        j += t[j] == '\\' ? 2 : 1;
                    }
                    ++j;
                    continue;
                }
                if ( t[j] == '\'' && !IsWordChar( t[j - 1] ) )
                {
                    ++j;
                    while ( j < t.size() && t[j] != '\'' && t[j] != '\n' )
                        j += t[j] == '\\' ? 2 : 1;
                    ++j;
                    continue;
                }
                depth += t[j] == '(' ? 1 : t[j] == ')' ? -1 : 0;
                ++j;
            }
            if ( sawRaw && sawEscQuote )
                offenders.push_back( s.Name + ":" + std::to_string( LineOf( t, start ) ) );
        }
    }
    EXPECT_TRUE( offenders.empty() )
         << "A macro argument holds both a raw string literal and an escaped quote: MSVC reports C2017 "
            "there. Name the expected text in a variable first and pass the variable to the macro:"
         << Listed( offenders );
}

// `path::native()` is `std::wstring` on Windows. Code that treats it as `std::string` compiles on
// macOS and fails on MSVC; code that genuinely needs the native form has to say which platform it is
// on. So a file using it must either be platform-split or spell the wide type out.
TEST( ReservedIdentifiers, EveryNativePathSpellingNamesItsWideForm )
{
    const std::regex         wideAware( R"(wstring|wchar_t|string_type|_WIN32|DESERT_PLATFORM_WINDOWS)" );
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        const std::size_t at = s.Code.find( ".native()" );
        if ( at != std::string::npos && !std::regex_search( s.Code, wideAware ) )
            offenders.push_back( s.Name + ":" + std::to_string( LineOf( s.Code, at ) ) );
    }
    EXPECT_TRUE( offenders.empty() )
         << "`path::native()` is a WIDE string on Windows. Use `string()`/`generic_string()`, or split the "
            "platforms and name the wide type:"
         << Listed( offenders );
}

// `M_PI` and its family are not standard; MSVC declares them only after `_USE_MATH_DEFINES`, which this
// workspace does not set (and a per-file define loses to the precompiled header). There is no Windows
// spelling to pair it with, so this one IS an absence: use `std::numbers` or `glm::pi`.
TEST( ReservedIdentifiers, NoSourceUsesThePosixMathConstants )
{
    const std::regex posixConstant(
         R"(\bM_(PI|PI_2|PI_4|1_PI|2_PI|2_SQRTPI|E|LOG2E|LOG10E|LN2|LN10|SQRT2|SQRT1_2)\b)" );
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        if ( s.Code.find( "M_" ) == std::string::npos )
            continue;
        for ( const std::string& o : EachMatch( s, posixConstant ) )
            offenders.push_back( o );
    }
    EXPECT_TRUE( offenders.empty() ) << "POSIX math constants do not exist on MSVC here; use std::numbers:"
                                     << Listed( offenders );
}

// `T name[] = { /* nothing yet */ };` is a zero-length array: clang accepts it as an extension, MSVC
// rejects it (C2466). The comment is what hides it from a reader, which is why the scan runs on code
// with comments blanked. `std::array<T, 0>` is the legal spelling.
TEST( ReservedIdentifiers, NoArrayIsInitialisedEmpty )
{
    const std::regex         emptyArray( R"(\[\s*\]\s*=\s*\{\s*\})" );
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        // clang-format keeps `]` and `=` on one line, so this is a safe cheap reject.
        if ( s.Code.find( "] =" ) == std::string::npos && s.Code.find( "]=" ) == std::string::npos )
            continue;
        for ( const std::string& o : EachMatch( s, emptyArray ) )
            offenders.push_back( o );
    }
    EXPECT_TRUE( offenders.empty() ) << "Zero-length C array (MSVC C2466); use std::array<T, 0>:"
                                     << Listed( offenders );
}

// `long` is 32 bits on Windows (LLP64). `1L << 40` is therefore undefined there and silently a
// different number than on macOS. Spell 64-bit shifts with `1ull` or `std::uint64_t{ 1 }`.
TEST( ReservedIdentifiers, NoShiftIsSpelledInLong )
{
    const std::regex         longShift( R"(\b1[uU]?[lL]\s*<<)" );
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        if ( s.Code.find( "<<" ) == std::string::npos ||
             ( s.Code.find( "1L" ) == std::string::npos && s.Code.find( "1l" ) == std::string::npos &&
               s.Code.find( "1UL" ) == std::string::npos && s.Code.find( "1ul" ) == std::string::npos &&
               s.Code.find( "1uL" ) == std::string::npos && s.Code.find( "1Ul" ) == std::string::npos ) )
            continue;
        for ( const std::string& o : EachMatch( s, longShift ) )
            offenders.push_back( o );
    }
    EXPECT_TRUE( offenders.empty() ) << "`1L <<` is a 32-bit shift on Windows; use 1ull / std::uint64_t:"
                                     << Listed( offenders );
}

// POSIX-only headers and calls. The popen census above asserts one pair by name; this is the same
// relation for the rest of the family, stated as "the use sits inside a platform conditional" — which
// is what every correct instance in this tree already does (LocalSocket, CrashHandler, EntryPoint,
// Photogrammetry). It cannot see a conditional wired to the wrong branch; the compiler sees that.
TEST( ReservedIdentifiers, EveryPosixOnlyUseSitsInsideAPlatformConditional )
{
    const std::regex posixHeader(
         R"(^\s*#\s*include\s*<(unistd\.h|dlfcn\.h|pthread\.h|dirent\.h|spawn\.h|execinfo\.h|cxxabi\.h|libgen\.h|glob\.h|fnmatch\.h|termios\.h|poll\.h|pwd\.h|strings\.h|ucontext\.h|semaphore\.h|netdb\.h|mach/[^>]+|netinet/[^>]+|arpa/[^>]+|sys/(socket|un|wait|mman|time|resource|ioctl|select|utsname|ucontext|sysctl|param|file|uio|event)\.h)>)" );
    const std::regex posixCall(
         R"((^|[^A-Za-z0-9_.>:]|[^A-Za-z0-9_]::)(setenv|unsetenv|strcasecmp|strncasecmp|realpath|mkstemp|mkdtemp|usleep|nanosleep|fork|waitpid|execvp|execv|posix_spawn|posix_spawnp|dlopen|dlsym|dlclose|localtime_r|gmtime_r|strtok_r)\s*\()" );
    // Not listed: `getpid`, which MSVC's <process.h> still declares under its POSIX name, and `kill`,
    // which is a member name far more often than the syscall (Assimp::DefaultLogger::kill).
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        // Compiled only on its own platform (BuildScripts/Platform*.lua).
        if ( s.Name.find( "/Platform/MacOS/" ) != std::string::npos ||
             s.Name.find( "/Platform/Linux/" ) != std::string::npos )
            continue;
        static const char* const kNames[] = { "#include", "setenv",  "strcasecmp",  "strncasecmp", "realpath",
                                              "mkstemp",  "mkdtemp", "usleep",      "nanosleep",   "fork",
                                              "waitpid",  "execv",   "posix_spawn", "dlopen",      "dlsym",
                                              "dlclose",  "_r(",     "_r (" };
        std::vector<bool>        guarded;
        std::istringstream       in( s.Code );
        std::string              line;
        std::size_t              number = 0;
        while ( std::getline( in, line ) )
        {
            ++number;
            // Cheap reject first, and the conditional stack only for a file that has a candidate at all.
            bool candidate = false;
            for ( const char* name : kNames )
                candidate = candidate || line.find( name ) != std::string::npos;
            if ( !candidate )
                continue;
            std::smatch m;
            const bool  posix =
                 std::regex_search( line, m, posixHeader ) || std::regex_search( line, m, posixCall );
            if ( posix && guarded.empty() )
                guarded = PlatformGuardedLines( s.Code );
            if ( posix && !guarded[number - 1] )
                offenders.push_back( s.Name + ":" + std::to_string( number ) + "  " + m.str() );
        }
    }
    EXPECT_TRUE( offenders.empty() )
         << "A POSIX-only header or call outside any platform conditional: it does not exist on MSVC. "
            "Put it under `#if !defined( _WIN32 )` (or DESERT_PLATFORM_*) and give Windows its own spelling "
            "(_putenv_s, _stricmp, CreateProcess, LoadLibrary, localtime_s, ...):"
         << Listed( offenders );
}

// UNITY BUILD: ONE TRANSLATION UNIT, MANY FILES. Windows CI compiles Common, Desert and Editor as MSBuild
// unity files, 12 sources each (BuildScripts/UnityBuild.lua). Two sources that each define the same
// internal-linkage name — an anonymous-namespace helper or a `static` constant — compile alone and
// collide the day they land in one group, which depends on nothing but the order of the file list. On
// 2026-09-26 one such pair (`SubjectTitle`) broke the Windows build while every macOS job was green.
//
// The census cannot know the grouping, so it asserts the relation that makes grouping irrelevant: a
// name defined with internal linkage in two unity sources of one project is either (a) in a file the
// opt-out list keeps out of unity files, or (b) a row of the register below — one named row per pair
// that exists today, each still true (a renamed helper must take its row with it). A NEW duplicate
// fails here, in milliseconds, before it is ever grouped with its twin.
//
// Scope is by enclosing named namespace: `Desert::Assets::Reader` and `Desert::Editor::Reader` do not
// collide. Overloads with different parameters are legal and still reported — the census reads names,
// not signatures — and the remedy for one is a register row, not a rename.
namespace
{
    // "project name", where name is qualified by its enclosing named namespaces.
    constexpr const char* kUnityDuplicateRegister[] = {
         "Desert Desert::Assets::DecodeImportInfo",   // MeshSourceAsset.cpp, TextureSourceAsset.cpp
         "Desert Desert::Assets::EncodeImportInfo",   // MeshSourceAsset.cpp, TextureSourceAsset.cpp
         "Desert Desert::Assets::PutU32",             // MeshDerivedData.cpp, TextureSourceAsset.cpp
         "Desert Desert::Assets::Reader",             // MeshSourceAsset.cpp, TextureSourceAsset.cpp
         "Desert Desert::Assets::kImportInfoVersion", // MeshSourceAsset.cpp, TextureSourceAsset.cpp
         "Desert Desert::Assets::kKnown",             // MeshSourceAsset.cpp, TextureSourceAsset.cpp
         "Desert Desert::Assets::s_Builder",          // MeshDerivedData.cpp, TextureSourceAsset.cpp
         "Desert Desert::Assets::s_BuilderMutex",     // MeshDerivedData.cpp, TextureSourceAsset.cpp
         "Desert Desert::ECS::Landscape",             // LandscapeCollision.cpp, LandscapeECSSystem.cpp
         "Desert Desert::Geometry::CopyLayer",      // EditMeshTopologyOperations.cpp, EditMeshXformOperations.cpp
         "Desert Desert::Geometry::Outcome",        // EditMeshModelOperations.cpp, EditMeshTopologyOperations.cpp
         "Desert Desert::Geometry::WriteOverlay",   // DynamicMeshSerialization.cpp, EditMeshSerialization.cpp
         "Editor Desert::Editor::Lower",            // AssetReferencesScan.cpp, FuzzyMatch.cpp
         "Editor Desert::Editor::RelativeToAssets", // EditorPreferences.cpp, CloudTypePanel.cpp
         "Editor Desert::Editor::SanitizeName",     // GamePackager.cpp, CollectionsPanel.cpp
         "Editor Desert::Editor::ToU32",            // SkyAtmosphereComponent.cpp, WorldPartitionPanel.cpp
    };

    std::string Trimmed( const std::string& s )
    {
        const std::size_t b = s.find_first_not_of( " \t\r\n" );
        const std::size_t e = s.find_last_not_of( " \t\r\n" );
        return b == std::string::npos ? std::string() : s.substr( b, e - b + 1 );
    }

    // The name a namespace-scope declaration or definition head introduces, or "" when it introduces none
    // this census can see.
    std::string DeclaredName( std::string head )
    {
        // Preprocessor lines inside the head are not part of the declaration.
        std::string        kept;
        std::istringstream lines( head );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            if ( Trimmed( line ).rfind( "#", 0 ) != 0 )
                kept += line + "\n";
        }
        head = Trimmed( kept );
        static const std::regex skip(
             R"(^(using\s+namespace\b|using\s+[\w:]+::\w+$|friend\b|return\b|extern\b))" );
        static const std::regex type(
             R"(^(template\s*<[^>]*>\s*)?(struct|class|enum\s+class|enum|union)\s+(\w+))" );
        static const std::regex alias( R"(^using\s+(\w+)\s*=)" );
        static const std::regex lastWord( R"((\w+)\s*$)" );
        static const std::regex variable( R"(^[^=]*?\b(\w+)\s*(\[[^\]]*\])?\s*(=|$))" );
        std::smatch             m;
        if ( head.empty() || std::regex_search( head, skip ) )
            return {};
        if ( std::regex_search( head, m, type ) )
            return m[3];
        if ( std::regex_search( head, m, alias ) )
            return m[1];
        const std::size_t paren = head.find( '(' );
        if ( paren != std::string::npos && head.substr( 0, paren ).find( '=' ) == std::string::npos )
        {
            const std::string before = head.substr( 0, paren );
            if ( !std::regex_search( before, m, lastWord ) )
                return {};
            const std::string name = m[1];
            for ( const char* keyword : { "if", "for", "while", "switch", "sizeof", "decltype", "static_assert" } )
            {
                if ( name == keyword )
                    return {};
            }
            return name;
        }
        const std::string upToParen = paren == std::string::npos ? head : head.substr( 0, paren );
        if ( std::regex_search( upToParen, m, variable ) )
            return m[1];
        return {};
    }

    // Internal-linkage names a source defines at namespace scope, qualified by the named namespaces
    // around them: everything inside an anonymous namespace, and `static` declarations outside classes.
    std::set<std::string> InternalNames( const std::string& code )
    {
        enum class Scope
        {
            Anonymous,
            Named,
            Other
        };
        std::set<std::string>    names;
        std::vector<Scope>       stack;
        std::vector<std::string> path;
        std::string              pending;
        static const std::regex  anonymous( R"(\bnamespace\s*$)" );
        static const std::regex  named( R"(\bnamespace\s+([\w:]+)\s*$)" );
        auto inside = [&stack]( Scope s ) { return std::find( stack.begin(), stack.end(), s ) != stack.end(); };
        auto record = [&]( const std::string& head )
        {
            const std::string name = DeclaredName( head );
            if ( name.empty() )
                return;
            std::string qualified;
            for ( const std::string& p : path )
                qualified += p + "::";
            names.insert( qualified + name );
        };
        for ( const char c : code )
        {
            if ( c == '{' )
            {
                std::smatch       m;
                const std::string head = Trimmed( pending );
                if ( std::regex_search( head, anonymous ) )
                    stack.push_back( Scope::Anonymous );
                else if ( std::regex_search( head, m, named ) )
                {
                    stack.push_back( Scope::Named );
                    path.push_back( m[1] );
                }
                else
                {
                    if ( !inside( Scope::Other ) &&
                         ( inside( Scope::Anonymous ) || head.rfind( "static ", 0 ) == 0 ) )
                        record( head );
                    stack.push_back( Scope::Other );
                }
                pending.clear();
            }
            else if ( c == '}' )
            {
                if ( !stack.empty() )
                {
                    if ( stack.back() == Scope::Named && !path.empty() )
                        path.pop_back();
                    stack.pop_back();
                }
                pending.clear();
            }
            else if ( c == ';' )
            {
                const std::string head = Trimmed( pending );
                if ( !inside( Scope::Other ) && ( inside( Scope::Anonymous ) || head.rfind( "static ", 0 ) == 0 ) )
                    record( head );
                pending.clear();
            }
            else
            {
                pending += c;
            }
        }
        return names;
    }

    // BuildScripts/UnityBuild.lua's opt-out patterns, with the leading `**` dropped: a source matching one
    // is its own translation unit on Windows too, so it cannot collide.
    std::vector<std::string> UnityOptOuts()
    {
        std::vector<std::string> out;
        const std::string        lua = ReadAll( RepoRoot() / "BuildScripts" / "UnityBuild.lua" );
        const std::regex         pattern( R"re(pattern\s*=\s*"\*\*([^"]+)")re" );
        for ( auto it = std::sregex_iterator( lua.begin(), lua.end(), pattern ); it != std::sregex_iterator();
              ++it )
            out.push_back( ( *it )[1] );
        return out;
    }
} // namespace

TEST( ReservedIdentifiers, NoInternalNameIsDefinedTwiceInOneUnityProject )
{
    const std::vector<std::string> optOuts = UnityOptOuts();
    ASSERT_GT( optOuts.size(), 3u ) << "BuildScripts/UnityBuild.lua's opt-out list was not found or not parsed";

    const std::pair<const char*, const char*>       kProjects[] = { { "Common", "Desert/Common/Source/" },
                                                                    { "Desert", "Desert/Desert/Source/" },
                                                                    { "Editor", "Editor/Source/" } };
    std::map<std::string, std::vector<std::string>> definers; // "project name" -> sources
    for ( const Source& s : Sources() )
    {
        if ( s.Name.size() < 4 || s.Name.compare( s.Name.size() - 4, 4, ".cpp" ) != 0 )
            continue;
        // Platform directories compile on one platform only, so a MacOS/Windows pair never meets.
        if ( s.Name.find( "/Platform/MacOS/" ) != std::string::npos ||
             s.Name.find( "/Platform/Linux/" ) != std::string::npos ||
             s.Name.find( "lightweightvk" ) != std::string::npos )
            continue;
        bool optedOut = false;
        for ( const std::string& o : optOuts )
            optedOut = optedOut || s.Name.find( o ) != std::string::npos;
        if ( optedOut )
            continue;
        for ( const auto& [project, prefix] : kProjects )
        {
            if ( s.Name.rfind( prefix, 0 ) != 0 )
                continue;
            for ( const std::string& name : InternalNames( s.Code ) )
                definers[std::string( project ) + " " + name].push_back( s.Name );
        }
    }

    std::set<std::string> registered( std::begin( kUnityDuplicateRegister ), std::end( kUnityDuplicateRegister ) );
    std::vector<std::string> unregistered;
    for ( const auto& [key, files] : definers )
    {
        if ( files.size() > 1 && registered.count( key ) == 0 )
        {
            std::string where;
            for ( const std::string& f : files )
                where += " " + f;
            unregistered.push_back( key + " —" + where );
        }
    }
    std::vector<std::string> stale;
    for ( const std::string& row : registered )
    {
        const auto it = definers.find( row );
        if ( it == definers.end() || it->second.size() < 2 )
            stale.push_back( row );
    }

    EXPECT_TRUE( unregistered.empty() )
         << "The same internal-linkage name is defined in two unity sources of one project. They compile "
            "alone and collide in the MSBuild unity file the day they are grouped together (Windows only). "
            "Rename one (prefer a name that says what it does), or — for an intended overload — add a row "
            "to kUnityDuplicateRegister:"
         << Listed( unregistered );
    EXPECT_TRUE( stale.empty() ) << "Register rows that are no longer duplicates; delete them:" << Listed( stale );
}

// THE NAMES windows.h TURNS INTO OTHER NAMES (class (b), CIW6).
//
// Every Win32 call with a string argument is a macro that picks its A or W form: `CreateDirectory` is
// `CreateDirectoryW` in any translation unit that has seen windows.h. A method of ours with that name is
// renamed in exactly the TUs that include windows.h before it, so its declaration and its call disagree
// across TUs (link error), or a header fights the macro with `#undef` that holds only while nobody includes
// windows.h after it. A handful more are plain object-like macros: `ERROR` (wingdi.h), `DELETE` (winnt.h),
// `IN`/`OUT`/`OPTIONAL` (minwindef.h, empty), `interface` (combaseapi.h, `struct`), `small` (rpcndr.h, `char`).
// All of it compiles on macOS and Linux; the name is the defect, so the name is what is refused - in code
// only, wherever it appears, because `::CreateFileW` is how a Windows source calls the API by its real name.
TEST( ReservedIdentifiers, NoIdentifierIsANameWindowsHDefinesAway )
{
    // A token set, not one regex: an alternation of sixty words over every byte of the tree is what made this
    // suite take minutes once (CIW5), and the question is only "is this identifier one of these".
    static const std::set<std::string> kMacroNames = { "LoadImage",
                                                       "GetMessage",
                                                       "SendMessage",
                                                       "PostMessage",
                                                       "CreateWindow",
                                                       "CreateWindowEx",
                                                       "DrawText",
                                                       "GetObject",
                                                       "CreateFile",
                                                       "DeleteFile",
                                                       "CopyFile",
                                                       "MoveFile",
                                                       "CreateDirectory",
                                                       "RemoveDirectory",
                                                       "LoadLibrary",
                                                       "GetModuleFileName",
                                                       "CreateEvent",
                                                       "CreateMutex",
                                                       "CreateSemaphore",
                                                       "GetCurrentDirectory",
                                                       "SetCurrentDirectory",
                                                       "GetTempPath",
                                                       "FindFirstFile",
                                                       "FindNextFile",
                                                       "GetFileAttributes",
                                                       "GetUserName",
                                                       "GetComputerName",
                                                       "FormatMessage",
                                                       "OutputDebugString",
                                                       "MessageBox",
                                                       "GetClassName",
                                                       "RegisterClass",
                                                       "GetCommandLine",
                                                       "GetEnvironmentVariable",
                                                       "SetEnvironmentVariable",
                                                       "CreateProcess",
                                                       "CreateFont",
                                                       "GetTextMetrics",
                                                       "TextOut",
                                                       "LoadIcon",
                                                       "LoadCursor",
                                                       "LoadBitmap",
                                                       "LoadString",
                                                       "GetWindowText",
                                                       "SetWindowText",
                                                       "DispatchMessage",
                                                       "PeekMessage",
                                                       "PlaySound",
                                                       "ChooseColor",
                                                       "ChooseFont",
                                                       "ERROR",
                                                       "DELETE",
                                                       "IN",
                                                       "OUT",
                                                       "OPTIONAL",
                                                       "interface",
                                                       "small" };
    std::vector<std::string>           offenders;
    for ( const Source& s : Sources() )
    {
        const std::string& code = s.Code;
        for ( std::size_t i = 0; i < code.size(); )
        {
            if ( !IsWordChar( code[i] ) )
            {
                ++i;
                continue;
            }
            std::size_t end = i;
            while ( end < code.size() && IsWordChar( code[end] ) )
                ++end;
            if ( kMacroNames.contains( code.substr( i, end - i ) ) )
                offenders.push_back( s.Name + ":" + std::to_string( LineOf( code, i ) ) + "  " +
                                     code.substr( i, end - i ) );
            i = end;
        }
    }
    EXPECT_TRUE( offenders.empty() ) << offenders.size()
                                     << " identifier(s) that windows.h #defines to another name. Rename them "
                                        "(MakeSemaphore, DrawLabel, smallOne...); a Windows source calls the "
                                        "API by its W name:"
                                     << Listed( offenders );
}

// THE WRITE TIME IS NOT AN IDENTITY (class (k), CIW6).
//
// A file's write time moves in the file system's ticks - ~15.6 ms on NTFS - so two same-size writes inside
// one tick are indistinguishable by (write time, size). FIX2 met it in the thumbnail memo, the shader include
// cache met it before that, and ModelingToolTarget keyed a lift on it. Every reader of a write time therefore
// either applies the racy rule (Common::Utils::IsRacyWriteTime, in the same file) or has a row here that says
// why an equal stamp cannot serve stale content. A row that stops matching fails as well.
namespace
{
    struct WriteTimeRow
    {
        const char* File;
        const char* Why;
    };
    constexpr WriteTimeRow kWriteTimeRegister[] = {
         { "Desert/Desert/Source/Engine/Runtime/AssetHotReload.cpp",
           "a watcher, not a memo: OPEN - a second same-size write inside one tick is picked up only at the "
           "file's next write" },
         { "Desert/Desert/Source/Engine/ECS/System/ScriptSystem.hpp",
           "a watcher, not a memo: OPEN - as AssetHotReload, for Lua sources" },
         { "Editor/Source/Editor/Panels/Logs/LogsPanel.cpp",
           "a tail follower: the log only grows, so a missed tick is read with the next append" },
         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp", "display and sort order only" },
         { "Editor/Source/Editor/Core/CrashRecovery.cpp", "picks the newest autosave: ordering, not identity" },
         { "Editor/Source/Editor/Import/ImportManager.cpp", "source-newer-than-cook ordering, not identity" },
         { "Editor/Source/Editor/Import/Blend/BlendImporter.hpp", "blend-newer-than-fbx ordering, not identity" },
         { "Editor/Source/Editor/Widgets/ThumbnailCache.cpp",
           "decoded-PNG memo: OPEN - FIX2's class; a PNG rewritten inside one tick keeps the old image until "
           "its next write" },
         { "Editor/Source/Editor/Widgets/ThumbnailFreshness.hpp",
           "FIX2: the record's writer tells the memo what it wrote, so the stamp is never the only witness" },
    };
} // namespace

TEST( ReservedIdentifiers, EveryWriteTimeReaderStatesItsRule )
{
    const std::regex      reads( R"(\blast_write_time\s*\()" );
    const std::regex      racyRule( R"(\bIsRacyWriteTime\s*\()" );
    std::set<std::string> registered;
    for ( const WriteTimeRow& row : kWriteTimeRegister )
        registered.insert( row.File );

    std::vector<std::string> offenders;
    std::set<std::string>    matched;
    for ( const Source& s : Sources() )
    {
        // Tests pin write times on purpose: that is how FIX2's class is reproduced on one machine.
        if ( s.Name.starts_with( "Desert/Tests/" ) || s.Code.find( "last_write_time" ) == std::string::npos ||
             !std::regex_search( s.Code, reads ) )
            continue;
        if ( std::regex_search( s.Code, racyRule ) )
            continue;
        if ( registered.contains( s.Name ) )
            matched.insert( s.Name );
        else
            offenders.push_back( s.Name );
    }
    std::vector<std::string> stale;
    for ( const std::string& name : registered )
    {
        if ( !matched.contains( name ) )
            stale.push_back( name );
    }
    EXPECT_TRUE( offenders.empty() )
         << "Sources that read a write time with neither Common::Utils::IsRacyWriteTime nor a register row "
            "saying why an equal stamp cannot serve stale content:"
         << Listed( offenders );
    EXPECT_TRUE( stale.empty() ) << "Register rows that no longer read a write time without the racy rule; "
                                    "delete them:"
                                 << Listed( stale );
}

// A PATH OR A COMMAND SPELLED FOR ONE PLATFORM (class (l), CIW6).
//
// Three spellings reach a Windows runner intact and mean something else there: a `path::string()` compared
// with a forward-slashed literal (the native separator is `\`), a single-quoted argument in a command handed
// to `_popen` (cmd.exe does not treat `'` as a quote, so the path splits at its first space), and a home
// directory read from HOME alone (Windows sets USERPROFILE; HOME exists there only if someone put it).
TEST( ReservedIdentifiers, NoPathOrCommandIsSpelledForOnePlatform )
{
    const std::regex nativeVsSlashed(
         R"((EXPECT|ASSERT)_(EQ|NE)\(\s*[^;]*\.string\(\)\s*,\s*"[^"\n]*/[^"\n]*"\s*\))"
         R"(|(EXPECT|ASSERT)_(EQ|NE)\(\s*"[^"\n]*/[^"\n]*"\s*,\s*[^;,]*\.string\(\)\s*\))" );
    const std::regex         windowsPipe( R"(\b_popen\s*\()" );
    const std::regex         singleQuoteInLiteral( R"("[^"\n]*'[^"\n]*")" );
    const std::regex         homeOnly( R"(getenv\(\s*"HOME"\s*\))" );
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        // The literal and the command live in string literals, which Code blanks: match the text, but only on
        // lines Code says carry code.
        std::istringstream       text( s.Text );
        std::istringstream       code( s.Code );
        std::string              textLine;
        std::string              codeLine;
        std::vector<std::string> recent;
        int                      number = 0;
        while ( std::getline( text, textLine ) && std::getline( code, codeLine ) )
        {
            ++number;
            const std::string where  = s.Name + ":" + std::to_string( number );
            const bool        isCode = codeLine.find_first_not_of( " \t\r" ) != std::string::npos;
            if ( isCode && textLine.find( ".string()" ) != std::string::npos &&
                 std::regex_search( textLine, nativeVsSlashed ) )
                offenders.push_back( where + "  a native path string compared with a '/' literal" );
            recent.push_back( isCode ? textLine : std::string() );
            if ( recent.size() > 4 )
                recent.erase( recent.begin() );
            if ( codeLine.find( "_popen" ) == std::string::npos || !std::regex_search( codeLine, windowsPipe ) )
                continue;
            for ( const std::string& previous : recent )
            {
                if ( std::regex_search( previous, singleQuoteInLiteral ) )
                    offenders.push_back( where + "  a single quote in a command cmd.exe will run" );
            }
        }
        // Tests SET HOME to keep the product out of the real user config, which works because the product
        // honours HOME before USERPROFILE; the product itself must know both.
        if ( !s.Name.starts_with( "Desert/Tests/" ) && std::regex_search( s.Code, homeOnly ) &&
             s.Text.find( "USERPROFILE" ) == std::string::npos )
            offenders.push_back( s.Name + "  reads HOME with no USERPROFILE beside it" );
    }
    EXPECT_TRUE( offenders.empty() ) << offenders.size()
                                     << " spelling(s) that mean something else on Windows:" << Listed( offenders );
}

// TWO EMITS IN ONE FULL EXPRESSION (class (m), CIW6).
//
// The order in which a call's arguments are evaluated is unspecified; clang goes left to right and MSVC right
// to left, both conforming. An emitter hands out temporaries as it goes, so
// `std::format( "{} * {}", EmitInput( a ), EmitInput( b ) )` numbers them differently per compiler: the
// shader graph produced different GLSL on the two platforms and moved the SPIR-V cache key with it. One emit
// per full expression - named locals first, then the expression that joins them - sequences it.
TEST( ReservedIdentifiers, NoFullExpressionEmitsTwice )
{
    const std::regex         emit( R"(\bEmit\w*\s*\()" );
    std::vector<std::string> offenders;
    for ( const Source& s : Sources() )
    {
        // Cheap reject: std::regex over every statement of every file is what made this suite slow once.
        if ( s.Code.find( "Emit" ) == std::string::npos )
            continue;
        std::size_t begin = 0;
        for ( std::size_t i = 0; i <= s.Code.size(); ++i )
        {
            if ( i < s.Code.size() && s.Code[i] != ';' && s.Code[i] != '{' && s.Code[i] != '}' )
                continue;
            const std::string statement = s.Code.substr( begin, i - begin );
            begin                       = i + 1;
            if ( statement.find( "Emit" ) == statement.rfind( "Emit" ) )
                continue;
            const auto count = std::distance( std::sregex_iterator( statement.begin(), statement.end(), emit ),
                                              std::sregex_iterator() );
            if ( count > 1 )
                offenders.push_back( s.Name + ":" + std::to_string( LineOf( s.Code, i ) ) );
        }
    }
    EXPECT_TRUE( offenders.empty() ) << offenders.size()
                                     << " full expression(s) with two Emit calls, whose order MSVC and clang "
                                        "evaluate differently; hoist each into a named local:"
                                     << Listed( offenders );
}
