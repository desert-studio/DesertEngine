// GROUP 9a OF THE TIMELINE CORE'S CONTRACT: the Layered Blend Per Bone node (ANIM-I13), moved out of
// timeline_contract_test.cpp when its implementation (Graph/LayeredBlendPerBone.cpp) landed.

#include "TimelineFixtures.hpp"

#include <glm/gtc/quaternion.hpp>

#include <Engine/Animation/Graph/PoseGraphInstance.hpp>

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
        node.CurveBlend = static_cast<int>( option );
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

// ── 9b. The pose graph evaluated node by node (ANIM-I13b): Apply Additive, a blend of blends ─────────────

TEST( ApplyAdditive, AddsTheDifferenceFromTheReferenceScaledByAlpha )
{
    G::GraphPose base;
    base.Pose                = LocalPose( 5 );
    base.Pose[1].Translation = glm::vec3( 1.0F, 0.0F, 0.0F );
    base.CurveNames          = { "Blink" };
    base.CurveValues         = { 0.25F };
    G::GraphPose additive;
    additive.Pose                = LocalPose( 5 );
    additive.Pose[1].Translation = glm::vec3( 0.0F, 5.0F, 0.0F );
    additive.Pose[2].Rotation    = glm::angleAxis( glm::radians( 90.0F ), glm::vec3( 0.0F, 1.0F, 0.0F ) );
    additive.CurveNames          = { "Blink", "Jaw" };
    additive.CurveValues         = { 0.5F, 1.0F };
    LocalPose reference( 5 );
    reference[1].Translation = glm::vec3( 0.0F, 2.0F, 0.0F );

    G::GraphPose out;
    ASSERT_TRUE( G::ApplyAdditive( base, additive, reference, 0.5F, out ).IsSuccess() );
    EXPECT_NEAR( glm::length( out.Pose[1].Translation - glm::vec3( 1.0F, 1.5F, 0.0F ) ), 0.0F, 1e-5F )
         << "half of the additive's difference from the reference (5 - 2) on top of the base";
    const float angle = glm::degrees( glm::angle( out.Pose[2].Rotation ) );
    EXPECT_NEAR( angle, 45.0F, 1e-3F ) << "alpha 0.5 of a 90 degree additive rotation";
    ASSERT_EQ( out.CurveNames.size(), 2U );
    EXPECT_NEAR( out.CurveValues[0], 0.5F, 1e-6F ) << "a curve both have: base + alpha x additive";
    EXPECT_NEAR( out.CurveValues[1], 0.5F, 1e-6F ) << "a curve only the additive has enters from 0";

    ASSERT_TRUE( G::ApplyAdditive( base, additive, reference, 0.0F, out ).IsSuccess() );
    EXPECT_EQ( out.Pose[1].Translation, base.Pose[1].Translation ) << "alpha 0 is the base bit for bit";
    EXPECT_EQ( out.Pose[2].Rotation, base.Pose[2].Rotation );

    EXPECT_FALSE( G::ApplyAdditive( base, additive, LocalPose( 4 ), 1.0F, out ).IsSuccess() );
}

namespace
{
    /// Leaf node i poses every bone at translation (i + 1, 0, 0), so the output says which leaf reached a bone.
    G::PoseGraphSources NumberedLeaves( const LocalPose& reference )
    {
        G::PoseGraphSources sources;
        sources.Sample = []( size_t node, G::GraphPose& out )
        {
            for ( size_t b = 0; b < out.Pose.Size(); ++b )
                out.Pose[b].Translation = glm::vec3( static_cast<float>( node + 1 ), 0.0F, 0.0F );
        };
        sources.Parameter         = []( const std::string& ) { return 0.5F; };
        sources.AdditiveReference = &reference;
        return sources;
    }

    G::PoseNode Leaf( std::string name )
    {
        G::PoseNode node;
        node.Name     = std::move( name );
        node.Kind     = static_cast<int>( G::PoseNodeKind::SequencePlayer );
        node.Sequence = G::SequencePlayerNode{ .Clip = "clip", .Loop = true };
        return node;
    }

    G::PoseNode Blend( std::string name, std::string base, std::string layer, std::string filterBone )
    {
        G::PoseNode node;
        node.Name         = std::move( name );
        node.Kind         = static_cast<int>( G::PoseNodeKind::LayeredBlendPerBone );
        node.PoseInputs   = { std::move( base ), std::move( layer ) };
        node.LayeredBlend = G::LayeredBlendPerBoneNode{};
        node.LayeredBlend->Layers.push_back( G::LayerSetup{ { G::BranchFilter{ std::move( filterBone ), 0 } } } );
        return node;
    }
} // namespace

TEST( PoseGraphInstance, ABlendOfBlendsEvaluatesEachNodeFromItsInputsPoses )
{
    const Skeleton skeleton = FiveBones();
    const uint32_t spine    = skeleton.FindBoneIndex( "spine" ).value();

    G::AnimGraph graph;
    graph.Name = "BlendOfBlends";
    graph.Parameters.push_back(
         G::Parameter{ .Name = "W", .Type = static_cast<int>( G::ParamType::Float ), .Default = 1.0F } );
    graph.Nodes = { Leaf( "L0" ), Leaf( "L1" ), Leaf( "L2" ),
                    Blend( "Inner", "L0", "L1", skeleton.GetBones()[0].Name ), // the whole body from L1
                    Blend( "Outer", "Inner", "L2", "spine" ) };                // the spine branch from L2
    graph.Nodes[4].ParameterInputs.push_back( G::ParameterPin{ G::LayerWeightPin( 0 ), "W" } );
    graph.OutputPose = "Outer";

    G::PoseGraphInstance instance;
    const auto           bound = instance.Bind( graph, skeleton );
    ASSERT_TRUE( bound.IsSuccess() ) << bound.GetError();

    const LocalPose reference( skeleton.GetBones().size() );
    G::GraphPose    out;
    instance.Evaluate( NumberedLeaves( reference ), skeleton, out );
    ASSERT_EQ( out.Pose.Size(), skeleton.GetBones().size() );
    EXPECT_NEAR( out.Pose[0].Translation.x, 2.0F, 1e-6F ) << "the root is the inner blend's: L1 over L0";
    EXPECT_NEAR( out.Pose[spine].Translation.x, 2.5F, 1e-6F )
         << "the spine is the outer blend's at the bound weight 0.5: halfway from the inner blend (2) to L2 (3)";
}

TEST( PoseGraphInstance, AnAdditiveOverALayeredBlendAndARefusedFilterBone )
{
    const Skeleton skeleton = FiveBones();

    G::AnimGraph graph;
    graph.Name  = "AdditiveOverBlend";
    graph.Nodes = { Leaf( "L0" ), Leaf( "L1" ), Leaf( "L2" ), Blend( "Upper", "L0", "L1", "spine" ) };
    G::PoseNode add;
    add.Name       = "Add";
    add.Kind       = static_cast<int>( G::PoseNodeKind::ApplyAdditive );
    add.PoseInputs = { "Upper", "L2" };
    graph.Nodes.push_back( add );
    graph.OutputPose = "Add";

    G::PoseGraphInstance instance;
    ASSERT_TRUE( instance.Bind( graph, skeleton ).IsSuccess() );
    const LocalPose reference( skeleton.GetBones().size() );
    G::GraphPose    out;
    instance.Evaluate( NumberedLeaves( reference ), skeleton, out );
    EXPECT_NEAR( out.Pose[0].Translation.x, 1.0F + 3.0F, 1e-6F )
         << "the root: L0 (outside the spine branch) plus the whole additive L2 (alpha unbound = 1)";

    graph.Nodes[3].LayeredBlend->Layers[0].Filters[0].BoneName = "no_such_bone";
    const auto refused                                         = instance.Bind( graph, skeleton );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Upper" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "no_such_bone" ), std::string::npos ) << refused.GetError();
}

// ── 9b. Linked Anim Layer: a layer of an interface, answered by whichever graph is linked ─────────────────

namespace
{
    G::PoseNode LinkedCall( std::string name, std::string input )
    {
        G::PoseNode node;
        node.Name        = std::move( name );
        node.Kind        = static_cast<int>( G::PoseNodeKind::LinkedAnimLayer );
        node.PoseInputs  = { std::move( input ) };
        node.LinkedLayer = G::LinkedAnimLayerNode{ "Weapon", "UpperBody" };
        return node;
    }

    /// The character's graph: Output Pose = LinkedAnimLayer "Call" (Weapon.UpperBody) over leaf "L0".
    G::AnimGraph Character()
    {
        G::AnimGraph graph;
        graph.Name = "Character";
        graph.Parameters.push_back(
             G::Parameter{ .Name = "Aim", .Type = static_cast<int>( G::ParamType::Float ), .Default = 1.0F } );
        graph.Nodes      = { Leaf( "L0" ), LinkedCall( "Call", "L0" ) };
        graph.OutputPose = "Call";
        graph.Layers     = G::AnimGraphLayers{ { G::AnimLayerInterface{ "Weapon", { "UpperBody" } } }, {} };
        return graph;
    }

    /// The rifle: Weapon.UpperBody = its "Aim" sequence over the caller's input on the spine branch, weight
    /// bound to the (host's) parameter "Aim". Its own Output Pose is a lone leaf.
    G::AnimGraph Rifle()
    {
        G::AnimGraph graph;
        graph.Name = "Rifle";
        graph.Parameters.push_back(
             G::Parameter{ .Name = "Aim", .Type = static_cast<int>( G::ParamType::Float ), .Default = 1.0F } );
        graph.Nodes      = { Leaf( "Idle" ) };
        graph.OutputPose = "Idle";
        G::PoseNode input;
        input.Name        = "In";
        input.Kind        = static_cast<int>( G::PoseNodeKind::LinkedInputPose );
        G::PoseNode blend = Blend( "Upper", "In", "Aim", "spine" );
        blend.ParameterInputs.push_back( G::ParameterPin{ G::LayerWeightPin( 0 ), "Aim" } );
        graph.Layers = G::AnimGraphLayers{
             { G::AnimLayerInterface{ "Weapon", { "UpperBody" } } },
             { G::AnimLayerGraph{ "Weapon", "UpperBody", { input, Leaf( "Aim" ), blend }, "Upper" } } };
        return graph;
    }

    /// Host leaves as NumberedLeaves; a linked layer's leaf n poses every bone at x = 10 + n.
    G::PoseGraphSources LinkedLeaves( const LocalPose& reference, G::LinkedLayerTable& table )
    {
        G::PoseGraphSources sources = NumberedLeaves( reference );
        sources.Parameter           = []( const std::string& ) { return 1.0F; };
        sources.SampleLinked        = []( size_t, size_t node, G::GraphPose& out )
        {
            for ( size_t b = 0; b < out.Pose.Size(); ++b )
                out.Pose[b].Translation = glm::vec3( 10.0F + static_cast<float>( node ), 0.0F, 0.0F );
        };
        sources.Linked = &table;
        return sources;
    }
} // namespace

TEST( LinkedAnimLayer, UnlinkedPassesTheInputAndLinkingSwapsTheLayerWithoutEditingTheGraph )
{
    const Skeleton     skeleton  = FiveBones();
    const uint32_t     spine     = skeleton.FindBoneIndex( "spine" ).value();
    const G::AnimGraph character = Character();

    G::PoseGraphInstance instance;
    const auto           bound = instance.Bind( character, skeleton );
    ASSERT_TRUE( bound.IsSuccess() ) << bound.GetError();

    const LocalPose     reference( skeleton.GetBones().size() );
    G::LinkedLayerTable table;
    G::GraphPose        out;
    instance.Evaluate( LinkedLeaves( reference, table ), skeleton, out );
    EXPECT_NEAR( out.Pose[spine].Translation.x, 1.0F, 1e-6F ) << "nothing linked: the call is its input, L0";

    const G::AnimGraph rifle  = Rifle();
    const auto         linked = table.Link( character, 101, rifle, skeleton );
    ASSERT_TRUE( linked.IsSuccess() ) << linked.GetError();
    instance.Evaluate( LinkedLeaves( reference, table ), skeleton, out );
    EXPECT_NEAR( out.Pose[0].Translation.x, 1.0F, 1e-6F ) << "the root is outside the rifle's spine branch";
    EXPECT_NEAR( out.Pose[spine].Translation.x, 11.0F, 1e-6F ) << "the spine is the rifle's Aim (layer node 1)";
    EXPECT_EQ( instance.Graph().Nodes.size(), character.Nodes.size() ) << "the character's graph was not edited";

    table.Unlink( 101 );
    instance.Evaluate( LinkedLeaves( reference, table ), skeleton, out );
    EXPECT_NEAR( out.Pose[spine].Translation.x, 1.0F, 1e-6F ) << "unlinked: the input passes again";
}

TEST( LinkedAnimLayer, ALinkIsRefusedByNameAndLeavesTheTableAsItWas )
{
    const Skeleton      skeleton = FiveBones();
    G::LinkedLayerTable table;

    G::AnimGraph noInterface = Character();
    noInterface.Layers.reset();
    noInterface.Nodes.pop_back();
    noInterface.OutputPose = "L0";
    const auto undeclared  = table.Link( noInterface, 101, Rifle(), skeleton );
    ASSERT_FALSE( undeclared.IsSuccess() );
    EXPECT_NE( undeclared.GetError().find( "'Weapon'" ), std::string::npos ) << undeclared.GetError();

    G::AnimGraph otherLayers = Character();
    otherLayers.Layers->Interfaces[0].Layers.push_back( "Hands" );
    EXPECT_FALSE( table.Link( otherLayers, 101, Rifle(), skeleton ).IsSuccess() ) << "the interfaces disagree";

    G::AnimGraph noAim = Character();
    noAim.Parameters.clear();
    const auto unread = table.Link( noAim, 101, Rifle(), skeleton );
    ASSERT_FALSE( unread.IsSuccess() );
    EXPECT_NE( unread.GetError().find( "'Aim'" ), std::string::npos ) << unread.GetError();

    G::AnimGraph badBone                                                                = Rifle();
    badBone.Layers->Implemented[0].Nodes[2].LayeredBlend->Layers[0].Filters[0].BoneName = "no_such_bone";
    const auto unbound = table.Link( Character(), 101, badBone, skeleton );
    ASSERT_FALSE( unbound.IsSuccess() );
    EXPECT_NE( unbound.GetError().find( "no_such_bone" ), std::string::npos ) << unbound.GetError();
    EXPECT_TRUE( table.Layers().empty() ) << "every refusal left nothing linked";
}

// ── 9c. ANIM-I14b: a layer is a whole graph (a state machine, a nested call); identity is the GUID ─────────

TEST( LinkedAnimLayer, UnlinkMatchesTheGraphGuidNotItsName )
{
    const Skeleton       skeleton  = FiveBones();
    const uint32_t       spine     = skeleton.FindBoneIndex( "spine" ).value();
    const G::AnimGraph   character = Character();
    G::PoseGraphInstance instance;
    ASSERT_TRUE( instance.Bind( character, skeleton ).IsSuccess() );
    const LocalPose     reference( skeleton.GetBones().size() );
    G::LinkedLayerTable table;
    G::GraphPose        out;

    EXPECT_FALSE( table.Link( character, 0, Rifle(), skeleton ).IsSuccess() ) << "a link needs a GUID";
    ASSERT_TRUE( table.Link( character, 201, Rifle(), skeleton ).IsSuccess() );
    table.Unlink( 202 ); // another file that happens to be called "Rifle" too
    instance.Evaluate( LinkedLeaves( reference, table ), skeleton, out );
    EXPECT_NEAR( out.Pose[spine].Translation.x, 11.0F, 1e-6F ) << "a different GUID unlinks nothing";
    table.Unlink( 201 );
    instance.Evaluate( LinkedLeaves( reference, table ), skeleton, out );
    EXPECT_NEAR( out.Pose[spine].Translation.x, 1.0F, 1e-6F ) << "its own GUID unlinks it";
}

TEST( LinkedAnimLayer, ALayerMayHoldAStateMachineAndCallANestedLayerButNotACycle )
{
    const Skeleton skeleton  = FiveBones();
    const uint32_t spine     = skeleton.FindBoneIndex( "spine" ).value();
    G::AnimGraph   character = Character();
    character.Layers->Interfaces.push_back( G::AnimLayerInterface{ "Hands", { "Grip" } } );
    G::PoseGraphInstance instance;
    ASSERT_TRUE( instance.Bind( character, skeleton ).IsSuccess() );

    // Rifle's UpperBody: a state machine blended over the input, then a nested call of Hands.Grip.
    G::AnimGraph rifle = Rifle();
    rifle.Layers->Interfaces.push_back( G::AnimLayerInterface{ "Hands", { "Grip" } } );
    G::PoseNode machine = Leaf( "Aim" );
    machine.Kind        = static_cast<int>( G::PoseNodeKind::StateMachine );
    machine.Sequence.reset();
    machine.Machine = G::StateMachine{ "Hold", { G::State{ .Name = "Hold", .Clip = "RifleHold" } } };
    rifle.Layers->Implemented[0].Nodes[1] = machine;
    G::PoseNode nested;
    nested.Name        = "Grip";
    nested.Kind        = static_cast<int>( G::PoseNodeKind::LinkedAnimLayer );
    nested.PoseInputs  = { "Upper" };
    nested.LinkedLayer = G::LinkedAnimLayerNode{ "Hands", "Grip" };
    rifle.Layers->Implemented[0].Nodes.push_back( nested );
    rifle.Layers->Implemented[0].OutputPose = "Grip";
    const auto planned                      = G::PlanPoseGraph( rifle );
    ASSERT_TRUE( planned.IsSuccess() ) << planned.GetError();

    G::LinkedLayerTable table;
    const auto          linked = table.Link( character, 301, rifle, skeleton );
    ASSERT_TRUE( linked.IsSuccess() ) << linked.GetError();
    ASSERT_TRUE( table.Layers()[0].Machines.has_value() ) << "the layer's machine has its own evaluator";
    EXPECT_EQ( table.Layers()[0].Machines->CurrentState( "Aim" )->Name, "Hold" );
    const LocalPose reference( skeleton.GetBones().size() );
    G::GraphPose    out;
    instance.Evaluate( LinkedLeaves( reference, table ), skeleton, out );
    EXPECT_NEAR( out.Pose[spine].Translation.x, 11.0F, 1e-6F ) << "the machine (layer node 1) poses the spine";

    // Gloves implement Hands.Grip by calling Weapon.UpperBody: linked, Weapon.UpperBody -> Hands.Grip -> ...
    G::AnimGraph gloves;
    gloves.Name       = "Gloves";
    gloves.Nodes      = { Leaf( "Idle" ) };
    gloves.OutputPose = "Idle";
    G::PoseNode in;
    in.Name = "In";
    in.Kind = static_cast<int>( G::PoseNodeKind::LinkedInputPose );
    G::PoseNode back;
    back.Name        = "Back";
    back.Kind        = static_cast<int>( G::PoseNodeKind::LinkedAnimLayer );
    back.PoseInputs  = { "In" };
    back.LinkedLayer = G::LinkedAnimLayerNode{ "Weapon", "UpperBody" };
    gloves.Layers    = G::AnimGraphLayers{
            { G::AnimLayerInterface{ "Weapon", { "UpperBody" } }, G::AnimLayerInterface{ "Hands", { "Grip" } } },
            { G::AnimLayerGraph{ "Hands", "Grip", { in, back }, "Back" } } };
    const auto cycle = table.Link( character, 302, gloves, skeleton );
    ASSERT_FALSE( cycle.IsSuccess() );
    EXPECT_NE( cycle.GetError().find( "Weapon.UpperBody -> Hands.Grip -> Weapon.UpperBody" ), std::string::npos )
         << cycle.GetError();
    EXPECT_EQ( table.Layers().size(), 1U ) << "the refused link left the table as it was";
}
