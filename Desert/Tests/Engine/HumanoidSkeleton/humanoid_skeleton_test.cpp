// SKEL-eng5: the built-in humanoid's rig has ONE home, the asset Humanoid.skeleton (engine content,
// Resources/Engine/). The factory reads it; no joint is restated in code. The suite stands in Editor/, where
// the hosts do, because the engine mount is working-directory relative.

#include <gtest/gtest.h>

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Geometry/ProceduralCharacterSkeleton.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using namespace Desert;

    constexpr const char* kHumanoid = "Resources/Engine/Meshes/Skinned/Humanoid.skeleton";

    fs::path EditorDir()
    {
        for ( const char* prefix : { "", "../", "../../", "../../../", "../../../../" } )
        {
            if ( fs::is_regular_file( fs::path( prefix ) / "Editor" / kHumanoid ) )
                return fs::absolute( fs::path( prefix ) / "Editor" ).lexically_normal();
        }
        return {};
    }

    class HumanoidSkeleton : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Cwd                 = fs::current_path();
            const fs::path editor = EditorDir();
            ASSERT_FALSE( editor.empty() ) << "Editor/" << kHumanoid << " not found";
            fs::current_path( editor );
        }

        void TearDown() override
        {
            fs::current_path( m_Cwd );
        }

        // The file read on its own, NOT through ReadSkeletonFile, so the comparison has two sides.
        static Assets::Serialization::SkeletonAssetData FileData()
        {
            const std::ifstream in( kHumanoid, std::ios::binary );
            std::ostringstream text;
            text << in.rdbuf();
            auto read = Assets::Serialization::ReadSkeletonJson( text.str() );
            EXPECT_TRUE( read.IsSuccess() ) << read.GetError();
            return read.IsSuccess() ? read.ExtractValue() : Assets::Serialization::SkeletonAssetData{};
        }

        fs::path m_Cwd;
    };
} // namespace

TEST_F( HumanoidSkeleton, TheFactoryPathIsTheEngineMountFile )
{
    EXPECT_EQ( Geometry::HumanoidSkeletonFile().lexically_normal(), fs::path( kHumanoid ).lexically_normal() );
}

TEST_F( HumanoidSkeleton, TheFactoryBonesAreTheFileBones )
{
    const auto file = FileData();
    ASSERT_FALSE( file.Bones.empty() );
    const auto rig = Geometry::LoadHumanoidSkeleton();
    ASSERT_NE( rig, nullptr );

    const auto& bones = rig->GetBones();
    ASSERT_EQ( bones.size(), file.Bones.size() );
    for ( size_t i = 0; i < bones.size(); ++i )
    {
        SCOPED_TRACE( file.Bones[i].Name );
        EXPECT_EQ( bones[i].Name, file.Bones[i].Name );
        EXPECT_EQ( bones[i].ParentBoneID, file.Bones[i].ParentBoneID );
        EXPECT_TRUE( bones[i].LocalBindTransform == file.Bones[i].LocalBindTransform );
        EXPECT_TRUE( bones[i].OffsetMatrix == file.Bones[i].OffsetMatrix );
    }
    EXPECT_EQ( rig->GetSignature(), file.Signature ) << "the rig in memory is not the rig the file names";
}

TEST_F( HumanoidSkeleton, EveryBoneTheBodyNamesIsInTheRig )
{
    const auto rig = Geometry::LoadHumanoidSkeleton();
    ASSERT_NE( rig, nullptr );
    for ( const auto& seg : Geometry::HumanoidSegments() )
    {
        EXPECT_TRUE( rig->FindBoneIndex( seg.BoneA ).has_value() ) << seg.BoneA;
        EXPECT_TRUE( rig->FindBoneIndex( seg.BoneB ).has_value() ) << seg.BoneB;
        EXPECT_TRUE( rig->FindBoneIndex( seg.SkinBone ).has_value() ) << seg.SkinBone;
    }
    for ( const auto& sphere : Geometry::HumanoidSpheres() )
        EXPECT_TRUE( rig->FindBoneIndex( sphere.Bone ).has_value() ) << sphere.Bone;
    for ( const char* foot : Geometry::HumanoidFeet() )
        EXPECT_TRUE( rig->FindBoneIndex( foot ).has_value() ) << foot;
}

TEST_F( HumanoidSkeleton, NoFileMeansNoRigNotAFallback )
{
    const fs::path empty = fs::temp_directory_path() / "humanoid_skeleton_no_mount";
    fs::create_directories( empty );
    fs::current_path( empty );
    EXPECT_EQ( Geometry::LoadHumanoidSkeleton(), nullptr );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
