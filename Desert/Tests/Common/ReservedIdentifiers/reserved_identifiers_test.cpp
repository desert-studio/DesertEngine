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

#include <filesystem>
#include <fstream>
#include <regex>
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
        for ( const fs::path& sub : { fs::path( "Desert" ), fs::path( "Editor" ), fs::path( "Runtime" ) } )
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
            const std::string code = line.substr( 0, line.find( "//" ) );
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
            if ( std::regex_search( line, nativeFilter ) )
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
            if ( firstUse == 0 && std::regex_search( line, posixPipe ) )
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
