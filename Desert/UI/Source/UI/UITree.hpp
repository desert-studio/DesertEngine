#pragma once

#include <UI/Args/ArgKind.hpp>
#include <UI/UICursor.hpp>
#include <UI/UIDragDrop.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

// THE TREE THE UI FRAMEWORK READS, AND THE ONLY THING IT KNOWS ABOUT WHO STORES IT.
//
// Layout, picking and introspection used to ask `entt::registry` directly — which component an element
// carries, who its children are — so the framework could not exist without the ECS and could not be fed a
// tree from anywhere else (a widget host, a test that builds nodes by hand, a tool). This is the seam UE draws
// between UMG (UObjects authored in a level) and Slate (a widget tree the framework walks): the framework is
// handed a read view, the engine adapts its storage to it (Engine/UI/Ecs/EcsUITree).
//
// It is a VIEW, not a retained widget graph. There is no invalidation here (plan §0.1): a walk asks the
// tree every frame, exactly as the ECS walk did, so a frame drawn through it is the frame drawn before.
namespace Desert::UI
{
    struct UIPaintArgs;

    // A node of the tree. Bit-for-bit an `entt::entity` in the ECS adapter (null mapped explicitly), so the
    // id a debugger prints for an element is the same id either side of the seam.
    enum class NodeId : std::uint32_t
    {
        Null = 0xFFFFFFFFu
    };

    class IUITree
    {
    public:
        virtual ~IUITree() = default;

        [[nodiscard]] virtual bool Valid( NodeId n ) const = 0;

        // An upper bound on the number of nodes. Bounds every ancestor walk: a Parent cycle is authorable
        // (a hierarchy panel can reparent), and an unbounded walk would hang instead of answering "none".
        [[nodiscard]] virtual std::size_t NodeBound() const = 0;

        [[nodiscard]] virtual NodeId Parent( NodeId n ) const = 0; // NodeId::Null for a root or an orphan

        // Children IN DRAW ORDER. Asked by index rather than handed out as a span: the ECS stores them as
        // `entt::entity`, and reading that storage through a NodeId would be an aliasing violation, not a
        // conversion. An invalid child may be listed — callers skip what Valid() refuses, as before.
        [[nodiscard]] virtual std::size_t ChildCount( NodeId n ) const             = 0;
        [[nodiscard]] virtual NodeId      ChildAt( NodeId n, std::size_t i ) const = 0;

        [[nodiscard]] virtual std::string_view Name( NodeId n ) const     = 0; // authored name, "" when none
        [[nodiscard]] virtual std::uint64_t    StableId( NodeId n ) const = 0; // survives save/load; 0 when none

        // The node's authored arguments of kind @p kind (the UI*Data whose `Arg` is @p kind), or nullptr.
        [[nodiscard]] virtual const void* Find( NodeId n, ArgKind kind ) const = 0;

        // The same arguments, writable — ONLY for the kinds a control writes its value back into (Toggle,
        // Slider, InputField, Dropdown, ScrollView, ListView). nullptr for every other kind, so a framework
        // walk cannot quietly edit layout or style through this door.
        [[nodiscard]] virtual void* FindState( NodeId n, ArgKind kind ) = 0;

        // Every node carrying @p kind (Canvas, Screen, Overlay …), in AUTHORED order — the order the scene
        // created them, which is the tie-break every "which comes first" rule above the tree relies on.
        virtual void Roots( ArgKind kind, std::vector<NodeId>& out ) const = 0;

        // WHICH STORAGE the ids belong to. Ids are unique only inside one store (entt recycles them per
        // registry), so a view that remembers per-node state must notice when it is handed another scene;
        // two trees over the same store answer the same address. Never dereferenced by the framework.
        [[nodiscard]] virtual const void* Storage() const = 0;

        // Where the HOST places @p n in its world — the origin a WorldSpace canvas is billboarded from (UE's
        // WidgetComponent is a scene component; the widget tree itself has no world). nullopt when the host
        // gives the node no placement.
        [[nodiscard]] virtual std::optional<glm::vec3> WorldOrigin( NodeId n ) const = 0;

        // The pointer shape @p n asks for while the pointer is over it or it holds the capture — a splitter's
        // resize arrows, a link's hand. nullopt = the node states none, and the framework's own answer for
        // its control kind stands (a text field's I-beam). Not pure: a store whose authored data carries no
        // cursor (the ECS scene today) truthfully states none for every node.
        [[nodiscard]] virtual std::optional<UICursorShape> CursorOf( NodeId n ) const
        {
            (void)n;
            return std::nullopt;
        }

        // Is @p n a pointer control by the tree's own declaration — one that a press on it (or on anything
        // inside it) belongs to, and that then HOLDS the pointer until the button is up (UIViewContext::
        // Capture)? A splitter's drag handle is: it must keep resizing when the pointer outruns it. The
        // authored control kinds answer for themselves (a slider captures, a button does not); this is for a
        // node whose behaviour is code rather than an authored kind. Not pure: false for every node of a
        // store that has none.
        [[nodiscard]] virtual bool CapturesPointer( NodeId n ) const
        {
            (void)n;
            return false;
        }

        // Is @p n a TEXT-INPUT node by the tree's own declaration — one whose code (its Paint) edits text, as
        // an authored InputField does? It is focusable, a press on it focuses it, its view's focus ring is
        // its own to draw, and while it holds focus the keys that would navigate are its text (W/S, the
        // pointer entering a button does not take focus away). Not pure: false for a store that has none.
        [[nodiscard]] virtual bool TakesTextInput( NodeId n ) const
        {
            (void)n;
            return false;
        }

        // SLATE'S OnDragDetected: the operation a drag started on @p n carries (UIDragDrop.hpp), or nullopt
        // when @p n is no drag source. Asked once, on the press, for the nearest node at or above the element
        // the press landed on; an authored UIDraggableData answers for itself before this is asked. Not pure:
        // a store whose sources are all authored states none.
        [[nodiscard]] virtual std::optional<UIDragPayload> DragSourceOf( NodeId n ) const
        {
            (void)n;
            return std::nullopt;
        }

        // SLATE'S OnDragOver: @p n's answer to @p payload — None when @p n is not a drop target at all, so the
        // question goes on to its parent. Asked every frame of a drag for the nodes the walk draws and the
        // chain under the cursor. An authored UIDropTargetData answers for itself before this is asked.
        [[nodiscard]] virtual UIDropAnswer DropAnswerOf( NodeId n, const UIDragPayload& payload ) const
        {
            (void)n;
            (void)payload;
            return UIDropAnswer::None;
        }

        // Draw @p n's own content (UIPaint.hpp, Slate's OnPaint) into the frame's draw list. Called by the walk
        // for every node with a rect, after its widgets and before its children. Default: nothing — a node's
        // look is its authored arguments, and only a tree whose nodes carry code (the retained tree) has more.
        // May edit the tree's arguments and its owner's state; must not create or destroy nodes.
        virtual void Paint( NodeId n, const UIPaintArgs& args )
        {
            (void)n;
            (void)args;
        }

        template <class T>
        [[nodiscard]] const T* Get( NodeId n ) const
        {
            return static_cast<const T*>( Find( n, T::Arg ) );
        }

        template <class T>
        [[nodiscard]] bool Has( NodeId n ) const
        {
            return Find( n, T::Arg ) != nullptr;
        }

        template <class T>
        [[nodiscard]] T* GetState( NodeId n )
        {
            return static_cast<T*>( FindState( n, T::Arg ) );
        }

    protected:
        IUITree()                            = default;
        IUITree( const IUITree& )            = default;
        IUITree& operator=( const IUITree& ) = default;
        IUITree( IUITree&& )                 = default;
        IUITree& operator=( IUITree&& )      = default;
    };

    // The children of @p n as a range, IN DRAW ORDER (IUITree::ChildCount / ChildAt). The count is read once,
    // when the range is made: a walk that edits the hierarchy while iterating was undefined before too.
    class ChildRange
    {
    public:
        class Iterator
        {
        public:
            Iterator( const IUITree* tree, NodeId parent, std::size_t i )
                 : m_Tree( tree ), m_Parent( parent ), m_I( i )
            {
            }
            NodeId operator*() const
            {
                return m_Tree->ChildAt( m_Parent, m_I );
            }
            Iterator& operator++()
            {
                ++m_I;
                return *this;
            }
            bool operator==( const Iterator& o ) const
            {
                return m_I == o.m_I;
            }

        private:
            const IUITree* m_Tree;
            NodeId         m_Parent;
            std::size_t    m_I;
        };

        ChildRange( const IUITree& tree, NodeId parent )
             : m_Tree( &tree ), m_Parent( parent ), m_Count( tree.ChildCount( parent ) )
        {
        }
        [[nodiscard]] Iterator begin() const
        {
            return { m_Tree, m_Parent, 0 };
        }
        [[nodiscard]] Iterator end() const
        {
            return { m_Tree, m_Parent, m_Count };
        }
        [[nodiscard]] std::size_t size() const
        {
            return m_Count;
        }
        [[nodiscard]] bool empty() const
        {
            return m_Count == 0;
        }
        [[nodiscard]] NodeId operator[]( std::size_t i ) const
        {
            return m_Tree->ChildAt( m_Parent, i );
        }
        [[nodiscard]] NodeId front() const
        {
            return m_Tree->ChildAt( m_Parent, 0 );
        }

    private:
        const IUITree* m_Tree;
        NodeId         m_Parent;
        std::size_t    m_Count;
    };

    [[nodiscard]] inline ChildRange ChildrenOf( const IUITree& tree, NodeId n )
    {
        return { tree, n };
    }

    // IUITree::Roots as a value, for a range-for.
    [[nodiscard]] inline std::vector<NodeId> RootsOf( const IUITree& tree, ArgKind kind )
    {
        std::vector<NodeId> out;
        tree.Roots( kind, out );
        return out;
    }
} // namespace Desert::UI
