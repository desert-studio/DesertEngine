// GROUP 9a OF THE TIMELINE CORE'S CONTRACT: the Layered Blend Per Bone node (ANIM-I13), moved out of
// timeline_contract_test.cpp when its implementation (Graph/LayeredBlendPerBone.cpp) landed.

#include "TimelineFixtures.hpp"

#include <glm/gtc/quaternion.hpp>

using namespace TimelineFixtures;

// ── 9a. Layered Blend Per Bone ────────────────────────────────────────────────────────────────────────

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

namespace
{
    G::LayeredBlendPerBoneNode SpineLayer()
    {
        G::LayeredBlendPerBoneNode node;
        node.Layers = { G::LayerSetup{ { G::BranchFilter{ "spine", 0 } } } };
        return node;
    }
} // namespace

TEST( LayeredBlendPerBone, TheLaterLayerWinsABoneBothReach )
{
    G::LayeredBlendPerBoneNode node;
    node.Layers  = { G::LayerSetup{ { G::BranchFilter{ "root", 0 } } },
                     G::LayerSetup{ { G::BranchFilter{ "chest", 0 } } } };
    const auto w = G::BuildPerBoneWeights( node, FiveBones() );
    ASSERT_TRUE( w.IsSuccess() );
    EXPECT_EQ( w.GetValue()[1].Layer, 0 );
    EXPECT_EQ( w.GetValue()[2].Layer, 1 );
    EXPECT_EQ( w.GetValue()[3].Layer, 1 );
    EXPECT_EQ( w.GetValue()[4].Layer, 0 );
}

TEST( LayeredBlendPerBone, MeshSpaceRotationKeepsTheLayersHeadingUnderABentBase )
{
    const Skeleton             skeleton = FiveBones();
    G::LayeredBlendPerBoneNode node;
    node.Layers                 = { G::LayerSetup{ { G::BranchFilter{ "chest", 0 } } } };
    node.MeshSpaceRotationBlend = true;
    const auto weights          = G::BuildPerBoneWeights( node, skeleton );
    ASSERT_TRUE( weights.IsSuccess() );

    // The base bends the spine 90 degrees; the layer's chest is straight in the MESH (identity everywhere).
    const glm::quat bend = glm::angleAxis( glm::radians( 90.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) );
    G::GraphPose    base;
    base.Pose             = LocalPose( 5 );
    base.Pose[1].Rotation = bend;
    G::GraphPose layer;
    layer.Pose = LocalPose( 5 );

    const G::GraphPose layers[] = { layer };
    const float        one[]    = { 1.0F };
    G::GraphPose       out;
    ASSERT_TRUE(
         G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, layers, one, out ).IsSuccess() );
    // Mesh-space chest = spine(bend) * local chest must be the layer's mesh rotation (identity).
    const glm::quat meshChest = out.Pose[1].Rotation * out.Pose[2].Rotation;
    EXPECT_NEAR( std::abs( glm::dot( meshChest, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) ) ), 1.0F, 1e-5F );

    // In LOCAL space the same layer copies the local identity, and the chest follows the bent spine.
    node.MeshSpaceRotationBlend = false;
    ASSERT_TRUE(
         G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, layers, one, out ).IsSuccess() );
    EXPECT_NEAR( std::abs( glm::dot( out.Pose[1].Rotation * out.Pose[2].Rotation, bend ) ), 1.0F, 1e-5F );
}

TEST( LayeredBlendPerBone, RootMotionFollowsTheLayersWeightOnTheRootBone )
{
    const Skeleton             skeleton = FiveBones();
    G::LayeredBlendPerBoneNode node     = SpineLayer(); // the spine layer does not reach the root
    const auto                 weights  = G::BuildPerBoneWeights( node, skeleton );
    ASSERT_TRUE( weights.IsSuccess() );

    G::GraphPose base;
    base.Pose                   = LocalPose( 5 );
    base.RootMotion.Translation = glm::vec3( 1.0F, 0.0F, 0.0F );
    G::GraphPose layer;
    layer.Pose                   = LocalPose( 5 );
    layer.RootMotion.Translation = glm::vec3( 0.0F, 0.0F, 5.0F );
    const G::GraphPose layers[]  = { layer };
    const float        one[]     = { 1.0F };
    G::GraphPose       out;

    ASSERT_TRUE(
         G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, layers, one, out ).IsSuccess() );
    EXPECT_EQ( out.RootMotion.Translation, base.RootMotion.Translation ) << "an upper-body layer never steers";

    node.BlendRootMotionBasedOnRootBone = false;
    ASSERT_TRUE(
         G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, layers, one, out ).IsSuccess() );
    EXPECT_EQ( out.RootMotion.Translation, layer.RootMotion.Translation );
}

TEST( LayeredBlendPerBone, CurvesCombineByTheNodesRule )
{
    const Skeleton             skeleton = FiveBones();
    G::LayeredBlendPerBoneNode node     = SpineLayer();
    const auto                 weights  = G::BuildPerBoneWeights( node, skeleton );
    ASSERT_TRUE( weights.IsSuccess() );

    G::GraphPose base;
    base.Pose        = LocalPose( 5 );
    base.CurveNames  = { "Blink" };
    base.CurveValues = { 0.2F };
    G::GraphPose layer;
    layer.Pose                  = LocalPose( 5 );
    layer.CurveNames            = { "Blink" };
    layer.CurveValues           = { 0.8F };
    const G::GraphPose layers[] = { layer };
    const float        half[]   = { 0.5F };
    G::GraphPose       out;

    const auto blinkWith = [&]( G::CurveBlendOption option )
    {
        node.CurveBlend = option;
        EXPECT_TRUE(
             G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, layers, half, out ).IsSuccess() );
        return out.CurveValues.empty() ? -1.0F : out.CurveValues[0];
    };
    EXPECT_FLOAT_EQ( blinkWith( G::CurveBlendOption::Override ), 0.8F );
    EXPECT_FLOAT_EQ( blinkWith( G::CurveBlendOption::DoNotOverride ), 0.2F );
    EXPECT_FLOAT_EQ( blinkWith( G::CurveBlendOption::UseBasePose ), 0.2F );
    EXPECT_FLOAT_EQ( blinkWith( G::CurveBlendOption::UseMaxValue ), 0.8F );
    EXPECT_FLOAT_EQ( blinkWith( G::CurveBlendOption::UseMinValue ), 0.2F );
    EXPECT_FLOAT_EQ( blinkWith( G::CurveBlendOption::BlendByWeight ), 0.2F + 0.4F );
    EXPECT_FLOAT_EQ( blinkWith( G::CurveBlendOption::NormalizeByWeight ), 0.6F / 1.5F );
}
