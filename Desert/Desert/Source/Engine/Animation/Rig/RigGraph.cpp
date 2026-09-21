#include "RigGraph.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Desert::Animation
{
    namespace
    {
        // ---- the pin tables --------------------------------------------------------------------------
        //
        // Every array here is non-empty on purpose: a kind with no inputs or no outputs leaves the span
        // DEFAULT-CONSTRUCTED in its row below rather than pointing at an empty array, because a
        // zero-length C array is a GNU extension clang takes and MSVC rejects (C2466) and that has reached
        // `dev` twice in one day. `std::span`'s default is empty and portable.

        constexpr std::array<RigPin, 1> kInTransform = { { { "Transform", RigValueKind::Transform } } };

        constexpr std::array<RigPin, 3> kInMake = { {
             { "Translation", RigValueKind::Vec3 },
             { "Rotation", RigValueKind::Quat },
             { "Scale", RigValueKind::Vec3 },
        } };

        constexpr std::array<RigPin, 2> kInTwoTransforms = { {
             { "A", RigValueKind::Transform },
             { "B", RigValueKind::Transform },
        } };

        constexpr std::array<RigPin, 3> kInBlend = { {
             { "A", RigValueKind::Transform },
             { "B", RigValueKind::Transform },
             { "Alpha", RigValueKind::Float },
        } };

        constexpr std::array<RigPin, 5> kInRemap = { {
             { "Value", RigValueKind::Float },
             { "InMin", RigValueKind::Float },
             { "InMax", RigValueKind::Float },
             { "OutMin", RigValueKind::Float },
             { "OutMax", RigValueKind::Float },
        } };

        constexpr std::array<RigPin, 1> kOutTransform = { { { "Transform", RigValueKind::Transform } } };

        constexpr std::array<RigPin, 3> kOutBreak = { {
             { "Translation", RigValueKind::Vec3 },
             { "Rotation", RigValueKind::Quat },
             { "Scale", RigValueKind::Vec3 },
        } };

        constexpr std::array<RigPin, 1> kOutFloat = { { { "Value", RigValueKind::Float } } };

        // ---- THE table -------------------------------------------------------------------------------
        //
        // Row i describes kind i, and the two static_asserts below are what make that sentence checkable
        // rather than a comment. The second derives the expected size from the LAST enumerator instead of
        // repeating a literal 10: a gate that pins a number can be satisfied by editing the number.
        constexpr std::array<RigNodeDescriptor, 10> kDescriptors = { {
             { RigNodeKind::GetControl, "GetControl", {}, kOutTransform, RigNodeTargetKind::Control, true },
             { RigNodeKind::GetBone, "GetBone", {}, kOutTransform, RigNodeTargetKind::Bone, false },
             { RigNodeKind::MakeTransform, "MakeTransform", kInMake, kOutTransform, RigNodeTargetKind::None,
               false },
             { RigNodeKind::BreakTransform, "BreakTransform", kInTransform, kOutBreak, RigNodeTargetKind::None,
               false },
             { RigNodeKind::MultiplyTransform, "MultiplyTransform", kInTwoTransforms, kOutTransform,
               RigNodeTargetKind::None, false },
             { RigNodeKind::InvertTransform, "InvertTransform", kInTransform, kOutTransform,
               RigNodeTargetKind::None, false },
             { RigNodeKind::BlendTransform, "BlendTransform", kInBlend, kOutTransform, RigNodeTargetKind::None,
               false },
             { RigNodeKind::Distance, "Distance", kInTwoTransforms, kOutFloat, RigNodeTargetKind::None, false },
             { RigNodeKind::RemapFloat, "RemapFloat", kInRemap, kOutFloat, RigNodeTargetKind::None, false },
             { RigNodeKind::SetControl, "SetControl", kInTransform, {}, RigNodeTargetKind::Control, true },
        } };

        constexpr bool RowsAreInKindOrder()
        {
            for ( size_t i = 0; i < kDescriptors.size(); ++i )
            {
                if ( static_cast<size_t>( kDescriptors[i].Kind ) != i )
                {
                    return false;
                }
            }
            return true;
        }

        static_assert( RowsAreInKindOrder(),
                       "row i of kDescriptors must describe kind i; DescribeRigNode indexes by the enum and "
                       "a mis-ordered row would describe the wrong node with no symptom but wrong arithmetic" );

        static_assert( kDescriptors.size() == static_cast<size_t>( RigNodeKind::SetControl ) + 1U,
                       "every RigNodeKind needs a row: a kind without one is a node the walk can hold and "
                       "neither the format nor the validator can describe" );

        [[nodiscard]] bool IsFinite( const RigValue& value )
        {
            switch ( static_cast<RigValueKind>( value.index() ) )
            {
                case RigValueKind::Float:
                    return std::isfinite( std::get<float>( value ) );
                case RigValueKind::Vec3:
                {
                    const glm::vec3& v = std::get<glm::vec3>( value );
                    return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
                }
                case RigValueKind::Quat:
                {
                    const glm::quat& q = std::get<glm::quat>( value );
                    return std::isfinite( q.w ) && std::isfinite( q.x ) && std::isfinite( q.y ) &&
                           std::isfinite( q.z );
                }
                case RigValueKind::Transform:
                {
                    const BoneTransform& t = std::get<BoneTransform>( value );
                    const RigValue       translation{ t.Translation };
                    const RigValue       rotation{ t.Rotation };
                    const RigValue       scale{ t.Scale };
                    return IsFinite( translation ) && IsFinite( rotation ) && IsFinite( scale );
                }
            }
            return false;
        }

        /// A slot that has the pin's type and an obviously-unwritten value. The walk cannot read a slot
        /// before it is written — links point strictly backwards — so this is not load-bearing for
        /// correctness; it is here so a debugger shows the right alternative instead of whichever one the
        /// variant defaulted to.
        [[nodiscard]] RigValue ZeroOf( RigValueKind kind )
        {
            switch ( kind )
            {
                case RigValueKind::Float:
                    return RigValue{ 0.0F };
                case RigValueKind::Vec3:
                    return RigValue{ glm::vec3( 0.0F ) };
                case RigValueKind::Quat:
                    return RigValue{ glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) };
                case RigValueKind::Transform:
                    return RigValue{ BoneTransform{} };
            }
            return RigValue{ 0.0F };
        }
    } // namespace

    std::string_view ToString( RigValueKind kind )
    {
        switch ( kind )
        {
            case RigValueKind::Float:
                return "Float";
            case RigValueKind::Vec3:
                return "Vec3";
            case RigValueKind::Quat:
                return "Quat";
            case RigValueKind::Transform:
                return "Transform";
        }
        return "?";
    }

    std::string_view ToString( RigControlSpace space )
    {
        return space == RigControlSpace::Local ? "Local" : "Global";
    }

    std::span<const RigNodeDescriptor> RigNodeDescriptors()
    {
        return kDescriptors;
    }

    const RigNodeDescriptor& DescribeRigNode( RigNodeKind kind )
    {
        return kDescriptors[static_cast<size_t>( kind )];
    }

    std::optional<RigNodeKind> RigNodeKindFromText( std::string_view text )
    {
        for ( const RigNodeDescriptor& row : kDescriptors )
        {
            if ( row.Name == text )
            {
                return row.Kind;
            }
        }
        return std::nullopt;
    }

    Common::BoolResultStr RigGraph::SetNodes( const ControlHierarchy& hierarchy, size_t boneCount,
                                              std::vector<RigNode> nodes )
    {
        if ( nodes.empty() )
        {
            return Common::MakeFormattedError<bool>(
                 "a rig graph with no nodes is not an empty graph, it is a graph that was meant to say "
                 "something; a rig with no forwards solve is spelled by not giving it one at all" );
        }

        std::unordered_map<std::string, size_t> byName;
        byName.reserve( nodes.size() );

        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            const RigNode& node = nodes[i];

            if ( node.Name.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "node {} has an empty name; the name is what a link, a refusal and an editor row all "
                     "bind on",
                     i );
            }
            if ( !byName.emplace( node.Name, i ).second )
            {
                return Common::MakeFormattedError<bool>(
                     "two nodes are named '{}'; a link naming it would have two answers", node.Name );
            }

            const RigNodeDescriptor& desc = DescribeRigNode( node.Kind );

            switch ( desc.Target )
            {
                case RigNodeTargetKind::Control:
                    if ( node.Target >= hierarchy.Size() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "node '{}' ({}) names control {}, and this rig has {}; an index that far out is "
                             "a graph written against a different rig, not a clamp to apply",
                             node.Name, desc.Name, node.Target, hierarchy.Size() );
                    }
                    break;
                case RigNodeTargetKind::Bone:
                    if ( node.Target >= boneCount )
                    {
                        return Common::MakeFormattedError<bool>(
                             "node '{}' ({}) reads bone {}, and this skeleton has {}; the graph was written "
                             "against a different skeleton",
                             node.Name, desc.Name, node.Target, boneCount );
                    }
                    break;
                case RigNodeTargetKind::None:
                    if ( node.Target != ControlHierarchy::INVALID )
                    {
                        return Common::MakeFormattedError<bool>(
                             "node '{}' is a {}, which reads and writes nothing named, yet it carries target "
                             "{}; a field that says something the kind cannot mean is a statement whose loss "
                             "is invisible",
                             node.Name, desc.Name, node.Target );
                    }
                    break;
            }

            if ( !desc.UsesSpace && node.Space != RigControlSpace::Global )
            {
                return Common::MakeFormattedError<bool>(
                     "node '{}' is a {}, which has no control space, yet it asks for {}", node.Name, desc.Name,
                     ToString( node.Space ) );
            }

            if ( node.Inputs.size() != desc.Inputs.size() )
            {
                return Common::MakeFormattedError<bool>( "node '{}' ({}) has {} inputs and the kind takes {}",
                                                         node.Name, desc.Name, node.Inputs.size(),
                                                         desc.Inputs.size() );
            }

            for ( size_t pin = 0; pin < node.Inputs.size(); ++pin )
            {
                const RigNodeInput& input    = node.Inputs[pin];
                const RigValueKind  expected = desc.Inputs[pin].Type;

                if ( input.Node == RigNodeInput::LITERAL )
                {
                    const RigValueKind actual = static_cast<RigValueKind>( input.Literal.index() );
                    if ( actual != expected )
                    {
                        return Common::MakeFormattedError<bool>(
                             "node '{}' ({}) has a {} literal on its '{}' pin, which is a {}", node.Name,
                             desc.Name, ToString( actual ), desc.Inputs[pin].Name, ToString( expected ) );
                    }
                    if ( !IsFinite( input.Literal ) )
                    {
                        return Common::MakeFormattedError<bool>(
                             "node '{}' ({}) has a non-finite literal on its '{}' pin; it would reach a bone "
                             "as a matrix that poisons every skinning weight it touches",
                             node.Name, desc.Name, desc.Inputs[pin].Name );
                    }
                    continue;
                }

                // THE ONE RULE THAT REPLACES CYCLE DETECTION. A link may only name a node that has already
                // run, so a cycle cannot be expressed at all — there is no colouring walk here and no depth
                // guard, because there is nothing for them to find.
                if ( input.Node >= i )
                {
                    return Common::MakeFormattedError<bool>(
                         "node '{}' ({}) takes its '{}' pin from node {}, which is itself or comes later. A "
                         "graph runs in the order it is written, because the hierarchy it reads and writes is "
                         "state no sort can see; a link therefore points backwards or not at all",
                         node.Name, desc.Name, desc.Inputs[pin].Name, input.Node );
                }

                const RigNodeDescriptor& producer = DescribeRigNode( nodes[input.Node].Kind );
                if ( input.Pin >= producer.Outputs.size() )
                {
                    return Common::MakeFormattedError<bool>(
                         "node '{}' takes its '{}' pin from output {} of '{}' ({}), which has {}", node.Name,
                         desc.Inputs[pin].Name, input.Pin, nodes[input.Node].Name, producer.Name,
                         producer.Outputs.size() );
                }

                const RigValueKind actual = producer.Outputs[input.Pin].Type;
                if ( actual != expected )
                {
                    return Common::MakeFormattedError<bool>(
                         "node '{}' ({}) wires '{}' of '{}', a {}, into its '{}' pin, which is a {}",
                         node.Name, desc.Name, producer.Outputs[input.Pin].Name, nodes[input.Node].Name,
                         ToString( actual ), desc.Inputs[pin].Name, ToString( expected ) );
                }
            }
        }

        // ---- THE TWO REFUSALS THAT KEEP A SILENT GRAPH OUT (see the header) ---------------------------
        //
        // A sink is a node with no outputs, so "this graph changes nothing" and "this graph has no sink"
        // are the same sentence, and liveness is one backwards pass because links already point backwards.
        std::vector<uint8_t> live( nodes.size(), 0 );
        bool                 anySink = false;

        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            if ( DescribeRigNode( nodes[i].Kind ).IsSink() )
            {
                live[i] = 1;
                anySink = true;
            }
        }

        if ( !anySink )
        {
            return Common::MakeFormattedError<bool>(
                 "this graph writes nothing: none of its {} nodes is a sink, so it computes values and "
                 "discards them. A forwards solve that cannot change a control is a stage that passes every "
                 "assertion a working one passes",
                 nodes.size() );
        }

        for ( size_t i = nodes.size(); i-- > 0; )
        {
            if ( live[i] == 0 )
            {
                continue;
            }
            for ( const RigNodeInput& input : nodes[i].Inputs )
            {
                if ( input.Node != RigNodeInput::LITERAL )
                {
                    live[input.Node] = 1;
                }
            }
        }

        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            if ( live[i] == 0 )
            {
                return Common::MakeFormattedError<bool>(
                     "node '{}' ({}) feeds no sink: its result is computed and thrown away. The honest "
                     "reading of that is a pin wired to the wrong place, which is why it is refused rather "
                     "than skipped",
                     nodes[i].Name, DescribeRigNode( nodes[i].Kind ).Name );
            }
        }

        // ---- storage, derived from the table so a count and its slots cannot disagree -----------------
        m_OutputBase.assign( nodes.size(), 0 );
        m_Slots.clear();
        m_Written.clear();

        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            const RigNodeDescriptor& desc = DescribeRigNode( nodes[i].Kind );
            m_OutputBase[i]               = static_cast<uint32_t>( m_Slots.size() );
            for ( const RigPin& pin : desc.Outputs )
            {
                m_Slots.push_back( ZeroOf( pin.Type ) );
            }
            if ( desc.IsSink() )
            {
                m_Written.push_back( nodes[i].Target );
            }
        }

        std::sort( m_Written.begin(), m_Written.end() );
        m_Written.erase( std::unique( m_Written.begin(), m_Written.end() ), m_Written.end() );

        m_Nodes = std::move( nodes );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RigGraph::Execute( ControlHierarchy& hierarchy, ComponentPose& component )
    {
        const auto read = [this]( const RigNodeInput& input ) -> const RigValue&
        {
            if ( input.Node == RigNodeInput::LITERAL )
            {
                return input.Literal;
            }
            return m_Slots[m_OutputBase[input.Node] + input.Pin];
        };

        for ( size_t i = 0; i < m_Nodes.size(); ++i )
        {
            const RigNode& node = m_Nodes[i];
            const uint32_t base = m_OutputBase[i];

            // Every `std::get` below is safe because `SetNodes` proved the pin types; a wrong alternative
            // here would be a defect in that proof, not in a file.
            switch ( node.Kind )
            {
                case RigNodeKind::GetControl:
                {
                    if ( node.Space == RigControlSpace::Local )
                    {
                        m_Slots[base] = RigValue{ hierarchy.Get( node.Target ).Pose };
                        break;
                    }
                    auto decomposed = BoneTransform::FromMatrix( hierarchy.GetGlobalTransform( node.Target ) );
                    if ( !decomposed.IsSuccess() )
                    {
                        return Common::MakeFormattedError<bool>( "node '{}' reading control '{}': {}",
                                                                 node.Name,
                                                                 hierarchy.Get( node.Target ).Name,
                                                                 decomposed.GetError() );
                    }
                    m_Slots[base] = RigValue{ decomposed.GetValue() };
                    break;
                }
                case RigNodeKind::GetBone:
                {
                    auto decomposed = BoneTransform::FromMatrix( component.Get( node.Target ) );
                    if ( !decomposed.IsSuccess() )
                    {
                        return Common::MakeFormattedError<bool>( "node '{}' reading bone {}: {}", node.Name,
                                                                 node.Target, decomposed.GetError() );
                    }
                    m_Slots[base] = RigValue{ decomposed.GetValue() };
                    break;
                }
                case RigNodeKind::MakeTransform:
                {
                    BoneTransform made;
                    made.Translation = std::get<glm::vec3>( read( node.Inputs[0] ) );
                    made.Rotation    = glm::normalize( std::get<glm::quat>( read( node.Inputs[1] ) ) );
                    made.Scale       = std::get<glm::vec3>( read( node.Inputs[2] ) );
                    m_Slots[base]    = RigValue{ made };
                    break;
                }
                case RigNodeKind::BreakTransform:
                {
                    const BoneTransform& t = std::get<BoneTransform>( read( node.Inputs[0] ) );
                    m_Slots[base]          = RigValue{ t.Translation };
                    m_Slots[base + 1]      = RigValue{ t.Rotation };
                    m_Slots[base + 2]      = RigValue{ t.Scale };
                    break;
                }
                case RigNodeKind::MultiplyTransform:
                {
                    const BoneTransform& lhs = std::get<BoneTransform>( read( node.Inputs[0] ) );
                    const BoneTransform& rhs = std::get<BoneTransform>( read( node.Inputs[1] ) );
                    auto decomposed = BoneTransform::FromMatrix( lhs.ToMatrix() * rhs.ToMatrix() );
                    if ( !decomposed.IsSuccess() )
                    {
                        return Common::MakeFormattedError<bool>( "node '{}' multiplying two transforms: {}",
                                                                 node.Name, decomposed.GetError() );
                    }
                    m_Slots[base] = RigValue{ decomposed.GetValue() };
                    break;
                }
                case RigNodeKind::InvertTransform:
                {
                    const BoneTransform& t          = std::get<BoneTransform>( read( node.Inputs[0] ) );
                    auto                 decomposed = BoneTransform::FromMatrix( glm::inverse( t.ToMatrix() ) );
                    if ( !decomposed.IsSuccess() )
                    {
                        return Common::MakeFormattedError<bool>( "node '{}' inverting a transform: {}",
                                                                 node.Name, decomposed.GetError() );
                    }
                    m_Slots[base] = RigValue{ decomposed.GetValue() };
                    break;
                }
                case RigNodeKind::BlendTransform:
                {
                    const BoneTransform& from = std::get<BoneTransform>( read( node.Inputs[0] ) );
                    const BoneTransform& to   = std::get<BoneTransform>( read( node.Inputs[1] ) );
                    // CLAMPED, and `Blend` deliberately does not clamp for its own callers. An alpha of 1.4
                    // slerps PAST the target, which on a limb is the joint turning inside out — and nothing
                    // authored a request for it: the alpha here comes from a literal or from RemapFloat,
                    // and the latter is already inside its output range.
                    const float alpha = std::clamp( std::get<float>( read( node.Inputs[2] ) ), 0.0F, 1.0F );
                    m_Slots[base]     = RigValue{ Blend( from, to, alpha ) };
                    break;
                }
                case RigNodeKind::Distance:
                {
                    const BoneTransform& lhs = std::get<BoneTransform>( read( node.Inputs[0] ) );
                    const BoneTransform& rhs = std::get<BoneTransform>( read( node.Inputs[1] ) );
                    m_Slots[base] = RigValue{ glm::distance( lhs.Translation, rhs.Translation ) };
                    break;
                }
                case RigNodeKind::RemapFloat:
                {
                    const float value  = std::get<float>( read( node.Inputs[0] ) );
                    const float inMin  = std::get<float>( read( node.Inputs[1] ) );
                    const float inMax  = std::get<float>( read( node.Inputs[2] ) );
                    const float outMin = std::get<float>( read( node.Inputs[3] ) );
                    const float outMax = std::get<float>( read( node.Inputs[4] ) );

                    if ( inMin == inMax )
                    {
                        // REFUSED RATHER THAN ANSWERED. An empty input range has no mapping; returning
                        // outMin, or the midpoint, or zero would each be a number this file invented.
                        return Common::MakeFormattedError<bool>(
                             "node '{}' remaps from an empty range [{}, {}]; there is no value to map {} to",
                             node.Name, inMin, inMax, value );
                    }

                    const float t = std::clamp( ( value - inMin ) / ( inMax - inMin ), 0.0F, 1.0F );
                    m_Slots[base] = RigValue{ outMin + ( t * ( outMax - outMin ) ) };
                    break;
                }
                case RigNodeKind::SetControl:
                {
                    const BoneTransform& value = std::get<BoneTransform>( read( node.Inputs[0] ) );
                    auto                 written =
                         node.Space == RigControlSpace::Local
                              ? hierarchy.SetPose( node.Target, value )
                              : hierarchy.SetGlobalTransform( node.Target, value.ToMatrix() );
                    if ( !written.IsSuccess() )
                    {
                        return Common::MakeFormattedError<bool>( "node '{}' writing control '{}' ({}): {}",
                                                                 node.Name,
                                                                 hierarchy.Get( node.Target ).Name,
                                                                 ToString( node.Space ), written.GetError() );
                    }
                    break;
                }
            }
        }

        return Common::MakeSuccess( true );
    }
} // namespace Desert::Animation
