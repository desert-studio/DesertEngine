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

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/Constants.hpp>

#include "../../TestSupport/engine_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include "../../TestSupport/scratch_dir.hpp"
#include "../../TestSupport/project_scope.hpp"
#include "../../TestSupport/runner.hpp"

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

TEST( PathCensus, TheProjectLayoutIsPinned )
{
    const ProjectRootGuard guard;
    const fs::path         project = "/ann/work/Game";
    Path::SetProjectRoot( project, "Content" );

    // Byte-for-byte the spellings below a project's assets root. Every asset registry, cooked file and
    // saved scene of every project depends on these exact strings; a census edit that shifts one is a
    // data migration, not a refactor, and must fail here first.
    const std::array<std::pair<const fs::path*, const char*>, 27> expected = { {
         { &Path::ASSETS_PATH, "Content/" },
         { &Path::MESH_PATH, "Content/Meshes/" },
         { &Path::MATERIAL_PATH, "Content/Materials/" },
         { &Path::TEXTUREDIR_PATH, "Content/Textures/" },
         { &Path::SKYBOX_PATH, "Content/Textures/HDR/" },
         { &Path::SCENE_PATH, "Content/Scenes/" },
         { &Path::PREFAB_PATH, "Content/Prefabs/" },
         { &Path::SCRIPT_PATH, "Content/Scripts/" },
         { &Path::COLLECTIONS_PATH, "Content/Collections/" },
         { &Path::LOCALIZATION_PATH, "Content/Localization/" },
         { &Path::CLOUD_NOISE_PATH, "Content/Clouds/" },
         { &Path::CLOUD_TYPE_PATH, "Content/Clouds/Types/" },
         { &Path::CLOUD_VOLUME_PATH, "Content/Clouds/Volumes/" },
         { &Path::CLOUD_LAYOUT_PATH, "Content/Clouds/Layouts/" },
         { &Path::UI_THEME_PATH, "Content/UI/Themes/" },
         { &Path::CONTROL_RIG_PATH, "Content/Rigs/" },
         { &Path::SHADER_GRAPH_PATH, "Content/ShaderGraphs/" },
         { &Path::ANIM_GRAPH_PATH, "Content/AnimGraphs/" },
         { &Path::RETARGET_PATH, "Content/Retargets/" },
         { &Path::FOLIAGE_TYPE_PATH, "Content/Foliage/" },
         { &Path::LANDSCAPE_LAYER_INFO_PATH, "Content/Landscape/Layers/" },
         { &Path::ANIMATION_PATH, "Content/Animations/" },
         { &Path::LEVEL_SEQUENCE_PATH, "Content/Sequences/" },
         { &Path::VFX_PATH, "Content/VFX/" },
         { &Path::FRACTURE_PATH, "Content/Fractures/" },
         { &Path::WATER_WAVES_PATH, "Content/Water/Waves/" },
         { &Path::COOKED_PATH, "Cooked/" },
    } };
    static_assert( expected.size() == Path::CONTENT_DIR_COUNT,
                   "a census row was added without pinning its project spelling here" );

    for ( const auto& [view, spelling] : expected )
        // Each spelling is read off the project directory, never the engine's or the working one.
        EXPECT_EQ( view->generic_string(), ( project / spelling ).generic_string() );
}

// No project, no content (UE): with no .deproj open every census row is empty, a checked read stops the
// process naming its reader, and a relative content path has nothing to resolve against. The engine
// directory being set changes none of that — there is no built-in content root to fall back to.
TEST( PathCensus, WithoutAProjectThereIsNoContent )
{
    const ProjectRootGuard                    guard;
    const Desert::TestSupport::EngineDirScope engineDir;
    Path::ClearProject();

    EXPECT_FALSE( Path::HasProject() );
    for ( std::size_t i = 0; i < Path::CONTENT_DIR_COUNT; ++i )
        EXPECT_TRUE( Path::Detail::Slot( static_cast<Path::ContentDir>( i ) ).empty() )
             << "census row " << i << " invented a content root without a project";
    EXPECT_THROW( (void)Path::FullPath( "Scenes/Level.desce" ), std::logic_error );
    EXPECT_DEATH( (void)Path::Dir( Path::ContentDir::Scene ), "no project is open" );
    EXPECT_DEATH( (void)Path::ProjectDir(), "no project is open" );
}

TEST( PathCensus, ANamedViewIsTheCensusRowItNames )
{
    // The views are references INTO the derived storage, not copies of it — that identity is what lets
    // AssetHandle's root table, the runtime scan roots and the packager's tree census hold a
    // `const fs::path*` and follow a project switch for free. Compare addresses, not spellings: two
    // equal copies would pass a value comparison and silently stop following remaps.
    const ProjectRootGuard guard;
    Path::SetProjectRoot( "/ann/work/Game", "Content" );
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
// Scenes/ with subdirectories (Levels/, per-feature folders), and the tool is pointed at all of them. A
// first-occurrence rule would resolve a checkout that itself sits under a folder called Scenes against the wrong
// ancestor and write the migrated material into a developer's home directory.
//
// No ProjectRootGuard here or below, and that is the point being made: the inverse reads the census row
// and the path handed to it, never the open project — which is what lets a tool ask about a file that
// belongs to a tree the process has not opened.
TEST( PathCensus, TheInverseResolvesAgainstTheNearestFolderOfThatName )
{
    const fs::path nested = "/home/me/Scenes/proj/Projects/Desert/Content/Scenes/Levels/x.desce";
    const auto     root   = Path::RootForContentPath( Path::ContentDir::Scene, nested );

    EXPECT_EQ( root, std::optional<fs::path>( "/home/me/Scenes/proj/Projects/Desert/Content" ) );
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

// ---------------------------------------------------------------------------------------------------
// NOTHING AUTHORED LIVES UNDER A COOKED FOLDER (AF8 census, extended by AF8b).
//
// `.gitignore` ignores every `Cooked/` folder: the project's Cooked tree holds derived caches (font and
// icon atlases, the local asset registry) and the packager's cook is Saved/Cooked/<Platform>/. A file
// written or referenced under a Cooked folder is therefore never committed - `git add` skips it in
// silence, the author's tree stays green, and the next clone has nothing there (A27: a rig and a clip
// were lost that way). Skinned meshes, rigs and clips lived in Editor/Cooked/Meshes behind a whitelist
// until AF8b moved them into the assets tree; these three tests keep anything from going back.
// ---------------------------------------------------------------------------------------------------

namespace
{
    // The checkout, baked by the build (DESERT_TEST_REPO_ROOT).
    std::optional<fs::path> RepoRoot()
    {
        return Desert::TestSupport::RepositoryRoot();
    }

    std::string ReadText( const fs::path& file )
    {
        const std::ifstream in( file, std::ios::binary );
        std::ostringstream  text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace

TEST( PathCensus, NoContentKindIsRootedUnderTheCookedTree )
{
    // A kind's root is where the registry scan and the importers put its files. Rooted under COOKED_PATH,
    // every file of the kind is derived-and-ignored by construction, whatever the author meant.
    const fs::path cooked = Path::COOKED_PATH.lexically_normal();
    for ( const Common::Content::ContentKindSpec& kind : Common::Content::ContentKinds() )
    {
        if ( kind.StatedOnly() )
            continue;
        const fs::path rel   = kind.Root->lexically_normal().lexically_relative( cooked );
        const bool     under = !rel.empty() && *rel.begin() != "..";
        EXPECT_FALSE( under ) << "content kind " << kind.Name << " is rooted at '" << kind.Root->generic_string()
                              << "', inside the Cooked tree '" << cooked.generic_string()
                              << "', which git ignores: its files would never be committed";
    }
}

TEST( PathCensus, EverySourceFileThatSpellsTheCookedRootIsARegisteredDerivedUse )
{
    // One named row per file, with the reason it may spell the root: each is a DERIVED use (a cache, the
    // local registry, the packager's cook). A new file spelling it has to be added here - and the row's
    // reason is the question a reviewer asks: is what it writes there really derived?
    const std::map<std::string, std::string> registered = {
         { "Desert/Common/Source/Common/Core/Constants.hpp", "defines COOKED_DIR_NAME and COOKED_PATH" },
         { "Desert/Common/Source/Common/Content/DerivedDataCache.cpp",
           "PackagedPath: derived font/icon atlases the packager ships" },
         { "Desert/Common/Source/Common/Content/DerivedDataCache.hpp",
           "comment: where PackagedPath puts a cache" },
         { "Desert/Desert/Source/Engine/Text/FontCache.hpp", "comment: font atlases are a PackagedPath cache" },
         { "Desert/Desert/Source/Engine/Vector/IconBake.hpp", "comment: icon atlases are a PackagedPath cache" },
         { "Editor/Source/EditorLayer.cpp", "the local asset registry Cooked/AssetRegistry.dreg" },
         { "Editor/Source/Editor/Packaging/PackagedContentTrees.hpp", "the packager packs the derived tree" },
         { "Editor/Source/Editor/Packaging/GamePackager.cpp", "the cooked registry's place inside the pak" },
         { "Editor/Source/Editor/Packaging/PackageCook.cpp", "Saved/Cooked/<Platform>/: the packager's cook" },
    };

    const auto root = RepoRoot();
    ASSERT_TRUE( root.has_value() ) << "run from inside the checkout (no .gitignore + Desert/ above "
                                    << Desert::TestSupport::RepositoryRoot().generic_string() << ")";

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT above returns on nullopt
    const fs::path& repo = root.value();

    const std::set<std::string> extensions = { ".hpp", ".cpp", ".h", ".inl", ".mm" };
    std::set<std::string>       found;
    for ( const char* top :
          { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source", "Tools" } )
    {
        std::error_code ec;
        if ( !fs::exists( repo / top, ec ) )
            continue;
        for ( auto it = fs::recursive_directory_iterator( repo / top, ec );
              it != fs::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( ec )
                break;
            if ( it->is_directory() && it->path().filename() == "ThirdParty" )
            {
                it.disable_recursion_pending();
                continue;
            }
            if ( !it->is_regular_file() || !extensions.contains( it->path().extension().string() ) )
                continue;
            const std::string text = ReadText( it->path() );
            if ( text.find( "COOKED_PATH" ) != std::string::npos ||
                 text.find( "COOKED_DIR_NAME" ) != std::string::npos )
                found.insert( it->path().lexically_relative( repo ).generic_string() );
        }
    }

    for ( const std::string& file : found )
        EXPECT_TRUE( registered.contains( file ) )
             << file << " spells the Cooked root and is not in this register; if what it writes there is "
             << "authored content, it belongs under the assets root (git ignores every Cooked/ folder)";
    for ( const auto& [file, why] : registered )
        EXPECT_TRUE( found.contains( file ) )
             << "registered '" << file << "' (" << why << ") no longer spells the Cooked root: delete its row";
}

TEST( PathCensus, NoAuthoredDocumentReferencesAFileUnderACookedFolder )
{
    // The reference side: a scene, a retarget or a project naming `Cooked/...` names a file git ignores,
    // so the reference resolves only on the machine that wrote it.
    const auto root = RepoRoot();
    ASSERT_TRUE( root.has_value() );
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the ASSERT above returns on nullopt
    const fs::path& repo = root.value();

    const std::set<std::string> extensions = { ".desce", ".retarget", ".deproj" };
    std::size_t                 read       = 0;
    std::error_code             ec;
    // Project content (Projects/) and the engine's own content (Editor/Resources).
    for ( const fs::path& tree : { repo / "Projects", repo / "Editor" / "Resources" } )
    {
        for ( auto it = fs::recursive_directory_iterator( tree, ec ); it != fs::recursive_directory_iterator();
              it.increment( ec ) )
        {
            if ( ec )
                break;
            if ( !it->is_regular_file() || !extensions.contains( it->path().extension().string() ) )
                continue;
            ++read;
            const std::string text = ReadText( it->path() );
            EXPECT_EQ( text.find( "Cooked/" ), std::string::npos )
                 << it->path().lexically_relative( repo ).generic_string()
                 << " references a file under a Cooked folder, which git ignores";
        }
    }
    EXPECT_GT( read, 0u ) << "no scene was read under " << ( repo / "Projects" ).generic_string();
}

namespace
{
    // The host steps this suite's process takes before gtest starts (TestSupport/runner.hpp).
    const Desert::TestSupport::SuiteHost kHostSteps{ { .EngineDir = true, .Project = true } };
} // namespace
