// SKEL-eng4: the engine's content mount (UE /Engine) is walked beside the project's assets root in EVERY
// project, so engine assets a project never copied — the built-in humanoid's Humanoid.skeleton — are in its
// scan. The mount is working-directory relative (like the shaders), so the suite stands where the hosts do:
// in Editor/.

#include <gtest/gtest.h>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

namespace
{
    namespace fs   = std::filesystem;
    namespace Path = Common::Constants::Path;
    using Common::Content::ContentKind;

    constexpr const char* kHumanoid = "Meshes/Skinned/Humanoid.skeleton";

    fs::path EditorDir()
    {
        for ( const char* prefix : { "", "../", "../../", "../../../", "../../../../" } )
        {
            const fs::path candidate = fs::path( prefix ) / "Editor/Resources/Assets" / kHumanoid;
            if ( fs::is_regular_file( candidate ) )
                return fs::absolute( fs::path( prefix ) / "Editor" ).lexically_normal();
        }
        return {};
    }

    class EngineContentMount : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Saved               = Path::CurrentProjectRoot();
            m_Cwd                 = fs::current_path();
            const fs::path editor = EditorDir();
            ASSERT_FALSE( editor.empty() ) << "Editor/Resources/Assets/" << kHumanoid << " not found";
            fs::current_path( editor );

            // A project whose own assets tree holds nothing at all.
            m_Project = fs::temp_directory_path() /
                        ( "engine_mount_" + std::to_string( ::testing::UnitTest::GetInstance()->random_seed() ) );
            fs::remove_all( m_Project );
            fs::create_directories( m_Project / "Content" );
            Path::SetProjectRoot( m_Project, "Content" );
        }

        void TearDown() override
        {
            Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
            fs::current_path( m_Cwd );
            std::error_code ec;
            fs::remove_all( m_Project, ec );
        }

        Path::ProjectRootState m_Saved;
        fs::path               m_Cwd;
        fs::path               m_Project;
    };
} // namespace

TEST_F( EngineContentMount, AProjectWithoutTheEngineTreeStillScansHumanoidSkeleton )
{
    const auto found = Common::Content::ScanContentRoots();
    const auto row   = std::find_if( found.begin(), found.end(), []( const auto& entry )
                                     { return entry.first.ends_with( std::string( "Assets/" ) + kHumanoid ); } );
    ASSERT_NE( row, found.end() ) << "the engine mount was not walked: Humanoid.skeleton is not in the scan";
    EXPECT_EQ( row->second.Kind, ContentKind::Skeleton );
    EXPECT_TRUE( row->first.starts_with( "engine:" ) ) << row->first;
}

TEST_F( EngineContentMount, AFileUnderTheMountIsItsKind )
{
    const auto kind = Common::Content::KindOfContentFile( Path::ENGINE_CONTENT_PATH / kHumanoid );
    ASSERT_TRUE( kind.has_value() );
    EXPECT_EQ( *kind, ContentKind::Skeleton );
}

TEST_F( EngineContentMount, AnAssetsKindHasTwoRootsAndTheShadersOne )
{
    const auto& skeleton = Common::Content::KindSpec( ContentKind::Skeleton );
    EXPECT_EQ( Common::Content::ScanRootsOf( skeleton ).size(), 2U );
    const auto& shader = Common::Content::KindSpec( ContentKind::Shader );
    EXPECT_EQ( Common::Content::ScanRootsOf( shader ).size(), 1U );
}

TEST_F( EngineContentMount, TheSandboxWalksTheMountOnce )
{
    Path::ResetToSandbox();
    const auto& skeleton = Common::Content::KindSpec( ContentKind::Skeleton );
    EXPECT_EQ( Common::Content::ScanRootsOf( skeleton ).size(), 1U );
}
