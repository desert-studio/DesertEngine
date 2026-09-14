// "Every number the UI Debugger shows is readable without opening the editor."
//
// The panel this suite backs was written second on purpose. A panel whose only evidence is a screenshot
// has already been shipped in this project once and had to be rebuilt, so the order here is: a data
// source a test can read, then a window that reads it.
//
// THE LOAD-BEARING TEST IS `HidingASkippedElementChangesNothing`. Everything else checks one number; that
// one checks the RELATION between two walks that must agree — the introspection enumeration's opinion of
// which elements are drawn, against the real renderer's behaviour when each of them is hidden in turn. It
// is the drift detector for the day somebody changes what the walk skips, and it is written the way this
// project's defect taxonomy says to write one: assert the agreement, not either side.
//
// `NoTwoAdjacentBatchesCouldHaveMerged` is the same idea for the other half. DrawList2D decides what
// opens a batch; ClassifyBatchBreak names which of those decisions fired. If a field is added to the
// batch key and not to the classifier, two adjacent commands will classify as "could have merged" while
// the draw list plainly did not merge them — and that is a failure here rather than a wrong column.

#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIIntrospection.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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

using Desert::UI::BatchBreak;
using Desert::UI::Rect;
using Desert::UI::UIElementNode;
using Desert::UI::UIFrameProbe;
using Desert::UI::UIFrameProbeSink;
using Desert::UI::UISkipCause;
using Desert::UI::UIViewContext;
namespace ECS = Desert::ECS;
namespace R2D = Desert::Graphic::Render2D;
namespace UI  = Desert::UI;

namespace
{
    constexpr float kSide = 1000.0f; // Stretch canvas at 1000x1000: design px == screen px (scale 1)
    const Rect      kViewport{ 0.0f, 0.0f, kSide, kSide };

    // Two textures the draw list will treat as distinct opaque ids. Never dereferenced.
    int         g_TexA = 0;
    int         g_TexB = 0;
    const void* TexA()
    {
        return &g_TexA;
    }
    const void* TexB()
    {
        return &g_TexB;
    }

    // A canvas with a vertical stack of panels — the shape a real HUD has, and the shape the batching
    // question is about.
    struct Scene
    {
        entt::registry            Registry;
        entt::entity              Canvas = entt::null;
        std::vector<entt::entity> Panels;

        explicit Scene( int panelCount = 3 )
        {
            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kSide;
            canvas.ReferenceHeight = kSide;
            Registry.emplace<ECS::RelationshipComponent>( Canvas );

            for ( int i = 0; i < panelCount; ++i )
                Panels.push_back( AddPanel( Canvas, 0.0f, static_cast<float>( i ) * 60.0f, 200.0f, 50.0f ) );
        }

        entt::entity AddPanel( entt::entity parent, float x, float y, float w, float h )
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

        ECS::UILayoutData& Layout( entt::entity e )
        {
            return Registry.get<ECS::UILayoutComponent>( e ).Data;
        }
    };

    // One walk of the canvas into a fresh draw list, with a fresh context — the headless equivalent of a
    // frame. Returns whether the canvas drew.
    bool Walk( Scene& scene, R2D::DrawList2D& dl, UIViewContext& ctx )
    {
        dl.Reset();
        UI::BeginUIFrame( ctx, scene.Registry, kViewport );
        const bool drawn = UI::RenderCanvas2D( ctx, scene.Registry, scene.Canvas, dl ).IsSuccess();
        UI::EndUIFrame( ctx, scene.Registry, dl, /*input=*/nullptr );
        return drawn;
    }

    // The bytes a draw list holds, as one comparable value: geometry AND batch structure. Two walks that
    // agree here produced the same frame.
    std::string Fingerprint( const R2D::DrawList2D& dl )
    {
        std::string s;
        s.reserve( 4096 );
        for ( const R2D::Vertex2D& v : dl.GetVertices() )
            s.append( reinterpret_cast<const char*>( &v ), sizeof( v ) );
        for ( const R2D::DrawCommand& c : dl.GetCommands() )
        {
            s.append( reinterpret_cast<const char*>( &c.Texture ), sizeof( c.Texture ) );
            s.append( reinterpret_cast<const char*>( &c.ClipRect ), sizeof( c.ClipRect ) );
            s.append( reinterpret_cast<const char*>( &c.IndexCount ), sizeof( c.IndexCount ) );
            s.append( reinterpret_cast<const char*>( &c.Text ), sizeof( c.Text ) );
            s.append( reinterpret_cast<const char*>( &c.Glass ), sizeof( c.Glass ) );
        }
        return s;
    }

    const UIElementNode* NodeFor( const UIFrameProbe& probe, entt::entity e )
    {
        for ( const UIElementNode& n : probe.Elements )
            if ( n.Entity == e )
                return &n;
        return nullptr;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The batch half: what opened each draw call.
// ---------------------------------------------------------------------------------------------------

TEST( UIIntrospectionBatches, EachReasonIsNamedByTheThingThatCausedIt )
{
    R2D::DrawList2D dl;
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, glm::vec4( 1.0f ) );                        // batch 0: first
    dl.AddImage( TexA(), { 0, 0 }, { 10, 10 }, { 0, 0 }, { 1, 1 }, glm::vec4( 1.0f ) ); // batch 1: texture
    dl.AddImage( TexA(), { 0, 0 }, { 10, 10 }, { 0, 0 }, { 1, 1 }, glm::vec4( 1.0f ) ); // merged into 1
    dl.AddImage( TexB(), { 0, 0 }, { 10, 10 }, { 0, 0 }, { 1, 1 }, glm::vec4( 1.0f ) ); // batch 2: texture
    dl.AddText( TexB(), { 0, 0 }, { 10, 10 }, { 0, 0 }, { 1, 1 }, glm::vec4( 1.0f ) );  // batch 3: text mode
    dl.PushClipRect( { 0, 0 }, { 5, 5 } );
    dl.AddText( TexB(), { 0, 0 }, { 4, 4 }, { 0, 0 }, { 1, 1 }, glm::vec4( 1.0f ) ); // batch 4: clip rect
    dl.PopClipRect();
    dl.AddGlassRect( { 0, 0 }, { 10, 10 }, glm::vec4( 1.0f ) );  // batch 5: glass
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, glm::vec4( 1.0f ) ); // batch 6: after glass

    UIFrameProbe probe;
    UI::CaptureDrawList( dl, probe );

    ASSERT_EQ( probe.Batches.size(), 7u );
    EXPECT_EQ( probe.Batches[0].Break, BatchBreak::First );
    EXPECT_EQ( probe.Batches[1].Break, BatchBreak::Texture );
    EXPECT_EQ( probe.Batches[2].Break, BatchBreak::Texture );
    EXPECT_EQ( probe.Batches[3].Break, BatchBreak::Text );
    EXPECT_EQ( probe.Batches[4].Break, BatchBreak::ClipRect );
    EXPECT_EQ( probe.Batches[5].Break, BatchBreak::GlassSelf );
    EXPECT_EQ( probe.Batches[6].Break, BatchBreak::GlassPrev );

    // The census adds up: every batch is attributed to exactly one reason.
    std::uint32_t total = 0;
    for ( const std::uint32_t n : probe.Stats.BreakCounts )
        total += n;
    EXPECT_EQ( total, probe.Stats.Batches );
    EXPECT_EQ( probe.Stats.BreakCounts[static_cast<std::size_t>( BatchBreak::Texture )], 2u );
    EXPECT_EQ( probe.Stats.BreakCounts[static_cast<std::size_t>( BatchBreak::None )], 0u );
}

TEST( UIIntrospectionBatches, GeometryCountsAreTheDrawListsOwn )
{
    Scene           scene;
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    UIFrameProbe probe;
    UI::CaptureDrawList( dl, probe );

    EXPECT_EQ( probe.Stats.Vertices, dl.GetVertices().size() );
    EXPECT_EQ( probe.Stats.Indices, dl.GetIndices().size() );
    EXPECT_EQ( probe.Stats.Triangles, dl.GetIndices().size() / 3 );
    EXPECT_EQ( probe.Stats.Batches, dl.GetCommands().size() );
}

TEST( UIIntrospectionBatches, EveryRecordedBatchIsSubmitted )
{
    // A MEASURED FINDING, not a check that was going to pass anyway. Render2D::Flush skips a command whose
    // IndexCount is zero — so "batches recorded" and "draw calls submitted" are, in principle, different
    // numbers. They are not, and cannot be: every DrawList2D primitive that opens a command also appends
    // geometry, and AddRing (the one that can produce nothing) refuses at `segments < 3` BEFORE opening
    // one. The guard in Flush is unreachable through the public API as it stands.
    //
    // Both numbers are still kept, because they answer two different questions and the day the guard stops
    // being unreachable is the day this assertion says so. The panel prints "batches with no geometry"
    // only when it is non-zero, which is what makes its silence mean something.
    Scene scene( 4 );
    scene.Registry.emplace<ECS::UIImageComponent>( scene.Panels[1] );
    scene.Layout( scene.Panels[2] ).ClipContents = true;

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    UIFrameProbe probe;
    UI::CaptureDrawList( dl, probe );
    EXPECT_EQ( probe.Stats.EmptyBatches, 0u );
    EXPECT_EQ( probe.Stats.DrawCalls, probe.Stats.Batches );
    EXPECT_EQ( probe.Stats.Batches, probe.Stats.DrawCalls + probe.Stats.EmptyBatches );
}

TEST( UIIntrospectionBatches, NoTwoAdjacentBatchesCouldHaveMerged )
{
    // THE DRIFT DETECTOR. DrawList2D decides what opens a batch; ClassifyBatchBreak names which decision
    // fired. If the batch key gains a field the classifier does not know about, the draw list will refuse
    // to merge two commands the classifier thinks are identical — and that is this assertion.
    Scene scene( 6 );
    scene.Registry.emplace<ECS::UIImageComponent>( scene.Panels[1] );
    scene.Layout( scene.Panels[2] ).ClipContents = true;
    scene.AddPanel( scene.Panels[2], 0.0f, 0.0f, 40.0f, 40.0f );
    scene.Registry.get<ECS::UIPanelComponent>( scene.Panels[4] ).Data.BackdropBlur = 1.0f;

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    const auto& cmds = dl.GetCommands();
    ASSERT_GE( cmds.size(), 2u );
    for ( std::size_t i = 1; i < cmds.size(); ++i )
        EXPECT_NE( UI::ClassifyBatchBreak( cmds[i - 1], cmds[i] ), BatchBreak::None )
             << "batch " << i
             << " was opened for a reason ClassifyBatchBreak cannot name; the draw list's "
                "batch key and this classifier have drifted apart";
}

// ---------------------------------------------------------------------------------------------------
// The element half: who was drawn, and why the rest were not.
// ---------------------------------------------------------------------------------------------------

TEST( UIIntrospectionWalk, CountsAddUp )
{
    Scene scene;
    scene.Layout( scene.Panels[1] ).Visibility = ECS::UIVisibility::Hidden;

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    EXPECT_EQ( probe.Walk.Visited, 3u );
    EXPECT_EQ( probe.Walk.Drawn, 2u );
    EXPECT_EQ( probe.Walk.Skipped, 1u );
    EXPECT_EQ( probe.Walk.Drawn + probe.Walk.Skipped, probe.Walk.Visited );
    EXPECT_EQ( probe.Walk.SkipCounts[static_cast<std::size_t>( UISkipCause::SelfHidden )], 1u );
}

TEST( UIIntrospectionWalk, OwnAndInheritedAreDifferentAnswers )
{
    // "This element is Hidden" and "a panel above it is Hidden" look identical on screen and need
    // opposite fixes, so they are two causes and the panel names who stopped the walk.
    Scene              scene( 1 );
    const entt::entity child                   = scene.AddPanel( scene.Panels[0], 0.0f, 0.0f, 20.0f, 20.0f );
    const entt::entity grandchild              = scene.AddPanel( child, 0.0f, 0.0f, 10.0f, 10.0f );
    scene.Layout( scene.Panels[0] ).Visibility = ECS::UIVisibility::Hidden;

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    const UIElementNode* root = NodeFor( probe, scene.Panels[0] );
    const UIElementNode* mid  = NodeFor( probe, child );
    const UIElementNode* leaf = NodeFor( probe, grandchild );
    ASSERT_NE( root, nullptr );
    ASSERT_NE( mid, nullptr );
    ASSERT_NE( leaf, nullptr );

    EXPECT_EQ( root->Cause, UISkipCause::SelfHidden );
    EXPECT_EQ( root->CauseBy, scene.Panels[0] );
    EXPECT_EQ( mid->Cause, UISkipCause::AncestorSkipped );
    EXPECT_EQ( mid->CauseBy, scene.Panels[0] ) << "the blame must name the ancestor that stopped, not the parent";
    EXPECT_EQ( leaf->Cause, UISkipCause::AncestorSkipped );
    EXPECT_EQ( leaf->CauseBy, scene.Panels[0] );
}

TEST( UIIntrospectionWalk, ABindingThatSaysHiddenIsItsOwnReason )
{
    Scene scene( 2 );
    auto& b  = scene.Registry.emplace<ECS::UIBindingComponent>( scene.Panels[1] ).Data;
    b.Key    = "hud.visible";
    b.Target = ECS::UIBindTarget::Visible;
    UI::UIDataStore::Get().Set( "hud.visible", false );

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    const UIElementNode* bound = NodeFor( probe, scene.Panels[1] );
    ASSERT_NE( bound, nullptr );
    EXPECT_FALSE( bound->Drawn );
    EXPECT_EQ( bound->Cause, UISkipCause::BindingHidden );

    UI::UIDataStore::Get().Clear(); // the store is global; leave it as it was found
}

TEST( UIIntrospectionWalk, AScreenThatIsNotCurrentIsItsOwnReason )
{
    Scene scene( 2 );
    scene.Registry.emplace<ECS::UIScreenComponent>( scene.Panels[0] ).Data.Name = "Menu";
    scene.Registry.emplace<ECS::UIScreenComponent>( scene.Panels[1] ).Data.Name = "Settings";

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    // Exactly one screen is current, so exactly one of the two is on screen.
    EXPECT_EQ( probe.Walk.SkipCounts[static_cast<std::size_t>( UISkipCause::ScreenNotCurrent )], 1u );

    // Without a context the enumeration is an AUTHORING view and screens are not gated — an author must
    // be able to pick an element on a screen the game is not showing. That is a second mode, not a bug.
    std::vector<UIElementNode> authoring;
    ASSERT_TRUE( UI::EnumerateCanvas( scene.Registry, scene.Canvas, kViewport, authoring, nullptr ).IsSuccess() );
    for ( const UIElementNode& n : authoring )
        EXPECT_TRUE( n.Drawn ) << "an authoring enumeration must not gate on a view's current screen";
}

TEST( UIIntrospectionWalk, HidingASkippedElementChangesNothing )
{
    // THE RELATION. The enumeration's opinion of which elements are drawn is checked against the real
    // renderer, one element at a time: an element the enumeration calls SKIPPED cannot be contributing a
    // single byte, so hiding it must leave the draw list byte-identical.
    Scene              scene( 3 );
    const entt::entity underHidden             = scene.AddPanel( scene.Panels[0], 0.0f, 0.0f, 20.0f, 20.0f );
    scene.Layout( scene.Panels[0] ).Visibility = ECS::UIVisibility::Hidden;
    scene.Layout( scene.Panels[2] ).Visibility = ECS::UIVisibility::Collapsed;
    (void)underHidden;

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    const std::string baseline = Fingerprint( dl );
    ASSERT_GT( probe.Walk.Skipped, 0u );

    std::uint32_t checked = 0;
    for ( const UIElementNode& n : probe.Elements )
    {
        if ( n.Drawn || !scene.Registry.has<ECS::UILayoutComponent>( n.Entity ) )
            continue;
        auto&                   field = scene.Layout( n.Entity ).Visibility;
        const ECS::UIVisibility prev  = field;
        field                         = ECS::UIVisibility::Hidden;

        R2D::DrawList2D again;
        UIViewContext   ctx2;
        ASSERT_TRUE( Walk( scene, again, ctx2 ) );
        EXPECT_EQ( Fingerprint( again ), baseline )
             << "entity " << static_cast<std::uint32_t>( n.Entity )
             << " was reported as skipped, but hiding it changed what the renderer drew";

        field = prev;
        ++checked;
    }
    EXPECT_GT( checked, 0u );
}

TEST( UIIntrospectionWalk, AnElementScrolledOutOfItsListIsCountedAsClipped )
{
    // The walk does not cull: a row below the fold of a list still records geometry, and the scissor
    // throws it away. That is a real cost with no picture to show for it, so the panel counts it.
    //
    // The row is placed BELOW the list and well inside the viewport on purpose. A row scrolled off the
    // top would be outside the viewport too, and then "clipped by its list" and "off screen" would be
    // indistinguishable — the assertion would pass with the inherited clip ignored entirely.
    Scene scene( 1 );
    auto& sv                        = scene.Registry.emplace<ECS::UIScrollViewComponent>( scene.Panels[0] ).Data;
    sv.ContentHeight                = 400.0f;
    sv.ScrollY                      = 0.0f;
    const entt::entity onScreenRow  = scene.AddPanel( scene.Panels[0], 0.0f, 10.0f, 40.0f, 20.0f );
    const entt::entity belowTheFold = scene.AddPanel( scene.Panels[0], 0.0f, 200.0f, 40.0f, 20.0f );

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    const UIElementNode* visible = NodeFor( probe, onScreenRow );
    const UIElementNode* hidden  = NodeFor( probe, belowTheFold );
    ASSERT_NE( visible, nullptr );
    ASSERT_NE( hidden, nullptr );

    EXPECT_TRUE( hidden->Drawn ) << "the walk records its geometry; only the scissor removes it";
    EXPECT_TRUE( hidden->Clipped ) << "it is 150 px below its list's bottom edge and 780 px inside the "
                                      "viewport, so only the inherited clip can be what removes it";
    EXPECT_FALSE( visible->Clipped );
    EXPECT_EQ( probe.Walk.Clipped, 1u );

    // And the editor's pick must agree with that: a row is not clickable where it is not drawn.
    EXPECT_EQ( UI::PickElement( scene.Registry, scene.Canvas, { 10.0f, 210.0f }, kViewport ),
               entt::entity{ entt::null } );
    EXPECT_EQ( UI::PickElement( scene.Registry, scene.Canvas, { 10.0f, 20.0f }, kViewport ), onScreenRow );
}

TEST( UIIntrospectionWalk, ARotatedClipperCountsWhatItCutEvenThoughTheBoxesOverlap )
{
    // THE COLUMN AND THE CLIP MUST BE ASKED THE SAME QUESTION. Since Ю9 a turned clipper cuts its own
    // quadrilateral, so "fully clipped" stopped being answerable from bounding boxes: the row below sits in
    // a CORNER of the clipper's box, where the two boxes plainly overlap and not one pixel survives. A
    // Clipped computed from boxes alone reports it visible — the middle link dropping the oblique half of
    // the very property this column is about — so the box answer is asserted here beside the real one, and
    // it has to be the WRONG one.
    Scene              scene( 1 );
    const entt::entity clipper           = scene.AddPanel( scene.Canvas, 300.0f, 300.0f, 200.0f, 200.0f );
    scene.Layout( clipper ).ClipContents = true;
    scene.Layout( clipper ).Rotation     = 45.0f;
    scene.Layout( clipper ).Pivot        = { 0.5f, 0.5f };
    // 210..250 in the clipper's OWN x, so every corner of it is outside the clipper's left edge and the
    // clip removes all of it. Turned onto the screen it lands near a corner of the clipper's bounding box,
    // which is the whole point: the two BOXES still overlap by about 50x50 px there. The overlap is
    // asserted below rather than assumed, so a change of rotation convention fails loudly instead of
    // quietly making this test vacuous.
    const entt::entity inTheCorner = scene.AddPanel( clipper, -90.0f, 80.0f, 40.0f, 40.0f );

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    const UIElementNode* node = NodeFor( probe, inTheCorner );
    ASSERT_NE( node, nullptr );
    EXPECT_TRUE( node->Drawn ) << "the walk records its geometry; only the clip removes it";
    EXPECT_GT( node->VisiblePx.W, 0.0f ) << "the BOXES overlap — without that this test proves nothing, "
                                            "because a box-only answer would happen to be right";
    EXPECT_GT( node->VisiblePx.H, 0.0f );
    EXPECT_TRUE( node->Clipped ) << "every corner of it is outside one edge of the rotated clipper";
    EXPECT_EQ( probe.Walk.Clipped, 1u );

    // And the pointer agrees, which is what the column is worth: nothing is selectable where nothing drew.
    EXPECT_NE( UI::PickElement( scene.Registry, scene.Canvas, { 300.0f, 300.0f }, kViewport ), inTheCorner );
}

TEST( UIIntrospectionWalk, DrawOrderIsTheOrderTheWalkEmitsIn )
{
    Scene              scene( 2 );
    const entt::entity child = scene.AddPanel( scene.Panels[0], 0.0f, 0.0f, 20.0f, 20.0f );

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    UIFrameProbe probe;
    ASSERT_TRUE( UI::CaptureFrame( ctx, scene.Registry, { scene.Canvas }, dl, kViewport, probe ).IsSuccess() );

    // Parent before child, siblings in order — which is what makes "the last writer is topmost" readable.
    EXPECT_EQ( NodeFor( probe, scene.Panels[0] )->Order, 0 );
    EXPECT_EQ( NodeFor( probe, child )->Order, 1 );
    EXPECT_EQ( NodeFor( probe, scene.Panels[1] )->Order, 2 );
}

TEST( UIIntrospectionWalk, ARefusalIsNotAnEmptyFrame )
{
    // A probe that reported zero elements and success would read as "this canvas costs nothing".
    Scene              scene( 1 );
    const entt::entity notACanvas = scene.Panels[0];

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    UIFrameProbe    probe;
    const auto      result = UI::CaptureFrame( ctx, scene.Registry, { notACanvas }, dl, kViewport, probe );
    EXPECT_FALSE( result.IsSuccess() );
    EXPECT_FALSE( probe.Valid );
    EXPECT_FALSE( probe.Refusal.empty() );
    EXPECT_EQ( probe.Captures, 0u );
}

// ---------------------------------------------------------------------------------------------------
// The gate: a closed panel costs nothing.
// ---------------------------------------------------------------------------------------------------

TEST( UIIntrospectionSink, DisarmedItTouchesNothing )
{
    Scene           scene( 4 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    UIFrameProbeSink sink;
    ASSERT_FALSE( sink.IsArmed() );
    for ( int i = 0; i < 32; ++i )
        sink.Capture( ctx, scene.Registry, { scene.Canvas }, dl, kViewport );

    EXPECT_EQ( sink.Captures(), 0u );
    EXPECT_TRUE( sink.Frame().Elements.empty() );
    EXPECT_TRUE( sink.Frame().Batches.empty() );
    // Not one allocation: the vectors were never grown, which is the difference between "cheap" and "free".
    EXPECT_EQ( sink.Frame().Elements.capacity(), 0u );
    EXPECT_EQ( sink.Frame().Batches.capacity(), 0u );
}

TEST( UIIntrospectionSink, ArmedItCapturesAndDisarmingDropsTheFrame )
{
    Scene           scene( 4 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    UIFrameProbeSink sink;
    sink.SetArmed( true );
    sink.Capture( ctx, scene.Registry, { scene.Canvas }, dl, kViewport );
    EXPECT_EQ( sink.Captures(), 1u );
    EXPECT_TRUE( sink.Frame().Valid );
    EXPECT_EQ( sink.Frame().Walk.Drawn, 4u );

    // A reading from a frame that has since been redrawn still looks like evidence, so it is dropped.
    sink.SetArmed( false );
    EXPECT_FALSE( sink.Frame().Valid );
    EXPECT_TRUE( sink.Frame().Elements.empty() );
}

// ---------------------------------------------------------------------------------------------------
// What one element costs, measured against the real walk.
// ---------------------------------------------------------------------------------------------------

TEST( UIIntrospectionCost, AnElementBetweenTwoTexturesIsTheOneThatOpensABatch )
{
    // Three panels: two flat, one carrying a sprite that resolves to a texture. The sprite is what splits
    // one batch into three, and the measurement has to say so with a number.
    Scene scene( 3 );
    scene.Registry.get<ECS::UIPanelComponent>( scene.Panels[1] ).Data.Sprite =
         Desert::Assets::AssetHandle( kBackgroundHandle );
    g_BackgroundServiceArmed = true;

    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    ASSERT_EQ( dl.GetCommands().size(), 3u ) << "the sprite panel should split the flat run in two";

    const UI::UIElementCost cost =
         UI::ProbeElementCost( ctx, scene.Registry, scene.Canvas, scene.Panels[1], kViewport );
    g_BackgroundServiceArmed = false;

    ASSERT_TRUE( cost.Valid ) << cost.Refusal;
    EXPECT_EQ( cost.BatchesWith, 3u );
    EXPECT_EQ( cost.BatchesWithout, 1u ) << "hiding the textured panel lets the two flat runs merge";
    EXPECT_TRUE( cost.OpensBatch );
    EXPECT_EQ( cost.FirstBatch, 1u );
    EXPECT_EQ( cost.Break, BatchBreak::Texture ) << "and the reason is the texture, which is the fix";
    EXPECT_GT( cost.Vertices, 0u );

    // The scene is left exactly as it was found: the measurement hides the element and restores it.
    EXPECT_EQ( scene.Layout( scene.Panels[1] ).Visibility, ECS::UIVisibility::Visible );
}

TEST( UIIntrospectionCost, AFlatPanelBetweenFlatPanelsOpensNothing )
{
    // The negative control. Without it "OpensBatch" could be true for everything and still pass above.
    Scene           scene( 3 );
    R2D::DrawList2D dl;
    UIViewContext   ctx;
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    const UI::UIElementCost cost =
         UI::ProbeElementCost( ctx, scene.Registry, scene.Canvas, scene.Panels[1], kViewport );
    ASSERT_TRUE( cost.Valid ) << cost.Refusal;
    EXPECT_EQ( cost.BatchesWith, cost.BatchesWithout );
    EXPECT_FALSE( cost.OpensBatch );
    EXPECT_GT( cost.Vertices, 0u ) << "it still costs geometry; it just does not cost a draw call";
}

TEST( UIIntrospectionCost, RefusesByNameWhenItCannotMeasure )
{
    Scene scene( 1 );

    const UI::UIElementCost noLayout =
         UI::ProbeElementCost( UIViewContext{}, scene.Registry, scene.Canvas, scene.Canvas, kViewport );
    EXPECT_FALSE( noLayout.Valid );
    EXPECT_FALSE( noLayout.Refusal.empty() );

    const UI::UIElementCost nothing =
         UI::ProbeElementCost( UIViewContext{}, scene.Registry, scene.Canvas, entt::null, kViewport );
    EXPECT_FALSE( nothing.Valid );
    EXPECT_FALSE( nothing.Refusal.empty() );
}

// --- Ю11: the panel must be able to say "a MATERIAL broke this batch" -------------------------------

TEST( UIIntrospectionBatches, AMaterialBreakIsNamedAsAMaterialAndCountedAsAPipelineBind )
{
    // Recorded directly rather than through a canvas walk: this is about the CLASSIFIER agreeing with
    // the draw list, and a walk would only add a way for the test to be about something else.
    static const char kMatA = 0, kMatB = 0;

    Desert::Graphic::Render2D::DrawList2D dl;
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddMaterialRect( &kMatA, { 20.0f, 0.0f }, { 30.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddMaterialRect( &kMatB, { 40.0f, 0.0f }, { 50.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    UI::UIFrameProbe probe;
    UI::CaptureDrawList( dl, probe );

    ASSERT_EQ( probe.Batches.size(), 3u );
    EXPECT_EQ( probe.Batches[0].Break, BatchBreak::First );
    EXPECT_EQ( probe.Batches[1].Break, BatchBreak::Material );
    EXPECT_EQ( probe.Batches[2].Break, BatchBreak::Material );
    EXPECT_EQ( probe.Batches[1].Material, &kMatA );
    EXPECT_EQ( probe.Batches[2].Material, &kMatB );

    // Two distinct materials are two distinct PIPELINES, which is the cost that separates a material
    // change from a texture change: a texture is a descriptor bind, a material is a pipeline bind too.
    // The counter used to be 0/1/2 for the three built-in pipelines and could not have said this.
    EXPECT_EQ( probe.Stats.UniqueMaterials, 2u );
    EXPECT_EQ( probe.Stats.PipelineSwitches, 2u );
    EXPECT_EQ( probe.Stats.UniqueTextures, 0u );
}

TEST( UIIntrospectionBatches, ACanvasWithNoMaterialReportsNoneOfIt )
{
    // The negative control of the test above: the two new numbers must stay at zero for every canvas
    // that does not use a material, or the panel would start attributing ordinary batches to Ю11.
    Desert::Graphic::Render2D::DrawList2D dl;
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddRectFilled( { 20.0f, 0.0f }, { 30.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    UI::UIFrameProbe probe;
    UI::CaptureDrawList( dl, probe );

    ASSERT_EQ( probe.Batches.size(), 1u );
    EXPECT_EQ( probe.Batches[0].Material, nullptr );
    EXPECT_EQ( probe.Stats.UniqueMaterials, 0u );
    EXPECT_EQ( probe.Stats.PipelineSwitches, 0u );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
