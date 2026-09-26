// "A pointer event belongs to a CHAIN, not to an element."
//
// The defect this closes, measured on 2026-09-09: UIPointerEvents fired on the single elected hot element
// and on nothing else. The walk descended into children, elected the topmost, and delivered the press
// there, so a panel that wanted to react to a click anywhere inside it had to carry a copy of the
// component on every leaf, and "this child handled it, the panel behind must not" could not be said at
// all -- there was nothing to stop, because nothing propagated.
//
// Every assertion here is therefore about the RELATION between an ancestor and a descendant, and each is
// written so that removing the propagation turns it red rather than merely changing a number. The
// fixture nests four levels on purpose: canvas > outer > inner > two leaves. Two leaves, because the one
// behaviour that a naive "bubble everything" implementation gets wrong is the pointer moving between two
// children of one parent, and a single leaf cannot show it.
//
// The seam is the public one: RenderCanvas2D takes a plain entt::registry, the canvas to draw, a DrawList2D
// and a UIInput, so the pointer is synthesised rather than injected, and what the canvas fired is read back
// out of the outMessages vector the runtime host already passes.

#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <Common/Json/Document.hpp>

namespace
{
    // DeserializeReflected reads a Json::Node and collects wrong-typed values as Issues; every fixture this
    // file feeds it is well-typed, so an Issue is a failure here.
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Value& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        Common::Json::Issues issues;
        Desert::Reflection::DeserializeReflected( type, obj, Common::Json::Root( src ), issues, resolver );
        for ( const auto& issue : issues )
            ADD_FAILURE() << Common::Json::Describe( issue );
    }
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Object& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        ReadReflectedValue( type, obj, Common::Json::Value( src ), resolver );
    }
} // namespace

// The renderer resolves sprites, fonts, icons and video through these. Every one of them owns GPU objects,
// and every draw helper already copes with the service being absent -- a sprite that will not resolve falls
// back to its flat colour, text and icons draw nothing. That is exactly the path a headless walk wants, so
// the suite supplies the accessors itself and returns nothing. The service METHODS below can then never
// run; each fails outright rather than returning a plausible value, so a change that manages to reach one
// is a loud failure instead of a quiet stub.
namespace Desert::Runtime
{
    TextureService* ResourceRegistry::GetTextureService()
    {
        return nullptr;
    }
    ImageService* ResourceRegistry::GetImageService()
    {
        return nullptr;
    }
    FontService* ResourceRegistry::GetFontService()
    {
        return nullptr;
    }
    // Ю13's theme service. Absent like the rest, which is a MEANINGFUL state and not a hole: a canvas
    // with no theme service behind it resolves every slot from the elements' own authored fields, which
    // is exactly what a canvas with no theme does and what every scene authored before themes existed
    // does. The walk copes with the accessor being null and never dereferences it.
    UIThemeService* ResourceRegistry::GetUIThemeService()
    {
        return nullptr;
    }
    IconService* ResourceRegistry::GetIconService()
    {
        return nullptr;
    }
    AnimatedImageService* ResourceRegistry::GetAnimatedImageService()
    {
        return nullptr;
    }
    VideoService* ResourceRegistry::GetVideoService()
    {
        return nullptr;
    }

    Graphic::Texture2D* TextureService::Get( const Assets::AssetHandle& ) const
    {
        ADD_FAILURE() << "TextureService::Get reached with no texture service";
        return nullptr;
    }
    Graphic::Image* ImageService::Resolve( const ImageHandle& ) const
    {
        ADD_FAILURE() << "ImageService::Resolve reached with no image service";
        return nullptr;
    }
    Graphic::Image2D* AnimatedImageService::Resolve( const Assets::AssetHandle& )
    {
        ADD_FAILURE() << "AnimatedImageService::Resolve reached with no animated image service";
        return nullptr;
    }
    Graphic::Image2D* VideoService::Resolve( uint64_t )
    {
        ADD_FAILURE() << "VideoService::Resolve reached with no video service";
        return nullptr;
    }
    const Assets::UIThemeRuntime* UIThemeService::Get( const Assets::AssetHandle& )
    {
        ADD_FAILURE() << "UIThemeService::Get reached with no theme service";
        return nullptr;
    }
    Font* FontService::Get( uint64_t, float )
    {
        ADD_FAILURE() << "FontService::Get reached with no font service";
        return nullptr;
    }
    uint64_t FontService::DefaultFontHandle()
    {
        ADD_FAILURE() << "FontService::DefaultFontHandle reached with no font service";
        return 0;
    }
    bool FontService::RequestGlyphs( uint64_t, const std::vector<uint32_t>& )
    {
        ADD_FAILURE() << "FontService::RequestGlyphs reached with no font service";
        return false;
    }
    Icon* IconService::Get( uint64_t )
    {
        ADD_FAILURE() << "IconService::Get reached with no icon service";
        return nullptr;
    }
} // namespace Desert::Runtime

using Desert::UI::Rect;
using Desert::UI::UIInput;
using Desert::UI::UIViewContext;
namespace ECS = Desert::ECS;
namespace R2D = Desert::Graphic::Render2D;

namespace
{
    constexpr float kSide = 1000.0f; // Stretch at 1000x1000, so design px == screen px (scale 1)

    const Rect kViewport{ 0.0f, 0.0f, kSide, kSide };

    // canvas
    //   outer   (0,0)-(400,400)
    //     inner (0,0)-(300,300)
    //       leafA (0,0)-(100,100)
    //       leafB (150,0)-(250,100)
    //
    // Nested so that one screen point is inside four elements at once, which is the only arrangement in
    // which "who heard it" has more than one possible answer. The two leaves are siblings and disjoint.
    struct Tree
    {
        entt::registry Registry;
        entt::entity   Canvas = entt::null;
        entt::entity   Outer  = entt::null;
        entt::entity   Inner  = entt::null;
        entt::entity   LeafA  = entt::null;
        entt::entity   LeafB  = entt::null;

        Tree()
        {
            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;

            // Every entity is created and given its components BEFORE any of them is linked up: holding a
            // reference into a component pool across a later emplace into that same pool is a dangling one.
            Outer = Make( { 0.0f, 0.0f }, { 400.0f, 400.0f } );
            Inner = Make( { 0.0f, 0.0f }, { 300.0f, 300.0f } );
            LeafA = Make( { 0.0f, 0.0f }, { 100.0f, 100.0f } );
            LeafB = Make( { 150.0f, 0.0f }, { 250.0f, 100.0f } );

            Link( Canvas, Outer );
            Link( Outer, Inner );
            Link( Inner, LeafA );
            Link( Inner, LeafB );
        }

        entt::entity Make( glm::vec2 offMin, glm::vec2 offMax )
        {
            const entt::entity e = Registry.create();
            auto&              l = Registry.emplace<ECS::UILayoutComponent>( e ).Data;
            l.AnchorMin          = { 0.0f, 0.0f };
            l.AnchorMax          = { 0.0f, 0.0f };
            l.OffsetMin          = offMin;
            l.OffsetMax          = offMax;
            Registry.emplace<ECS::UIPanelComponent>( e );
            return e;
        }

        void Link( entt::entity parent, entt::entity child )
        {
            if ( !Registry.has<ECS::RelationshipComponent>( parent ) )
                Registry.emplace<ECS::RelationshipComponent>( parent );
            if ( !Registry.has<ECS::RelationshipComponent>( child ) )
                Registry.emplace<ECS::RelationshipComponent>( child );
            Registry.get<ECS::RelationshipComponent>( parent ).Children.push_back( child );
            Registry.get<ECS::RelationshipComponent>( child ).Parent = parent;
        }

        // Give @p e a listener that says @p name on every edge, so a test never has to decide which edge
        // it is asserting about twice.
        ECS::UIPointerEventsData& Listen( entt::entity e, const std::string& name )
        {
            auto& ev          = Registry.emplace<ECS::UIPointerEventsComponent>( e ).Data;
            ev.OnEnterMessage = name + ":enter";
            ev.OnExitMessage  = name + ":exit";
            ev.OnDownMessage  = name + ":down";
            ev.OnUpMessage    = name + ":up";
            return ev;
        }

        void SetHitTest( entt::entity e, ECS::UIHitTest h )
        {
            Registry.get<ECS::UILayoutComponent>( e ).Data.HitTest = h;
        }
    };

    // One walk with a synthesised pointer; hands back every message the canvas fired, IN ORDER. Order is
    // the point: a set would pass on a build that ran the chain backwards.
    std::vector<std::string> Frame( Tree& t, UIViewContext& ctx, glm::vec2 mouse, bool down = false,
                                    bool released = false )
    {
        UIInput input;
        input.MousePx       = mouse;
        input.MouseDown     = down;
        input.MouseReleased = released;

        R2D::DrawList2D          dl;
        std::vector<std::string> out;
        Desert::UI::BeginUIFrame( ctx, t.Registry, kViewport );
        const auto drawn = Desert::UI::RenderCanvas2D( ctx, t.Registry, t.Canvas, dl, nullptr, &input );
        EXPECT_TRUE( drawn.IsSuccess() ) << drawn.GetError();
        // The routing is the VIEW's, not the walk's: it runs once per frame over the election every canvas
        // of that frame contributed to. This suite draws one canvas, so a frame is Begin / one walk / End.
        Desert::UI::EndUIFrame( ctx, t.Registry, dl, &input, nullptr, nullptr, &out );
        return out;
    }

    // The hot election is resolved one frame late by design (the same deferral ImGui uses), so the pointer
    // is walked to @p mouse and left there for a frame before the button goes down. Without the settling
    // frames the press frame ALSO carries the whole Enter chain, and a test that asserted the messages of
    // that frame would be asserting two different things at once.
    std::vector<std::string> Press( Tree& t, UIViewContext& ctx, glm::vec2 mouse )
    {
        Frame( t, ctx, mouse );
        Frame( t, ctx, mouse );
        return Frame( t, ctx, mouse, /*down=*/true );
    }

    std::vector<std::string> Release( Tree& t, UIViewContext& ctx, glm::vec2 mouse )
    {
        Frame( t, ctx, mouse );
        Frame( t, ctx, mouse );
        return Frame( t, ctx, mouse, /*down=*/false, /*released=*/true );
    }

    constexpr glm::vec2 kOnLeafA{ 50.0f, 50.0f };
    constexpr glm::vec2 kOnLeafB{ 200.0f, 50.0f };
    constexpr glm::vec2 kOnInner{ 50.0f, 200.0f };  // inside inner + outer, no leaf
    constexpr glm::vec2 kOnOuter{ 350.0f, 350.0f }; // inside outer only
    constexpr glm::vec2 kOffAll{ 900.0f, 900.0f };
} // namespace

// --- THE ROUTE ------------------------------------------------------------------------------------------
//
// The relation: a press on the innermost element is heard by EVERY ancestor that is listening, and the
// order is innermost-first. Asserting only that the outer panel heard something would pass just as happily
// on a build that delivered to the root and stopped, which is the opposite mistake.
TEST( UIEventRoute, APressOnALeafIsHeardByEveryAncestorInnermostFirst )
{
    Tree t;
    t.Listen( t.Canvas, "canvas" );
    t.Listen( t.Outer, "outer" );
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    const std::vector<std::string> expected = { "leafA:down", "inner:down", "outer:down", "canvas:down" };
    EXPECT_EQ( msgs, expected ) << "the press did not travel the chain target-first";
}

// The other pass, and the reason it exists: a Tunnel listener acts BEFORE its own children. All tunnel
// listeners run before any bubble listener, so the two passes are one route and not two independent ones --
// which is what a per-element "call the parent too" would have been.
TEST( UIEventRoute, TunnelListenersAllRunBeforeAnyBubbleListener )
{
    Tree t;
    t.Listen( t.Outer, "outer" ).Phase = ECS::UIEventPhase::Tunnel;
    t.Listen( t.Inner, "inner" ); // Bubble, the default
    t.Listen( t.LeafA, "leafA" ).Phase = ECS::UIEventPhase::Tunnel;

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    // Tunnel descends: outer before leafA. Then the bubble pass runs, and inner is all that is left.
    ASSERT_EQ( msgs.size(), 3u );
    EXPECT_EQ( msgs[0], "outer:down" );
    EXPECT_EQ( msgs[1], "leafA:down" );
    EXPECT_EQ( msgs[2], "inner:down" );
}

// The half of propagation that is about STOPPING it. Without this the route is a broadcast and "the child
// handled it" cannot be said -- which is the older, worse half of the same defect.
TEST( UIEventRoute, StopPropagationOnTheTargetEndsTheRouteAndTheAncestorsHearNothing )
{
    Tree t;
    t.Listen( t.Canvas, "canvas" );
    t.Listen( t.Outer, "outer" );
    t.Listen( t.LeafA, "leafA" ).StopPropagation = true;

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    ASSERT_EQ( msgs.size(), 1u ) << "propagation continued past a listener that stopped it";
    EXPECT_EQ( msgs[0], "leafA:down" );
}

// Tunnel + StopPropagation together, which is the combination neither field can express alone: an ancestor
// that takes every press inside itself and whose children never see one. This is the shape a modal dialog
// or a click-outside-to-close scrim needs, and UIHitTest::Blocking cannot be it -- Blocking responds to
// nothing, so it cannot report the click it swallowed.
TEST( UIEventRoute, ATunnellingAncestorThatStopsTakesThePressAndItsChildrenNeverSeeOne )
{
    Tree  t;
    auto& outer           = t.Listen( t.Outer, "outer" );
    outer.Phase           = ECS::UIEventPhase::Tunnel;
    outer.StopPropagation = true;
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    ASSERT_EQ( msgs.size(), 1u );
    EXPECT_EQ( msgs[0], "outer:down" );
}

// Release travels the same route as press. Written separately because they are two call sites, and a
// change that routes one and forgets the other is exactly the middle-link defect this project keeps
// finding: both ends look right and the link between them drops something.
TEST( UIEventRoute, ReleaseTravelsTheSameChainAsPress )
{
    Tree t;
    t.Listen( t.Outer, "outer" );
    t.Listen( t.LeafA, "leafA" );

    UIViewContext   ctx;
    const auto      msgs = Release( t, ctx, kOnLeafA );

    const std::vector<std::string> expected = { "leafA:up", "outer:up" };
    EXPECT_EQ( msgs, expected );
}

// --- THE ROUTE OBEYS THE HIT-TEST AXIS (task У4) --------------------------------------------------------
//
// Two fields that must agree rather than each being right on its own. ChildrenOnly says the pointer does
// not see THIS element; the route must therefore skip it -- and must NOT skip what is above it, because
// transparent is not opaque. Both halves in one assertion, because a build that skipped the whole rest of
// the chain would satisfy either half alone.
TEST( UIEventRouteMeetsHitTest, AChildrenOnlyAncestorIsSkippedAndTheOneAboveItStillHears )
{
    Tree t;
    t.Listen( t.Canvas, "canvas" );
    t.Listen( t.Outer, "outer" );
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );
    t.SetHitTest( t.Inner, ECS::UIHitTest::ChildrenOnly );

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    const std::vector<std::string> expected = { "leafA:down", "outer:down", "canvas:down" };
    EXPECT_EQ( msgs, expected ) << "a ChildrenOnly ancestor either heard the press or swallowed the chain";
}

// Blocking stops the pointer, and a routed press IS the pointer. The element itself responds to nothing --
// that is У4's definition -- and neither does anything above it, or "blocking" would describe the hit test
// and not the event the hit test produced.
TEST( UIEventRouteMeetsHitTest, ABlockingTargetSwallowsThePressForItsAncestorsToo )
{
    Tree t;
    t.Listen( t.Canvas, "canvas" );
    t.Listen( t.Outer, "outer" );
    t.Listen( t.LeafA, "leafA" );
    t.SetHitTest( t.LeafA, ECS::UIHitTest::Blocking );

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    EXPECT_TRUE( msgs.empty() ) << "a Blocking element let a press through to " << msgs.size() << " listener(s)";
}

// The sub-tree half of the same axis: None makes the element and everything under it transparent, so the
// leaf is never elected and there is no route to run at all. This is the combination У4 called unreachable,
// asserted here from the event side rather than the drawing side.
TEST( UIEventRouteMeetsHitTest, NothingUnderANoneAncestorCanEvenStartARoute )
{
    Tree t;
    t.Listen( t.Outer, "outer" );
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );
    t.SetHitTest( t.Outer, ECS::UIHitTest::None );

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    EXPECT_TRUE( msgs.empty() ) << "a press reached a sub-tree that is transparent to the pointer";
}

// --- ENTER AND EXIT ARE A CHAIN DIFFERENCE, NOT A ROUTE -------------------------------------------------
//
// THE DEFECT THIS PREVENTS, and the reason Enter/Exit are not simply bubbled like a press: the pointer
// moving from one child of a panel to its sibling never leaves the panel, so the panel must hear nothing.
// Bubbling would fire its Exit and then its Enter on every such move, and a parent that lights up on hover
// would flicker on every internal movement. DOM draws the same line (mouseenter/mouseleave do not bubble)
// and Slate walks the difference of the two widget paths.
//
// The relation is between the two chains, and it is one subtraction: what they share is what did not
// happen.
TEST( UIEventHover, MovingBetweenTwoChildrenOfOnePanelSaysNothingAboutThePanel )
{
    Tree t;
    t.Listen( t.Canvas, "canvas" );
    t.Listen( t.Outer, "outer" );
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );
    t.Listen( t.LeafB, "leafB" );

    UIViewContext ctx;
    Frame( t, ctx, kOnLeafA ); // settle the election: the first frame has nothing to compare against
    Frame( t, ctx, kOnLeafA );
    const auto msgs = Frame( t, ctx, kOnLeafB );

    const std::vector<std::string> expected = { "leafA:exit", "leafB:enter" };
    EXPECT_EQ( msgs, expected ) << "the shared ancestors were told about a boundary the pointer never "
                                   "crossed";
}

// The other direction of the same rule, and the one that proves the ancestors are on the chain at all: a
// pointer arriving from OUTSIDE crosses every boundary, so every ancestor hears Enter -- outermost first,
// because that is the order the boundaries are crossed in.
TEST( UIEventHover, ArrivingFromOutsideEntersEveryAncestorOutermostFirst )
{
    Tree t;
    t.Listen( t.Outer, "outer" );
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );

    UIViewContext ctx;
    Frame( t, ctx, kOffAll );
    Frame( t, ctx, kOffAll );
    const auto msgs = Frame( t, ctx, kOnLeafA );

    const std::vector<std::string> expected = { "outer:enter", "inner:enter", "leafA:enter" };
    EXPECT_EQ( msgs, expected );
}

// And leaving is the mirror: innermost first, because the innermost boundary is the one crossed first.
// Kept as its own case because "reverse the exit loop" is a mutation that the enter case cannot see.
TEST( UIEventHover, LeavingForAnAncestorExitsOnlyTheBranchThePointerLeft )
{
    Tree t;
    t.Listen( t.Outer, "outer" );
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );

    UIViewContext ctx;
    Frame( t, ctx, kOnLeafA );
    Frame( t, ctx, kOnLeafA );
    const auto toInner = Frame( t, ctx, kOnInner ); // still inside inner and outer

    const std::vector<std::string> expectedInner = { "leafA:exit" };
    EXPECT_EQ( toInner, expectedInner ) << "leaving a leaf claimed to leave its ancestors too";

    Frame( t, ctx, kOnInner );
    const auto toOuter = Frame( t, ctx, kOnOuter ); // now outside inner, still inside outer

    const std::vector<std::string> expectedOuter = { "inner:exit" };
    EXPECT_EQ( toOuter, expectedOuter );
}

// Enter/Exit obey the same per-element hit-test bit the route does: a ChildrenOnly element is transparent
// to the pointer, so it is not told the pointer arrived. Its ancestors and descendants are unaffected,
// which is what makes this a statement about one element rather than about the chain.
TEST( UIEventHover, AChildrenOnlyElementIsNotToldThePointerArrived )
{
    Tree t;
    t.Listen( t.Outer, "outer" );
    t.Listen( t.Inner, "inner" );
    t.Listen( t.LeafA, "leafA" );
    t.SetHitTest( t.Inner, ECS::UIHitTest::ChildrenOnly );

    UIViewContext ctx;
    Frame( t, ctx, kOffAll );
    Frame( t, ctx, kOffAll );
    const auto msgs = Frame( t, ctx, kOnLeafA );

    const std::vector<std::string> expected = { "outer:enter", "leafA:enter" };
    EXPECT_EQ( msgs, expected );
}

// --- WHAT THE DEFAULTS PROMISE --------------------------------------------------------------------------
//
// A component authored before this change carries neither field, so it deserializes to these values; a
// scene that behaved one way must not quietly behave another. Bubble and "do not stop" are the values that
// make an existing single listener fire exactly as it did, and this pins them rather than trusting the
// struct's initialisers to stay put.
TEST( UIEventRoute, TheDefaultsAreBubbleAndDoNotStop )
{
    const ECS::UIPointerEventsData fresh;
    EXPECT_EQ( fresh.Phase, ECS::UIEventPhase::Bubble );
    EXPECT_FALSE( fresh.StopPropagation );

    // And the behaviour that follows from them: one listener, on the element the pointer is over, fires --
    // which is everything the old single-element delivery did.
    Tree t;
    t.Listen( t.LeafA, "leafA" );

    UIViewContext   ctx;
    const auto      msgs = Press( t, ctx, kOnLeafA );

    const std::vector<std::string> expected = { "leafA:down" };
    EXPECT_EQ( msgs, expected );
}

// --- THE RELOAD -----------------------------------------------------------------------------------------
//
// A knob that is authored and then silently dropped on save is a defect this tree has paid for before:
// VisibilityComponent had no entry in the serialization table at all, so the outliner's eye worked for
// exactly as long as the process lived. These two fields go through ComponentRegistry's MakeReflected,
// which calls straight into SerializeReflected / DeserializeReflected on the SAME TypeInfo asserted here,
// so this is the round trip the scene file takes and not a model of it.
TEST( UIEventPersistence, PhaseAndStopPropagationSurviveTheRoundTrip )
{
    using namespace Desert::Reflection;

    const TypeInfo* type = ReflectionRegistry::Get().Find( "UIPointerEventsData" );
    ASSERT_NE( type, nullptr ) << "UIPointerEventsData is not reflected, so it is not serialized either";

    ECS::UIPointerEventsData authored;
    authored.OnDownMessage   = "open:settings";
    authored.Phase           = ECS::UIEventPhase::Tunnel;
    authored.StopPropagation = true;

    const Common::Json::Object written = SerializeReflected( *type, &authored, nullptr );

    ECS::UIPointerEventsData reloaded;
    ReadReflectedValue( *type, &reloaded, written, nullptr );

    EXPECT_EQ( reloaded.OnDownMessage, "open:settings" );
    EXPECT_EQ( reloaded.Phase, ECS::UIEventPhase::Tunnel ) << "the phase did not survive a save and load";
    EXPECT_TRUE( reloaded.StopPropagation ) << "StopPropagation did not survive a save and load";
}

// THE OTHER DIRECTION: no scene in the repository states these keys, so every listener that ever loads is
// a listener authored before they existed, and it must come back Bubble-and-do-not-stop. That is the whole
// reason no scene migration ships with this change, and it is asserted rather than assumed.
TEST( UIEventPersistence, AListenerSavedBeforeTheseFieldsExistedComesBackWithTheDefaults )
{
    using namespace Desert::Reflection;

    const TypeInfo* type = ReflectionRegistry::Get().Find( "UIPointerEventsData" );
    ASSERT_NE( type, nullptr );

    Common::Json::Object old; // exactly what a pre-change scene file holds for this component
    old["OnDownMessage"] = Common::Json::Value( std::string( "open:settings" ) );

    // Default-constructed, because that is what the load path hands the deserializer: ComponentRegistry's
    // MakeReflected deserializes into `AddComponent<T>()` on an entity that does not have one yet.
    ECS::UIPointerEventsData reloaded;
    ReadReflectedValue( *type, &reloaded, old, nullptr );

    EXPECT_EQ( reloaded.OnDownMessage, "open:settings" );
    EXPECT_EQ( reloaded.Phase, ECS::UIEventPhase::Bubble );
    EXPECT_FALSE( reloaded.StopPropagation );
}

// WHY THE TEST ABOVE HAD TO CONSTRUCT A FRESH STRUCT, WRITTEN DOWN BECAUSE IT IS NOT OBVIOUS AND IT BIT
// THIS TASK. An absent key does not restore the field to its default -- it leaves whatever the target
// already held. The defaults in the test above therefore hold because the LOAD PATH allocates a fresh
// component, not because the deserializer puts them there, and anything that deserializes over a live
// component keeps the live value. Pinned so that a future change to either half has to face the other.
TEST( UIEventPersistence, AnAbsentKeyLeavesTheTargetUntouchedRatherThanResettingIt )
{
    using namespace Desert::Reflection;

    const TypeInfo* type = ReflectionRegistry::Get().Find( "UIPointerEventsData" );
    ASSERT_NE( type, nullptr );

    Common::Json::Object old;
    old["OnDownMessage"] = Common::Json::Value( std::string( "open:settings" ) );

    ECS::UIPointerEventsData live;
    live.Phase           = ECS::UIEventPhase::Tunnel;
    live.StopPropagation = true;
    ReadReflectedValue( *type, &live, old, nullptr );

    EXPECT_EQ( live.Phase, ECS::UIEventPhase::Tunnel );
    EXPECT_TRUE( live.StopPropagation );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
