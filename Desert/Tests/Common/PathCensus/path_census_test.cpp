// The guard over Constants.hpp's path census: after a project root changes, EVERY derived directory
// must equal its root joined with its census row's relative part — for all rows at once, not for the
// three somebody remembered to spot-check. The defect shape this pins is the one the seventeen-variable
// era invited: a remap that rewrote sixteen paths and forgot the seventeenth looked correct everywhere
// anyone looked. The census makes that unrepresentable by construction; this suite is the tripwire for
// the day someone hand-edits the derivation itself (a special-cased row would pass compilation and fail
// here).
//
// What is deliberately NOT tested: the on-disk .deproj format and the folders a new project is
// scaffolded with — that census lives with the project format and has its own suite.

#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace Path = Common::Constants::Path;
namespace fs   = std::filesystem;

namespace
{
    // Restores the project root, so this suite cannot leak a fake project into whatever runs after it.
    class ProjectRootGuard
    {
    public:
        ProjectRootGuard() : m_Saved( Path::CurrentProjectRoot() )
        {
        }

        ~ProjectRootGuard()
        {
            Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
        }

        ProjectRootGuard( const ProjectRootGuard& )            = delete;
        ProjectRootGuard& operator=( const ProjectRootGuard& ) = delete;

    private:
        Path::ProjectRootState m_Saved;
    };

    // The relation itself, spelled once: what row `d` must equal under the root pair (projectDir,
    // assetsRoot). This mirrors the documented contract, not the implementation — it recomputes the
    // expected value from the census spec independently of Derive().
    fs::path Expected( Path::ContentDir d, const fs::path& projectDir, const fs::path& assetsRoot )
    {
        const auto&    spec = Path::CONTENT_DIRS[static_cast<std::size_t>( d )];
        const fs::path base = spec.Root == Path::DirRoot::Assets
                                   ? ( projectDir / assetsRoot ).lexically_normal()
                                   : ( projectDir / Path::COOKED_DIR_NAME ).lexically_normal();
        return base / spec.Rel;
    }

    void ExpectAllRowsDerivedFrom( const fs::path& projectDir, const fs::path& assetsRoot )
    {
        for ( std::size_t i = 0; i < Path::CONTENT_DIR_COUNT; ++i )
        {
            const auto d = static_cast<Path::ContentDir>( i );
            EXPECT_EQ( Path::Dir( d ), Expected( d, projectDir, assetsRoot ) )
                 << "census row " << i << " (rel '" << Path::CONTENT_DIRS[i].Rel
                 << "') does not equal its root plus its relative part";
        }
    }
} // namespace

TEST( PathCensus, EveryDerivedPathEqualsItsRootPlusRelativePart )
{
    ProjectRootGuard guard;

    // Two projects that share nothing, opened in sequence — the relation must hold after EACH remap,
    // for every row, or a stale path from the previous project survives into the new one.
    Path::SetProjectRoot( "/ann/work/Game", "Content" );
    ExpectAllRowsDerivedFrom( "/ann/work/Game", "Content" );

    Path::SetProjectRoot( "/opt/ci/checkout/Other", "Assets" );
    ExpectAllRowsDerivedFrom( "/opt/ci/checkout/Other", "Assets" );
}

TEST( PathCensus, NoRowSurvivesARemapPointingAtThePreviousProject )
{
    ProjectRootGuard guard;

    // The forgotten-seventeenth-variable defect, stated directly: after moving to project B, no derived
    // directory may still mention project A. Checked over the whole census, not a sample.
    Path::SetProjectRoot( "/ann/work/Game", "Content" );
    Path::SetProjectRoot( "/opt/ci/checkout/Other", "Assets" );

    for ( std::size_t i = 0; i < Path::CONTENT_DIR_COUNT; ++i )
    {
        const auto d = static_cast<Path::ContentDir>( i );
        EXPECT_EQ( Path::Dir( d ).generic_string().find( "/ann/work/Game" ), std::string::npos )
             << "census row " << i << " still points into the previously opened project";
    }
}

TEST( PathCensus, TheSandboxLayoutIsTheHistoricalOne )
{
    ProjectRootGuard guard;
    Path::ResetToSandbox();

    // Byte-for-byte the spellings the engine shipped with before the census existed. Every asset
    // registry, cooked file and saved scene in the sandbox depends on these exact strings; a census
    // edit that shifts one is a data migration, not a refactor, and must fail here first.
    const std::array<std::pair<const fs::path*, const char*>, 21> expected = { {
         { &Path::ASSETS_PATH, "Resources/Assets/" },
         { &Path::MESH_PATH, "Resources/Assets/Meshes/" },
         { &Path::MATERIAL_PATH, "Resources/Assets/Materials/" },
         { &Path::TEXTUREDIR_PATH, "Resources/Assets/Textures/" },
         { &Path::SKYBOX_PATH, "Resources/Assets/Textures/HDR/" },
         { &Path::SCENE_PATH, "Resources/Assets/Scenes/" },
         { &Path::PREFAB_PATH, "Resources/Assets/Prefabs/" },
         { &Path::SCRIPT_PATH, "Resources/Assets/Scripts/" },
         { &Path::COLLECTIONS_PATH, "Resources/Assets/Collections/" },
         { &Path::LOCALIZATION_PATH, "Resources/Assets/Localization/" },
         { &Path::CLOUD_NOISE_PATH, "Resources/Assets/Clouds/" },
         { &Path::CLOUD_TYPE_PATH, "Resources/Assets/Clouds/Types/" },
         { &Path::CLOUD_VOLUME_PATH, "Resources/Assets/Clouds/Volumes/" },
         { &Path::CLOUD_LAYOUT_PATH, "Resources/Assets/Clouds/Layouts/" },
         { &Path::UI_THEME_PATH, "Resources/Assets/UI/Themes/" },
         { &Path::CONTROL_RIG_PATH, "Resources/Assets/Rigs/" },
         { &Path::SHADER_GRAPH_PATH, "Resources/Assets/ShaderGraphs/" },
         { &Path::ANIM_GRAPH_PATH, "Resources/Assets/AnimGraphs/" },
         { &Path::RETARGET_PATH, "Resources/Assets/Retargets/" },
         { &Path::COOKED_PATH, "Cooked/" },
         { &Path::MESH_PATH_COOKED, "Cooked/Meshes/" },
    } };
    static_assert( expected.size() == Path::CONTENT_DIR_COUNT,
                   "a census row was added without pinning its sandbox spelling here" );

    for ( const auto& [view, spelling] : expected )
        EXPECT_EQ( view->generic_string(), spelling );
}

TEST( PathCensus, ANamedViewIsTheCensusRowItNames )
{
    // The views are references INTO the derived storage, not copies of it — that identity is what lets
    // AssetHandle's root table, the runtime scan roots and the packager's tree census hold a
    // `const fs::path*` and follow a project switch for free. Compare addresses, not spellings: two
    // equal copies would pass a value comparison and silently stop following remaps.
    EXPECT_EQ( &Path::ASSETS_PATH, &Path::Dir( Path::ContentDir::Assets ) );
    EXPECT_EQ( &Path::COOKED_PATH, &Path::Dir( Path::ContentDir::Cooked ) );
    EXPECT_EQ( &Path::CLOUD_LAYOUT_PATH, &Path::Dir( Path::ContentDir::CloudLayout ) );
}

TEST( PathCensus, AStoredPointerFollowsARemap )
{
    ProjectRootGuard guard;

    // The long-lived-table pattern, exercised end to end: a pointer taken before a project opens must
    // read the remapped value afterwards. This is the load-bearing property the census refactor was not
    // allowed to break.
    const fs::path* mesh = &Path::MESH_PATH;

    Path::SetProjectRoot( "/ann/work/Game", "Content" );
    EXPECT_EQ( *mesh, fs::path( "/ann/work/Game/Content" ) / "Meshes/" );

    Path::SetProjectRoot( "/opt/ci/checkout/Other", "Assets" );
    EXPECT_EQ( *mesh, fs::path( "/opt/ci/checkout/Other/Assets" ) / "Meshes/" );
}

TEST( PathCensus, CurrentProjectRootReportsWhatWasSet )
{
    ProjectRootGuard guard;

    // What the test guards above (and every other suite's ProjectRootGuard) depend on: the stored pair
    // is the WHOLE state, so reading it back and re-setting it must restore every derived path.
    Path::SetProjectRoot( "/ann/work/Game", "Content" );
    const Path::ProjectRootState saved = Path::CurrentProjectRoot();
    EXPECT_EQ( saved.ProjectDir, fs::path( "/ann/work/Game" ) );
    EXPECT_EQ( saved.AssetsRoot, fs::path( "Content" ) );

    Path::SetProjectRoot( "/opt/ci/checkout/Other", "Assets" );
    Path::SetProjectRoot( saved.ProjectDir, saved.AssetsRoot );
    ExpectAllRowsDerivedFrom( "/ann/work/Game", "Content" );
}

// THE INVERSE, over the WHOLE census. RootForContentPath answers "which root was this file's directory
// derived from", and it exists because four defects in one day were a path resolved from the process's
// working directory instead of from the file being worked on. The guarantee a caller rests on is a
// ROUND TRIP: Derive() builds Dir(d) from a root, and a file placed inside Dir(d) must give that same
// root back. Asserted for every row rather than for the one row the migration tool needed, because the
// two directions read the same census and this is what keeps them from drifting apart.
TEST( PathCensus, TheInverseRecoversTheRootEveryRowWasDerivedFrom )
{
    ProjectRootGuard guard;
    Path::SetProjectRoot( "/ann/work/Game", "Content" );

    const fs::path assets = fs::path( "/ann/work/Game" ) / "Content";
    const fs::path cooked = fs::path( "/ann/work/Game" ) / Path::COOKED_DIR_NAME;

    for ( std::size_t i = 0; i < Path::CONTENT_DIR_COUNT; ++i )
    {
        const auto  d    = static_cast<Path::ContentDir>( i );
        const auto& spec = Path::CONTENT_DIRS[i];

        const std::optional<fs::path> root = Path::RootForContentPath( d, Path::Dir( d ) / "file.ext" );

        // The two rows that name a root ITSELF have no relative part to find, and a file somewhere
        // inside a root says nothing about where that root begins — so they answer nothing rather than
        // guess, and a change that made them guess would land here.
        if ( spec.Rel.empty() )
        {
            EXPECT_FALSE( root.has_value() )
                 << "census row " << i << " names a root itself, so it cannot locate that root from a "
                 << "file inside it";
            continue;
        }

        ASSERT_TRUE( root.has_value() ) << "census row " << i << " (rel '" << spec.Rel
                                        << "') did not recognise a file inside its own directory";
        EXPECT_EQ( root->lexically_normal(), spec.Root == Path::DirRoot::Assets ? assets : cooked )
             << "census row " << i << " (rel '" << spec.Rel << "') recovered the wrong root";
    }
}

// THE LAST OCCURRENCE WINS, and the case that made it matter: this repository's scenes live flat in
// Scenes/ with ONE subdirectory, Autosave/, and the tool is pointed at both. A first-occurrence rule
// would resolve a checkout that itself sits under a folder called Scenes against the wrong ancestor and
// write the migrated material into a developer's home directory.
//
// No ProjectRootGuard here or below, and that is the point being made: the inverse reads the census row
// and the path handed to it, never the open project — which is what lets a tool ask about a file that
// belongs to a tree the process has not opened.
TEST( PathCensus, TheInverseResolvesAgainstTheNearestFolderOfThatName )
{
    const fs::path nested = "/home/me/Scenes/proj/Editor/Resources/Assets/Scenes/Autosave/x.desce";
    const auto     root   = Path::RootForContentPath( Path::ContentDir::Scene, nested );

    ASSERT_TRUE( root.has_value() );
    EXPECT_EQ( *root, fs::path( "/home/me/Scenes/proj/Editor/Resources/Assets" ) );
}

// The two answers that are not paths. A file under no such folder gets nothing back — the caller has to
// decide what that means rather than be handed a plausible-looking root — and a caller-RELATIVE spelling
// gets the empty path, which is the honest statement that its root is wherever the caller is standing.
// The second is not the working-directory dependence this function exists to remove: the caller named
// the file that way, so the answer reproduces the caller's own frame instead of inventing one.
TEST( PathCensus, TheInverseRefusesAPathOutsideTheRowAndKeepsACallerRelativeFrame )
{
    EXPECT_FALSE( Path::RootForContentPath( Path::ContentDir::Scene, "/ann/work/Game/Content/Meshes/x.desce" )
                       .has_value() );

    const auto relative = Path::RootForContentPath( Path::ContentDir::Scene, "Scenes/x.desce" );
    ASSERT_TRUE( relative.has_value() );
    EXPECT_TRUE( relative->empty() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
