// WHERE ASSIMP IS ALLOWED TO BE — five assertions about RELATIONS between two places in this
// repository, each of which reads as correct on its own.
//
// WHAT CHANGED UNDER THIS SUITE, AND WHY THE BOUNDARY NEEDED WRITING DOWN. Until D40 the two platforms
// did not build the same assimp at all: Windows linked a 400 KB import library committed on 27 July,
// built with toolset v142 against a workspace pinned to v143, whose file name was typed into the link
// line; macOS linked `-lassimp` out of Homebrew, i.e. whatever the developer happened to have (6.0.5
// here against the prebuilt's 5.x). Neither side pinned a version, so two machines could build a
// different engine from one commit. It is one pinned submodule compiled from source now.
//
// Fixing that does not keep it fixed. Every one of the five properties below was true before this change
// as well — and true BY ACCIDENT, which is exactly the state that ends: nobody had written an
// `#include <assimp/...>` into the engine, so the boundary held because no one had crossed it yet.
//
// NOT ONE OF THESE IS OBSERVABLE FROM A BUILD THAT SUCCEEDS. A build that links assimp into the shipped
// runtime succeeds. A build file that names a toolset succeeds until the toolset changes. A packaging
// script that copies a DLL nobody has succeeds silently — that one was live for months behind an
// `if exist`. So these are censuses over the repository's own text, the same instrument as
// ShippedShaderPasses and PointerOwnership, and for the same reason.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::filesystem::path RepositoryRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( here / "BuildScripts" / "ThirdParty" ); ++up )
        {
            here = here.parent_path();
        }
        return here;
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  out;
        out << in.rdbuf();
        return out.str();
    }

    // Every regular file under @p dir whose extension is in @p extensions, recursively. Returns paths
    // relative to the repository root so failure messages name something a person can open.
    std::vector<std::filesystem::path> FilesUnder( const std::filesystem::path&    dir,
                                                   const std::vector<std::string>& extensions )
    {
        std::vector<std::filesystem::path> found;
        std::error_code                    ec;
        if ( !std::filesystem::exists( dir, ec ) )
        {
            return found;
        }
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( dir, ec ) )
        {
            if ( !entry.is_regular_file( ec ) )
            {
                continue;
            }
            const std::string ext = entry.path().extension().string();
            if ( std::find( extensions.begin(), extensions.end(), ext ) != extensions.end() )
            {
                found.push_back( std::filesystem::relative( entry.path(), RepositoryRoot() ) );
            }
        }
        return found;
    }

    // `git ls-files`, i.e. what is actually TRACKED. The working tree cannot answer this: build outputs
    // sit in it by design and .gitignore is what separates them, so a directory walk would either report
    // every object file as a violation or have to re-implement gitignore to avoid it.
    std::vector<std::string> TrackedFiles()
    {
        std::vector<std::string> files;
        // `popen`/`pclose` ARE POSIX AND MSVC HAS NEITHER — it spells them `_popen`/`_pclose`. This file
        // shipped with the POSIX names and broke BOTH Windows configurations while every local gate and
        // the whole macOS sweep stayed green; same shape as the launcher's `setenv`.
        //
        // AND THE NAME IS THE SMALLER HALF. cmd.exe strips the outer pair of quotes off the whole command
        // line, so a quoted path needs one more pair around the lot, and the null device is `nul`, not
        // `/dev/null`. Getting only the name right would compile, run, produce nothing, and this census
        // would then examine an empty list — which the ASSERT below turns into a loud failure rather than
        // the silent pass it would otherwise be. The other two suites in this tree that pipe a child
        // process (CloudNoiseVolumeHandle, AssetHandleStability) already do it this way.
#ifdef DESERT_PLATFORM_WINDOWS
        const std::string command = "\"\"git\" -C \"" + RepositoryRoot().string() + "\" ls-files 2>nul\"";
        FILE*             pipe    = _popen( command.c_str(), "r" );
#else
        const std::string command = "git -C '" + RepositoryRoot().string() + "' ls-files 2>/dev/null";
        FILE*             pipe    = popen( command.c_str(), "r" );
#endif
        if ( pipe == nullptr )
        {
            return files;
        }
        std::array<char, 4096> line{};
        while ( std::fgets( line.data(), static_cast<int>( line.size() ), pipe ) != nullptr )
        {
            std::string entry( line.data() );
            while ( !entry.empty() && ( entry.back() == '\n' || entry.back() == '\r' ) )
            {
                entry.pop_back();
            }
            if ( !entry.empty() )
            {
                files.push_back( entry );
            }
        }
#ifdef DESERT_PLATFORM_WINDOWS
        _pclose( pipe );
#else
        pclose( pipe );
#endif
        return files;
    }
} // namespace

// П1. THE SHIPPED GAME MUST NOT CONTAIN AN ASSET IMPORTER. `Desert` is the engine library and `Runtime`
// is the game executable; assimp belongs to the EDITOR and to one offline tool. Asserted over the
// sources rather than over a link map because a link map only exists after a successful build, and the
// point is to refuse the `#include` that would make that build wrong.
TEST( AssimpBoundary, NeitherTheEngineNorTheRuntimeReachesForAssimp )
{
    const auto root = RepositoryRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<std::filesystem::path> guarded = {
         root / "Desert" / "Desert" / "Source",
         root / "Desert" / "Common" / "Source",
         // NOT Desert/Runtime/Source — the runtime lives at the repository root, and the first version of
         // this suite guarded the wrong path. It passed, because a directory that does not exist contains
         // no offending include; the `exists` assertions below are what turned that silence into a
         // failure. A census over a path nobody checks is the defect it is supposed to catch.
         root / "Runtime" / "Source",
    };

    for ( const auto& dir : guarded )
    {
        for ( const auto& file : FilesUnder( dir, { ".cpp", ".hpp", ".h", ".inl" } ) )
        {
            const std::string text = ReadFile( root / file );
            EXPECT_EQ( text.find( "#include <assimp/" ), std::string::npos )
                 << file.generic_string()
                 << " includes an assimp header. assimp is the EDITOR's importer and an offline tool's; "
                    "a single include here puts a mesh parser for fifty file formats into every shipped "
                    "game, and it would do so without one line of output anywhere to say it happened.";
            EXPECT_EQ( text.find( "#include \"assimp/" ), std::string::npos )
                 << file.generic_string() << " includes an assimp header (quoted form).";
        }
    }

    // The other half of the same statement: the build files for those two projects must not name the
    // library either. An `#include` is how it arrives by accident; a `links` entry is how it arrives on
    // purpose, and this suite has to refuse both.
    for ( const char* project : { "Desert/Desert/premake5.lua", "Runtime/premake5.lua" } )
    {
        const std::string text = ReadFile( root / project );
        if ( text.empty() )
        {
            continue; // named below, so a moved file fails loudly rather than silently passing
        }
        EXPECT_EQ( text.find( "Assimp" ), std::string::npos )
             << project << " names the Assimp project in its build file.";
        EXPECT_EQ( text.find( "assimp" ), std::string::npos ) << project << " names assimp.";
    }
    EXPECT_TRUE( std::filesystem::exists( root / "Desert/Desert/premake5.lua" ) )
         << "Desert/Desert/premake5.lua has moved; the check above silently covered nothing.";
    EXPECT_TRUE( std::filesystem::exists( root / "Runtime/premake5.lua" ) )
         << "Runtime/premake5.lua has moved; the check above silently covered nothing.";
    EXPECT_TRUE( std::filesystem::exists( root / "Runtime/Source" ) )
         << "Runtime/Source has moved; the include census above walked nothing.";
}

// П5. NO BUILT ARTIFACT IS TRACKED. This is what makes the submodule the only source of the library:
// the state it forbids is the one that existed until D40 — a committed `.lib` that nobody could
// reproduce, whose toolset nobody could see, and whose `.dll` half was never committed at all. It also
// closes the return of a prebuilt "just for now".
TEST( AssimpBoundary, NoCompiledLibraryIsTrackedAnywhereInTheRepository )
{
    const auto tracked = TrackedFiles();
    ASSERT_FALSE( tracked.empty() )
         << "`git ls-files` returned nothing, so this census examined no files at all. That is an "
            "environment failure, not a pass: run the suite from inside the repository.";

    const std::array<const char*, 5> forbidden = { ".lib", ".dll", ".a", ".dylib", ".so" };
    for ( const auto& file : tracked )
    {
        for ( const char* extension : forbidden )
        {
            const std::size_t at       = file.rfind( extension );
            const bool        endsWith = at != std::string::npos && at + std::strlen( extension ) == file.size();
            EXPECT_FALSE( endsWith )
                 << "'" << file
                 << "' is a compiled artifact and it is TRACKED. A binary in git is a "
                    "library nobody can rebuild, whose compiler and flags are invisible, and which goes "
                    "stale without a single line of output. assimp was exactly this until D40.";
        }
    }
}

// П6. NO BUILD FILE NAMES A TOOLSET. `assimp-vc142-mtd` was typed into a `links` line while the
// workspace pins v143: the two disagreed, and the place that broke when assimp was updated did not
// mention assimp. A toolset may be CHOSEN — `toolset "v143"` is a deliberate statement — but it may not
// be spelled inside a file name that something links by.
TEST( AssimpBoundary, NoBuildFileCarriesAToolsetNameInsideALibraryName )
{
    const auto root = RepositoryRoot();

    std::vector<std::filesystem::path> buildFiles;
    for ( const char* dir : { "BuildScripts", "Tools", "Editor", "Desert", "scripts" } )
    {
        for ( const auto& file : FilesUnder( root / dir, { ".lua", ".bat", ".sh", ".ps1" } ) )
        {
            buildFiles.push_back( file );
        }
    }
    for ( const char* file : { "premake5.lua", "Makefile" } )
    {
        if ( std::filesystem::exists( root / file ) )
        {
            buildFiles.emplace_back( file );
        }
    }
    ASSERT_FALSE( buildFiles.empty() ) << "no build files were found, so this census covered nothing.";

    for ( const auto& file : buildFiles )
    {
        // Skip the submodule's own build system: it is somebody else's repository and not ours to hold
        // to this rule.
        if ( file.generic_string().find( "ThirdParty/" ) != std::string::npos )
        {
            continue;
        }
        // PER LINE, AND COMMENTS ARE EXEMPT. The first version read the whole file as one string and
        // fired on this change's own prose — the comments that explain what `assimp-vc142-mtd` WAS are
        // the reason the removal is reviewable, and a census that forbids describing the past forces the
        // next person to delete the history instead of reading it. What is forbidden is an executable
        // line.
        std::istringstream lines( ReadFile( root / file ) );
        std::string        line;
        int                lineNumber = 0;
        while ( std::getline( lines, line ) )
        {
            ++lineNumber;
            const bool isComment = line.find( "--" ) != std::string::npos ||
                                   line.find( "REM " ) != std::string::npos ||
                                   line.find( '#' ) != std::string::npos;
            if ( isComment )
            {
                continue;
            }
            for ( const char* toolset : { "vc142", "vc143", "vc141" } )
            {
                const std::size_t at = line.find( toolset );
                if ( at == std::string::npos )
                {
                    continue;
                }
                // A deliberate `toolset "v143"` is spelled with a leading v and nothing dash-joined
                // around it; what this refuses is the toolset appearing INSIDE an identifier such as
                // `assimp-vc142-mtd`. The test for that is a '-' immediately before it.
                const bool insideALibraryName = at > 0 && line[at - 1] == '-';
                EXPECT_FALSE( insideALibraryName )
                     << file.generic_string() << ":" << lineNumber << " contains '" << toolset
                     << "' inside a library name: '" << line
                     << "'. That is how `assimp-vc142-mtd` came to be linked by a workspace pinned to "
                        "v143 — updating the dependency then breaks a line that does not mention it. "
                        "Link the PROJECT, not a file.";
            }
        }
    }
}

// П7 AND П8, WHICH ARE ONE STATEMENT ABOUT THE PACKAGING SCRIPTS SEEN FROM TWO SIDES: after static
// linking there is no assimp runtime, so nothing may CLAIM there is one and nothing may try to COPY one.
// The second half is what catches "linked it statically and forgot to delete the copier" — and the
// copier that existed was invisible, because `if exist` turns a missing file into a silent success.
TEST( AssimpBoundary, NoPackagingOrBuildStepShipsAnAssimpRuntime )
{
    const auto root = RepositoryRoot();

    std::vector<std::filesystem::path> scripts;
    for ( const char* dir : { "scripts", "Tools", "BuildScripts", "Editor", ".github" } )
    {
        for ( const auto& file : FilesUnder( root / dir, { ".bat", ".sh", ".ps1", ".lua", ".yml" } ) )
        {
            scripts.push_back( file );
        }
    }
    ASSERT_FALSE( scripts.empty() );

    for ( const auto& file : scripts )
    {
        if ( file.generic_string().find( "ThirdParty/" ) != std::string::npos )
        {
            continue;
        }
        const std::string text = ReadFile( root / file );

        // A copy step naming an assimp binary. `assimp` plus `.dll` on one line is the shape; the
        // comment in this very suite's header explains why a guarded copy is worse than none.
        std::istringstream lines( text );
        std::string        line;
        int                lineNumber = 0;
        while ( std::getline( lines, line ) )
        {
            ++lineNumber;
            const bool isComment = line.find( "REM " ) != std::string::npos ||
                                   line.find( "--" ) != std::string::npos || line.find( '#' ) != std::string::npos;
            if ( isComment )
            {
                continue; // prose may DISCUSS the old DLL; only an executable step is forbidden
            }
            const bool namesAssimp =
                 line.find( "assimp" ) != std::string::npos || line.find( "Assimp" ) != std::string::npos;
            const bool namesRuntime = line.find( ".dll" ) != std::string::npos ||
                                      line.find( ".dylib" ) != std::string::npos ||
                                      line.find( ".so" ) != std::string::npos;
            EXPECT_FALSE( namesAssimp && namesRuntime )
                 << file.generic_string() << ":" << lineNumber << " still moves an assimp shared library: '"
                 << line
                 << "'. assimp is linked statically now, so there is no such file — and the step this "
                    "replaces was guarded by `if exist`, which means it did nothing on every Windows "
                    "build for months and said so nowhere.";
        }
    }

    // П7's own half: NO COMMENT MAY STILL CLAIM ASSIMP IS A DLL, because it is not one since D40.
    //
    // THIS CHECK WAS ONE LITERAL PHRASE AND THAT WAS NOT ENOUGH. It looked for the exact sentence
    // "assimp arrives as a DLL" in Package.bat, passed, and MISSED A SECOND FALSE SENTENCE IN THE SAME
    // FILE eleven lines away — "Assimp is the one third-party dependency that ships as a DLL" — sitting
    // above the loop that copies DLLs into the drop. Pinning a phrase catches the instance; what has to
    // be caught is the CLAIM. So the rule is: a comment mentioning assimp may not also assert, in the
    // present tense, that it is a shared library. Past tense is deliberately allowed — the notes
    // explaining what used to be true are why this change is reviewable, and a rule that forbade them
    // would force the next person to delete the history instead of reading it.
    for ( const char* script :
          { "scripts/Windows/Package.bat", "scripts/Windows/Setup.bat", "scripts/Windows/BuildWindows.bat" } )
    {
        const std::string text = ReadFile( root / script );
        ASSERT_FALSE( text.empty() ) << script << " is missing; this half of П7 covered nothing.";

        std::istringstream lines( text );
        std::string        line;
        int                lineNumber = 0;
        while ( std::getline( lines, line ) )
        {
            ++lineNumber;
            std::string lowered = line;
            std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            if ( lowered.find( "assimp" ) == std::string::npos )
            {
                continue;
            }
            for ( const char* claim :
                  { "arrives as a dll", "ships as a dll", "comes as a dll", "is a dll", "as a shared library" } )
            {
                EXPECT_EQ( lowered.find( claim ), std::string::npos )
                     << script << ":" << lineNumber << " still claims assimp is a shared library: '" << line
                     << "'. It is compiled from a pinned submodule INTO the executable since D40. A "
                        "comment promising a guarantee the tree does not give is a defect class this "
                        "project has closed twelve times; write what is true, in the present tense, and "
                        "keep the old sentence only in the past tense if it explains something.";
            }
        }
    }
}

// A `REM` INSIDE A `for ... in ( ... )` LIST IS NOT A COMMENT — IT IS AN ITEM. This is the check that
// would have caught the defect that made this suite grow.
//
// MEASURED, ON CI, BY FAILING. Three `REM` lines were written into the middle of Setup.bat's
// `for %%P in ( ... )` path list to explain why the entry beside them had changed. The list is DATA, so
// the loop iterated the words of the comment as if they were paths — the log reads "- REM", "- tree",
// "- not", "- the", "- two", "- prebuilt" — and Setup.bat exited 1 on BOTH Windows configurations while
// every macOS job in the same run stayed green.
//
// WHY IT IS A CENSUS AND NOT A LESSON. This repository has already paid for this class once: `echo set
// ERROR=0>> file` lost its digit to cmd's redirection parsing and the Windows test runner could not
// report a failure AT ALL until 2026-08-15. Both defects are invisible from macOS by construction, so
// the answer cannot be "be careful with batch files" — it has to be something that fails here, on the
// platform the developer is standing on.
//
// AND THE RULE IS NARROW ON PURPOSE, BECAUSE THE FIRST VERSION OF IT WAS NOT. It flagged any `REM`
// nested inside any parentheses, and firing it at this repository — not at a mutation — immediately
// reddened eleven lines of `scripts\Windows\BuildWindows.bat` that are CORRECT: inside a
// parenthesised COMMAND BLOCK (`if not exist ... ( ... )`) `rem` is an ordinary command and works as a
// comment, which is why that script has been green on Windows all along. A gate that condemns working
// code is worse than no gate — someone would have rewritten a correct script to satisfy it. Only the
// `in ( ... )` LIST of a `for` is data, so only that is refused.
TEST( AssimpBoundary, NoBatchScriptPutsAcommentInsideAForInList )
{
    const auto root = RepositoryRoot();

    const auto batchFiles = FilesUnder( root / "scripts", { ".bat" } );
    ASSERT_FALSE( batchFiles.empty() ) << "no .bat files were found, so this census covered nothing.";

    int listsInspected = 0;
    for ( const auto& file : batchFiles )
    {
        std::istringstream lines( ReadFile( root / file ) );
        std::string        line;
        int                lineNumber = 0;
        bool               inList     = false;

        while ( std::getline( lines, line ) )
        {
            ++lineNumber;

            std::string lowered = line;
            std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );

            if ( !inList )
            {
                // `for %%X in (` with nothing closing it on the same line opens a multi-line list.
                const std::size_t forAt = lowered.find( "for " );
                const std::size_t inAt  = lowered.find( " in (" );
                if ( forAt != std::string::npos && inAt != std::string::npos && forAt < inAt &&
                     lowered.find( ')', inAt ) == std::string::npos )
                {
                    inList = true;
                    ++listsInspected;
                }
                continue;
            }

            // Inside the list: every word is an item. `)` ends it (the `) do (` line).
            const std::size_t firstCharacter = lowered.find_first_not_of( " \t" );
            if ( firstCharacter != std::string::npos )
            {
                const std::string head = lowered.substr( firstCharacter, 4 );
                EXPECT_FALSE( head == "rem " || head == "rem" )
                     << file.generic_string() << ":" << lineNumber
                     << " puts a `REM` inside a `for ... in ( ... )` list: '" << line
                     << "'. cmd does not strip it — every word becomes an ITEM, so the loop runs over "
                        "\"REM\", \"tree\", \"not\", \"the\" ... Move the prose above the `for`. "
                        "Measured: exactly this failed both Windows configurations while macOS stayed "
                        "green.";
            }
            if ( lowered.find( ')' ) != std::string::npos )
            {
                inList = false;
            }
        }
    }

    EXPECT_GT( listsInspected, 0 )
         << "no multi-line `for ... in ( ... )` list was found in any .bat file, so this census examined "
            "nothing. Setup.bat has one; if it moved, follow it rather than deleting this.";
}

// THE REGISTER IS ROWS, AND THE ROWS HAVE REASONS. A register whose rows carry no argument decays into
// a list somebody edits to make a gate pass — which is the failure mode of pinning a COUNT, one level
// up. The extensions themselves are checked against the built library in AssimpLibraryPin; what is
// checked here is that the file remains a register rather than becoming a bare list.
TEST( AssimpBoundary, TheImporterRegisterGivesEveryFormatAReason )
{
    const auto        root = RepositoryRoot();
    const std::string text = ReadFile( root / "BuildScripts" / "ThirdParty" / "AssimpImporters.txt" );
    ASSERT_FALSE( text.empty() ) << "BuildScripts/ThirdParty/AssimpImporters.txt is missing. It is what "
                                    "Assimp.lua derives the disable list from; without it the build "
                                    "would ship every format assimp has.";

    std::istringstream lines( text );
    std::string        line;
    int                rows = 0;
    while ( std::getline( lines, line ) )
    {
        if ( line.empty() || line[0] == '#' )
        {
            continue;
        }
        ++rows;
        // NAME, extensions, then prose. Three fields minimum, and the third has to be a sentence rather
        // than a word: "because we need it" is the shape this is meant to prevent.
        std::istringstream       fields( line );
        std::string              name;
        std::string              word;
        std::vector<std::string> words;
        fields >> name;
        while ( fields >> word )
        {
            words.push_back( word );
        }
        EXPECT_FALSE( name.empty() );
        EXPECT_GE( words.size(), 6u )
             << "row '" << name
             << "' in AssimpImporters.txt has no reason worth the name. Every format this engine ships "
                "is a parser in the editor's address space; the row is where the argument for it lives.";
    }
    EXPECT_GE( rows, 1 ) << "the register names no importers at all.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
