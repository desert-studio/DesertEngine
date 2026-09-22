// THE FIRST SceneRenderer::Init IS SKIPPED, AND SOMETHING ELSE HAS TO GUARANTEE THE SECOND ONE HAPPENS.
//
// WHAT WAS WRONG. The editor initialised its scene renderer TWICE on every start. `EditorLayer::OnAttach`
// built one for the empty "New Scene" — pipelines, framebuffers, cascade attachments, a device-idle wait
// — and the project's default scene, queued for load by the CONSTRUCTOR that ran a moment earlier, threw
// all of it away as soon as the staged startup finished. Nobody ever saw a frame of that first scene:
// `OnUpdate` returns before any scene render for the whole of the startup load, and `OnUIRender` draws
// the loading overlay and nothing else. Measured cost in Debug: see the task's report.
//
// WHY IT NEEDS A TEST RATHER THAN A COMMENT. The fix is one condition in OnAttach and one fallback in
// OnUpdate, and they are 300 lines apart in a file three developers are editing. Alone, each looks
// harmless; together they are the whole guarantee that the scene is initialised before the first frame
// records against it. Delete the fallback and the editor renders a scene whose renderer has no systems —
// but ONLY when the queued scene turns out to be missing, unreadable, or written by an older build, which
// is a path no ordinary run takes. That is exactly the "both ends right, the link between them missing"
// shape this project keeps paying for (DEV_CONTRACT §2.3.1).
//
// The relation is between two statements in one .cpp that no header includes, so it is read out of the
// source, as AssetPreloadCensus next door does for the same reason.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    constexpr const char* kEditorLayer = "Editor/Source/EditorLayer.cpp";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + kEditorLayer );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // //-comments removed. Both halves of this relation are surrounded by comments that quote them.
    std::string StripLineComments( const std::string& text )
    {
        std::string       out;
        std::stringstream in( text );
        std::string       line;
        while ( std::getline( in, line ) )
        {
            const auto first = line.find_first_not_of( " \t" );
            if ( first == std::string::npos || line.compare( first, 2, "//" ) != 0 )
            {
                out += line;
                out += '\n';
            }
            else
            {
                out += '\n';
            }
        }
        return out;
    }
} // namespace

TEST( SceneInitDeferral, TheSourceWasFound )
{
    // An empty read makes every `find(...) != npos` below fail for the wrong reason and every
    // `== npos` pass for the wrong reason. Contract §1.4: an empty successful answer is a silent
    // wrong answer.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository from "
                                 << std::filesystem::current_path().string();
    const std::string source = StripLineComments( ReadAll( root + kEditorLayer ) );
    ASSERT_GT( source.size(), 100000u )
         << "EditorLayer.cpp read as " << source.size() << " bytes; that is not the file this suite is about";
}

TEST( SceneInitDeferral, OnAttachDoesNotBuildARendererForAScenceAlreadyQueuedForReplacement )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string source = StripLineComments( ReadAll( root + kEditorLayer ) );

    const std::size_t guard = source.find( "if ( !m_SceneLoadRequested )" );
    ASSERT_NE( guard, std::string::npos )
         << "OnAttach initialises the main scene unconditionally again. When the constructor has already "
            "queued a scene load — which it has for --scene and for every project with a default scene — "
            "that Init builds a whole set of pipelines and framebuffers for a scene no frame will ever "
            "contain, and the load then destroys them.";

    // The guard has to be the one in FRONT of the Init, not merely somewhere in the file.
    const std::size_t init = source.find( "m_MainScene->Init()", guard );
    ASSERT_NE( init, std::string::npos ) << "no m_MainScene->Init() after the guard";
    EXPECT_LT( init - guard, 200u ) << "the !m_SceneLoadRequested guard and the Init it is supposed to "
                                       "govern are "
                                    << ( init - guard )
                                    << " characters apart; they are no longer the same statement";
}

TEST( SceneInitDeferral, TheDeferredLoadInitialisesTheSceneEvenWhenItRefusesTheFile )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string source = StripLineComments( ReadAll( root + kEditorLayer ) );

    const std::size_t load = source.find( "LoadSceneInternal( path );" );
    ASSERT_NE( load, std::string::npos ) << "the deferred scene load is gone from OnUpdate";

    const std::size_t fallback = source.find( "if ( !m_MainScene->IsInitialized() )", load );
    ASSERT_NE( fallback, std::string::npos )
         << "nothing after the deferred load asks whether the scene ended up initialised. "
            "LoadSceneInternal has three early returns — the file is gone, unreadable, or written by an "
            "older build — and each leaves the scene untouched. With OnAttach's Init skipped, that is a "
            "renderer with no render systems and a frame about to record into it.";
    EXPECT_LT( fallback - load, 900u )
         << "the IsInitialized fallback is " << ( fallback - load )
         << " characters after the load it belongs to; it has drifted out of that block";

    const std::size_t init = source.find( "m_MainScene->Init()", fallback );
    ASSERT_NE( init, std::string::npos ) << "the fallback asks the question and never initialises";
    EXPECT_LT( init - fallback, 200u ) << "the fallback's Init is not inside the fallback any more";

    // The editor passes bind to framebuffers that only exist after Init, so the registry follows it here
    // exactly as it does inside LoadSceneInternal.
    const std::size_t registry = source.find( "std::make_unique<Render::RenderRegistry>", fallback );
    ASSERT_NE( registry, std::string::npos ) << "the fallback initialises the scene and leaves the editor "
                                                "passes bound to nothing";
    EXPECT_LT( registry - fallback, 700u ) << "the registry rebuild has drifted out of the fallback";
}

TEST( SceneInitDeferral, TheEditorPassRegistryIsNotBuiltAgainstAnUninitialisedScene )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string source = StripLineComments( ReadAll( root + kEditorLayer ) );

    // OnAttach's registry construction is the FIRST one in the file. Every editor pass creates its
    // pipeline from `scene->GetTargetFramebuffer()`, which does not exist until Init has run.
    const std::size_t first = source.find( "std::make_unique<Render::RenderRegistry>" );
    ASSERT_NE( first, std::string::npos ) << "the editor pass registry is never constructed";

    const std::size_t guard = source.rfind( "if ( m_MainScene->IsInitialized() )", first );
    ASSERT_NE( guard, std::string::npos )
         << "OnAttach builds the editor pass registry unconditionally. With the first Init skipped there "
            "is no target framebuffer for the grid, collider and UI passes to bind their pipelines to.";
    EXPECT_LT( first - guard, 120u ) << "the IsInitialized guard is " << ( first - guard )
                                     << " characters in front of the construction it governs";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
