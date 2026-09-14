// "One view's canvas state never reaches another's."
//
// The defect this exists to prevent, measured on 2026-09-05: every cross-frame value the UI walk kept —
// hover and tween clocks, the elected hot element, the drag, the press edge, the screen stack — lived at
// namespace scope in UICanvasRenderer2D.cpp, one set per process, while the engine draws more than one
// canvas per frame. Three independent ways that bit:
//
//   * the editor builds a Render::RenderRegistry per open scene document, and its constructor installs an
//     EditorUIPass, so two viewports walked two scenes into the same variables;
//   * the UI Editor panel walks the SAME scene a second time and passes input = nullptr, but the walk hands
//     its hot election over at the end whether or not it had input — so the inert preview cleared the
//     viewport's elected element every frame it was open. That one needs no second document;
//   * entt::entity is unique only INSIDE its registry, so the per-entity clocks answered to entity 7 of
//     every scene at once. The same shape as the pipeline-cache key that dropped five fields.
//
// So the assertions here are about the RELATION between two views rather than about either one: two
// registries walked in one frame, and a scene walked twice by two views. Each is written so that giving
// both walks ONE context — which is what the file-scope variables were — turns it red. That mutation was
// run; see the report.
//
// This is also the first test coverage Engine/UI has ever had. scripts/CI/UnreachedSources.sh listed all
// three of its translation units among the 275 that no suite compiles.

#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>

// One handle the animated-image stub below answers for, and the fake image it hands back. The draw list
// treats a texture as an OPAQUE id — it stores the pointer and never dereferences it — so a fixed address
// is a complete stand-in for a GPU image here, and it is what lets the canvas-background draw be asserted
// without a device. Only this handle resolves; everything else still gets nothing, so a button with no
// sprite of its own is unaffected.
namespace
{
    constexpr uint64_t kBackgroundHandle = 0xB00B5;

    // Never dereferenced. Taken as an address so it is a real, unique object rather than a made-up number.
    int                       g_FakeImageStorage       = 0;
    bool                      g_BackgroundServiceArmed = false;
    Desert::Graphic::Image2D* FakeImage()
    {
        return reinterpret_cast<Desert::Graphic::Image2D*>( &g_FakeImageStorage );
    }
} // namespace

// The renderer resolves sprites, fonts, icons and video through these. Every one of them owns GPU objects,
// and every draw helper already copes with the service being absent — a sprite that will not resolve falls
// back to its flat colour, text and icons draw nothing. That is exactly the path a headless walk wants, so
// the suite supplies the accessors itself and returns nothing.
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
    IconService* ResourceRegistry::GetIconService()
    {
        return nullptr;
    }
    // The one service the suite can stand up, because the only thing the renderer does with what it
    // returns is put the pointer in a draw command. It is armed by a single test and otherwise absent.
    AnimatedImageService* ResourceRegistry::GetAnimatedImageService()
    {
        static AnimatedImageService stub;
        return g_BackgroundServiceArmed ? &stub : nullptr;
    }
    VideoService* ResourceRegistry::GetVideoService()
    {
        return nullptr;
    }

    // The service METHODS the walk calls on whatever those accessors hand back. Every accessor above
    // returns nullptr, so none of these can run — they exist because the linker still wants the symbols,
    // and each fails the test outright rather than returning a plausible value, so a future change that
    // manages to reach one is a loud failure instead of a quiet stub.
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
    // Answers for exactly one handle. Every other sprite in the walk keeps resolving to nothing, so a
    // button or panel with no image of its own draws its flat colour as it does everywhere else.
    Graphic::Image2D* AnimatedImageService::Resolve( const Assets::AssetHandle& handle )
    {
        return static_cast<uint64_t>( handle ) == kBackgroundHandle ? FakeImage() : nullptr;
    }
    Graphic::Image2D* VideoService::Resolve( uint64_t )
    {
        ADD_FAILURE() << "VideoService::Resolve reached with no video service";
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
using Desert::UI::UICanvasContext;
using Desert::UI::UIInput;
using Desert::UI::UIViewContext;
namespace ECS = Desert::ECS;
namespace R2D = Desert::Graphic::Render2D;

namespace
{
    constexpr float kSide = 1000.0f; // canvas is Stretch at 1000x1000, so design px == screen px (scale 1)

    const Rect kViewport{ 0.0f, 0.0f, kSide, kSide };

    // A canvas with one button in its top-left corner (0,0)-(100,50). The colours are deliberately far
    // apart in every channel so "which state did it draw" is a exact-equality question, not a threshold.
    struct Fixture
    {
        entt::registry Registry;
        entt::entity   Canvas = entt::null;
        entt::entity   Button = entt::null;

        Fixture()
        {
            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;

            Button           = Registry.create();
            auto& layout     = Registry.emplace<ECS::UILayoutComponent>( Button ).Data;
            layout.AnchorMin = { 0.0f, 0.0f };
            layout.AnchorMax = { 0.0f, 0.0f };
            layout.OffsetMin = { 0.0f, 0.0f };
            layout.OffsetMax = { 100.0f, 50.0f };

            auto& button        = Registry.emplace<ECS::UIButtonComponent>( Button ).Data;
            button.NormalColor  = { 0.1f, 0.1f, 0.1f };
            button.HoverColor   = { 0.5f, 0.5f, 0.5f };
            button.PressedColor = { 0.9f, 0.9f, 0.9f };

            Registry.emplace<ECS::RelationshipComponent>( Canvas ).Children.push_back( Button );
            Registry.emplace<ECS::RelationshipComponent>( Button ).Parent = Canvas;
        }
    };

    // Pointer state in canvas pixels. MouseDown makes the drawn colour an exact PressedColor rather than an
    // eased hover mix, which takes the wall clock out of every assertion that only cares about the election.
    UIInput At( float x, float y, bool down = true )
    {
        UIInput in;
        in.MousePx   = { x, y };
        in.MouseDown = down;
        return in;
    }

    // The colour the button was drawn with. It is the only element in the fixture, so the first vertex of
    // the list carries it.
    glm::vec4 DrawnColor( const R2D::DrawList2D& dl )
    {
        EXPECT_FALSE( dl.GetVertices().empty() ) << "the canvas drew nothing at all";
        return dl.GetVertices().empty() ? glm::vec4( -1.0f ) : dl.GetVertices().front().Color;
    }

    // Every walk in this file goes through here. RenderCanvas2D REFUSES rather than returning a bare false
    // (Ю1), and a test that swallowed the refusal would go on to assert about an empty draw list and pass
    // for entirely the wrong reason — so the refusal is surfaced at the one place that makes the call.
    // It is also a whole FRAME of the view — BeginUIFrame / EndUIFrame around the one canvas — because
    // that is what a host does and because the walk refuses outside a frame (Ю4). Tests that need two
    // canvases in one frame use DrawTwo below instead of calling this twice, which would be two frames.
    bool Draw( UIViewContext& ctx, entt::registry& reg, entt::entity canvas, R2D::DrawList2D& dl,
               const UIInput* input = nullptr, std::string* outClicked = nullptr, entt::entity* focused = nullptr,
               std::vector<std::string>* outMessages = nullptr )
    {
        Desert::UI::BeginUIFrame( ctx, reg, kViewport );
        const auto drawn = Desert::UI::RenderCanvas2D( ctx, reg, canvas, dl,
                                                       /*worldViewProj=*/nullptr, input, outClicked, focused );
        EXPECT_TRUE( drawn.IsSuccess() ) << drawn.GetError();
        Desert::UI::EndUIFrame( ctx, reg, dl, input, focused, outClicked, outMessages );
        return drawn.IsSuccess() && drawn.GetValue();
    }

    // Draw one frame of @p f through @p ctx and hand back what the button was painted.
    glm::vec4 Frame( UIViewContext& ctx, Fixture& f, const UIInput* input )
    {
        R2D::DrawList2D dl;
        Draw( ctx, f.Registry, f.Canvas, dl, input );
        return DrawnColor( dl );
    }

    // Same, spelled so a call can build the pointer state inline (a temporary lives to the end of the full
    // expression, which is longer than the walk).
    glm::vec4 Frame( UIViewContext& ctx, Fixture& f, const UIInput& input )
    {
        return Frame( ctx, f, &input );
    }

    // Push this view's wall clock @p seconds into the past, so the NEXT frame it draws measures that delta.
    // The renderer reads a real clock (hover eases and tweens are wall-clock driven by design); this is how
    // a test asks it for a specific one without sleeping.
    void RewindClock( UIViewContext& ctx, float seconds )
    {
        ctx.LastFrameTime -= seconds;
    }

    bool SameColor( const glm::vec4& a, const glm::vec3& rgb )
    {
        return std::fabs( a.r - rgb.r ) < 1e-5f && std::fabs( a.g - rgb.g ) < 1e-5f &&
               std::fabs( a.b - rgb.b ) < 1e-5f;
    }
} // namespace

// --- (1) Two scenes, two views, one frame ----------------------------------------------------------------
//
// The editor case. Both registries hand out the SAME entity ids — that is the point, and it is why the key
// had to stop being a bare entt::entity.
TEST( UICanvasContext, TheHotElectionOfOneViewDoesNotReachAnother )
{
    Fixture a, b;
    ASSERT_EQ( a.Button, b.Button ) << "the two registries must hand out the same id for this to test anything";

    UIViewContext ctxA, ctxB;

    // Frame 1 elects: A's pointer is on its button, B's is far away. Controls react to the PREVIOUS frame's
    // winner, so nothing is pressed yet in either.
    Frame( ctxA, a, At( 10.0f, 10.0f ) );
    Frame( ctxB, b, At( 900.0f, 900.0f ) );

    // Frame 2 acts on that election.
    const glm::vec4 drawnA = Frame( ctxA, a, At( 10.0f, 10.0f ) );
    const glm::vec4 drawnB = Frame( ctxB, b, At( 900.0f, 900.0f ) );

    EXPECT_TRUE( SameColor( drawnA, glm::vec3( 0.9f ) ) )
         << "the pointer is inside A's button and it did not react";
    EXPECT_TRUE( SameColor( drawnB, glm::vec3( 0.1f ) ) )
         << "B's pointer is 900 px away from its button, but the button lit up — A's election reached it";
}

// --- (2) One scene, two views, and the second one has no input at all ------------------------------------
//
// The UI Editor panel. Inertness is not enough: the hand-over at the end of the walk (Hot = HotNext) runs
// whether or not there was input, so a second inert walk over the same scene used to null the viewport's
// elected element every frame. Observable with one document open, which is what made it the third argument.
TEST( UICanvasContext, AnInertPreviewDoesNotClearTheInteractiveViewsElection )
{
    Fixture         f;
    UIViewContext   viewport;
    UIViewContext   preview;
    preview.DrivesSceneAnimation = false; // as UIEditorPanel configures it

    Frame( viewport, f, At( 10.0f, 10.0f ) );
    Frame( preview, f, nullptr ); // the authoring window, drawn in the same frame

    const glm::vec4 drawn = Frame( viewport, f, At( 10.0f, 10.0f ) );
    EXPECT_TRUE( SameColor( drawn, glm::vec3( 0.9f ) ) )
         << "the viewport's button stopped reacting while an inert preview of the same scene was drawn";
}

// --- (3) The per-entity key ------------------------------------------------------------------------------
//
// A hover clock stored against a bare entt::entity is a key two scenes both answer to. Here view A has fully
// hovered ITS entity 1; view B's entity 1 is a different button in a different registry and must be at rest.
TEST( UICanvasContext, APerEntityClockIsKeyedInsideItsOwnView )
{
    Fixture         a, b;
    UIViewContext   ctxA, ctxB;

    Frame( ctxA, a, At( 10.0f, 10.0f, /*down=*/false ) );
    Frame( ctxB, b, At( 900.0f, 900.0f, /*down=*/false ) );

    // A has been hovering long enough for its ease to saturate.
    ctxA.CanvasState( a.Canvas ).HoverT[a.Button] = 1.0f;
    ASSERT_EQ( ctxA.CanvasState( a.Canvas ).HoverT.count( a.Button ), 1u );

    RewindClock( ctxB, 0.5f ); // give B a real frame delta, so a leaked clock would have time to show
    const glm::vec4 drawnB = Frame( ctxB, b, At( 900.0f, 900.0f, /*down=*/false ) );

    EXPECT_TRUE( SameColor( drawnB, glm::vec3( 0.1f ) ) )
         << "B's button drew a hover blend from a clock that belongs to A's entity of the same id";
    EXPECT_NEAR( ctxB.CanvasState( b.Canvas ).HoverT[b.Button], 0.0f, 1e-4f );
}

// --- (4) Each view keeps its own frame delta -------------------------------------------------------------
//
// The clock was one file-scope float refreshed at the top of every call, so of two walks in one frame the
// second measured ~0 seconds and its hover eases, tweens and screen transition stood still. Both views here
// are handed the same 50 ms and must both spend it.
TEST( UICanvasContext, EveryViewMeasuresItsOwnFrameDelta )
{
    Fixture         a, b;
    UIViewContext   ctxA, ctxB;

    Frame( ctxA, a, At( 10.0f, 10.0f, /*down=*/false ) ); // seed both clocks
    Frame( ctxB, b, At( 10.0f, 10.0f, /*down=*/false ) );

    RewindClock( ctxA, 0.05f );
    RewindClock( ctxB, 0.05f );
    Frame( ctxA, a, At( 10.0f, 10.0f, /*down=*/false ) );
    Frame( ctxB, b, At( 10.0f, 10.0f, /*down=*/false ) );

    EXPECT_NEAR( ctxA.FrameDt, 0.05f, 5e-3f );
    EXPECT_NEAR( ctxB.FrameDt, 0.05f, 5e-3f )
         << "the second view of the frame measured no time — the two walks are sharing one clock";

    // And the hover ease that delta drives moved by the same amount in both. The tolerances here are wide
    // on purpose: the clock is a real one, the two walks are microseconds apart, and the defect this
    // catches is one view easing to 0.6 while the other sits at exactly 0 — not a difference in the fourth
    // decimal. A tighter bound made this test fail on the spread between two consecutive steady_clock
    // reads, which is a flake and worse than no test at all.
    EXPECT_GT( ctxA.CanvasState( a.Canvas ).HoverT[a.Button], 0.5f )
         << "50 ms of hover moved view A's ease by nothing";
    EXPECT_GT( ctxB.CanvasState( b.Canvas ).HoverT[b.Button], 0.5f )
         << "50 ms of hover moved view B's ease by nothing";
    EXPECT_NEAR( ctxA.CanvasState( a.Canvas ).HoverT[a.Button], ctxB.CanvasState( b.Canvas ).HoverT[b.Button],
                 0.01f );
}

// --- (5) Screen navigation is view state, the anim playhead is scene state -------------------------------
//
// Two views of one scene: one navigates, the other must not follow. This is the half of the split that had
// to stay OUT of the components (UI_ROADMAP.md section F) — navigating in the editor must not rewrite the
// authored scene.
TEST( UICanvasContext, ScreenNavigationBelongsToTheViewThatDidIt )
{
    Fixture f;

    // Rehome the button under a "Home" screen and add an empty "Settings" beside it, which is how a real
    // canvas with pages is built. InitialScreen is named rather than left to the seeding loop's first hit:
    // that loop walks an entt view, whose order is the component pool's, not the creation order.
    auto& stack         = f.Registry.emplace<ECS::UIScreenStackComponent>( f.Canvas ).Data;
    stack.InitialScreen = "Home";

    const entt::entity home                                      = f.Registry.create();
    f.Registry.emplace<ECS::UIScreenComponent>( home ).Data.Name = "Home";
    auto& homeLayout     = f.Registry.emplace<ECS::UILayoutComponent>( home ).Data;
    homeLayout.AnchorMax = { 1.0f, 1.0f }; // a screen spreads over the whole canvas
    homeLayout.OffsetMax = { 0.0f, 0.0f };

    const entt::entity settings                                      = f.Registry.create();
    f.Registry.emplace<ECS::UIScreenComponent>( settings ).Data.Name = "Settings";
    auto& settingsLayout     = f.Registry.emplace<ECS::UILayoutComponent>( settings ).Data;
    settingsLayout.AnchorMax = { 1.0f, 1.0f };
    settingsLayout.OffsetMax = { 0.0f, 0.0f };

    auto& canvasKids = f.Registry.get<ECS::RelationshipComponent>( f.Canvas ).Children;
    canvasKids.clear();
    canvasKids.push_back( home );
    canvasKids.push_back( settings );
    f.Registry.emplace<ECS::RelationshipComponent>( home ).Children.push_back( f.Button );
    f.Registry.emplace<ECS::RelationshipComponent>( settings );
    f.Registry.get<ECS::RelationshipComponent>( f.Button ).Parent = home;

    auto& button          = f.Registry.get<ECS::UIButtonComponent>( f.Button ).Data;
    button.Action         = ECS::UIButtonAction::ShowScreen;
    button.OnClickMessage = "Settings";

    UIViewContext viewport, second;

    // Seed both views, then release the pointer over the button in ONE of them.
    Frame( viewport, f, At( 10.0f, 10.0f ) );
    Frame( second, f, At( 900.0f, 900.0f ) );
    ASSERT_TRUE( viewport.Hot == f.Button ) << "the pointer sat on the button and something else was elected";

    UIInput click       = At( 10.0f, 10.0f, /*down=*/false );
    click.MouseReleased = true;
    {
        R2D::DrawList2D dl;
        std::string     clicked;
        Draw( viewport, f.Registry, f.Canvas, dl, &click, &clicked );
        EXPECT_EQ( clicked, "screen:Settings" );
    }
    Frame( second, f, At( 900.0f, 900.0f ) );

    EXPECT_EQ( viewport.CanvasState( f.Canvas ).Screen, "Settings" );
    EXPECT_EQ( second.CanvasState( f.Canvas ).Screen, "Home" )
         << "a second view of the same scene followed a navigation it never made";
}

// --- (6) The one clock that is NOT view state ------------------------------------------------------------
//
// UIAnimComponent's playhead lives in the component because the Sequencer scrubs it, so it is SCENE state
// and exactly one view may advance it. Both advancing it is the mirror image of the bug this whole change
// fixes: every clip would run at twice its authored speed whenever the UI Editor panel is open.
TEST( UICanvasContext, OnlyTheDrivingViewAdvancesTheScenesAnimationPlayhead )
{
    Fixture f;
    auto&   clip  = f.Registry.emplace<ECS::UIAnimComponent>( f.Button ).Data;
    clip.Playing  = true;
    clip.Duration = 100.0f; // long enough that nothing wraps
    clip.Loop     = false;

    UIViewContext viewport;
    UIViewContext preview;
    preview.DrivesSceneAnimation = false;

    Frame( viewport, f, At( 900.0f, 900.0f, /*down=*/false ) );
    Frame( preview, f, nullptr );
    clip.Time = 5.0f;

    RewindClock( preview, 0.05f );
    Frame( preview, f, nullptr );
    EXPECT_FLOAT_EQ( clip.Time, 5.0f ) << "the authoring preview advanced a playhead it does not own";

    RewindClock( viewport, 0.05f );
    Frame( viewport, f, At( 900.0f, 900.0f, /*down=*/false ) );
    EXPECT_NEAR( clip.Time, 5.05f, 5e-3f ) << "the driving view did not advance the playhead";
}

// --- (7) A view pointed at another scene forgets the first one -------------------------------------------
//
// One host does reuse its context across scenes: the UI Editor panel follows the active document. The ids
// it remembers mean something else in the new registry, so the context drops them.
TEST( UICanvasContext, RebindingAViewToAnotherRegistryDropsItsPerEntityState )
{
    Fixture         a, b;
    UIViewContext   ctx;

    Frame( ctx, a, At( 10.0f, 10.0f ) );
    Frame( ctx, a, At( 10.0f, 10.0f ) );
    ASSERT_EQ( ctx.Hot, a.Button ) << "the pointer was over A's button for two frames and it was not elected";
    ASSERT_FALSE( ctx.CanvasState( a.Canvas ).HoverT.empty() );

    const glm::vec4 drawnB = Frame( ctx, b, At( 900.0f, 900.0f ) );
    EXPECT_TRUE( ctx.Hot == entt::null ) << "the election survived a change of scene";
    EXPECT_EQ( ctx.CanvasStateCount(), 1u ) << "a rebind kept the old scene's (canvas x view) cell as well";
    EXPECT_TRUE( SameColor( drawnB, glm::vec3( 0.1f ) ) )
         << "B's button reacted to an election made in A, because the id matched";
}

// --- (8) The canvas background must not invent a colour it does not have ---------------------------------
//
// UICanvasData::Sprite was a dead setting — reflected, serialized, shown in Details, read by nothing. It now
// draws as a full-canvas backdrop. It has no colour of its own, so it may NOT take the flat-fill fallback a
// panel takes: doing so paints an opaque white sheet over the whole scene whenever the image is missing.
// Here no image service exists, so nothing can resolve, and the frame must be exactly what it was before.
TEST( UICanvasContext, AnUnresolvableCanvasBackgroundDrawsNothingRatherThanAWhiteSheet )
{
    Fixture bare, withSprite;
    withSprite.Registry.get<ECS::UICanvasComponent>( withSprite.Canvas ).Data.Sprite =
         Desert::Assets::AssetHandle( 0x1234u );

    UIViewContext   c1, c2;
    R2D::DrawList2D dlBare, dlSprite;
    Draw( c1, bare.Registry, bare.Canvas, dlBare );
    Draw( c2, withSprite.Registry, withSprite.Canvas, dlSprite );

    EXPECT_EQ( dlSprite.GetVertices().size(), dlBare.GetVertices().size() )
         << "a background sprite that did not resolve still put geometry on screen";
    EXPECT_TRUE( SameColor( DrawnColor( dlSprite ), glm::vec3( 0.1f ) ) )
         << "the first thing drawn is no longer the button — a backdrop was painted under it from nothing";
}

// --- (9) And when it DOES resolve, it is drawn: full canvas, under everything ----------------------------
//
// The other half of the dead setting. Test (8) says a background that cannot resolve invents nothing; this
// one says a background that can resolve reaches the draw list, covers the whole canvas rect, and is the
// FIRST thing emitted so every child lands on top of it.
//
// It is asserted here rather than in a frame because RenderCanvas2D takes a registry and a draw list and
// touches no file.
//
// THIS COMMENT USED TO SAY A CANVAS BACKGROUND COULD NOT BE AUTHORED IN A `.desce` AT ALL, and listed
// three serializer defects behind that: an absolute machine-local path on the write side, a silent 0 with
// no log on the read side, and a numeric handle above 2^53 mangled by a JSON double round trip (measured:
// 5355760296319878840 came back as 5355760296319879168). All three were real and ALL THREE ARE FIXED — the
// first two by Ф5's extraction of the texture reference into Engine/Core/Serialize/TextureSlot.cpp (which
// stores the root-tagged stable key and logs every miss with the roots it searched), the third by
// ReflectionSerializer's integral read path. Each has its own suite now: TextureSlotRoundTrip,
// ReflectionSerializer and UIComponentRoundTrip, the last of which round-trips THIS component's Sprite
// through JSON text on the very handle quoted above. `Editor/Resources/Assets/Scenes/UI_SpriteSlots.desce`
// carries an authored canvas background as `cooked:Textures/T_Checker.tex`, which is the same claim made
// in the corpus rather than in a comment.
TEST( UICanvasContext, AResolvableCanvasBackgroundCoversTheCanvasAndIsDrawnFirst )
{
    Fixture f;
    f.Registry.get<ECS::UICanvasComponent>( f.Canvas ).Data.Sprite =
         Desert::Assets::AssetHandle( kBackgroundHandle );

    g_BackgroundServiceArmed = true;
    UIViewContext   ctx;
    R2D::DrawList2D dl;
    Draw( ctx, f.Registry, f.Canvas, dl );
    g_BackgroundServiceArmed = false;

    ASSERT_FALSE( dl.GetCommands().empty() );
    EXPECT_EQ( dl.GetCommands().front().Texture, FakeImage() )
         << "the first draw command is not the canvas backdrop, so a child would be painted over by it";

    // The first quad is the backdrop: four vertices spanning the whole canvas, which at Stretch is the
    // whole viewport. The safe area does not cut it -- a notch inset says where CONTENT may not go, not
    // where the wallpaper stops.
    ASSERT_GE( dl.GetVertices().size(), 4u );
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for ( std::size_t i = 0; i < 4; ++i )
    {
        const glm::vec2 p = dl.GetVertices()[i].Position;
        minX              = std::min( minX, p.x );
        minY              = std::min( minY, p.y );
        maxX              = std::max( maxX, p.x );
        maxY              = std::max( maxY, p.y );
    }
    EXPECT_FLOAT_EQ( minX, 0.0f );
    EXPECT_FLOAT_EQ( minY, 0.0f );
    EXPECT_FLOAT_EQ( maxX, kSide );
    EXPECT_FLOAT_EQ( maxY, kSide );

    // And the button is still drawn, on top: the backdrop did not replace the tree.
    EXPECT_GT( dl.GetVertices().size(), 4u ) << "the canvas drew its backdrop and nothing else";
}

// =========================================================================================================
// У4 — the two visibility axes. Written as RELATIONS between two arrangements rather than as expected
// pixel coordinates, because the numbers a layout group produces are not the claim: the claim is that
// Collapsed costs its neighbours exactly one slot and Hidden costs them nothing.
// =========================================================================================================

namespace
{
    // A VBox filling the canvas with three 100x50 items stacked top to bottom, no spacing and no padding,
    // so a slot is worth exactly its own height and the arithmetic below has no other term in it. The
    // three colours are far apart in every channel so a rect can be recovered from the vertex buffer by
    // colour, which survives corner rounding and any other geometry the panel decides to emit.
    struct Stack
    {
        static constexpr float kItemH = 50.0f;

        entt::registry Registry;
        entt::entity   Canvas = entt::null;
        entt::entity   Box    = entt::null;
        entt::entity   Item[3]{ entt::null, entt::null, entt::null };

        static glm::vec3 ColorOf( int i )
        {
            return i == 0   ? glm::vec3( 1.0f, 0.0f, 0.0f )
                   : i == 1 ? glm::vec3( 0.0f, 1.0f, 0.0f )
                            : glm::vec3( 0.0f, 0.0f, 1.0f );
        }

        Stack()
        {
            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;

            Box                 = Registry.create();
            auto& boxLayout     = Registry.emplace<ECS::UILayoutComponent>( Box ).Data;
            boxLayout.AnchorMin = { 0.0f, 0.0f };
            boxLayout.AnchorMax = { 1.0f, 1.0f };
            boxLayout.OffsetMin = { 0.0f, 0.0f };
            boxLayout.OffsetMax = { 0.0f, 0.0f };

            auto& group        = Registry.emplace<ECS::UILayoutGroupComponent>( Box ).Data;
            group.Type         = ECS::UILayoutType::Vertical;
            group.Spacing      = 0.0f;
            group.Padding      = glm::vec4( 0.0f );
            group.StretchCross = true;

            Registry.emplace<ECS::RelationshipComponent>( Canvas ).Children.push_back( Box );
            Registry.emplace<ECS::RelationshipComponent>( Box ).Parent = Canvas;

            // Every entity is created and given its components BEFORE any of them is linked up. Holding a
            // reference into a component pool across a later emplace into that same pool is a dangling
            // one — entt is free to reallocate — and the first version of this fixture did exactly that:
            // the box's children vector was written through a freed pointer and the stack drew nothing.
            for ( int i = 0; i < 3; ++i )
            {
                Item[i]          = Registry.create();
                auto& layout     = Registry.emplace<ECS::UILayoutComponent>( Item[i] ).Data;
                layout.AnchorMin = { 0.0f, 0.0f };
                layout.AnchorMax = { 0.0f, 0.0f };
                layout.OffsetMin = { 0.0f, 0.0f };
                layout.OffsetMax = { 100.0f, kItemH }; // preferred size = the slot the group gives it

                auto& panel        = Registry.emplace<ECS::UIPanelComponent>( Item[i] ).Data;
                panel.Color        = ColorOf( i );
                panel.Opacity      = 1.0f;
                panel.CornerRadius = 0.0f;

                Registry.emplace<ECS::RelationshipComponent>( Item[i] ).Parent = Box;
            }
            for ( int i = 0; i < 3; ++i )
                Registry.get<ECS::RelationshipComponent>( Box ).Children.push_back( Item[i] );
        }

        void SetVisibility( int item, ECS::UIVisibility v )
        {
            Registry.get<ECS::UILayoutComponent>( Item[item] ).Data.Visibility = v;
        }
    };

    // The bounding box of every vertex painted in @p rgb, or nullopt when the colour was never drawn. This
    // is how "where did that element end up" is read back without asking the renderer to report it.
    std::optional<Rect> RectOfColor( const R2D::DrawList2D& dl, const glm::vec3& rgb )
    {
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        bool  seen = false;
        for ( const auto& v : dl.GetVertices() )
        {
            if ( !SameColor( v.Color, rgb ) )
                continue;
            seen = true;
            minX = std::min( minX, v.Position.x );
            minY = std::min( minY, v.Position.y );
            maxX = std::max( maxX, v.Position.x );
            maxY = std::max( maxY, v.Position.y );
        }
        if ( !seen )
            return std::nullopt;
        return Rect{ minX, minY, maxX - minX, maxY - minY };
    }

    // Draw @p s once and hand back where each of its three items landed (nullopt = not drawn at all).
    std::array<std::optional<Rect>, 3> Layout( Stack& s )
    {
        UIViewContext   ctx;
        R2D::DrawList2D dl;
        Draw( ctx, s.Registry, s.Canvas, dl );
        return { RectOfColor( dl, Stack::ColorOf( 0 ) ), RectOfColor( dl, Stack::ColorOf( 1 ) ),
                 RectOfColor( dl, Stack::ColorOf( 2 ) ) };
    }
} // namespace

// --- (10) THE LAYOUT AXIS -------------------------------------------------------------------------------
//
// The relation, and it is one subtraction: a Collapsed element costs the siblings below it exactly its own
// slot, and a Hidden one costs them nothing. Asserting the three absolute positions instead would pass just
// as happily on a build where Hidden also closed the gap, as long as the arithmetic was self-consistent.
TEST( UICanvasVisibility, CollapsedCostsTheSiblingsExactlyOneSlotAndHiddenCostsThemNothing )
{
    Stack visible, hidden, collapsed;
    hidden.SetVisibility( 1, ECS::UIVisibility::Hidden );
    collapsed.SetVisibility( 1, ECS::UIVisibility::Collapsed );

    const auto v = Layout( visible );
    const auto h = Layout( hidden );
    const auto c = Layout( collapsed );

    ASSERT_TRUE( v[0] && v[1] && v[2] ) << "the untouched stack did not draw all three items";
    ASSERT_TRUE( h[0] && h[2] );
    ASSERT_TRUE( c[0] && c[2] );

    // Neither state draws the element. That is the half the two share.
    EXPECT_FALSE( h[1].has_value() ) << "a Hidden element was still painted";
    EXPECT_FALSE( c[1].has_value() ) << "a Collapsed element was still painted";

    // The first item is above the change and must not move in either.
    EXPECT_FLOAT_EQ( h[0]->Y, v[0]->Y );
    EXPECT_FLOAT_EQ( c[0]->Y, v[0]->Y );

    // THE RELATION. The gap the middle item held is exactly its own height, so the item below it moves up
    // by that and by nothing else when it collapses, and does not move at all when it merely hides.
    EXPECT_FLOAT_EQ( h[2]->Y, v[2]->Y ) << "Hidden closed the gap — then it is Collapsed under another name";
    EXPECT_FLOAT_EQ( v[2]->Y - c[2]->Y, Stack::kItemH )
         << "Collapsed moved the sibling below by " << ( v[2]->Y - c[2]->Y ) << " px and the slot it "
         << "vacated is " << Stack::kItemH << " px tall";

    // And the surviving items keep their size: a closed gap redistributes position, not height.
    EXPECT_FLOAT_EQ( c[2]->H, v[2]->H );
    EXPECT_FLOAT_EQ( h[2]->H, v[2]->H );
}

// --- (11) The pick and the draw are one layout ----------------------------------------------------------
//
// The editor resolves rects a second time (UICanvasLayout.cpp) so a click in the viewport selects what is
// under it. Two solvers that must agree is the defect shape this project keeps hitting, and Collapsed is a
// fresh chance to hit it: if the pick still gave the collapsed child a slot, every element below it would
// be selectable 50 px away from where it is drawn.
TEST( UICanvasVisibility, TheEditorPickAgreesWithTheDrawAboutACollapsedSlot )
{
    Stack s;
    s.SetVisibility( 1, ECS::UIVisibility::Collapsed );

    const auto drawn = Layout( s );
    ASSERT_TRUE( drawn[2].has_value() );

    const glm::vec2 inside( drawn[2]->X + 5.0f, drawn[2]->Y + drawn[2]->H * 0.5f );
    EXPECT_EQ( Desert::UI::PickElement( s.Registry, s.Canvas, inside, kViewport ), s.Item[2] )
         << "a click in the middle of the third item, where it is DRAWN, did not pick it";

    // And the collapsed one is not pickable anywhere, because it is nowhere.
    for ( float y = 0.0f; y < 3.0f * Stack::kItemH; y += 5.0f )
        EXPECT_NE( Desert::UI::PickElement( s.Registry, s.Canvas, { 5.0f, y }, kViewport ), s.Item[1] )
             << "the collapsed item was picked at y=" << y;
}

// =========================================================================================================
// The hit-test axis. All four values are asserted through the ELECTION (ctx.Hot), because that is the one
// thing every control downstream reads, and through whether the button reacts, because being elected and
// responding are the two halves the four values split differently.
// =========================================================================================================

namespace
{
    // A full-canvas panel with a button in its top-left corner. The panel is the ancestor whose HitTest is
    // under test; the button is what the pointer is really over.
    struct Nested
    {
        entt::registry Registry;
        entt::entity   Canvas = entt::null;
        entt::entity   Panel  = entt::null;
        entt::entity   Button = entt::null;

        Nested()
        {
            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;

            Panel                 = Registry.create();
            auto& panelLayout     = Registry.emplace<ECS::UILayoutComponent>( Panel ).Data;
            panelLayout.AnchorMin = { 0.0f, 0.0f };
            panelLayout.AnchorMax = { 1.0f, 1.0f };
            panelLayout.OffsetMin = { 0.0f, 0.0f };
            panelLayout.OffsetMax = { 0.0f, 0.0f };
            Registry.emplace<ECS::UIPanelComponent>( Panel );

            Button           = Registry.create();
            auto& layout     = Registry.emplace<ECS::UILayoutComponent>( Button ).Data;
            layout.AnchorMin = { 0.0f, 0.0f };
            layout.AnchorMax = { 0.0f, 0.0f };
            layout.OffsetMin = { 0.0f, 0.0f };
            layout.OffsetMax = { 100.0f, 50.0f };

            auto& button        = Registry.emplace<ECS::UIButtonComponent>( Button ).Data;
            button.NormalColor  = { 0.1f, 0.1f, 0.1f };
            button.HoverColor   = { 0.5f, 0.5f, 0.5f };
            button.PressedColor = { 0.9f, 0.9f, 0.9f };

            Registry.emplace<ECS::RelationshipComponent>( Canvas ).Children.push_back( Panel );
            auto& panelKids  = Registry.emplace<ECS::RelationshipComponent>( Panel );
            panelKids.Parent = Canvas;
            panelKids.Children.push_back( Button );
            Registry.emplace<ECS::RelationshipComponent>( Button ).Parent = Panel;
        }

        void SetHitTest( entt::entity e, ECS::UIHitTest h )
        {
            Registry.get<ECS::UILayoutComponent>( e ).Data.HitTest = h;
        }
    };

    // Two frames of @p n with the pointer held at (@p x, @p y): the first elects, the second acts on that
    // election (controls compare against the PREVIOUS frame's winner). Hands back who was elected and what
    // the button was painted.
    struct Probe
    {
        entt::entity Hot = entt::null;
        glm::vec4    ButtonColor{ -1.0f };
    };

    Probe Press( Nested& n, float x, float y )
    {
        UIViewContext   ctx;
        R2D::DrawList2D first;
        const UIInput   in = At( x, y );
        Draw( ctx, n.Registry, n.Canvas, first, &in );

        Probe           out;
        R2D::DrawList2D second;
        Draw( ctx, n.Registry, n.Canvas, second, &in );
        out.Hot = ctx.HotNext == entt::null ? ctx.Hot : ctx.HotNext;
        // The panel is drawn first and the button on top of it, so the button's quad is the LAST colour in
        // the list that is one of its three states.
        for ( const auto& v : second.GetVertices() )
            if ( SameColor( v.Color, glm::vec3( 0.1f ) ) || SameColor( v.Color, glm::vec3( 0.5f ) ) ||
                 SameColor( v.Color, glm::vec3( 0.9f ) ) )
                out.ButtonColor = v.Color;
        return out;
    }
} // namespace

// --- (12) All: the baseline both ways -------------------------------------------------------------------
TEST( UICanvasHitTest, AllElectsTheElementAndItsChildren )
{
    Nested n;

    const Probe onButton = Press( n, 10.0f, 10.0f );
    EXPECT_EQ( onButton.Hot, n.Button );
    EXPECT_TRUE( SameColor( onButton.ButtonColor, glm::vec3( 0.9f ) ) ) << "the button did not react";

    const Probe onPanel = Press( n, 900.0f, 900.0f );
    EXPECT_EQ( onPanel.Hot, n.Panel ) << "a plain panel must stop the pointer; that is what All means";
}

// --- (13) ChildrenOnly: the old RaycastTarget = false ---------------------------------------------------
//
// The two halves in one test, because either alone is satisfied by a mistake: the element must NOT be
// elected where only it is under the pointer, and its child must STILL be elected where the child is.
TEST( UICanvasHitTest, ChildrenOnlyDoesNotElectItselfButStillElectsItsChild )
{
    Nested n;
    n.SetHitTest( n.Panel, ECS::UIHitTest::ChildrenOnly );

    EXPECT_TRUE( Press( n, 900.0f, 900.0f ).Hot == entt::null )
         << "a ChildrenOnly element was elected where nothing but it is under the pointer";

    const Probe onButton = Press( n, 10.0f, 10.0f );
    EXPECT_EQ( onButton.Hot, n.Button ) << "the child of a transparent parent stopped being hit-testable";
    EXPECT_TRUE( SameColor( onButton.ButtonColor, glm::vec3( 0.9f ) ) )
         << "the child was elected but no longer responds";
}

// --- (14) None: UE's HitTestInvisible, which neither old boolean could say ------------------------------
//
// THE SUB-TREE IS THE POINT. RaycastTarget = false cleared on the panel alone left the button underneath
// perfectly clickable, so "this overlay lets every click through" had to be spelled by clearing a flag on
// every descendant by hand. Nothing anywhere under a None may be elected.
TEST( UICanvasHitTest, NothingInTheSubTreeOfANoneCanBecomeHot )
{
    Nested n;
    n.SetHitTest( n.Panel, ECS::UIHitTest::None );

    EXPECT_TRUE( Press( n, 900.0f, 900.0f ).Hot == entt::null );

    const Probe onButton = Press( n, 10.0f, 10.0f );
    EXPECT_TRUE( onButton.Hot == entt::null ) << "the button under a HitTest::None panel was still elected";
    EXPECT_TRUE( SameColor( onButton.ButtonColor, glm::vec3( 0.1f ) ) )
         << "the button under a HitTest::None panel still reacted to the pointer";

    // And it is the ANCESTOR's value doing it: the button's own is untouched and says All.
    EXPECT_EQ( n.Registry.get<ECS::UILayoutComponent>( n.Button ).Data.HitTest, ECS::UIHitTest::All );
}

// --- (15) Blocking: what Interactable = false became, plus the propagation it never had ----------------
//
// Two claims that pull in opposite directions and are both required: the element STOPS the pointer (a modal
// scrim has to swallow the click) and NOTHING under it responds (a greyed-out form is grey all the way
// down). A value that only did the first would be All; one that only did the second would be None.
TEST( UICanvasHitTest, BlockingStopsThePointerAndSilencesTheWholeSubTree )
{
    Nested n;
    n.SetHitTest( n.Panel, ECS::UIHitTest::Blocking );

    EXPECT_EQ( Press( n, 900.0f, 900.0f ).Hot, n.Panel ) << "a Blocking element let the pointer past it";

    const Probe onButton = Press( n, 10.0f, 10.0f );
    EXPECT_EQ( onButton.Hot, n.Panel )
         << "the click landed on the button inside a Blocking panel instead of being swallowed by it";
    EXPECT_TRUE( SameColor( onButton.ButtonColor, glm::vec3( 0.1f ) ) )
         << "a button inside a Blocking panel still reacted — the old Interactable flag did not propagate "
            "and this value exists to fix exactly that";
}

// --- (16) The axes do not leak into each other ---------------------------------------------------------
//
// Nine of the twelve products are Hidden or Collapsed, where the hit-test value cannot be observed because
// there is nothing on screen to point at. That is a property to STATE, not to leave implied: an element
// nobody can see must not eat clicks whatever its Hit Test says, which is also UE's rule (neither Hidden
// nor Collapsed is hit-testable there either).
TEST( UICanvasHitTest, AnElementThatIsNotVisibleIsNotHitTestableWhateverItsHitTestSays )
{
    for ( const ECS::UIVisibility invisible : { ECS::UIVisibility::Hidden, ECS::UIVisibility::Collapsed } )
    {
        for ( const ECS::UIHitTest hit : { ECS::UIHitTest::All, ECS::UIHitTest::ChildrenOnly,
                                           ECS::UIHitTest::Blocking, ECS::UIHitTest::None } )
        {
            Nested n;
            n.Registry.get<ECS::UILayoutComponent>( n.Panel ).Data.Visibility = invisible;
            n.SetHitTest( n.Panel, hit );

            EXPECT_TRUE( Press( n, 900.0f, 900.0f ).Hot == entt::null )
                 << "an invisible panel was elected with Visibility " << static_cast<int>( invisible )
                 << " and Hit Test " << static_cast<int>( hit );
            EXPECT_TRUE( Press( n, 10.0f, 10.0f ).Hot == entt::null )
                 << "the button inside an invisible panel was elected with Visibility "
                 << static_cast<int>( invisible ) << " and Hit Test " << static_cast<int>( hit );
        }
    }
}

// =========================================================================================================
// Ю1 — THE KEYBOARD IS THE SAME HIT TEST. У4 above asserts all four values through the POINTER; every one of
// those tests stays green on a build where Tab and Enter ignore the axis entirely, which is what `dev`
// shipped. So the claim here is not "the keyboard obeys Blocking" — it is that the two input paths reach the
// SAME SET, asserted as an equality across all four values, plus one pinned row so that "neither path works"
// cannot satisfy it (a count with no named row is satisfiable by breaking both sides).
// =========================================================================================================

namespace
{
    constexpr const char* kFired = "u1-fired";

    // Make the button report its own activation, whichever path fires it. Without an action a click writes
    // nothing to outClicked and both probes below would read "did not fire" forever.
    void ArmButton( Nested& n )
    {
        auto& b          = n.Registry.get<ECS::UIButtonComponent>( n.Button ).Data;
        b.Action         = ECS::UIButtonAction::SendMessage;
        b.OnClickMessage = kFired;
    }

    // Did the POINTER manage to fire the button, with the panel set to @p hit? Frame one elects (the hot
    // element is resolved a frame late by design), frame two releases over it.
    bool PointerFires( ECS::UIHitTest hit )
    {
        Nested n;
        n.SetHitTest( n.Panel, hit );
        ArmButton( n );

        UIViewContext   ctx;
        R2D::DrawList2D a, b;
        const UIInput   hold = At( 10.0f, 10.0f );
        Draw( ctx, n.Registry, n.Canvas, a, &hold );

        UIInput release       = At( 10.0f, 10.0f, /*down=*/false );
        release.MouseReleased = true;
        std::string clicked;
        Draw( ctx, n.Registry, n.Canvas, b, &release, &clicked );
        return clicked == kFired;
    }

    // Did the KEYBOARD? Frame one presses Tab, which fills the focus list and moves focus into it; frame two
    // presses Enter. The pointer is parked at (900,900) — over the panel, never over the button — and never
    // released, so nothing here can fire through the pointer path by accident.
    bool KeyboardFires( ECS::UIHitTest hit )
    {
        Nested n;
        n.SetHitTest( n.Panel, hit );
        ArmButton( n );

        UIViewContext   ctx;
        R2D::DrawList2D a, b;
        entt::entity    focused = entt::null;

        UIInput tab = At( 900.0f, 900.0f, /*down=*/false );
        tab.Tab     = true;
        Draw( ctx, n.Registry, n.Canvas, a, &tab, nullptr, &focused );

        UIInput enter = At( 900.0f, 900.0f, /*down=*/false );
        enter.Submit  = true;
        std::string clicked;
        Draw( ctx, n.Registry, n.Canvas, b, &enter, &clicked, &focused );
        return clicked == kFired;
    }

    // Where Tab PARKED the focus, with the panel set to @p hit. Separate from KeyboardFires because the two
    // gates are separate: Enter being inert on an unreachable control and Tab refusing to stop on it are
    // different properties, and a build with only the first still makes the user press Tab twice to get past
    // a control they cannot use. Measured: gating Enter alone leaves every assertion in (17) green.
    entt::entity FocusAfterTab( Nested& n, ECS::UIHitTest hit )
    {
        n.SetHitTest( n.Panel, hit );

        UIViewContext   ctx;
        R2D::DrawList2D dl;
        entt::entity    focused = entt::null;
        UIInput         tab     = At( 900.0f, 900.0f, /*down=*/false );
        tab.Tab                 = true;
        Draw( ctx, n.Registry, n.Canvas, dl, &tab, nullptr, &focused );
        return focused;
    }
} // namespace

// --- (17) The relation, over all four values ------------------------------------------------------------
TEST( UICanvasHitTest, TheKeyboardReachesExactlyWhatThePointerReaches )
{
    for ( const ECS::UIHitTest hit :
          { ECS::UIHitTest::All, ECS::UIHitTest::ChildrenOnly, ECS::UIHitTest::Blocking, ECS::UIHitTest::None } )
    {
        const bool pointer  = PointerFires( hit );
        const bool keyboard = KeyboardFires( hit );
        EXPECT_EQ( pointer, keyboard )
             << "with the ancestor's Hit Test = " << static_cast<int>( hit ) << " the pointer "
             << ( pointer ? "could" : "could not" ) << " fire the button and the keyboard "
             << ( keyboard ? "could" : "could not" )
             << " — the two paths must agree, and the greyed-out "
                "modal is exactly the case where a keyboard that disagrees is the whole defect";
    }

    // THE PINNED ROWS. An equality is satisfied just as well by both paths being dead, so say which way
    // round each end is. All must fire through both doors; Blocking must fire through neither.
    EXPECT_TRUE( PointerFires( ECS::UIHitTest::All ) ) << "the pointer stopped working entirely";
    EXPECT_TRUE( KeyboardFires( ECS::UIHitTest::All ) ) << "Tab+Enter no longer reaches a plain button";
    EXPECT_FALSE( KeyboardFires( ECS::UIHitTest::Blocking ) )
         << "Tab walked into a Blocking panel and Enter fired the button inside it";
}

// --- (17b) And Tab does not even STOP on a control the pointer cannot reach ------------------------------
//
// The focus LIST is gated as well as the activation, and this is the assertion that says so: with Enter
// alone gated, a Blocking panel still swallows a Tab stop — focus lands on something inert and the author
// has to press Tab twice to get anywhere, with nothing on screen explaining it. Two gates, two properties.
TEST( UICanvasHitTest, TabDoesNotStopOnAControlThePointerCannotReach )
{
    Nested all, blocking, none;
    EXPECT_EQ( FocusAfterTab( all, ECS::UIHitTest::All ), all.Button ) << "Tab no longer reaches a plain button";
    EXPECT_TRUE( FocusAfterTab( blocking, ECS::UIHitTest::Blocking ) == entt::null )
         << "Tab parked focus inside a Blocking panel";
    EXPECT_TRUE( FocusAfterTab( none, ECS::UIHitTest::None ) == entt::null )
         << "Tab parked focus inside a HitTest::None sub-tree";
}

// --- (17c) A focus the host already held does not fire Enter either -------------------------------------
//
// The focus list gate (17b) only decides where Tab can GO. `focused` belongs to the host and survives
// frames, so the case it cannot cover is a control that held focus legitimately and had an ancestor turned
// Blocking under it afterwards — a modal opening over a form is exactly that, and it has no pointer
// equivalent to compare against. Hence the second gate, on the focus test itself, and hence this test:
// removing it leaves (17) and (17b) entirely green.
TEST( UICanvasHitTest, EnterOnAFocusHeldFromBeforeDoesNotFireAnUnreachableButton )
{
    auto fires = []( ECS::UIHitTest hit )
    {
        Nested n;
        n.SetHitTest( n.Panel, hit );
        ArmButton( n );

        entt::entity    focused = n.Button; // handed, not tabbed: the panel changed under a live focus
        UIViewContext   ctx;
        R2D::DrawList2D dl;
        std::string     clicked;
        UIInput         enter = At( 900.0f, 900.0f, /*down=*/false );
        enter.Submit          = true;
        Draw( ctx, n.Registry, n.Canvas, dl, &enter, &clicked, &focused );
        return clicked == kFired;
    };

    EXPECT_TRUE( fires( ECS::UIHitTest::All ) ) << "Enter stopped working on a reachable focused button";
    EXPECT_FALSE( fires( ECS::UIHitTest::Blocking ) )
         << "Enter fired a button inside a Blocking panel because focus predated the panel's change";
    EXPECT_FALSE( fires( ECS::UIHitTest::None ) ) << "Enter fired a button inside a HitTest::None sub-tree";
}

// --- (18) The fourth keyboard door: typing ---------------------------------------------------------------
//
// Enter is not the only key that reaches a control. A focused UIInputField consumes TypedText and Backspace,
// and `focused` is the HOST's — it survives frames — so the case that has no pointer analogue at all is a
// field that held focus legitimately and then had an ancestor turned Blocking under it. Nothing in the
// pointer path can express that, which is why it is a test of its own rather than a row in (17).
TEST( UICanvasHitTest, AFieldOutOfTheHitTestsReachStopsAcceptingTypedText )
{
    auto typeInto = [&]( ECS::UIHitTest hit ) -> std::string
    {
        Nested n;
        n.SetHitTest( n.Panel, hit );

        // A field beside the button, inside the same panel.
        const entt::entity field = n.Registry.create();
        auto&              L     = n.Registry.emplace<ECS::UILayoutComponent>( field ).Data;
        L.AnchorMin              = { 0.0f, 0.0f };
        L.AnchorMax              = { 0.0f, 0.0f };
        L.OffsetMin              = { 0.0f, 100.0f };
        L.OffsetMax              = { 200.0f, 140.0f };
        n.Registry.emplace<ECS::UIInputFieldComponent>( field );
        n.Registry.emplace<ECS::RelationshipComponent>( field ).Parent = n.Panel;
        n.Registry.get<ECS::RelationshipComponent>( n.Panel ).Children.push_back( field );

        // Focus is HANDED to the field rather than tabbed to, which is the stale-focus case: it is what a
        // host holds after the field was legitimately focused and the panel changed afterwards.
        entt::entity    focused = field;
        UIViewContext   ctx;
        R2D::DrawList2D dl;
        UIInput         keys = At( 900.0f, 900.0f, /*down=*/false );
        keys.TypedText       = "x";
        Draw( ctx, n.Registry, n.Canvas, dl, &keys, nullptr, &focused );
        return n.Registry.get<ECS::UIInputFieldComponent>( field ).Data.Text;
    };

    EXPECT_EQ( typeInto( ECS::UIHitTest::All ), "x" ) << "a reachable field stopped accepting text";
    EXPECT_EQ( typeInto( ECS::UIHitTest::Blocking ), "" )
         << "a field inside a Blocking panel took keystrokes the pointer could never have delivered to it";
    EXPECT_EQ( typeInto( ECS::UIHitTest::None ), "" ) << "a field inside a HitTest::None sub-tree took keystrokes";
}

// =========================================================================================================
// Ю1 — THE CANVAS IS ASKED. RenderCanvas2D and the layout queries used to elect
// `*reg.view<UICanvasComponent>().begin()`, so a scene's second canvas was drawn by nothing, picked by
// nothing and measured by nothing, silently. These assert the relation "what you ask for is what you get",
// which is the only claim that a build electing the first canvas cannot satisfy.
// =========================================================================================================

namespace
{
    // Two canvases in one registry, each with a full-canvas panel of its own colour, so which canvas was
    // drawn is a question the vertex buffer answers.
    struct TwoCanvases
    {
        entt::registry Registry;
        entt::entity   CanvasA = entt::null, CanvasB = entt::null;
        entt::entity   PanelA = entt::null, PanelB = entt::null;

        static glm::vec3 ColorA()
        {
            return { 1.0f, 0.0f, 0.0f };
        }
        static glm::vec3 ColorB()
        {
            return { 0.0f, 1.0f, 0.0f };
        }

        TwoCanvases()
        {
            CanvasA = Make( ColorA(), PanelA );
            CanvasB = Make( ColorB(), PanelB );
        }

    private:
        entt::entity Make( const glm::vec3& rgb, entt::entity& panelOut )
        {
            const entt::entity canvas = Registry.create();
            auto&              cd     = Registry.emplace<ECS::UICanvasComponent>( canvas ).Data;
            cd.ScaleMode              = ECS::UICanvasScaleMode::Stretch;
            cd.ReferenceWidth         = kSide;
            cd.ReferenceHeight        = kSide;

            panelOut         = Registry.create();
            auto& layout     = Registry.emplace<ECS::UILayoutComponent>( panelOut ).Data;
            layout.AnchorMin = { 0.0f, 0.0f };
            layout.AnchorMax = { 1.0f, 1.0f };
            layout.OffsetMin = { 0.0f, 0.0f };
            layout.OffsetMax = { 0.0f, 0.0f };

            auto& p        = Registry.emplace<ECS::UIPanelComponent>( panelOut ).Data;
            p.Color        = rgb;
            p.Opacity      = 1.0f;
            p.CornerRadius = 0.0f;

            Registry.emplace<ECS::RelationshipComponent>( canvas ).Children.push_back( panelOut );
            Registry.emplace<ECS::RelationshipComponent>( panelOut ).Parent = canvas;
            return canvas;
        }
    };
} // namespace

// --- (19) Whichever canvas is named is the one that draws ------------------------------------------------
TEST( UICanvasSelection, TheCanvasThatWasAskedForIsTheOneDrawn )
{
    TwoCanvases t;

    UIViewContext   ctxA, ctxB;
    R2D::DrawList2D a, b;
    EXPECT_TRUE( Draw( ctxA, t.Registry, t.CanvasA, a ) );
    EXPECT_TRUE( Draw( ctxB, t.Registry, t.CanvasB, b ) );

    EXPECT_TRUE( RectOfColor( a, TwoCanvases::ColorA() ).has_value() );
    EXPECT_FALSE( RectOfColor( a, TwoCanvases::ColorB() ).has_value() )
         << "asking for canvas A drew canvas B's content too";

    // THE HALF THAT WAS BROKEN. On the electing build this one is empty: B is not `*view.begin()`, so
    // whatever the caller asked for, A came back.
    EXPECT_TRUE( RectOfColor( b, TwoCanvases::ColorB() ).has_value() )
         << "the second canvas was asked for and something else was drawn — this is the whole defect";
    EXPECT_FALSE( RectOfColor( b, TwoCanvases::ColorA() ).has_value() );
}

// --- (20) The layout queries answer about the SAME canvas the draw did ----------------------------------
//
// Pick and draw disagreeing is this project's recurring shape, and a second canvas is a fresh way to get it:
// the editor's pick used to walk canvas A whatever was on screen, so an element of canvas B was drawn where
// nothing could select it.
TEST( UICanvasSelection, ThePickAndTheScaleAnswerAboutTheCanvasTheyWereGiven )
{
    TwoCanvases t;

    EXPECT_EQ( Desert::UI::PickElement( t.Registry, t.CanvasB, { 500.0f, 500.0f }, kViewport ), t.PanelB );
    EXPECT_EQ( Desert::UI::PickElement( t.Registry, t.CanvasA, { 500.0f, 500.0f }, kViewport ), t.PanelA );

    Rect r;
    EXPECT_TRUE( Desert::UI::GetElementRect( t.Registry, t.CanvasB, t.PanelB, kViewport, r ) );
    EXPECT_FALSE( Desert::UI::GetElementRect( t.Registry, t.CanvasA, t.PanelB, kViewport, r ) )
         << "canvas A reported a rect for an element that is not in it";

    const auto scale = Desert::UI::CanvasScale( t.Registry, t.CanvasB, kViewport );
    ASSERT_TRUE( scale.IsSuccess() ) << scale.GetError();
    EXPECT_FLOAT_EQ( scale.GetValue(), 1.0f ); // Stretch
}

// --- (21) Not naming one is a refusal, never a default --------------------------------------------------
//
// The contract's §1.4 case: "there are two and I drew one of them" is a successful-looking answer to a
// question nobody could have asked. Both the renderer and the resolver have to say so out loud.
TEST( UICanvasSelection, NotNamingACanvasIsARefusalAndNotTheFirstOne )
{
    TwoCanvases t;

    UIViewContext   ctx;
    R2D::DrawList2D dl;
    const auto      unnamed = Desert::UI::RenderCanvas2D( ctx, t.Registry, entt::null, dl );
    EXPECT_FALSE( unnamed.IsSuccess() ) << "RenderCanvas2D accepted no canvas and drew something anyway";
    EXPECT_TRUE( dl.GetVertices().empty() ) << "a refused walk still emitted geometry";

    // An entity that exists but is not a canvas is a DIFFERENT refusal — a caller bug, not an empty scene.
    R2D::DrawList2D dl2;
    const auto      notACanvas = Desert::UI::RenderCanvas2D( ctx, t.Registry, t.PanelA, dl2 );
    EXPECT_FALSE( notACanvas.IsSuccess() );
    EXPECT_NE( notACanvas.GetError(), unnamed.GetError() )
         << "'you named nothing' and 'you named a panel' came back as the same sentence";

    // And the resolver refuses to break the tie rather than handing back the first.
    EXPECT_EQ( Desert::UI::CanvasCount( t.Registry ), 2u );
    EXPECT_FALSE( Desert::UI::SoleCanvas( t.Registry ).IsSuccess() )
         << "SoleCanvas elected a winner out of two canvases — the exact behaviour this task removed";

    entt::registry empty;
    EXPECT_EQ( Desert::UI::CanvasCount( empty ), 0u );
    EXPECT_FALSE( Desert::UI::SoleCanvas( empty ).IsSuccess() );
}

// --- (22) CanvasOf derives the answer instead of guessing it --------------------------------------------
//
// This is what the editor asks: an element already names its canvas by being inside it. It is exact, and it
// is the reason the viewport no longer needs an election at all when something is selected.
TEST( UICanvasSelection, CanvasOfWalksUpToTheCanvasTheElementIsActuallyIn )
{
    TwoCanvases t;

    EXPECT_EQ( Desert::UI::CanvasOf( t.Registry, t.PanelB ), t.CanvasB );
    EXPECT_EQ( Desert::UI::CanvasOf( t.Registry, t.PanelA ), t.CanvasA );
    EXPECT_EQ( Desert::UI::CanvasOf( t.Registry, t.CanvasB ), t.CanvasB ) << "a canvas is its own canvas";

    const entt::entity orphan = t.Registry.create();
    EXPECT_TRUE( Desert::UI::CanvasOf( t.Registry, orphan ) == entt::null );
    EXPECT_TRUE( Desert::UI::CanvasOf( t.Registry, entt::null ) == entt::null );

    // A parent cycle is authorable (the hierarchy panel reparents), and this must return rather than hang.
    const entt::entity a = t.Registry.create(), b = t.Registry.create();
    t.Registry.emplace<ECS::RelationshipComponent>( a ).Parent = b;
    t.Registry.emplace<ECS::RelationshipComponent>( b ).Parent = a;
    EXPECT_TRUE( Desert::UI::CanvasOf( t.Registry, a ) == entt::null );
}

// =========================================================================================================
// Ю8 — THE RENDER TRANSFORM, AND THE ONE THING THAT HAD TO BE TESTED ABOUT IT
//
// A rotated element has two halves that can each be right on their own: the quad that reaches the screen,
// and the region the pointer is accepted in. Testing them separately is exactly the mistake this project
// keeps paying for — so what is asserted below is their AGREEMENT, and it is asserted against the geometry
// the walk actually emitted rather than against a rect recomputed by the test.
//
// The drawn quad is read out of the draw list. The elected region is read out of the context. For a grid of
// sample points the two must give the same answer at every point; a rotation applied to one and not the
// other reddens this at roughly a quarter of the samples, and applying it in the WRONG DIRECTION to the
// pointer reddens it too (an inverse-vs-forward slip is the likely defect here, and it is symmetric about
// the pivot, so a centre-pivot test alone would pass — which is why the pivot below is a corner).
// =========================================================================================================

namespace
{
    // A canvas with ONE panel, sharp-cornered so it emits exactly one quad and the first four vertices of
    // the list ARE its screen corners. Rotation / Scale / Pivot are the test's to set.
    struct XformFixture
    {
        entt::registry Registry;
        entt::entity   Canvas = entt::null;
        entt::entity   Panel  = entt::null;

        XformFixture()
        {
            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;

            Panel            = Registry.create();
            auto& layout     = Registry.emplace<ECS::UILayoutComponent>( Panel ).Data;
            layout.AnchorMin = { 0.0f, 0.0f };
            layout.AnchorMax = { 0.0f, 0.0f };
            layout.OffsetMin = { 200.0f, 300.0f };
            layout.OffsetMax = { 500.0f, 420.0f };

            auto& panel        = Registry.emplace<ECS::UIPanelComponent>( Panel ).Data;
            panel.CornerRadius = 0.0f; // sharp => AddRectFilled takes the four-vertex path
            panel.Opacity      = 1.0f;

            Registry.emplace<ECS::RelationshipComponent>( Canvas ).Children.push_back( Panel );
            Registry.emplace<ECS::RelationshipComponent>( Panel ).Parent = Canvas;
        }

        ECS::UILayoutData& Layout( entt::entity e )
        {
            return Registry.get<ECS::UILayoutComponent>( e ).Data;
        }
    };

    // Where @p p sits relative to the convex quad @p q (given in order). All four cross products share a
    // sign for a point inside, whichever way round the quad is wound — which matters because a negative
    // scale flips the winding.
    //
    // ON_EDGE IS A THIRD ANSWER AND IT IS NOT A HEDGE. A sample within half a pixel of a rotated edge is
    // a tie the two sides settle differently for reasons that are not this test's subject: the pointer
    // test is closed on both bounds (`>=` and `<=`) while the sign of a cross product a few ulps from
    // zero is whatever the rotation's rounding made it. The pivot is itself a CORNER of the quad, so
    // there is always at least one such sample. Ties are skipped and counted; the assertion is about
    // every point that is unambiguously in or out.
    enum class Where
    {
        Inside,
        Outside,
        OnEdge
    };

    Where WhereInQuad( const std::array<glm::vec2, 4>& q, const glm::vec2& p )
    {
        int   positive = 0, negative = 0;
        float nearest = 1e30f;
        for ( int i = 0; i < 4; ++i )
        {
            const glm::vec2 a   = q[i];
            const glm::vec2 b   = q[( i + 1 ) % 4];
            const float     c   = ( b.x - a.x ) * ( p.y - a.y ) - ( b.y - a.y ) * ( p.x - a.x );
            const float     len = glm::length( b - a );
            if ( len > 0.0f )
                nearest = std::min( nearest, std::fabs( c ) / len ); // px from the edge's line
            if ( c > 0.0f )
                ++positive;
            if ( c < 0.0f )
                ++negative;
        }
        if ( nearest < 0.5f )
            return Where::OnEdge;
        return ( positive == 0 || negative == 0 ) ? Where::Inside : Where::Outside;
    }

    // The first four vertices of the list, i.e. the panel's quad where it landed on screen.
    std::array<glm::vec2, 4> DrawnQuad( const R2D::DrawList2D& dl )
    {
        std::array<glm::vec2, 4> q{};
        EXPECT_GE( dl.GetVertices().size(), 4u ) << "the panel emitted no quad at all";
        for ( std::size_t i = 0; i < 4 && i < dl.GetVertices().size(); ++i )
            q[i] = dl.GetVertices()[i].Position;
        return q;
    }

    // Walk once with the pointer at @p p and answer whether the walk elected @p e. One frame is enough:
    // the election is finished by the time RenderCanvas2D returns (ctx.Hot = ctx.HotNext).
    bool ElectsAt( XformFixture& f, entt::entity e, const glm::vec2& p )
    {
        UIViewContext   ctx;
        R2D::DrawList2D dl;
        const UIInput   in = At( p.x, p.y, /*down=*/false );
        Draw( ctx, f.Registry, f.Canvas, dl, &in );
        return ctx.Hot == e;
    }
} // namespace

// The relation, over a grid dense enough to straddle every edge of a turned rectangle.
TEST( UICanvasContext, WhereARotatedElementIsDrawnIsWhereItTakesThePointer )
{
    XformFixture f;
    f.Layout( f.Panel ).Rotation = 30.0f;
    f.Layout( f.Panel ).Pivot    = { 0.0f, 0.0f }; // a CORNER: an inverse/forward slip is not symmetric here

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    Draw( ctx, f.Registry, f.Canvas, dl, nullptr );
    const std::array<glm::vec2, 4> quad = DrawnQuad( dl );

    // The quad really did move — otherwise this test would be asserting agreement about nothing.
    ASSERT_GT( std::fabs( quad[0].y - quad[1].y ), 1.0f ) << "the panel was drawn axis-aligned";

    int inside = 0, disagreements = 0, ties = 0;
    for ( float y = 20.0f; y < 900.0f; y += 20.0f )
        for ( float x = 20.0f; x < 900.0f; x += 20.0f )
        {
            const glm::vec2 p     = { x, y };
            const Where     where = WhereInQuad( quad, p );
            if ( where == Where::OnEdge )
            {
                ++ties;
                continue;
            }
            const bool drawn   = where == Where::Inside;
            const bool elected = ElectsAt( f, f.Panel, p );
            inside += drawn ? 1 : 0;
            if ( drawn != elected )
            {
                ++disagreements;
                if ( disagreements <= 5 )
                    ADD_FAILURE() << "at (" << x << "," << y << ") the element is "
                                  << ( drawn ? "DRAWN but not electable" : "electable but NOT DRAWN" );
            }
        }

    EXPECT_EQ( disagreements, 0 );
    EXPECT_GT( inside, 40 ) << "the sample grid never landed on the element, so it proved nothing";
    // The half-pixel tolerance must stay a minority report, or it would be the tolerance being measured
    // rather than the agreement. Expressed against the element's own sample count rather than as a
    // number, because that is the quantity it has to be small compared to.
    EXPECT_LT( ties * 4, inside ) << ties << " of the samples were within half a pixel of an edge, against "
                                  << inside << " unambiguously inside";
}

// Propagation, and it is the relation again one level down: the child states no transform of its own, so
// everything about where it is drawn AND where it is clickable comes from its parent.
TEST( UICanvasContext, AChildOfARotatedParentIsDrawnAndPickedWhereTheParentCarriedIt )
{
    XformFixture       f;
    const entt::entity child = f.Registry.create();
    auto&              cl    = f.Registry.emplace<ECS::UILayoutComponent>( child ).Data;
    cl.AnchorMin             = { 0.0f, 0.0f };
    cl.AnchorMax             = { 0.0f, 0.0f };
    cl.OffsetMin             = { 20.0f, 20.0f };
    cl.OffsetMax             = { 120.0f, 70.0f };
    auto& cp                 = f.Registry.emplace<ECS::UIPanelComponent>( child ).Data;
    cp.CornerRadius          = 0.0f;
    f.Registry.emplace<ECS::RelationshipComponent>( child ).Parent = f.Panel;
    f.Registry.get<ECS::RelationshipComponent>( f.Panel ).Children.push_back( child );

    // Where is the child's centre with the parent straight? Read it from the drawing, not computed here.
    glm::vec2 straightCentre;
    {
        R2D::DrawList2D dl;
        UIViewContext   ctx;
        Draw( ctx, f.Registry, f.Canvas, dl, nullptr );
        ASSERT_GE( dl.GetVertices().size(), 8u ); // parent quad, then the child's
        straightCentre = ( dl.GetVertices()[4].Position + dl.GetVertices()[6].Position ) * 0.5f;
    }

    f.Layout( f.Panel ).Rotation = 90.0f; // the PARENT turns; the child says nothing about transforms
    glm::vec2                turnedCentre;
    std::array<glm::vec2, 4> childQuad{};
    {
        R2D::DrawList2D dl;
        UIViewContext   ctx;
        Draw( ctx, f.Registry, f.Canvas, dl, nullptr );
        ASSERT_GE( dl.GetVertices().size(), 8u );
        for ( int i = 0; i < 4; ++i )
            childQuad[i] = dl.GetVertices()[4 + i].Position;
        turnedCentre = ( childQuad[0] + childQuad[2] ) * 0.5f;
    }

    // A quarter turn about the parent's centre (350,360) sends the child's centre from (270,345) to
    // (365,280) — written out rather than derived, so an error in the composition cannot cancel itself.
    EXPECT_NEAR( straightCentre.x, 270.0f, 1e-2f );
    EXPECT_NEAR( straightCentre.y, 345.0f, 1e-2f );
    EXPECT_NEAR( turnedCentre.x, 365.0f, 1e-2f );
    EXPECT_NEAR( turnedCentre.y, 280.0f, 1e-2f );

    // And the pointer followed it: the child is electable where it now is and not where it used to be.
    EXPECT_TRUE( ElectsAt( f, child, turnedCentre ) )
         << "the child was drawn at its parent's rotation but does not take the pointer there";
    EXPECT_FALSE( ElectsAt( f, child, straightCentre ) )
         << "the child still takes the pointer at the place it was drawn BEFORE the parent turned";

    // The child's own quad is a 100x50 rectangle whichever way it is turned — a parent transform must
    // carry a child, not restretch it.
    EXPECT_NEAR( glm::length( childQuad[1] - childQuad[0] ), 100.0f, 1e-2f );
    EXPECT_NEAR( glm::length( childQuad[3] - childQuad[0] ), 50.0f, 1e-2f );
}

// PICKELEMENT IS A SECOND IMPLEMENTATION OF THE SAME QUESTION (the editor's WYSIWYG select), and the
// header of UICanvasLayout.hpp says in as many words that the two must not drift. Under a transform they
// have a new way to drift, so the agreement is pinned here too.
TEST( UICanvasContext, TheEditorsPickAgreesWithTheWalkAboutARotatedElement )
{
    XformFixture f;
    f.Layout( f.Panel ).Rotation = -40.0f;
    f.Layout( f.Panel ).Scale    = { 1.3f, 0.7f };
    f.Layout( f.Panel ).Pivot    = { 1.0f, 0.0f };

    int disagreements = 0, hits = 0;
    for ( float y = 20.0f; y < 900.0f; y += 25.0f )
        for ( float x = 20.0f; x < 900.0f; x += 25.0f )
        {
            const bool picked  = Desert::UI::PickElement( f.Registry, f.Canvas, { x, y }, kViewport ) == f.Panel;
            const bool elected = ElectsAt( f, f.Panel, { x, y } );
            hits += picked ? 1 : 0;
            if ( picked != elected )
            {
                ++disagreements;
                if ( disagreements <= 5 )
                    ADD_FAILURE() << "at (" << x << "," << y << ") the editor pick says " << picked
                                  << " and the renderer's election says " << elected;
            }
        }
    EXPECT_EQ( disagreements, 0 );
    EXPECT_GT( hits, 40 ) << "the grid never hit the element, so the agreement was vacuous";
}

// PIVOT WITH ITS CONSUMER. Д26 deleted this field because nothing read it; the assertion that it is not
// dead again is that the SAME rotation about two different pivots puts the element in two different
// places — and that both places are hit-testable, so it moved the pointer with the picture.
TEST( UICanvasContext, TheSameRotationAboutTwoPivotsLandsInTwoPlaces )
{
    const auto centreOfPanelWith = []( const glm::vec2& pivot )
    {
        XformFixture f;
        f.Layout( f.Panel ).Rotation = 45.0f;
        f.Layout( f.Panel ).Pivot    = pivot;
        R2D::DrawList2D dl;
        UIViewContext   ctx;
        Draw( ctx, f.Registry, f.Canvas, dl, nullptr );
        EXPECT_GE( dl.GetVertices().size(), 4u );
        return ( dl.GetVertices()[0].Position + dl.GetVertices()[2].Position ) * 0.5f;
    };

    const glm::vec2 aboutCentre = centreOfPanelWith( { 0.5f, 0.5f } );
    const glm::vec2 aboutCorner = centreOfPanelWith( { 0.0f, 0.0f } );

    // About its own centre the element does not move at all; about its top-left corner it swings away.
    EXPECT_NEAR( aboutCentre.x, 350.0f, 1e-2f );
    EXPECT_NEAR( aboutCentre.y, 360.0f, 1e-2f );
    EXPECT_GT( glm::length( aboutCorner - aboutCentre ), 50.0f )
         << "Pivot did not move the picture, which is what got the field deleted in the first place";

    // ...and the pointer went with it, at both pivots.
    {
        XformFixture f;
        f.Layout( f.Panel ).Rotation = 45.0f;
        f.Layout( f.Panel ).Pivot    = { 0.0f, 0.0f };
        EXPECT_TRUE( ElectsAt( f, f.Panel, aboutCorner ) );
        EXPECT_FALSE( ElectsAt( f, f.Panel, aboutCentre ) )
             << "the corner-pivot element still takes the pointer where a centre-pivot one would be";
    }
}

// =========================================================================================================
// Ю9 — A ROTATED CLIPPER CUTS THE QUADRILATERAL, NOT THE BOX AROUND IT, AND THE POINTER IS CUT WITH IT.
//
// Ю8 shipped the box and recorded the looseness as a decision, pinned by a test that named it. This is that
// decision closed, and the test that named it is replaced rather than kept beside its successor.
//
// The assertions below are all of ONE shape and it is not "the clip is at coordinates X": it is that the
// set of points the walk elects and the set of points the emitted TRIANGLES cover are the same set. Either
// half alone is satisfiable by a wrong clip that is wrong twice.
// =========================================================================================================

namespace
{
    // Is @p p covered by a triangle the draw list emitted whose corners all carry @p color? The colour is
    // how one element's geometry is told from another's without the test knowing the emission order — and
    // it is exact equality, because the fixture's colours are authored, not blended.
    bool CoveredByColor( const R2D::DrawList2D& dl, const glm::vec2& p, const glm::vec3& color )
    {
        const auto& v = dl.GetVertices();
        const auto& i = dl.GetIndices();
        for ( std::size_t t = 0; t + 2 < i.size(); t += 3 )
        {
            const auto& a = v[i[t]];
            const auto& b = v[i[t + 1]];
            const auto& c = v[i[t + 2]];
            if ( glm::vec3( a.Color ) != color || glm::vec3( b.Color ) != color || glm::vec3( c.Color ) != color )
                continue;
            const float e0 = ( b.Position.x - a.Position.x ) * ( p.y - a.Position.y ) -
                             ( b.Position.y - a.Position.y ) * ( p.x - a.Position.x );
            const float e1 = ( c.Position.x - b.Position.x ) * ( p.y - b.Position.y ) -
                             ( c.Position.y - b.Position.y ) * ( p.x - b.Position.x );
            const float e2 = ( a.Position.x - c.Position.x ) * ( p.y - c.Position.y ) -
                             ( a.Position.y - c.Position.y ) * ( p.x - c.Position.x );
            if ( ( e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f ) || ( e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f ) )
                return true;
        }
        return false;
    }

    // A child panel of @p parent, sharp-cornered, in its own authored colour.
    entt::entity AddChildPanel( XformFixture& f, entt::entity parent, const glm::vec2& mn, const glm::vec2& mx,
                                const glm::vec3& color )
    {
        const entt::entity child = f.Registry.create();
        auto&              cl    = f.Registry.emplace<ECS::UILayoutComponent>( child ).Data;
        cl.AnchorMin             = { 0.0f, 0.0f };
        cl.AnchorMax             = { 0.0f, 0.0f };
        cl.OffsetMin             = mn;
        cl.OffsetMax             = mx;
        auto& panel              = f.Registry.emplace<ECS::UIPanelComponent>( child ).Data;
        panel.CornerRadius       = 0.0f;
        panel.Color              = color;
        panel.Opacity            = 1.0f;
        f.Registry.emplace<ECS::RelationshipComponent>( child ).Parent = parent;
        f.Registry.get<ECS::RelationshipComponent>( parent ).Children.push_back( child );
        return child;
    }

    // Sweep a grid and compare the two halves. Returns the number of points the picture covered, so a
    // caller can refuse a vacuous agreement (two empty sets agree perfectly).
    int SweepAgreement( XformFixture& f, entt::entity target, const glm::vec3& color, float step )
    {
        R2D::DrawList2D dl;
        UIViewContext   ctx;
        Draw( ctx, f.Registry, f.Canvas, dl, nullptr );

        int covered = 0, disagreements = 0;
        for ( float y = 5.0f; y < kSide; y += step )
            for ( float x = 5.0f; x < kSide; x += step )
            {
                const glm::vec2 p( x, y );
                const bool      drawn   = CoveredByColor( dl, p, color );
                const bool      elected = ElectsAt( f, target, p );
                covered += drawn ? 1 : 0;
                if ( drawn != elected && ++disagreements <= 5 )
                    ADD_FAILURE() << "at (" << x << "," << y << ") the pixels say " << drawn
                                  << " and the pointer says " << elected;
            }
        EXPECT_EQ( disagreements, 0 );
        return covered;
    }
} // namespace

// ONE LEVEL. The clipper is turned 45 degrees and its child overflows it in every direction, so the corners
// of the clipper's BOUNDING BOX are precisely the places the two answers can differ: under Ю8 the pixels
// survived there and the pointer was accepted there; now neither is.
TEST( UICanvasContext, ARotatedClipperRefusesThePointerExactlyWhereItCutThePixels )
{
    XformFixture    f;
    const glm::vec3 kChild( 0.9f, 0.2f, 0.7f );
    f.Layout( f.Panel ).ClipContents = true;
    f.Layout( f.Panel ).Rotation     = 45.0f;
    const entt::entity child = AddChildPanel( f, f.Panel, { -200.0f, -200.0f }, { 500.0f, 400.0f }, kChild );

    EXPECT_GT( SweepAgreement( f, child, kChild, 7.0f ), 200 ) << "the child was never drawn at all";

    // Not vacuous in the OTHER direction either: there is a point inside the clipper's box and outside the
    // clipper itself, and it must now be refused by both halves. Under Ю8 both accepted it.
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    Draw( ctx, f.Registry, f.Canvas, dl, nullptr );
    glm::vec4 box{ 0.0f };
    for ( const auto& cmd : dl.GetCommands() )
        if ( cmd.ClipRect.z > 0.0f )
            box = cmd.ClipRect;
    ASSERT_GT( box.z, 0.0f ) << "nothing was clipped at all";
    const glm::vec2 boxCorner( box.x + 2.0f, box.y + 2.0f ); // inside the box, outside the 45-degree diamond
    EXPECT_FALSE( CoveredByColor( dl, boxCorner, kChild ) ) << "a pixel survived in the corner of the box";
    EXPECT_FALSE( ElectsAt( f, child, boxCorner ) ) << "the pointer was accepted in the corner of the box";
    // ...while the middle of the same box is inside the clipper and kept by both.
    const glm::vec2 middle( box.x + box.z * 0.5f, box.y + box.w * 0.5f );
    EXPECT_TRUE( CoveredByColor( dl, middle, kChild ) );
    EXPECT_TRUE( ElectsAt( f, child, middle ) );
}

// TWO LEVELS, BOTH TURNED, AND THE TRAP Ю8 NAMED. Its own propagation test did not redden on the mutation
// it existed for, because a stack one deep cannot tell composition from replacement. So here BOTH the outer
// panel and the inner clipper carry a rotation of their own, and the counts at the end assert that the two
// clippers genuinely disagree somewhere — without which "intersect" and "keep the inner one" are the same
// picture and the mutation is equivalent rather than survived.
TEST( UICanvasContext, TwoRotatedClippersNestAsAnIntersectionOfBothQuadrilaterals )
{
    XformFixture    f;
    const glm::vec3 kLeaf( 0.15f, 0.85f, 0.35f );

    // Outer: the fixture's panel, turned and clipping.
    f.Layout( f.Panel ).ClipContents = true;
    f.Layout( f.Panel ).Rotation     = -35.0f;
    f.Layout( f.Panel ).Pivot        = { 0.5f, 0.5f };

    // Inner: a clipper of its own, turned the other way, deliberately hanging out of the outer one.
    const entt::entity inner  = AddChildPanel( f, f.Panel, { 60.0f, -40.0f }, { 420.0f, 160.0f }, kLeaf );
    auto&              innerL = f.Registry.get<ECS::UILayoutComponent>( inner ).Data;
    innerL.ClipContents       = true;
    innerL.Rotation           = 55.0f;
    innerL.Pivot              = { 0.5f, 0.5f };
    // The leaf overflows the inner clipper in every direction, so what survives is decided by the clips.
    const entt::entity leaf = AddChildPanel( f, inner, { -300.0f, -300.0f }, { 700.0f, 600.0f }, kLeaf );

    EXPECT_GT( SweepAgreement( f, leaf, kLeaf, 5.0f ), 150 ) << "the leaf was never drawn at all";

    // AND THE COUNTS THAT MAKE IT NON-EQUIVALENT. Drawn with only the inner clip, the leaf would cover
    // strictly more than it does now — the outer clipper must remove something. Measured by taking the
    // outer clipper's ClipContents away and counting the difference.
    const auto coveredCount = [&]( XformFixture& fx )
    {
        R2D::DrawList2D dl;
        UIViewContext   ctx;
        Draw( ctx, fx.Registry, fx.Canvas, dl, nullptr );
        int n = 0;
        for ( float y = 5.0f; y < kSide; y += 5.0f )
            for ( float x = 5.0f; x < kSide; x += 5.0f )
                n += CoveredByColor( dl, { x, y }, kLeaf ) ? 1 : 0;
        return n;
    };
    const int both                                                    = coveredCount( f );
    f.Layout( f.Panel ).ClipContents                                  = false;
    const int innerAlone                                              = coveredCount( f );
    f.Layout( f.Panel ).ClipContents                                  = true;
    f.Registry.get<ECS::UILayoutComponent>( inner ).Data.ClipContents = false;
    const int outerAlone                                              = coveredCount( f );

    EXPECT_LT( both, innerAlone ) << "the outer clipper removed nothing, so nesting cannot be told from "
                                     "replacement and this test would pass for the wrong reason";
    EXPECT_LT( both, outerAlone ) << "the inner clipper removed nothing";
}

// THE EDITOR'S PICK IS THE THIRD ANSWER TO THE SAME QUESTION, and until Ю9 it did not ask about clipping at
// all — not even about a straight one. A row scrolled out of its viewport was invisible, refused by the
// renderer's election, and still selectable by clicking where it would have been.
TEST( UICanvasContext, TheEditorsPickIsRefusedByAClipperTheWalkIsRefusedBy )
{
    XformFixture    f;
    const glm::vec3 kChild( 0.9f, 0.2f, 0.7f );
    f.Layout( f.Panel ).ClipContents = true;
    f.Layout( f.Panel ).Rotation     = 45.0f;
    const entt::entity child = AddChildPanel( f, f.Panel, { -200.0f, -200.0f }, { 500.0f, 400.0f }, kChild );

    int disagreements = 0, hits = 0;
    for ( float y = 5.0f; y < kSide; y += 9.0f )
        for ( float x = 5.0f; x < kSide; x += 9.0f )
        {
            const bool picked  = Desert::UI::PickElement( f.Registry, f.Canvas, { x, y }, kViewport ) == child;
            const bool elected = ElectsAt( f, child, { x, y } );
            hits += picked ? 1 : 0;
            if ( picked != elected && ++disagreements <= 5 )
                ADD_FAILURE() << "at (" << x << "," << y << ") the editor pick says " << picked
                              << " and the renderer's election says " << elected;
        }
    EXPECT_EQ( disagreements, 0 );
    EXPECT_GT( hits, 100 ) << "the grid never hit the child, so the agreement is vacuous";
}

// THE OTHER HALF OF THE SAME QUESTION, AND IT HAD NO TEST AT ALL. Ю10 says in as many words that the
// editor's pick used not to apply a ScrollView's scroll offset to its children, and that sharing one walk
// fixed it — but removing the offset line again left every suite in this repository green (measured on the
// Ю9/Ю10 merge: five suites, zero failures). A fix nothing reddens for is a fix nobody can keep.
//
// The row is authored BELOW its list and scrolled up INTO it, so the two answers are not merely a few
// pixels apart: unscrolled it sits entirely outside the clip and is pickable nowhere, scrolled it is in
// the middle of the list. And it is asserted as the relation rather than as a coordinate — the editor's
// pick against the renderer's own election, which is the side that never lost the offset.
TEST( UICanvasContext, TheEditorsPickFollowsARowItsListHasScrolledUnderIt )
{
    XformFixture    f;
    const glm::vec3 kRow( 0.2f, 0.6f, 0.95f );

    // The fixture's panel (200,300)-(500,420) becomes the list: 120 px tall over 400 px of content.
    auto& sv         = f.Registry.emplace<ECS::UIScrollViewComponent>( f.Panel ).Data;
    sv.ContentHeight = 400.0f;
    sv.ScrollY       = 150.0f;

    // Authored at y 480..520 — 60 px BELOW the list's bottom edge. Scrolled by 150 it lands at 330..370,
    // the middle of the list.
    const entt::entity row = AddChildPanel( f, f.Panel, { 20.0f, 180.0f }, { 280.0f, 220.0f }, kRow );

    int disagreements = 0, hits = 0;
    for ( float y = 5.0f; y < kSide; y += 5.0f )
        for ( float x = 5.0f; x < kSide; x += 5.0f )
        {
            const bool picked  = Desert::UI::PickElement( f.Registry, f.Canvas, { x, y }, kViewport ) == row;
            const bool elected = ElectsAt( f, row, { x, y } );
            hits += picked ? 1 : 0;
            if ( picked != elected && ++disagreements <= 5 )
                ADD_FAILURE() << "at (" << x << "," << y << ") the editor pick says " << picked
                              << " and the renderer's election says " << elected;
        }
    EXPECT_EQ( disagreements, 0 );
    EXPECT_GT( hits, 100 ) << "the grid never picked the scrolled row, so the agreement is vacuous";

    // Not vacuous in the other direction either: the place the row would occupy WITHOUT the offset is
    // outside the list, and nothing may be picked there.
    EXPECT_NE( Desert::UI::PickElement( f.Registry, f.Canvas, { 300.0f, 350.0f }, kViewport ),
               entt::entity{ entt::null } )
         << "the scrolled row is not pickable where it is drawn";
    EXPECT_NE( Desert::UI::PickElement( f.Registry, f.Canvas, { 300.0f, 500.0f }, kViewport ), row )
         << "the row was picked at its UNSCROLLED position, 80 px outside the list that clips it";
}

// TWO LEVELS, BOTH TURNED — and this one exists because the single-level test above did NOT catch the
// mutation it should have. Making DrawList2D::PushTransform REPLACE the current matrix instead of
// composing with it left that test green, because its child stated no transform of its own and a stack
// one deep cannot tell replacement from composition. The mutation was EQUIVALENT there, not survived;
// what it needed was a case where the stack is two deep, which is this one.
TEST( UICanvasContext, TwoTurnedLevelsComposeRatherThanReplace )
{
    XformFixture f;
    f.Layout( f.Panel ).Rotation = 30.0f;
    f.Layout( f.Panel ).Pivot    = { 0.5f, 0.5f };

    const entt::entity child = f.Registry.create();
    auto&              cl    = f.Registry.emplace<ECS::UILayoutComponent>( child ).Data;
    cl.AnchorMin             = { 0.0f, 0.0f };
    cl.AnchorMax             = { 0.0f, 0.0f };
    cl.OffsetMin             = { 20.0f, 20.0f };
    cl.OffsetMax             = { 120.0f, 70.0f };
    cl.Rotation              = 60.0f; // 30 + 60 = 90 composed, which is the one angle written down exactly
    cl.Pivot                 = { 0.5f, 0.5f };
    f.Registry.emplace<ECS::UIPanelComponent>( child ).Data.CornerRadius = 0.0f;
    f.Registry.emplace<ECS::RelationshipComponent>( child ).Parent       = f.Panel;
    f.Registry.get<ECS::RelationshipComponent>( f.Panel ).Children.push_back( child );

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    Draw( ctx, f.Registry, f.Canvas, dl, nullptr );
    ASSERT_GE( dl.GetVertices().size(), 8u );

    std::array<glm::vec2, 4> quad{};
    for ( int i = 0; i < 4; ++i )
        quad[i] = dl.GetVertices()[4 + i].Position;

    // The child's own top edge is 100 px long and, at 90 degrees composed, must be VERTICAL. Replacing
    // instead of composing would leave it at the child's own 60 degrees, i.e. 50 px of run.
    const glm::vec2 topEdge = quad[1] - quad[0];
    EXPECT_NEAR( glm::length( topEdge ), 100.0f, 1e-2f );
    EXPECT_NEAR( topEdge.x, 0.0f, 1e-2f ) << "the child was drawn at its own rotation, not at the "
                                             "composition of its own with its parent's";
    EXPECT_NEAR( std::fabs( topEdge.y ), 100.0f, 1e-2f );

    // WHERE the child ends up, worked out by hand so the assertion is independent of the code under
    // test. The child's own 60 degrees is about its OWN centre, which that rotation leaves at (270,345);
    // the parent's 30 degrees is about (350,360), so the offset (-80,-15) becomes
    //   ( 0.866*-80 - 0.5*-15, 0.5*-80 + 0.866*-15 ) = (-61.782, -52.990)
    // and the centre lands at (288.218, 307.010). Composed the other way round it would not: the two
    // rotations are about different points, so the order shows in the position as well as the angle.
    const glm::vec2 centre = ( quad[0] + quad[2] ) * 0.5f;
    EXPECT_NEAR( centre.x, 288.218f, 1e-2f );
    EXPECT_NEAR( centre.y, 307.010f, 1e-2f );

    // The pointer is at the composition too, which is the half a draw-list test cannot reach.
    EXPECT_TRUE( ElectsAt( f, child, centre ) );
    EXPECT_EQ( Desert::UI::PickElement( f.Registry, f.Canvas, centre, kViewport ), child )
         << "the editor's pick disagrees with the walk about a doubly-rotated child";
}

// =========================================================================================================
// Ю4 — THE KEY IS THE PAIR (canvas x view), NOT EITHER HALF
//
// U3 gave the state a VIEW. It was half the key, and the missing half was not the smaller one: a view draws
// EVERY canvas of its scene, so a HUD canvas and a menu canvas shared one screen machine, one hover-clock
// table and one hot election. The tests below vary ONE coordinate at a time and require the other to hold
// still, which is the only shape that can tell a two-dimensional key from either of its projections — a
// suite that only ever varied the view passes with the canvas half deleted, and one that only varied the
// canvas passes with the view half deleted.
// =========================================================================================================

namespace
{
    // ONE registry, TWO canvases, one button each. Both canvases are Stretch at 1000x1000 so design px are
    // screen px; `Upper` has the higher Sort Order, so it is drawn second and is therefore on top — of the
    // pixels and of the pointer.
    struct TwoCanvasFixture
    {
        entt::registry Registry;
        entt::entity   Lower = entt::null, LowerButton = entt::null;
        entt::entity   Upper = entt::null, UpperButton = entt::null;

        TwoCanvasFixture()
        {
            Lower       = MakeCanvas( 0 );
            LowerButton = MakeButton( Lower, 0.0f );
            Upper       = MakeCanvas( 10 );
            UpperButton = MakeButton( Upper, 200.0f ); // disjoint from the lower one by default
        }

        entt::entity MakeCanvas( int sortOrder )
        {
            const entt::entity e = Registry.create();
            auto&              c = Registry.emplace<ECS::UICanvasComponent>( e ).Data;
            c.ScaleMode          = ECS::UICanvasScaleMode::Stretch;
            c.ReferenceWidth     = kSide;
            c.ReferenceHeight    = kSide;
            c.SortOrder          = sortOrder;
            Registry.emplace<ECS::RelationshipComponent>( e );
            return e;
        }

        entt::entity MakeButton( entt::entity canvas, float x )
        {
            const entt::entity e = Registry.create();
            auto&              l = Registry.emplace<ECS::UILayoutComponent>( e ).Data;
            l.AnchorMin          = { 0.0f, 0.0f };
            l.AnchorMax          = { 0.0f, 0.0f };
            l.OffsetMin          = { x, 0.0f };
            l.OffsetMax          = { x + 100.0f, 50.0f };

            auto& b        = Registry.emplace<ECS::UIButtonComponent>( e ).Data;
            b.NormalColor  = { 0.1f, 0.1f, 0.1f };
            b.HoverColor   = { 0.5f, 0.5f, 0.5f };
            b.PressedColor = { 0.9f, 0.9f, 0.9f };

            Registry.get<ECS::RelationshipComponent>( canvas ).Children.push_back( e );
            Registry.emplace<ECS::RelationshipComponent>( e ).Parent = canvas;
            return e;
        }

        // One frame of @p view over both canvases, in draw order, with the pointer at @p input. Returns
        // the button action the frame fired, if any — a button only acts when the host offers somewhere to
        // put the answer, so a frame with no outClicked cannot navigate.
        std::string Frame( UIViewContext& view, const UIInput& input )
        {
            R2D::DrawList2D dl;
            std::string     clicked;
            const auto      canvases = Desert::UI::CanvasesInDrawOrder( Registry );
            Desert::UI::BeginUIFrame( view, Registry, kViewport );
            for ( const entt::entity c : canvases )
            {
                const auto drawn = Desert::UI::RenderCanvas2D( view, Registry, c, dl,
                                                               /*worldViewProj=*/nullptr, &input, &clicked );
                EXPECT_TRUE( drawn.IsSuccess() ) << drawn.GetError();
            }
            Desert::UI::EndUIFrame( view, Registry, dl, &input, /*focused=*/nullptr, &clicked );
            return clicked;
        }
    };

    // A canvas with two screens under it, "<name>A" (initial) and "<name>B", each spread over the canvas.
    // Returns the entity of the second screen so a test can hang a navigating button on the first.
    void AddTwoScreens( entt::registry& reg, entt::entity canvas, const std::string& a, const std::string& b,
                        entt::entity moveUnderFirst = entt::null )
    {
        reg.emplace<ECS::UIScreenStackComponent>( canvas ).Data.InitialScreen = a;
        for ( const std::string& name : { a, b } )
        {
            const entt::entity s                               = reg.create();
            reg.emplace<ECS::UIScreenComponent>( s ).Data.Name = name;
            auto& l                                            = reg.emplace<ECS::UILayoutComponent>( s ).Data;
            l.AnchorMax                                        = { 1.0f, 1.0f };
            l.OffsetMax                                        = { 0.0f, 0.0f };
            // A screen is a container, not a target: it spreads over the whole canvas, so with the default
            // HitTest it would be elected by any pointer inside the canvas and the topmost canvas's screen
            // would swallow every click in the frame. ChildrenOnly is what a real screen carries, and it is
            // the engine behaving correctly rather than a workaround — measured here first.
            l.HitTest                                           = ECS::UIHitTest::ChildrenOnly;
            reg.emplace<ECS::RelationshipComponent>( s ).Parent = canvas;
            auto& kids = reg.get<ECS::RelationshipComponent>( canvas ).Children;
            if ( name == a && moveUnderFirst != entt::null )
            {
                kids.erase( std::remove( kids.begin(), kids.end(), moveUnderFirst ), kids.end() );
                reg.get<ECS::RelationshipComponent>( s ).Children.push_back( moveUnderFirst );
                reg.get<ECS::RelationshipComponent>( moveUnderFirst ).Parent = s;
            }
            kids.push_back( s );
        }
    }
} // namespace

// --- THE DECISIVE ONE: four cells, and each moves only when its own pair is addressed --------------------
//
// Two views draw the SAME two canvases in the same frames. Each view points at a different canvas's button.
// Four (canvas x view) cells exist and exactly two of them may warm up. Collapse the key to the view alone
// and the two cells of a view merge, so the canvas the view is NOT pointing at warms up too; collapse it to
// the canvas alone and the two views merge, so the other view's button warms up.
TEST( UICanvasContextPair, HoverInOneCellMovesNoOtherCellOfTheTable )
{
    TwoCanvasFixture f;
    UIViewContext    viewA, viewB;

    const UIInput onLower = At( 10.0f, 10.0f, /*down=*/false );  // inside LowerButton only
    const UIInput onUpper = At( 210.0f, 10.0f, /*down=*/false ); // inside UpperButton only

    // Frame 1 elects; the ease only starts once the election is in (the walk reacts to LAST frame's hot).
    f.Frame( viewA, onLower );
    f.Frame( viewB, onUpper );
    RewindClock( viewA, 0.5f );
    RewindClock( viewB, 0.5f );
    f.Frame( viewA, onLower );
    f.Frame( viewB, onUpper );

    const float aOnLower = viewA.CanvasState( f.Lower ).HoverT[f.LowerButton];
    const float aOnUpper = viewA.CanvasState( f.Upper ).HoverT[f.UpperButton];
    const float bOnLower = viewB.CanvasState( f.Lower ).HoverT[f.LowerButton];
    const float bOnUpper = viewB.CanvasState( f.Upper ).HoverT[f.UpperButton];

    EXPECT_GT( aOnLower, 0.5f ) << "view A pointed at the lower canvas's button and its clock never moved";
    EXPECT_GT( bOnUpper, 0.5f ) << "view B pointed at the upper canvas's button and its clock never moved";

    // The two negative controls, one per axis. Without them the assertions above pass on a shared table.
    EXPECT_NEAR( aOnUpper, 0.0f, 1e-4f )
         << "the OTHER CANVAS of the same view warmed up: the cell is keyed by the view alone";
    EXPECT_NEAR( bOnLower, 0.0f, 1e-4f )
         << "the OTHER CANVAS of the same view warmed up: the cell is keyed by the view alone";

    // And the view axis, on ONE canvas — the brief's own case, with one registry rather than two.
    EXPECT_NEAR( viewB.CanvasState( f.Lower ).HoverT[f.LowerButton], 0.0f, 1e-4f )
         << "view B's cell for the lower canvas warmed from view A's pointer: the cell is keyed by the "
            "canvas alone";
    EXPECT_NEAR( viewA.CanvasState( f.Upper ).HoverT[f.UpperButton], 0.0f, 1e-4f )
         << "view A's cell for the upper canvas warmed from view B's pointer: the cell is keyed by the "
            "canvas alone";

    EXPECT_EQ( viewA.CanvasStateCount(), 2u );
    EXPECT_EQ( viewB.CanvasStateCount(), 2u );
}

// --- The screen machine is a property of the pair, and this is what a shared one DOES --------------------
//
// Two canvases in one view, each with its own two screens. The names are disjoint, because screens are
// sub-trees of ONE canvas — which is exactly why a single machine per view cannot work: the second walk of
// every frame finds the current name foreign to its own tree and RE-SEEDS, so the two canvases would fight
// over one string forever and both end up on their first screen.
TEST( UICanvasContextPair, EachCanvasNavigatesItsOwnScreensInsideOneView )
{
    TwoCanvasFixture f;
    AddTwoScreens( f.Registry, f.Lower, "Home", "Settings", f.LowerButton );
    AddTwoScreens( f.Registry, f.Upper, "Idle", "Alert" );

    auto& button          = f.Registry.get<ECS::UIButtonComponent>( f.LowerButton ).Data;
    button.Action         = ECS::UIButtonAction::ShowScreen;
    button.OnClickMessage = "Settings";

    UIViewContext view, untouched;

    f.Frame( view, At( 10.0f, 10.0f ) );        // elect the lower canvas's button
    f.Frame( untouched, At( 900.0f, 900.0f ) ); // a second view of the same scene, pointing at nothing
    ASSERT_EQ( view.Hot, f.LowerButton );

    UIInput click       = At( 10.0f, 10.0f, /*down=*/false );
    click.MouseReleased = true;
    EXPECT_EQ( f.Frame( view, click ), "screen:Settings" );

    // Three more frames: a shared machine does not merely start wrong, it oscillates, so one frame after
    // the click could pass by accident.
    for ( int i = 0; i < 3; ++i )
    {
        f.Frame( view, At( 900.0f, 900.0f, /*down=*/false ) );
        f.Frame( untouched, At( 900.0f, 900.0f, /*down=*/false ) );
    }

    EXPECT_EQ( view.CanvasState( f.Lower ).Screen, "Settings" ) << "the navigation did not stick";
    EXPECT_EQ( view.CanvasState( f.Upper ).Screen, "Idle" )
         << "the OTHER canvas of the same view moved, or was re-seeded by the navigating one";
    EXPECT_EQ( untouched.CanvasState( f.Lower ).Screen, "Home" )
         << "a second view of the same scene followed a navigation it never made";
    EXPECT_EQ( untouched.CanvasState( f.Upper ).Screen, "Idle" );
}

// --- The pointer is the VIEW's: one election over every canvas of the frame ------------------------------
//
// This is the seam the four overlay features sit on. A canvas drawn later is on top, and being on top has
// to mean the pointer stops there — otherwise a modal scrim is a picture of a modal and not a modal.
TEST( UICanvasContextPair, TheCanvasDrawnLastTakesThePointerFromTheOneBelowIt )
{
    TwoCanvasFixture f;
    // Move the upper canvas's button onto the lower one's, so one point is inside both.
    auto& l     = f.Registry.get<ECS::UILayoutComponent>( f.UpperButton ).Data;
    l.OffsetMin = { 0.0f, 0.0f };
    l.OffsetMax = { 100.0f, 50.0f };

    UIViewContext view;
    f.Frame( view, At( 10.0f, 10.0f ) );
    EXPECT_EQ( view.Hot, f.UpperButton ) << "the pointer was over both canvases and the one drawn FIRST kept it";

    // The negative control, and it is the one that matters: with the overlay's button moved away the same
    // point must reach the canvas underneath. Without it, "the last canvas always wins" would also pass —
    // including the way it wins by erasing the election of everything drawn before it, which is what the
    // per-walk hand-over did.
    l.OffsetMin = { 200.0f, 0.0f };
    l.OffsetMax = { 300.0f, 50.0f };
    UIViewContext second;
    second.Reset();
    f.Frame( second, At( 10.0f, 10.0f ) );
    EXPECT_EQ( second.Hot, f.LowerButton )
         << "an overlay canvas that does not cover the pointer still swallowed the election";
}

// --- Lifetime: a cell dies with its canvas, inside a living view -----------------------------------------
//
// The twin of "a hidden preview still owns its renderer slot" (Docs/RENDERER_FRAME_STATE.md). entt recycles
// ids, so a cell outliving its canvas is not dead weight — the next canvas can be handed that id.
TEST( UICanvasContextPair, ACanvasThatStopsExistingTakesItsCellWithIt )
{
    TwoCanvasFixture f;
    UIViewContext    view;

    f.Frame( view, At( 10.0f, 10.0f ) );
    ASSERT_EQ( view.CanvasStateCount(), 2u );
    view.CanvasState( f.Upper ).Screen = "Alert"; // something recognisable to inherit

    const entt::entity destroyed = f.Upper;
    f.Registry.destroy( f.Upper );
    f.Upper = entt::null;

    f.Frame( view, At( 10.0f, 10.0f ) );
    EXPECT_EQ( view.CanvasStateCount(), 1u ) << "the destroyed canvas's cell outlived it";

    // The recycled id, and the correction it forced. entt reuses the INDEX but bumps the version, so the
    // reborn canvas is a different key and could not have inherited the cell even if the cell had stayed.
    // What the retirement is really for is therefore the LEAK — one cell per canvas the level ever
    // destroyed, in a view that lives as long as the document — and, at the far end, the 12 version bits
    // wrapping after 4096 reuses of one index. Both are reasons; "the next canvas inherits it" is not, and
    // it was the reason this test was written to prove until it measured otherwise.
    const entt::entity reborn = f.MakeCanvas( 10 );
    EXPECT_NE( reborn, destroyed ) << "entt handed back an identical id, version bits and all";
    EXPECT_EQ( entt::to_integral( reborn ) & 0xFFFFFu, entt::to_integral( destroyed ) & 0xFFFFFu )
         << "the index was not recycled, so nothing here is about recycling";
    f.MakeButton( reborn, 200.0f );
    f.Frame( view, At( 10.0f, 10.0f ) );
    EXPECT_TRUE( view.CanvasState( reborn ).Screen.empty() );
    EXPECT_EQ( view.CanvasStateCount(), 2u ) << "the retired cell came back";
}

// --- Draw order is authored, not the component pool's ----------------------------------------------------
TEST( UICanvasContextPair, CanvasesAreOrderedByTheirAuthoredSortOrder )
{
    TwoCanvasFixture f;
    ASSERT_EQ( Desert::UI::CanvasesInDrawOrder( f.Registry ), ( std::vector<entt::entity>{ f.Lower, f.Upper } ) );

    // Reverse the authored order and the draw order follows it, not the creation order.
    f.Registry.get<ECS::UICanvasComponent>( f.Lower ).Data.SortOrder = 20;
    EXPECT_EQ( Desert::UI::CanvasesInDrawOrder( f.Registry ), ( std::vector<entt::entity>{ f.Upper, f.Lower } ) );

    // Equal orders keep the order the scene created them in — a STABLE sort, so the tie is decided by the
    // file rather than by the sort's internal pivoting.
    f.Registry.get<ECS::UICanvasComponent>( f.Lower ).Data.SortOrder = 10;
    EXPECT_EQ( Desert::UI::CanvasesInDrawOrder( f.Registry ), ( std::vector<entt::entity>{ f.Lower, f.Upper } ) );
}

// --- A walk outside a frame of its view is refused, not silently frozen ----------------------------------
TEST( UICanvasContextPair, AWalkWithNoOpenFrameIsRefusedByName )
{
    TwoCanvasFixture f;
    UIViewContext    view;
    R2D::DrawList2D  dl;

    const auto refused = Desert::UI::RenderCanvas2D( view, f.Registry, f.Lower, dl );
    EXPECT_FALSE( refused.IsSuccess() )
         << "a walk with no BeginUIFrame drew a frame whose clock can never advance";
    EXPECT_NE( refused.GetError().find( "BeginUIFrame" ), std::string::npos ) << refused.GetError();
}

// --- The editor's pick and the renderer's election must agree ABOUT WHICH CANVAS -------------------------
//
// The viewport picks by asking every canvas and keeping the last hit; the walk elects by drawing every
// canvas and keeping the last writer. Two loops, one answer required — and they can only give it if both
// iterate the SAME order. The pick's loop used to walk `reg.view<UICanvasComponent>()`, which is the pool's
// order (measured: the REVERSE of creation) and knows nothing of Sort Order, so with two canvases
// overlapping at the cursor it selected the one drawn underneath.
TEST( UICanvasContextPair, TheEditorsPickAndTheWalkAgreeOnWhichCanvasIsOnTop )
{
    TwoCanvasFixture f;
    auto&            l = f.Registry.get<ECS::UILayoutComponent>( f.UpperButton ).Data;
    l.OffsetMin        = { 0.0f, 0.0f }; // both buttons under one point
    l.OffsetMax        = { 100.0f, 50.0f };

    const glm::vec2 point{ 10.0f, 10.0f };
    for ( const int upperOrder : { 10, -10 } ) // on top, then underneath
    {
        f.Registry.get<ECS::UICanvasComponent>( f.Upper ).Data.SortOrder = upperOrder;

        UIViewContext view;
        f.Frame( view, At( point.x, point.y ) );

        // The editor's loop, spelled exactly as ViewportPanel spells it.
        entt::entity picked = entt::null;
        for ( const entt::entity canvas : Desert::UI::CanvasesInDrawOrder( f.Registry ) )
            if ( const entt::entity hit = Desert::UI::PickElement( f.Registry, canvas, point, kViewport );
                 hit != entt::null )
                picked = hit;

        EXPECT_EQ( picked, view.Hot ) << "the pick and the election disagree with Upper's Sort Order at "
                                      << upperOrder;
        EXPECT_EQ( picked, upperOrder > 0 ? f.UpperButton : f.LowerButton );
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
