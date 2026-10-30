#include <UI/Retained/WidgetTree.hpp>

#include <UI/Args/UIButtonArgs.hpp>
#include <UI/Args/UIControlArgs.hpp>
#include <UI/UICanvasContext.hpp>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <utility>

namespace Desert::UI
{
    namespace
    {
        // The prefix of every message a retained button sends (WidgetTree::ClickMessageOf). Never authored in a
        // level: a level's buttons send what their author typed, and this tree reads only its own messages.
        constexpr std::string_view kRetainedClickPrefix = "retained.click:";
    } // namespace

    WidgetTree::~WidgetTree() = default;

    WidgetTree::Node& WidgetTree::NodeAt( NodeId n )
    {
        return m_Nodes[SlotOf( n )];
    }

    const WidgetTree::Node& WidgetTree::NodeAt( NodeId n ) const
    {
        return m_Nodes[SlotOf( n )];
    }

    void* WidgetTree::ArgPtr( NodeId n, ArgKind kind ) const
    {
        if ( !Valid( n ) || Index( kind ) >= kArgKindCount )
            return nullptr;
        return NodeAt( n ).Args[Index( kind )].Ptr;
    }

    NodeId WidgetTree::Create( std::string_view name, NodeId parent )
    {
        if ( parent != NodeId::Null && !Valid( parent ) )
            return NodeId::Null;

        std::uint32_t slot = 0;
        if ( !m_Free.empty() )
        {
            slot = m_Free.back();
            m_Free.pop_back();
        }
        else
        {
            if ( m_Nodes.size() >= kMaxSlots )
                return NodeId::Null;
            slot = static_cast<std::uint32_t>( m_Nodes.size() );
            m_Nodes.emplace_back();
        }

        Node& node      = m_Nodes[slot];
        node.Alive      = true;
        node.Serial     = m_NextSerial++;
        node.Parent     = parent;
        node.Name       = std::string( name );
        const NodeId id = MakeId( slot, node.Version );
        if ( parent != NodeId::Null )
            NodeAt( parent ).Children.push_back( id );
        return id;
    }

    void WidgetTree::Release( NodeId n )
    {
        // The children list is moved out first: releasing a child must not edit the list being walked.
        std::vector<NodeId> children = std::move( NodeAt( n ).Children );
        for ( const NodeId c : children )
            if ( Valid( c ) )
                Release( c );

        Node&               node = NodeAt( n );
        const std::uint32_t next = node.Version >= kVersionMax ? 0u : node.Version + 1u;
        node                     = Node{};
        node.Version             = next;
        // A slot whose version wrapped would hand a dead id back to life; it is retired instead of reused.
        if ( next != 0u )
            m_Free.push_back( SlotOf( n ) );
    }

    void WidgetTree::Destroy( NodeId n )
    {
        if ( !Valid( n ) )
            return;
        const NodeId parent = NodeAt( n ).Parent;
        if ( parent != NodeId::Null && Valid( parent ) )
            std::erase( NodeAt( parent ).Children, n );
        Release( n );
    }

    void WidgetTree::Clear()
    {
        // Released root by root rather than dropped wholesale, so every slot moves to a new version and no id
        // handed out before the clear can name a node made after it.
        for ( std::uint32_t i = 0; i < m_Nodes.size(); ++i )
            if ( m_Nodes[i].Alive && m_Nodes[i].Parent == NodeId::Null )
                Release( MakeId( i, m_Nodes[i].Version ) );
    }

    void WidgetTree::SetStableId( NodeId n, std::uint64_t id )
    {
        if ( Valid( n ) )
            NodeAt( n ).Stable = id;
    }

    void WidgetTree::SetWorldOrigin( NodeId n, std::optional<glm::vec3> origin )
    {
        if ( Valid( n ) )
            NodeAt( n ).Origin = origin;
    }

    void WidgetTree::SetCursor( NodeId n, std::optional<UICursorShape> cursor )
    {
        if ( Valid( n ) )
            NodeAt( n ).Cursor = cursor;
    }

    void WidgetTree::SetCapturesPointer( NodeId n, bool captures )
    {
        if ( Valid( n ) )
            NodeAt( n ).Captures = captures;
    }

    void WidgetTree::SetTakesTextInput( NodeId n, bool takes )
    {
        if ( Valid( n ) )
            NodeAt( n ).TextInput = takes;
    }

    void WidgetTree::OnPaint( NodeId n, Delegate<void( const UIPaintArgs& )> onPaint )
    {
        if ( Valid( n ) )
            NodeAt( n ).Painter = std::move( onPaint );
    }

    void WidgetTree::RefreshAttributes()
    {
        // By index and re-fetched on every step: a getter is user code and the vector may not stay put.
        for ( std::size_t i = 0; i < m_Nodes.size(); ++i )
        {
            if ( !m_Nodes[i].Alive )
                continue;
            for ( std::size_t b = 0; i < m_Nodes.size() && b < m_Nodes[i].Bindings.size(); ++b )
            {
                const auto binding = m_Nodes[i].Bindings[b];
                binding( *this );
            }
            for ( std::size_t w = 0; i < m_Nodes.size() && w < m_Nodes[i].Watches.size(); ++w )
                m_Nodes[i].Watches[w]( *this, nullptr );
        }
    }

    std::string WidgetTree::ClickMessageOf( NodeId n )
    {
        return std::string( kRetainedClickPrefix ) + std::to_string( static_cast<std::uint32_t>( n ) );
    }

    void WidgetTree::OnClicked( NodeId n, Delegate<void()> onClicked )
    {
        auto* button = Edit<UIButtonData>( n );
        if ( button == nullptr )
            return;
        button->Action         = UIButtonAction::SendEvent;
        button->OnClickMessage = ClickMessageOf( n );
        NodeAt( n ).Clicked    = std::move( onClicked );
    }

    void WidgetTree::OnDoubleClicked( NodeId n, Delegate<void()> onDoubleClicked )
    {
        if ( Valid( n ) )
            NodeAt( n ).DoubleClicked = std::move( onDoubleClicked );
    }

    void WidgetTree::OnTextCommitted( NodeId n, Delegate<void( const std::string& )> onCommitted )
    {
        auto* field = Edit<UIInputFieldData>( n );
        if ( field == nullptr )
            return;
        field->OnCommittedMessage = ClickMessageOf( n );
        NodeAt( n ).Committed     = std::move( onCommitted );
    }

    void WidgetTree::OnDragDetected( NodeId n, Delegate<UIDragPayload()> onDragDetected )
    {
        if ( Valid( n ) )
            NodeAt( n ).DragSource = std::move( onDragDetected );
    }

    void WidgetTree::OnDrop( NodeId n, DropHandler handler )
    {
        if ( Valid( n ) && !handler.Type.empty() )
            NodeAt( n ).Drops.push_back( std::move( handler ) );
    }

    const WidgetTree::DropHandler* WidgetTree::HandlerFor( NodeId n, const UIDragPayload& payload ) const
    {
        if ( !Valid( n ) )
            return nullptr;
        for ( const DropHandler& h : NodeAt( n ).Drops )
            if ( payload.Is( h.Type ) && ( !h.Accepts.IsBound() || h.Accepts.Execute( payload ) ) )
                return &h;
        return nullptr;
    }

    void WidgetTree::RequestKeyboardFocus( NodeId n )
    {
        m_FocusRequest = Valid( n ) ? n : NodeId::Null;
    }

    std::optional<NodeId> WidgetTree::TakeKeyboardFocusRequest()
    {
        return std::exchange( m_FocusRequest, std::nullopt );
    }

    void WidgetTree::DispatchFrame( const UIViewContext& view, std::string_view clicked,
                                    std::span<const std::string> messages )
    {
        // Everything is READ first and queued; nothing runs until the tree has been read to the end.
        std::vector<std::function<void()>> fire;

        auto clickOf = [&]( std::string_view msg )
        {
            if ( !msg.starts_with( kRetainedClickPrefix ) )
                return;
            msg.remove_prefix( kRetainedClickPrefix.size() );
            // "<id>" is a button's click; "<id>|<text>" an input field's commit (OnTextCommitted).
            const std::size_t bar    = msg.find( '|' );
            const std::string_view id = bar == std::string_view::npos ? msg : msg.substr( 0, bar );
            std::uint32_t     raw    = 0;
            const auto [end, ec]     = std::from_chars( id.data(), id.data() + id.size(), raw );
            if ( ec != std::errc{} || end != id.data() + id.size() )
                return;
            const NodeId n = static_cast<NodeId>( raw );
            if ( !Valid( n ) )
                return;
            if ( bar == std::string_view::npos )
            {
                if ( NodeAt( n ).Clicked.IsBound() )
                    fire.push_back( [d = NodeAt( n ).Clicked] { d.Execute(); } );
            }
            else if ( NodeAt( n ).Committed.IsBound() )
                fire.push_back( [d = NodeAt( n ).Committed, text = std::string( msg.substr( bar + 1 ) )]
                                { d.Execute( text ); } );
        };
        clickOf( clicked );
        for ( const std::string& msg : messages )
            clickOf( msg );

        for ( std::size_t i = 0; i < m_Nodes.size(); ++i )
            if ( m_Nodes[i].Alive )
                for ( Watch& w : m_Nodes[i].Watches )
                    w( *this, &fire );

        // The double click this frame made, when it was made on this tree: the nearest listener at or above
        // the element the press was elected on.
        const UIPressRecord& press = view.LastPress;
        if ( view.Scene == Storage() && press.Frame == view.FrameIndex && press.Clicks == 2 &&
             press.Button == UIPointerButton::Left )
        {
            std::size_t guard = 0;
            for ( NodeId t = press.Target; t != NodeId::Null && Valid( t ) && guard++ <= NodeBound();
                  t        = NodeAt( t ).Parent )
                if ( NodeAt( t ).DoubleClicked.IsBound() )
                {
                    fire.push_back( [d = NodeAt( t ).DoubleClicked] { d.Execute(); } );
                    break;
                }
        }

        // The drop this frame made on this tree, and the target a drag hovers now — each to the handler of
        // that target that takes the payload. The payload is copied into the call: the view's copy dies with
        // the drag state, the handler's lives as long as the handler wants it.
        if ( view.Scene == Storage() )
        {
            if ( view.LastDrop.Frame == view.FrameIndex )
                if ( const DropHandler* h = HandlerFor( view.LastDrop.Target, view.LastDrop.Payload );
                     h != nullptr && h->OnDrop.IsBound() )
                    fire.push_back( [d = h->OnDrop, p = view.LastDrop.Payload] { d.Execute( p ); } );
            if ( view.Drag.Active && view.Drag.OverAnswer == UIDropAnswer::Accept )
                if ( const DropHandler* h = HandlerFor( view.Drag.Over, view.Drag.Payload );
                     h != nullptr && h->OnDragOver.IsBound() )
                    fire.push_back( [d = h->OnDragOver, p = view.Drag.Payload] { d.Execute( p ); } );
        }

        for ( const auto& f : fire )
            f();
    }

    // --- IUITree -------------------------------------------------------------------------------------------

    bool WidgetTree::Valid( NodeId n ) const
    {
        if ( n == NodeId::Null )
            return false;
        const std::uint32_t slot = SlotOf( n );
        return slot < m_Nodes.size() && m_Nodes[slot].Alive && m_Nodes[slot].Version == VersionOf( n );
    }

    std::size_t WidgetTree::NodeBound() const
    {
        return m_Nodes.size();
    }

    NodeId WidgetTree::Parent( NodeId n ) const
    {
        return Valid( n ) ? NodeAt( n ).Parent : NodeId::Null;
    }

    std::size_t WidgetTree::ChildCount( NodeId n ) const
    {
        return Valid( n ) ? NodeAt( n ).Children.size() : 0;
    }

    NodeId WidgetTree::ChildAt( NodeId n, std::size_t i ) const
    {
        if ( !Valid( n ) )
            return NodeId::Null;
        const auto& children = NodeAt( n ).Children;
        return i < children.size() ? children[i] : NodeId::Null;
    }

    std::string_view WidgetTree::Name( NodeId n ) const
    {
        return Valid( n ) ? std::string_view( NodeAt( n ).Name ) : std::string_view{};
    }

    std::uint64_t WidgetTree::StableId( NodeId n ) const
    {
        return Valid( n ) ? NodeAt( n ).Stable : 0u;
    }

    const void* WidgetTree::Find( NodeId n, ArgKind kind ) const
    {
        return ArgPtr( n, kind );
    }

    void* WidgetTree::FindState( NodeId n, ArgKind kind )
    {
        return IsStateKind( kind ) ? ArgPtr( n, kind ) : nullptr;
    }

    void WidgetTree::Roots( ArgKind kind, std::vector<NodeId>& out ) const
    {
        out.clear();
        if ( Index( kind ) >= kArgKindCount )
            return;
        std::vector<std::uint32_t> slots;
        for ( std::uint32_t i = 0; i < m_Nodes.size(); ++i )
            if ( m_Nodes[i].Alive && m_Nodes[i].Args[Index( kind )].Ptr != nullptr )
                slots.push_back( i );
        // AUTHORED order is creation order (IUITree::Roots); a reused slot is not an older node.
        std::sort( slots.begin(), slots.end(),
                   [this]( std::uint32_t a, std::uint32_t b ) { return m_Nodes[a].Serial < m_Nodes[b].Serial; } );
        out.reserve( slots.size() );
        for ( const std::uint32_t s : slots )
            out.push_back( MakeId( s, m_Nodes[s].Version ) );
    }

    const void* WidgetTree::Storage() const
    {
        return this;
    }

    std::optional<glm::vec3> WidgetTree::WorldOrigin( NodeId n ) const
    {
        return Valid( n ) ? NodeAt( n ).Origin : std::nullopt;
    }

    std::optional<UICursorShape> WidgetTree::CursorOf( NodeId n ) const
    {
        return Valid( n ) ? NodeAt( n ).Cursor : std::nullopt;
    }

    bool WidgetTree::CapturesPointer( NodeId n ) const
    {
        return Valid( n ) && NodeAt( n ).Captures;
    }

    bool WidgetTree::TakesTextInput( NodeId n ) const
    {
        return Valid( n ) && NodeAt( n ).TextInput;
    }

    std::optional<UIDragPayload> WidgetTree::DragSourceOf( NodeId n ) const
    {
        if ( !Valid( n ) || !NodeAt( n ).DragSource.IsBound() )
            return std::nullopt;
        UIDragPayload made = NodeAt( n ).DragSource.Execute();
        if ( made.Empty() ) // a source that has nothing to carry right now (an empty list's row)
            return std::nullopt;
        return made;
    }

    UIDropAnswer WidgetTree::DropAnswerOf( NodeId n, const UIDragPayload& payload ) const
    {
        if ( !Valid( n ) || NodeAt( n ).Drops.empty() )
            return UIDropAnswer::None;
        return HandlerFor( n, payload ) != nullptr ? UIDropAnswer::Accept : UIDropAnswer::Reject;
    }

    void WidgetTree::Paint( NodeId n, const UIPaintArgs& args )
    {
        if ( !Valid( n ) || !NodeAt( n ).Painter.IsBound() )
            return;
        // Run from a copy: the delegate may edit this tree's arguments, and a node's storage is not promised
        // to stay where it was while user code runs.
        const Delegate<void( const UIPaintArgs& )> painter = NodeAt( n ).Painter;
        painter.Execute( args );
    }
} // namespace Desert::UI
