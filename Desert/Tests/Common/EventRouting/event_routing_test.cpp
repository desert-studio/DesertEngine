#include <gtest/gtest.h>

#include <Common/Core/Events/EventTree.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace Common;

namespace
{
    using Log = std::vector<std::string>;

    struct Probe
    {
        Probe( std::string name, Log& log ) : Name( std::move( name ) ), Journal( &log )
        {
        }
        ~Probe()
        {
            Journal->push_back( Name + ".destroyed" );
        }

        bool OnPreviewKeyPressed( KeyPressedEvent& )
        {
            Journal->push_back( Name + ".previewKey" );
            return ClaimsPreview;
        }
        bool OnKeyPressed( KeyPressedEvent& )
        {
            Journal->push_back( Name + ".key" );
            return ClaimsKey;
        }
        bool OnMouseButtonPressed( MouseButtonPressedEvent& )
        {
            Journal->push_back( Name + ".click" );
            return ClaimsClick;
        }
        bool OnWindowResized( EventWindowResize& )
        {
            Journal->push_back( Name + ".resize" );
            return true;
        }

        std::string Name;
        Log*        Journal;
        bool        ClaimsPreview = false;
        bool        ClaimsKey     = false;
        bool        ClaimsClick   = false;
    };

    struct KeyOnly
    {
        bool OnKeyPressed( KeyPressedEvent& )
        {
            ++Calls;
            return false;
        }
        int Calls = 0;
    };

    struct Silent
    {
    };

    struct BaseTypeCatcher
    {
        bool OnKeyPressed( KeyEvent& )
        {
            return true;
        }
    };

    struct VoidHandler
    {
        void OnKeyPressed( KeyPressedEvent& )
        {
        }
    };

    struct Counter
    {
        bool OnKeyPressed( KeyPressedEvent& )
        {
            ++Keys;
            return true;
        }
        bool OnWindowResized( EventWindowResize& )
        {
            ++Resizes;
            return false;
        }
        long long Keys    = 0;
        long long Resizes = 0;
    };

    KeyPressedEvent APressedKey()
    {
        return KeyPressedEvent( KeyCode::A, 0 );
    }

    MouseButtonPressedEvent ALeftClick()
    {
        return MouseButtonPressedEvent( MouseButton::Left );
    }

    struct PanelOverBackground
    {
        Log         log;
        EventTree   tree;
        EventNodeId background;
        EventNodeId panel;
        Probe*      backgroundProbe = nullptr;
        Probe*      panelProbe      = nullptr;

        PanelOverBackground()
        {
            auto bg         = tree.Emplace<Probe>( tree.Root(), "background", log );
            auto p          = tree.Emplace<Probe>( bg.Id, "panel", log );
            background      = bg.Id;
            panel           = p.Id;
            backgroundProbe = &bg.Object;
            panelProbe      = &p.Object;
        }
    };
} // namespace

TEST( EventRouting, AClassWithoutHandlersReceivesNothingAndCostsNothing )
{
    static_assert( std::is_empty_v<Silent> );
    static_assert( !ReceivesEvents<Silent> );
    static_assert( EventHandlerTableFor<Silent>.BubbleMask == 0 && EventHandlerTableFor<Silent>.PreviewMask == 0 );

    EventTree tree;
    auto      counter = tree.Emplace<KeyOnly>( tree.Root() );
    auto      silent  = tree.Emplace<Silent>( counter.Id );
    ASSERT_TRUE( tree.SetFocus( silent.Id ) );

    auto             key   = APressedKey();
    const EventReply reply = tree.Route( key );
    EXPECT_FALSE( reply.Handled );
    EXPECT_EQ( counter.Object.Calls, 1 );
}

TEST( EventRouting, AHandlerIsCalledOnlyForItsOwnEventType )
{
    static_assert( HandlesEvent<KeyOnly, KeyPressedEvent> );
    static_assert( !HandlesEvent<KeyOnly, KeyTypedEvent> );
    static_assert( !HandlesEvent<KeyOnly, MouseButtonPressedEvent> );
    static_assert( EventHandlerTableFor<KeyOnly>.BubbleMask == EventBit<KeyPressedEvent> );

    EventTree tree;
    auto      node = tree.Emplace<KeyOnly>( tree.Root() );
    tree.SetFocus( node.Id );
    tree.SetHovered( node.Id );

    KeyTypedEvent     typed( 'a' );
    MouseMovedEvent   moved( 1.0f, 2.0f );
    auto              click = ALeftClick();
    EventWindowResize resize( 640, 480 );
    EventWindowClose  close;
    tree.Route( typed );
    tree.Route( moved );
    tree.Route( click );
    tree.Route( resize );
    tree.Route( close );
    EXPECT_EQ( node.Object.Calls, 0 );

    auto key = APressedKey();
    tree.Route( key );
    EXPECT_EQ( node.Object.Calls, 1 );
}

TEST( EventRouting, AHandlerTakingABaseEventTypeOrNotReturningBoolIsRefused )
{
    static_assert( !HandlesEvent<BaseTypeCatcher, KeyPressedEvent> );
    static_assert( AcceptsEventThroughAnotherSignature<BaseTypeCatcher, KeyPressedEvent> );
    static_assert( !HandlesEvent<VoidHandler, KeyPressedEvent> );
    static_assert( AcceptsEventThroughAnotherSignature<VoidHandler, KeyPressedEvent> );
    static_assert( !RoutedEvent<KeyEvent> );
}

TEST( EventRouting, TheFocusedNodeHearsAKeyBeforeItsBackground )
{
    PanelOverBackground f;
    f.tree.SetFocus( f.panel );

    auto key = APressedKey();
    f.tree.Route( key );

    const Log expected = { "background.previewKey", "panel.previewKey", "panel.key", "background.key" };
    EXPECT_EQ( f.log, expected );
}

TEST( EventRouting, HandledStopsTheBubble )
{
    PanelOverBackground f;
    f.tree.SetFocus( f.panel );
    f.panelProbe->ClaimsKey = true;

    auto             key   = APressedKey();
    const EventReply reply = f.tree.Route( key );

    EXPECT_TRUE( reply.Handled );
    EXPECT_EQ( reply.HandledBy, f.panel );
    EXPECT_EQ( reply.Phase, EventPhase::Bubble );
    const Log expected = { "background.previewKey", "panel.previewKey", "panel.key" };
    EXPECT_EQ( f.log, expected );
}

TEST( EventRouting, HandledInPreviewStopsTheWholeRouteAtTheAncestor )
{
    PanelOverBackground f;
    f.tree.SetFocus( f.panel );
    f.backgroundProbe->ClaimsPreview = true;

    auto             key   = APressedKey();
    const EventReply reply = f.tree.Route( key );

    EXPECT_EQ( reply.HandledBy, f.background );
    EXPECT_EQ( reply.Phase, EventPhase::Preview );
    const Log expected = { "background.previewKey" };
    EXPECT_EQ( f.log, expected );
}

TEST( EventRouting, WithoutFocusNobodyHearsAKey )
{
    PanelOverBackground f;
    auto                key   = APressedKey();
    const EventReply    reply = f.tree.Route( key );
    EXPECT_FALSE( reply.Handled );
    EXPECT_TRUE( f.log.empty() );
}

TEST( EventRouting, PointerEventsGoToTheHoveredNodeNotTheFocusedOne )
{
    PanelOverBackground f;
    auto                other = f.tree.Emplace<Probe>( f.tree.Root(), "other", f.log );
    f.tree.SetFocus( f.panel );
    f.tree.SetHovered( other.Id );

    auto click = ALeftClick();
    f.tree.Route( click );

    const Log expected = { "other.click" };
    EXPECT_EQ( f.log, expected );
}

TEST( EventRouting, MouseCaptureOverridesHover )
{
    PanelOverBackground f;
    auto                other = f.tree.Emplace<Probe>( f.tree.Root(), "other", f.log );
    f.tree.SetHovered( other.Id );
    f.tree.SetCapture( f.panel );

    auto click = ALeftClick();
    f.tree.Route( click );

    const Log expected = { "panel.click", "background.click" };
    EXPECT_EQ( f.log, expected );
}

TEST( EventRouting, ANodeTakenOutOfTheTreeHearsNothing )
{
    PanelOverBackground f;
    f.tree.SetFocus( f.panel );
    std::unique_ptr<Probe> taken = f.tree.Take<Probe>( f.panel );
    ASSERT_NE( taken, nullptr );
    EXPECT_FALSE( f.tree.Contains( f.panel ) );
    EXPECT_FALSE( f.tree.Focus().IsSet() );

    f.tree.SetFocus( f.background );
    auto key = APressedKey();
    f.tree.Route( key );
    auto click = ALeftClick();
    f.tree.SetHovered( f.background );
    f.tree.Route( click );

    for ( const std::string& line : f.log )
        EXPECT_EQ( line.rfind( "panel.", 0 ), std::string::npos ) << line;
    EXPECT_EQ( taken->Name, "panel" );
}

TEST( EventRouting, ADestroyedObjectHearsNothingAndLeavesNoDanglingReference )
{
    PanelOverBackground f;
    f.tree.SetFocus( f.panel );
    f.tree.SetHovered( f.panel );
    f.tree.SetCapture( f.panel );

    EXPECT_TRUE( f.tree.Remove( f.panel ) );
    const Log destroyed = { "panel.destroyed" };
    EXPECT_EQ( f.log, destroyed );
    f.log.clear();

    EXPECT_FALSE( f.tree.Contains( f.panel ) );
    EXPECT_EQ( f.tree.Get<Probe>( f.panel ), nullptr );
    EXPECT_FALSE( f.tree.Focus().IsSet() );
    EXPECT_FALSE( f.tree.Capture().IsSet() );

    auto              key   = APressedKey();
    auto              click = ALeftClick();
    EventWindowResize resize( 1, 1 );
    f.tree.Route( key );
    f.tree.Route( click );
    f.tree.Route( resize );
    const Log onlyTheBackground = { "background.resize" };
    EXPECT_EQ( f.log, onlyTheBackground );
}

TEST( EventRouting, RemovingANodeDestroysItsSubtreeChildrenFirst )
{
    PanelOverBackground f;
    f.tree.Emplace<Probe>( f.panel, "button", f.log );
    EXPECT_EQ( f.tree.Size(), 3u );

    f.tree.Remove( f.background );

    const Log expected = { "button.destroyed", "panel.destroyed", "background.destroyed" };
    EXPECT_EQ( f.log, expected );
    EXPECT_EQ( f.tree.Size(), 0u );
}

namespace
{
    struct RemovesANodeOnKey
    {
        RemovesANodeOnKey( EventTree& tree, Log& log ) : Tree( &tree ), Journal( &log )
        {
        }
        bool OnKeyPressed( KeyPressedEvent& )
        {
            Journal->push_back( "remover.key" );
            Tree->Remove( Victim );
            Journal->push_back( "remover.returns" );
            return false;
        }
        EventTree*  Tree;
        Log*        Journal;
        EventNodeId Victim{};
    };
} // namespace

TEST( EventRouting, ANodeRemovedDuringDeliveryIsSkippedAndDestroyedAfterDelivery )
{
    PanelOverBackground f;
    auto                remover = f.tree.Emplace<RemovesANodeOnKey>( f.panel, f.tree, f.log );
    remover.Object.Victim       = f.background;
    f.tree.SetFocus( remover.Id );

    auto key = APressedKey();
    f.tree.Route( key );
    f.log.push_back( "route.returned" );

    const Log expected = { "background.previewKey", "panel.previewKey",     "remover.key",   "remover.returns",
                           "panel.destroyed",       "background.destroyed", "route.returned" };
    EXPECT_EQ( f.log, expected );
    EXPECT_EQ( f.tree.Size(), 0u );
}

TEST( EventRouting, AHandlerMayRemoveItsOwnNode )
{
    Log       log;
    EventTree tree;
    auto      remover     = tree.Emplace<RemovesANodeOnKey>( tree.Root(), tree, log );
    remover.Object.Victim = remover.Id;
    tree.SetFocus( remover.Id );

    auto key = APressedKey();
    tree.Route( key );

    const Log expected = { "remover.key", "remover.returns" };
    EXPECT_EQ( log, expected );
    EXPECT_FALSE( tree.Contains( remover.Id ) );
}

TEST( EventRouting, WindowEventsReachEveryNodeAndHandledCannotHideThem )
{
    PanelOverBackground f;
    auto                counter = f.tree.Emplace<Counter>( f.panel );

    EventWindowResize resize( 800, 600 );
    const EventReply  reply = f.tree.Route( resize );

    EXPECT_TRUE( reply.Handled );
    EXPECT_EQ( counter.Object.Resizes, 1 );
    EXPECT_EQ( std::count( f.log.begin(), f.log.end(), "background.resize" ), 1 );
    EXPECT_EQ( std::count( f.log.begin(), f.log.end(), "panel.resize" ), 1 );
}

TEST( EventRouting, BroadcastOrderIsFocusChainThenPointerChainThenTreeOrder )
{
    Log       log;
    EventTree tree;
    auto      a  = tree.Emplace<Probe>( tree.Root(), "a", log );
    auto      a1 = tree.Emplace<Probe>( a.Id, "a1", log );
    auto      a2 = tree.Emplace<Probe>( a.Id, "a2", log );
    auto      b  = tree.Emplace<Probe>( tree.Root(), "b", log );
    auto      b1 = tree.Emplace<Probe>( b.Id, "b1", log );
    tree.Emplace<Probe>( tree.Root(), "c", log );
    tree.SetFocus( a2.Id );
    tree.SetHovered( b1.Id );
    (void)a1;

    EventWindowResize resize( 1, 1 );
    tree.Route( resize );

    const Log expected = { "a2.resize", "a.resize", "b1.resize", "b.resize", "a1.resize", "c.resize" };
    EXPECT_EQ( log, expected );
}

TEST( EventRouting, DeliveryOrderIsDeterministic )
{
    auto run = []
    {
        Log log;
        {
            EventTree tree;
            auto      a = tree.Emplace<Probe>( tree.Root(), "a", log );
            auto      b = tree.Emplace<Probe>( a.Id, "b", log );
            auto      c = tree.Emplace<Probe>( a.Id, "c", log );
            tree.Emplace<Probe>( c.Id, "d", log );
            tree.SetFocus( b.Id );
            tree.SetHovered( c.Id );
            for ( int i = 0; i < 3; ++i )
            {
                auto              key   = APressedKey();
                auto              click = ALeftClick();
                EventWindowResize resize( 1, 1 );
                tree.Route( key );
                tree.Route( click );
                tree.Route( resize );
            }
        }
        return log;
    };
    const Log first = run();
    EXPECT_EQ( run(), first );
    ASSERT_GE( first.size(), 9u );
    const Log head( first.begin(), first.begin() + 9 );
    const Log expected = { "a.previewKey", "b.previewKey", "b.key",    "a.key",   "c.click",
                           "a.click",      "b.resize",     "a.resize", "c.resize" };
    EXPECT_EQ( head, expected );
}

TEST( EventRouting, TheHandlerTableIsBuiltOncePerType )
{
    constexpr uint32_t mask = EventHandlerTableFor<Probe>.BubbleMask;
    static_assert( mask ==
                   (EventBit<KeyPressedEvent> | EventBit<MouseButtonPressedEvent> | EventBit<EventWindowResize>));
    static_assert( EventHandlerTableFor<Probe>.PreviewMask == EventBit<KeyPressedEvent> );

    Log       log;
    EventTree tree;
    auto      first  = tree.Emplace<Probe>( tree.Root(), "first", log );
    auto      second = tree.Emplace<Probe>( tree.Root(), "second", log );
    auto      other  = tree.Emplace<KeyOnly>( tree.Root() );

    EXPECT_EQ( tree.HandlerTableOf( first.Id ), &EventHandlerTableFor<Probe> );
    EXPECT_EQ( tree.HandlerTableOf( second.Id ), &EventHandlerTableFor<Probe> );
    EXPECT_NE( tree.HandlerTableOf( other.Id ), &EventHandlerTableFor<Probe> );
}

TEST( EventRouting, AStaleIdNamesNoNodeEvenAfterItsSlotIsReused )
{
    Log               log;
    EventTree         tree;
    auto              old   = tree.Emplace<Probe>( tree.Root(), "old", log );
    const EventNodeId stale = old.Id;
    tree.Remove( stale );
    auto fresh = tree.Emplace<Probe>( tree.Root(), "fresh", log );

    EXPECT_EQ( fresh.Id.Index, stale.Index );
    EXPECT_FALSE( tree.Contains( stale ) );
    EXPECT_EQ( tree.Get<Probe>( stale ), nullptr );
    EXPECT_FALSE( tree.SetFocus( stale ) );
    EXPECT_FALSE( tree.Remove( stale ) );
    EXPECT_TRUE( tree.Contains( fresh.Id ) );
}

TEST( EventRouting, ANodeIsReadBackOnlyAsTheTypeItWasInsertedAs )
{
    EventTree tree;
    auto      node = tree.Emplace<KeyOnly>( tree.Root() );
    EXPECT_EQ( tree.Get<KeyOnly>( node.Id ), &node.Object );
    EXPECT_EQ( tree.Get<Counter>( node.Id ), nullptr );
}

TEST( EventRouting, TheTreeOwnsItsObjects )
{
    Log log;
    {
        EventTree tree;
        auto      parent = tree.Emplace<Probe>( tree.Root(), "parent", log );
        tree.Emplace<Probe>( parent.Id, "child", log );
        tree.Adopt( tree.Root(), std::make_unique<Probe>( "adopted", log ) );
    }
    const Log expected = { "child.destroyed", "parent.destroyed", "adopted.destroyed" };
    EXPECT_EQ( log, expected );
}

namespace
{
    struct RoutesAClickOnKey
    {
        explicit RoutesAClickOnKey( EventTree& tree ) : Tree( &tree )
        {
        }
        bool OnKeyPressed( KeyPressedEvent& )
        {
            auto click = ALeftClick();
            Inner      = Tree->Route( click );
            return true;
        }
        bool OnMouseButtonPressed( MouseButtonPressedEvent& )
        {
            ++Clicks;
            return true;
        }
        EventTree* Tree;
        EventReply Inner{};
        int        Clicks = 0;
    };
} // namespace

TEST( EventRouting, DeliveryMayNest )
{
    EventTree tree;
    auto      node = tree.Emplace<RoutesAClickOnKey>( tree.Root(), tree );
    tree.SetFocus( node.Id );
    tree.SetHovered( node.Id );

    auto             key   = APressedKey();
    const EventReply outer = tree.Route( key );

    EXPECT_TRUE( outer.Handled );
    EXPECT_TRUE( node.Object.Inner.Handled );
    EXPECT_EQ( node.Object.Clicks, 1 );
}

TEST( EventRouting, DeliveryCostIsMeasured )
{
    constexpr int kDepth          = 16;
    constexpr int kKeys           = 1'000'000;
    constexpr int kBroadcastNodes = 256;
    constexpr int kResizes        = 10'000;

    EventTree   tree;
    auto        counter = tree.Emplace<Counter>( tree.Root() );
    EventNodeId deepest = counter.Id;
    for ( int i = 1; i < kDepth; ++i )
        deepest = tree.Emplace<Silent>( deepest ).Id;
    for ( int i = static_cast<int>( tree.Size() ); i < kBroadcastNodes; ++i )
        tree.Emplace<Counter>( tree.Root() );
    tree.SetFocus( deepest );

    using Clock         = std::chrono::steady_clock;
    const auto keyStart = Clock::now();
    for ( int i = 0; i < kKeys; ++i )
    {
        auto key = APressedKey();
        tree.Route( key );
    }
    const double keyNs = std::chrono::duration<double, std::nano>( Clock::now() - keyStart ).count() / kKeys;

    const auto resizeStart = Clock::now();
    for ( int i = 0; i < kResizes; ++i )
    {
        EventWindowResize resize( 1, 1 );
        tree.Route( resize );
    }
    const double resizeNs =
         std::chrono::duration<double, std::nano>( Clock::now() - resizeStart ).count() / kResizes;

    EXPECT_EQ( counter.Object.Keys, kKeys );
    EXPECT_EQ( counter.Object.Resizes, kResizes );
    std::printf(
         "MEASURED EventRouting: focus route depth %d = %.1f ns/event; broadcast over %zu nodes = %.1f ns/event "
         "(%.2f ns/node)\n",
         kDepth, keyNs, tree.Size(), resizeNs, resizeNs / static_cast<double>( tree.Size() ) );
}

namespace
{
    struct HearsKeys
    {
        bool OnKeyPressed( KeyPressedEvent& )
        {
            ++Keys;
            return false;
        }
        int Keys = 0;
    };

    struct AlsoHearsTheMouse : HearsKeys
    {
        bool OnMouseMoved( MouseMovedEvent& )
        {
            ++Moves;
            return false;
        }
        int Moves = 0;
    };

    struct NamesTheWrongEvent
    {
        bool OnKeyTyped( KeyTypedEvent& )
        {
            ++Typed;
            return true;
        }
        bool OnMouseMoved( MouseScrolledEvent& )
        {
            ++Misnamed;
            return true;
        }
        int Typed    = 0;
        int Misnamed = 0;
    };
} // namespace

TEST( EventRouting, ADerivedClassAddingItsOwnHandlerKeepsTheHandlersOfItsBase )
{
    static_assert( HandlesEvent<AlsoHearsTheMouse, KeyPressedEvent> );
    static_assert( HandlesEvent<AlsoHearsTheMouse, MouseMovedEvent> );

    EventTree tree;
    auto      node = tree.Emplace<AlsoHearsTheMouse>( tree.Root() );
    tree.SetFocus( node.Id );
    tree.SetHovered( node.Id );

    auto            key = APressedKey();
    MouseMovedEvent moved( 3.0f, 4.0f );
    tree.Route( key );
    tree.Route( moved );

    EXPECT_EQ( node.Object.Keys, 1 );
    EXPECT_EQ( node.Object.Moves, 1 );
}

TEST( EventRouting, AMethodNamedForAnotherEventDoesNotFireOnThisOne )
{
    static_assert( HandlesEvent<NamesTheWrongEvent, KeyTypedEvent> );
    static_assert( !HandlesEvent<NamesTheWrongEvent, KeyPressedEvent> );
    static_assert( !HandlesEvent<NamesTheWrongEvent, MouseMovedEvent> );
    static_assert( !HandlesEvent<NamesTheWrongEvent, MouseScrolledEvent> );

    EventTree tree;
    auto      node = tree.Emplace<NamesTheWrongEvent>( tree.Root() );
    tree.SetFocus( node.Id );
    tree.SetHovered( node.Id );

    auto               key = APressedKey();
    MouseMovedEvent    moved( 1.0f, 1.0f );
    MouseScrolledEvent scrolled( 0.0f, 1.0f );
    EXPECT_FALSE( tree.Route( key ).Handled );
    EXPECT_FALSE( tree.Route( moved ).Handled );
    EXPECT_FALSE( tree.Route( scrolled ).Handled );
    EXPECT_EQ( node.Object.Misnamed, 0 );
    EXPECT_EQ( node.Object.Typed, 0 );

    KeyTypedEvent typed( 'x' );
    EXPECT_TRUE( tree.Route( typed ).Handled );
    EXPECT_EQ( node.Object.Typed, 1 );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

namespace
{
    struct SwapchainExtent
    {
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };

    struct WindowWithSwapchain
    {
        bool OnWindowResized( EventWindowResize& resize )
        {
            Swapchain = { resize.width, resize.height };
            return false;
        }
        SwapchainExtent Swapchain;
    };

    struct ApplicationShape
    {
        Log                 log;
        WindowWithSwapchain window;
        EventTree           tree;
        EventNodeId         windowNode = tree.Attach( tree.Root(), window );
        EventNodeId         layerNode  = tree.Emplace<Probe>( windowNode, "layer", log ).Id;
        EventNodeId         panelNode  = tree.Emplace<Probe>( layerNode, "panel", log ).Id;
    };

    struct Lives
    {
        bool* Alive;
        explicit Lives( bool& alive ) : Alive( &alive )
        {
            alive = true;
        }
        ~Lives()
        {
            *Alive = false;
        }
        bool OnKeyPressed( KeyPressedEvent& )
        {
            return true;
        }
    };
} // namespace

TEST( EventRouting, AWindowResizeReachesTheSwapchainNodeWhileAPanelHoldsFocusAndClaimsIt )
{
    ApplicationShape app;
    app.tree.SetFocus( app.panelNode );
    app.tree.SetHovered( app.panelNode );

    EventWindowResize resize( 1280, 720 );
    const EventReply  reply = app.tree.Route( resize );

    EXPECT_TRUE( reply.Handled );
    EXPECT_EQ( app.window.Swapchain.Width, 1280u );
    EXPECT_EQ( app.window.Swapchain.Height, 720u );
    EXPECT_EQ( std::count( app.log.begin(), app.log.end(), "panel.resize" ), 1 );
    EXPECT_EQ( std::count( app.log.begin(), app.log.end(), "layer.resize" ), 1 );
}

TEST( EventRouting, AKeyWithTheFocusInAPanelBubblesThroughItsLayerAndWindow )
{
    ApplicationShape app;
    app.tree.SetFocus( app.panelNode );

    auto key = APressedKey();
    app.tree.Route( key );

    const Log expected = { "layer.previewKey", "panel.previewKey", "panel.key", "layer.key" };
    EXPECT_EQ( app.log, expected );
}

TEST( EventRouting, AnAttachedObjectBelongsToItsOwnerNotToTheTree )
{
    bool alive = false;
    {
        Lives       object( alive );
        EventTree   tree;
        EventNodeId node = tree.Attach( tree.Root(), object );
        tree.SetFocus( node );

        auto key = APressedKey();
        EXPECT_TRUE( tree.Route( key ).Handled );
        EXPECT_TRUE( tree.Remove( node ) );
        EXPECT_TRUE( alive );
        tree.Attach( tree.Root(), object );
    }
    EXPECT_FALSE( alive );
}

TEST( EventRouting, ALinkTakesItsNodeOutOfTheTreeWhenItsHolderDies )
{
    bool      alive = false;
    EventTree tree;
    {
        Lives         object( alive );
        EventNodeLink link( tree, tree.Attach( tree.Root(), object ) );
        tree.SetFocus( link.Id() );
        EXPECT_TRUE( tree.Contains( link.Id() ) );
    }
    auto key = APressedKey();
    EXPECT_FALSE( tree.Route( key ).Handled );
    EXPECT_EQ( tree.Size(), 0u );
}

TEST( EventRouting, ALinkWhoseNodeWentWithItsParentReleasesNothingElse )
{
    ApplicationShape app;
    bool             alive = false;
    Lives            object( alive );
    EventNodeId      stranger = app.tree.Attach( app.windowNode, object );
    {
        EventNodeLink link( app.tree, app.tree.Attach( app.panelNode, object ) );
        EXPECT_TRUE( app.tree.Remove( app.layerNode ) );
        EventNodeId reused = app.tree.Attach( app.windowNode, object );
        EXPECT_TRUE( app.tree.Contains( reused ) );
        EXPECT_FALSE( app.tree.Contains( link.Id() ) );
    }
    EXPECT_TRUE( app.tree.Contains( stranger ) );
    EXPECT_TRUE( alive );
}

TEST( EventRouting, AMovedLinkIsReleasedOnceByItsLastHolder )
{
    EventTree     tree;
    bool          alive = false;
    Lives         object( alive );
    EventNodeLink first( tree, tree.Attach( tree.Root(), object ) );
    const auto    id = first.Id();
    EventNodeLink second( std::move( first ) );
    EXPECT_FALSE( first.Id().IsSet() );
    EXPECT_TRUE( tree.Contains( id ) );
    second.Release();
    EXPECT_FALSE( tree.Contains( id ) );
}

TEST( EventRouting, ADeferredPointerEventReachesTheNodeHoveredAtTheFrameBoundary )
{
    PanelOverBackground f;
    f.panelProbe->ClaimsClick = true;
    auto                other = f.tree.Emplace<Probe>( f.tree.Root(), "other", f.log );
    f.tree.SetHovered( other.Id );

    f.tree.Defer( ALeftClick() );
    EXPECT_TRUE( f.log.empty() );
    EXPECT_EQ( f.tree.DeferredCount(), 1u );

    f.tree.SetHovered( f.panel );
    f.tree.RouteDeferred();

    const Log expected = { "panel.click" };
    EXPECT_EQ( f.log, expected );
    EXPECT_EQ( f.tree.DeferredCount(), 0u );
    f.tree.RouteDeferred();
    EXPECT_EQ( f.log, expected );
}

namespace
{
    class HidesItsHandler
    {
        bool OnKeyPressed( KeyPressedEvent& )
        {
            return true;
        }
    };
} // namespace

TEST( EventRouting, AHandlerTheTreeCannotReachIsNoHandler )
{
    static_assert( !HandlesEvent<HidesItsHandler, KeyPressedEvent> );
    static_assert( !ReceivesEvents<HidesItsHandler> );
    SUCCEED();
}
