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
// (3) Outside-driven cameras (the editor camera, a pinned preview camera) each carry an id issued once at
//     construction (Core::CameraSourceTicket), so alternating two of them on one view cuts at every switch with
//     no ResetTemporalHistory call, and the same camera twice keeps its history.
//     Mutations: a ticket copy sharing its source's id; IssueCameraSourceId returning a constant;
//     MakeViewCameraIdentity dropping bit 32 (issued ids collide with entity ids); Camera::GetSourceId returning
//     the entity only; SceneRenderer keying the view by GetSourceEntity.
//     Mutations: re-adding an m_PrevViewProj* member; moving m_ViewState.BeginFrame below AddFrameSSAO; dropping
//     m_ViewState.EndFrame.
#include <Engine/Core/CameraSourceId.hpp>
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
        const std::ifstream file( path, std::ios::binary );
        std::ostringstream  text;
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

TEST( ViewIdentity, IssuedCameraSourceIdsAreUniqueAndNeverAnEntity )
{
    using namespace Desert::Core;
    const CameraSourceTicket a;
    const CameraSourceTicket b;
    EXPECT_NE( a.Get(), b.Get() );
    EXPECT_NE( a.Get() & kIssuedCameraSourceBit, 0u );
    EXPECT_NE( b.Get() & kIssuedCameraSourceBit, 0u );

    // A copy is another camera object; an assignment keeps the destination object's id.
    const auto               copyOf = []( const CameraSourceTicket& ticket ) { return ticket; };
    const CameraSourceTicket copy   = copyOf( a );
    EXPECT_NE( copy.Get(), a.Get() );
    CameraSourceTicket assigned;
    const auto         assignedId = assigned.Get();
    assigned                      = a;
    EXPECT_EQ( assigned.Get(), assignedId );

    // Never the identity of an entity camera, whatever the entity id (bit 32 survives the identity packing).
    const uint64_t generation = NextSceneGeneration();
    const auto     lowBits    = static_cast<uint32_t>( a.Get() & 0xFFFFFFFFull );
    EXPECT_NE( MakeViewCameraIdentity( generation, a.Get() ),
               MakeViewCameraIdentity( generation, EntityCameraSource( lowBits ) ) );
}

// The editor camera and a pinned preview camera on ONE renderer: each switch is a camera cut by identity alone,
// the same camera on consecutive frames keeps its history.
TEST( ViewIdentity, TwoOutsideDrivenCamerasAlternatedOnOneViewCutAtEverySwitch )
{
    const Desert::Core::CameraSourceTicket editorCamera;
    const Desert::Core::CameraSourceTicket previewCamera;
    const uint64_t                         generation = Desert::Core::NextSceneGeneration();
    const auto                             through    = [&]( const Desert::Core::CameraSourceTicket& camera )
    { return Inputs( MakeViewCameraIdentity( generation, camera.Get() ), generation ); };

    SceneViewState state;
    EXPECT_EQ( Begin( state, through( editorCamera ) ), HistoryResetReason::FirstFrame );
    state.EndFrame( RDG::ExecuteReport{} );
    EXPECT_EQ( Begin( state, through( editorCamera ) ), HistoryResetReason::None );
    state.EndFrame( RDG::ExecuteReport{} );
    EXPECT_EQ( Begin( state, through( previewCamera ) ), HistoryResetReason::CameraCut );
    state.EndFrame( RDG::ExecuteReport{} );
    EXPECT_EQ( Begin( state, through( previewCamera ) ), HistoryResetReason::None );
    state.EndFrame( RDG::ExecuteReport{} );
    EXPECT_EQ( Begin( state, through( editorCamera ) ), HistoryResetReason::CameraCut );
    state.EndFrame( RDG::ExecuteReport{} );
    EXPECT_EQ( Begin( state, through( editorCamera ) ), HistoryResetReason::None );
    state.EndFrame( RDG::ExecuteReport{} );
}

// The renderer keys its view by the camera source id, and a camera without a source entity falls back to its
// issued ticket — never to the shared kNoSourceEntity.
TEST( ViewIdentityCensus, TheViewIsKeyedByTheCameraSourceId )
{
    const std::string camera = ReadText( EngineSource() / "Core" / "Camera.hpp" );
    EXPECT_TRUE( std::regex_search(
         camera,
         std::regex(
              R"(GetSourceId\s*\(\s*\)\s*const[\s\S]*?\?\s*EntityCameraSource\s*\(\s*m_SourceEntity\s*\)\s*:\s*m_SourceTicket\s*\.\s*Get\s*\(\s*\))" ) ) )
         << "Camera::GetSourceId must fall back to the camera object's issued ticket";
    // Every census here matches tokens with \s* / \s+ between them, so a clang-format re-wrap cannot turn it red.
    EXPECT_TRUE( std::regex_search( camera, std::regex( R"(CameraSourceTicket\s+m_SourceTicket\s*;)" ) ) );

    const std::string renderer = ReadText( EngineSource() / "Graphic" / "SceneRenderer.cpp" );
    EXPECT_TRUE( std::regex_search(
         renderer,
         std::regex(
              R"(MakeViewCameraIdentity\s*\(\s*m_SceneGeneration\s*,\s*cam\s*->\s*GetSourceId\s*\(\s*\)\s*\))" ) ) )
         << "SceneRenderer must key the view by (scene generation, camera source id)";
}

TEST( ViewIdentity, SceneTakesANewGenerationAtConstructionAndAtEveryClear )
{
    const std::string header = ReadText( EngineSource() / "Core" / "Scene.hpp" );
    const std::regex  initialised( R"(m_Generation\s*=\s*NextSceneGeneration\s*\(\s*\)\s*;)" );
    EXPECT_TRUE( std::regex_search( header, initialised ) )
         << "Scene must take its generation where it is created (member initialiser)";

    const std::string source = ReadText( EngineSource() / "Core" / "Scene.cpp" );
    std::smatch       head;
    ASSERT_TRUE( std::regex_search( source, head, std::regex( R"(void\s+Scene\s*::\s*Clear\s*\(\s*\)\s*\{)" ) ) );
    // The body by brace depth, not by an indentation pattern, so a re-format cannot cut it short.
    const auto start = static_cast<size_t>( head.position( 0 ) );
    size_t     end   = start + static_cast<size_t>( head.length( 0 ) );
    int        depth = 1;
    for ( ; end < source.size() && depth > 0; ++end )
    {
        if ( source[end] == '{' )
            ++depth;
        else if ( source[end] == '}' )
            --depth;
    }
    ASSERT_EQ( depth, 0 ) << "Scene::Clear has no closing brace";
    const std::string clearBody = source.substr( start, end - start );
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
