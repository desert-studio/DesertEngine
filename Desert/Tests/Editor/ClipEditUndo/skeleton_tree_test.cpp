#include <gtest/gtest.h>

#include <Editor/Panels/AnimationEditor/SkeletonTree.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Editor;

namespace
{
    // root -> spine -> { upperarm_l -> lowerarm_l, upperarm_r -> lowerarm_r }
    Animation::Skeleton MakeSkeleton()
    {
        std::vector<Animation::BoneInfo> bones( 6 );
        const char* names[]   = { "root", "spine", "upperarm_l", "lowerarm_l", "upperarm_r", "lowerarm_r" };
        const int   parents[] = { -1, 0, 1, 2, 1, 4 };
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            bones[i].Name = names[i];
            if ( parents[i] >= 0 )
                bones[i].ParentBoneID = static_cast<uint32_t>( parents[i] );
        }
        return Animation::Skeleton( std::move( bones ) );
    }

    std::vector<std::string> Names( const Animation::Skeleton& s, const std::vector<SkeletonTreeRow>& rows )
    {
        std::vector<std::string> out;
        for ( const auto& row : rows )
            out.push_back( std::string( row.Depth, '.' ) + s.GetBones()[row.Bone].Name );
        return out;
    }
} // namespace

TEST( SkeletonTree, NameSelectsItsIndexAndTheIndexNamesIt )
{
    const auto skeleton = MakeSkeleton();
    for ( uint32_t i = 0; i < skeleton.GetBones().size(); ++i )
    {
        const auto found = BoneByName( skeleton, skeleton.GetBones()[i].Name );
        ASSERT_TRUE( found.has_value() ) << skeleton.GetBones()[i].Name;
        EXPECT_EQ( *found, i );
    }
    EXPECT_FALSE( BoneByName( skeleton, "lowerarm" ).has_value() ) << "a prefix is not a bone";
    EXPECT_FALSE( BoneByName( skeleton, "LOWERARM_L" ).has_value() ) << "names are exact";
}

TEST( SkeletonTree, RowsAreDepthFirstInIndexOrder )
{
    const auto skeleton = MakeSkeleton();
    const auto rows     = BuildSkeletonTreeRows( skeleton, "", {} );
    EXPECT_EQ( Names( skeleton, rows ),
               ( std::vector<std::string>{ "root", ".spine", "..upperarm_l", "...lowerarm_l", "..upperarm_r",
                                           "...lowerarm_r" } ) );
    EXPECT_TRUE( rows[0].HasChildren );
    EXPECT_FALSE( rows[3].HasChildren );
}

TEST( SkeletonTree, CollapsedBoneHidesItsSubtreeOnly )
{
    const auto        skeleton = MakeSkeleton();
    std::vector<bool> collapsed( 6, false );
    collapsed[2]    = true; // upperarm_l
    const auto rows = BuildSkeletonTreeRows( skeleton, "", collapsed );
    EXPECT_EQ( Names( skeleton, rows ),
               ( std::vector<std::string>{ "root", ".spine", "..upperarm_l", "..upperarm_r", "...lowerarm_r" } ) );
}

TEST( SkeletonTree, FilterKeepsMatchesAndTheirAncestorsIgnoringCollapse )
{
    const auto        skeleton = MakeSkeleton();
    std::vector<bool> collapsed( 6, true );
    const auto        rows = BuildSkeletonTreeRows( skeleton, "LowerArm_L", collapsed );
    EXPECT_EQ( Names( skeleton, rows ),
               ( std::vector<std::string>{ "root", ".spine", "..upperarm_l", "...lowerarm_l" } ) );
    EXPECT_FALSE( rows[0].Matches );
    EXPECT_TRUE( rows[3].Matches );
    EXPECT_TRUE( BuildSkeletonTreeRows( skeleton, "nope", {} ).empty() );
}

TEST( SkeletonTree, DecomposeReadsLocationRotationScale )
{
    const glm::mat4 m = glm::translate( glm::mat4( 1.0f ), glm::vec3( 1.0f, 2.0f, 3.0f ) ) *
                        glm::rotate( glm::mat4( 1.0f ), glm::radians( 90.0f ), glm::vec3( 0.0f, 0.0f, 1.0f ) ) *
                        glm::scale( glm::mat4( 1.0f ), glm::vec3( 2.0f ) );
    const auto rows = DecomposeBoneTransform( m );
    EXPECT_NEAR( rows.Location.y, 2.0f, 1e-4f );
    EXPECT_NEAR( rows.RotationDegrees.z, 90.0f, 1e-3f );
    EXPECT_NEAR( rows.Scale.x, 2.0f, 1e-4f );
}
