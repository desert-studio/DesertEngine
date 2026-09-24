// THE COOK GATE — "on disk" stopped meaning "shipped", so this is what fails the build instead.
//
// ── WHY IT EXISTS, AND WHY IT EXISTS *NOW* ────────────────────────────────────────────────────────
//
// GAP_ANALYSIS T2.7 says a cook gate is needed "the same day T2.6 lands", and the author of the lazy
// loading slice wrote down exactly why it was not needed yet: he had LEFT the directory walk in place,
// so the walk still minted every handle and registered every shell, and the packager saw exactly what
// it had always seen. His sentence — "the danger opens together with T2.4, when the walk disappears".
//
// The walk has disappeared. Both hosts now boot from `Cooked/AssetRegistry.dreg` and neither scans a
// directory, so a content file that has no row in it is a file the engine does not have: it is not
// preloaded, it is not offered in a picker, and — the part that actually costs money — it is not in a
// packaged build, while the editor session that authored it looked completely normal.
//
// ── THE PRECEDENT, WHICH IS THE ARGUMENT FOR MAKING THIS ERROR RATHER THAN WARN ───────────────────
//
// `PackagedContentTrees.hpp` exists because a hand-typed sequence of `AddTreeToPak` calls forgot fonts
// and icons. `Constants.hpp` declared FONTS_PATH and ICONS_PATH, the runtime services scanned them,
// and the packager packed three other trees — so a built game contained NOT ONE `.ttf` and the first
// frame with text died. No test saw it, because the only thing that would have was packaging.
//
// That is this defect with a different list, so it gets the same answer the packager got: a relation
// asserted over the CONTENT TREE, not over a schema somebody maintains.
//
// ── WHAT IT COMPARES AGAINST, AND WHY IT IS NOT THE DISK ──────────────────────────────────────────
//
// THE FIRST VERSION OF THIS GATE READ THE WORKING DIRECTORY AND THAT WAS WRONG. The project registry
// is a COMMITTED file — a claim about what the repository carries — and the disk is a claim about one
// machine. On the integrator's checkout the two disagreed ten times, and exactly ONE of those ten was
// a file git tracks; the other nine were his own cook's output under `Editor/Cooked/`, which
// `.gitignore` excludes on purpose. So the gate was red over content no other clone has, and the
// remedy it printed — re-cook and commit — would have written HIS machine's state into the shared
// file and turned the gate red for everybody else.
//
// Measured from a fresh checkout of `dev` while fixing it: the committed registry carried ten rows for
// files a clean clone does not contain, all ten produced by the machine that cooked it.
//
// It was the second instance of the shape in one day — a suite that depended on two fixtures nobody
// had committed was the first, and it was certified 257/257 green by an author who had the files on
// disk. So the rule is written down rather than applied quietly:
//
//     AN INSTRUMENT MUST READ THE DISK ONLY WHEN THE QUESTION IS ABOUT THIS MACHINE.
//
// This gate therefore compares the project registry against `Common::Content::TrackedContent`, which
// is `git ls-files` filtered through the same content census the disk scan uses. Both directions: a
// tracked file with no row does not reach a packaged build, and a row with no tracked file sends the
// loader after something no clone has.
//
// THE COMPARISON ITSELF is `Common::Content::Compare`, shared with the cook, the standalone
// tool and the packager — a copy per caller is the shape this whole task is about ending. Only the
// LIST differs, and that is the part that has to differ.
//
// ── WHAT IS DELIBERATELY NOT ASSERTED ─────────────────────────────────────────────────────────────
//
// The DECLARED IDENTITY column and the DEPENDENCY EDGES. Both can only be read by parsing an asset
// with the class that owns its format, which is the engine, which this suite does not link — and a
// gate that cannot compute a value must not judge it. They converge on the editor's own post-boot
// cook; what this gate holds is the property a missing row destroys.

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
    // The suite runs from build/Bin/Tests/<cfg>, so the repository is some way up. Probed by a file
    // that can only be this repository's.
    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 8; ++up )
        {
            if ( fs::exists( prefix / "Editor" / "Desert.deproj" ) )
                return fs::absolute( prefix ).lexically_normal();
            prefix /= "..";
        }
        return {};
    }

    // Opens the sandbox project the way the editor opens it, INCLUDING the working directory. Engine
    // resource roots are never remapped by a project (Constants.hpp says so beside them), so
    // `Resources/Shaders/` resolves against the process's working directory and both hosts `cd` into
    // the directory that holds it before starting. A gate that did not would find no shaders and
    // certify a registry that is missing 76 rows.
    class SandboxProject
    {
    public:
        explicit SandboxProject( const fs::path& repoRoot )
             : m_SavedRoot( Common::Constants::Path::CurrentProjectRoot() ), m_SavedCwd( fs::current_path() )
        {
            const fs::path editorDir = repoRoot / "Editor";
            fs::current_path( editorDir );

            const auto json =
                 Common::Utils::FileSystem::ReadFileContent( ( editorDir / "Desert.deproj" ).string() );
            if ( !json )
                return;

            const auto project = Common::Project::ReadProjectFile( json.GetValue() );
            if ( !project )
                return;

            Common::Constants::Path::SetProjectRoot( editorDir, project.GetValue().AssetsRoot );
            m_Opened = true;
        }

        ~SandboxProject()
        {
            Common::Constants::Path::SetProjectRoot( m_SavedRoot.ProjectDir, m_SavedRoot.AssetsRoot );
            std::error_code ec;
            fs::current_path( m_SavedCwd, ec );
        }

        SandboxProject( const SandboxProject& )            = delete;
        SandboxProject& operator=( const SandboxProject& ) = delete;

        [[nodiscard]] bool Opened() const
        {
            return m_Opened;
        }

    private:
        Common::Constants::Path::ProjectRootState m_SavedRoot;
        fs::path                                  m_SavedCwd;
        bool                                      m_Opened = false;
    };
} // namespace

// 0. THE GATE CAN SEE WHAT IT CLAIMS TO CHECK. Without this every assertion below runs over an empty
// set and reports green — a census that found nothing wrong is byte-identical to one that found
// nothing at all.
TEST( CookedRegistryGate, TheGateCanSeeBothTheRegistryAndTheContentTree )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const SandboxProject project( root );
    ASSERT_TRUE( project.Opened() ) << "Editor/Desert.deproj could not be read";

    ASSERT_TRUE( fs::is_directory( Common::Constants::Path::SHADERDIR_PATH ) )
         << "engine resources do not resolve from the working directory this suite set, so the scan "
            "below would miss every shader and certify a registry that has none";

    // `if` AND NOT `ASSERT_TRUE`, throughout this file, and the reason is worth one line: gtest's
    // ASSERT expands to a `return` the flow analyser does not follow, so every `*tracked` after one
    // reads to clang-tidy as an unchecked optional. A plain guard says the same thing to the reader,
    // to gtest and to the analyser at once — and a NOLINT would have silenced the one check that
    // exists to catch exactly the mistake this file is about: an empty answer read as a good one.
    const auto tracked = Common::Content::TrackedContent( root );
    if ( !tracked.has_value() )
    {
        ADD_FAILURE() << "THE GATE COULD NOT RUN, which is a different answer from 'nothing is wrong'. "
                         "`git ls-files` failed or "
                      << root.string()
                      << " is not a checkout. CI runs this over a git checkout; if that has stopped "
                         "being true, the gate has stopped being a gate and this failure is the only "
                         "thing that will say so.";
        return;
    }
    EXPECT_FALSE( tracked->empty() )
         << "git tracks no content files at all, which cannot be true of this repository";

    EXPECT_TRUE( Common::Utils::FileSystem::Exists( Common::Utils::AssetRegistry::DefaultPath() ) )
         << "there is no cooked asset registry at " << Common::Utils::AssetRegistry::DefaultPath().string()
         << ". Since T2.4 neither host scans the content roots at boot, so a checkout without this file "
            "is a checkout with no content and a build of it would ship none. Run "
            "`cd Editor && ../build/Bin/Debug/AssetRegistryTool cook Desert.deproj` and commit it.";
}

// ── THE RELATION ────────────────────────────────────────────────────────────────────────────────────

TEST( CookedRegistryGate, TheCommittedRegistryDescribesExactlyWhatACleanCloneCarries )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const SandboxProject project( root );
    ASSERT_TRUE( project.Opened() );

    auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
    ASSERT_TRUE( loaded ) << loaded.GetError();

    const auto tracked = Common::Content::TrackedContent( root );
    if ( !tracked.has_value() )
    {
        ADD_FAILURE() << "the gate could not run — see the first test for what that means";
        return;
    }
    const std::map<std::string, Common::Content::ContentFile>& present = *tracked;
    ASSERT_FALSE( present.empty() );

    // AGAINST WHAT GIT TRACKS, NOT AGAINST THE DISK. An untracked file under `Editor/Cooked/` is this
    // machine's cook output; it is described by the cook's own registry, which is not committed and is
    // not this gate's subject.
    const auto problems = Common::Content::Compare( loaded.GetValue(), *tracked, "tracked by this repository" );

    // EVERY disagreement is printed, not the first: a re-cook fixes all of them at once, and a gate
    // that reports one per run turns one command into as many runs as there are files.
    for ( const Common::Content::RegistryDisagreement& problem : problems )
        ADD_FAILURE() << problem.Detail;

    EXPECT_TRUE( problems.empty() ) << problems.size() << " disagreement(s); each is printed above";
}

// ── AND THE CENSUS THAT KEEPS THE GATE HONEST ───────────────────────────────────────────────────────

TEST( CookedRegistryGate, EveryContentKindIsRepresentedByTheShippedCorpus )
{
    // A GATE THAT CANNOT SEE A KIND CANNOT GUARD IT. If a content kind exists in the census and this
    // repository ships no file of it, then no run of the relation above has ever exercised that kind's
    // root or extension — and a mistake in either (a root that does not exist, an extension spelled
    // without its dot) would sit there green until the day somebody authored the first file.
    //
    // It is the same argument as "a census must be able to name what it forbids": the thing that makes
    // a census worth having is that it would have gone red, and that is only true of rows it reaches.
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const SandboxProject project( root );
    ASSERT_TRUE( project.Opened() );

    auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
    ASSERT_TRUE( loaded ) << loaded.GetError();

    const auto tracked = Common::Content::TrackedContent( root );
    if ( !tracked.has_value() )
    {
        ADD_FAILURE() << "the gate could not run — see the first test for what that means";
        return;
    }

    std::set<std::string> kindsTracked;
    for ( const auto& [key, file] : *tracked )
        kindsTracked.insert( std::string( Common::Content::KindName( file.Kind ) ) );

    // KINDS THAT ARE PRODUCED, NEVER COMMITTED — one named row each, with the suite that reaches the row
    // instead. A kind listed here that the repository DOES track again is red: the exemption would then be
    // hiding the ordinary check, and the row has to go.
    //
    // Texture left this list when AF3 committed `.detex` files again; every kind still here is cook output only.
    struct CookOnlyKind
    {
        const char* Kind;
        const char* Suite; // repository-relative source file of the suite that reaches the row
        const char* Test;  // the test in it that goes red when the row is wrong
    };
    constexpr CookOnlyKind kCookOnlyKinds[] = {
         // A partitioned world's cells and index (AF2) exist only as cook output; the WorldCells suite holds
         // the census's extensions equal to the cook's file names and reads the kind back from a cooked header.
         { "WorldCell", "Desert/Tests/Engine/WorldCells/world_cells_test.cpp",
           "ACookedFileNamesItsKindInItsHeader" },
         { "WorldIndex", "Desert/Tests/Engine/WorldCells/world_cells_test.cpp",
           "ACookedFileNamesItsKindInItsHeader" },
    };

    for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
    {
        const auto        kind = static_cast<Common::Content::ContentKind>( i );
        const std::string name( Common::Content::KindName( kind ) );

        const auto* const cookOnly =
             std::find_if( std::begin( kCookOnlyKinds ), std::end( kCookOnlyKinds ),
                           [&name]( const CookOnlyKind& row ) { return name == row.Kind; } );
        if ( cookOnly != std::end( kCookOnlyKinds ) )
        {
            EXPECT_EQ( kindsTracked.find( name ), kindsTracked.end() )
                 << "'" << name << "' is registered as produced-never-committed, and the repository tracks a "
                 << "file of it again: delete its row in kCookOnlyKinds so the ordinary check applies";
            const std::ifstream suite( root / cookOnly->Suite );
            std::stringstream text;
            text << suite.rdbuf();
            EXPECT_NE( text.str().find( std::string( ", " ) + cookOnly->Test + " )" ), std::string::npos )
                 << "'" << name << "' is exempted on the strength of " << cookOnly->Suite << " / "
                 << cookOnly->Test << ", which no longer exists - the census row is reached by nothing";
            continue;
        }

        EXPECT_NE( kindsTracked.find( name ), kindsTracked.end() )
             << "this repository ships no '" << name
             << "' file, so nothing has ever exercised that census row: its root ("
             << Common::Content::KindSpec( kind ).Root->string() << ") and its extension ("
             << Common::Content::KindSpec( kind ).Extension
             << ") could both be wrong and every run of this gate would still be green. Add one file of "
                "that kind to the repository, or remove the row.\n"
                "COMMITTED, not merely present on your machine: a fixture nobody pushed certifies this "
                "gate for its author and for nobody else, which has already happened twice.";

        EXPECT_FALSE( loaded.GetValue().OfKind( name ).empty() )
             << "the registry holds no '" << name << "' row while the repository tracks one";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
