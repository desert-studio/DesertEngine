#pragma once

#include <UI/Args/ArgKind.hpp>
#include <UI/Retained/Attribute.hpp>
#include <UI/Retained/Delegate.hpp>
#include <UI/UICursor.hpp>
#include <UI/UIPaint.hpp>
#include <UI/UITree.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

// A RETAINED WIDGET TREE BUILT FROM C++ — the Slate half of UE's seam, beside the ECS scene that is the UMG
// half (Engine/UI/Ecs/EcsUITree). The editor's own panels are not level content: they are built by code
// (WidgetBuilder.hpp, Slate's SNew), bound to the editor's model by attributes (Attribute.hpp, TAttribute)
// and answer the user through delegates (Delegate.hpp, FOnClicked / FOnFloatValueChanged).
//
// IT IS AN IUITree, AND NOTHING ELSE IS NEW. A node carries the same authored UI*Data arguments an ECS
// element carries (UI/Args), so layout, the walk, the widgets, focus, overlays and the renderer draw it with
// the code they draw a level's canvas with — there is no second widget set and no bridge. What the tree adds
// lives on its side of the seam: storage that is not a registry, attributes evaluated into the arguments
// before a frame (RefreshAttributes), and the frame's outcome turned back into delegate calls (DispatchFrame).
//
// One frame of a host:
//
//     tree.RefreshAttributes();                       // bound values -> the nodes' arguments
//     BeginUIFrame( view, tree, viewport, dt );
//     RenderCanvas2D( view, tree, canvas, dl, nullptr, &input, &clicked, &focused );
//     EndUIFrame( view, tree, dl, &input, &focused, &clicked, &messages );
//     tree.DispatchFrame( view, clicked, messages );  // clicks, value changes, double clicks -> delegates
//
// Node ids are an index and a version (as entt's are), so the id of a destroyed node never names the node
// that reuses its slot: Valid() refuses it.
namespace Desert::UI
{
    struct UIViewContext;

    class WidgetTree final : public IUITree
    {
    public:
        WidgetTree() = default;
        ~WidgetTree() override;
        WidgetTree( const WidgetTree& )            = delete;
        WidgetTree& operator=( const WidgetTree& ) = delete;
        WidgetTree( WidgetTree&& )                 = delete; // Storage() is this object's address
        WidgetTree& operator=( WidgetTree&& )      = delete;

        // --- Building ----------------------------------------------------------------------------------
        // A new node named @p name, appended as the LAST child of @p parent (drawn on top of its siblings),
        // or a root when @p parent is Null. An invalid non-null parent is a caller bug: the node is refused
        // (NodeId::Null) rather than silently made a root — and so is a tree that has run out of slots.
        [[nodiscard]] NodeId Create( std::string_view name, NodeId parent = NodeId::Null );

        // @p n and its whole sub-tree. Its ids become invalid; its slots are reused under new versions.
        void Destroy( NodeId n );

        // Every node.
        void Clear();

        // The arguments of kind T::Arg on @p n — added from @p data, or replaced when already there. nullptr
        // when @p n is not a node of this tree.
        template <class T>
        T* Add( NodeId n, T data = T{} )
        {
            if ( !Valid( n ) )
                return nullptr;
            Box& box = NodeAt( n ).Args[Index( T::Arg )];
            box.Reset();
            box.Ptr  = new T( std::move( data ) );
            box.Free = []( void* p ) { delete static_cast<T*>( p ); };
            return static_cast<T*>( box.Ptr );
        }

        // The arguments of kind T::Arg on @p n for editing, or nullptr when @p n has none (or is invalid).
        template <class T>
        [[nodiscard]] T* Edit( NodeId n )
        {
            return static_cast<T*>( ArgPtr( n, T::Arg ) );
        }

        template <class T>
        void Remove( NodeId n )
        {
            if ( Valid( n ) )
                NodeAt( n ).Args[Index( T::Arg )].Reset();
        }

        void SetStableId( NodeId n, std::uint64_t id );
        void SetWorldOrigin( NodeId n, std::optional<glm::vec3> origin );
        void SetCursor( NodeId n, std::optional<UICursorShape> cursor );

        // A press on @p n (or inside it) belongs to @p n and holds the pointer until the button is up
        // (IUITree::CapturesPointer) — a drag handle. Its paint reads the hold as UIPaintArgs::Captured.
        void SetCapturesPointer( NodeId n, bool captures );

        // --- Paint leaf (Slate's SLeafWidget::OnPaint) ---------------------------------------------------
        // @p n draws @p onPaint into the frame's draw list every frame, over its own widgets and under its
        // children (UIPaint.hpp). The delegate runs DURING the walk: it may draw, read the pointer and edit
        // its owner's state or this tree's arguments, but it must not create or destroy nodes.
        void OnPaint( NodeId n, Delegate<void( const UIPaintArgs& )> onPaint );

        // --- Attributes (Slate's TAttribute) -------------------------------------------------------------
        // Field @p field of @p n's T arguments follows @p value. A constant is written now and never again;
        // a bound attribute is written now and on every RefreshAttributes. The binding dies with the node.
        template <class T, class V>
        void Bind( NodeId n, V T::*field, std::type_identity_t<Attribute<V>> value )
        {
            if ( !Valid( n ) )
                return;
            if ( T* data = Edit<T>( n ) )
                data->*field = value.Get();
            if ( !value.IsBound() )
                return;
            NodeAt( n ).Bindings.push_back(
                 [n, field, value = std::move( value )]( WidgetTree& tree )
                 {
                     if ( T* data = tree.Edit<T>( n ) )
                         data->*field = value.Get();
                 } );
        }

        // Write every bound attribute into its node, then note the values the frame starts from, so the
        // changes DispatchFrame reports are the frame's own and not the model's. Once per frame, BEFORE
        // BeginUIFrame.
        void RefreshAttributes();

        // --- Delegates -----------------------------------------------------------------------------------
        // The click of @p n's button. Makes the button send its own message (UIButtonAction::SendEvent with
        // ClickMessageOf(n)), which DispatchFrame turns back into this call. @p n must carry UIButtonData.
        void OnClicked( NodeId n, Delegate<void()> onClicked );

        // A change of field @p field of @p n's T arguments made BY THE FRAME (a drag, a key, typing) — not by
        // RefreshAttributes. Slate's OnValueChanged: the owner updates its model here, and the bound
        // attribute brings the new value back next frame.
        template <class T, class V>
        void OnChanged( NodeId n, V T::*field, std::type_identity_t<Delegate<void( const V& )>> onChanged )
        {
            if ( !Valid( n ) )
                return;
            NodeAt( n ).Watches.push_back(
                 [n, field, onChanged = std::move( onChanged ),
                  seen = std::optional<V>{}]( WidgetTree& tree, std::vector<std::function<void()>>* fire ) mutable
                 {
                     const T* data = tree.Edit<T>( n );
                     if ( data == nullptr )
                         return;
                     const V& now = data->*field;
                     if ( fire != nullptr && seen.has_value() && !( *seen == now ) && onChanged.IsBound() )
                         fire->push_back( [onChanged, value = V( now )] { onChanged.Execute( value ); } );
                     seen = now;
                 } );
            // The value it starts from is the one it was built with.
            NodeAt( n ).Watches.back()( *this, nullptr );
        }

        // A double click whose press landed on @p n or on anything inside it (the nearest listener wins).
        void OnDoubleClicked( NodeId n, Delegate<void()> onDoubleClicked );

        // The commit of @p n's input field — Enter, or focus leaving the field (UE SEditableText::
        // OnTextCommitted with OnEnter / OnUserMovedFocus) — with the text committed. Makes the field send its
        // own commit message (UIInputFieldData::OnCommittedMessage = ClickMessageOf(n), sent as
        // "<message>|<text>"), which DispatchFrame turns back into this call. @p n must carry UIInputFieldData.
        void OnTextCommitted( NodeId n, Delegate<void( const std::string& )> onCommitted );

        // SLATE'S SetKeyboardFocus, asked by a widget of this tree (a spin box entering text mode hands focus to
        // its text field). The tree cannot move focus itself — focus is the host's (the NodeId it passes to the
        // walk) — so the request waits here and the host applies it after DispatchFrame, together with the
        // keyboard (UIKeyboardUser::Owner = its view). Null asks for no focus. The last request of a frame wins.
        void RequestKeyboardFocus( NodeId n );
        [[nodiscard]] std::optional<NodeId> TakeKeyboardFocusRequest();

        // Turn the frame that just ended into delegate calls: the clicks among @p clicked / @p messages, the
        // values the frame changed, and @p view's double click when this frame made one on this tree. Every
        // delegate runs AFTER the tree has been read, so one may build or destroy nodes. Once per frame, after
        // EndUIFrame.
        void DispatchFrame( const UIViewContext& view, std::string_view clicked,
                            std::span<const std::string> messages );

        // The message @p n's button sends (OnClicked). Carries the node's version, so a recycled slot's
        // message never reaches the delegate of the node that held it before.
        [[nodiscard]] static std::string ClickMessageOf( NodeId n );

        // --- IUITree ------------------------------------------------------------------------------------
        [[nodiscard]] bool                         Valid( NodeId n ) const override;
        [[nodiscard]] std::size_t                  NodeBound() const override;
        [[nodiscard]] NodeId                       Parent( NodeId n ) const override;
        [[nodiscard]] std::size_t                  ChildCount( NodeId n ) const override;
        [[nodiscard]] NodeId                       ChildAt( NodeId n, std::size_t i ) const override;
        [[nodiscard]] std::string_view             Name( NodeId n ) const override;
        [[nodiscard]] std::uint64_t                StableId( NodeId n ) const override;
        [[nodiscard]] const void*                  Find( NodeId n, ArgKind kind ) const override;
        [[nodiscard]] void*                        FindState( NodeId n, ArgKind kind ) override;
        void                                       Roots( ArgKind kind, std::vector<NodeId>& out ) const override;
        [[nodiscard]] const void*                  Storage() const override;
        [[nodiscard]] std::optional<glm::vec3>     WorldOrigin( NodeId n ) const override;
        [[nodiscard]] std::optional<UICursorShape> CursorOf( NodeId n ) const override;
        [[nodiscard]] bool                         CapturesPointer( NodeId n ) const override;
        void                                       Paint( NodeId n, const UIPaintArgs& args ) override;

    private:
        // One owned argument block, type-erased: the T it holds is the one whose Arg indexes the slot.
        struct Box
        {
            void* Ptr               = nullptr;
            void ( *Free )( void* ) = nullptr;

            Box()                        = default;
            Box( const Box& )            = delete;
            Box& operator=( const Box& ) = delete;
            Box( Box&& o ) noexcept : Ptr( std::exchange( o.Ptr, nullptr ) ), Free( o.Free )
            {
            }
            Box& operator=( Box&& o ) noexcept
            {
                if ( this != &o )
                {
                    Reset();
                    Ptr  = std::exchange( o.Ptr, nullptr );
                    Free = o.Free;
                }
                return *this;
            }
            ~Box()
            {
                Reset();
            }
            void Reset()
            {
                if ( Ptr != nullptr )
                    Free( Ptr );
                Ptr = nullptr;
            }
        };

        // A watch notes the value it saw; handed a list, it also queues its delegate when the value moved.
        using Watch = std::function<void( WidgetTree&, std::vector<std::function<void()>>* )>;

        struct Node
        {
            bool                                            Alive   = false;
            std::uint32_t                                   Version = 0;
            std::uint64_t                                   Serial  = 0; // creation order, for Roots
            NodeId                                          Parent  = NodeId::Null;
            std::vector<NodeId>                             Children;
            std::string                                     Name;
            std::uint64_t                                   Stable = 0;
            std::optional<glm::vec3>                        Origin;
            std::optional<UICursorShape>                    Cursor;
            bool                                            Captures = false;
            std::array<Box, kArgKindCount>                  Args;
            std::vector<std::function<void( WidgetTree& )>> Bindings;
            std::vector<Watch>                              Watches;
            Delegate<void()>                                Clicked;
            Delegate<void()>                                DoubleClicked;
            Delegate<void( const std::string& )>            Committed;
            Delegate<void( const UIPaintArgs& )>            Painter;
        };

        static constexpr std::uint32_t kIndexBits  = 20;
        static constexpr std::uint32_t kIndexMask  = ( 1u << kIndexBits ) - 1u;
        static constexpr std::uint32_t kVersionMax = ( 1u << ( 32u - kIndexBits ) ) - 1u;
        // The all-ones index is part of NodeId::Null, so the last usable slot is the one below it.
        static constexpr std::uint32_t kMaxSlots = kIndexMask;

        [[nodiscard]] static constexpr std::size_t Index( ArgKind kind )
        {
            return static_cast<std::size_t>( kind );
        }
        [[nodiscard]] static constexpr NodeId MakeId( std::uint32_t slot, std::uint32_t version )
        {
            return static_cast<NodeId>( ( version << kIndexBits ) | slot );
        }
        [[nodiscard]] static constexpr std::uint32_t SlotOf( NodeId n )
        {
            return static_cast<std::uint32_t>( n ) & kIndexMask;
        }
        [[nodiscard]] static constexpr std::uint32_t VersionOf( NodeId n )
        {
            return static_cast<std::uint32_t>( n ) >> kIndexBits;
        }

        // Only for an id Valid() accepted.
        [[nodiscard]] Node&       NodeAt( NodeId n );
        [[nodiscard]] const Node& NodeAt( NodeId n ) const;
        [[nodiscard]] void*       ArgPtr( NodeId n, ArgKind kind ) const;

        void Release( NodeId n ); // frees @p n's slot and its sub-tree's, without touching its parent

        std::vector<Node>          m_Nodes;
        std::vector<std::uint32_t> m_Free;
        std::uint64_t              m_NextSerial = 0;
        std::optional<NodeId>      m_FocusRequest;
    };
} // namespace Desert::UI
