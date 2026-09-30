// THE PICKER AND THE RUNTIME MUST AGREE, and this suite asserts the AGREEMENT rather than either side.
//
// The defect this suite was born for: the editor's clip pickers matched a clip to a rig TOLERANTLY (bone
// names), the runtime EXACTLY (a bone hash), and an AnimGraph state pointing at a clip the picker had just
// offered resolved to nothing, silently. Since SKEL-TREE the skeleton is an asset: a clip and a mesh name it
// by its header GUID, and both sides ask ClipPlaysOnMesh (SkeletonReference.hpp) through ClipSkeletonMatch.
// The load-bearing test is AgreesWithRuntimeResolution: every clip SelectClipsForMesh offers must be
// findable by name through FindClipForMesh, and nothing it withheld may resolve.

#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Animation/SkeletonReference.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using Common::Content::AssetGuid;
using Desert::Animation::ClipRigIdentity;
using Desert::Animation::FindClipForMesh;
using Desert::Animation::MeshSkeletonIdentity;
using Desert::Animation::SelectClipsForMesh;
using Desert::Animation::SkeletonAssetRef;

namespace
{
    // Three skeleton assets. Fixed GUIDs, so a failure message is reproducible.
    const SkeletonAssetRef kMannequin{ AssetGuid{ 0x1111, 0x0001 }, "Mannequin" };
    const SkeletonAssetRef kMannequinLegacy{ AssetGuid{ 0x1111, 0x0002 }, "MannequinLegacy" };
    const SkeletonAssetRef kFox{ AssetGuid{ 0x2222, 0x0001 }, "Fox" };

    ClipRigIdentity Clip( const std::string& name, const SkeletonAssetRef& skeleton )
    {
        ClipRigIdentity c;
        c.ClipName = name;
        c.Skeleton = skeleton;
        return c;
    }

    // Walk, Run on the mannequin; LegacyIdle on a skeleton the mannequin declares compatible; Survey on the fox;
    // Orphan names no skeleton at all.
    std::vector<ClipRigIdentity> Library()
    {
        return { Clip( "Walk", kMannequin ), Clip( "LegacyIdle", kMannequinLegacy ), Clip( "Survey", kFox ),
                 Clip( "Run", kMannequin ), Clip( "Orphan", SkeletonAssetRef{} ) };
    }

    MeshSkeletonIdentity MannequinMesh()
    {
        return { kMannequin, { kMannequinLegacy.Guid } };
    }
} // namespace

TEST( ClipSkeletonMatch, TheSameSkeletonPlays )
{
    EXPECT_TRUE( ClipPlaysOnMesh( kMannequin, kMannequin, {} ).IsSuccess() );
}

// A foreign skeleton is refused, and the refusal names BOTH skeletons: the only two facts anyone needs to fix it.
TEST( ClipSkeletonMatch, AForeignSkeletonIsRefusedNamingBoth )
{
    const auto refused = ClipPlaysOnMesh( kFox, kMannequin, {} );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Fox" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "Mannequin" ), std::string::npos ) << refused.GetError();
}

// CompatibleSkeletons is ONE-WAY (UE USkeleton::CompatibleSkeletons): the mesh's skeleton listing the clip's
// lets that clip play; the reverse pairing does not follow from it.
TEST( ClipSkeletonMatch, CompatibleSkeletonsIsOneWay )
{
    const std::vector<AssetGuid> mannequinCompatible{ kMannequinLegacy.Guid };
    EXPECT_TRUE( ClipPlaysOnMesh( kMannequinLegacy, kMannequin, mannequinCompatible ).IsSuccess() );
    EXPECT_FALSE( ClipPlaysOnMesh( kMannequin, kMannequinLegacy, {} ).IsSuccess() )
         << "the legacy skeleton lists nothing, so the mannequin's clips must not play on it.";
    EXPECT_FALSE( ClipPlaysOnMesh( kFox, kMannequin, mannequinCompatible ).IsSuccess() );
}

// A null GUID is "no skeleton named", never an identity two things can share.
TEST( ClipSkeletonMatch, ANullReferenceIsNotAnIdentity )
{
    EXPECT_FALSE( ClipPlaysOnMesh( SkeletonAssetRef{}, SkeletonAssetRef{}, {} ).IsSuccess() );
    EXPECT_FALSE( ClipPlaysOnMesh( SkeletonAssetRef{}, kMannequin, {} ).IsSuccess() );
    EXPECT_FALSE( ClipPlaysOnMesh( kMannequin, SkeletonAssetRef{}, {} ).IsSuccess() );
}

// Bone names decide nothing at play time: two skeletons with the same name but different GUIDs are different.
TEST( ClipSkeletonMatch, TheNameIsNotTheIdentity )
{
    const SkeletonAssetRef impostor{ AssetGuid{ 0x9999, 0x0001 }, kMannequin.Name };
    EXPECT_FALSE( ClipPlaysOnMesh( impostor, kMannequin, {} ).IsSuccess() );
}

TEST( ClipSkeletonMatch, ThePickerOffersExactlyTheClipsThatPlay )
{
    EXPECT_EQ( SelectClipsForMesh( Library(), MannequinMesh() ), ( std::vector<size_t>{ 0, 1, 3 } ) );
}

// THE RELATION. Not "the picker is right" and not "the runtime is right" -- that they answer the same way.
TEST( ClipSkeletonMatch, AgreesWithRuntimeResolution )
{
    const auto clips = Library();
    for ( const MeshSkeletonIdentity& mesh : { MannequinMesh(), MeshSkeletonIdentity{ kFox, {} } } )
    {
        const auto offered = SelectClipsForMesh( clips, mesh );
        ASSERT_FALSE( offered.empty() );
        for ( const size_t i : offered )
        {
            const auto found = FindClipForMesh( clips, mesh, clips[i].ClipName );
            ASSERT_TRUE( found.IsSuccess() ) << "the picker offered '" << clips[i].ClipName
                                             << "' but the runtime cannot resolve it: " << found.GetError();
            EXPECT_EQ( found.GetValue(), i );
        }
    }
}

// The mirror direction: nothing the picker withheld may resolve behind its back.
TEST( ClipSkeletonMatch, NothingResolvesThatWasNotOffered )
{
    const auto clips   = Library();
    const auto mesh    = MannequinMesh();
    const auto offered = SelectClipsForMesh( clips, mesh );
    for ( size_t i = 0; i < clips.size(); ++i )
    {
        if ( std::find( offered.begin(), offered.end(), i ) != offered.end() )
            continue;
        EXPECT_FALSE( FindClipForMesh( clips, mesh, clips[i].ClipName ).IsSuccess() )
             << "'" << clips[i].ClipName << "' was withheld by the picker but resolves at runtime.";
    }
}

// Two failures, two fixes: a refused clip carries the rule's reason (both skeletons), an unknown one says so.
TEST( ClipSkeletonMatch, RefusalNamesTheClipAndBothSkeletons )
{
    const auto clips   = Library();
    const auto refused = FindClipForMesh( clips, MannequinMesh(), "Survey" );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Survey" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "Fox" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "Mannequin" ), std::string::npos ) << refused.GetError();

    const auto unknown = FindClipForMesh( clips, MannequinMesh(), "Swim" );
    ASSERT_FALSE( unknown.IsSuccess() );
    EXPECT_NE( unknown.GetError().find( "no clip named 'Swim'" ), std::string::npos ) << unknown.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
