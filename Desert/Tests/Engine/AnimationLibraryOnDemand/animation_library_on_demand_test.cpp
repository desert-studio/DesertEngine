// AL1-6: the animation library is filled from the content registry's `.anim` rows WITHOUT reading a
// clip, and a clip is read through AsyncAssetLoader when a lookup names it. These pin both halves
// against the project's real cooked clips.

#include <gtest/gtest.h>

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>

#include <filesystem>
#include <format>

namespace
{
    using namespace Desert;
    namespace fs   = std::filesystem;
    namespace Path = Common::Constants::Path;

    constexpr const char* kClip = "SkinProbe_Tilt"; // Editor/Resources/Assets/Meshes/Skinned/SkinProbe_Tilt.anim

    fs::path RepoRoot()
    {
        for ( const char* prefix : { "", "../", "../../", "../../../", "../../../../" } )
        {
            const fs::path candidate =
                 fs::path( prefix ) / "Editor/Resources/Assets/Meshes/Skinned/SkinProbe_Tilt.anim";
            if ( fs::is_regular_file( candidate ) )
                return fs::absolute( fs::path( prefix ).empty() ? fs::path( "." ) : fs::path( prefix ) )
                     .lexically_normal();
        }
        return {};
    }

    class AnimationLibraryOnDemand : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Saved = Path::CurrentProjectRoot();
            Assets::AsyncAssetLoader::Get().ResetForTest();
            const fs::path root = RepoRoot();
            ASSERT_FALSE( root.empty() );
            Path::SetProjectRoot( root / "Editor", "Resources/Assets" );
            Assets::ContentRegistry::ResetForTest();
            std::vector<std::string> refused;
            ASSERT_TRUE( Assets::ContentRegistry::Gather( &refused ) );
        }

        void TearDown() override
        {
            Assets::AsyncAssetLoader::Get().ShutdownAndDrain();
            Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
            Assets::ContentRegistry::ResetForTest();
            Common::AssetPathIndex::Clear();
        }

        std::size_t ClipAssets() const
        {
            return m_Manager.FindAllByType<Assets::AnimationAsset>().size();
        }

        Assets::AssetManager m_Manager;

    private:
        Path::ProjectRootState m_Saved;
    };
} // namespace

TEST_F( AnimationLibraryOnDemand, IndexingTheRegistryRowsReadsAndCreatesNoClip )
{
    Animation::AnimationLibrary library( &m_Manager );
    const std::size_t rows = Assets::ContentRegistry::Rows( Common::Content::ContentKind::Animation ).size();
    ASSERT_GT( rows, 0U ) << "the project has no .anim rows; this suite has nothing to index";

    EXPECT_EQ( library.IndexRegistryRows(), rows );
    EXPECT_EQ( ClipAssets(), 0U ) << "indexing created clip assets; it must only remember the rows";
    EXPECT_TRUE( library.HasPending( kClip ) )
         << "the row's stated Name was not indexed, so a lookup by clip name cannot reach it unread";
    EXPECT_FALSE( library.HasPending( "NoSuchClipInThisProject" ) )
         << "a name no row states is reported as pending, so a real miss would never be reported";
}

TEST_F( AnimationLibraryOnDemand, ALookupRequestsOnlyTheNamedClipAndItBecomesPlayableWhenRead )
{
    Animation::AnimationLibrary library( &m_Manager );
    ASSERT_GT( library.IndexRegistryRows(), 1U );

    const Animation::MeshSkeletonIdentity none{};
    const auto                            first = library.FindForMesh( none, kClip );
    EXPECT_FALSE( first ) << "a clip nobody has read was answered synchronously";
    ASSERT_EQ( ClipAssets(), 1U ) << "the lookup created other clips than the one it named";

    const auto asset = m_Manager.FindAllByType<Assets::AnimationAsset>().begin()->second;
    ASSERT_TRUE( asset );
    EXPECT_FALSE( asset->IsReadyForUse() ) << "the clip was read inside the lookup, not by the loader";
    EXPECT_TRUE( Assets::AsyncAssetLoader::Get().IsRequested( asset->GetMetadata().Handle ) );

    EXPECT_TRUE( Assets::AsyncAssetLoader::Get().FlushOne( asset->GetMetadata().Handle ) );
    EXPECT_TRUE( asset->IsReadyForUse() );
    EXPECT_EQ( asset->GetClip().AnimationName, kClip );
    EXPECT_FALSE( library.HasPending( kClip ) ) << "a read clip is still reported as pending";
}

// THM1l-b11: UE's OnAssetAdded reaching the index. On real rigs (Fox, CesiumMan) the import wrote Fox_Walk.anim
// with the rig's signature and the character still stood in its bind pose: the library had indexed the rows
// once, at population, and a clip the import wrote afterwards never reached it. The cook's NoteFile is the
// import's write; nothing else is called here, and the library must offer the clip, and offer it ONCE when
// the import rewrites it.
TEST_F( AnimationLibraryOnDemand, AClipAnImportWritesAfterPopulationIsOffered )
{
    const fs::path source = RepoRoot() / "Editor/Resources/Assets/Meshes/Skinned/SkinProbe_Tilt.anim";
    const fs::path project =
         fs::temp_directory_path() /
         std::format( "anim_library_import_{}", ::testing::UnitTest::GetInstance()->random_seed() );
    fs::remove_all( project );
    const fs::path clipDir = project / "Resources/Assets/Meshes/Skinned";
    fs::create_directories( clipDir );
    Path::SetProjectRoot( project, "Resources/Assets" );
    Assets::ContentRegistry::ResetForTest();
    std::vector<std::string> refused;
    ASSERT_TRUE( Assets::ContentRegistry::Gather( &refused ) );

    Animation::AnimationLibrary library( &m_Manager );
    ASSERT_EQ( library.IndexRegistryRows(), 0U ) << "the empty project already has clips";
    EXPECT_FALSE( library.HasPending( kClip ) );

    // The import writes the clip.
    const fs::path written = clipDir / "Imported_Tilt.anim";
    fs::copy_file( source, written, fs::copy_options::overwrite_existing );
    Assets::ContentRegistry::NoteFile( written );

    EXPECT_TRUE( library.HasPending( kClip ) ) << "the clip the import wrote never reached the library";
    const Animation::MeshSkeletonIdentity none{};
    EXPECT_FALSE( library.FindForMesh( none, kClip ) ) << "a mesh naming no skeleton was offered a clip";
    ASSERT_EQ( ClipAssets(), 1U );
    const auto asset = m_Manager.FindAllByType<Assets::AnimationAsset>().begin()->second;
    ASSERT_TRUE( Assets::AsyncAssetLoader::Get().FlushOne( asset->GetMetadata().Handle ) );

    // A mesh on the skeleton the imported clip names (by GUID) is offered it.
    ASSERT_FALSE( asset->GetSkeleton().IsNull() ) << "the imported clip names no skeleton";
    const Animation::MeshSkeletonIdentity rig{ { asset->GetSkeleton(), "the imported clip's skeleton" }, {} };
    EXPECT_EQ( library.GetForMesh( rig ).size(), 1U ) << "the skeleton the imported clip names is offered nothing";
    EXPECT_TRUE( library.FindForMesh( rig, kClip ) );

    // The import rewrites it (a reimport): still one clip, re-registered from the asset read in place.
    fs::copy_file( source, written, fs::copy_options::overwrite_existing );
    Assets::ContentRegistry::NoteFile( written );
    EXPECT_EQ( library.GetForMesh( rig ).size(), 1U ) << "a rewritten clip is offered twice or not at all";
    EXPECT_FALSE( library.HasPending( kClip ) );

    fs::remove_all( project );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
