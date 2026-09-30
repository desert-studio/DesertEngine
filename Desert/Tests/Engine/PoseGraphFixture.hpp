#pragma once

// Pose graphs the Animator suites play (AnimatorBlending, ControlRigStage, BoneControlContract, RetargetAsset):
// ONE spelling of "a base with one layer" and "a base with one additive", so four suites do not each build
// the graph their own way. Included by relative path; header-only.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <string>
#include <utility>
#include <vector>

namespace PoseGraphFixture
{
    namespace G = Desert::Animation::Graph;

    /// The node index of the layer's (or the additive's) SequencePlayer in both graphs below.
    inline constexpr size_t kLayerNode = 1;

    inline G::PoseNode BaseMachine()
    {
        G::PoseNode base;
        base.Name    = "Base";
        base.Kind    = static_cast<int>( G::PoseNodeKind::StateMachine );
        base.Machine = G::StateMachine{};
        return base;
    }

    inline G::PoseNode Sequence( std::string name )
    {
        G::PoseNode node;
        node.Name     = std::move( name );
        node.Kind     = static_cast<int>( G::PoseNodeKind::SequencePlayer );
        node.Sequence = G::SequencePlayerNode{ .Clip = "clip", .Loop = true };
        return node;
    }

    /// Output Pose = Layered Blend Per Bone "Blend" over the base machine "Base" (the Animator's Source stage)
    /// and ONE SequencePlayer layer "Layer"; the layer's branch filters are `filters` and its weight pin is
    /// bound to the Float parameter "LayerWeight" (default 1).
    inline G::AnimGraph OneLayerGraph( std::vector<G::BranchFilter> filters )
    {
        G::AnimGraph graph;
        graph.Name = "OneLayer";
        graph.Parameters.push_back( G::Parameter{ .Name = "LayerWeight", .Type = static_cast<int>( G::ParamType::Float ),
                                                  .Default = 1.0F } );
        graph.Nodes.push_back( BaseMachine() );
        graph.Nodes.push_back( Sequence( "Layer" ) );
        G::PoseNode blend;
        blend.Name         = "Blend";
        blend.Kind         = static_cast<int>( G::PoseNodeKind::LayeredBlendPerBone );
        blend.PoseInputs   = { "Base", "Layer" };
        blend.LayeredBlend = G::LayeredBlendPerBoneNode{};
        blend.LayeredBlend->Layers.push_back( G::LayerSetup{ std::move( filters ) } );
        blend.ParameterInputs.push_back( G::ParameterPin{ G::LayerWeightPin( 0 ), "LayerWeight" } );
        graph.Nodes.push_back( std::move( blend ) );
        graph.OutputPose = "Blend";
        return graph;
    }

    /// Output Pose = Apply Additive "Add" over the base machine "Base" and the SequencePlayer "Additive";
    /// Alpha bound to the Float parameter "Alpha" (default 1).
    inline G::AnimGraph AdditiveGraph()
    {
        G::AnimGraph graph;
        graph.Name = "Additive";
        graph.Parameters.push_back(
             G::Parameter{ .Name = "Alpha", .Type = static_cast<int>( G::ParamType::Float ), .Default = 1.0F } );
        graph.Nodes.push_back( BaseMachine() );
        graph.Nodes.push_back( Sequence( "Additive" ) );
        G::PoseNode add;
        add.Name       = "Add";
        add.Kind       = static_cast<int>( G::PoseNodeKind::ApplyAdditive );
        add.PoseInputs = { "Base", "Additive" };
        add.ParameterInputs.push_back( G::ParameterPin{ std::string( G::kApplyAdditiveAlphaPin ), "Alpha" } );
        graph.Nodes.push_back( std::move( add ) );
        graph.OutputPose = "Add";
        return graph;
    }

    /// `graph` on `animator`, `clip` in its layer / additive node, the weight (or alpha) parameter at `weight`.
    [[nodiscard]] inline Common::BoolResultStr Drive( Desert::Animation::Animator& animator,
                                                              const G::AnimGraph&          graph,
                                                              const Desert::Animation::AnimationClip& clip,
                                                              float weight = 1.0F, bool loop = true )
    {
        auto set = animator.SetPoseGraph( graph );
        if ( !set )
            return set;
        animator.SetPoseGraphSource( kLayerNode, clip, loop );
        animator.SetPoseGraphParameter( graph.Parameters.front().Name, weight );
        return set;
    }

    /// A full-body layer: the branch from `rootBone` at depth 0, every bone at full weight.
    inline G::AnimGraph FullBodyLayer( const std::string& rootBone )
    {
        return OneLayerGraph( { G::BranchFilter{ rootBone, 0 } } );
    }
} // namespace PoseGraphFixture
