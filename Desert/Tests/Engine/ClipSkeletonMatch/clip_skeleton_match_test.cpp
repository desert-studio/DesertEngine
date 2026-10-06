// THE PICKER AND THE RUNTIME MUST AGREE, and this suite asserts the AGREEMENT rather than either side.
//
// The defect: the editor's clip pickers matched a clip to a rig TOLERANTLY (bone names), the runtime matched
// EXACTLY (skeleton signature). Each side was individually defensible, so a unit test of either passed. For
// Mixamo content the two answers differ by construction — a rig exported WITH a skin carries leaf/end bones
// the animation-only export never mentions, so the signatures differ while the names still line up — and an
// AnimGraph state pointing at a clip the picker had just offered resolved to nothing, silently.
//
// Both sides now go through ClipSkeletonMatch. The load-bearing test here is AgreesWithRuntimeResolution:
// every clip SelectClipsForRig offers must be findable by name through FindClipForRig.

#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using Desert::Animation::BoneInfo;
using Desert::Animation::ClipDrivesRig;
using Desert::Animation::ClipRigIdentity;
using Desert::Animation::FindClipForRig;
using Desert::Animation::IdentifyRig;
using Desert::Animation::RigIdentity;
using Desert::Animation::SelectClipsForRig;
using Desert::Animation::Skeleton;

namespace
{
    // A Mixamo-shaped chain. `withEndBones` reproduces the difference between the two exports of ONE rig:
    // the skinned character carries the "_end" leaf bones, the animation-only file does not.
    Skeleton MixamoRig( bool withEndBones )
    {
        const std::vector<std::string> core = { "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1",
                                                "mixamorig:Neck", "mixamorig:Head" };
        const std::vector<std::string> ends = { "mixamorig:HeadTop_End", "mixamorig:LeftToe_End",
                                                "mixamorig:RightToe_End" };

        std::vector<BoneInfo> bones;
        for ( size_t i = 0; i < core.size(); ++i )
        {
            BoneInfo b;
            b.Name = core[i];
            if ( i > 0 )
                b.ParentBoneID = static_cast<uint32_t>( i - 1 );
            bones.push_back( std::move( b ) );
        }
        if ( withEndBones )
        {
            for ( const auto& name : ends )
            {
                BoneInfo b;
                b.Name         = name;
                b.ParentBoneID = static_cast<uint32_t>( core.size() - 1 );
                bones.push_back( std::move( b ) );
            }
        }
        return Skeleton( std::move( bones ) );
    }

    ClipRigIdentity Clip( const char* name, uint64_t signature, std::vector<std::string> bones )
    {
        ClipRigIdentity c;
        c.ClipName          = name;
        c.SkeletonSignature = signature;
        c.AnimatedBones     = std::move( bones );
        return c;
    }

    // The library as it looks after importing one Mixamo character and three Mixamo animations, plus one
    // clip from an entirely unrelated rig that must NOT be offered.
    std::vector<ClipRigIdentity> MixamoLibrary( uint64_t animExportSignature )
    {
        const std::vector<std::string> animated = { "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1",
                                                    "mixamorig:Neck", "mixamorig:Head" };
        return { Clip( "Idle", animExportSignature, animated ), Clip( "Walk", animExportSignature, animated ),
                 Clip( "Run", animExportSignature, animated ),
                 Clip( "RobotArmSwing", 0x5151515151515151ULL, { "Base", "Segment1", "Segment2", "Gripper" } ) };
    }
} // namespace

// The premise of the whole defect. If this ever stops holding, the fixture below stops testing anything, so
// it is asserted rather than assumed.
TEST( ClipSkeletonMatch, TheTwoMixamoExportsHaveDifferentSignatures )
{
    const Skeleton character = MixamoRig( true );
    const Skeleton animOnly  = MixamoRig( false );

    EXPECT_NE( character.GetSignature(), animOnly.GetSignature() )
         << "the fixture no longer reproduces the exact-vs-tolerant disagreement";
    EXPECT_EQ( character.GetBones().size(), 8u );
    EXPECT_EQ( animOnly.GetBones().size(), 5u );
}

TEST( ClipSkeletonMatch, ExactSignatureAloneMissesTheMixamoClip )
{
    const Skeleton    character = MixamoRig( true );
    const RigIdentity rig       = IdentifyRig( character );
    const auto        clips     = MixamoLibrary( MixamoRig( false ).GetSignature() );

    // What the OLD runtime did: signature equality only. This is the defect, pinned so the fix cannot be
    // quietly reverted into "it was always fine".
    for ( const auto& clip : clips )
        EXPECT_NE( clip.SkeletonSignature, rig.Signature );

    // What the one rule does instead.
    EXPECT_TRUE( ClipDrivesRig( clips[0], rig ) );
}

TEST( ClipSkeletonMatch, UnrelatedRigIsStillRejected )
{
    const RigIdentity rig   = IdentifyRig( MixamoRig( true ) );
    const auto        clips = MixamoLibrary( MixamoRig( false ).GetSignature() );

    EXPECT_FALSE( ClipDrivesRig( clips[3], rig ) ) << "tolerance must not become 'everything matches'";

    const auto found = FindClipForRig( clips, rig, "RobotArmSwing" );
    EXPECT_FALSE( found );
    EXPECT_NE( found.GetError().find( "RobotArmSwing" ), std::string::npos );
}

// THE RELATION. Not "the picker is right" and not "the runtime is right" — that they answer the same way.
TEST( ClipSkeletonMatch, AgreesWithRuntimeResolution )
{
    const RigIdentity rig   = IdentifyRig( MixamoRig( true ) );
    const auto        clips = MixamoLibrary( MixamoRig( false ).GetSignature() );

    const auto offered = SelectClipsForRig( clips, rig );
    ASSERT_FALSE( offered.empty() ) << "a picker that offers nothing cannot disagree with anything";

    for ( const size_t i : offered )
    {
        const auto found = FindClipForRig( clips, rig, clips[i].ClipName );
        EXPECT_TRUE( found ) << "the picker offers '" << clips[i].ClipName
                             << "' and the runtime cannot resolve it: " << found.GetError();
        if ( found )
            EXPECT_EQ( found.GetValue(), i );
    }
}

// The mirror direction: nothing the picker withheld may resolve behind its back.
TEST( ClipSkeletonMatch, NothingResolvesThatWasNotOffered )
{
    const RigIdentity rig     = IdentifyRig( MixamoRig( true ) );
    const auto        clips   = MixamoLibrary( MixamoRig( false ).GetSignature() );
    const auto        offered = SelectClipsForRig( clips, rig );

    for ( size_t i = 0; i < clips.size(); ++i )
    {
        const bool wasOffered = std::find( offered.begin(), offered.end(), i ) != offered.end();
        const bool resolves   = FindClipForRig( clips, rig, clips[i].ClipName ).IsSuccess();
        EXPECT_EQ( wasOffered, resolves ) << "clip '" << clips[i].ClipName << "'";
    }
}

TEST( ClipSkeletonMatch, RefusalNamesTheClipAndTheAlternatives )
{
    const RigIdentity rig   = IdentifyRig( MixamoRig( true ) );
    const auto        clips = MixamoLibrary( MixamoRig( false ).GetSignature() );

    const auto missing = FindClipForRig( clips, rig, "Sprint" );
    ASSERT_FALSE( missing );
    EXPECT_NE( missing.GetError().find( "Sprint" ), std::string::npos );
    EXPECT_NE( missing.GetError().find( "Idle" ), std::string::npos )
         << "an artist needs to see what the rig DOES have, or the message is just 'no'";

    const auto unnamed = FindClipForRig( clips, rig, "" );
    EXPECT_FALSE( unnamed ) << "an empty state clip name is a refusal, not a silent no-op";
}

// A clip that claims this exact rig is matched even before its tracks are known — the case the signature
// clause exists for, and the reason it was not simply deleted with the old exact-only lookup.
TEST( ClipSkeletonMatch, SignatureStillMatchesAClipWithNoNamedBones )
{
    const Skeleton    character = MixamoRig( true );
    const RigIdentity rig       = IdentifyRig( character );

    const std::vector<ClipRigIdentity> clips = { Clip( "NewClip", character.GetSignature(), {} ) };
    EXPECT_TRUE( ClipDrivesRig( clips[0], rig ) );
    EXPECT_TRUE( FindClipForRig( clips, rig, "NewClip" ) );
}

// 0 means "no rig claimed" and must never be treated as an identity two things can share.
TEST( ClipSkeletonMatch, UnclaimedSignatureIsNotAnIdentity )
{
    RigIdentity rig;
    rig.Signature = 0;

    const std::vector<ClipRigIdentity> clips = { Clip( "Orphan", 0, {} ) };
    EXPECT_FALSE( ClipDrivesRig( clips[0], rig ) );
    EXPECT_TRUE( SelectClipsForRig( clips, rig ).empty() );
}
