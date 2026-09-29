// THE BUILD AND PACKAGING SCRIPTS, PINNED AGAINST THE TREE THEY DESCRIBE.
//
// Every assertion here is a RELATION between two things that must agree and are written down in
// different files, which is this project's most expensive defect shape: both sides look right on
// their own, so a unit test of either passes.
//
// The five relations (the fifth, GLFW's two include doors, is described at its test), and the live defect each of
// them was written from:
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
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if !defined( _WIN32 )
#include <sys/wait.h>
#endif

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
    constexpr std::array<PendingReference, 1> kPendingReferences = { {
         { "scripts/CI/CheckTidy.bat",
           "NOT FILED YET, and the blocker is measured rather than organisational. The gate reads a "
           "compile_commands.json that scripts/CI/GenCompileCommands.sh derives from premake's gmake2 "
           "makefiles; on Windows the build is MSBuild/MSVC, so those makefiles describe flags, macros "
           "and system headers that nothing on that platform actually compiles — the analyser would be "
           "reading a different program from the one being shipped. A Windows entry point therefore "
           "needs an MSVC-flavoured database first, which is its own task. What it costs to wait is "
           "18 of our 986 .cpp/.hpp: the files carrying a _WIN32 / _MSC_VER / DESERT_PLATFORM_WINDOWS "
           "branch, which is the only code the macOS gate cannot see. Until then a Windows developer "
           "gets the named refusal above rather than a silent skip." },
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
    // both scripts assemble it from a variable, so "Engine/Content/Shaders" appears in neither.
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

// ── 5. GLFW HAS TWO DOORS, AND THE VULKAN ONE DECLARES WHAT IT SERVES ────────────────────────────
//
// glfw3.h declares glfwCreateWindowSurface only if a Vulkan header was included BEFORE its first
// inclusion in the translation unit; the include guard ignores every later GLFW_INCLUDE_VULKAN. Under
// the MSVC unity build a translation unit is a group of sources, so one bare <GLFW/glfw3.h> anywhere
// in Desert can end up ahead of VulkanSwapChain.cpp. SPAWN1 added one source, the groups shifted, and
// every Windows job failed with 'glfwCreateWindowSurface': identifier not found while macOS stayed
// green. MSVC1 then put Vulkan into the only door, and every tool and suite that reaches Window.hpp
// through Components.hpp without a Vulkan include directory (WorldGen, SceneMigrator, the clang-tidy
// header pass) failed on 'vulkan/vulkan.h' file not found.
//
// The relations pinned: (a) no engine, editor or runtime source names <GLFW/glfw3.h> except Glfw.hpp;
// (b) Glfw.hpp does not need Vulkan; (c) GlfwVulkan.hpp includes Vulkan and declares
// glfwCreateWindowSurface itself, so the declaration no longer depends on inclusion order; (d) every
// source that calls glfwCreateWindowSurface names GlfwVulkan.hpp itself.
TEST( BuildScriptContract, GlfwIsIncludedOnlyThroughItsEntryHeaders )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root";

    const std::string plain       = "Desert/Desert/Source/Engine/Core/Glfw.hpp";
    const std::string vulkan      = "Desert/Desert/Source/Engine/Core/GlfwVulkan.hpp";
    const std::string plainText   = ReadFile( root + plain );
    const std::string vulkanText  = ReadFile( root + vulkan );
    const std::string surfaceCall = "glfwCreateWindowSurface(";
    ASSERT_FALSE( plainText.empty() ) << plain << " is missing";
    ASSERT_FALSE( vulkanText.empty() ) << vulkan << " is missing";

    EXPECT_NE( plainText.find( "#include <GLFW/glfw3.h>" ), std::string::npos ) << plain << " must include GLFW";
    EXPECT_EQ( plainText.find( "vulkan.h" ), std::string::npos )
         << plain << " must not need the Vulkan SDK: it is reached from Components.hpp by Vulkan-free projects";
    EXPECT_EQ( plainText.find( "GLFW_INCLUDE_VULKAN" ), std::string::npos )
         << plain << " must not ask GLFW to include Vulkan";

    const auto vulkanAt = vulkanText.find( "#include <vulkan/vulkan.h>" );
    const auto doorAt   = vulkanText.find( "#include <Engine/Core/Glfw.hpp>" );
    const auto declAt   = vulkanText.find( "GLFWAPI VkResult glfwCreateWindowSurface(" );
    ASSERT_NE( vulkanAt, std::string::npos ) << vulkan << " must include <vulkan/vulkan.h>";
    ASSERT_NE( doorAt, std::string::npos ) << vulkan << " must reach GLFW through " << plain;
    ASSERT_NE( declAt, std::string::npos ) << vulkan << " must declare glfwCreateWindowSurface itself";
    EXPECT_LT( vulkanAt, declAt ) << vulkan << " must include Vulkan before its declaration";
    EXPECT_LT( doorAt, declAt ) << vulkan << " must include GLFW (for GLFWAPI) before its declaration";

    std::vector<std::string> bare;
    std::vector<std::string> undeclared;
    std::size_t              scanned = 0;
    std::size_t              callers = 0;
    for ( const char* dir : { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
    {
        for ( const auto& e : fs::recursive_directory_iterator( root + dir ) )
        {
            const std::string ext = e.path().extension().string();
            if ( !e.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".mm" ) )
                continue;
            const std::string rel = fs::relative( e.path(), root ).generic_string();
            ++scanned;
            if ( rel == plain || rel == vulkan )
                continue;
            const std::string text = ReadFile( e.path().string() );
            if ( text.find( "<GLFW/glfw3.h>" ) != std::string::npos )
                bare.push_back( rel );
            if ( text.find( surfaceCall ) != std::string::npos )
            {
                ++callers;
                if ( text.find( "#include <Engine/Core/GlfwVulkan.hpp>" ) == std::string::npos )
                    undeclared.push_back( rel );
            }
        }
    }
    EXPECT_GT( scanned, 100U ) << "the walk found almost no sources; the roots moved";
    EXPECT_GT( callers, 0U ) << "nothing calls " << surfaceCall << " any more; the relation guards nothing";

    std::ostringstream report;
    for ( const auto& b : bare )
        report << "\n  " << b;
    EXPECT_TRUE( bare.empty() ) << "sources including <GLFW/glfw3.h> instead of <Engine/Core/Glfw.hpp>:"
                                << report.str();

    std::ostringstream callReport;
    for ( const auto& u : undeclared )
        callReport << "\n  " << u;
    EXPECT_TRUE( undeclared.empty() ) << "sources calling glfwCreateWindowSurface without naming "
                                      << "<Engine/Core/GlfwVulkan.hpp>:" << callReport.str();
}

// THE GLUED-TEXT GATE, PINNED ON FIXTURES (FMT1). scripts/CI/CheckGluedText.sh gates CI's changed lines and the
// hand-off; a regex that silently stopped matching would turn it into a gate that is always green, and one that
// matched too much would redden every branch touching a path join. Each fixture is one line with a known verdict,
// run through the script's --file mode, so the script itself — not a copy of its rule — is what is tested.
#if !defined( _WIN32 )
namespace
{
    int RunGluedTextGate( const std::string& root, const fs::path& fixture )
    {
        const std::string command = std::format(
             "bash '{}scripts/CI/CheckGluedText.sh' --file '{}' >/dev/null 2>&1", root, fixture.string() );
        // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, runs the real script
        const int status = std::system( command.c_str() );
        return WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
    }
} // namespace

TEST( BuildScriptContract, GluedTextGateSeparatesGlueFromFormat )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root";

    struct Fixture
    {
        const char* Name;
        const char* Line;
        int         Expected;
    };
    const std::array<Fixture, 9> fixtures = { {
         { "glued", R"(std::string a = "[" + id + "] " + text;)", 1 },
         { "glued_right", R"(return prefix + ".bak";)", 1 },
         { "glued_append", R"(out += "name=" + name;)", 1 },
         { "format", R"(std::string a = std::format( "[{}] {}", id, text );)", 0 },
         { "two_literals", R"(auto s = "one" "two"; auto t = "a" + "b";)", 0 },
         { "path_join", R"(const auto p = root / "Assets" / name;)", 0 },
         { "char_arith", R"(int d = '0' + digit; out += "literal"; // "x" + y)", 0 },
         { "glued_ok", R"(auto a = "[" + id; // glued-ok: fixture of the escape itself)", 0 },
         { "glued_ok_no_reason", R"(auto a = "[" + id; // glued-ok:)", 1 },
    } };

    const fs::path dir = fs::temp_directory_path() / "BuildScriptContract_GluedText";
    fs::create_directories( dir );
    for ( const auto& f : fixtures )
    {
        const fs::path file = dir / std::format( "{}.cpp", f.Name );
        {
            std::ofstream out( file, std::ios::binary );
            out << f.Line << '\n';
        }
        EXPECT_EQ( RunGluedTextGate( root, file ), f.Expected ) << f.Name << ": " << f.Line;
    }
    fs::remove_all( dir );
}
#endif

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
