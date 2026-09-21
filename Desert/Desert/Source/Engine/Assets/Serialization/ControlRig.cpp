#include <Engine/Assets/Serialization/ControlRig.hpp>

#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Rig/RigGraph.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <span>
#include <unordered_set>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets::Serialization
{
    namespace
    {
        // The ONE table mapping the file's spelling to the enum. A second copy of it — one for reading and
        // one for writing — is how a format ends up able to write a word it cannot read.
        struct SpaceKindRow
        {
            std::string_view            Text;
            Animation::ControlSpaceKind Kind;
        };

        constexpr std::array<SpaceKindRow, 3> kSpaceKinds = { {
             { "Component", Animation::ControlSpaceKind::Component },
             { "Bone", Animation::ControlSpaceKind::Bone },
             { "Control", Animation::ControlSpaceKind::Control },
        } };

        [[nodiscard]] std::optional<Animation::ControlSpaceKind> KindFromText( const std::string& text )
        {
            for ( const auto& row : kSpaceKinds )
            {
                if ( row.Text == text )
                {
                    return row.Kind;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string TextFromKind( Animation::ControlSpaceKind kind )
        {
            for ( const auto& row : kSpaceKinds )
            {
                if ( row.Kind == kind )
                {
                    return std::string( row.Text );
                }
            }
            // Unreachable while the table covers the enum, which the static_assert below pins.
            return std::string( kSpaceKinds[0].Text );
        }

        static_assert( kSpaceKinds.size() == 3,
                       "every ControlSpaceKind must have exactly one spelling in the file; a kind with no "
                       "row is a rig this format can hold in memory and cannot write" );

        /// @param zeroScaleReason why a zero scale component is fatal FOR THIS FIELD. A parameter and not a
        /// sentence baked in here, because the two answers are genuinely different — a zero in an offset
        /// or a pose is a parent space that cannot be inverted, a zero in a shape transform is a control
        /// drawn flat — and one message covering both would be wrong about one of them. "A comment is not
        /// the code" applies to a refusal's text too: it is the only thing the rigger gets.
        [[nodiscard]] Common::BoolResultStr FiniteTransform( const std::string& control, const std::string& which,
                                                             const RigTransformData& t,
                                                             const std::string&      zeroScaleReason )
        {
            // A non-finite transform is a control whose global is NaN: it draws nothing, hit-tests to
            // nothing, and drives its bone to a matrix that poisons every skinning weight it touches. The
            // control and the field are named because "the rig is invalid" is a morning spent bisecting.
            for ( int c = 0; c < 3; ++c )
            {
                if ( !std::isfinite( t.Translation[c] ) || !std::isfinite( t.Scale[c] ) )
                {
                    return Common::MakeFormattedError<bool>( "control '{}' has a non-finite {} on component {}",
                                                             control, which, c );
                }
            }
            for ( int c = 0; c < 4; ++c )
            {
                if ( !std::isfinite( t.Rotation[c] ) )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a non-finite {} rotation on component {}", control, which, c );
                }
            }
            if ( t.Scale.x == 0.0f || t.Scale.y == 0.0f || t.Scale.z == 0.0f )
            {
                return Common::MakeFormattedError<bool>( "control '{}' has a zero component in its {} scale "
                                                         "({}, {}, {}); {}",
                                                         control, which, t.Scale.x, t.Scale.y, t.Scale.z,
                                                         zeroScaleReason );
            }
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] Animation::BoneTransform ToBoneTransform( const RigTransformData& t )
        {
            Animation::BoneTransform out;
            out.Translation = t.Translation;
            out.Rotation    = t.Rotation;
            out.Scale       = t.Scale;
            return out;
        }

        [[nodiscard]] RigTransformData FromBoneTransform( const Animation::BoneTransform& t )
        {
            RigTransformData out;
            out.Translation = t.Translation;
            out.Rotation    = t.Rotation;
            out.Scale       = t.Scale;
            return out;
        }

        /// Refuses a parent chain that closes on itself, walking only the `Control` slots — the other two
        /// kinds terminate by construction. Iterative rather than recursive: a hand-edited file is exactly
        /// where an unbounded depth can come from, and a stack overflow is not a refusal.
        [[nodiscard]] Common::BoolResultStr RefuseCycles( const ControlRigData&                          data,
                                                          const std::unordered_map<std::string, size_t>& byName )
        {
            // 0 = unvisited, 1 = on the current path, 2 = proven acyclic. The classic colouring, because a
            // plain visited-set answers "have I been here" and not "am I inside my own subtree".
            std::vector<uint8_t> state( data.Controls.size(), 0 );
            std::vector<size_t>  stack;
            std::vector<size_t>  cursor;

            for ( size_t root = 0; root < data.Controls.size(); ++root )
            {
                if ( state[root] != 0 )
                {
                    continue;
                }
                stack.clear();
                cursor.clear();
                stack.push_back( root );
                cursor.push_back( 0 );
                state[root] = 1;

                while ( !stack.empty() )
                {
                    const size_t node  = stack.back();
                    size_t&      slot  = cursor.back();
                    const auto&  slots = data.Controls[node].Parents;

                    if ( slot >= slots.size() )
                    {
                        state[node] = 2;
                        stack.pop_back();
                        cursor.pop_back();
                        continue;
                    }

                    const ControlSpaceData& space = slots[slot];
                    ++slot;
                    if ( space.Kind != "Control" )
                    {
                        continue;
                    }

                    const auto found = byName.find( space.Target );
                    if ( found == byName.end() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' is parented to '{}', which this rig does not define",
                             data.Controls[node].Name, space.Target );
                    }

                    const size_t parent = found->second;
                    if ( state[parent] == 1 )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' is parented to '{}', which closes a cycle; a control cannot be "
                             "its own ancestor because its global would have no fixed point",
                             data.Controls[node].Name, space.Target );
                    }
                    if ( state[parent] == 0 )
                    {
                        state[parent] = 1;
                        stack.push_back( parent );
                        cursor.push_back( 0 );
                    }
                }
            }
            return Common::MakeSuccess( true );
        }

        /// Controls in an order where every `Control` parent precedes its child. `ControlHierarchy::Add`
        /// refuses an unknown parent, so a file written child-first is legal content that would be refused
        /// for the wrong reason; ordering it here is what makes the file's own order not matter.
        [[nodiscard]] std::vector<size_t> ResolveOrder( const ControlRigData&                          data,
                                                        const std::unordered_map<std::string, size_t>& byName )
        {
            std::vector<size_t>  order;
            std::vector<uint8_t> emitted( data.Controls.size(), 0 );
            order.reserve( data.Controls.size() );

            std::vector<size_t> stack;
            std::vector<size_t> cursor;
            for ( size_t root = 0; root < data.Controls.size(); ++root )
            {
                if ( emitted[root] != 0 )
                {
                    continue;
                }
                stack.clear();
                cursor.clear();
                stack.push_back( root );
                cursor.push_back( 0 );

                while ( !stack.empty() )
                {
                    const size_t node  = stack.back();
                    size_t&      slot  = cursor.back();
                    const auto&  slots = data.Controls[node].Parents;

                    if ( slot >= slots.size() )
                    {
                        if ( emitted[node] == 0 )
                        {
                            emitted[node] = 1;
                            order.push_back( node );
                        }
                        stack.pop_back();
                        cursor.pop_back();
                        continue;
                    }

                    const ControlSpaceData& space = slots[slot];
                    ++slot;
                    if ( space.Kind != "Control" )
                    {
                        continue;
                    }
                    // Validate has already established the name resolves and the graph is acyclic, which
                    // is what makes this walk terminate without a depth guard of its own.
                    const size_t parent = byName.at( space.Target );
                    if ( emitted[parent] == 0 )
                    {
                        stack.push_back( parent );
                        cursor.push_back( 0 );
                    }
                }
            }
            return order;
        }
        // ---- the graph's file form ------------------------------------------------------------------
        //
        // Every rule below asks `Animation::RigNodeDescriptors()` — the same table the walk dispatches on.
        // There is no list of kinds, pin names or pin types in this file, and that is deliberate: the third
        // paragraph of `RigGraph.hpp` says why the pin table had to be data rather than a virtual, and this
        // is the caller it had to be data FOR.

        [[nodiscard]] std::optional<Animation::RigValueKind> PayloadKind( const RigGraphInputData& input )
        {
            int                                    present = 0;
            std::optional<Animation::RigValueKind> kind;

            if ( input.Link.has_value() )
            {
                ++present;
            }
            if ( input.Float.has_value() )
            {
                ++present;
                kind = Animation::RigValueKind::Float;
            }
            if ( input.Vec3.has_value() )
            {
                ++present;
                kind = Animation::RigValueKind::Vec3;
            }
            if ( input.Quat.has_value() )
            {
                ++present;
                kind = Animation::RigValueKind::Quat;
            }
            if ( input.Transform.has_value() )
            {
                ++present;
                kind = Animation::RigValueKind::Transform;
            }

            if ( present != 1 )
            {
                return std::nullopt;
            }
            // A link's type is the producing pin's, which only the caller can look up.
            return input.Link.has_value() ? std::optional<Animation::RigValueKind>{} : kind;
        }

        [[nodiscard]] int PayloadCount( const RigGraphInputData& input )
        {
            return static_cast<int>( input.Link.has_value() ) + static_cast<int>( input.Float.has_value() ) +
                   static_cast<int>( input.Vec3.has_value() ) + static_cast<int>( input.Quat.has_value() ) +
                   static_cast<int>( input.Transform.has_value() );
        }

        [[nodiscard]] std::optional<size_t> FindPin( std::span<const Animation::RigPin> pins,
                                                     const std::string&                 name )
        {
            for ( size_t i = 0; i < pins.size(); ++i )
            {
                if ( pins[i].Name == name )
                {
                    return i;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] Common::BoolResultStr
        ValidateRigGraphData( const RigGraphData&                            graph,
                              const std::unordered_map<std::string, size_t>& controlsByName )
        {
            if ( graph.Nodes.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "the rig declares a graph with no nodes; a rig without a forwards solve is spelled by "
                     "leaving the Graph field out, not by writing an empty one" );
            }

            // EVERY NAME IS COLLECTED BEFORE ANY LINK IS RESOLVED, and that is a diagnostic decision rather
            // than a structural one. Building the map as the loop went made a FORWARD link report "this
            // graph does not define 'place'" — true of the map at that instant, false of the file, and
            // exactly the kind of message that sends a rigger looking for a typo in a name that is right
            // there three lines below. With every name known, "does not define" means absent and "comes
            // later" means later.
            std::unordered_map<std::string, size_t> byName;
            byName.reserve( graph.Nodes.size() );

            for ( size_t i = 0; i < graph.Nodes.size(); ++i )
            {
                if ( graph.Nodes[i].Name.empty() )
                {
                    return Common::MakeFormattedError<bool>(
                         "graph node {} has an empty name; the name is what a link, a refusal and an editor "
                         "row all bind on",
                         i );
                }
                if ( !byName.emplace( graph.Nodes[i].Name, i ).second )
                {
                    return Common::MakeFormattedError<bool>(
                         "two graph nodes are named '{}'; a link naming it would have two answers",
                         graph.Nodes[i].Name );
                }
            }

            std::vector<std::string>            names;
            std::vector<Animation::RigNodeKind> kinds;
            std::vector<std::vector<uint32_t>>  producers( graph.Nodes.size() );

            for ( size_t i = 0; i < graph.Nodes.size(); ++i )
            {
                const RigGraphNodeData& node = graph.Nodes[i];

                const auto kind = Animation::RigNodeKindFromText( node.Kind );
                if ( !kind.has_value() )
                {
                    return Common::MakeFormattedError<bool>(
                         "graph node '{}' is of kind '{}', which this build does not know", node.Name, node.Kind );
                }

                const Animation::RigNodeDescriptor& desc = Animation::DescribeRigNode( *kind );

                switch ( desc.Target )
                {
                    case Animation::RigNodeTargetKind::Control:
                        if ( node.Target.empty() )
                        {
                            return Common::MakeFormattedError<bool>( "graph node '{}' ({}) names no control",
                                                                     node.Name, node.Kind );
                        }
                        if ( controlsByName.find( node.Target ) == controlsByName.end() )
                        {
                            return Common::MakeFormattedError<bool>(
                                 "graph node '{}' names control '{}', which this rig does not define", node.Name,
                                 node.Target );
                        }
                        break;
                    case Animation::RigNodeTargetKind::Bone:
                        // The BONE's existence is a question for a skeleton, and this function has none;
                        // `BuildControlRig` refuses an unknown bone by name, as it already does for a space.
                        if ( node.Target.empty() )
                        {
                            return Common::MakeFormattedError<bool>( "graph node '{}' ({}) names no bone",
                                                                     node.Name, node.Kind );
                        }
                        break;
                    case Animation::RigNodeTargetKind::None:
                        if ( !node.Target.empty() )
                        {
                            return Common::MakeFormattedError<bool>(
                                 "graph node '{}' is a {}, which names nothing, yet it targets '{}'", node.Name,
                                 node.Kind, node.Target );
                        }
                        break;
                }

                if ( desc.UsesSpace )
                {
                    if ( !Animation::RigControlSpaceFromText( node.Space ).has_value() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' ({}) has space '{}', which is neither Local nor Global", node.Name,
                             node.Kind, node.Space );
                    }
                }
                else if ( !node.Space.empty() )
                {
                    return Common::MakeFormattedError<bool>(
                         "graph node '{}' is a {}, which has no control space, yet it asks for '{}'", node.Name,
                         node.Kind, node.Space );
                }

                if ( node.Inputs.size() != desc.Inputs.size() )
                {
                    return Common::MakeFormattedError<bool>( "graph node '{}' ({}) has {} inputs and the kind "
                                                             "takes {}",
                                                             node.Name, node.Kind, node.Inputs.size(),
                                                             desc.Inputs.size() );
                }

                std::vector<uint8_t> filled( desc.Inputs.size(), 0 );

                for ( const RigGraphInputData& input : node.Inputs )
                {
                    const auto pin = FindPin( desc.Inputs, input.Pin );
                    if ( !pin.has_value() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' ({}) has an input for pin '{}', which the kind does not have",
                             node.Name, node.Kind, input.Pin );
                    }
                    if ( filled[*pin] != 0 )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' fills pin '{}' twice; which of the two the walk would read has "
                             "no answer that is not invented here",
                             node.Name, input.Pin );
                    }
                    filled[*pin] = 1;

                    const int payloads = PayloadCount( input );
                    if ( payloads == 0 )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' pin '{}' carries neither a link nor a value", node.Name, input.Pin );
                    }
                    if ( payloads > 1 )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' pin '{}' carries {} payloads; a precedence rule would be a "
                             "second fact about which one wins, and the loser would be invisible",
                             node.Name, input.Pin, payloads );
                    }

                    const Animation::RigValueKind expected = desc.Inputs[*pin].Type;

                    if ( !input.Link.has_value() )
                    {
                        const Animation::RigValueKind actual = *PayloadKind( input );
                        if ( actual != expected )
                        {
                            return Common::MakeFormattedError<bool>(
                                 "graph node '{}' has a {} value on pin '{}', which is a {}", node.Name,
                                 Animation::ToString( actual ), input.Pin, Animation::ToString( expected ) );
                        }
                        if ( input.Float.has_value() && !std::isfinite( *input.Float ) )
                        {
                            return Common::MakeFormattedError<bool>(
                                 "graph node '{}' has a non-finite value on pin '{}'", node.Name, input.Pin );
                        }
                        if ( input.Vec3.has_value() )
                        {
                            for ( int c = 0; c < 3; ++c )
                            {
                                if ( !std::isfinite( ( *input.Vec3 )[c] ) )
                                {
                                    return Common::MakeFormattedError<bool>(
                                         "graph node '{}' has a non-finite value on pin '{}'", node.Name,
                                         input.Pin );
                                }
                            }
                        }
                        if ( input.Quat.has_value() )
                        {
                            for ( int c = 0; c < 4; ++c )
                            {
                                if ( !std::isfinite( ( *input.Quat )[c] ) )
                                {
                                    return Common::MakeFormattedError<bool>(
                                         "graph node '{}' has a non-finite value on pin '{}'", node.Name,
                                         input.Pin );
                                }
                            }
                        }
                        if ( input.Transform.has_value() )
                        {
                            if ( auto ok = FiniteTransform( node.Name, "graph value", *input.Transform,
                                                            "a transform scaled to zero on an axis cannot be "
                                                            "inverted and reaches a bone as a basis that "
                                                            "will not decompose" );
                                 !ok )
                            {
                                return ok;
                            }
                        }
                        continue;
                    }

                    const RigLinkData& link  = *input.Link;
                    const auto         found = byName.find( link.Node );
                    if ( found == byName.end() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' pin '{}' links to '{}', which this graph does not define", node.Name,
                             input.Pin, link.Node );
                    }
                    if ( found->second >= i )
                    {
                        // THE RULE THAT REPLACES CYCLE DETECTION. A graph runs in the order it is written,
                        // because the hierarchy it reads and writes is state no sort can see; a link
                        // therefore points backwards or not at all, and a cycle cannot be spelled.
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' pin '{}' links to '{}', which is itself or comes later in the "
                             "file; a graph runs in the order it is written and a link points backwards",
                             node.Name, input.Pin, link.Node );
                    }

                    const auto producerKind = Animation::RigNodeKindFromText( graph.Nodes[found->second].Kind );
                    const Animation::RigNodeDescriptor& producer = Animation::DescribeRigNode( *producerKind );

                    const auto outPin = FindPin( producer.Outputs, link.Pin );
                    if ( !outPin.has_value() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' pin '{}' reads output '{}' of '{}' ({}), which it does not have",
                             node.Name, input.Pin, link.Pin, link.Node, producer.Name );
                    }
                    if ( producer.Outputs[*outPin].Type != expected )
                    {
                        return Common::MakeFormattedError<bool>(
                             "graph node '{}' wires '{}' of '{}', a {}, into pin '{}', which is a {}", node.Name,
                             link.Pin, link.Node, Animation::ToString( producer.Outputs[*outPin].Type ), input.Pin,
                             Animation::ToString( expected ) );
                    }

                    producers[i].push_back( static_cast<uint32_t>( found->second ) );
                }

                names.push_back( node.Name );
                kinds.push_back( *kind );
            }

            // THE SAME FUNCTION THE LOADER ASKS. See its comment: one walk, two callers, so a rig editor
            // cannot save a file the loader would refuse.
            return Animation::RefuseDiscardedWork( names, kinds, producers );
        }

    } // namespace

    Common::BoolResultStr ValidateControlRigData( const ControlRigData& data )
    {
        if ( data.Controls.empty() )
        {
            return Common::MakeFormattedError<bool>( "rig '{}' defines no controls", data.Name );
        }

        std::unordered_map<std::string, size_t> byName;
        byName.reserve( data.Controls.size() );

        for ( size_t i = 0; i < data.Controls.size(); ++i )
        {
            const ControlElementData& control = data.Controls[i];
            if ( control.Name.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "control {} has an empty name; the name is the only key a drive, a space and a "
                     "Sequencer track bind on",
                     i );
            }
            if ( !byName.emplace( control.Name, i ).second )
            {
                return Common::MakeFormattedError<bool>(
                     "two controls are named '{}'; an animator would have two things to select that are one "
                     "thing when keyed",
                     control.Name );
            }

            constexpr const char* kSpaceReason = "the parent space it builds cannot be inverted, so a drag "
                                                 "on it or on any child would have no answer";
            if ( auto ok = FiniteTransform( control.Name, "offset", control.Offset, kSpaceReason ); !ok )
            {
                return ok;
            }
            if ( auto ok = FiniteTransform( control.Name, "pose", control.Pose, kSpaceReason ); !ok )
            {
                return ok;
            }
            if ( control.ShapeTransform.has_value() )
            {
                // REFUSED AND NOT CLAMPED. A flattened shape draws as a line or a point: invisible at any
                // zoom and unhittable, which is the identical symptom to a typo'd shape name that this
                // format already refuses by name. A knob that can reach that state silently would be the
                // "a knob that hides a defect instead of fixing it" the contract forbids.
                if ( auto ok = FiniteTransform( control.Name, "shape transform", *control.ShapeTransform,
                                                "a shape scaled to zero on an axis draws flat, which is a "
                                                "control the animator can neither see nor grab" );
                     !ok )
                {
                    return ok;
                }
            }
        }

        for ( const ControlElementData& control : data.Controls )
        {
            if ( control.Parents.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "control '{}' declares no parent space; a control with no space is not 'in world "
                     "space', it is a control whose parent transform is undefined. Declare a Component slot",
                     control.Name );
            }

            float weightSum = 0.0f;
            for ( const ControlSpaceData& space : control.Parents )
            {
                const auto kind = KindFromText( space.Kind );
                if ( !kind.has_value() )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a parent space of kind '{}', which is not one of Component, Bone "
                         "or Control",
                         control.Name, space.Kind );
                }
                if ( !std::isfinite( space.Weight ) || space.Weight < 0.0f )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a parent space with weight {}; a weight is a non-negative finite "
                         "number",
                         control.Name, space.Weight );
                }
                weightSum += space.Weight;

                if ( *kind == Animation::ControlSpaceKind::Component )
                {
                    if ( !space.Target.empty() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' has a Component space naming '{}'; the mesh's own space has no "
                             "target, and a slot that says both is two statements about one fact",
                             control.Name, space.Target );
                    }
                }
                else if ( space.Target.empty() )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a {} space that names nothing to follow", control.Name, space.Kind );
                }
                else if ( *kind == Animation::ControlSpaceKind::Control )
                {
                    if ( space.Target == control.Name )
                    {
                        return Common::MakeFormattedError<bool>( "control '{}' is parented to itself",
                                                                 control.Name );
                    }
                    if ( byName.find( space.Target ) == byName.end() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' is parented to '{}', which this rig does not define", control.Name,
                             space.Target );
                    }
                }
            }

            if ( weightSum <= 0.0f )
            {
                return Common::MakeFormattedError<bool>(
                     "control '{}' has parent spaces whose weights are all zero; the blend would be "
                     "normalised by zero and the answer would be invented",
                     control.Name );
            }
        }

        if ( auto ok = RefuseCycles( data, byName ); !ok )
        {
            return ok;
        }

        if ( data.Drives.empty() )
        {
            // The load-bearing refusal. See the header: a rig that drives no bones is a pipeline stage that
            // cannot change the pose, and such a stage passes every assertion a working one passes.
            return Common::MakeFormattedError<bool>(
                 "rig '{}' drives no bones; it would be a pipeline stage that cannot change the pose, which "
                 "is indistinguishable from a rig that works",
                 data.Name );
        }

        std::unordered_set<std::string> drivenBones;
        drivenBones.reserve( data.Drives.size() );
        for ( size_t i = 0; i < data.Drives.size(); ++i )
        {
            const ControlDriveData& drive = data.Drives[i];
            if ( drive.Control.empty() || drive.Bone.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "drive {} names control '{}' and bone '{}'; both halves are required", i, drive.Control,
                     drive.Bone );
            }
            if ( byName.find( drive.Control ) == byName.end() )
            {
                return Common::MakeFormattedError<bool>(
                     "drive {} names control '{}', which this rig does not define", i, drive.Control );
            }
            if ( !drivenBones.emplace( drive.Bone ).second )
            {
                return Common::MakeFormattedError<bool>(
                     "two drives write bone '{}'; which control wins has no answer that is not invented here",
                     drive.Bone );
            }
        }

        if ( data.Graph.has_value() )
        {
            if ( auto ok = ValidateRigGraphData( *data.Graph, byName ); !ok )
            {
                return Common::MakeFormattedError<bool>( "rig '{}': {}", data.Name, ok.GetError() );
            }
        }

        return Common::MakeSuccess( true );
    }

    Common::ResultStr<ControlRigData> ParseControlRig( const std::string& text )
    {
        if ( text.empty() )
        {
            return Common::MakeFormattedError<ControlRigData>( "the file is empty" );
        }

        // THE VERSION IS READ FIRST, ON ITS OWN, as an untyped tree — see the header. A struct imposes the
        // rest of the schema on a document whose whole problem may be that it does not match the schema.
        if ( const auto tree = rfl::json::read<rfl::Generic>( text ); tree )
        {
            if ( const auto fields = tree.value().to_object(); fields )
            {
                if ( const auto stated = fields.value().get( "FormatVersion" ); stated.has_value() )
                {
                    const auto number = stated.value().to_int();
                    if ( number.has_value() && number.value() != kControlRigVersion )
                    {
                        return Common::MakeFormattedError<ControlRigData>(
                             "control rig format version {} was written by a different build; this one reads "
                             "version {}",
                             number.value(), kControlRigVersion );
                    }
                }
            }
        }

        const auto parsed = rfl::json::read<ControlRigData>( text );
        if ( !parsed )
        {
            return Common::MakeFormattedError<ControlRigData>( "{}", parsed.error().what() );
        }

        ControlRigData data = parsed.value();

        const int32_t version = data.FormatVersion.value_or( kControlRigVersion );
        if ( version != kControlRigVersion )
        {
            return Common::MakeFormattedError<ControlRigData>(
                 "control rig format version {} was written by a different build; this one reads version {}",
                 version, kControlRigVersion );
        }

        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return Common::MakeFormattedError<ControlRigData>( "{}", valid.GetError() );
        }

        data.FormatVersion = kControlRigVersion;
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteControlRig( const ControlRigData& data )
    {
        ControlRigData out = data;
        out.FormatVersion  = kControlRigVersion;
        return rfl::json::write( out, YYJSON_WRITE_PRETTY );
    }

    Common::ResultStr<ControlRigData> LoadControlRigFile( const std::filesystem::path& path )
    {
        auto text = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !text )
        {
            return Common::MakeFormattedError<ControlRigData>( "cannot read control rig '{}': {}", path.string(),
                                                               text.GetError() );
        }

        auto parsed = ParseControlRig( text.GetValue() );
        if ( !parsed )
        {
            return Common::MakeFormattedError<ControlRigData>( "control rig '{}': {}", path.string(),
                                                               parsed.GetError() );
        }
        return parsed;
    }

    Common::BoolResultStr SaveControlRigFile( const std::filesystem::path& path, const ControlRigData& data )
    {
        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return Common::MakeFormattedError<bool>( "refusing to write control rig '{}': {}", path.string(),
                                                     valid.GetError() );
        }
        return Common::Utils::FileSystem::WriteContentToFileAtomic( path, WriteControlRig( data ) );
    }

    Common::BoolResultStr BuildControlRig( const ControlRigData& data, const Animation::Skeleton& skeleton,
                                           Animation::ControlRigStage& out )
    {
        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return valid;
        }

        std::unordered_map<std::string, size_t> byName;
        byName.reserve( data.Controls.size() );
        for ( size_t i = 0; i < data.Controls.size(); ++i )
        {
            byName.emplace( data.Controls[i].Name, i );
        }

        Animation::ControlHierarchy& hierarchy = out.GetHierarchy();

        // Where the file's names become this skeleton's indices, and the ONLY place they do.
        std::unordered_map<std::string, uint32_t> controlIndex;
        controlIndex.reserve( data.Controls.size() );

        for ( const size_t source : ResolveOrder( data, byName ) )
        {
            const ControlElementData& file = data.Controls[source];

            Animation::ControlElement element;
            element.Name      = file.Name;
            element.ShapeName = file.ShapeName;
            // ABSENT IS IDENTITY, spelled once, here. `RigTransformData{}` is the identity TRS (scale one,
            // unit quaternion), which is exactly the composition BuildFrame performed before this field
            // existed — that equivalence is what lets `kControlRigVersion` stay at 1.
            element.ShapeTransform = ToBoneTransform( file.ShapeTransform.value_or( RigTransformData{} ) );
            element.Offset         = ToBoneTransform( file.Offset );
            element.Pose           = ToBoneTransform( file.Pose );
            element.Parents.reserve( file.Parents.size() );

            for ( const ControlSpaceData& space : file.Parents )
            {
                Animation::ControlSpace slot;
                // Validate has established the spelling is one of the three.
                slot.Kind   = *KindFromText( space.Kind );
                slot.Weight = space.Weight;

                if ( slot.Kind == Animation::ControlSpaceKind::Bone )
                {
                    const auto bone = skeleton.FindBoneIndex( space.Target );
                    if ( !bone.has_value() )
                    {
                        // REFUSED, NOT RESOLVED TO IDENTITY. See the header: the silent version of this is
                        // every control at the origin with nothing said.
                        return Common::MakeFormattedError<bool>(
                             "rig '{}': control '{}' follows bone '{}', which this skeleton (signature {}) "
                             "does not have",
                             data.Name, file.Name, space.Target, skeleton.GetSignature() );
                    }
                    slot.Index = *bone;
                }
                else if ( slot.Kind == Animation::ControlSpaceKind::Control )
                {
                    const auto parent = controlIndex.find( space.Target );
                    if ( parent == controlIndex.end() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "rig '{}': control '{}' follows control '{}', which the resolve order did not "
                             "place before it",
                             data.Name, file.Name, space.Target );
                    }
                    slot.Index = parent->second;
                }

                element.Parents.push_back( slot );
            }

            auto added = hierarchy.Add( std::move( element ) );
            if ( !added )
            {
                return Common::MakeFormattedError<bool>( "rig '{}': {}", data.Name, added.GetError() );
            }
            controlIndex.emplace( file.Name, added.GetValue() );
        }

        std::vector<Animation::ControlBoneDrive> drives;
        drives.reserve( data.Drives.size() );
        for ( const ControlDriveData& drive : data.Drives )
        {
            const auto bone = skeleton.FindBoneIndex( drive.Bone );
            if ( !bone.has_value() )
            {
                return Common::MakeFormattedError<bool>(
                     "rig '{}': control '{}' drives bone '{}', which this skeleton (signature {}) does not "
                     "have",
                     data.Name, drive.Control, drive.Bone, skeleton.GetSignature() );
            }

            Animation::ControlBoneDrive resolved;
            // Validate has established the control name is defined, and every defined control was added.
            resolved.Control = controlIndex.at( drive.Control );
            resolved.Bone    = *bone;
            drives.push_back( resolved );
        }

        if ( auto ok = out.SetDrives( skeleton, std::move( drives ) ); !ok )
        {
            return Common::MakeFormattedError<bool>( "rig '{}': {}", data.Name, ok.GetError() );
        }

        if ( !data.Graph.has_value() )
        {
            // NO GRAPH IS A WHOLE RIG. `ControlRigStage` without one runs the identity solve T5.4 shipped,
            // which is what a generation-1 `.derig` has always meant, and the byte-for-byte proof of that
            // is what lets `kControlRigVersion` stay at 1.
            return Common::MakeSuccess( true );
        }

        // ---- file form -> the walk ------------------------------------------------------------------
        //
        // The order of `Nodes` becomes the order of execution, and `Validate` has already established that
        // every link points at an earlier one, so the index of a link's target is simply its position here.
        std::unordered_map<std::string, uint32_t> nodeIndex;
        nodeIndex.reserve( data.Graph->Nodes.size() );

        std::vector<Animation::RigNode> nodes;
        nodes.reserve( data.Graph->Nodes.size() );

        for ( const RigGraphNodeData& file : data.Graph->Nodes )
        {
            Animation::RigNode node;
            node.Name = file.Name;
            // Validate has established the spelling, the target, the space, the pins and the types.
            node.Kind = *Animation::RigNodeKindFromText( file.Kind );

            const Animation::RigNodeDescriptor& desc = Animation::DescribeRigNode( node.Kind );

            if ( desc.Target == Animation::RigNodeTargetKind::Control )
            {
                node.Target = controlIndex.at( file.Target );
            }
            else if ( desc.Target == Animation::RigNodeTargetKind::Bone )
            {
                const auto bone = skeleton.FindBoneIndex( file.Target );
                if ( !bone.has_value() )
                {
                    // REFUSED, NOT RESOLVED TO IDENTITY, for the reason every other bone name here is.
                    return Common::MakeFormattedError<bool>(
                         "rig '{}': graph node '{}' reads bone '{}', which this skeleton (signature {}) does "
                         "not have",
                         data.Name, file.Name, file.Target, skeleton.GetSignature() );
                }
                node.Target = *bone;
            }

            if ( desc.UsesSpace )
            {
                node.Space = *Animation::RigControlSpaceFromText( file.Space );
            }

            // BY THE DESCRIPTOR'S PIN ORDER, NOT THE FILE'S. The walk indexes `Inputs` positionally, and a
            // rigger who lists Alpha before A has written a legal file that means what it says.
            node.Inputs.assign( desc.Inputs.size(), Animation::RigNodeInput{} );

            for ( const RigGraphInputData& input : file.Inputs )
            {
                const size_t            pin = *FindPin( desc.Inputs, input.Pin );
                Animation::RigNodeInput wired;

                if ( input.Link.has_value() )
                {
                    const auto&                         link     = *input.Link;
                    const uint32_t                      producer = nodeIndex.at( link.Node );
                    const Animation::RigNodeDescriptor& from = Animation::DescribeRigNode( nodes[producer].Kind );
                    wired.Node                               = producer;
                    wired.Pin = static_cast<uint8_t>( *FindPin( from.Outputs, link.Pin ) );
                }
                else if ( input.Float.has_value() )
                {
                    wired.Literal = Animation::RigValue{ *input.Float };
                }
                else if ( input.Vec3.has_value() )
                {
                    wired.Literal = Animation::RigValue{ *input.Vec3 };
                }
                else if ( input.Quat.has_value() )
                {
                    wired.Literal = Animation::RigValue{ *input.Quat };
                }
                else
                {
                    wired.Literal = Animation::RigValue{ ToBoneTransform( *input.Transform ) };
                }

                node.Inputs[pin] = wired;
            }

            nodeIndex.emplace( file.Name, static_cast<uint32_t>( nodes.size() ) );
            nodes.push_back( std::move( node ) );
        }

        Animation::RigGraph graph;
        if ( auto built = graph.SetNodes( out.GetHierarchy(), skeleton.GetBones().size(), std::move( nodes ) );
             !built )
        {
            return Common::MakeFormattedError<bool>( "rig '{}': {}", data.Name, built.GetError() );
        }
        if ( auto installed = out.SetGraph( std::move( graph ) ); !installed )
        {
            return Common::MakeFormattedError<bool>( "rig '{}': {}", data.Name, installed.GetError() );
        }

        return Common::MakeSuccess( true );
    }

    Common::ResultStr<ControlRigData> BuildDataFromControlRig( const std::string&                name,
                                                               const Animation::ControlRigStage& rig,
                                                               const Animation::Skeleton&        skeleton )
    {
        const Animation::ControlHierarchy& hierarchy = rig.GetHierarchy();

        ControlRigData data;
        data.FormatVersion = kControlRigVersion;
        data.Name          = name;
        data.Controls.reserve( hierarchy.Size() );

        const auto boneName = [&skeleton]( uint32_t index ) -> Common::ResultStr<std::string>
        {
            if ( index >= skeleton.GetBones().size() )
            {
                return Common::MakeFormattedError<std::string>(
                     "bone index {} is outside this skeleton's {} bones", index, skeleton.GetBones().size() );
            }
            return Common::MakeSuccess( skeleton.GetBones()[index].Name );
        };

        for ( uint32_t i = 0; i < static_cast<uint32_t>( hierarchy.Size() ); ++i )
        {
            const Animation::ControlElement& control = hierarchy.Get( i );

            ControlElementData file;
            file.Name      = control.Name;
            file.ShapeName = control.ShapeName;
            file.Offset    = FromBoneTransform( control.Offset );
            file.Pose      = FromBoneTransform( control.Pose );

            // THE CANONICAL SPELLING OF "no shape transform" IS THE ABSENT FIELD, chosen once and here.
            // Both spellings are legal input and build the same control, so the writer picks one: a rig
            // whose controls are all default does not grow a block of ones per control, and a generation-1
            // file round-trips through this function unchanged instead of gaining fields it never had.
            if ( const RigTransformData shape = FromBoneTransform( control.ShapeTransform );
                 shape != RigTransformData{} )
            {
                file.ShapeTransform = shape;
            }
            file.Parents.reserve( control.Parents.size() );

            for ( const Animation::ControlSpace& slot : control.Parents )
            {
                ControlSpaceData space;
                space.Kind   = TextFromKind( slot.Kind );
                space.Weight = slot.Weight;

                if ( slot.Kind == Animation::ControlSpaceKind::Bone )
                {
                    auto named = boneName( slot.Index );
                    if ( !named )
                    {
                        return Common::MakeFormattedError<ControlRigData>( "control '{}': {}", control.Name,
                                                                           named.GetError() );
                    }
                    space.Target = named.GetValue();
                }
                else if ( slot.Kind == Animation::ControlSpaceKind::Control )
                {
                    if ( slot.Index >= hierarchy.Size() )
                    {
                        return Common::MakeFormattedError<ControlRigData>(
                             "control '{}' follows control index {}, which this rig does not have", control.Name,
                             slot.Index );
                    }
                    space.Target = hierarchy.Get( slot.Index ).Name;
                }

                file.Parents.push_back( std::move( space ) );
            }

            data.Controls.push_back( std::move( file ) );
        }

        data.Drives.reserve( rig.GetDrives().size() );
        for ( const Animation::ControlBoneDrive& drive : rig.GetDrives() )
        {
            if ( drive.Control >= hierarchy.Size() )
            {
                return Common::MakeFormattedError<ControlRigData>(
                     "a drive names control index {}, which this rig does not have", drive.Control );
            }
            auto named = boneName( drive.Bone );
            if ( !named )
            {
                return Common::MakeFormattedError<ControlRigData>( "a drive: {}", named.GetError() );
            }

            ControlDriveData file;
            file.Control = hierarchy.Get( drive.Control ).Name;
            file.Bone    = named.GetValue();
            data.Drives.push_back( std::move( file ) );
        }

        if ( rig.HasGraph() )
        {
            RigGraphData graph;
            graph.Nodes.reserve( rig.GetGraph().GetNodes().size() );

            const std::vector<Animation::RigNode>& nodes = rig.GetGraph().GetNodes();
            for ( const Animation::RigNode& node : nodes )
            {
                const Animation::RigNodeDescriptor& desc = Animation::DescribeRigNode( node.Kind );

                RigGraphNodeData file;
                file.Name = node.Name;
                file.Kind = std::string( desc.Name );

                if ( desc.Target == Animation::RigNodeTargetKind::Control )
                {
                    if ( node.Target >= hierarchy.Size() )
                    {
                        return Common::MakeFormattedError<ControlRigData>(
                             "graph node '{}' names control index {}, which this rig does not have", node.Name,
                             node.Target );
                    }
                    file.Target = hierarchy.Get( node.Target ).Name;
                }
                else if ( desc.Target == Animation::RigNodeTargetKind::Bone )
                {
                    auto named = boneName( node.Target );
                    if ( !named )
                    {
                        return Common::MakeFormattedError<ControlRigData>( "graph node '{}': {}", node.Name,
                                                                           named.GetError() );
                    }
                    file.Target = named.GetValue();
                }

                // EMPTY IS THE CANONICAL SPELLING OF "this kind has no space", chosen once and here — the
                // same rule `ShapeTransform`'s absent field follows, and what makes the round trip stable.
                if ( desc.UsesSpace )
                {
                    file.Space = std::string( Animation::ToString( node.Space ) );
                }

                file.Inputs.reserve( node.Inputs.size() );
                for ( size_t pin = 0; pin < node.Inputs.size(); ++pin )
                {
                    const Animation::RigNodeInput& wired = node.Inputs[pin];

                    RigGraphInputData input;
                    input.Pin = std::string( desc.Inputs[pin].Name );

                    if ( wired.Node == Animation::RigNodeInput::LITERAL )
                    {
                        switch ( static_cast<Animation::RigValueKind>( wired.Literal.index() ) )
                        {
                            case Animation::RigValueKind::Float:
                                input.Float = std::get<float>( wired.Literal );
                                break;
                            case Animation::RigValueKind::Vec3:
                                input.Vec3 = std::get<glm::vec3>( wired.Literal );
                                break;
                            case Animation::RigValueKind::Quat:
                                input.Quat = std::get<glm::quat>( wired.Literal );
                                break;
                            case Animation::RigValueKind::Transform:
                                input.Transform =
                                     FromBoneTransform( std::get<Animation::BoneTransform>( wired.Literal ) );
                                break;
                        }
                    }
                    else
                    {
                        const Animation::RigNodeDescriptor& from =
                             Animation::DescribeRigNode( nodes[wired.Node].Kind );

                        RigLinkData link;
                        link.Node  = nodes[wired.Node].Name;
                        link.Pin   = std::string( from.Outputs[wired.Pin].Name );
                        input.Link = std::move( link );
                    }

                    file.Inputs.push_back( std::move( input ) );
                }

                graph.Nodes.push_back( std::move( file ) );
            }

            data.Graph = std::move( graph );
        }

        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return Common::MakeFormattedError<ControlRigData>( "{}", valid.GetError() );
        }

        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets::Serialization
