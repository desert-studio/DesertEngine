// "AN ELEMENT THAT DRAWS NOTHING IS INDISTINGUISHABLE FROM AN ELEMENT THAT WAS MEANT TO DRAW NOTHING"
// — and the whole of Ю16 hangs on the walk keeping that promise for a render-texture element, plus one
// relation that no frame can photograph.
//
// THE THREE THINGS THIS SUITE PINS, and each is a real mistake that a green build would not catch:
//
//   1. A REFUSAL IS VISIBLE. A backend that cannot hand over a world — no free renderer slot, a file that
//      does not exist, no backend wired at all — makes the element draw the MAGENTA error fill. The easy
//      and wrong implementation is `if ( world ) AddImage( ... );`, which compiles, ships, and turns a
//      six-slot shortage into a rect that looks exactly like a rect nobody finished authoring. §1.4
//      forbids it and IUIMaterialSource says so in prose; here it is as an assertion.
//
//   2. ASKING IS THE DEMAND, AND NOT ASKING IS THE RELEASE. A renderer slot comes back by DESTROYING the
//      capture that holds it and by nothing else, so the backend has to learn that an element left the
//      screen. It learns it from the walk NOT asking. That makes "the walk does not ask about an element
//      it does not draw" a load-bearing property of the slot budget rather than an optimisation — if a
//      hidden element is still asked about, six elements on a canvas hold six slots forever and the
//      seventh can never build. No frame can show this: the refusal it produces needs a sustained
//      six-of-six, which is the same thing the ViewBudget suite cannot photograph either.
//
//   3. THE SIZE IS THE ELEMENT'S, WITH ResolutionScale ON IT. The backend allocates a target from this
//      number and cannot recompute it — anchors, canvas scale and layout groups are the walk's business.
//      A request carrying the design size instead of the screen size, or dropping ResolutionScale, is a
//      knob that moves nothing and a target that is the wrong size, and both are silent.
//
// The suite is DEVICE-FREE, exactly like UICanvasContext next door: RenderCanvas2D takes a plain
// entt::registry and a DrawList2D, the source is an interface, and the stub below answers it. That is the
// purity UIRenderTextureSource.hpp exists to protect — naming the concrete Render2D cache here would drag
// a Vulkan device, a Core::Scene and a SceneRenderer into a test about six comparisons.

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIRenderTextureSource.hpp>
#include <Engine/Graphic/Render2D/UIRenderTextureView.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

// The renderer resolves sprites, fonts, icons and video through these, and every one owns GPU objects.
// Every draw helper already copes with the service being absent, which is exactly the path a headless
// walk wants — see UICanvasContext's suite for the longer version of this argument.
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

    // THE ANALYSER WANTS THESE STATIC AND THEY CANNOT BE. Each is an out-of-line definition of a method
    // the ENGINE declared; changing its signature would stop it being that method and the link would fail
    // — which is the whole point of defining them here. Suppressed by name and with the reason, in the
    // narrowest block that covers them, rather than by widening the project's gate.
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    //
    // The methods the walk would call on what those accessors hand back. None can run — every accessor
    // above is null — so each fails outright rather than returning a plausible value, and a change that
    // manages to reach one is loud instead of quiet.
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
        ADD_FAILURE() << "AnimatedImageService::Resolve reached with no animated-image service";
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
    // NOLINTEND(readability-convert-member-functions-to-static)
} // namespace Desert::Runtime

namespace ECS = Desert::ECS;
namespace R2D = Desert::Graphic::Render2D;

using Desert::UI::Rect;
using Desert::UI::UIRenderTextureRequest;
using Desert::UI::UIViewContext;

namespace
{
    constexpr float kSide = 1000.0f; // Stretch at 1000x1000, so design px == screen px (scale 1)

    const Rect kViewport{ 0.0f, 0.0f, kSide, kSide };

    // Never dereferenced: a draw list treats a texture as an OPAQUE id and stores the pointer. Taken as
    // an address so it is a real, unique object rather than a made-up number that could collide with a
    // null check somewhere.
    int         g_WorldStorage = 0;
    const void* FakeWorld()
    {
        return &g_WorldStorage;
    }

    // What a backend was asked, recorded. The suite asserts about the QUESTION as much as the answer,
    // because the question is where the element's size and its ResolutionScale meet.
    struct Ask
    {
        entt::entity Element = entt::null;
        std::string  ScenePath;
        uint32_t     Width  = 0;
        uint32_t     Height = 0;
    };

    // A backend that answers whatever the test tells it to, and remembers being asked.
    class StubSource final : public Desert::UI::IUIRenderTextureSource
    {
    public:
        explicit StubSource( bool answers ) : m_Answers( answers )
        {
        }

        const void* ResolveRenderTexture( entt::entity element, const UIRenderTextureRequest& request ) override
        {
            Asks.push_back( Ask{ .Element   = element,
                                 .ScenePath = std::string( request.ScenePath ),
                                 .Width     = request.WidthPx,
                                 .Height    = request.HeightPx } );
            return m_Answers ? FakeWorld() : nullptr;
        }

        std::vector<Ask> Asks;

    private:
        bool m_Answers;
    };

    // A canvas with one render-texture element at (100,100)-(500,400) — 400x300 px.
    struct Fixture
    {
        // DECLARATION ORDER IS THE CONSTRUCTION ORDER, and both entities come out of the registry, so the
        // registry has to be the first member. Initialised in the list rather than in the body because
        // `entt::null` followed by an assignment is two values for one identity.
        entt::registry Registry;
        entt::entity   Canvas  = Registry.create();
        entt::entity   Element = Registry.create();

        explicit Fixture( const char* scenePath = "Resources/Assets/Scenes/W.desce" )
        {
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;

            auto& layout     = Registry.emplace<ECS::UILayoutComponent>( Element ).Data;
            layout.AnchorMin = { 0.0f, 0.0f };
            layout.AnchorMax = { 0.0f, 0.0f };
            layout.OffsetMin = { 100.0f, 100.0f };
            layout.OffsetMax = { 500.0f, 400.0f };

            auto& rt     = Registry.emplace<ECS::UIRenderTextureComponent>( Element ).Data;
            rt.ScenePath = scenePath;

            Registry.emplace<ECS::RelationshipComponent>( Canvas ).Children.push_back( Element );
            Registry.emplace<ECS::RelationshipComponent>( Element ).Parent = Canvas;
        }

        ECS::UIRenderTextureData& Data()
        {
            return Registry.get<ECS::UIRenderTextureComponent>( Element ).Data;
        }
        ECS::UILayoutData& Layout()
        {
            return Registry.get<ECS::UILayoutComponent>( Element ).Data;
        }
    };

    // One whole frame of a view: the walk refuses outside a Begin/End pair (Ю4), and a test that called
    // RenderCanvas2D bare would assert about an empty list and pass for the wrong reason.
    void Draw( UIViewContext& ctx, Fixture& f, R2D::DrawList2D& dl )
    {
        Desert::UI::BeginUIFrame( ctx, f.Registry, kViewport );
        const auto drawn = Desert::UI::RenderCanvas2D( ctx, f.Registry, f.Canvas, dl );
        EXPECT_TRUE( drawn.IsSuccess() ) << drawn.GetError();
        Desert::UI::EndUIFrame( ctx, f.Registry, dl, /*input=*/nullptr );
    }

    // Is there a batch bound to @p texture? A render-texture element draws one image quad, so this is
    // "did the world reach the draw list".
    bool DrewTexture( const R2D::DrawList2D& dl, const void* texture )
    {
        return std::any_of( dl.GetCommands().begin(), dl.GetCommands().end(),
                            [texture]( const R2D::DrawCommand& cmd ) { return cmd.Texture == texture; } );
    }

    // Is there a SOLID (untextured) batch whose vertices are magenta? The error fill is an
    // AddRectFilled, so it lands in a null-texture command with (1,0,1,1) vertices.
    bool DrewMagenta( const R2D::DrawList2D& dl )
    {
        for ( const auto& cmd : dl.GetCommands() )
        {
            if ( cmd.Texture != nullptr || cmd.Text )
            {
                continue;
            }
            for ( uint32_t i = 0; i < cmd.IndexCount; ++i )
            {
                const auto& v = dl.GetVertices()[dl.GetIndices()[cmd.IndexOffset + i]];
                if ( v.Color.r > 0.99f && v.Color.g < 0.01f && v.Color.b > 0.99f && v.Color.a > 0.99f )
                {
                    return true;
                }
            }
        }
        return false;
    }
} // namespace

// --- 1. THE ANSWER REACHES THE PICTURE -------------------------------------------------------------

TEST( UIRenderTexture, AnAnsweredElementDrawsTheWorldItWasGiven )
{
    Fixture       f;
    StubSource    source( /*answers=*/true );
    UIViewContext ctx;
    ctx.RenderTextures = &source;

    R2D::DrawList2D dl;
    Draw( ctx, f, dl );

    EXPECT_TRUE( DrewTexture( dl, FakeWorld() ) )
         << "the backend handed over a world and the element did not draw it";
    EXPECT_FALSE( DrewMagenta( dl ) ) << "an element that WAS given a world drew the error fill as well";
}

// --- 2. A REFUSAL IS VISIBLE, and it is visible for every reason a backend can have -----------------

TEST( UIRenderTexture, ARefusedElementDrawsMagentaRatherThanNothing )
{
    Fixture       f;
    StubSource    source( /*answers=*/false );
    UIViewContext ctx;
    ctx.RenderTextures = &source;

    R2D::DrawList2D dl;
    Draw( ctx, f, dl );

    EXPECT_FALSE( dl.GetVertices().empty() )
         << "a refused render-texture element drew NOTHING AT ALL. That is exactly the failure this "
            "engine's contract forbids: it is indistinguishable from an element nobody finished "
            "authoring, and the person looking at it has no way to learn that six renderer slots were in "
            "use. It must draw the magenta error fill.";
    EXPECT_TRUE( DrewMagenta( dl ) ) << "a refused element drew something, but not the magenta error fill";
    EXPECT_FALSE( DrewTexture( dl, FakeWorld() ) ) << "a refused element drew a world anyway";
}

TEST( UIRenderTexture, AnElementWithNoBackendAtAllAlsoDrawsMagenta )
{
    // A host that never wired a source — a unit test, or the UI Editor panel before somebody remembers.
    // The element's entire job is a picture, so unlike a material (which falls back to the element's own
    // fill) there is nothing to fall back TO, and silence here would be the same defect one level up.
    Fixture       f;
    UIViewContext ctx; // RenderTextures deliberately left null

    R2D::DrawList2D dl;
    Draw( ctx, f, dl );

    EXPECT_TRUE( DrewMagenta( dl ) )
         << "a view with no render-texture backend drew nothing for an element that names a scene";
}

// --- 3. ASKING IS THE DEMAND, AND NOT ASKING IS THE RELEASE ----------------------------------------

TEST( UIRenderTexture, AVisibleElementIsAskedAboutOncePerFrame )
{
    Fixture       f;
    StubSource    source( /*answers=*/true );
    UIViewContext ctx;
    ctx.RenderTextures = &source;

    R2D::DrawList2D dl;
    Draw( ctx, f, dl );

    ASSERT_EQ( source.Asks.size(), 1u ) << "an on-screen element must be asked about exactly once a frame";
    EXPECT_EQ( source.Asks.front().Element, f.Element );
    EXPECT_EQ( source.Asks.front().ScenePath, "Resources/Assets/Scenes/W.desce" );
}

TEST( UIRenderTexture, AHiddenElementIsNotAskedAboutAtAllAndThatIsHowItsSlotComesBack )
{
    // THE LOAD-BEARING ONE. The backend holds a renderer slot per capture and returns it by DESTROYING
    // the capture; it decides what to destroy from what the last walk did NOT ask about. So if a hidden
    // element is still asked about, its slot is held forever — six of them on a canvas would starve every
    // other surface in the process, and the refusal would be permanent and unexplainable.
    //
    // This cannot be a frame. Reaching a sustained six-of-six needs several live renderers held open at
    // once, which is the same state the ViewBudget suite argues no picture can hold.
    Fixture       f;
    StubSource    source( /*answers=*/true );
    UIViewContext ctx;
    ctx.RenderTextures = &source;

    f.Layout().Visibility = ECS::UIVisibility::Hidden;

    R2D::DrawList2D dl;
    Draw( ctx, f, dl );

    EXPECT_TRUE( source.Asks.empty() )
         << "a hidden render-texture element was still asked about, so the backend would keep its "
            "renderer slot for a rect nobody can see. Hiding an element is the only way an author has to "
            "give a slot back.";
}

TEST( UIRenderTexture, ShowingAndHidingTheSameElementTogglesTheDemand )
{
    // The RELATION, not either state alone: a walk that asked about nothing at all would pass the test
    // above, and a walk that asked about everything would pass the one before it. Both directions in one
    // view, over three frames, is what makes them two facts.
    Fixture       f;
    StubSource    source( /*answers=*/true );
    UIViewContext ctx;
    ctx.RenderTextures = &source;

    R2D::DrawList2D visible;
    Draw( ctx, f, visible );
    const std::size_t afterShown = source.Asks.size();

    f.Layout().Visibility = ECS::UIVisibility::Hidden;
    R2D::DrawList2D hidden;
    Draw( ctx, f, hidden );
    const std::size_t afterHidden = source.Asks.size();

    f.Layout().Visibility = ECS::UIVisibility::Visible;
    R2D::DrawList2D shownAgain;
    Draw( ctx, f, shownAgain );
    const std::size_t afterReshown = source.Asks.size();

    EXPECT_EQ( afterShown, 1u ) << "the first, visible frame did not demand the world";
    EXPECT_EQ( afterHidden, afterShown ) << "the hidden frame demanded it anyway — the slot never returns";
    EXPECT_EQ( afterReshown, afterHidden + 1 )
         << "the element came back on screen and was NOT demanded again, so the capture the backend "
            "destroyed while it was hidden would never be rebuilt: the rect stays magenta forever.";
}

// --- 4. THE SIZE IN THE QUESTION -------------------------------------------------------------------

TEST( UIRenderTexture, TheRequestCarriesTheElementsOwnPixelSize )
{
    Fixture       f; // 400 x 300 at scale 1
    StubSource    source( /*answers=*/true );
    UIViewContext ctx;
    ctx.RenderTextures = &source;

    R2D::DrawList2D dl;
    Draw( ctx, f, dl );

    ASSERT_EQ( source.Asks.size(), 1u );
    EXPECT_EQ( source.Asks.front().Width, 400u );
    EXPECT_EQ( source.Asks.front().Height, 300u );
}

TEST( UIRenderTexture, ResolutionScaleReachesTheRequestAndIsNotAKnobThatMovesNothing )
{
    Fixture    f;
    StubSource source( /*answers=*/true );

    f.Data().ResolutionScale = 0.5f;
    {
        UIViewContext ctx;
        ctx.RenderTextures = &source;
        R2D::DrawList2D dl;
        Draw( ctx, f, dl );
    }
    ASSERT_EQ( source.Asks.size(), 1u );
    EXPECT_EQ( source.Asks.front().Width, 200u ) << "ResolutionScale did not reach the target's size";
    EXPECT_EQ( source.Asks.front().Height, 150u );

    f.Data().ResolutionScale = 2.0f;
    {
        UIViewContext ctx;
        ctx.RenderTextures = &source;
        R2D::DrawList2D dl;
        Draw( ctx, f, dl );
    }
    ASSERT_EQ( source.Asks.size(), 2u );
    EXPECT_EQ( source.Asks.back().Width, 800u );
    EXPECT_EQ( source.Asks.back().Height, 600u );
}

TEST( UIRenderTexture, ATargetIsClampedAtBothEndsBecauseItIsARealAllocation )
{
    // An element mid-tween passes through zero width, and a full-screen element at ResolutionScale 2 on a
    // 4K display asks for more than any of them is worth. Neither is an authoring mistake — both are
    // states the frame passes through — so the walk clamps rather than refusing.
    Fixture    f;
    StubSource source( /*answers=*/true );

    f.Layout().OffsetMax     = f.Layout().OffsetMin; // a zero-sized rect
    f.Data().ResolutionScale = 1.0f;
    {
        UIViewContext ctx;
        ctx.RenderTextures = &source;
        R2D::DrawList2D dl;
        Draw( ctx, f, dl );
    }
    EXPECT_TRUE( source.Asks.empty() )
         << "a zero-sized element asked for a render target. There is nothing to show and nothing to "
            "allocate it for.";

    // And the ceiling, from an element far larger than any sane target.
    f.Layout().OffsetMin     = { 0.0f, 0.0f };
    f.Layout().OffsetMax     = { kSide * 8.0f, kSide * 8.0f };
    f.Data().ResolutionScale = 2.0f;
    {
        UIViewContext ctx;
        ctx.RenderTextures = &source;
        R2D::DrawList2D dl;
        Draw( ctx, f, dl );
    }
    ASSERT_EQ( source.Asks.size(), 1u );
    EXPECT_LE( source.Asks.front().Width, 4096u ) << "an unbounded render target was asked for";
    EXPECT_LE( source.Asks.front().Height, 4096u );
}

// --- 5. THE AUTHORED COLOUR REACHES THE QUAD -------------------------------------------------------

TEST( UIRenderTexture, TintAndOpacityReachTheDrawnQuad )
{
    Fixture       f;
    StubSource    source( /*answers=*/true );
    UIViewContext ctx;
    ctx.RenderTextures = &source;

    f.Data().Tint    = { 1.0f, 0.0f, 0.0f };
    f.Data().Opacity = 0.25f;

    R2D::DrawList2D dl;
    Draw( ctx, f, dl );

    bool found = false;
    for ( const auto& cmd : dl.GetCommands() )
    {
        if ( cmd.Texture != FakeWorld() )
        {
            continue;
        }
        const auto& v = dl.GetVertices()[dl.GetIndices()[cmd.IndexOffset]];
        EXPECT_NEAR( v.Color.r, 1.0f, 1e-4f );
        EXPECT_NEAR( v.Color.g, 0.0f, 1e-4f );
        EXPECT_NEAR( v.Color.b, 0.0f, 1e-4f );
        EXPECT_NEAR( v.Color.a, 0.25f, 1e-4f ) << "Opacity did not reach the quad, so the knob moves nothing";
        found = true;
    }
    EXPECT_TRUE( found ) << "no batch was bound to the world at all";
}

// The budget question the cache asks, and the view it builds from the same answer. A background demand here
// would let an asset thumbnail's reserve refuse an element a player is looking at; a profile or extent other
// than the built one would check a forecast for a different view than the one allocated.
TEST( UIRenderTexture, TheBudgetIsAskedForTheElementsOwnViewAsAUserSurface )
{
    using namespace Desert;
    const auto request = Graphic::Render2D::RequestUIRenderTextureView( 320, 180 );
    EXPECT_EQ( request.Who, Engine::ViewBudget::Demand::UserSurface );
    EXPECT_EQ( request.Extent.Width, 320u );
    EXPECT_EQ( request.Extent.Height, 180u );
    EXPECT_TRUE( request.Profile == Graphic::kPreviewViewProfile );

    // The entitlement end to end through the shared rule: a request that exactly fills what is free passes
    // for this element, where a background demand keeping any reserve would be refused.
    Engine::ViewBudget::Reading reading;
    reading.CeilingBytes = 1000;
    reading.UsageBytes   = 400;
    EXPECT_TRUE( Engine::ViewBudget::MayCreate( request.Who, 600, 1, reading ).Ok );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
