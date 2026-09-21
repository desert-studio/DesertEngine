#pragma once

/**
 * THE RIG GRAPH — T5.5, AND IT IS A WALK, NOT A VIRTUAL MACHINE.
 *
 * T5.4 put the rig in the pipeline: pose in, hierarchy, solve, hierarchy, pose out. The solve was a hole,
 * marked by a comment, and what filled it was identity — a control's global BECAME its bone's transform,
 * which is a rig whose forwards event is one "set transform" per control. This file is the arithmetic that
 * goes in that hole: nodes that read the hierarchy and the pose, compute, and write control poses back.
 *
 * ── WHY NOT A VM (R7, report 01 §(b)1) ──────────────────────────────────────────────────────────────
 *
 * `ERigVMOpCode` carries 65 dead opcodes plus 16 dead array opcodes, kept forever for asset compatibility,
 * and four memory classes with handles, slices, traits and lazy branches to feed them. The analysis' answer
 * is one sentence: "a direct-execution graph walk over the same node structs gives the same semantics with
 * none of the memory-handle machinery". That is this class. There is no instruction stream, no operand
 * stack and no bytecode: `Execute` is a `for` over the nodes with a `switch` on the kind.
 *
 * ── NODES ARE A TABLE AND A SWITCH, NOT A CLASS HIERARCHY ───────────────────────────────────────────
 *
 * R7 says "without the VM, a node is just a virtual `Execute(Context&)`", and that would have been the
 * obvious shape. It is refused here for three reasons this tree can name:
 *
 *   - a node with a virtual Execute is a heap object, so a graph becomes `vector<unique_ptr<RigNode>>` and
 *     owes `PointerOwnership` an argument; a graph of plain structs is copyable, comparable and has no
 *     lifetime story at all;
 *   - `PureVirtualCensus` exists because "the correct virtual pair" gets written and half-wired. Ten node
 *     kinds would have needed pin names, pin types and pin counts as virtuals — thirty-odd overrides
 *     answering questions that are constant per kind, i.e. data wearing a function;
 *   - the pin table has to be readable by the FILE layer as well, to refuse a `.derig` that wires a Float
 *     into a Transform. A virtual can only answer for an object that already exists, so the format would
 *     have needed a second copy of the same facts — the "two ends of a chain, one link loses something"
 *     shape this project keeps closing.
 *
 * So the truth about a kind lives in exactly one `constexpr` row (`RigNodeDescriptors()`), the count is
 * DERIVED from the table rather than written down beside it, and `static_assert`s pin row i to kind i.
 *
 * ── EXECUTION ORDER IS THE FILE'S ORDER, AND THAT IS A DECISION ─────────────────────────────────────
 *
 * The obvious alternative is a topological sort over the links. It is wrong here, and the reason is report
 * 01 §6.4(3): the hierarchy is state that is "unobservable to the compiler, shared by every node, and the
 * reason node ordering is the author's responsibility". A `GetControl` after a `SetControl` on the same
 * control reads the written value; before it, the old one. Those two graphs have IDENTICAL link sets, so a
 * sort over the links is free to swap them — silently, and differently on a different day.
 *
 * Therefore: nodes run in the order they are declared, and **a link may only name an EARLIER node**. A
 * forward or self link is refused by name. Two consequences fall out for free, and both are load-bearing:
 * a cycle is unreachable by construction (no colouring walk, no depth guard), and the order a reader sees
 * in the file is the order that runs.
 *
 * ── THE STATE THAT MUST BE UNREACHABLE ──────────────────────────────────────────────────────────────
 *
 * T5.4's was "a rig with no drives": a stage that cannot change the pose passes every assertion a working
 * one passes. A graph has the same failure mode three times over, and all three are refused:
 *
 *   1. A GRAPH WITH NO SINK. A sink is a node with no outputs — `SetControl` is the only one today — and a
 *      graph of nothing but readers and arithmetic computes values it then discards. `SetNodes` refuses.
 *   2. A DEAD NODE. Any node that does not feed a sink, transitively, is work whose result is discarded.
 *      Refused by name, because the honest reading of one is "the author wired it to the wrong pin".
 *   3. A GRAPH WHOSE WRITES CANNOT REACH A DRIVEN BONE. That check needs the drive list, so it lives in
 *      `ControlRigStage::SetGraph`, not here — but it is the same refusal, and it is the one that makes
 *      "the graph ran" and "the pose changed" the same statement.
 *
 * ── NO HIDDEN PER-NODE STATE ────────────────────────────────────────────────────────────────────────
 *
 * Report 01 §6.4(1) describes UE's: a reflected member with neither Input nor Output, living in work
 * memory, persisting across evaluations — `FCachedRigElement` on nearly every hierarchy node. We have no
 * such thing and will not grow one by accident: a node's whole state is its output slots, those are
 * overwritten every evaluation, and `Execute` takes the hierarchy and the pose as arguments rather than
 * holding them. The cross-cutting decision list says it in one line (§5.4.6): "nodes read a snapshot,
 * never the world; per-frame scratch belongs to the pass".
 */

#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief What flows along a link.
     *
     * A TAGGED VARIANT, and R7 names what it replaces: `FRigControlValue` is 33 type-punned floats, ~132
     * bytes to hold a bool, and every read of it is a `reinterpret_cast`. `std::variant` is the same idea
     * with the tag the compiler already maintains, and a wrong-type read is a refusal rather than garbage.
     *
     * The alternatives are kept apart from `RigValueKind` below by `static_assert`, so a type added to one
     * and not the other does not compile.
     */
    using RigValue = std::variant<float, glm::vec3, glm::quat, BoneTransform>;

    /// The same four, as the thing a pin declares. Values are the variant's alternative indices.
    enum class RigValueKind : uint8_t
    {
        Float     = 0,
        Vec3      = 1,
        Quat      = 2,
        Transform = 3,
    };

    static_assert( std::variant_size_v<RigValue> == 4,
                   "RigValueKind names every alternative of RigValue; a type in one and not the other is a "
                   "pin the format can declare and the walk cannot carry" );
    static_assert( std::is_same_v<std::variant_alternative_t<0, RigValue>, float> );
    static_assert( std::is_same_v<std::variant_alternative_t<1, RigValue>, glm::vec3> );
    static_assert( std::is_same_v<std::variant_alternative_t<2, RigValue>, glm::quat> );
    static_assert( std::is_same_v<std::variant_alternative_t<3, RigValue>, BoneTransform> );

    [[nodiscard]] std::string_view ToString( RigValueKind kind );

    /**
     * @brief Which of a control's two transforms a node reads or writes.
     *
     * `Local` is `ControlElement::Pose` — the ANIMATED value, what a clip or a Sequencer key writes.
     * `Global` is the control in component space, which on write goes through
     * `ControlHierarchy::SetGlobalTransform` and is back-solved to a local (the hierarchy stores no global;
     * see its file note). Both are needed and neither is the other: "put the hand THERE" is global, "turn
     * the wrist by this much" is local.
     */
    enum class RigControlSpace : uint8_t
    {
        Local,
        Global,
    };

    [[nodiscard]] std::string_view ToString( RigControlSpace space );

    /// What a node's `Target` names, if anything. Data, so the format can refuse `"GetBone"` naming a
    /// control without a second table of its own.
    enum class RigNodeTargetKind : uint8_t
    {
        None,
        Control,
        Bone,
    };

    /**
     * @brief The node kinds, and the whole set is ten.
     *
     * Two readers, five transform operations, two scalar operations and one writer. The scalar pair is not
     * decoration: without `Distance` and `RemapFloat` a `Float` could only ever be a literal, so
     * `BlendTransform`'s alpha would be a constant and the graph could express arithmetic but not DRIVEN
     * arithmetic — which is the difference between an expression and a rig.
     */
    enum class RigNodeKind : uint8_t
    {
        GetControl = 0,
        GetBone,
        MakeTransform,
        BreakTransform,
        MultiplyTransform,
        InvertTransform,
        BlendTransform,
        Distance,
        RemapFloat,
        SetControl,
    };

    struct RigPin
    {
        std::string_view Name;
        RigValueKind     Type = RigValueKind::Float;
    };

    /**
     * @brief Everything constant about a kind, in one row.
     *
     * `Outputs.empty()` IS the definition of a sink — there is no separate `bool Writes`, because two
     * spellings of one fact is how they drift apart. A node that produces nothing exists only for what it
     * does to the hierarchy, and a node that produces something is pure.
     */
    struct RigNodeDescriptor
    {
        RigNodeKind             Kind = RigNodeKind::GetControl;
        std::string_view        Name;
        std::span<const RigPin> Inputs;
        std::span<const RigPin> Outputs;
        RigNodeTargetKind       Target    = RigNodeTargetKind::None;
        bool                    UsesSpace = false;

        [[nodiscard]] bool IsSink() const
        {
            return Outputs.empty();
        }
    };

    /// The one table. The count is read off it (`.size()`), never written beside it — a gate that pins a
    /// NUMBER can be satisfied by editing the number.
    [[nodiscard]] std::span<const RigNodeDescriptor> RigNodeDescriptors();

    /// Total over the enum: every kind has a row, pinned by `static_assert` in the .cpp.
    [[nodiscard]] const RigNodeDescriptor& DescribeRigNode( RigNodeKind kind );

    /// The file's spelling -> the kind. `std::nullopt` for a word this build does not know, which the
    /// format refuses by name rather than taking as the first row.
    [[nodiscard]] std::optional<RigNodeKind> RigNodeKindFromText( std::string_view text );

    /**
     * @brief One input pin, wired to an earlier node's output OR carrying a literal.
     *
     * A LITERAL IS NOT A NODE. The alternative — a `Constant` node kind per type — doubles the node count
     * of every real graph, and makes "this pin is 0.5" a thing with a name, a row in a file and a place in
     * the execution order. Here it is a value on the pin, which is also what a node-graph editor shows.
     */
    struct RigNodeInput
    {
        static constexpr uint32_t LITERAL = UINT32_MAX;

        /// The producing node's index, or `LITERAL`. Must be strictly less than this node's own index.
        uint32_t Node = LITERAL;
        uint8_t  Pin  = 0;
        RigValue Literal{ 0.0F };
    };

    /// One node. A plain struct: no heap, no virtuals, no per-instance state beyond its wiring.
    struct RigNode
    {
        std::string               Name;
        RigNodeKind               Kind   = RigNodeKind::GetControl;
        uint32_t                  Target = ControlHierarchy::INVALID;
        RigControlSpace           Space  = RigControlSpace::Global;
        std::vector<RigNodeInput> Inputs;
    };

    /**
     * @brief The rig's forwards solve: nodes, in order, run over a hierarchy and a pose.
     *
     * Built once (`SetNodes`, which refuses everything structural), then executed per frame. An empty graph
     * is legal and means "no graph": `ControlRigStage` then behaves exactly as it did before this file
     * existed, which is the positive control the suite compares bytes against.
     */
    class RigGraph
    {
    public:
        /**
         * @brief Install the nodes, or say which one is wrong. THE ONLY WAY TO POPULATE A GRAPH.
         *
         * Refuses: an empty name, a duplicate name; a target that is required and missing, or present and
         * meaningless for the kind; a `Space` on a kind that has none; the wrong number of inputs; a link
         * to a node index that is not strictly smaller (which is also how forward links, self links and
         * every cycle are refused, at once); a link to a pin the producing kind does not have; a link or a
         * literal whose TYPE is not the pin's; a graph with no sink; and any node that does not feed one.
         *
         * @param boneCount the skeleton the `GetBone` nodes are being resolved against. Taken as a count
         *        rather than a `Skeleton&` because that is all this refusal needs, and a graph that holds a
         *        skeleton reference is a graph with a lifetime story.
         */
        [[nodiscard]] Common::BoolResultStr SetNodes( const ControlHierarchy& hierarchy, size_t boneCount,
                                                      std::vector<RigNode> nodes );

        [[nodiscard]] const std::vector<RigNode>& GetNodes() const
        {
            return m_Nodes;
        }

        [[nodiscard]] bool Empty() const
        {
            return m_Nodes.empty();
        }

        /// The controls this graph writes, ascending and unique. What `ControlRigStage::SetGraph` needs to
        /// answer "can anything this graph does reach a driven bone".
        [[nodiscard]] const std::vector<uint32_t>& WrittenControls() const
        {
            return m_Written;
        }

        /**
         * @brief Run the walk. Every node once, in order.
         *
         * @param component the bone spaces, already pointed at this frame's pose — `GetBone` reads it and
         *        it resolves lazily, so a graph that names two bones converts two.
         *
         * Refuses, naming the node, on the only three arithmetic failures that exist here: a transform that
         * cannot be inverted, a remap whose input range is empty, and a write the hierarchy itself rejects.
         * Allocation-free after the first call: the output slots are a member.
         */
        [[nodiscard]] Common::BoolResultStr Execute( ControlHierarchy& hierarchy, ComponentPose& component );

    private:
        std::vector<RigNode> m_Nodes;

        /// Index into `m_Slots` of node i's first output. Derived in `SetNodes` from the descriptors, so an
        /// output count and its storage cannot disagree.
        std::vector<uint32_t> m_OutputBase;

        /// One entry per output pin of every node, reused between frames.
        std::vector<RigValue> m_Slots;

        std::vector<uint32_t> m_Written;
    };
} // namespace Desert::Animation
