// The world's clock (Engine/Core/WorldTime.hpp) and the census that it is the ONLY clock.
//
// The first half asserts the clock's rules on the value type the scene owns: pause stops game time,
// dilation scales it, real time always moves, a fixed step makes two runs identical, and the editor's
// Realtime toggle is what moves preview time. The second half reads the source of every consumer that used
// to keep its own clock and asserts it now reads the world's — a relation between files no unit test of
// the class could see.
#include <Engine/Core/WorldTime.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <format>
#include <iterator>
#include <vector>

using Desert::Core::WorldClockMode;
using Desert::Core::WorldTime;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    std::string RepoRoot()
    {
        std::filesystem::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::filesystem::exists( prefix / "Desert/Desert/Source/Engine/Core/WorldTime.hpp" ) )
                return ( prefix / "" ).generic_string();
            prefix /= "..";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
} // namespace

// ---- The clock ---------------------------------------------------------------------------------------

TEST( WorldTime, GameTimeAdvancesByTheMeasuredStepInGame )
{
    WorldTime time;
    for ( int i = 0; i < 10; ++i )
        time.Tick( 0.02f, WorldClockMode::Game );
    EXPECT_NEAR( time.GetGameTimeSeconds(), 0.2, 1e-5 );
    EXPECT_FLOAT_EQ( time.GetDeltaSeconds(), 0.02f );
}

TEST( WorldTime, PauseStopsGameTimeButNotRealTime )
{
    WorldTime time;
    time.Tick( 0.1f, WorldClockMode::Game );
    time.SetPaused( true );
    for ( int i = 0; i < 5; ++i )
        time.Tick( 0.1f, WorldClockMode::Game );
    EXPECT_NEAR( time.GetGameTimeSeconds(), 0.1, 1e-6 );
    EXPECT_EQ( time.GetDeltaSeconds(), 0.0f );
    EXPECT_NEAR( time.GetRealTimeSeconds(), 0.6, 1e-5 );
    EXPECT_FLOAT_EQ( time.GetRealDeltaSeconds(), 0.1f );

    time.SetPaused( false );
    time.Tick( 0.1f, WorldClockMode::Game );
    EXPECT_NEAR( time.GetGameTimeSeconds(), 0.2, 1e-6 );
}

TEST( WorldTime, DilationScalesGameTimeAndNotRealTime )
{
    WorldTime time;
    ASSERT_TRUE( time.SetTimeDilation( 0.5f ) );
    for ( int i = 0; i < 4; ++i )
        time.Tick( 0.1f, WorldClockMode::Game );
    EXPECT_NEAR( time.GetGameTimeSeconds(), 0.2, 1e-6 );
    EXPECT_FLOAT_EQ( time.GetDeltaSeconds(), 0.05f );
    EXPECT_NEAR( time.GetRealTimeSeconds(), 0.4, 1e-6 );
}

TEST( WorldTime, DilationOutsideTheRangeIsRefusedAndKeepsTheOldValue )
{
    WorldTime time;
    ASSERT_TRUE( time.SetTimeDilation( 2.0f ) );
    for ( const float bad : { 0.0f, -1.0f, 1000.0f, std::numeric_limits<float>::quiet_NaN() } )
    {
        const auto result = time.SetTimeDilation( bad );
        EXPECT_FALSE( result ) << bad;
        if ( !result )
            EXPECT_NE( result.GetError().find( "time dilation" ), std::string::npos );
    }
    EXPECT_FLOAT_EQ( time.GetTimeDilation(), 2.0f );
}

TEST( WorldTime, RealTimeMovesInEveryMode )
{
    for ( const auto mode : { WorldClockMode::Frozen, WorldClockMode::Preview, WorldClockMode::Game } )
    {
        WorldTime time;
        time.Tick( 0.25f, mode );
        EXPECT_NEAR( time.GetRealTimeSeconds(), 0.25, 1e-7 ) << static_cast<int>( mode );
    }
}

TEST( WorldTime, AHitchIsClampedToTheUndilatedMaximum )
{
    WorldTime time;
    time.Tick( 5.0f, WorldClockMode::Game );
    EXPECT_FLOAT_EQ( time.GetDeltaSeconds(), WorldTime::kMaxUndilatedFrameSeconds );
    EXPECT_NEAR( time.GetRealTimeSeconds(), 5.0, 1e-6 );
}

TEST( WorldTime, FixedStepIgnoresTheMeasuredStepSoTwoRunsAreIdentical )
{
    // Two "captures": the same number of frames, wildly different wall-clock steps.
    const std::array<float, 6> runA{ 0.001f, 0.2f, 0.016f, 0.05f, 0.3f, 0.0f };
    const std::array<float, 6> runB{ 0.033f, 0.004f, 0.1f, 0.016f, 0.0f, 0.25f };

    WorldTime a;
    WorldTime b;
    a.SetFixedStep( kStep );
    b.SetFixedStep( kStep );
    for ( size_t i = 0; i < runA.size(); ++i )
    {
        a.Tick( runA[i], WorldClockMode::Preview );
        b.Tick( runB[i], WorldClockMode::Preview );
        EXPECT_EQ( a.GetGameTimeSeconds(), b.GetGameTimeSeconds() ) << "frame " << i;
        EXPECT_EQ( a.GetDeltaSeconds(), b.GetDeltaSeconds() ) << "frame " << i;
    }
    EXPECT_NEAR( a.GetGameTimeSeconds(), 6.0 * kStep, 1e-6 );
    // Real time stays honest under a fixed step.
    EXPECT_NE( a.GetRealTimeSeconds(), b.GetRealTimeSeconds() );
}

TEST( WorldTime, ResetStartsTheWorldFromZero )
{
    WorldTime time;
    time.Tick( 0.1f, WorldClockMode::Game );
    time.SetPaused( true );
    time.Reset();
    EXPECT_EQ( time.GetGameTimeSeconds(), 0.0 );
    EXPECT_EQ( time.GetRealTimeSeconds(), 0.0 );
    EXPECT_FALSE( time.IsPaused() );
}

// ---- The editor's rule: preview time moves only while the viewport is Realtime -------------------------

TEST( WorldTime, ClockModeFollowsSceneStateAndRealtime )
{
    // playing, pausedState, previewRealtime
    EXPECT_EQ( WorldTime::ClockModeFor( true, false, false ), WorldClockMode::Game );
    EXPECT_EQ( WorldTime::ClockModeFor( true, false, true ), WorldClockMode::Game );
    EXPECT_EQ( WorldTime::ClockModeFor( false, true, true ), WorldClockMode::Frozen );
    EXPECT_EQ( WorldTime::ClockModeFor( false, false, true ), WorldClockMode::Preview );
    EXPECT_EQ( WorldTime::ClockModeFor( false, false, false ), WorldClockMode::Frozen );
}

TEST( WorldTime, PreviewAdvancesOnlyWhileRealtime )
{
    WorldTime time;
    time.Tick( 0.1f, WorldTime::ClockModeFor( false, false, /*previewRealtime*/ false ) );
    EXPECT_EQ( time.GetGameTimeSeconds(), 0.0 );
    EXPECT_EQ( time.GetDeltaSeconds(), 0.0f );

    time.Tick( 0.1f, WorldTime::ClockModeFor( false, false, /*previewRealtime*/ true ) );
    EXPECT_NEAR( time.GetGameTimeSeconds(), 0.1, 1e-7 );
    EXPECT_FLOAT_EQ( time.GetDeltaSeconds(), 0.1f );
}

// ---- One source of time: every consumer reads the world's clock ----------------------------------------

namespace
{
    struct Consumer
    {
        const char* File;
        const char* MustRead; // the hookup to the world's clock
    };

    // Every consumer that used to keep its own clock (TIME1), and the line that now connects it to the
    // world's. One named row each — a consumer that goes back to its own clock fails here by name.
    constexpr std::array<Consumer, 9> kConsumers{ {
         { "Desert/Desert/Source/Engine/Core/Scene.cpp", "Common::Timestep( m_WorldTime.GetDeltaSeconds() )" },
         { "Desert/Desert/Source/Engine/Core/Scene.cpp", "system->SetWorldTime( m_WorldTime )" },
         { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp",
           "const Common::Timestep animTs( m_WorldDeltaSeconds )" },
         { "Desert/Desert/Source/Engine/ECS/System/VolumetricCloudECSSystem.hpp",
           "AdvanceWind( data, m_WorldDeltaSeconds )" },
         { "Desert/Desert/Source/Engine/ECS/System/VolumetricCloudECSSystem.hpp",
           "m_WorldDeltaSeconds = time.GetDeltaSeconds()" },
         { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.cpp",
           "SceneTimeBind( material, TimeSeconds )" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
           "frame.TimeSeconds = m_WorldTimeSeconds" },
         { "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp", "scene.GetWorldTime().GetGameTimeSeconds()" },
         { "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp",
           "->SetDeltaSeconds( scene.GetWorldTime().GetDeltaSeconds() )" },
    } };

    // Files under the scanned directories that may read a wall clock, because what they measure is the
    // MACHINE (how long a bake or a load took, for a log line), never something drawn. One row each.
    const std::set<std::string> kWallClockAllowed{
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.hpp",
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererShadow.cpp", // shadow alloc ms
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",   // bake ms
    };

    constexpr std::array<const char*, 3> kScannedDirs{
         "Desert/Desert/Source/Engine/Graphic/Systems",
         "Desert/Desert/Source/Engine/Graphic/Materials",
         "Desert/Desert/Source/Engine/ECS/System",
    };
} // namespace

TEST( WorldTimeOneSource, EveryConsumerReadsTheWorldsClock )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";
    for ( const auto& consumer : kConsumers )
    {
        const std::string text = ReadAll( root + consumer.File );
        ASSERT_FALSE( text.empty() ) << consumer.File << " is missing";
        EXPECT_NE( text.find( consumer.MustRead ), std::string::npos )
             << consumer.File << " no longer reads the world's clock through '" << consumer.MustRead << "'";
    }
}

TEST( WorldTimeOneSource, NoRenderOrSimulationCodeKeepsItsOwnClock )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    // A private clock is a wall clock read, or a hard-coded frame step standing in for one.
    const std::array<const char*, 4> kOwnClock{ "steady_clock", "high_resolution_clock", "glfwGetTime",
                                                "kDeltaTime" };
    std::vector<std::string>         offenders;
    size_t                           scanned = 0;
    for ( const char* dir : kScannedDirs )
    {
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root + dir ) )
        {
            const auto ext = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" ) )
                continue;
            ++scanned;
            const std::string relative = std::filesystem::relative( entry.path(), root ).generic_string();
            if ( kWallClockAllowed.contains( relative ) )
                continue;
            const std::string text = ReadAll( entry.path() );
            for ( const char* needle : kOwnClock )
                if ( text.find( needle ) != std::string::npos )
                    offenders.push_back( std::format( "{} uses {}", relative, needle ) );
        }
    }
    EXPECT_GT( scanned, 50u ) << "the scan reached too few files to mean anything";
    std::string list;
    for ( const auto& o : offenders )
        std::format_to( std::back_inserter( list ), "\n  {}", o );
    EXPECT_TRUE( offenders.empty() ) << "render/simulation code keeping its own clock instead of "
                                        "Core::WorldTime:"
                                     << list;
}

TEST( WorldTimeOneSource, TheWallClockAllowListHasNoDeadRows )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    for ( const auto& file : kWallClockAllowed )
    {
        const std::string text = ReadAll( root + file );
        EXPECT_NE( text.find( "steady_clock" ), std::string::npos )
             << file << " no longer reads a wall clock; remove its row";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
