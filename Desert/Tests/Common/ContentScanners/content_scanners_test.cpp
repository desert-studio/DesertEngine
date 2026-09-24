// EVERY SCANNER THAT ENUMERATES CONTENT GOES THROUGH ONE ENUMERATION — now checked, not merely written.
//
// `Common::Utils::FileSystem::ListFilesRecursive` returns BOTH halves of the content world: the loose
// files on disk and everything a mounted `.dpak` holds under the same root. Its own header states the
// rule and the reason: "every scanner that enumerates content must go through this: the font and icon
// services each used to walk only the disk half, so a packaged game — where the loose directories do not
// exist at all — scanned nothing and no text could resolve its font."
//
// That sentence was a COMMENT, and a comment cannot see a second offender arrive. Two did.
//
//   * `EditorLayer::CollectAvailableScenes` walked the scenes root itself, so a packaged project had no
//     levels in the Open Scene popup, in the palette's Scene group, or on the control channel;
//   * `BuildSettingsPanel::RescanScenes` did the same over the whole content root — the THIRD list of
//     this project's levels — and that one chooses which scenes go INTO a package. It would have shipped
//     a game with no levels in it, successfully.
//
// Two instances of one shape is this project's own bar for making a rule enforceable, so here it is.
//
// ── AND THE WORST ROW TURNED OUT NOT TO BE ABOUT ENUMERATION AT ALL ────────────────────────────────
//
// I8 took the four rows A6-2 left. Three were the plain defect and are fixed. The fourth — the Lua
// script picker — walked the literal relative path "Resources" from the PROCESS's working directory,
// and following that thread found something the register could not have said: the six example scripts
// it offered lived in the ENGINE resource tree (Resources/Scripts/Examples), which is not one of the
// five trees PackagedContentTrees() builds an archive from. So every .lua this editor has ever offered
// was UNPACKAGEABLE, three committed scenes named one, and a packaged game would have loaded none of
// them. The picker's bad path was not sloppiness beside the real defect; it was the only thing making
// a folder outside the project's content look like content. The scripts were moved under the assets
// root by the same change, which is what ContentDir::Script has meant all along.

//
// ── WHAT THE REGISTER SAYS, AND WHY IT HAS TWO KINDS OF ROW ────────────────────────────────────────
//
// Not every directory walk is a content scanner, and a census that pretended otherwise would be noise
// nobody could act on. A user's `~/.desertengine`, a machine-local thumbnail cache, the packager reading
// the tree it is about to pack, an importer opening `.fbx` files that are never shipped, and the runtime
// looking for the `.dpak` files themselves — none of those can go through a mount, and two of them
// (the packager, the pak discovery) would be circular if they did.
//
// So a row is either NOT CONTENT, with the reason, or it is DEBT, with an owner. The debt rows are real
// defects that are latent today because the editor does not yet mount a pak; they are listed rather than
// fixed because they live in other people's files, and a register with owners is how this codebase
// carries that (SettingConsumers does exactly the same and for the same reason).
//
// ── WHAT THIS SUITE ACTUALLY ENFORCES ──────────────────────────────────────────────────────────────
//
//   1. NO UNREGISTERED WALK. A file that starts walking a directory itself must be argued for here.
//      This is the direction that matters: it is how a third `CollectAvailableScenes` gets caught the
//      day it is written rather than the day somebody packages the game.
//   2. NO STALE ROW. A row whose file no longer walks anything is removed, so the register describes the
//      tree instead of the tree of some past afternoon.
//   3. THE DEBT DOES NOT GROW. Its size is pinned. A new content scanner cannot be filed as debt to make
//      this suite pass — adding a row to that list is a deliberate, visible act.
//   4. EVERY DEBT ROW NAMES AN OWNER, because an exception with nobody's name on it is unreadable in a
//      month.
//
// TOOLS/ ARE OUT OF SCOPE, and that is one decision rather than ten rows. `PakTool`, `DShaderTool`,
// `SceneMigrator`, `ProjectHub` and the rest are offline programs over a source tree; there is no mount
// in the process and no packaged case for them to be wrong about. Their walks are not content scanning.

// The comment-and-literal blanker is SHARED with the settings census rather than rewritten here, and
// the sharing is not thrift. This suite's first run failed on the two files it had just FIXED, because
// their new comments say "this used to be a raw recursive_directory_iterator" — a checker that reads
// comments as code is the same "a comment is not the code" defect, inverted, and the blanker is where
// that lesson is already paid for (its own header lists a character literal that ate hundreds of lines).
#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // ── THE REGISTER ────────────────────────────────────────────────────────────────────────────────

    enum class Verdict
    {
        TheOneEnumeration, ///< the implementation every other scanner is supposed to call
        NotContent,        ///< it walks something that is not this project's content
        Debt,              ///< it walks content the raw way, and somebody owes the fix
    };

    struct ScannerRow
    {
        const char* File;
        Verdict     What;
        // Why it is not content, or — for a debt row — WHAT it walks. Never empty: a row nobody can read
        // is a row that gets copied for the wrong reason.
        const char* Reason;
        // Debt rows only. Empty for the others.
        const char* Owner;
    };

    // Filed by A6-2 with four unassigned rows. I8 took all four and answered each with a NUMBER, taken
    // from the real assets tree packed into a real .dpak and mounted with nothing loose behind it
    // (249 files in the archive, 0 on disk). What a raw walk returns in that project, against what the
    // shared enumeration returns:
    //
    //   AssetReferencesScan           every file under the assets root     0  vs 249   -> FIXED
    //   ComponentEditorRegistrations  .lua the script picker offers        0  vs   6   -> FIXED
    //   NodeGraphPanel                .dgraph the Load popup offers        0  vs   5   -> GONE (the popup
    //                                                                                    itself is gone;
    //                                                                                    see the test below)
    //   FileExplorerPanel             immediate children of the root       0  vs   8   -> MEASURED REFUSAL
    //
    // THE ONE THAT STAYS, and why it is not laziness. Swapping the browser's walk would make it LIST
    // eight folders it can then do nothing with: beside the two enumerations this file makes 24 further
    // filesystem calls, and every one of them is disk-only — 5 is_directory, 5 exists, 5 absolute,
    // 2 last_write_time, 1 file_size and 1 status ask questions no pak entry answers, and
    // 3 create_directories, 2 create_directory, 1 copy_file and 2 permissions WRITE. A browser whose
    // rows cannot be sized, dated, opened, renamed, moved or deleted is the §1.4 shape one level up: a
    // listing that looks complete while every operation on it fails. The fix is the panel's disk-shaped
    // MODEL, not its walk, and that is a task rather than a line.
    constexpr const char* kBrowserOwner = "I8 measured the refusal (0 vs 8 with a pak mounted); the fix is "
                                          "the panel's disk-shaped model and needs a task of its own";

    constexpr ScannerRow kScanners[] = {
         // ── the one implementation ──────────────────────────────────────────────────────────────────
         { "Desert/Common/Source/Common/Utilities/FileSystem.cpp", Verdict::TheOneEnumeration,
           "ListFilesRecursive itself: the disk half of the content world, merged with the pak half.", "" },

         // ── not content ─────────────────────────────────────────────────────────────────────────────
         { "Desert/Common/Source/Common/Utilities/ContentManifest.cpp", Verdict::NotContent,
           "hashes a SOURCE tree to produce a release artifact. The manifest of a mounted archive comes "
           "from FromPak, which reads the index instead of the bytes.",
           "" },
         { "Desert/Desert/Source/Engine/Project/StartupLayout.cpp", Verdict::NotContent,
           "looks beside THIS EXECUTABLE for the one .deproj a drop carries, before any project is open "
           "and therefore before any content exists to enumerate. It answers 'which project am I', not "
           "'what content is there' - and it must not go through a mount, because nothing is mounted yet.",
           "" },
         { "Editor/Source/Editor/Core/CrashRecovery.cpp", Verdict::NotContent,
           "the user's own ~/.desertengine session directory. Never packaged, never a project's content.", "" },
         { "Editor/Source/Editor/Core/LayoutManager.cpp", Verdict::NotContent,
           "~/.desertengine/Layouts - one person's saved window layouts, per the config-ownership rule.", "" },
         { "Editor/Source/Editor/Packaging/GamePackager.cpp", Verdict::NotContent,
           "reads the source tree it is about to PACK. Reading it through a mount would be circular: the "
           "packager would pack the archive into itself.",
           "" },
         { "Editor/Source/Editor/Packaging/PackageCook.cpp", Verdict::NotContent,
           "walks the local DerivedDataCache buckets it stages into Saved/Cooked for the package - a "
           "machine-local cache, never mounted content.",
           "" },
         { "Editor/Source/Editor/Import/Blend/BlendImporter.hpp", Verdict::NotContent,
           "source art (.blend) that exists only in an authoring tree and is never shipped.", "" },
         { "Editor/Source/Editor/Import/ImportManager.cpp", Verdict::NotContent,
           "source art (.fbx and friends) awaiting cook. A packaged game imports nothing.", "" },
         { "Editor/Source/Editor/Import/MeshDnD.cpp", Verdict::NotContent, "same: source art, pre-cook.", "" },
         { "Editor/Source/Editor/Import/TextureImporter.cpp", Verdict::NotContent,
           "source IMAGES awaiting cook (LooseTextureSources) - the editor's startup cook and the packager's. "
           "A packaged game decodes no image; it reads the .tex this walk produces through the registry.",
           "" },
         { "Editor/Source/Editor/Import/MeshMaterial.cpp", Verdict::NotContent,
           "same: textures beside a source mesh, resolved during import.", "" },
         { "Editor/Source/Editor/Panels/Collections/CollectionsPanel.cpp", Verdict::NotContent,
           "enumerates installed collection FOLDERS, not files - it looks for directories that contain a "
           "collection.json. The shared enumeration returns files and cannot answer that question.",
           "" },
         { "Editor/Source/Editor/Widgets/ThumbnailCache.cpp", Verdict::NotContent,
           "Cooked/Thumbnails, a machine-local cache this function also DELETES from. Not content, and "
           "nothing a package contains.",
           "" },
         { "Runtime/Source/PackagedContent.cpp", Verdict::NotContent,
           "finds the .dpak files THEMSELVES. It cannot go through the mount it is about to create.", "" },

         // ── debt: these really do walk this project's content ───────────────────────────────────────
         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp", Verdict::Debt,
           "the content browser itself - the editor's own window onto the content world, showing only "
           "the loose half of it. Measured at 0 rows against 8 with a pak mounted; see kBrowserOwner "
           "above for why swapping the walk alone would make it worse rather than better.",
           kBrowserOwner },
    };

    // PINNED. Point 3: a new content scanner must not be able to make this suite pass by joining the
    // debt list. Moving one to NotContent, or fixing it away, is what makes this number go DOWN.
    //
    // 4 -> 1: I8 routed AssetReferencesScan, NodeGraphPanel and the script picker in
    // ComponentEditorRegistrations through the shared enumeration, so their rows are gone rather than
    // reworded — a register describes the tree, and those three files no longer walk anything. The graph
    // panel then stopped walking in any sense at all: `.dgraph` became an asset, the panel became a
    // document over one of them, and its file list went with the tool.
    constexpr std::size_t kDebtRowCount = 1;

    // ── FINDING THE TREE AND READING IT ─────────────────────────────────────────────────────────────

    // Walk up from wherever the binary was started, exactly as SettingConsumers and the font-baker
    // suite do, so this need not be run from one precise directory.
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
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Does this file construct a directory iterator, IN CODE?
    //
    // COMMENTS AND LITERALS ARE BLANKED FIRST, and that is not a refinement — it is what makes the
    // answer mean anything. Written without it, this suite failed on the two files A6-2 had just fixed:
    // both carry a comment saying "this used to be a raw recursive_directory_iterator", and the checker
    // read the sentence about the defect as the defect. A census that cannot tell a description of a
    // thing from the thing is the "a comment is not the code" failure with the reader on the wrong side.
    bool WalksADirectory( const std::string& source )
    {
        const std::string code = Desert::Tests::ConsumerText::StripCommentsAndLiterals( source );
        return code.find( "directory_iterator" ) != std::string::npos;
    }

    // The roots that hold code which RUNS - an editing session or a shipped game. Tools/ is excluded by
    // the argument at the top of this file.
    const std::vector<std::string>& ScannedRoots()
    {
        static const std::vector<std::string> roots = {
             "Editor/Source",
             "Runtime/Source",
             "Desert/Desert/Source",
             "Desert/Common/Source",
        };
        return roots;
    }

    std::vector<std::string> FilesThatWalkADirectory( const std::string& root )
    {
        std::vector<std::string> found;
        std::error_code          ec;
        for ( auto it = std::filesystem::recursive_directory_iterator( root + "/", ec );
              it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( ec )
                break;
            if ( !it->is_regular_file( ec ) )
                continue;

            const std::string extension = it->path().extension().string();
            if ( extension != ".cpp" && extension != ".hpp" )
                continue;

            if ( WalksADirectory( ReadFile( it->path().string() ) ) )
                found.push_back( it->path().generic_string() );
        }
        return found;
    }

    // Everything under the scanned roots that walks a directory, as repo-relative generic paths.
    std::vector<std::string> EveryWalkerInTheTree( const std::string& root )
    {
        std::vector<std::string> all;
        for ( const std::string& sub : ScannedRoots() )
        {
            for ( std::string path : FilesThatWalkADirectory( root + sub ) )
            {
                // Strip the discovered prefix so the result is comparable with the register's rows.
                const std::size_t at = path.find( sub );
                if ( at != std::string::npos )
                    path = path.substr( at );
                all.push_back( path );
            }
        }
        std::sort( all.begin(), all.end() );
        return all;
    }
} // namespace

// The suite is worthless if it cannot see the tree, and "saw nothing" would otherwise read as "nothing
// walks a directory" - the §1.4 shape applied to a checker. Asserted first and separately.
TEST( ContentScanners, TheSuiteCanSeeTheRepository )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";
    EXPECT_FALSE( ReadFile( root + "Desert/Common/Source/Common/Utilities/FileSystem.cpp" ).empty() );
    EXPECT_FALSE( EveryWalkerInTheTree( root ).empty() ) << "no file in the whole tree walks a directory, "
                                                            "which cannot be true while the shared "
                                                            "enumeration is itself one";
}

// ── 1. No unregistered walk ────────────────────────────────────────────────────────────────────────
//
// THE DIRECTION THAT MATTERS. A file that starts walking the content root itself is exactly the defect
// this rule exists for, and it arrived twice while the rule was only a sentence in a header.
TEST( ContentScanners, EveryFileThatWalksADirectoryIsInTheRegister )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::set<std::string> registered;
    for ( const ScannerRow& row : kScanners )
        registered.insert( row.File );

    for ( const std::string& walker : EveryWalkerInTheTree( root ) )
    {
        EXPECT_EQ( registered.count( walker ), 1u )
             << walker
             << " walks a directory itself and is not in the register.\n"
                "If it enumerates this PROJECT'S CONTENT it must call "
                "Common::Utils::FileSystem::ListFilesRecursive, which sees a mounted .dpak as well as "
                "loose files; a packaged project's directories do not exist.\n"
                "If it walks something else - a user directory, a source tree awaiting import, a cache - "
                "add a NotContent row saying which.";
    }
}

// ── 2. No stale row ────────────────────────────────────────────────────────────────────────────────
TEST( ContentScanners, EveryRegisteredFileStillWalksADirectory )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const ScannerRow& row : kScanners )
    {
        const std::string source = ReadFile( root + row.File );
        ASSERT_FALSE( source.empty() ) << row.File
                                       << " is in the register and could not be read; it has "
                                          "been moved or deleted and its row is stale.";
        EXPECT_TRUE( WalksADirectory( source ) )
             << row.File
             << " no longer walks a directory. Delete its row - a register that describes a "
                "past afternoon is worse than none, because it is read as current.";
    }
}

// ── 3. The debt does not grow ──────────────────────────────────────────────────────────────────────
TEST( ContentScanners, TheDebtIsExactlyWhatWasFiledAndNoMore )
{
    std::size_t debt = 0;
    for ( const ScannerRow& row : kScanners )
        if ( row.What == Verdict::Debt )
            ++debt;

    EXPECT_EQ( debt, kDebtRowCount )
         << "the debt register changed size. Going DOWN is the point and the number moves with it. Going "
            "UP means a new content scanner was filed as debt instead of calling ListFilesRecursive, and "
            "that is the thing this suite exists to make somebody argue for out loud.";
}

// ── 4. Every exception is readable and owned ───────────────────────────────────────────────────────
TEST( ContentScanners, EveryRowSaysWhyAndEveryDebtRowSaysWho )
{
    for ( const ScannerRow& row : kScanners )
    {
        EXPECT_STRNE( row.Reason, "" ) << row.File
                                       << " has no reason; a row nobody can read gets copied "
                                          "for the wrong reason.";

        if ( row.What == Verdict::Debt )
        {
            EXPECT_STRNE( row.Owner, "" )
                 << row.File
                 << " is filed as debt with nobody's name on it, which is unreadable in a "
                    "month (SettingConsumers refuses the same shape).";
        }
        else
        {
            EXPECT_STREQ( row.Owner, "" ) << row.File
                                          << " is not debt and yet names an owner; the two "
                                             "columns would stop meaning what they say.";
        }
    }
}

// THE ONE IMPLEMENTATION IS EXACTLY ONE. Two files answering "what content is there" is the shape the
// whole rule is about, so the register may not contain a second row claiming to be it.
TEST( ContentScanners, ThereIsExactlyOneSharedEnumeration )
{
    std::size_t implementations = 0;
    for ( const ScannerRow& row : kScanners )
        if ( row.What == Verdict::TheOneEnumeration )
            ++implementations;

    EXPECT_EQ( implementations, 1u );
}

// AND THE TWO LISTS A6-2 FIXED STAY FIXED. A regression witness, named: these two files walked the
// content root themselves, and one of them decided which scenes went into a package.
TEST( ContentScanners, TheTwoSceneListsGoThroughTheSharedEnumeration )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* file :
          { "Editor/Source/EditorLayer.cpp", "Editor/Source/Editor/Panels/Build/BuildSettingsPanel.cpp" } )
    {
        const std::string source = ReadFile( root + file );
        ASSERT_FALSE( source.empty() ) << file;
        EXPECT_NE( source.find( "ListFilesRecursive" ), std::string::npos )
             << file << " stopped using the shared enumeration; a packaged project would lose its levels.";
    }
}

// AND THE ONES I8 FIXED STAY FIXED, for the same reason and in the same shape as the witness above.
// Deleting a row is what "fixed" means here, so without this test the register would forget these files
// existed and a reverted call site would only be noticed by whoever packaged the game.
//
// NodeGraphPanel.cpp WAS THE THIRD OF THEM AND IS GONE FROM THIS LIST, which is a stronger outcome rather
// than a weaker one: the `Load ▾` popup it enumerated `.dgraph` files for does not exist any more. A
// `.dgraph` is an asset and its window is a document, so opening one is the browser's double-click or the
// palette's Open group — and that group is built from OpenableAssets, which walks ASSETS_PATH through this
// same shared enumeration (EditorLayer.cpp). The panel now walks nothing at all, so there is no call site
// here left to revert.
TEST( ContentScanners, TheScannersI8FixedGoThroughTheSharedEnumeration )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* file : { "Editor/Source/Editor/Core/AssetReferencesScan.cpp",
                               "Editor/Source/Editor/Panels/SceneProperties/ComponentEditorRegistrations.cpp" } )
    {
        const std::string source = ReadFile( root + file );
        ASSERT_FALSE( source.empty() ) << file;
        EXPECT_NE( Desert::Tests::ConsumerText::StripCommentsAndLiterals( source ).find( "ListFilesRecursive" ),
                   std::string::npos )
             << file
             << " stopped using the shared enumeration. Measured with a mounted pak and no loose files, a "
                "raw walk returns 0 where this one returns 249 / 6 respectively.";
    }
}

// THE SCRIPT PICKER LOOKS UNDER THE CENSUS ROW FOR SCRIPTS, not under a path of its own. This is the
// SECOND half of that row's defect and the enumeration check above cannot see it: a file can call
// ListFilesRecursive and still hand it a literal spelled against the working directory, which is what
// stood here and what four other defects in this engine have been.
TEST( ContentScanners, TheScriptPickerAsksThePathCensusWhereScriptsAre )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::string code = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadFile( root + "Editor/Source/Editor/Panels/SceneProperties/ComponentEditorRegistrations.cpp" ) );
    ASSERT_FALSE( code.empty() );

    // THE TWO NAMES TOGETHER, IN ONE CALL — not each of them somewhere in the file. Written as two
    // separate searches this test PASSED against a mutation that put the raw literal walk back, because
    // SCRIPT_PATH was still named a few lines further down in the empty-list message. Both sides were
    // individually present and the thing they had to say about each other was gone: §4's shape, in the
    // checker rather than in the engine.
    std::erase_if( code, []( unsigned char c ) { return std::isspace( c ) != 0; } );

    EXPECT_NE( code.find( "ListFilesRecursive(Common::Constants::Path::SCRIPT_PATH)" ), std::string::npos )
         << "the script picker must enumerate the scripts root the PATH CENSUS names, in one call: "
            "Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SCRIPT_PATH ). The "
            "census row follows SetProjectRoot; a literal is resolved against the process's working "
            "directory and does not — measured with a second project open, the literal offered 6 scripts "
            "belonging to the project that was NOT open and 0 belonging to the one that was.";
}

// A SCRIPT THE EDITOR NAMES IS A SCRIPT THAT EXISTS.
//
// The defect this closes was one literal: the New Project template attached SCRIPT_PATH joined to a
// lower-case, underscored player-controller file name, and no tree of this project has ever held a
// file spelled that way. (The old spelling is deliberately not quoted anywhere in this file: this test
// reads RAW source, so writing it out here would make the suite fail on its own description — the
// inverted comment-is-not-the-code trap the blanker exists for, in the one test that cannot use it.) The slot was
// created broken, every time, and said nothing until somebody pressed Play and read the log. Both halves were
// individually plausible — the join is through the path census, and the file name looks like the file that is
// there — and the thing they had to say about each other was never checked, which is this project's most-repeated
// defect shape.
//
// Deliberately over the RAW source: the blanker's whole job is to remove literals, and a literal is
// exactly what this test is about. Extension-only spellings (".lua") carry no file name and are skipped.
TEST( ContentScanners, EveryLuaFileNamedInTheEditorExists )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // SCRIPT_PATH is relative to the EDITOR'S working directory, which is Editor/ — that is what
    // RunEditor.sh cds to and what makes "Resources/..." resolve at all. From the repository root the
    // same file therefore sits one level deeper.
    const std::string scriptsRoot = "Editor/" + Common::Constants::Path::SCRIPT_PATH.generic_string();

    std::size_t checked = 0;
    for ( auto it = std::filesystem::recursive_directory_iterator( root + "Editor/Source" );
          it != std::filesystem::recursive_directory_iterator(); ++it )
    {
        if ( !it->is_regular_file() || it->path().extension() != ".cpp" )
            continue;

        const std::string source = ReadFile( it->path().string() );
        for ( std::size_t at = source.find( ".lua\"" ); at != std::string::npos;
              at             = source.find( ".lua\"", at + 1 ) )
        {
            const std::size_t open = source.rfind( '"', at );
            if ( open == std::string::npos )
                continue;
            const std::string named = source.substr( open + 1, at + 4 - open - 1 );
            if ( named == ".lua" || named.find( '\n' ) != std::string::npos )
                continue; // an extension test, not a file name

            ++checked;
            EXPECT_TRUE( std::filesystem::exists( root + scriptsRoot + named ) )
                 << it->path().filename().string() << " names the script '" << named
                 << "', and no such file exists under " << scriptsRoot
                 << ". A slot pointing at a script that is not there is created silently and only "
                    "reports itself on Play.";
        }
    }

    EXPECT_GT( checked, 0u ) << "no .lua file name was found in the editor's sources, which cannot be "
                                "true while the New Project template attaches one - the scan is broken, "
                                "not the tree";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
