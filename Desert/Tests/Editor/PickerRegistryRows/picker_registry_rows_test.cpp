// "Every asset picker reads the registry's rows, never the preloader's objects" (AL1-1).
//
// Three statements, one suite, because they are one claim: the census says no picker ENUMERATES loaded
// objects; the corpus test says the rows it reads instead are the list the objects used to make, kind for
// kind and in the same order; the watch test says a file that appears reaches that list without a rescan.

#include <Engine/Assets/ContentDirectoryWatch.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Common::Content::ContentKind;
    using Desert::Assets::ContentDirectoryWatch;
    namespace ContentRegistry = Desert::Assets::ContentRegistry;
    namespace Path            = Common::Constants::Path;

    // The repository root, found from wherever the binary was started (the handoff runs suites from the root,
    // a developer from build/Bin/Tests/<cfg>).
    fs::path RepoRoot()
    {
        for ( const char* prefix : { "", "../", "../../", "../../../", "../../../../" } )
        {
            const fs::path candidate = fs::path( prefix ) / "Editor/Source/Editor/Panels";
            if ( fs::is_directory( candidate ) )
                return fs::absolute( fs::path( prefix ).empty() ? fs::path( "." ) : fs::path( prefix ) )
                     .lexically_normal();
        }
        return {};
    }

    class ProjectRootGuard
    {
    public:
        ProjectRootGuard() : m_Saved( Path::CurrentProjectRoot() )
        {
        }
        ~ProjectRootGuard()
        {
            Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
            ContentRegistry::ResetForTest();
            Common::AssetPathIndex::Clear();
        }
        ProjectRootGuard( const ProjectRootGuard& )            = delete;
        ProjectRootGuard& operator=( const ProjectRootGuard& ) = delete;

    private:
        Path::ProjectRootState m_Saved;
    };

    // The kinds an editor picker lists — one per consumer the census below forbids from walking objects.
    constexpr std::array kPickerKinds = {
         ContentKind::Skybox,     ContentKind::Texture,          ContentKind::Shader,
         ContentKind::CloudType,  ContentKind::CloudLayout,      ContentKind::UITheme,
         ContentKind::ControlRig, ContentKind::Retarget,         ContentKind::CloudModellingVolume,
         ContentKind::StaticMesh, ContentKind::SkinnedMesh,      ContentKind::Material,
         ContentKind::AnimGraph,  ContentKind::CloudNoiseVolume,
    };

    bool Contains( const std::vector<ContentRegistry::PickerRow>& rows, const fs::path& file )
    {
        const std::string key = Common::AssetHandle::StableKeyForPath( file );
        return std::any_of( rows.begin(), rows.end(),
                            [&]( const ContentRegistry::PickerRow& row ) { return row.Key == key; } );
    }
} // namespace

TEST( PickerRegistryRows, NoPanelOrWidgetEnumeratesLoadedObjects )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the editor's sources were not found from " << fs::current_path();

    std::vector<std::string> offenders;
    std::size_t              scanned = 0;
    for ( const char* dir : { "Editor/Source/Editor/Panels", "Editor/Source/Editor/Widgets" } )
    {
        for ( const auto& entry : fs::recursive_directory_iterator( root / dir ) )
        {
            const std::string ext = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                continue;
            ++scanned;
            std::ifstream in( entry.path() );
            std::string   line;
            for ( int number = 1; std::getline( in, line ); ++number )
            {
                const auto first = line.find_first_not_of( " \t" );
                if ( first != std::string::npos && line.compare( first, 2, "//" ) == 0 )
                    continue; // a comment may NAME the old call; only code may make it
                if ( line.find( "FindAllByType<" ) != std::string::npos )
                    offenders.push_back( fs::relative( entry.path(), root ).generic_string() + ":" +
                                         std::to_string( number ) );
            }
        }
    }

    EXPECT_GT( scanned, 100u ) << "the census read too few files to be a census";
    EXPECT_TRUE( offenders.empty() ) << "these list LOADED objects instead of ContentRegistry::Rows - the list "
                                        "would empty the moment the preloader stops creating shells:\n  "
                                     << [&]
    {
        std::string all;
        for ( const auto& o : offenders )
            all += o + "\n  ";
        return all;
    }();
}

TEST( PickerRegistryRows, OnTheCorpusTheRowsAreTheListThePreloaderBuiltObjectsFrom )
{
    // THE OLD LIST, STATED BY ITS MECHANISM. Every object `FindAllByType<T>` returned was a shell the
    // preloader created by iterating `FilesOfKind(kind)` in order (`AssetPreloader::ProcessAssetKind`),
    // minus files whose load failed. So the old picker's entries, names and order are exactly that
    // enumeration's, and the new one must be the same sequence of files with a handle that names its row.
    const ProjectRootGuard guard;
    const fs::path         root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    Path::SetProjectRoot( root / "Editor", "Resources/Assets" );
    ContentRegistry::ResetForTest();

    std::vector<std::string> refused;
    ASSERT_TRUE( ContentRegistry::Gather( &refused ) );

    std::size_t total = 0;
    for ( const ContentKind kind : kPickerKinds )
    {
        const std::vector<fs::path>                   old  = ContentRegistry::FilesOfKind( kind );
        const std::vector<ContentRegistry::PickerRow> rows = ContentRegistry::Rows( kind );
        const std::string_view                        name = Common::Content::KindName( kind );

        ASSERT_EQ( rows.size(), old.size() ) << name;
        for ( std::size_t i = 0; i < rows.size(); ++i )
        {
            EXPECT_EQ( rows[i].Path, old[i] ) << name << " entry " << i;
            EXPECT_EQ( rows[i].Path.filename(), old[i].filename() ) << name << " label " << i;
            EXPECT_NE( static_cast<uint64_t>( rows[i].Handle ), 0u ) << name << " " << rows[i].Key;
            EXPECT_EQ( ContentRegistry::KeyForHandle( rows[i].Handle ), rows[i].Key ) << name;
        }
        total += rows.size();
    }

    // The frame pickers of this task must have something to show, or the equality above is vacuous.
    EXPECT_FALSE( ContentRegistry::Rows( ContentKind::Skybox ).empty() );
    EXPECT_FALSE( ContentRegistry::Rows( ContentKind::Material ).empty() );
    EXPECT_GT( total, 50u );
}

// THE NAME AND THE SKELETON ARE TAGS THE SCAN READ, NOT OBJECTS THE LIST LOADED (UE's FAssetData tags). The
// five kinds that state a display name at the top of their document list it; the mesh pickers split on the
// header's skinned flag. Literal names from the corpus files, so a scan that read the wrong member fails.
TEST( PickerRegistryRows, OnTheCorpusNamesAndTheSkinnedSplitComeFromTheRegistryTags )
{
    using Common::Content::ContentKind;
    const ProjectRootGuard guard;
    const fs::path         root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    Path::SetProjectRoot( root / "Editor", "Resources/Assets" );
    ContentRegistry::ResetForTest();
    ASSERT_TRUE( ContentRegistry::Gather() );
    const auto nameOf = []( ContentKind kind, std::string_view keySuffix ) -> std::string
    {
        for ( const auto& row : ContentRegistry::Rows( kind ) )
        {
            if ( row.Key.ends_with( keySuffix ) )
                return row.DisplayName;
        }
        return "<no row>";
    };
    EXPECT_EQ( nameOf( ContentKind::UITheme, "Desert_Dark.detheme" ), "Desert Dark" );
    EXPECT_EQ( nameOf( ContentKind::ControlRig, "IKProbe_Arm.derig" ), "IKProbe Arm" );
    EXPECT_EQ( nameOf( ContentKind::Retarget, "ForeignArm_To_IKProbe.retarget" ), "ForeignArm to IKProbe" );
    EXPECT_EQ( nameOf( ContentKind::AnimGraph, "OneBoneBlend.danimgraph" ), "OneBoneBlend" );
    EXPECT_EQ( nameOf( ContentKind::CloudType, "Altocumulus.decloudtype" ), "Altocumulus" );
    // Rigs, retargets and graphs REQUIRE the member; every one of their rows must carry a name.
    for ( const ContentKind kind : { ContentKind::ControlRig, ContentKind::Retarget, ContentKind::AnimGraph } )
    {
        for ( const auto& row : ContentRegistry::Rows( kind ) )
            EXPECT_FALSE( row.DisplayName.empty() ) << row.Key << " states a name the scan did not read";
    }

    const auto skinned = ContentRegistry::MeshRows( true );
    const auto statics = ContentRegistry::MeshRows( false );
    const auto hasKey  = []( const auto& rows, std::string_view suffix )
    { return std::ranges::any_of( rows, [&]( const auto& row ) { return row.Key.ends_with( suffix ); } ); };
    EXPECT_TRUE( hasKey( statics, "StaticProbe.stmesh" ) );
    EXPECT_FALSE( hasKey( skinned, "StaticProbe.stmesh" ) );
    for ( const char* mesh : { "IKProbe.skmesh", "SkinProbe.skmesh", "TwoBoneProbe.skmesh" } )
    {
        EXPECT_TRUE( hasKey( skinned, mesh ) ) << mesh << " is missing from the skinned picker";
        EXPECT_FALSE( hasKey( statics, mesh ) ) << mesh << " is on the static picker";
    }
    EXPECT_EQ( skinned.size() + statics.size(), ContentRegistry::Rows( ContentKind::StaticMesh ).size() +
                                                     ContentRegistry::Rows( ContentKind::SkinnedMesh ).size() )
         << "a mesh row is on neither picker";
}

TEST( PickerRegistryRows, AFileThatAppearsReachesTheRowsWithoutARescanAndLeavesWhenItGoes )
{
    const ProjectRootGuard guard;
    const fs::path         repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    const fs::path source = repo / "Editor/Resources/Assets/UI/Themes/Desert_Light.detheme";
    ASSERT_TRUE( fs::exists( source ) );

    const fs::path project = fs::temp_directory_path() / "al1_picker_rows_project";
    fs::remove_all( project );
    Path::SetProjectRoot( project, "Assets" );
    const fs::path themes = *Common::Content::KindSpec( ContentKind::UITheme ).Root;
    fs::create_directories( themes );
    fs::copy_file( source, themes / "First.detheme" );

    ContentRegistry::ResetForTest();
    ASSERT_TRUE( ContentRegistry::Gather() );
    ASSERT_EQ( ContentRegistry::Rows( ContentKind::UITheme ).size(), 1u );

    ContentDirectoryWatch watch;
    EXPECT_EQ( watch.Poll(), 0u ) << "the first poll is the baseline";
    EXPECT_EQ( watch.Poll(), 0u ) << "nothing moved";

    // Directory mtimes have coarse resolution on some filesystems; step past it.
    const auto before = fs::last_write_time( themes );
    fs::copy_file( source, themes / "Second.detheme" );
    fs::last_write_time( themes, before + std::chrono::seconds( 2 ) );

    EXPECT_EQ( watch.Poll(), 1u );
    EXPECT_TRUE( Contains( ContentRegistry::Rows( ContentKind::UITheme ), themes / "Second.detheme" ) );
    EXPECT_EQ( ContentRegistry::Rows( ContentKind::UITheme ).size(), 2u );

    // A folder that arrives whole brings its files with it.
    fs::create_directories( themes / "Imported" );
    fs::copy_file( source, themes / "Imported/Third.detheme" );
    fs::last_write_time( themes, before + std::chrono::seconds( 4 ) );
    EXPECT_EQ( watch.Poll(), 1u );
    EXPECT_TRUE( Contains( ContentRegistry::Rows( ContentKind::UITheme ), themes / "Imported/Third.detheme" ) );

    fs::remove( themes / "Second.detheme" );
    fs::last_write_time( themes, before + std::chrono::seconds( 6 ) );
    EXPECT_EQ( watch.Poll(), 1u );
    EXPECT_FALSE( Contains( ContentRegistry::Rows( ContentKind::UITheme ), themes / "Second.detheme" ) );
    EXPECT_EQ( ContentRegistry::Rows( ContentKind::UITheme ).size(), 2u );

    fs::remove_all( project );
}

TEST( PickerRegistryRows, AnInPlaceEditRedescribesTheRowThroughUpdate )
{
    // What hot reload does for a file it already watches: `Update` on the path it saw move.
    const ProjectRootGuard guard;
    const fs::path         repo    = RepoRoot();
    const fs::path         source  = repo / "Editor/Resources/Assets/UI/Themes/Desert_Light.detheme";
    const fs::path         project = fs::temp_directory_path() / "al1_picker_rows_update";
    fs::remove_all( project );
    Path::SetProjectRoot( project, "Assets" );
    const fs::path themes = *Common::Content::KindSpec( ContentKind::UITheme ).Root;
    fs::create_directories( themes );
    const fs::path file = themes / "Edited.detheme";
    fs::copy_file( source, file );

    ContentRegistry::ResetForTest();
    ASSERT_TRUE( ContentRegistry::Gather() );
    ASSERT_TRUE( ContentRegistry::Save() ) << "the cache is written into the temporary project";
    ASSERT_FALSE( ContentRegistry::Dirty() );

    {
        std::ofstream out( file, std::ios::app );
        out << "\n";
    }
    ContentRegistry::Update( file );
    EXPECT_TRUE( ContentRegistry::Dirty() ) << "the size moved, so the row was re-described";

    fs::remove( file );
    ContentRegistry::Update( file );
    EXPECT_TRUE( ContentRegistry::Rows( ContentKind::UITheme ).empty() );

    fs::remove_all( project );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
