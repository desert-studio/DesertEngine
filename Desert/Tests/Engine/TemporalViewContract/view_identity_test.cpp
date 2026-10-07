// TAA1 step 3 — WHICH VIEW, WHICH WORLD, ONE PREVIOUS-FRAME SOURCE.
//
// (1) Identity is never an address: a scene reloaded into the same Scene object gets a new generation
//     (Core::NextSceneGeneration, taken at construction and in Scene::Clear), and the view's camera identity
//     (MakeViewCameraIdentity) carries it, so SceneViewState resets with CameraCut.
//     Mutations that turn these red: MakeViewCameraIdentity ignoring the generation; Scene::Clear not taking a
//     new generation; NextSceneGeneration returning a constant.
// (2) Censuses of the one previous-frame source: no renderer keeps its own previous view-projection; SSR, GI and
//     the clouds keep a PassHistoryStamp; SceneRenderer::OnUpdate opens the view frame before the first pass that
//     reads it and closes it after the graph executed.
//     Mutations: re-adding an m_PrevViewProj* member; moving m_ViewState.BeginFrame below AddFrameSSAO; dropping
//     m_ViewState.EndFrame.
#include <Engine/Core/SceneGeneration.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <regex>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ViewIdentityTest
{
    using namespace Desert::Graphic;
    namespace Scal = Common::Scalability;

    std::string ReadText( const std::filesystem::path& path )
    {
        std::ifstream      file( path, std::ios::binary );
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::filesystem::path EngineSource()
    {
        return Desert::TestSupport::RepositoryRoot() / "Desert" / "Desert" / "Source" / "Engine";
    }

    // A non-temporal frame (FXAA at 100 %): no upscaler, no history textures — only the reset decision is tested.
    ViewInputs Inputs( const uint64_t cameraIdentity, const uint64_t sceneIdentity )
    {
        ViewInputs      in;
        const glm::vec3 eye( 0.0f, 150.0f, 0.0f );
        in.View       = glm::lookAt( eye, eye + glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        in.Projection = Desert::Core::MakePerspective( glm::radians( 60.0f ), 16.0f / 9.0f, 10.0f, 5'000'000.0f );
        in.CameraPosition           = eye;
        in.NearPlane                = 10.0f;
        in.FarPlane                 = 5'000'000.0f;
        in.CameraIdentity           = cameraIdentity;
        in.SceneIdentity            = sceneIdentity;
        in.Output                   = ViewExtent{ 1920, 1080 };
        in.AntiAliasing.Method      = Scal::AntiAliasingMethod::FXAA;
        in.AntiAliasing.PostProcess = Scal::AntiAliasingMethod::FXAA;
        in.TimeSeconds              = 1.0;
        return in;
    }

    HistoryResetReason Begin( SceneViewState& state, const ViewInputs& in )
    {
        const auto frame = state.BeginFrame( in, nullptr );
        EXPECT_TRUE( frame.IsSuccess() ) << ( frame.IsSuccess() ? std::string() : frame.GetError() );
        return frame.IsSuccess() ? frame.GetValue().HistoryReset : HistoryResetReason::FirstFrame;
    }
} // namespace ViewIdentityTest

using namespace ViewIdentityTest;

TEST( ViewIdentity, SceneGenerationsAreNeverZeroAndNeverRepeat )
{
    const uint64_t a = Desert::Core::NextSceneGeneration();
    const uint64_t b = Desert::Core::NextSceneGeneration();
    EXPECT_NE( a, 0u );
    EXPECT_GT( b, a );
}

TEST( ViewIdentity, CameraIdentityCarriesTheGenerationAndTheEntity )
{
    EXPECT_NE( MakeViewCameraIdentity( 1, 5 ), MakeViewCameraIdentity( 2, 5 ) );
    EXPECT_NE( MakeViewCameraIdentity( 1, 5 ), MakeViewCameraIdentity( 1, 6 ) );
    EXPECT_EQ( MakeViewCameraIdentity( 3, 5 ), MakeViewCameraIdentity( 3, 5 ) );
}

// The reload: same Scene object (so the same address, same entity ids), next generation. The view must not
// reproject the new world through the old one's previous matrices.
TEST( ViewIdentity, AReloadIntoTheSameSceneResetsTheViewHistory )
{
    constexpr uint32_t kCameraEntity = 5;
    const uint64_t     before        = Desert::Core::NextSceneGeneration();
    const uint64_t     after         = Desert::Core::NextSceneGeneration(); // what Scene::Clear takes

    SceneViewState state;
    EXPECT_EQ( Begin( state, Inputs( MakeViewCameraIdentity( before, kCameraEntity ), before ) ),
               HistoryResetReason::FirstFrame );
    state.EndFrame( RDG::ExecuteReport{} );
    // Control: the same world again keeps the history.
    EXPECT_EQ( Begin( state, Inputs( MakeViewCameraIdentity( before, kCameraEntity ), before ) ),
               HistoryResetReason::None );
    state.EndFrame( RDG::ExecuteReport{} );

    EXPECT_EQ( Begin( state, Inputs( MakeViewCameraIdentity( after, kCameraEntity ), after ) ),
               HistoryResetReason::CameraCut );
}

TEST( ViewIdentity, SceneTakesANewGenerationAtConstructionAndAtEveryClear )
{
    const std::string header = ReadText( EngineSource() / "Core" / "Scene.hpp" );
    const std::regex initialised( R"(m_Generation\s*=\s*NextSceneGeneration\(\);)" );
    EXPECT_TRUE( std::regex_search( header, initialised ) )
         << "Scene must take its generation where it is created (member initialiser)";

    const std::string source = ReadText( EngineSource() / "Core" / "Scene.cpp" );
    const auto        clear  = source.find( "void Scene::Clear()" );
    ASSERT_NE( clear, std::string::npos );
    const auto end = source.find( "\n    }", clear );
    ASSERT_NE( end, std::string::npos );
    const std::string clearBody = source.substr( clear, end - clear );
    EXPECT_TRUE( std::regex_search( clearBody, initialised ) )
         << "Scene::Clear (every load / reload / new scene) must take a new generation";
}

TEST( ViewIdentityCensus, NoRendererKeepsItsOwnPreviousViewProjection )
{
    std::vector<std::string> offenders;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( EngineSource() ) )
    {
        const auto ext = entry.path().extension();
        if ( !entry.is_regular_file() || ( ext != ".hpp" && ext != ".cpp" && ext != ".h" ) )
            continue;
        // A declaration, not a mention: ViewFrame.hpp and PassHistory.hpp name the deleted members in prose.
        static const std::regex member( R"(glm::mat4\s+m_PrevViewProj)" );
        if ( std::regex_search( ReadText( entry.path() ), member ) )
            offenders.push_back( entry.path().generic_string() );
    }
    EXPECT_TRUE( offenders.empty() ) << "a per-renderer previous matrix is back (ViewFrame::PrevViewProjection is "
                                        "the one source): "
                                     << ( offenders.empty() ? std::string() : offenders.front() );
}

TEST( ViewIdentityCensus, TemporalPassesKeepAPassHistoryStamp )
{
    const auto graphic = EngineSource() / "Graphic" / "Systems" / "Scene";
    for ( const auto& path :
          { graphic / "Deferred" / "SSRRenderer.hpp", graphic / "Deferred" / "GIResolveRenderer.hpp",
            graphic / "Clouds" / "VolumetricCloudRenderer.hpp" } )
        EXPECT_NE( ReadText( path ).find( "PassHistoryStamp m_History;" ), std::string::npos ) << path.string();
}

TEST( ViewIdentityCensus, OnUpdateOpensTheViewFrameBeforeItsReadersAndClosesItAfterExecute )
{
    const std::string source = ReadText( EngineSource() / "Graphic" / "SceneRenderer.cpp" );
    const auto        body   = source.find( "void SceneRenderer::OnUpdate(" );
    ASSERT_NE( body, std::string::npos );
    const auto begin   = source.find( "m_ViewState.BeginFrame(", body );
    const auto ssao    = source.find( "AddFrameSSAO(", body );
    const auto execute = source.find( "ExecuteGraph( graph )", body );
    const auto end     = source.find( "m_ViewState.EndFrame(", body );
    ASSERT_NE( begin, std::string::npos );
    ASSERT_NE( ssao, std::string::npos );
    ASSERT_NE( execute, std::string::npos );
    ASSERT_NE( end, std::string::npos );
    EXPECT_LT( begin, ssao );
    EXPECT_LT( ssao, execute );
    EXPECT_LT( execute, end );
}
