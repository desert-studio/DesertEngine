// "A list of ten thousand rows costs what a list of twenty rows costs."
//
// THE OBSERVABLE IS A COUNT, NOT A CLOCK. A virtualized list claims that the frame stops following the
// number of rows. Asserting that with a timer on a machine that runs several agents at once would be a
// gate that fires at random, so the claim is pinned on two counts instead, and neither has a noise floor:
//
//   * how many elements the walk VISITED -- read from UI::EnumerateCanvas, the same enumeration the
//     editor's click-select and the UI Debugger are built on;
//   * how many vertices and draw commands the frame EMITTED -- read from the DrawList2D the GPU backend
//     iterates, not from a tally kept beside it.
//
// The clock is still here, in ListViewCost.WalkTimeAgainstRowCount, but it PRINTS and asserts nothing.
// That measurement is what decides whether this element should exist at all, and it belongs in the record
// beside the counts rather than in a red/green verdict.
//
// THE LOAD-BEARING TEST IS `RowsOutsideTheWindowAreNotAsked`. A render-texture element holds one of the
// six renderer slots and gives it back only when its capture is DESTROYED, which the backend learns from
// the walk no longer asking about it (UIRenderTextureSource.hpp). A virtualized list is the first
// consumer that stops asking EN MASSE, so that test drives a counting stand-in for the backend and
// requires the demand set to shrink to the window.

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIIntrospection.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

// The renderer resolves sprites, fonts, icons and video through these. Every one of them owns GPU
// objects, and every draw helper already copes with the service being absent -- a sprite that will not
// resolve falls back to its flat colour, text and icons draw nothing. That is exactly the path a headless
// walk wants, so the suite supplies the accessors itself and returns nothing.
namespace Desert::Runtime
{
    // NOLINTBEGIN(readability-convert-member-functions-to-static) — these are DEFINITIONS of the engine's
    // own member functions, supplied here instead of linking the services. Their signatures belong to
    // Engine/Runtime/ResourceRegistry.hpp and cannot be changed from a test.
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

    // The service METHODS the walk calls on whatever those accessors hand back. Every accessor above
    // returns nullptr, so none of these can run -- they exist because the linker still wants the symbols,
    // and each fails the test outright rather than returning a plausible value.
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
    // NOLINTEND(readability-convert-member-functions-to-static)
} // namespace Desert::Runtime

using Desert::UI::Rect;
using Desert::UI::UIElementNode;
using Desert::UI::UIFrameProbe;
using Desert::UI::UISkipCause;
using Desert::UI::UIViewContext;
namespace ECS = Desert::ECS;
namespace R2D = Desert::Graphic::Render2D;
namespace UI  = Desert::UI;

namespace
{
    constexpr float kSide = 1000.0f; // Stretch canvas at 1000x1000: design px == screen px (scale 1)
    const Rect      kViewport{ 0.0f, 0.0f, kSide, kSide };

    // The list's own box, and the row pitch -- chosen so the window is a round number: 600 px of viewport
    // at 40 px a row shows exactly 15 rows.
    constexpr float kListH      = 600.0f;
    constexpr float kListW      = 400.0f;
    constexpr float kRowHeight  = 40.0f;
    constexpr int   kRowsOnView = static_cast<int>( kListH / kRowHeight );

    // A canvas holding ONE scrollable container with @p rows children, each row a panel with two panel
    // children of its own -- the shape an inventory or a chat log has, and three elements per row.
    //
    // NO TEXT, AND THAT IS DELIBERATE. The headless walk has no font service, so a UIText would emit
    // nothing and measure nothing; every real row carries a label, and a label is glyph quads on top of
    // what is counted here. So every saving this file reports is a LOWER BOUND on the real one.
    struct ListScene
    {
        entt::registry            Registry;
        entt::entity              Canvas    = entt::null;
        entt::entity              Container = entt::null;
        std::vector<entt::entity> Rows;

        entt::entity AddChild( entt::entity parent, float x, float y, float w, float h )
        {
            const entt::entity e = Registry.create();
            auto&              L = Registry.emplace<ECS::UILayoutComponent>( e ).Data;
            L.AnchorMin          = { 0.0f, 0.0f };
            L.AnchorMax          = { 0.0f, 0.0f };
            L.OffsetMin          = { x, y };
            L.OffsetMax          = { x + w, y + h };
            Registry.emplace<ECS::UIPanelComponent>( e );
            Registry.emplace<ECS::RelationshipComponent>( e ).Parent = parent;
            Registry.get<ECS::RelationshipComponent>( parent ).Children.push_back( e );
            return e;
        }

        void MakeCanvas()
        {
            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;
            Registry.emplace<ECS::RelationshipComponent>( Canvas );
        }

        // The container element itself, sized kListW x kListH at the canvas origin.
        entt::entity MakeContainer()
        {
            Container = AddChild( Canvas, 0.0f, 0.0f, kListW, kListH );
            Registry.remove<ECS::UIPanelComponent>( Container );
            return Container;
        }

        // One row and its two children. @p y is the row's top in the container's content space -- the
        // scroll view needs it authored, the list view computes it and ignores what is authored here.
        void AddRow( float y )
        {
            const entt::entity row = AddChild( Container, 0.0f, y, kListW, kRowHeight - 2.0f );
            AddChild( row, 4.0f, 4.0f, 32.0f, 32.0f );
            AddChild( row, 40.0f, 4.0f, 200.0f, 32.0f );
            Rows.push_back( row );
        }
    };

    // The scrolling container the engine had before Ю17: every child is walked, and the ones off the top
    // and bottom are cut by the scissor after they have already cost a walk and their vertices.
    ListScene MakeScrollViewList( int rows )
    {
        ListScene s;
        s.MakeCanvas();
        s.MakeContainer();
        auto& sv         = s.Registry.emplace<ECS::UIScrollViewComponent>( s.Container ).Data;
        sv.ContentHeight = static_cast<float>( rows ) * kRowHeight;
        for ( int i = 0; i < rows; ++i )
        {
            s.AddRow( static_cast<float>( i ) * kRowHeight );
        }
        return s;
    }

    // The virtualized list: the same tree, the same rows, and a container that walks only the window.
    ListScene MakeListView( int rows, int overscan = 1 )
    {
        ListScene s;
        s.MakeCanvas();
        s.MakeContainer();
        auto& lv      = s.Registry.emplace<ECS::UIListViewComponent>( s.Container ).Data;
        lv.ItemHeight = kRowHeight;
        lv.Overscan   = overscan;
        for ( int i = 0; i < rows; ++i )
        {
            s.AddRow( static_cast<float>( i ) * kRowHeight ); // y ignored: the list assigns the row rect
        }
        return s;
    }

    // A stand-in for Graphic::Render2D::UIRenderTextureCache that owns no slots and only REMEMBERS being
    // asked. Which is the whole of what the real one learns from the walk: see UIRenderTextureSource.hpp,
    // "calling this is also the demand".
    struct CountingRenderTextures final : UI::IUIRenderTextureSource
    {
        std::unordered_set<entt::entity> Asked;

        const void* ResolveRenderTexture( entt::entity element, const UI::UIRenderTextureRequest& ) override
        {
            Asked.insert( element );
            return nullptr; // no picture: the walk draws the magenta fill, which is not what is measured
        }
    };

    // One walk of the canvas into a fresh draw list -- the headless equivalent of a frame.
    bool Walk( ListScene& scene, R2D::DrawList2D& dl, UIViewContext& ctx )
    {
        dl.Reset();
        UI::BeginUIFrame( ctx, scene.Registry, kViewport );
        const bool drawn = UI::RenderCanvas2D( ctx, scene.Registry, scene.Canvas, dl ).IsSuccess();
        UI::EndUIFrame( ctx, scene.Registry, dl, /*input=*/nullptr );
        return drawn;
    }

    // The bytes a draw list holds, folded into ONE NUMBER: geometry and batch structure together. Two
    // walks that agree here produced the same frame.
    //
    // A HASH AND NOT THE BYTES, and that is a lesson from breaking this file on purpose. The first
    // version compared two binary std::strings with EXPECT_EQ; a mutation that made the list draw all
    // two thousand rows did fail it, and then gtest tried to render two 800 KB binary strings into a
    // failure message and the process was killed at 62 GB. A test whose failure cannot be PRINTED is a
    // test that reports a crash instead of a diagnosis.
    std::uint64_t Fingerprint( const R2D::DrawList2D& dl )
    {
        std::uint64_t h   = 1469598103934665603ULL; // FNV-1a
        const auto    eat = [&h]( const void* p, std::size_t n )
        {
            const auto* b = static_cast<const unsigned char*>( p );
            for ( std::size_t i = 0; i < n; ++i )
            {
                h = ( h ^ b[i] ) * 1099511628211ULL;
            }
        };
        for ( const R2D::Vertex2D& v : dl.GetVertices() )
        {
            eat( &v, sizeof( v ) );
        }
        for ( const R2D::DrawCommand& c : dl.GetCommands() )
        {
            eat( static_cast<const void*>( &c.Texture ), sizeof( c.Texture ) );
            eat( &c.ClipRect, sizeof( c.ClipRect ) );
            eat( &c.IndexCount, sizeof( c.IndexCount ) );
            eat( &c.Text, sizeof( c.Text ) );
            eat( &c.Glass, sizeof( c.Glass ) );
        }
        return h;
    }

    // How many elements this frame DREW, counted by the enumeration rather than by the draw list, so the
    // number means "the walk reached it" and not "it happened to emit geometry".
    std::vector<UIElementNode> Enumerate( ListScene& scene, UIViewContext& ctx )
    {
        std::vector<UIElementNode> nodes;
        const auto                 ok = UI::EnumerateCanvas( scene.Registry, scene.Canvas, kViewport, nodes,
                                                             &ctx.CanvasState( scene.Canvas ) );
        EXPECT_TRUE( static_cast<bool>( ok ) ) << ok.GetError();
        return nodes;
    }

    std::size_t DrawnCount( ListScene& scene, UIViewContext& ctx )
    {
        const auto nodes = Enumerate( scene, ctx );
        return static_cast<std::size_t>(
             std::count_if( nodes.begin(), nodes.end(), []( const UIElementNode& n ) { return n.Drawn; } ) );
    }

    // Median of an odd-length sample, which is what a shared machine's timings want: one agent's build
    // kicking off mid-run moves the mean and leaves the median alone. The SPREAD comes back too, because
    // a number without its own floor is not a measurement: `lo` is the fastest walk seen and `hi` the
    // slowest, and a difference smaller than (hi - lo) is not a difference.
    struct Timing
    {
        double Median = 0.0;
        double Lo     = 0.0;
        double Hi     = 0.0;
    };

    Timing Summarise( std::vector<double>& samples )
    {
        std::sort( samples.begin(), samples.end() );
        return Timing{ samples[samples.size() / 2], samples.front(), samples.back() };
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The counts: what the window costs, and what it does not.
// ---------------------------------------------------------------------------------------------------

TEST( ListViewWindow, DrawnElementsFollowTheWindowAndNotTheRowCount )
{
    // Three row counts two orders of magnitude apart, one window. If the list virtualizes, all three
    // draw the same number of elements; if it does not, the number follows `rows`.
    std::size_t drawn[3] = {};
    std::size_t verts[3] = {};
    int         i        = 0;
    for ( const int rows : { 20, 200, 2000 } )
    {
        ListScene       scene = MakeListView( rows );
        R2D::DrawList2D dl;
        UIViewContext   ctx;
        ASSERT_TRUE( Walk( scene, dl, ctx ) );
        drawn[i] = DrawnCount( scene, ctx );
        verts[i] = dl.GetVertices().size();
        ++i;
    }

    EXPECT_EQ( drawn[0], drawn[1] );
    EXPECT_EQ( drawn[1], drawn[2] );
    EXPECT_EQ( verts[0], verts[1] );
    EXPECT_EQ( verts[1], verts[2] );

    // And the window is the one the geometry implies: the rows that fit, plus the overscan row below.
    // Three elements per row, plus the canvas and the list itself.
    // The canvas itself is not one of them: EnumerateCanvas reports a canvas's CHILDREN.
    const auto windowRows = static_cast<std::size_t>( kRowsOnView ) + 1u;
    EXPECT_EQ( drawn[0], windowRows * 3u + 1u );
}

TEST( ListViewWindow, TheScrollViewItReplacesPaysForEveryRow )
{
    // The negative control for the test above, and the measurement's own justification: the same tree in
    // the container that existed before Ю17 draws every row at every row count.
    for ( const int rows : { 20, 200, 2000 } )
    {
        ListScene       scene = MakeScrollViewList( rows );
        R2D::DrawList2D dl;
        UIViewContext   ctx;
        ASSERT_TRUE( Walk( scene, dl, ctx ) );
        EXPECT_EQ( DrawnCount( scene, ctx ), static_cast<std::size_t>( rows ) * 3u + 1u )
             << "scroll view with " << rows << " rows";
    }
}

TEST( ListViewWindow, ScrollingMovesTheWindowAndTheClampedEdgeIsOneRowSmaller )
{
    ListScene       scene = MakeListView( 2000 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    // At the top there is nothing above to overscan into, so the window is the rows that fit plus the one
    // below. That asymmetry is the whole of the edge behaviour and it is pinned rather than rounded off.
    EXPECT_EQ( DrawnCount( scene, ctx ), static_cast<std::size_t>( kRowsOnView + 1 ) * 3u + 1u );

    auto& lv   = scene.Registry.get<ECS::UIListViewComponent>( scene.Container ).Data;
    lv.ScrollY = 1000.0f * kRowHeight;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    // Away from both edges: the rows that fit, plus one overscan row on each side.
    EXPECT_EQ( DrawnCount( scene, ctx ), static_cast<std::size_t>( kRowsOnView + 2 ) * 3u + 1u );

    const auto nodes   = Enumerate( scene, ctx );
    const auto rowNode = [&]( entt::entity e ) -> const UIElementNode*
    {
        for ( const UIElementNode& n : nodes )
        {
            if ( n.Entity == e )
            {
                return &n;
            }
        }
        return nullptr;
    };
    ASSERT_NE( rowNode( scene.Rows[1000] ), nullptr );
    EXPECT_TRUE( rowNode( scene.Rows[1000] )->Drawn );
    ASSERT_NE( rowNode( scene.Rows[0] ), nullptr );
    EXPECT_FALSE( rowNode( scene.Rows[0] )->Drawn );
    EXPECT_EQ( rowNode( scene.Rows[0] )->Cause, UISkipCause::OutsideWindow );

    // A row the window does not cover still knows WHERE it is — off the top by forty thousand pixels.
    // Refusing to answer would lose the outliner the only question worth asking about row zero here.
    EXPECT_TRUE( rowNode( scene.Rows[0] )->RectValid );
    EXPECT_LT( rowNode( scene.Rows[0] )->RectPx.Y, 0.0f );
}

TEST( ListViewWindow, TheWholeTreeIsStillEnumerated )
{
    // A row the list did not render must still be REPORTABLE, or the UI Debugger and the editor's
    // outliner lose two thousand elements the scene plainly has -- the same reason a Collapsed child of
    // a layout group is enumerated with no slot rather than dropped.
    ListScene       scene = MakeListView( 2000 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    const auto nodes = Enumerate( scene, ctx );
    EXPECT_EQ( nodes.size(), 2000u * 3u + 1u );
}

TEST( ListViewWindow, HidingARowOutsideTheWindowChangesNothingInTheFrame )
{
    // The relation, written the way this project's defect taxonomy says to write one: assert the
    // AGREEMENT between what the enumeration calls not-drawn and what the real walk does, rather than
    // either side alone. It is the drift detector for the day somebody changes how the window is
    // computed in one of the two walks and not in the other.
    ListScene       scene = MakeListView( 2000 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    const std::uint64_t before = Fingerprint( dl );

    scene.Registry.get<ECS::UILayoutComponent>( scene.Rows[1500] ).Data.Visibility = ECS::UIVisibility::Hidden;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_EQ( Fingerprint( dl ), before );

    // The negative control, and it is not optional: without it this passes on a walk that draws nothing
    // at all. A row INSIDE the window must move the bytes.
    scene.Registry.get<ECS::UILayoutComponent>( scene.Rows[3] ).Data.Visibility = ECS::UIVisibility::Hidden;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_NE( Fingerprint( dl ), before );
}

// ---------------------------------------------------------------------------------------------------
// The renderer slot: the reason this task was interesting, and the half no count in a draw list shows.
// ---------------------------------------------------------------------------------------------------

TEST( ListViewRenderTexture, OnlyTheWindowsRowsAreAsked )
{
    // A render-texture element holds one of the six renderer slots and gets it back only by having its
    // capture DESTROYED; the backend learns an element is on screen from being asked about it and from
    // nothing else. So "does virtualization return the slots" is the same question as "does the walk stop
    // asking", and this drives a counting stand-in for the backend to read the answer off directly.
    ListScene scene = MakeListView( 2000 );
    for ( const entt::entity row : scene.Rows )
    {
        scene.Registry.emplace<ECS::UIRenderTextureComponent>( row ).Data.ScenePath = "Any.desce";
    }

    CountingRenderTextures source;
    R2D::DrawList2D        dl;
    UIViewContext          ctx;
    ctx.RenderTextures = &source;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    // Sixteen rows on screen, sixteen demands -- against six slots that still refuses ten of them, and
    // that refusal is the backend's to make. What matters here is that it is asked about SIXTEEN and not
    // about two thousand, because two thousand would mean 1984 captures built and destroyed every frame.
    EXPECT_EQ( source.Asked.size(), static_cast<std::size_t>( kRowsOnView + 1 ) );
    EXPECT_EQ( source.Asked.count( scene.Rows[0] ), 1u );
    EXPECT_EQ( source.Asked.count( scene.Rows[1000] ), 0u );

    // Scrolled away, the row that held a slot is not asked again -- which is the DESTRUCTION, stated in
    // the only terms the backend can observe.
    source.Asked.clear();
    scene.Registry.get<ECS::UIListViewComponent>( scene.Container ).Data.ScrollY = 1000.0f * kRowHeight;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_EQ( source.Asked.count( scene.Rows[0] ), 0u );
    EXPECT_EQ( source.Asked.count( scene.Rows[1000] ), 1u );
}

TEST( ListViewRenderTexture, TheScrollViewAsksAboutEveryRow )
{
    // The negative control, and it is the measurement that made the element worth building: the container
    // Ю17 replaces demands a whole Core::Scene and SceneRenderer for every row of the list at once, which
    // is 2000 demands against six slots -- 1994 magenta fills and a log line naming the budget.
    ListScene scene = MakeScrollViewList( 2000 );
    for ( const entt::entity row : scene.Rows )
    {
        scene.Registry.emplace<ECS::UIRenderTextureComponent>( row ).Data.ScenePath = "Any.desce";
    }

    CountingRenderTextures source;
    R2D::DrawList2D        dl;
    UIViewContext          ctx;
    ctx.RenderTextures = &source;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_EQ( source.Asked.size(), 2000u );
}

// ---------------------------------------------------------------------------------------------------
// The contract's edges.
// ---------------------------------------------------------------------------------------------------

TEST( ListViewContract, TheTwoScrollingContainersShareTheirThemeSlotsAndMustShareTheirDefaults )
{
    // UIListView draws its background and thumb through StyleSlot::ScrollView{Background,Scrollbar} --
    // one token for one look, rather than a second pair an author would have to keep identical. That is
    // only honest while the two components' own defaults agree: Desert_Dark binds those slots to
    // UIScrollViewData's values (Desert/Tests/Engine/UIStyle pins it), so a list with a different default
    // would change appearance the moment a theme was attached and match with none.
    const ECS::UIScrollViewData scroll{};
    const ECS::UIListViewData   list{};
    EXPECT_EQ( list.Background, scroll.Background );
    EXPECT_EQ( list.ScrollbarColor, scroll.ScrollbarColor );
}

TEST( ListViewContract, AHiddenRowLeavesItsSlotEmptyRatherThanClosingTheGap )
{
    // The one place this container disagrees with UILayoutGroup, and the disagreement is the point: the
    // row index IS the child index, so the window is two divisions. Closing the gap would need a running
    // count over every child ahead of the window -- exactly the whole-list pass the element deletes.
    ListScene       scene = MakeListView( 200 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    const auto rectOf = [&]( entt::entity e )
    {
        for ( const UIElementNode& n : Enumerate( scene, ctx ) )
        {
            if ( n.Entity == e )
            {
                return n.RectPx;
            }
        }
        ADD_FAILURE() << "row not enumerated";
        return Rect{};
    };
    const float row5Before = rectOf( scene.Rows[5] ).Y;

    scene.Registry.get<ECS::UILayoutComponent>( scene.Rows[2] ).Data.Visibility = ECS::UIVisibility::Collapsed;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    EXPECT_FLOAT_EQ( rectOf( scene.Rows[5] ).Y, row5Before ) << "a collapsed row moved its siblings";
    // ...and the collapsed row is gone from the picture, so the hole is visible rather than silent.
    EXPECT_EQ( DrawnCount( scene, ctx ), static_cast<std::size_t>( kRowsOnView + 1 ) * 3u + 1u - 3u );
}

TEST( ListViewContract, ScrollIsClampedToContentDerivedFromTheChildCount )
{
    // UIScrollViewData carries an AUTHORED ContentHeight that can disagree with what is in the list; here
    // it is the child count times the pitch, so adding a row cannot leave the scroll range lying.
    ListScene       scene = MakeListView( 30 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;

    auto& lv   = scene.Registry.get<ECS::UIListViewComponent>( scene.Container ).Data;
    lv.ScrollY = 1.0e6f; // far past the end
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    // 30 rows of 40 px is 1200 px of content in a 600 px viewport: 600 px of travel and no more.
    EXPECT_FLOAT_EQ( lv.ScrollY, 30.0f * kRowHeight - kListH );

    lv.ScrollY = -500.0f;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_FLOAT_EQ( lv.ScrollY, 0.0f );
}

TEST( ListViewContract, AZeroItemHeightCannotTurnVirtualizationOff )
{
    // The one input that could quietly restore the whole-list walk: a pitch of zero makes the window
    // unbounded. It is floored at one design pixel where it is read, so the window stays a window.
    ListScene       scene = MakeListView( 5000 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    scene.Registry.get<ECS::UIListViewComponent>( scene.Container ).Data.ItemHeight = 0.0f;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    // 600 px of viewport at a 1 px pitch is 601 rows (the overscan row above is clamped away at the
    // top edge), and not five thousand.
    EXPECT_EQ( DrawnCount( scene, ctx ), 601u * 3u + 1u );
}

TEST( ListViewContract, AnEmptyListDrawsItsBackgroundAndNoThumb )
{
    ListScene       scene = MakeListView( 0 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_EQ( DrawnCount( scene, ctx ), 1u ); // the list element itself, nothing under it
    EXPECT_FALSE( dl.GetVertices().empty() );  // ...and it is still a box on screen, not nothing
}

// ---------------------------------------------------------------------------------------------------
// The measurement. Prints; asserts nothing.
// ---------------------------------------------------------------------------------------------------

TEST( ListViewCost, WalkTimeAgainstRowCount )
{
    std::printf( "\n  Debug build. Median of the repeats, with [fastest..slowest] beside it -- a difference\n" );
    std::printf( "  smaller than that bracket is not a difference.\n\n" );
    std::printf( "  rows |     scroll view (us)      |       list view (us)      | drawn sv | drawn lv\n" );
    std::printf( "  -----+---------------------------+---------------------------+----------+---------\n" );

    for ( const int rows : { 20, 100, 500, 1000, 5000, 20000 } )
    {
        // Fewer repeats where one walk already costs a third of a second: the median of five says the
        // same thing as the median of twenty-one there, and twenty-one would put this suite past a
        // minute for a number that is printed rather than gated.
        const int kReps = rows >= 5000 ? 5 : 21;

        Timing svUs;
        Timing lvUs;
        Timing lvCanvasUs;

        std::size_t svDrawn = 0;
        std::size_t lvDrawn = 0;

        {
            ListScene           scene = MakeScrollViewList( rows );
            R2D::DrawList2D     dl;
            UIViewContext       ctx;
            std::vector<double> samples;
            for ( int r = 0; r < kReps; ++r )
            {
                const auto t0 = std::chrono::steady_clock::now();
                Walk( scene, dl, ctx );
                const auto t1 = std::chrono::steady_clock::now();
                samples.push_back( std::chrono::duration<double, std::micro>( t1 - t0 ).count() );
            }
            svUs    = Summarise( samples );
            svDrawn = DrawnCount( scene, ctx );
        }
        {
            ListScene           scene = MakeListView( rows );
            R2D::DrawList2D     dl;
            UIViewContext       ctx;
            std::vector<double> samples;
            std::vector<double> canvasOnly;
            for ( int r = 0; r < kReps; ++r )
            {
                const auto t0 = std::chrono::steady_clock::now();
                dl.Reset();
                UI::BeginUIFrame( ctx, scene.Registry, kViewport );
                const auto t1 = std::chrono::steady_clock::now();
                (void)UI::RenderCanvas2D( ctx, scene.Registry, scene.Canvas, dl );
                const auto t2 = std::chrono::steady_clock::now();
                UI::EndUIFrame( ctx, scene.Registry, dl, /*input=*/nullptr );
                const auto t3 = std::chrono::steady_clock::now();
                samples.push_back( std::chrono::duration<double, std::micro>( t3 - t0 ).count() );
                canvasOnly.push_back( std::chrono::duration<double, std::micro>( t2 - t1 ).count() );
            }
            lvUs       = Summarise( samples );
            lvCanvasUs = Summarise( canvasOnly );
            lvDrawn    = DrawnCount( scene, ctx );
        }

        std::printf( "  %5d | %8.1f [%7.1f..%8.1f] | %8.1f [%7.1f..%8.1f] | %8zu | %8zu\n", rows, svUs.Median,
                     svUs.Lo, svUs.Hi, lvUs.Median, lvUs.Lo, lvUs.Hi, svDrawn, lvDrawn );
        std::printf( "        (of the list view's median, %.1f us is RenderCanvas2D itself)\n",
                     lvCanvasUs.Median );
    }
    std::printf( "\n" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
