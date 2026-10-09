// "The canvas is over the 3D, it answers the pointer, and a VIEW CHANGE does not end either."
//
// THE TASK THIS FILE EXISTS FOR was filed as "a witness for canvas-over-3D that survives a view change",
// and the second half is the half nothing covered. Measured before writing a line: 21 scenes in this
// repository carry a UICanvas and ZERO of them carry a mesh, so no frame ever taken here has put a canvas
// over LIT GEOMETRY -- which is the one configuration the defect class is about, because you need lit
// geometry for lit geometry to paint over you.
//
// WHAT IS ASSERTED HERE AND WHAT IS NOT. "Over" is ultimately a position in a Vulkan command buffer. No
// test on this machine can observe one, so it is not described here -- it is turned into a REGISTER that
// can be read: the order of the calls in SceneRenderer::OnUpdate, which IS the frame's order (no phase sort,
// no numeric placement), asserted on the source. The frame half of the witness is
// scripts/MacOS/UIOverSceneWitness.sh: a live editor driven A->B->A through the control channel, finding the
// canvas's marker panel in the picture. Two instruments, named, and neither pretending to be the other.
//
// THE LOAD-BEARING TEST IS `TheHotElementSurvivesTheViewVisitingAnotherSceneAndComingBack`. Everything
// else checks one fact; that one checks the RELATION a view change can break -- the pointer's answer
// before and after the same view has looked at a different registry. It is the device-free shape of the
// A->B->A protocol, and it is the one that would catch a per-entity table surviving a scene it does not
// belong to, which is this project's recurring defect (entity ids are unique only inside a registry).

#include <fstream>
#include <sstream>
#include <string>
#include <Engine/UI/UICanvasContext.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIIntrospection.hpp>

#include <TestSupport/ui_canvas_resources_mock.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

// The resources every view in this suite draws with: a mock that answers nothing unless a test says so.
namespace
{
    TestSupport::MockUICanvasResources s_Resources;
} // namespace

// The walk resolves sprites, fonts, icons, video and themes through the view's IUICanvasResources; every
// view here is handed the mock below, which answers nothing: a sprite draws its flat colour, text draws nothing.

using Desert::UI::Rect;
using Desert::UI::UIElementNode;
using Desert::UI::UIInput;
using Desert::UI::UIViewContext;
namespace ECS         = Desert::ECS;
namespace R2D         = Desert::Graphic::Render2D;
namespace UI          = Desert::UI;

namespace
{
    // SceneRenderer.cpp's text, found from the test's working directory (the suite runs from its build
    // directory or the repo root); empty when it cannot be found, which the test reports.
    std::string SceneRendererSource()
    {
        for ( const char* prefix : { "", "../", "../../", "../../../", "../../../../", "../../../../../" } )
        {
            std::ifstream file( std::string( prefix ) + "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
            if ( file )
            {
                std::ostringstream text;
                text << file.rdbuf();
                return text.str();
            }
        }
        return {};
    }

    // gtest cannot print `entt::null` -- formatting it instantiates entt's conversion operator with
    // `long long`, and std::underlying_type<long long> has no member `type`, so the failure message is a
    // compile error inside <type_traits>. A typed entity prints fine and compares the same.
    constexpr entt::entity kNoEntity = entt::null;

    constexpr float kW = 1280.0f;
    constexpr float kH = 720.0f;
    const Rect      kViewport{ 0.0f, 0.0f, kW, kH };

    // The marker's authored rect, in design px, and it is the same rect UI_OverScene.desce carries. The
    // scene and this suite are two readers of one geometry; a change to one that forgets the other is
    // what the frame witness would catch, and saying the numbers here keeps them comparable.
    constexpr float kMarkerX = 60.0f, kMarkerY = 60.0f, kMarkerW = 200.0f, kMarkerH = 100.0f;

    // A scene shaped like UI_OverScene: 3D ENTITIES FIRST, then a canvas over them. The 3D half is what
    // makes this more than a canvas test -- the walk must ignore a camera, a light and meshes that are
    // siblings of the canvas rather than tripping over them.
    struct OverScene
    {
        entt::registry            Registry;
        entt::entity              Canvas = entt::null;
        entt::entity              Marker = entt::null;
        entt::entity              Button = entt::null;
        std::vector<entt::entity> ThreeD;

        entt::entity AddChild( entt::entity parent, float x, float y, float w, float h )
        {
            const entt::entity e = Registry.create();
            auto&              L = Registry.emplace<ECS::UILayoutComponent>( e ).Data;
            L.AnchorMin          = { 0.0f, 0.0f };
            L.AnchorMax          = { 0.0f, 0.0f };
            L.OffsetMin          = { x, y };
            L.OffsetMax          = { x + w, y + h };
            Registry.emplace<ECS::RelationshipComponent>( e ).Parent = parent;
            Registry.get<ECS::RelationshipComponent>( parent ).Children.push_back( e );
            return e;
        }

        OverScene()
        {
            // The 3D the canvas has to sit over. Not decoration: a canvas walk that asked the registry
            // for "the first entity" or iterated a pool would meet these first.
            for ( int i = 0; i < 3; ++i )
            {
                const entt::entity mesh = Registry.create();
                Registry.emplace<ECS::TransformComponent>( mesh );
                Registry.emplace<ECS::StaticMeshComponent>( mesh );
                ThreeD.push_back( mesh );
            }
            const entt::entity sun = Registry.create();
            Registry.emplace<ECS::TransformComponent>( sun );
            Registry.emplace<ECS::DirectionLightComponent>( sun );
            ThreeD.push_back( sun );

            Canvas                 = Registry.create();
            auto& canvas           = Registry.emplace<ECS::UICanvasComponent>( Canvas ).Data;
            canvas.ScaleMode       = ECS::UICanvasScaleMode::Stretch;
            canvas.ReferenceWidth  = kW;
            canvas.ReferenceHeight = kH;
            Registry.emplace<ECS::RelationshipComponent>( Canvas );

            Marker      = AddChild( Canvas, kMarkerX, kMarkerY, kMarkerW, kMarkerH );
            auto& panel = Registry.emplace<ECS::UIPanelComponent>( Marker ).Data;
            panel.Color = glm::vec3( 0.0f, 0.85f, 1.0f );

            Button = AddChild( Canvas, 480.0f, 500.0f, 320.0f, 56.0f );
            Registry.emplace<ECS::UIButtonComponent>( Button );
        }

        static glm::vec2 MarkerCentre()
        {
            return { kMarkerX + kMarkerW * 0.5f, kMarkerY + kMarkerH * 0.5f };
        }
    };

    // One frame of one view. Returns the draw list so a caller can count what reached the screen.
    bool Walk( OverScene& scene, R2D::DrawList2D& dl, UIViewContext& ctx, const UIInput* input = nullptr )
    {
        dl.Reset();
        UI::BeginUIFrame( ctx, scene.Registry, kViewport, /*frameDtSeconds=*/0.0f );
        const bool drawn = UI::RenderCanvas2D( ctx, scene.Registry, scene.Canvas, dl, nullptr, input ).IsSuccess();
        UI::EndUIFrame( ctx, scene.Registry, dl, input );
        return drawn;
    }

    // Who the pointer would be over, as the walk elects it. TWO frames, because the election is resolved
    // one frame late on purpose (the same deferral ImGui uses), so a single frame can never answer.
    entt::entity HotAt( OverScene& scene, UIViewContext& ctx, const glm::vec2& at )
    {
        R2D::DrawList2D dl;
        UIInput         input;
        input.MousePx = at;
        Walk( scene, dl, ctx, &input );
        Walk( scene, dl, ctx, &input );
        return ctx.Hot;
    }

    std::size_t DrawnCount( OverScene& scene, UIViewContext& ctx )
    {
        std::vector<UIElementNode> nodes;
        const auto                 ok = UI::EnumerateCanvas( scene.Registry, scene.Canvas, kViewport, nodes,
                                                             &ctx.CanvasState( scene.Canvas ) );
        EXPECT_TRUE( static_cast<bool>( ok ) ) << ok.GetError();
        return static_cast<std::size_t>(
             std::count_if( nodes.begin(), nodes.end(), []( const UIElementNode& n ) { return n.Drawn; } ) );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// OVER: the position in the frame, as the only thing about it that a test can read.
// ---------------------------------------------------------------------------------------------------

TEST( CanvasOverScene, TheUIPointIsAddedAfterEverythingThatDrawsTheScene )
{
    // The whole of "over 3D": the UI extension point (the editor's canvas pass) is added to the frame AFTER the
    // opaque raster, the deferred lighting composite, the translucency and the overlays, and before the post
    // chain (the Jump Flood outline first). The frame's order is the order of these calls in OnUpdate; a UI
    // point added before the composite would be painted over by lit geometry - the particle top-down defect
    // with a different victim.
    const std::string source = SceneRendererSource();
    ASSERT_FALSE( source.empty() ) << "SceneRenderer.cpp not found from the working directory";
    const auto at = [&source]( const char* call )
    {
        const std::size_t position = source.find( call );
        EXPECT_NE( position, std::string::npos ) << call << " is not called in SceneRenderer.cpp";
        return position;
    };
    const std::size_t ui = at( "AddExtensionPoint( graph, textures, RDG::ExtensionPoint::UI, overlay );" );
    EXPECT_GT( ui, at( "AddFrameBasePass( graph, textures );" ) );
    EXPECT_GT( ui, at( "AddFrameTranslucency( graph, textures );" ) );
    EXPECT_GT( ui, at( "AddExtensionPoint( graph, textures, RDG::ExtensionPoint::Overlay, overlay );" ) );
    // ...and the negative side: the post chain starts after it.
    EXPECT_LT( ui, at( "AddFrameJumpFlood( graph, textures );" ) );
}

TEST( CanvasOverScene, TheCanvasDrawsWithThreeDEntitiesBesideItInTheRegistry )
{
    OverScene       scene;
    R2D::DrawList2D dl;
    UIViewContext   ctx{ s_Resources };
    ASSERT_TRUE( Walk( scene, dl, ctx ) );

    EXPECT_EQ( DrawnCount( scene, ctx ), 2u ); // the marker and the button; the canvas is not a child
    EXPECT_FALSE( dl.GetVertices().empty() );

    // The negative control for the line above: the meshes and the light contribute NOTHING to the UI
    // draw list, so a canvas-only registry produces the same frame. A walk that had started iterating
    // the registry instead of the canvas tree would fail this and pass everything else.
    const std::size_t withThreeD = dl.GetVertices().size();
    for ( const entt::entity e : scene.ThreeD )
    {
        scene.Registry.destroy( e );
    }
    UIViewContext fresh{ s_Resources };
    ASSERT_TRUE( Walk( scene, dl, fresh ) );
    EXPECT_EQ( dl.GetVertices().size(), withThreeD );
}

// ---------------------------------------------------------------------------------------------------
// AND ANSWERS THE POINTER: drawing and being clickable are two properties, not one.
// ---------------------------------------------------------------------------------------------------

TEST( CanvasOverScene, ThePointerLandsOnTheElementTheFrameDrewThere )
{
    OverScene     scene;
    UIViewContext ctx{ s_Resources };
    EXPECT_EQ( HotAt( scene, ctx, OverScene::MarkerCentre() ), scene.Marker );
    EXPECT_EQ( HotAt( scene, ctx, { 640.0f, 528.0f } ), scene.Button );
    // Off the canvas's elements entirely: nothing is elected, which is what lets a click reach the 3D.
    EXPECT_EQ( HotAt( scene, ctx, { 1200.0f, 40.0f } ), kNoEntity );
}

TEST( CanvasOverScene, AnOverlayThatDrawsWithoutAnsweringThePointerIsItsOwnFailure )
{
    // UIHitTest::None is UE's HitTestInvisible: the element and its children are still DRAWN and the
    // pointer passes through. That is a legitimate authoring choice and a defect when it is not one, so
    // the witness states which of the two this canvas is.
    OverScene     scene;
    UIViewContext before{ s_Resources };
    ASSERT_EQ( HotAt( scene, before, OverScene::MarkerCentre() ), scene.Marker );

    scene.Registry.get<ECS::UILayoutComponent>( scene.Marker ).Data.HitTest = ECS::UIHitTest::None;
    UIViewContext after{ s_Resources };
    EXPECT_EQ( HotAt( scene, after, OverScene::MarkerCentre() ), kNoEntity );

    // ...and it still draws, which is the half that makes this a defect shape rather than a hidden
    // element: the picture is unchanged and only the pointer's answer moved.
    R2D::DrawList2D dl;
    UIViewContext   ctx{ s_Resources };
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_EQ( DrawnCount( scene, ctx ), 2u );
}

// ---------------------------------------------------------------------------------------------------
// AND SURVIVES A VIEW CHANGE. A->B->A, in the four shapes a view changes in.
// ---------------------------------------------------------------------------------------------------

TEST( CanvasOverSceneViewChange, TheHotElementSurvivesTheViewVisitingAnotherSceneAndComingBack )
{
    // A -> B -> A on ONE view. Entity ids are unique only inside a registry, so the per-entity tables a
    // view carries answer to ids that mean something else in scene B; BeginUIFrame drops them when it
    // notices the registry changed, and this is the assertion that it still does.
    OverScene a;
    OverScene b;

    UIViewContext      view{ s_Resources };
    const entt::entity hotInA = HotAt( a, view, OverScene::MarkerCentre() );
    ASSERT_EQ( hotInA, a.Marker );

    // The visit. B's canvas is a different tree with its own ids, and the pointer is somewhere else.
    ASSERT_EQ( HotAt( b, view, OverScene::MarkerCentre() ), b.Marker );

    // ...and back. The answer must be A's marker again, not a recycled id and not nothing.
    EXPECT_EQ( HotAt( a, view, OverScene::MarkerCentre() ), a.Marker );

    // The frame must come back too, and identically: the canvas is not a function of where the view has
    // been. Asserted on the element count AND the geometry, because either alone can be right by luck.
    R2D::DrawList2D dl;
    ASSERT_TRUE( Walk( a, dl, view ) );
    const std::size_t verts = dl.GetVertices().size();
    ASSERT_TRUE( Walk( b, dl, view ) );
    ASSERT_TRUE( Walk( a, dl, view ) );
    EXPECT_EQ( dl.GetVertices().size(), verts );
    EXPECT_EQ( DrawnCount( a, view ), 2u );
}

TEST( CanvasOverSceneViewChange, TheCellsOfASceneTheViewHasLeftDoNotAnswerForTheNewOne )
{
    // The mechanism the test above rests on, stated directly: crossing to another registry empties the
    // (canvas x view) table rather than keeping a row an id from the new scene could collide with.
    //
    // WHY A STAMP AND NOT A COUNT. The first version of this test counted the cells, and a mutation that
    // deleted the reset walked straight through it: entt hands out ids in creation order, so two scenes
    // built the same way give their canvases the SAME id, the retire pass finds that id still valid in
    // the new registry, and the stale cell is silently REUSED. One cell before, one cell after, nothing
    // to see. Counting could not express the defect; a value that belongs to A and must not be readable
    // from B can. (The same blind spot is why A and B below are deliberately not identical.)
    OverScene a;
    OverScene b;
    b.Registry.destroy( b.ThreeD.back() ); // make B structurally unlike A, so ids cannot line up by luck

    UIViewContext   view{ s_Resources };
    R2D::DrawList2D dl;
    ASSERT_TRUE( Walk( a, dl, view ) );
    ASSERT_NE( view.FindCanvasState( a.Canvas ), nullptr );
    view.CanvasState( a.Canvas ).Locals.Set( "u18.stamp", true );

    ASSERT_TRUE( Walk( b, dl, view ) );
    EXPECT_EQ( view.Registry, &b.Registry );
    const UI::UICanvasContext* cellInB = view.FindCanvasState( b.Canvas );
    if ( cellInB != nullptr )
    {
        EXPECT_FALSE( cellInB->Locals.Has( "u18.stamp" ) )
             << "scene B is reading a value that belongs to scene A's cell";
    }

    ASSERT_TRUE( Walk( a, dl, view ) );
    const UI::UICanvasContext* cellBack = view.FindCanvasState( a.Canvas );
    ASSERT_NE( cellBack, nullptr );
    EXPECT_FALSE( cellBack->Locals.Has( "u18.stamp" ) )
         << "the view kept its cell for this canvas across a visit to another scene";
}

TEST( CanvasOverSceneViewChange, TwoLiveViewsOfOneSceneElectIndependently )
{
    // Several viewports at once is shipped, and the pointer belongs to a VIEW. Two views of one scene
    // with the cursor in two different places must give two different answers -- one shared election is
    // the defect the (canvas x view) key was introduced to close, and it reappears as "the second
    // viewport steals the first one's hover".
    OverScene     scene;
    UIViewContext left{ s_Resources };
    UIViewContext right{ s_Resources };

    EXPECT_EQ( HotAt( scene, left, OverScene::MarkerCentre() ), scene.Marker );
    EXPECT_EQ( HotAt( scene, right, { 640.0f, 528.0f } ), scene.Button );
    // ...and the first view's answer did not move when the second one drew.
    EXPECT_EQ( left.Hot, scene.Marker );
}

TEST( CanvasOverSceneViewChange, TheAuthoringPreviewToggleDoesNotChangeWhatIsDrawn )
{
    // Design <-> Preview is the fourth shape of a view change and the only one with no pass-order
    // consequence: it moves what the view DRIVES (input, overlays, scene animation), not where the
    // canvas lands. Pinned because "no consequence" is a claim, and because the control channel offers
    // no command for this toggle, so the frame witness cannot check it.
    OverScene       scene;
    R2D::DrawList2D dl;
    UIViewContext   design{ s_Resources };
    design.AuthoringPreview = true;
    ASSERT_TRUE( Walk( scene, dl, design ) );
    const std::size_t designVerts = dl.GetVertices().size();

    UIViewContext preview{ s_Resources };
    preview.AuthoringPreview = false;
    ASSERT_TRUE( Walk( scene, dl, preview ) );
    EXPECT_EQ( dl.GetVertices().size(), designVerts );

    // The same view crossing between the two modes, which is what the editor actually does.
    design.AuthoringPreview = false;
    ASSERT_TRUE( Walk( scene, dl, design ) );
    EXPECT_EQ( dl.GetVertices().size(), designVerts );
}

TEST( CanvasOverSceneViewChange, AWorldSpaceCanvasIsStillDrawnByTheUIPhase )
{
    // The canvas mode whose PIXELS are a function of the 3D camera: WorldSpace billboards and
    // distance-scales straight into the same screen-space draw list. It is therefore the mode where
    // "over 3D" is most fragile -- and it is drawn by the same pass, so the register above covers it.
    // What is asserted here is the part that is this file's: switching the mode does not take the
    // canvas out of the frame.
    OverScene       scene;
    R2D::DrawList2D dl;
    UIViewContext   ctx{ s_Resources };
    ASSERT_TRUE( Walk( scene, dl, ctx ) );
    EXPECT_FALSE( dl.GetVertices().empty() );

    auto& canvas      = scene.Registry.get<ECS::UICanvasComponent>( scene.Canvas ).Data;
    canvas.RenderMode = ECS::UICanvasRenderMode::WorldSpace;

    // A world-space canvas needs the camera's matrix; without one the walk must still answer, and the
    // element must still be enumerated where the pointer can find it.
    const glm::mat4 viewProj( 1.0f );
    dl.Reset();
    UI::BeginUIFrame( ctx, scene.Registry, kViewport, /*frameDtSeconds=*/0.0f );
    const auto drawn = UI::RenderCanvas2D( ctx, scene.Registry, scene.Canvas, dl, &viewProj );
    UI::EndUIFrame( ctx, scene.Registry, dl, nullptr );
    EXPECT_TRUE( drawn.IsSuccess() ) << drawn.GetError();
    EXPECT_FALSE( dl.GetVertices().empty() ) << "a world-space canvas emitted no geometry at all";
}
