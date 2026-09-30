#include "PoseGraphInstance.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <format>
#include <utility>

namespace Desert::Animation::Graph
{
    namespace
    {
        /// `add / ref` per axis; a zero reference scale has no ratio, and 1 (no change) is the only answer
        /// that does not invent one.
        glm::vec3 ScaleRatio( const glm::vec3& add, const glm::vec3& ref )
        {
            glm::vec3 ratio( 1.0F );
            for ( int axis = 0; axis < 3; ++axis )
                if ( ref[axis] != 0.0F )
                    ratio[axis] = add[axis] / ref[axis];
            return ratio;
        }
    } // namespace

    Common::BoolResultStr ApplyAdditive( const GraphPose& base, const GraphPose& additive, const LocalPose& reference,
                                         float alpha, GraphPose& out )
    {
        const size_t n = base.Pose.Size();
        if ( additive.Pose.Size() != n || reference.Size() != n )
            return Common::MakeError<bool>( std::format(
                 "Apply Additive: the base has {} bones, the additive {} and the reference {}", n,
                 additive.Pose.Size(), reference.Size() ) );

        out = base;
        if ( alpha <= 0.0F )
            return Common::MakeSuccess( true ); // bit-identical to the base, the node's contract

        const glm::quat identity( 1.0F, 0.0F, 0.0F, 0.0F );
        for ( size_t b = 0; b < n; ++b )
        {
            const BoneTransform& add = additive.Pose[b];
            const BoneTransform& ref = reference[b];
            BoneTransform&       o   = out.Pose[b];

            const glm::quat delta = add.Rotation * glm::inverse( ref.Rotation );
            o.Rotation            = glm::normalize( glm::slerp( identity, delta, alpha ) * o.Rotation );
            o.Translation += alpha * ( add.Translation - ref.Translation );
            o.Scale *= glm::mix( glm::vec3( 1.0F ), ScaleRatio( add.Scale, ref.Scale ), alpha );
        }

        for ( size_t c = 0; c < additive.CurveNames.size() && c < additive.CurveValues.size(); ++c )
        {
            const std::string& name  = additive.CurveNames[c];
            const float        value = alpha * additive.CurveValues[c];
            bool               found = false;
            for ( size_t i = 0; i < out.CurveNames.size() && i < out.CurveValues.size(); ++i )
                if ( out.CurveNames[i] == name )
                {
                    out.CurveValues[i] += value;
                    found = true;
                    break;
                }
            if ( !found )
            {
                out.CurveNames.push_back( name );
                out.CurveValues.push_back( value );
            }
        }
        return Common::MakeSuccess( true );
    }

    float PoseGraphInstance::PinValue( const PoseNode& node, std::string_view pin, float unbound,
                                       const std::function<float( const std::string& )>& parameter )
    {
        for ( const ParameterPin& bound : node.ParameterInputs )
            if ( bound.Pin == pin )
                return parameter( bound.Parameter );
        return unbound;
    }

    Common::BoolResultStr PoseGraphInstance::Bind( AnimGraph graph, const Skeleton& skeleton )
    {
        auto plan = PlanPoseGraph( graph );
        if ( !plan )
            return Common::MakeError<bool>( plan.GetError() );

        const size_t                                 count = graph.Nodes.size();
        std::vector<std::vector<int>>                inputs( count );
        std::vector<std::vector<PerBoneBlendWeight>> tables( count );
        for ( const int index : plan.GetValue() )
        {
            const PoseNode& node = graph.Nodes[static_cast<size_t>( index )];
            for ( const std::string& wired : node.PoseInputs )
                inputs[static_cast<size_t>( index )].push_back(
                     static_cast<int>( FindNode( graph, wired ) - graph.Nodes.data() ) ); // planned: it exists
            if ( static_cast<PoseNodeKind>( node.Kind ) == PoseNodeKind::LayeredBlendPerBone )
            {
                auto table = BuildPerBoneWeights( *node.LayeredBlend, skeleton );
                if ( !table )
                    return Common::MakeError<bool>( std::format( "AnimGraph '{}': Layered Blend Per Bone '{}': {}",
                                                                 graph.Name, node.Name, table.GetError() ) );
                tables[static_cast<size_t>( index )] = std::move( table.GetValue() );
            }
        }

        m_Graph  = std::move( graph );
        m_Plan   = std::move( plan.GetValue() );
        m_Inputs = std::move( inputs );
        m_Tables = std::move( tables );
        m_Poses.assign( count, GraphPose{} );
        return Common::MakeSuccess( true );
    }

    void PoseGraphInstance::Evaluate( const PoseGraphSources& sources, const Skeleton& skeleton, GraphPose& out )
    {
        if ( m_Plan.empty() )
            return;

        const size_t bones = skeleton.GetBones().size();
        for ( const int index : m_Plan )
        {
            const auto       n     = static_cast<size_t>( index );
            const PoseNode&  node  = m_Graph.Nodes[n];
            GraphPose&       pose  = m_Poses[n];
            const auto&      wired = m_Inputs[n];
            const auto       kind  = static_cast<PoseNodeKind>( node.Kind );
            switch ( kind )
            {
                case PoseNodeKind::StateMachine:
                case PoseNodeKind::SequencePlayer:
                    pose.Pose.Resize( bones );
                    pose.CurveNames.clear();
                    pose.CurveValues.clear();
                    pose.RootMotion = BoneTransform{};
                    sources.Sample( n, pose );
                    break;

                case PoseNodeKind::LayeredBlendPerBone:
                {
                    const size_t layers = wired.size() - 1;
                    m_LayerScratch.resize( layers );
                    m_WeightScratch.resize( layers );
                    for ( size_t layer = 0; layer < layers; ++layer )
                    {
                        m_LayerScratch[layer]  = m_Poses[static_cast<size_t>( wired[layer + 1] )];
                        m_WeightScratch[layer] = PinValue( node, LayerWeightPin( layer ), 1.0F, sources.Parameter );
                    }
                    // DISCARDED DELIBERATELY: Bind sized the table against this skeleton and the plan fixed the
                    // layer count, so the node's size checks cannot fail; were one to, the base shows.
                    if ( !BlendLayeredPerBone( *node.LayeredBlend, m_Tables[n], skeleton,
                                               m_Poses[static_cast<size_t>( wired[0] )], m_LayerScratch,
                                               m_WeightScratch, pose ) )
                        pose = m_Poses[static_cast<size_t>( wired[0] )];
                    break;
                }

                case PoseNodeKind::ApplyAdditive:
                {
                    const GraphPose& base  = m_Poses[static_cast<size_t>( wired[0] )];
                    const float      alpha = PinValue( node, kApplyAdditiveAlphaPin, 1.0F, sources.Parameter );
                    // No reference means no difference to take: the base shows, as with alpha 0.
                    if ( sources.AdditiveReference == nullptr ||
                         !ApplyAdditive( base, m_Poses[static_cast<size_t>( wired[1] )], *sources.AdditiveReference,
                                         alpha, pose ) )
                        pose = base;
                    break;
                }
            }
        }
        out = m_Poses[static_cast<size_t>( m_Plan.back() )];
    }
} // namespace Desert::Animation::Graph
