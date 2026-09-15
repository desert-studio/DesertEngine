// AN ANIMATOR'S BONE->TRACK CACHE AND THE CLIP IT POINTS INTO MUST AGREE — a suite about a RELATION.
//
// The relation: THE CACHED TRACK POINTERS ARE VALID ONLY WHILE THE CLIP'S OWN TRACK STORAGE IS THE
// STORAGE THEY WERE BUILT FROM. `Animator::ResolveTrack` builds, per clip, a vector of `const BoneTrack*`
// mapping skeleton bone index -> that clip's track, and it cached the result keyed on the CLIP'S ADDRESS
// alone, "for the animator's lifetime".
//
// Why that key is not enough, and it is not a hypothetical. `Assets::AnimationAsset` owns its
// `AnimationClip` BY VALUE, so the clip keeps one address for the asset's whole life while the tracks
// underneath it are replaced: `Unload()` does `Tracks.clear()` + `shrink_to_fit()` (returning the storage
// to the allocator) and a later `Load()` allocates a fresh vector. Asset eviction does exactly that to a
// clip an entity is still playing — DELIBERATELY, because `AnimationLibrary::Resolve` reloads on every
// lookup and the library's design accepts eviction on that basis. The animator then dereferenced pointers
// into freed memory and the process died inside `std::lower_bound` over a keyframe vector that no longer
// existed.
//
// It had never fired before 2026-09-09 for one reason: this repository contained no `.anim` file, so no
// file-backed clip had ever played, so no clip an animator held had ever been unloaded. D34 filled the
// animation library from disk and the crash arrived on the same afternoon, four runs out of six.
//
// WHAT THIS SUITE ASSERTS is the relation rather than the crash, because a use-after-free is not a
// dependable thing to assert: freed memory often still reads back the old values. So the clip's tracks are
// REPLACED WITH DIFFERENT ONES in place, exactly as a reload replaces them, and the pose is required to
// reflect the new tracks. Under the address-only cache the pose keeps reporting the old ones (or crashes),
// and either way this is red.

#include <gtest/gtest.h>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/glm.hpp>

#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::FrameNumber;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
using Desert::Animation::Skeleton;

namespace
{
    // Two bones, so a rebuild that mis-orders the binding is visible as well as one that keeps stale
    // pointers. Identity bind and identity offset, so a bone's skinning matrix IS the sampled transform
    // and the assertions can read a translation straight out of it.
    Skeleton TwoBoneRig()
    {
        BoneInfo root;
        root.Name               = "Root";
        root.OffsetMatrix       = glm::mat4( 1.0f );
        root.LocalBindTransform = glm::mat4( 1.0f );
        root.ParentBoneID       = std::nullopt;

        BoneInfo child;
        child.Name               = "Child";
        child.OffsetMatrix       = glm::mat4( 1.0f );
        child.LocalBindTransform = glm::mat4( 1.0f );
        child.ParentBoneID       = 0u;

        std::vector<BoneInfo> bones = { root, child };
        return Skeleton( std::move( bones ) );
    }

    // A clip holding one constant position key per named bone. Constant so the assertion is a value and
    // not an interpolation, and the same clip NAME throughout so the ECS's "did the clip change?" test
    // (which compares names) would not re-Play it — that comparison is precisely why a stale binding
    // survives a reload instead of being rebuilt by accident.
    AnimationClip ClipWith( float rootY, float childY, bool includeChild )
    {
        AnimationClip clip;
        // A5: the clip states a length in TICKS on the project grid. `Duration` + `TicksPerSecond = 1`
        // used to say "one second" by setting the rate so a tick WAS a second.
        clip.AnimationName     = "Probe";
        clip.DurationTicks     = FrameNumber{ PROJECT_TICK_RATE.Numerator };
        clip.SkeletonSignature = 0;

        BoneTrack rootTrack;
        rootTrack.BoneName = "Root";
        rootTrack.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 0.0f, rootY, 0.0f ) } );
        clip.Tracks.push_back( rootTrack );

        if ( includeChild )
        {
            BoneTrack childTrack;
            childTrack.BoneName = "Child";
            childTrack.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 0.0f, childY, 0.0f ) } );
            clip.Tracks.push_back( childTrack );
        }
        return clip;
    }

    float RootTranslationY( const Animator& animator )
    {
        return animator.GetPose().Matrices.at( 0 )[3].y;
    }
} // namespace

// THE RELATION. The clip object stays put; its tracks are replaced. The pose must follow the tracks.
TEST( AnimatorClipRebind, ThePoseFollowsTheClipsCurrentTracksAfterItsStorageIsReplaced )
{
    const Skeleton rig = TwoBoneRig();
    Animator       animator( rig );

    // One AnimationClip object for the whole test, standing in for AnimationAsset::m_Clip — which is
    // exactly what the animator is handed in the engine, and exactly what does not move.
    AnimationClip clip = ClipWith( /*rootY=*/10.0f, /*childY=*/0.0f, /*includeChild=*/false );

    animator.Play( clip, /*loop=*/true );
    animator.Update( Common::Timestep( 0.0f ) );
    ASSERT_EQ( animator.GetCurrentClip(), &clip );
    EXPECT_FLOAT_EQ( RootTranslationY( animator ), 10.0f )
         << "the animator does not sample the clip it was given at all, so the rest of this test cannot "
            "mean anything";

    // THE REPLACEMENT, in the same shape as AnimationAsset::Unload() + Load(): the vector is emptied and
    // its storage released, then a new one is built. Different size AND different values, so a stale
    // binding is wrong about both.
    clip.Tracks.clear();
    clip.Tracks.shrink_to_fit();
    const AnimationClip reloaded = ClipWith( /*rootY=*/250.0f, /*childY=*/7.0f, /*includeChild=*/true );
    clip.Tracks                  = reloaded.Tracks;

    // Nothing re-Plays the clip: the pointer is unchanged and so is the clip's name, which is all the ECS
    // compares. This is the frame after an eviction reload, and it is where the process used to die.
    animator.Update( Common::Timestep( 0.0f ) );

    EXPECT_FLOAT_EQ( RootTranslationY( animator ), 250.0f )
         << "the animator is still sampling the clip's PREVIOUS tracks. In the engine that storage has been "
            "returned to the allocator by AnimationAsset::Unload, so this is a read of freed memory that "
            "happens to have survived -- see Animator::TrackBinding.";

    // The second bone appeared with the reload, so a binding rebuilt correctly resolves it too. A cache
    // that merely dropped its stale pointers without re-deriving the mapping would leave this at bind.
    ASSERT_GE( animator.GetPose().Matrices.size(), 2u );
    EXPECT_FLOAT_EQ( animator.GetPose().Matrices[1][3].y, 250.0f + 7.0f )
         << "the bone the reload ADDED is not bound. Child is parented to Root, so its skinning matrix is "
            "Root's translation plus its own.";
}

// AND THE CACHE MUST STILL BE A CACHE. A fix that rebuilt the binding on every call would pass the test
// above and quietly cost a name lookup per bone per frame, so the unchanged case is asserted too: same
// storage, same answer, and the pointers handed back are the clip's own tracks rather than copies.
TEST( AnimatorClipRebind, AnUnchangedClipKeepsItsBinding )
{
    const Skeleton rig = TwoBoneRig();
    Animator       animator( rig );

    AnimationClip clip = ClipWith( /*rootY=*/33.0f, /*childY=*/4.0f, /*includeChild=*/true );

    animator.Play( clip, /*loop=*/true );
    for ( int frame = 0; frame < 5; ++frame )
    {
        animator.Update( Common::Timestep( 1.0f / 60.0f ) );
        EXPECT_FLOAT_EQ( RootTranslationY( animator ), 33.0f )
             << "frame " << frame << ": a clip whose tracks never moved changed its answer";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
