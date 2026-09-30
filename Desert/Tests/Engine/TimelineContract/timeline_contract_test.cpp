// THE TIMELINE CORE'S CONTRACT, WRITTEN BEFORE ITS IMPLEMENTATION (ANIM-UNIFY step 0).
//
// Every test here is against a SIGNATURE in Engine/Animation/Timeline/ and Engine/Animation/Graph/
// {LayeredBlendPerBone,LinkedAnimLayer}.hpp. At step 0 the suite compiles and does not link — the
// implementation pieces make it link one group at a time, and a piece is done when its group is green.
//
// The groups, in the order the pieces land:
//   1. enums and on-disk integers (pure static_asserts — green from day one);
//   2. channels: defaults, sampling, rotation = RotationKeyFrame's slerp bit for bit, events crossed;
//   3. sections/tracks/sequence: WeightAt, the fold, Validate's refusals, host restrictions;
//   4. the Player's time;
//   5. the Evaluator and the host seam (resolve once, unresolved REPORTED);
//   6. easing presets (the UI key model's replacement);
//   7. serialization round trip;
//   8. LiftClip: the migration is the identity, bit for bit;
//   9. LayeredBlendPerBone and LinkedAnimLayer.
//
// ONE FILE PER LANDED GROUP: groups 1-2 (timeline_channel_test.cpp), 3 (timeline_section_test.cpp, the
// fold in timeline_evaluator_test.cpp), 4 (timeline_player_test.cpp), 5 and 7 (timeline_evaluator_test.cpp)
// and 6 (timeline_easing_test.cpp) have their implementation and are built; this file holds the groups
// whose pieces have not landed, and joins the suite's `files` (premake5.lua) with them — excluded until
// then, never stubbed.

#include "TimelineFixtures.hpp"

using namespace TimelineFixtures;

// ── 8. LiftClip: the .anim migration is the identity ────────────────────────────────────────────────

TEST( TimelineLiftClip, EveryBoneSamplesBitForBitAfterTheLift )
{
    AnimationClip clip;
    clip.AnimationName = "Walk";
    clip.DurationTicks = Tick( 60 );
    BoneTrack spine;
    spine.BoneName     = "Spine";
    spine.PositionKeys = { PositionKeyFrame{ Tick( 0 ), glm::vec3( 0, 1, 0 ) },
                           PositionKeyFrame{ Tick( 60 ), glm::vec3( 3, 1, -2 ) } };
    spine.RotationKeys = { RotationKeyFrame{ Tick( 0 ), glm::quat( 1, 0, 0, 0 ) },
                           RotationKeyFrame{ Tick( 60 ), glm::angleAxis( 1.1F, glm::vec3( 0, 0, 1 ) ) } };
    clip.Tracks.push_back( spine );
    clip.Curves.push_back( AnimationCurve{ "Footstep", { Key( 0, 0.0F ), Key( 60, 1.0F ) } } );
    clip.Notifies.push_back( AnimationNotify{ "Step", Tick( 30 ), 2, Tick( 0 ) } );

    const auto lifted = LiftClip( clip );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    const Sequence& sequence = lifted.GetValue();
    EXPECT_EQ( sequence.Host, SequenceHost::AnimationClip );
    EXPECT_TRUE( Validate( sequence ).IsSuccess() );

    std::vector<BoneInfo> bones( 1 );
    bones[0].Name = "Spine";
    const Skeleton         skeleton( std::move( bones ) );
    const BoneBindingTable table = BindBones( sequence, skeleton );
    EXPECT_EQ( table.Missing, 0U );

    for ( const FrameTime at : { At( 0 ), At( 7, 0.5F ), At( 30 ), At( 59, 0.99F ), At( 60 ) } )
    {
        LocalPose pose( 1 );
        ASSERT_TRUE( EvaluatePose( sequence, table, at, pose ).IsSuccess() );
        const BoneTransform expected = spine.Sample( at, clip.TickRate );
        EXPECT_EQ( pose[0].Translation, expected.Translation ) << at.AsTicks();
        EXPECT_EQ( pose[0].Rotation, expected.Rotation ) << at.AsTicks();
        EXPECT_EQ( pose[0].Scale, expected.Scale ) << at.AsTicks();
    }

    int curveTracks = 0;
    for ( const Track& track : sequence.Tracks )
    {
        const Binding* binding = FindBinding( sequence, track.Binding );
        ASSERT_NE( binding, nullptr );
        curveTracks += ( track.Kind == TrackKind::Float && track.Property == "Footstep" &&
                         binding->Kind == BindingKind::Sequence )
                            ? 1
                            : 0;
    }
    EXPECT_EQ( curveTracks, 1 ) << "a named curve is a Float track on the sequence binding";
    int eventTracks = 0;
    for ( const Track& track : sequence.Tracks )
    {
        eventTracks += track.Kind == TrackKind::Event ? 1 : 0;
    }
    EXPECT_EQ( eventTracks, 1 ) << "notifies become ONE event track, rows kept on the keys";
}

TEST( TimelineLiftClip, TwoTracksForOneBoneAreRefusedNotMerged )
{
    AnimationClip clip;
    clip.DurationTicks = Tick( 10 );
    BoneTrack a;
    a.BoneName = "Hand_L";
    clip.Tracks.push_back( a );
    clip.Tracks.push_back( a );
    EXPECT_FALSE( LiftClip( clip ).IsSuccess() );
}

// ── 9. AnimGraph layer nodes ────────────────────────────────────────────────────────────────────────

namespace
{
    // root(0) ─ spine(1) ─ chest(2) ─ head(3)
    //        └ leg(4)
    Skeleton FiveBones()
    {
        std::vector<BoneInfo> bones( 5 );
        const char*           names[]   = { "root", "spine", "chest", "head", "leg" };
        const int             parents[] = { -1, 0, 1, 2, 0 };
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            bones[i].Name = names[i];
            if ( parents[i] >= 0 )
            {
                bones[i].ParentBoneID = static_cast<uint32_t>( parents[i] );
            }
        }
        return Skeleton( std::move( bones ) );
    }
} // namespace

TEST( LayeredBlendPerBone, DepthZeroIsTheWholeBranchAtFullWeight )
{
    G::LayeredBlendPerBoneNode node;
    node.Layers        = { G::LayerSetup{ { G::BranchFilter{ "spine", 0 } } } };
    const auto weights = G::BuildPerBoneWeights( node, FiveBones() );
    ASSERT_TRUE( weights.IsSuccess() ) << weights.GetError();
    const auto& w = weights.GetValue();
    ASSERT_EQ( w.size(), 5U );
    EXPECT_EQ( w[0].Layer, -1 );
    EXPECT_EQ( w[1].Weight, 1.0F );
    EXPECT_EQ( w[3].Weight, 1.0F );
    EXPECT_EQ( w[4].Layer, -1 ) << "the leg is not under the spine";
}

TEST( LayeredBlendPerBone, APositiveDepthRampsAndANegativeOneExcludes )
{
    G::LayeredBlendPerBoneNode ramp;
    ramp.Layers  = { G::LayerSetup{ { G::BranchFilter{ "spine", 2 } } } };
    const auto r = G::BuildPerBoneWeights( ramp, FiveBones() );
    ASSERT_TRUE( r.IsSuccess() );
    EXPECT_FLOAT_EQ( r.GetValue()[1].Weight, 0.5F );
    EXPECT_FLOAT_EQ( r.GetValue()[2].Weight, 1.0F );

    G::LayeredBlendPerBoneNode exclude;
    exclude.Layers = { G::LayerSetup{ { G::BranchFilter{ "spine", 0 }, G::BranchFilter{ "head", -1 } } } };
    const auto e   = G::BuildPerBoneWeights( exclude, FiveBones() );
    ASSERT_TRUE( e.IsSuccess() );
    EXPECT_EQ( e.GetValue()[2].Weight, 1.0F );
    EXPECT_EQ( e.GetValue()[3].Weight, 0.0F );
}

TEST( LayeredBlendPerBone, AnUnknownFilterBoneIsRefusedByName )
{
    G::LayeredBlendPerBoneNode node;
    node.Layers        = { G::LayerSetup{ { G::BranchFilter{ "spien", 0 } } } };
    const auto weights = G::BuildPerBoneWeights( node, FiveBones() );
    ASSERT_FALSE( weights.IsSuccess() );
    EXPECT_NE( weights.GetError().find( "spien" ), std::string::npos );
}

TEST( LayeredBlendPerBone, ZeroLayerWeightIsTheBaseBitForBitAndSizesMustAgree )
{
    const Skeleton             skeleton = FiveBones();
    G::LayeredBlendPerBoneNode node;
    node.Layers        = { G::LayerSetup{ { G::BranchFilter{ "spine", 0 } } } };
    const auto weights = G::BuildPerBoneWeights( node, skeleton );
    ASSERT_TRUE( weights.IsSuccess() );

    G::GraphPose base;
    base.Pose                = LocalPose( 5 );
    base.Pose[2].Translation = glm::vec3( 1.25F, 0.5F, 0.0F );
    G::GraphPose layer;
    layer.Pose                = LocalPose( 5 );
    layer.Pose[2].Translation = glm::vec3( 9.0F );

    const G::GraphPose layers[] = { layer };
    const float        zero[]   = { 0.0F };
    G::GraphPose       out;
    ASSERT_TRUE(
         G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, layers, zero, out ).IsSuccess() );
    EXPECT_EQ( out.Pose[2].Translation, base.Pose[2].Translation );

    EXPECT_FALSE( G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, {}, {}, out ).IsSuccess() );
}

TEST( LinkedAnimLayer, UnlinkedIsTheDefaultAndAHalfImplementationIsRefused )
{
    G::AnimLayerInterface locomotion;
    locomotion.Guid      = AssetGuid{ 7, 1 };
    locomotion.Functions = { G::AnimLayerFunction{ "FullBody", { "In" }, "" },
                             G::AnimLayerFunction{ "UpperBody", { "In" }, "" } };
    G::LinkedLayerTable          table;
    const G::LinkedAnimLayerNode node{ locomotion.Guid, "UpperBody" };
    EXPECT_TRUE( table.Resolve( node ).IsNull() );

    const G::LayerImplementation half{ AssetGuid{ 8, 1 }, locomotion.Guid, { "FullBody" } };
    const auto                   refused = table.Link( locomotion, half );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "UpperBody" ), std::string::npos );

    const G::LayerImplementation other{ AssetGuid{ 8, 2 }, AssetGuid{ 6, 6 }, { "FullBody", "UpperBody" } };
    EXPECT_FALSE( table.Link( locomotion, other ).IsSuccess() ) << "implements another interface";

    const G::LayerImplementation rifle{ AssetGuid{ 8, 3 }, locomotion.Guid, { "FullBody", "UpperBody" } };
    ASSERT_TRUE( table.Link( locomotion, rifle ).IsSuccess() );
    EXPECT_EQ( table.Resolve( node ), rifle.Graph );

    table.Unlink( locomotion.Guid );
    EXPECT_TRUE( table.Resolve( node ).IsNull() );
}
}
