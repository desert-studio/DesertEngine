#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── THE IDENTITY LAYER SHARED BY EVERY NODE CANVAS IN THE EDITOR ──────────────────────────────────────
//
// `imgui-node-editor` remembers where a node sits under the id the panel drew it with. That id is
// therefore the ONLY thing tying "where the user dragged this" to "which element it was", and both of
// the editor's canvases used to answer it with a POSITION IN A VECTOR:
//
//     AnimGraphPanel.cpp  NodeId( i )            = i + 1
//     AnimGraphPanel.cpp  LinkId( state, trans ) = kLink + state * 4096 + trans
//
// Delete state 2 of 5 and every later state shifts down one index, so on the next frame state i is drawn
// under the id that belonged to state i+1: `ed::GetNodePosition` hands back its NEIGHBOUR's position and
// the panel writes that into `State.X/Y`. One deletion silently moves every state after it, and the file
// is then saved with the moved layout. `Desert/Tests/Editor/GraphCanvasIdentity` measures it.
//
// The fix is not better arithmetic. It is that identity must come from the ELEMENT and not from where it
// happens to sit in a vector, which is the same disease as "a `.dgraph` has no identity", one frame wide.
//
// TWO SOURCES OF IDS, ONE LEDGER, AND THAT SPLIT IS DELIBERATE. A document that already issues stable
// ids — the shader graph's `Document::NextId` — is ALREADY RIGHT, and replacing its numbering would
// renumber every committed `.dgraph`'s links for no gain. So it keeps its ids and merely declares them
// here (`ElementLedger::See`). A document that has no id of its own — the anim graph, whose states are
// named by a string and nothing else — gets them issued (`ElementIdMap::Resolve`). `ElementIdMap` is
// built ON `ElementLedger`, so the freshness rule below is ONE implementation that both canvases run.
namespace Desert::Editor::Graph
{
    /// A canvas element's identity. NOT an index: see the header comment.
    enum class ElementId : uint64_t
    {
        Invalid = 0
    };

    [[nodiscard]] constexpr uint64_t Raw( ElementId id )
    {
        return static_cast<uint64_t>( id );
    }

    /// What an id names. Nodes, pins and links share ONE id space inside `imgui-node-editor`, so the
    /// three kinds are drawn from disjoint ranges and `KindOf` can answer from the number alone.
    enum class ElementKind : uint8_t
    {
        Node = 0,
        Pin  = 1,
        Link = 2,
    };

    inline constexpr uint64_t kKindSpan = 0x2000'0000ULL;
    inline constexpr uint64_t kNodeBase = 1ULL;                // 0 is Invalid, so the node range starts at 1
    inline constexpr uint64_t kPinBase  = kKindSpan;           // 0x2000'0000
    inline constexpr uint64_t kLinkBase = kKindSpan * 2ULL;    // 0x4000'0000
    inline constexpr uint64_t kEndOfIds = kKindSpan * 3ULL;    // one past the last id any kind may take

    /// The range an id belongs to. An id outside every range answers `Node`, which is what the shader
    /// graph's own small ids are — it is a RANGE question and not a claim about a particular document.
    [[nodiscard]] ElementKind KindOf( ElementId id );

    /// The base of a kind's range. Named rather than spelled at each call site because the three
    /// constants above and the arithmetic that uses them are the one thing that must not drift.
    [[nodiscard]] uint64_t BaseOf( ElementKind kind );

    // ── THE FRESHNESS LEDGER ──────────────────────────────────────────────────────────────────────────
    //
    // "Does the canvas already know where this element is?" — the question both canvases answered with a
    // single document-wide `m_ApplyPositions` bool, which is true exactly once per window and therefore
    // cannot answer it for an element created afterwards. The shader graph patched the hole with a second
    // code path (`ed::SetNodePosition` at the node-palette popup); the anim graph did not patch it at
    // all, so a state added after the first frame was placed wherever the canvas felt like and its real
    // `State.X/Y` were then overwritten from there.
    //
    // Per ELEMENT instead of per document: an id seen for the first time needs its stored position PUSHED
    // into the canvas, and any other id needs its position READ BACK out of it. One rule, no special case.
    class ElementLedger
    {
    public:
        /// Opens a frame. Every id the panel intends to draw must be passed to `See` before `EndFrame`.
        void BeginFrame();

        /// Declares that @p id is on the canvas this frame. Returns true the FIRST frame an id appears —
        /// which is the panel's instruction to push the model's stored position into the canvas instead
        /// of reading the canvas's idea of it back out.
        [[nodiscard]] bool See( ElementId id );

        /// Closes a frame: ids that were not seen are forgotten, because the element behind them is gone
        /// and an id that comes back later must come back as a fresh one.
        void EndFrame();

        /// How many ids the canvas is currently believed to know about.
        [[nodiscard]] size_t LiveCount() const;

        /// Whether @p id was seen in some earlier frame (i.e. `See` would answer false for it now).
        [[nodiscard]] bool IsKnown( ElementId id ) const;

    private:
        std::unordered_set<uint64_t> m_Known;
        std::unordered_set<uint64_t> m_SeenThisFrame;
    };

    /// What `ElementIdMap::Resolve` answers: the element's id, and whether the canvas has never seen it.
    struct Resolved
    {
        ElementId Id    = ElementId::Invalid;
        bool      Fresh = false;
    };

    // ── IDENTITY FOR A DOCUMENT THAT HAS NONE ─────────────────────────────────────────────────────────
    //
    // A key -> id table whose ids outlive nothing but the key. The KEY is the document's own name for the
    // element — for the anim graph, a state's `Name`, which is precisely the string `Entry` and
    // `Transition::To` already resolve against, so this introduces no second notion of "which state is
    // this" for the model to disagree with.
    class ElementIdMap
    {
    public:
        /// Opens a frame. Keys not resolved before `EndFrame` lose their ids.
        void BeginFrame();

        /// The id for @p key, allocating one the first time the key is seen.
        [[nodiscard]] Resolved Resolve( ElementKind kind, std::string_view key );

        /// Closes a frame, retiring the ids of keys that went unresolved.
        void EndFrame();

        /// The key an id was issued for, or nullptr. This is what lets a panel answer "the user deleted
        /// THIS node" without any index arithmetic at all.
        [[nodiscard]] const std::string* KeyOf( ElementId id ) const;

        /// The id currently issued for @p key, or `Invalid`. Does not allocate.
        [[nodiscard]] ElementId Lookup( ElementKind kind, std::string_view key ) const;

        [[nodiscard]] size_t LiveCount() const;

    private:
        /// The table key: kind and document key together, because a node and a link may honestly carry
        /// the same name and must not share an id.
        [[nodiscard]] static std::string TableKey( ElementKind kind, std::string_view key );

        ElementLedger                             m_Ledger;
        std::unordered_map<std::string, uint64_t> m_ByKey;  // kind+key -> id
        std::unordered_map<uint64_t, std::string> m_ById;   // id -> document key (no kind prefix)
        std::array<uint64_t, 3>                   m_Next{}; // next free offset within each kind's range
    };

    // ── WHAT A CANVAS IS ASKED TO DRAW, AS DATA ───────────────────────────────────────────────────────
    //
    // The plan is the seam that makes both canvases MEASURABLE without an ImGui context: every id, every
    // position and every link endpoint a frame would submit, in submission order. `Fingerprint` over it
    // is the anim graph's equivalent of `ShaderGraphDeterminism`'s FNV-1a over emitted DSL — a number
    // that moves the moment identity, order or the freshness rule changes for either graph.
    struct PlannedNode
    {
        ElementId   Id = ElementId::Invalid;
        std::string Key;                  // the document's own name for this element
        float       X            = 0.0f;  // the model's stored position
        float       Y            = 0.0f;
        bool        PushPosition = false; // fresh: tell the canvas where this is, do not ask it
    };

    struct PlannedLink
    {
        ElementId   Id = ElementId::Invalid;
        std::string Key;
        uint64_t    FromPin = 0;
        uint64_t    ToPin   = 0;
    };

    struct CanvasPlan
    {
        std::vector<PlannedNode> Nodes;
        std::vector<PlannedLink> Links;
    };

    // ── WHY FRAMING IS DEFERRED, AND WHY IT IS NOT A MAGIC NUMBER ─────────────────────────────────────
    //
    // A canvas cannot be navigated before it exists, and on its FIRST frame it may not exist.
    // `imgui-node-editor` initialises lazily inside the first `ed::Begin`: it runs a THROWAWAY
    // `Canvas::Begin`/`Canvas::End` cycle to give the canvas a size before loading settings
    // (imgui_node_editor.cpp:1136-1145), and `Canvas::End` finishes by emitting a dummy widget the size
    // of the canvas (imgui_canvas.cpp:188-190). The ImGui cursor therefore moves DOWN BY A WHOLE CANVAS,
    // and the real `Canvas::Begin` of that same first frame starts below it. `Canvas::Begin` refuses a
    // widget rect that does not overlap the enclosing window's clip rect (imgui_canvas.cpp:111-117) —
    // and a refused canvas draws nothing, `NavigateToContent` included.
    //
    // MEASURED ON THIS TREE, not reasoned about. With the anim graph's canvas inside a child window sized
    // exactly to it, the first Begin landed at y = 965 against a clip rect ending at y = 962 and was
    // refused; the shader graph's, drawn straight into the document window whose clip rect runs 7 px
    // further down, landed at the same y = 965 against 969 and survived BY FOUR PIXELS. The anim graph
    // then called `NavigateToContent` on a canvas that was not there, and — having no `Frame All` — never
    // got its view back: a docked Anim Graph drew an empty rectangle for its whole life.
    //
    // So framing waits for the second drawn frame, which is the first frame whose canvas is certainly
    // initialised. The number is 2 because the vendor's lazy init costs exactly one frame, and both
    // halves of that sentence are checked by `GraphCanvasIdentity`.
    class DeferredFrameAll
    {
    public:
        /// Ask for the content to be framed as soon as the canvas can be navigated. Called when a
        /// document opens and whenever what it shows is replaced wholesale.
        void Request();

        /// Call once per frame, AFTER `ed::End()`. True on the frame the content should be framed, and
        /// on no other. It returns a bool rather than navigating itself so that the rule can be tested
        /// where there is no `ed::EditorContext` to navigate — which is every build machine.
        [[nodiscard]] bool Tick();

    private:
        static constexpr int kFramesUntilCanvasExists = 2;

        int m_FramesLeft = kFramesUntilCanvasExists;
    };

    /// FNV-1a over everything a frame would submit. Positions are hashed as their exact bit pattern, so
    /// a layout that shifted by one pixel moves the number.
    [[nodiscard]] uint64_t Fingerprint( const CanvasPlan& plan );
} // namespace Desert::Editor::Graph
