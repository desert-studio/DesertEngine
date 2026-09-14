// THE BUILD AND PACKAGING SCRIPTS, PINNED AGAINST THE TREE THEY DESCRIBE.
//
// Every assertion here is a RELATION between two things that must agree and are written down in
// different files, which is this project's most expensive defect shape: both sides look right on
// their own, so a unit test of either passes.
//
// The four relations, and the live defect each of them was written from:
//
//   1. A script path named in prose must exist. README.md:14 invoked
//      a RunProjectHub.sh under scripts/MacOS for months after the launcher moved to its own repo.
//      Nothing said so, because prose has no compiler. The same class swallows every rename that
//      misses one call site, and this task renamed four scripts.
//
//   2. The Windows Vulkan SDK pin in scripts/Windows/Setup.bat must equal the one in
//      .github/workflows/ci.yml. A developer building against a different SDK from CI is how a link
//      error becomes "works on my machine"; the ci.yml comment pins its version deliberately and
//      says why, and the setup script had no version at all until it started installing the SDK.
//
//   3. The engine resource trees the packagers copy must be exactly the non-Assets directories under
//      Editor/Resources — DERIVED from the tree, so adding Editor/Resources/Audio reddens this
//      instead of silently shipping a package without it. Both packagers are checked, because a
//      one-platform fix is how the two drifted in the first place.
//
//   4. The base scene's asset closure must contain every asset the scene names in plain text, and
//      every file in it must exist. This is the guard on the packaging change that took the drop from
//      133 MB to under 4 MB: the failure mode of a too-small package is not a crash, it is a game
//      that starts and shows nothing.

#include <gtest/gtest.h>

#include <Editor/Core/AssetReferences.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Walk up from wherever the binary was started, exactly as the other tree-reading suites do, so
    // this need not be run from one precise directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Common/Source/Common/Utilities/FileSystem.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Every "scripts/..." or "scripts\..." spelling in @p text that ends in a script extension.
    // Backslashes are normalised, because the Windows half of the tree writes them that way and the
    // question — does this file exist — is the same question.
    std::set<std::string> ScriptPathsIn( const std::string& text )
    {
        static const std::vector<std::string> kExts = { ".sh", ".bat", ".ps1", ".py" };
        std::set<std::string>                 found;

        for ( std::size_t pos = text.find( "scripts" ); pos != std::string::npos;
              pos             = text.find( "scripts", pos + 1 ) )
        {
            // "scripts" must start a path component: `Scripts/` inside an asset path, and the word in
            // ordinary prose, are not references to this directory.
            //
            // BACKSLASH IS IN THIS LIST AND IT WAS MISSING ONCE. scripts\Windows\BuildWindows.bat
            // spells its analyser path `%ROOT%\scripts\CI\CheckTidy.bat`; with '\' rejected as a
            // boundary the gate simply did not see it, and a reference the gate cannot see is exactly
            // what this suite exists to make impossible. Half the tree writes Windows separators.
            if ( pos > 0 )
            {
                const char before = text[pos - 1];
                if ( before != ' ' && before != '"' && before != '\'' && before != '`' && before != '(' &&
                     before != '/' && before != '\\' && before != '\n' && before != '\t' )
                    continue;
                if ( before == '/' && pos >= 2 && text[pos - 2] != '.' )
                    continue; // ".../scripts" of some other tree
            }

            std::size_t end = pos;
            while ( end < text.size() )
            {
                const char c  = text[end];
                const bool ok = std::isalnum( static_cast<unsigned char>( c ) ) || c == '/' || c == '\\' ||
                                c == '.' || c == '_' || c == '-';
                if ( !ok )
                    break;
                ++end;
            }

            std::string candidate = text.substr( pos, end - pos );
            std::replace( candidate.begin(), candidate.end(), '\\', '/' );

            const bool hasExt =
                 std::any_of( kExts.begin(), kExts.end(),
                              [&]( const std::string& e ) {
                                  return candidate.size() > e.size() &&
                                         candidate.compare( candidate.size() - e.size(), e.size(), e ) == 0;
                              } );
            if ( hasExt )
                found.insert( candidate );
        }
        return found;
    }

    // The dotted version that follows @p declaration, which must be the ASSIGNMENT and not a mention.
    // The first cut of this searched for the bare key name and found it in Setup.bat's own comment —
    // "Must equal VULKAN_SDK_VERSION in ci.yml" — parsed no digits out of it and reported the pin as
    // absent. A check that reads a comment instead of the code is the defect it exists to catch.
    std::string VersionAfter( const std::string& text, const std::string& declaration )
    {
        const std::size_t at = text.find( declaration );
        if ( at == std::string::npos )
            return {};
        std::size_t i = at + declaration.size();
        while ( i < text.size() && ( text[i] == ' ' || text[i] == '"' ) )
            ++i;
        std::string value;
        while ( i < text.size() && ( std::isdigit( static_cast<unsigned char>( text[i] ) ) || text[i] == '.' ) )
            value += text[i++];
        return value;
    }

    // The whitespace-separated words between @p open and @p close — the literal list a shell `for`
    // iterates. Both packagers spell their engine-tree loop that way, so the test can read the list
    // the script actually walks instead of matching on a path the script builds from a variable.
    std::vector<std::string> WordsBetween( const std::string& text, const std::string& open,
                                           const std::string& close )
    {
        const std::size_t start = text.find( open );
        if ( start == std::string::npos )
            return {};
        const std::size_t from = start + open.size();
        const std::size_t end  = text.find( close, from );
        if ( end == std::string::npos )
            return {};

        std::vector<std::string> words;
        std::istringstream       in( text.substr( from, end - from ) );
        std::string              word;
        while ( in >> word )
            words.push_back( word );
        std::sort( words.begin(), words.end() );
        return words;
    }
} // namespace

// ── 1. NO SCRIPT REFERENCE POINTS AT A FILE THAT IS NOT THERE ───────────────────────────────────

namespace
{
    struct PendingReference
    {
        const char* Path;
        const char* OwedBy;
    };

    // THE ONE LEGITIMATE REASON A REFERENCE CAN POINT AT NOTHING: the file is owed by a task that has
    // not landed. Each row NAMES that task, because an exception with no owner is unreadable in a
    // month — the same rule ConfigOwnership's debt register is held to.
    //
    // It cannot rot, and that is the half that matters: a row whose file now EXISTS is a FAILURE, not
    // a pass, so the register empties itself as the work arrives instead of quietly outliving it. A
    // register that only ever grows is a list of excuses.
    //
    // std::array rather than a C array: this register is expected to reach zero rows, and a
    // zero-length C array is a GNU extension that MSVC rejects outright (C2466).
    constexpr std::array<PendingReference, 2> kPendingReferences = { {
         { "scripts/CI/CheckTidy.sh",
           "I15 — the clang-tidy gate itself. scripts/MacOS/BuildMacOS.sh runs it by default and "
           "REFUSES when it is absent, so this row disappears the moment that task lands." },
         { "scripts/CI/CheckTidy.bat",
           "NOBODY YET — Windows has no entry point for the analyser at all; the gate landed as bash. "
           "C1 named the path its build script will call so the hole is visible; filing the task is a "
           "decision for the lead, and until then a Windows developer gets the same named refusal." },
    } };

    const PendingReference* PendingRowFor( const std::string& ref )
    {
        for ( const auto& row : kPendingReferences )
            if ( ref == row.Path )
                return &row;
        return nullptr;
    }
} // namespace

// A reference that is owed by a named task may be absent. Every other absent reference fails, and a
// row that is no longer absent fails too.
TEST( BuildScriptContract, ThePendingReferenceRegisterIsNotStale )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const auto& row : kPendingReferences )
    {
        EXPECT_FALSE( fs::exists( root + row.Path ) )
             << row.Path << " now exists, so its row in kPendingReferences is stale — delete the row. "
             << "It was owed by: " << row.OwedBy;
    }
}

TEST( BuildScriptContract, EveryReferencedScriptExists )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root";

    std::vector<std::string> sources = { root + "README.md", root + ".github/workflows/ci.yml" };
    for ( const auto& e : fs::recursive_directory_iterator( root + "scripts" ) )
    {
        if ( !e.is_regular_file() )
            continue;
        sources.push_back( e.path().string() );
    }

    std::vector<std::string> dead;
    std::vector<std::string> pending;
    for ( const auto& source : sources )
    {
        const std::string text = ReadFile( source );
        ASSERT_FALSE( text.empty() ) << source << " is empty or unreadable";
        for ( const auto& ref : ScriptPathsIn( text ) )
        {
            if ( fs::exists( root + ref ) )
                continue;
            if ( const PendingReference* row = PendingRowFor( ref ) )
                pending.push_back( source + " -> " + ref + "  [owed by: " + row->OwedBy + "]" );
            else
                dead.push_back( source + " -> " + ref );
        }
    }

    // Printed, not swallowed. The point of the register is that the holes stay VISIBLE in every run.
    for ( const auto& p : pending )
        std::cout << "  pending: " << p << "\n";

    std::ostringstream report;
    for ( const auto& d : dead )
        report << "\n  " << d;
    EXPECT_TRUE( dead.empty() ) << "script references pointing at nothing:" << report.str();
}

// ── 2. THE VULKAN SDK PIN IS THE SAME ON BOTH SIDES ─────────────────────────────────────────────

TEST( BuildScriptContract, WindowsSetupPinsTheSameVulkanSdkAsCI )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string ci    = ReadFile( root + ".github/workflows/ci.yml" );
    const std::string setup = ReadFile( root + "scripts/Windows/Setup.bat" );
    ASSERT_FALSE( ci.empty() );
    ASSERT_FALSE( setup.empty() );

    const std::string ciVersion    = VersionAfter( ci, "VULKAN_SDK_VERSION:" );
    const std::string setupVersion = VersionAfter( setup, "set \"VULKAN_SDK_VERSION=" );

    EXPECT_FALSE( ciVersion.empty() ) << "ci.yml no longer pins VULKAN_SDK_VERSION";
    EXPECT_FALSE( setupVersion.empty() ) << "Setup.bat no longer pins VULKAN_SDK_VERSION";
    EXPECT_EQ( ciVersion, setupVersion )
         << "CI builds Windows against " << ciVersion << " and a developer's machine gets " << setupVersion
         << ". The one that is wrong is whichever was changed alone.";
}

// ── 3. THE PACKAGERS SHIP EVERY ENGINE RESOURCE TREE ────────────────────────────────────────────

TEST( BuildScriptContract, BothPackagersShipEveryEngineResourceTree )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // DERIVED from the tree, not typed: this is the whole point. `Assets` is excluded because it is
    // the sandbox PROJECT's content, which travels as the base scene's closure rather than whole.
    std::vector<std::string> engineTrees;
    for ( const auto& e : fs::directory_iterator( root + "Editor/Resources" ) )
    {
        if ( !e.is_directory() )
            continue;
        const std::string name = e.path().filename().string();
        if ( name != "Assets" )
            engineTrees.push_back( name );
    }
    std::sort( engineTrees.begin(), engineTrees.end() );
    ASSERT_FALSE( engineTrees.empty() );

    const std::string sh  = ReadFile( root + "scripts/MacOS/Package.sh" );
    const std::string bat = ReadFile( root + "scripts/Windows/Package.bat" );
    ASSERT_FALSE( sh.empty() );
    ASSERT_FALSE( bat.empty() );

    // Read the list each script's loop actually walks. Matching on the built path would prove nothing:
    // both scripts assemble it from a variable, so "Editor/Resources/Shaders" appears in neither.
    const std::vector<std::string> shTrees  = WordsBetween( sh, "for tree in ", "; do" );
    const std::vector<std::string> batTrees = WordsBetween( bat, "for %%T in (", ")" );

    EXPECT_EQ( shTrees, engineTrees )
         << "scripts/MacOS/Package.sh copies a different set of engine resource trees than "
            "Editor/Resources holds. A tree it omits is one the drop starts without and then fails on "
            "at the first thing that reads it.";
    EXPECT_EQ( batTrees, engineTrees )
         << "scripts/Windows/Package.bat copies a different set of engine resource trees than "
            "Editor/Resources holds.";
}

// ── 4. THE BASE SCENE'S CLOSURE IS COMPLETE ─────────────────────────────────────────────────────

TEST( BuildScriptContract, BaseSceneClosureCoversWhatTheSceneNames )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const fs::path assetsRoot = fs::path( root + "Editor/Resources/Assets" ).lexically_normal();
    const fs::path projectDir = fs::path( root + "Editor" ).lexically_normal();
    ASSERT_TRUE( fs::is_directory( assetsRoot ) );

    const std::string sceneKey  = "Scenes/Starter.desce";
    const std::string sceneText = ReadFile( ( assetsRoot / sceneKey ).string() );
    ASSERT_FALSE( sceneText.empty() ) << "the drop's base scene is unreadable";

    Desert::Editor::AssetReferenceIndex index;
    Desert::Editor::BuildAssetReferenceIndex( index, assetsRoot, projectDir );
    ASSERT_FALSE( index.Entries().empty() );

    const std::vector<std::string> closure = index.ClosureFrom( sceneKey );
    ASSERT_FALSE( closure.empty() ) << "the base scene is not in an index built from its own tree";

    // Every file the closure names really is a file. A name that is not one would be copied by the
    // packagers' `cp`, which fails loudly there — but only on the platform that runs it, and only at
    // package time.
    for ( const auto& rel : closure )
        EXPECT_TRUE( fs::is_regular_file( assetsRoot / rel ) ) << rel << " is in the closure and is not a file";

    // THE RELATION THAT MATTERS: everything the scene names in PLAIN TEXT must be in the graph-derived
    // closure. The graph also finds references the text does not spell out (Starter_Glass_Inst.demat
    // is reached by handle and by nothing else), so this is a lower bound on the closure and not a
    // definition of it — if the token rule ever regresses, the closure shrinks below this bound.
    std::set<std::string> spelledOut;
    for ( std::size_t pos = sceneText.find( ".demat" ); pos != std::string::npos;
          pos             = sceneText.find( ".demat", pos + 1 ) )
    {
        std::size_t start = pos;
        while ( start > 0 && sceneText[start - 1] != '"' )
            --start;
        spelledOut.insert( sceneText.substr( start, pos + 6 - start ) );
    }
    ASSERT_FALSE( spelledOut.empty() ) << "the base scene names no material at all — is this still the scene?";

    for ( const auto& named : spelledOut )
    {
        EXPECT_NE( std::find( closure.begin(), closure.end(), named ), closure.end() )
             << named << " is written into " << sceneKey << " and is NOT in the closure the packagers ship";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
