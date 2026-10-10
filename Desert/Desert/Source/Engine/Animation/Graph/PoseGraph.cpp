#include "AnimGraph.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>

// The pose graph's structure: node kinds and their pins, lookups, and the evaluation plan. Kept apart from
// the Evaluator because the loader (AnimGraphSerialization.cpp) refuses an unplannable graph and links no
// runtime.
namespace Desert::Animation::Graph
{
    const char* TypeName( ParamType type )
    {
        switch ( type )
        {
            case ParamType::Bool:
                return "Bool";
            case ParamType::Int:
                return "Int";
            case ParamType::Float:
                return "Float";
        }
        return "?";
    }

    std::string DeclaredParameterList( const AnimGraph& graph )
    {
        std::string declared;
        for ( const auto& p : graph.Parameters )
        {
            declared += declared.empty() ? "" : ", ";
            declared += std::format( "'{}' ({})", p.Name, TypeName( static_cast<ParamType>( p.Type ) ) );
        }
        return declared.empty() ? std::string( "none at all" ) : declared;
    }

    const char* KindName( PoseNodeKind kind )
    {
        switch ( kind )
        {
            case PoseNodeKind::StateMachine:
                return "StateMachine";
            case PoseNodeKind::LayeredBlendPerBone:
                return "LayeredBlendPerBone";
            case PoseNodeKind::SequencePlayer:
                return "SequencePlayer";
            case PoseNodeKind::ApplyAdditive:
                return "ApplyAdditive";
            case PoseNodeKind::LinkedAnimLayer:
                return "LinkedAnimLayer";
            case PoseNodeKind::LinkedInputPose:
                return "LinkedInputPose";
            case PoseNodeKind::TwoBoneIK:
                return "TwoBoneIK";
            case PoseNodeKind::LookAt:
                return "LookAt";
            case PoseNodeKind::BlendSpace1D:
                return "BlendSpace1D";
        }
        return "?";
    }

    PoseNodePins PinsOf( PoseNodeKind kind )
    {
        switch ( kind )
        {
            case PoseNodeKind::StateMachine:
                // A state machine produces its pose from its states' clips: no Pose input, and its
                // conditions read the graph's parameters directly rather than through pins.
                return PoseNodePins{ .PoseInputs = 0, .ParameterPins = {} };
            case PoseNodeKind::LayeredBlendPerBone:
                // The base pin; the layers' pose and weight pins grow with the payload (PoseInputCountOf,
                // HasParameterPin), so the kind alone has no fixed parameter pins.
                return PoseNodePins{ .PoseInputs = 1, .ParameterPins = {} };
            case PoseNodeKind::SequencePlayer:
                // A clip on its own clock: a leaf, like the state machine.
                return PoseNodePins{ .PoseInputs = 0, .ParameterPins = {} };
            case PoseNodeKind::ApplyAdditive:
            {
                // Pin 0 Base, pin 1 Additive; Alpha scales the additive (UE's exposed Alpha pin).
                static constexpr std::array<const char*, 1> kPins{ kApplyAdditiveAlphaPin.data() };
                return PoseNodePins{ .PoseInputs = 2, .ParameterPins = kPins };
            }
            case PoseNodeKind::LinkedAnimLayer:
                // Pin 0 is the layer's input pose (and the output while nothing is linked).
                return PoseNodePins{ .PoseInputs = 1, .ParameterPins = {} };
            case PoseNodeKind::LinkedInputPose:
                // The caller's pose: a leaf of the layer graph.
                return PoseNodePins{ .PoseInputs = 0, .ParameterPins = {} };
            case PoseNodeKind::BlendSpace1D:
            {
                // A leaf like the sequence player: its samples play on their own clocks; X is the axis.
                static constexpr std::array<const char*, 1> kPins{ kBlendSpaceAxisPin.data() };
                return PoseNodePins{ .PoseInputs = 0, .ParameterPins = kPins };
            }
            case PoseNodeKind::TwoBoneIK:
            case PoseNodeKind::LookAt:
            {
                // Pin 0 the component pose it controls; Alpha its strength (UE SkeletalControlBase's Alpha).
                static constexpr std::array<const char*, 1> kPins{ kBoneControlAlphaPin.data() };
                return PoseNodePins{ .PoseInputs = 1, .ParameterPins = kPins };
            }
        }
        return PoseNodePins{};
    }

    bool IsSourceKind( PoseNodeKind kind )
    {
        return kind == PoseNodeKind::StateMachine || kind == PoseNodeKind::SequencePlayer;
    }

    std::string BlendSpace1DError( const BlendSpace1DNode& node )
    {
        if ( node.Samples.empty() )
            return "it has no sample, so it plays nothing";
        for ( size_t i = 0; i < node.Samples.size(); ++i )
        {
            const BlendSample& sample = node.Samples[i];
            if ( sample.Clip.empty() )
                return std::format( "sample {} names no clip", i );
            if ( !std::isfinite( sample.Value ) )
                return std::format( "sample {} ('{}') has a value that is not a number", i, sample.Clip );
            if ( i > 0 && !( sample.Value > node.Samples[i - 1].Value ) )
                return std::format(
                     "sample {} ('{}', {}) does not lie above sample {} ('{}', {}); the samples are "
                     "a row on the axis, strictly ascending",
                     i, sample.Clip, sample.Value, i - 1, node.Samples[i - 1].Clip, node.Samples[i - 1].Value );
        }
        if ( !std::isfinite( node.WeightSpeed ) || node.WeightSpeed < 0.0F )
            return std::format( "its weight speed {} is not a speed (0 = no smoothing, else weight per second)",
                                node.WeightSpeed );
        return {};
    }

    void BlendSpace1DTargetWeights( const BlendSpace1DNode& node, const float x, const std::span<float> out )
    {
        std::fill( out.begin(), out.end(), 0.0F );
        const size_t count = std::min( out.size(), node.Samples.size() );
        if ( count == 0 )
            return;
        if ( !( x > node.Samples.front().Value ) ) // also a NaN axis: the row's start
        {
            out[0] = 1.0F;
            return;
        }
        if ( x >= node.Samples[count - 1].Value )
        {
            out[count - 1] = 1.0F;
            return;
        }
        for ( size_t i = 1; i < count; ++i )
        {
            const float upper = node.Samples[i].Value;
            if ( x > upper )
                continue;
            const float lower = node.Samples[i - 1].Value;
            const float alpha = ( x - lower ) / ( upper - lower );
            out[i - 1]        = 1.0F - alpha;
            out[i]            = alpha;
            return;
        }
    }

    void InterpolateBlendWeights( const std::span<float> weights, const std::span<const float> target,
                                  const float speedPerSecond, const float seconds )
    {
        const size_t count = std::min( weights.size(), target.size() );
        if ( speedPerSecond <= 0.0F )
        {
            std::copy_n( target.begin(), count, weights.begin() );
            return;
        }
        const float step  = speedPerSecond * std::max( seconds, 0.0F );
        float       total = 0.0F;
        for ( size_t i = 0; i < count; ++i )
        {
            weights[i] += std::clamp( target[i] - weights[i], -step, step );
            total += weights[i];
        }
        if ( total <= 0.0F )
        {
            std::copy_n( target.begin(), count, weights.begin() );
            return;
        }
        for ( size_t i = 0; i < count; ++i )
            weights[i] /= total;
    }

    float AdvanceSyncedPhase( const float phase, const std::span<const float> weights,
                              const std::span<const float> lengthSeconds, const float seconds, const bool loop )
    {
        float weighted = 0.0F;
        float total    = 0.0F;
        for ( size_t i = 0; i < std::min( weights.size(), lengthSeconds.size() ); ++i )
            if ( weights[i] > 0.0F && lengthSeconds[i] > 0.0F )
            {
                weighted += weights[i] * lengthSeconds[i];
                total += weights[i];
            }
        if ( total <= 0.0F )
            return phase;
        const float next = phase + seconds / ( weighted / total );
        if ( !loop )
            return std::clamp( next, 0.0F, 1.0F );
        const float wrapped = next - std::floor( next );
        return wrapped < 1.0F ? wrapped : 0.0F;
    }

    std::string LayerWeightPin( size_t layer )
    {
        return std::format( "BlendWeights_{}", layer );
    }

    int PoseInputCountOf( const PoseNode& node )
    {
        const auto kind  = static_cast<PoseNodeKind>( node.Kind );
        const int  fixed = PinsOf( kind ).PoseInputs;
        if ( kind == PoseNodeKind::LayeredBlendPerBone && node.LayeredBlend )
            return fixed + static_cast<int>( node.LayeredBlend->Layers.size() );
        return fixed;
    }

    bool HasParameterPin( const PoseNode& node, std::string_view pin )
    {
        const PoseNodePins pins = PinsOf( static_cast<PoseNodeKind>( node.Kind ) );
        if ( std::find( pins.ParameterPins.begin(), pins.ParameterPins.end(), pin ) != pins.ParameterPins.end() )
            return true;
        if ( static_cast<PoseNodeKind>( node.Kind ) == PoseNodeKind::LayeredBlendPerBone && node.LayeredBlend )
            for ( size_t layer = 0; layer < node.LayeredBlend->Layers.size(); ++layer )
                if ( pin == LayerWeightPin( layer ) )
                    return true;
        return false;
    }

    AnimGraph MakeStateMachineGraph( std::string name )
    {
        AnimGraph graph;
        graph.Name = std::move( name );

        PoseNode machine;
        machine.Name    = std::string( kDefaultStateMachineNode );
        machine.Kind    = static_cast<int>( PoseNodeKind::StateMachine );
        machine.Machine = StateMachine{};
        graph.Nodes.push_back( std::move( machine ) );
        graph.OutputPose = std::string( kDefaultStateMachineNode );
        std::tie( graph.OutputPoseX, graph.OutputPoseY ) =
             DefaultOutputPosePosition( graph.Nodes, graph.OutputPose );
        return graph;
    }

    std::pair<float, float> DefaultOutputPosePosition( const std::vector<PoseNode>& nodes,
                                                       std::string_view             outputPose )
    {
        float rightmost = 0.0f;
        float level     = 0.0f;
        for ( const PoseNode& node : nodes )
        {
            rightmost = std::max( rightmost, node.X );
            if ( node.Name == outputPose )
                level = node.Y;
        }
        return { nodes.empty() ? 0.0f : rightmost + kOutputPoseSpacingX, level };
    }

    namespace
    {
        /// One body for both constnesses: `GraphT` is `AnimGraph` or `const AnimGraph`, and the node handed
        /// back carries the same constness as the graph it was found in.
        template <typename GraphT>
        auto* FindNodeIn( GraphT& graph, std::string_view name )
        {
            const auto it = std::find_if( graph.Nodes.begin(), graph.Nodes.end(),
                                          [name]( const PoseNode& node ) { return node.Name == name; } );
            return it == graph.Nodes.end() ? nullptr : &*it;
        }

        /// The state machine at the base source, if that source is one; same constness rule as FindNodeIn.
        /// The walk runs on the const view and the node is re-addressed by index in `graph` itself.
        template <typename GraphT>
        auto* OutputMachineIn( GraphT& graph )
        {
            using MachinePtr       = decltype( &*graph.Nodes.front().Machine );
            const PoseNode* source = BaseSourceNode( std::as_const( graph ) );
            if ( source == nullptr || static_cast<PoseNodeKind>( source->Kind ) != PoseNodeKind::StateMachine )
                return MachinePtr{};
            auto& machine = graph.Nodes[static_cast<size_t>( source - graph.Nodes.data() )].Machine;
            return machine ? &*machine : MachinePtr{};
        }
    } // namespace

    const PoseNode* FindNode( const AnimGraph& graph, std::string_view name )
    {
        return FindNodeIn( graph, name );
    }

    PoseNode* FindNode( AnimGraph& graph, std::string_view name )
    {
        return FindNodeIn( graph, name );
    }

    const PoseNode* BaseSourceNode( const AnimGraph& graph )
    {
        // Down the base pin (pin 0) of each blend node; bounded by the node count so a cycle (which
        // PlanPoseGraph refuses, but an editor may be holding one mid-edit) ends the walk.
        const PoseNode* node = FindNode( graph, graph.OutputPose );
        for ( size_t hops = 0; node != nullptr && hops <= graph.Nodes.size(); ++hops )
        {
            if ( IsSourceKind( static_cast<PoseNodeKind>( node->Kind ) ) )
                return node;
            if ( node->PoseInputs.empty() )
                return nullptr;
            node = FindNode( graph, node->PoseInputs.front() );
        }
        return nullptr;
    }

    const StateMachine* OutputMachine( const AnimGraph& graph )
    {
        return OutputMachineIn( graph );
    }

    StateMachine* OutputMachine( AnimGraph& graph )
    {
        return OutputMachineIn( graph );
    }

    namespace
    {
        /// Every per-node rule that does not need the wires followed: the name, the kind, the pins, the
        /// payload. The first broken rule, as a sentence naming the node; empty when the node is sound.
        std::string LayerList( const AnimLayerInterface& anInterface )
        {
            std::string list;
            for ( const std::string& layer : anInterface.Layers )
                list += std::format( "{}'{}'", list.empty() ? "" : ", ", layer );
            return list.empty() ? std::string( "none at all" ) : list;
        }

        std::string InterfaceList( const AnimGraph& graph )
        {
            std::string list;
            if ( graph.Layers )
                for ( const AnimLayerInterface& declared : graph.Layers->Interfaces )
                    list += std::format( "{}'{}'", list.empty() ? "" : ", ", declared.Name );
            return list.empty() ? std::string( "none at all" ) : list;
        }

        /// The rules of the linked-layer kinds: which scope may hold them, and what a call names.
        std::string LinkedKindError( const AnimGraph& graph, const PoseNode& node, GraphScope scope )
        {
            const auto kind = static_cast<PoseNodeKind>( node.Kind );
            if ( kind == PoseNodeKind::LinkedAnimLayer && !node.LinkedLayer )
                return std::format( "AnimGraph '{}': node '{}' is a LinkedAnimLayer node naming no layer",
                                    graph.Name, node.Name );
            if ( kind != PoseNodeKind::LinkedAnimLayer && node.LinkedLayer )
                return std::format( "AnimGraph '{}': node '{}' ({}) names a linked layer, which only a "
                                    "LinkedAnimLayer node does",
                                    graph.Name, node.Name, KindName( kind ) );
            if ( scope == GraphScope::Host && kind == PoseNodeKind::LinkedInputPose )
                return std::format( "AnimGraph '{}': node '{}' is a LinkedInputPose, which only a layer graph has "
                                    "(it is the pose the layer's caller hands in)",
                                    graph.Name, node.Name );
            if ( kind != PoseNodeKind::LinkedAnimLayer )
                return {};
            const AnimLayerInterface* called = FindLayerInterface( graph, node.LinkedLayer->Interface );
            if ( called == nullptr )
                return std::format( "AnimGraph '{}': LinkedAnimLayer node '{}' calls interface '{}', which the "
                                    "graph does not declare. It declares: {}",
                                    graph.Name, node.Name, node.LinkedLayer->Interface, InterfaceList( graph ) );
            if ( std::find( called->Layers.begin(), called->Layers.end(), node.LinkedLayer->Layer ) ==
                 called->Layers.end() )
                return std::format(
                     "AnimGraph '{}': LinkedAnimLayer node '{}' calls layer '{}' of interface '{}', "
                     "which has no such layer. Its layers: {}",
                     graph.Name, node.Name, node.LinkedLayer->Layer, called->Name, LayerList( *called ) );
            return {};
        }

        /// The node part of PlanPoseGraph (everything but the host's Layers check). A layer graph is planned
        /// through this directly, so LayersError and PlanPoseGraph do not call each other.
        Common::ResultStr<std::vector<int>> PlanNodes( const AnimGraph& graph, GraphScope scope );

        /// The graph's `Layers`: declarations sound, every implemented layer graph plannable, of a declared
        /// layer, once, and an implemented interface implemented whole.
        std::string LayersError( const AnimGraph& graph )
        {
            if ( !graph.Layers )
                return {};
            const AnimGraphLayers& layers = *graph.Layers;
            for ( size_t i = 0; i < layers.Interfaces.size(); ++i )
            {
                const AnimLayerInterface& declared = layers.Interfaces[i];
                if ( declared.Name.empty() || declared.Layers.empty() )
                    return std::format( "AnimGraph '{}' declares a layer interface with no {}", graph.Name,
                                        declared.Name.empty() ? "name"
                                                              : std::format( "layers ('{}')", declared.Name ) );
                for ( size_t j = 0; j < i; ++j )
                    if ( layers.Interfaces[j].Name == declared.Name )
                        return std::format( "AnimGraph '{}' declares layer interface '{}' twice", graph.Name,
                                            declared.Name );
                for ( size_t a = 0; a < declared.Layers.size(); ++a )
                    for ( size_t b = 0; b < a; ++b )
                        if ( declared.Layers[a] == declared.Layers[b] )
                            return std::format( "AnimGraph '{}': layer interface '{}' declares layer '{}' twice",
                                                graph.Name, declared.Name, declared.Layers[a] );
            }
            for ( size_t i = 0; i < layers.Implemented.size(); ++i )
            {
                const AnimLayerGraph&     layer    = layers.Implemented[i];
                const AnimLayerInterface* declared = FindLayerInterface( graph, layer.Interface );
                if ( declared == nullptr )
                    return std::format( "AnimGraph '{}' implements a layer of interface '{}', which it does not "
                                        "declare. It declares: {}",
                                        graph.Name, layer.Interface, InterfaceList( graph ) );
                if ( std::find( declared->Layers.begin(), declared->Layers.end(), layer.Layer ) ==
                     declared->Layers.end() )
                    return std::format(
                         "AnimGraph '{}' implements layer '{}' of interface '{}', which has no such "
                         "layer. Its layers: {}",
                         graph.Name, layer.Layer, layer.Interface, LayerList( *declared ) );
                for ( size_t j = 0; j < i; ++j )
                    if ( layers.Implemented[j].Interface == layer.Interface &&
                         layers.Implemented[j].Layer == layer.Layer )
                        return std::format( "AnimGraph '{}' implements layer '{}.{}' twice", graph.Name,
                                            layer.Interface, layer.Layer );
                if ( auto plan = PlanNodes( LayerGraphAsGraph( graph, layer ), GraphScope::Layer ); !plan )
                    return plan.GetError();
            }
            // A layer that reaches itself through the graph's own implementations evaluates forever once
            // the graph is linked; refused here, by name (the cross-graph case is LinkedLayerTable::Link's).
            std::vector<LayerCalls> own;
            own.reserve( layers.Implemented.size() );
            for ( const AnimLayerGraph& layer : layers.Implemented )
                own.push_back( { layer.Interface, layer.Layer, CalledLayers( layer.Nodes ) } );
            if ( std::string cycle = LayerCycle( own ); !cycle.empty() )
                return std::format( "AnimGraph '{}': its layers call each other in a cycle ({}), which would "
                                    "evaluate forever once linked",
                                    graph.Name, cycle );
            for ( const AnimLayerInterface& declared : layers.Interfaces )
            {
                const auto implements = [&]( const std::string& name )
                {
                    return std::any_of( layers.Implemented.begin(), layers.Implemented.end(),
                                        [&]( const AnimLayerGraph& l )
                                        { return l.Interface == declared.Name && l.Layer == name; } );
                };
                if ( std::none_of( declared.Layers.begin(), declared.Layers.end(), implements ) )
                    continue; // declared to be called, not implemented
                for ( const std::string& name : declared.Layers )
                    if ( !implements( name ) )
                        return std::format( "AnimGraph '{}' implements interface '{}' without its layer '{}'; a "
                                            "half-implemented interface is a layer that plays nothing once linked",
                                            graph.Name, declared.Name, name );
            }
            return {};
        }

        std::string NodeError( const AnimGraph& graph, const PoseNode& node, GraphScope scope )
        {
            if ( node.Name.empty() )
                return std::format( "AnimGraph '{}' has a pose node with no name; wires name nodes, so a "
                                    "nameless node can be wired by nothing",
                                    graph.Name );

            const auto kind = static_cast<PoseNodeKind>( node.Kind );
            if ( std::string_view( KindName( kind ) ) == "?" )
                return std::format( "AnimGraph '{}': node '{}' is of kind {}, which no pose node kind is",
                                    graph.Name, node.Name, node.Kind );

            // The payload before the pins: a Layered Blend Per Bone's pins are counted from its layers.
            if ( kind == PoseNodeKind::StateMachine && !node.Machine )
                return std::format( "AnimGraph '{}': node '{}' is a StateMachine node with no machine in it",
                                    graph.Name, node.Name );
            if ( kind != PoseNodeKind::StateMachine && node.Machine )
                return std::format( "AnimGraph '{}': node '{}' ({}) carries a state machine, which only a "
                                    "StateMachine node has",
                                    graph.Name, node.Name, KindName( kind ) );
            if ( kind == PoseNodeKind::LayeredBlendPerBone && !node.LayeredBlend )
                return std::format( "AnimGraph '{}': node '{}' is a LayeredBlendPerBone node with no layer setup "
                                    "in it",
                                    graph.Name, node.Name );
            if ( kind != PoseNodeKind::LayeredBlendPerBone && node.LayeredBlend )
                return std::format( "AnimGraph '{}': node '{}' ({}) carries a layer setup, which only a "
                                    "LayeredBlendPerBone node has",
                                    graph.Name, node.Name, KindName( kind ) );

            if ( kind == PoseNodeKind::SequencePlayer && !node.Sequence )
                return std::format( "AnimGraph '{}': node '{}' is a SequencePlayer node with no clip setup in it",
                                    graph.Name, node.Name );
            if ( kind != PoseNodeKind::SequencePlayer && node.Sequence )
                return std::format( "AnimGraph '{}': node '{}' ({}) carries a sequence player setup, which only a "
                                    "SequencePlayer node has",
                                    graph.Name, node.Name, KindName( kind ) );
            if ( ( kind == PoseNodeKind::TwoBoneIK ) != node.TwoBoneIK.has_value() )
                return std::format( "AnimGraph '{}': node '{}' ({}) {}", graph.Name, node.Name, KindName( kind ),
                                    node.TwoBoneIK ? "carries a two-bone IK setup, which only a TwoBoneIK node has"
                                                   : "is a TwoBoneIK node with no IK setup in it" );
            if ( ( kind == PoseNodeKind::LookAt ) != node.LookAt.has_value() )
                return std::format( "AnimGraph '{}': node '{}' ({}) {}", graph.Name, node.Name, KindName( kind ),
                                    node.LookAt ? "carries a look-at setup, which only a LookAt node has"
                                                : "is a LookAt node with no look-at setup in it" );
            if ( ( kind == PoseNodeKind::BlendSpace1D ) != node.BlendSpace.has_value() )
                return std::format( "AnimGraph '{}': node '{}' ({}) {}", graph.Name, node.Name, KindName( kind ),
                                    node.BlendSpace ? "carries a blend space, which only a BlendSpace1D node has"
                                                    : "is a BlendSpace1D node with no samples setup in it" );
            if ( node.BlendSpace )
            {
                if ( std::string error = BlendSpace1DError( *node.BlendSpace ); !error.empty() )
                    return std::format( "AnimGraph '{}': BlendSpace1D node '{}': {}", graph.Name, node.Name,
                                        error );
                // A layer graph's sources are sampled by the link's own clocks (SampleLinked), which play one clip
                // per node; a blend space there would stand in the bind pose, so it is refused by name instead.
                if ( scope == GraphScope::Layer )
                    return std::format( "AnimGraph '{}': BlendSpace1D node '{}' is in a layer graph; blend spaces "
                                        "play in the host graph only",
                                        graph.Name, node.Name );
            }
            if ( std::string linked = LinkedKindError( graph, node, scope ); !linked.empty() )
                return linked;
            if ( kind == PoseNodeKind::SequencePlayer && node.Sequence->Clip.empty() )
                return std::format( "AnimGraph '{}': SequencePlayer node '{}' names no clip", graph.Name,
                                    node.Name );

            const int poseInputs = PoseInputCountOf( node );
            if ( static_cast<int>( node.PoseInputs.size() ) != poseInputs )
                return std::format( "AnimGraph '{}': node '{}' ({}) has {} Pose pin(s) and {} wire(s) into them",
                                    graph.Name, node.Name, KindName( kind ), poseInputs, node.PoseInputs.size() );

            for ( const ParameterPin& bound : node.ParameterInputs )
            {
                if ( !HasParameterPin( node, bound.Pin ) )
                    return std::format( "AnimGraph '{}': node '{}' ({}) binds a parameter to pin '{}', which a "
                                        "{} node does not have",
                                        graph.Name, node.Name, KindName( kind ), bound.Pin, KindName( kind ) );
                const bool declared =
                     std::any_of( graph.Parameters.begin(), graph.Parameters.end(),
                                  [&bound]( const Parameter& p ) { return p.Name == bound.Parameter; } );
                if ( !declared )
                    return std::format( "AnimGraph '{}': node '{}' binds pin '{}' to parameter '{}', which the "
                                        "graph does not declare. It declares: {}",
                                        graph.Name, node.Name, bound.Pin, bound.Parameter,
                                        DeclaredParameterList( graph ) );
            }

            return {};
        }
    } // namespace

    const AnimLayerInterface* FindLayerInterface( const AnimGraph& graph, std::string_view name )
    {
        if ( !graph.Layers )
            return nullptr;
        const auto& declared = graph.Layers->Interfaces;
        const auto  it       = std::find_if( declared.begin(), declared.end(),
                                             [name]( const AnimLayerInterface& i ) { return i.Name == name; } );
        return it == declared.end() ? nullptr : &*it;
    }

    AnimGraph LayerGraphAsGraph( const AnimGraph& owner, const AnimLayerGraph& layer )
    {
        AnimGraph graph;
        graph.Name       = std::format( "{} / {}.{}", owner.Name, layer.Interface, layer.Layer );
        graph.Parameters = owner.Parameters;
        graph.Nodes      = layer.Nodes;
        graph.OutputPose  = layer.OutputPose;
        graph.OutputPoseX = layer.OutputPoseX;
        graph.OutputPoseY = layer.OutputPoseY;
        if ( owner.Layers && !owner.Layers->Interfaces.empty() )
            graph.Layers = AnimGraphLayers{ owner.Layers->Interfaces, {} };
        return graph;
    }

    std::vector<LinkedAnimLayerNode> CalledLayers( const std::vector<PoseNode>& nodes )
    {
        std::vector<LinkedAnimLayerNode> calls;
        for ( const PoseNode& node : nodes )
            if ( static_cast<PoseNodeKind>( node.Kind ) == PoseNodeKind::LinkedAnimLayer && node.LinkedLayer )
                calls.push_back( *node.LinkedLayer );
        return calls;
    }

    std::string LayerCycle( const std::vector<LayerCalls>& layers )
    {
        // Depth-first over "layer i calls a layer answered by j"; a grey node met again closes the cycle.
        const auto answering = [&]( const LinkedAnimLayerNode& call ) -> std::optional<size_t>
        {
            for ( size_t j = 0; j < layers.size(); ++j )
                if ( layers[j].Interface == call.Interface && layers[j].Layer == call.Layer )
                    return j;
            return std::nullopt;
        };
        enum class Mark : uint8_t
        {
            White,
            Grey,
            Black
        };
        std::vector<Mark>             marks( layers.size(), Mark::White );
        std::vector<size_t>           path;
        std::string                   cycle;
        std::function<bool( size_t )> visit = [&]( size_t i ) -> bool
        {
            marks[i] = Mark::Grey;
            path.push_back( i );
            for ( const LinkedAnimLayerNode& call : layers[i].Calls )
            {
                const auto j = answering( call );
                if ( !j || marks[*j] == Mark::Black )
                    continue;
                if ( marks[*j] == Mark::Grey )
                {
                    const auto from = std::find( path.begin(), path.end(), *j );
                    for ( auto it = from; it != path.end(); ++it )
                        cycle += std::format( "{}.{} -> ", layers[*it].Interface, layers[*it].Layer );
                    cycle += std::format( "{}.{}", layers[*j].Interface, layers[*j].Layer );
                    return true;
                }
                if ( visit( *j ) )
                    return true;
            }
            path.pop_back();
            marks[i] = Mark::Black;
            return false;
        };
        for ( size_t i = 0; i < layers.size(); ++i )
            if ( marks[i] == Mark::White && visit( i ) )
                return cycle;
        return {};
    }

    Common::ResultStr<std::vector<int>> PlanPoseGraph( const AnimGraph& graph, GraphScope scope )
    {
        if ( scope == GraphScope::Host )
            if ( const std::string error = LayersError( graph ); !error.empty() )
                return Common::MakeError<std::vector<int>>( error );
        return PlanNodes( graph, scope );
    }

    namespace
    {
        Common::ResultStr<std::vector<int>> PlanNodes( const AnimGraph& graph, GraphScope scope )
        {
            using Plan = std::vector<int>;

            for ( size_t i = 0; i < graph.Nodes.size(); ++i )
            {
                if ( const std::string error = NodeError( graph, graph.Nodes[i], scope ); !error.empty() )
                    return Common::MakeError<Plan>( error );
                for ( size_t j = 0; j < i; ++j )
                    if ( graph.Nodes[j].Name == graph.Nodes[i].Name )
                        return Common::MakeError<Plan>( std::format(
                             "AnimGraph '{}' has two pose nodes called '{}'; a wire to that name would "
                             "mean either",
                             graph.Name, graph.Nodes[i].Name ) );
            }

            const auto indexOf = [&graph]( std::string_view name ) -> int
            {
                for ( size_t i = 0; i < graph.Nodes.size(); ++i )
                    if ( graph.Nodes[i].Name == name )
                        return static_cast<int>( i );
                return -1;
            };

            const int output = indexOf( graph.OutputPose );
            if ( output < 0 )
                return Common::MakeError<Plan>(
                     graph.OutputPose.empty()
                          ? std::format( "AnimGraph '{}' has nothing wired into Output Pose", graph.Name )
                          : std::format( "AnimGraph '{}' wires node '{}' into Output Pose, and has no such node",
                                         graph.Name, graph.OutputPose ) );

            // Depth-first from the output through the Pose pins, post-order: a node is placed after every node
            // wired into it. `path` is the chain being walked; meeting a node on it again is a cycle.
            enum class Mark : uint8_t
            {
                Unvisited,
                OnPath,
                Placed
            };
            std::vector<Mark> marks( graph.Nodes.size(), Mark::Unvisited );
            std::vector<int>  path;
            Plan              plan;
            std::string       error;

            // Recursion is the shape of the problem: a depth-first walk over the pose DAG, its depth bounded by
            // the node count, since a node already on `path` ends the walk as a cycle instead of descending.
            // NOLINTNEXTLINE(misc-no-recursion)
            const auto visit = [&]( const auto& self, int node ) -> bool
            {
                marks[node] = Mark::OnPath;
                path.push_back( node );
                for ( const std::string& wired : graph.Nodes[node].PoseInputs )
                {
                    const int input = indexOf( wired );
                    if ( input < 0 )
                    {
                        error =
                             std::format( "AnimGraph '{}': node '{}' has a Pose pin wired to '{}', and the graph "
                                          "has no such node",
                                          graph.Name, graph.Nodes[node].Name, wired );
                        return false;
                    }
                    if ( marks[input] == Mark::OnPath )
                    {
                        std::string loop;
                        const auto  from = std::find( path.begin(), path.end(), input );
                        for ( auto it = from; it != path.end(); ++it )
                            loop += std::format( "{} -> ", graph.Nodes[*it].Name );
                        loop += graph.Nodes[input].Name;
                        error = std::format( "AnimGraph '{}' has a cycle in its pose graph: {}. A pose graph "
                                             "evaluates each node after its inputs, and a loop has no first node",
                                             graph.Name, loop );
                        return false;
                    }
                    if ( marks[input] == Mark::Unvisited && !self( self, input ) )
                        return false;
                }
                path.pop_back();
                marks[node] = Mark::Placed;
                plan.push_back( node );
                return true;
            };

            if ( !visit( visit, output ) )
                return Common::MakeError<Plan>( error );
            return Common::MakeSuccess( std::move( plan ) );
        }
    } // namespace
} // namespace Desert::Animation::Graph
