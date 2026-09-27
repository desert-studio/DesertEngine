// SEVERAL ANGLES ON ONE WORLD: the bookkeeping, and the one structural fact that makes it worth having.
//
// This file sits beside scene_view_lifetime_test.cpp because it is the other half of the same fact. That
// one asserts that CLOSING a view returns its renderer slot and leaves the survivors addressable; this one
// asserts what a view IS — Engine/Core/SceneViewList.hpp — and that the scene walks the ECS ONCE however
// many views are open.
//
// WHY THE SECOND HALF IS A SOURCE CENSUS AND NOT A CALL. Scene.cpp needs a VkDevice and is compiled by no
// suite (scripts/CI/UnreachedSources.sh), so "ExecuteSystems ran once with two views open" cannot be
// observed here by running it. It was observed by RUNNING THE EDITOR, which is where the numbers in the
// report come from; what a suite can do is stand guard over the SHAPE that produced them, so that moving
// the ECS pass inside the per-view loop — the single edit that would undo this — reddens instead of
// quietly doubling a frame's cost. A census cannot prove the timing; it can prove that the only structure
// that could produce that timing is still there, and that is the half that rots.

#include <Engine/Core/SceneViewList.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Stand-ins. The list reads no byte of either type — it is pointer identity and ordering — which is
    // exactly why SceneViewList is a template and why this suite can drive the REAL rules instead of a
    // copy of them that would be free to drift.
    struct FakeRenderer
    {
        int Tag = 0;
    };

    struct FakeCamera
    {
        int Tag = 0;
    };

    using List = Desert::Core::SceneViewList<FakeRenderer, FakeCamera>;

    std::shared_ptr<FakeCamera> Cam( int tag )
    {
        return std::make_shared<FakeCamera>( FakeCamera{ tag } );
    }

    // Walks up from the working directory looking for a file only the repository has — the runner's
    // working directory is not fixed. (The convention in this directory is to copy this rather than share
    // a header; see RendererSceneLifetime, PureVirtualCensus, DeviceLostCensus.)
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/Scene.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return "./";
    }

    std::string ReadFile( const std::string& relative )
    {
        std::ifstream in( RepoRoot() + relative );
        EXPECT_TRUE( in.good() ) << "could not open " << relative;
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The body of one function, from its signature to the next one at the same indentation. Crude on
    // purpose: the alternative is a parser, and what is being asked is "does this token appear inside
    // THIS function", which does not need one.
    std::string FunctionBody( const std::string& source, const std::string& signature )
    {
        const auto start = source.find( signature );
        EXPECT_NE( start, std::string::npos ) << "signature not found: " << signature;
        if ( start == std::string::npos )
            return {};

        const auto end = source.find( "\n    }\n", start );
        EXPECT_NE( end, std::string::npos ) << "no closing brace found for: " << signature;
        return source.substr( start, end == std::string::npos ? std::string::npos : end - start );
    }

    size_t CountOf( const std::string& haystack, const std::string& needle )
    {
        size_t count = 0;
        for ( size_t at = haystack.find( needle ); at != std::string::npos;
              at        = haystack.find( needle, at + needle.size() ) )
            ++count;
        return count;
    }
} // namespace

// ── THE LIST ─────────────────────────────────────────────────────────────────────────────────────────

TEST( SceneViewList, AViewIsAddedAtTheBackAndKeepsItsOwnCamera )
{
    List         list;
    FakeRenderer a{ 1 };
    FakeRenderer b{ 2 };

    const auto first  = list.Add( &a, Cam( 10 ) );
    const auto second = list.Add( &b, Cam( 20 ) );

    ASSERT_TRUE( first );
    ASSERT_TRUE( second );
    EXPECT_EQ( *first, 0u );
    EXPECT_EQ( *second, 1u );
    EXPECT_EQ( list.Count(), 2u );

    // THE DEFECT THIS REPLACES, stated as an assertion: the scene used to hold ONE camera, so two
    // renderers meant two pictures from one place. Two views must carry two cameras.
    ASSERT_NE( list.At( 0 ), nullptr );
    ASSERT_NE( list.At( 1 ), nullptr );
    EXPECT_NE( list.At( 0 )->Camera, list.At( 1 )->Camera );
    EXPECT_EQ( list.At( 0 )->Camera->Tag, 10 );
    EXPECT_EQ( list.At( 1 )->Camera->Tag, 20 );
}

TEST( SceneViewList, ANullRendererAndADuplicateAreBothRefused )
{
    List         list;
    FakeRenderer a{ 1 };

    ASSERT_TRUE( list.Add( &a, Cam( 10 ) ) );

    EXPECT_FALSE( list.Add( nullptr, Cam( 11 ) ) );
    // A duplicate is the dangerous refusal: the same renderer twice records two passes into one set of
    // per-frame GPU state, which breaks the (frame x slot) rule from inside the engine and shows up as a
    // torn picture with nothing in the log.
    EXPECT_FALSE( list.Add( &a, Cam( 12 ) ) );
    EXPECT_EQ( list.Count(), 1u );
    // The refused duplicate must not have replaced the camera of the view that was already there.
    EXPECT_EQ( list.At( 0 )->Camera->Tag, 10 );
}

TEST( SceneViewList, RemovingTheMiddleViewKeepsTheSurvivorsInOrder )
{
    // FOUR VIEWS, AND THE ONE REMOVED IS NOT THE SECOND-TO-LAST. Written with three first, and the
    // swap-and-pop mutation PASSED: removing index 1 of three moves the back into index 1, which is
    // where an erase would have left it anyway. The scenario, not the rule, was what agreed. With four,
    // an erase leaves [a, c, d] and a swap leaves [a, d, c], and only one of those is index order.
    List         list;
    FakeRenderer a{ 1 };
    FakeRenderer b{ 2 };
    FakeRenderer c{ 3 };
    FakeRenderer d{ 4 };

    (void)list.Add( &a, Cam( 10 ) );
    (void)list.Add( &b, Cam( 20 ) );
    (void)list.Add( &c, Cam( 30 ) );
    (void)list.Add( &d, Cam( 40 ) );

    EXPECT_TRUE( list.Remove( &b ) );
    ASSERT_EQ( list.Count(), 3u );

    // ERASE, NOT SWAP-AND-POP. Index 0 is "the first view still open" and Scene's play-state camera rule
    // reads exactly that one; a swap would move a different view into the position that rule points at.
    EXPECT_EQ( list.At( 0 )->Renderer, &a );
    EXPECT_EQ( list.At( 1 )->Renderer, &c );
    EXPECT_EQ( list.At( 2 )->Renderer, &d );
    EXPECT_EQ( list.At( 0 )->Camera->Tag, 10 );
    EXPECT_EQ( list.At( 1 )->Camera->Tag, 30 );
    EXPECT_EQ( list.At( 2 )->Camera->Tag, 40 );
}

TEST( SceneViewList, ClosingAViewTwiceSaysSoInsteadOfPretendingItWorked )
{
    List         list;
    FakeRenderer a{ 1 };
    FakeRenderer stranger{ 9 };

    (void)list.Add( &a, Cam( 10 ) );

    EXPECT_TRUE( list.Remove( &a ) );
    EXPECT_FALSE( list.Remove( &a ) );        // already gone
    EXPECT_FALSE( list.Remove( &stranger ) ); // never here
}

TEST( SceneViewList, AWorldWithNoViewIsAStateAndNotAFailure )
{
    List         list;
    FakeRenderer a{ 1 };

    EXPECT_TRUE( list.Empty() );
    EXPECT_EQ( list.At( 0 ), nullptr ); // and not a dangling reference the caller cannot test

    (void)list.Add( &a, Cam( 10 ) );
    EXPECT_TRUE( list.Remove( &a ) );

    // A scene whose last viewport closed still exists and still simulates; it just has nowhere to put a
    // picture. Before the list, "no renderer" was an uninitialised raw pointer.
    EXPECT_TRUE( list.Empty() );
    EXPECT_EQ( list.At( 0 ), nullptr );
}

TEST( SceneViewList, AReopenedViewGoesToTheBackAndNotIntoTheHoleItLeft )
{
    List         list;
    FakeRenderer a{ 1 };
    FakeRenderer b{ 2 };

    (void)list.Add( &a, Cam( 10 ) );
    (void)list.Add( &b, Cam( 20 ) );
    EXPECT_TRUE( list.Remove( &a ) );

    const auto again = list.Add( &a, Cam( 11 ) );
    ASSERT_TRUE( again );
    EXPECT_EQ( *again, 1u );
    EXPECT_EQ( list.At( 0 )->Renderer, &b ); // b is now the first view, and index 0 means "first"
    EXPECT_EQ( list.IndexOf( &a ), std::optional<size_t>( 1u ) );
}

// ── THE SHAPE THAT MAKES N VIEWS COST ONE ECS PASS ───────────────────────────────────────────────────

TEST( SceneViewCost, TheEcsIsWalkedOnceAndOnlyTheGpuPassesAreMultiplied )
{
    const std::string body = FunctionBody( ReadFile( "Desert/Desert/Source/Engine/Core/Scene.cpp" ),
                                           "Common::BoolResultStr Scene::OnUpdate( const Common::Timestep& ts )" );
    ASSERT_FALSE( body.empty() );

    // Comments are stripped before the shape is read: a census that fires on prose describing what it
    // forbids gets switched off, and takes the real finding with it when it goes.
    std::string code;
    {
        std::istringstream lines( body );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            const auto comment = line.find( "//" );
            code += ( comment == std::string::npos ? line : line.substr( 0, comment ) );
            code += '\n';
        }
    }

    // ONE CALL, and it is the whole claim of this change: a second angle on one world costs a second set
    // of GPU passes, not a second walk of every component pool. Two calls here means somebody put the ECS
    // pass inside the view loop.
    EXPECT_EQ( CountOf( code, "ExecuteSystems(" ), 1u )
         << "Scene::OnUpdate must walk the ECS exactly once, however many views are open.";

    // ...and it must come BEFORE the per-view loop, not inside it. The loop is named by the profile scope
    // whose two numbers are the measurement in the report, so the scope name is pinned with it.
    const auto ecs      = code.find( "ExecuteSystems(" );
    const auto viewLoop = code.find( "DESERT_PROFILE_SCOPE( \"Scene: Views\" )" );
    ASSERT_NE( viewLoop, std::string::npos )
         << "the per-view GPU loop lost its profile scope; the ECS/GPU split is no longer measurable.";
    EXPECT_LT( ecs, viewLoop ) << "the ECS pass must finish before any view records.";

    // The recorded command buffers are REPLAYED per view (ExecuteAll does not consume the arena), so this
    // call belongs inside the loop and there must be exactly one of it.
    EXPECT_EQ( CountOf( code, "buffer->ExecuteAll(" ), 1u );
    EXPECT_LT( viewLoop, code.find( "buffer->ExecuteAll(" ) )
         << "the draw replay must be inside the per-view loop, or only one view gets the frame's draws.";

    // ONE VIEW'S FRAME IS ONE CONTIGUOUS SEQUENCE, and this is the assertion that costs the most to
    // restore if it is lost. The scene used to expose the open and the close as separate frame phases,
    // each looping the views; with two views open that overlapped the two renderers' brackets and the
    // SECOND view's HDR target came out empty while every input to it was provably identical. So: the
    // open, the replay, the render and the close all sit inside this one loop, in this order.
    const auto open   = code.find( "Renderer->BeginScene(" );
    const auto replay = code.find( "buffer->ExecuteAll(" );
    const auto render = code.find( "Renderer->OnUpdate(" );
    const auto close  = code.find( "Renderer->EndScene(" );
    ASSERT_NE( open, std::string::npos );
    ASSERT_NE( render, std::string::npos );
    ASSERT_NE( close, std::string::npos );
    EXPECT_LT( viewLoop, open ) << "a view must be OPENED inside the per-view loop, not in a phase of its own.";
    EXPECT_LT( open, replay );
    EXPECT_LT( replay, render );
    EXPECT_LT( render, close ) << "a view must be CLOSED before the next one opens.";
    EXPECT_EQ( CountOf( code, "Renderer->BeginScene(" ), 1u );
    EXPECT_EQ( CountOf( code, "Renderer->EndScene(" ), 1u );
}

TEST( SceneViewCost, TheSceneHasNoSeparateFramePhasesLeftForACallerToInterleave )
{
    const std::string header = ReadFile( "Desert/Desert/Source/Engine/Core/Scene.hpp" );
    ASSERT_FALSE( header.empty() );

    std::string code;
    {
        std::istringstream lines( header );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            const auto comment = line.find( "//" );
            code += ( comment == std::string::npos ? line : line.substr( 0, comment ) );
            code += '\n';
        }
    }

    // THE ORDERING RULE IS ENFORCED BY THERE BEING NOTHING TO ORDER. Scene::OnUpdate brackets each
    // view's renderer itself; if a scene-level BeginScene/EndScene pair comes back, a host can once
    // again put work between one renderer's open and its close — which is the shape that rendered the
    // second view black, and which no test of the running editor caught because one renderer per scene
    // never exposes it.
    EXPECT_EQ( CountOf( code, "BeginScene" ), 0u )
         << "Scene must not expose a frame-open phase separate from OnUpdate.";
    EXPECT_EQ( CountOf( code, "EndScene" ), 0u )
         << "Scene must not expose a frame-close phase separate from OnUpdate.";
    EXPECT_NE( code.find( "Common::BoolResultStr OnUpdate( const Common::Timestep& ts );" ), std::string::npos )
         << "Scene::OnUpdate must be the one call that renders a frame, and it must be able to refuse.";
}

TEST( SceneViewCost, TheRendererIsHandedItsViewsCameraAndDoesNotAskTheScene )
{
    const std::string body =
         FunctionBody( ReadFile( "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" ),
                       "Common::BoolResultStr SceneRenderer::BeginScene( const Desert::Core::Scene& scene," );
    ASSERT_FALSE( body.empty() );

    std::string code;
    {
        std::istringstream lines( body );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            const auto comment = line.find( "//" );
            code += ( comment == std::string::npos ? line : line.substr( 0, comment ) );
            code += '\n';
        }
    }

    // `scene.GetMainCamera()` is view 0's camera. A renderer that reads it renders view 0's angle no
    // matter which view it belongs to — which is how "several viewports" could only ever mean "several
    // worlds", and it is a defect no frame of a one-view editor can show.
    EXPECT_EQ( CountOf( code, "GetMainCamera" ), 0u )
         << "SceneRenderer::BeginScene must take its camera from the view, not from the scene.";
    EXPECT_NE( code.find( "m_SceneInfo.ActiveCamera = camera;" ), std::string::npos );
}

// ── A RECORDED COMMAND IS REPLAYED, SO IT MAY NOT GIVE ITS PAYLOAD AWAY ──────────────────────────────

TEST( SceneViewCost, NoRenderCommandHandsItsPayloadAwayWhenItExecutes )
{
    // THE DEFECT, AND IT WAS LIVE. PointLightCommand::Execute and SpotLightCommand::Execute both did
    // `renderer.Add*Light( std::move( Light ) )`. One renderer per scene never noticed: the command was
    // executed once and then destroyed. A scene with TWO views replays the same recording into each
    // view's renderer, so the first view got the light and the second got a moved-from value — a light
    // that exists in one viewport and not in the other, with nothing in the log.
    //
    // EVERY COMMAND IS NAMED, and the list is derived from the directory rather than counted: a new
    // command file that is never added here would be a row this census silently does not guard, and a
    // count pinned instead of the rows can be satisfied by editing the count.
    const std::string              dir      = "Desert/Desert/Source/Engine/Graphic/Render/Commands/";
    const std::vector<std::string> commands = {
         "DrawGenericMeshCommand.hpp",      "DrawMeshCommand.hpp",  "DrawSkinnedMeshCommand.hpp",
         "DrawSlotMaterialMeshCommand.hpp", "HeightFogCommand.hpp", "PointLightCommand.hpp",
         "ProceduralSkyCommand.hpp",        "SkyboxCommand.hpp",    "SpotLightCommand.hpp",
         "VolumetricCloudCommand.hpp",
    };

    for ( const auto& file : commands )
    {
        const std::string source = ReadFile( dir + file );
        ASSERT_FALSE( source.empty() ) << file;

        // Only the body of Execute is examined: a CONSTRUCTOR may (and should) move its arguments into
        // the command — that happens once, while recording, and is what keeps recording allocation-free.
        const auto at = source.find( "void Execute( SceneRenderer& renderer ) override" );
        ASSERT_NE( at, std::string::npos ) << file << " has no Execute to check; the census cannot see it.";

        std::string body = source.substr( at );
        std::string code;
        {
            std::istringstream lines( body );
            std::string        line;
            while ( std::getline( lines, line ) )
            {
                if ( !code.empty() && line == "        }" )
                {
                    code += line;
                    break;
                }
                const auto comment = line.find( "//" );
                code += ( comment == std::string::npos ? line : line.substr( 0, comment ) );
                code += '\n';
            }
        }

        EXPECT_EQ( CountOf( code, "std::move(" ), 0u )
             << file << ": Execute must not hand its payload away — it is replayed once per view.";
    }
}
