// The flight instrument's decisions (Editor/Core/FlightRules.hpp), without Vulkan: the route and the pose
// along it, how many frames a flight takes, which frame a timing belongs to, the percentiles, the worst frame
// and its cause, the CSV — and the command line that arms it.

#include <gtest/gtest.h>

#include <Editor/Core/CommandLine.hpp>
#include <Editor/Core/FlightRules.hpp>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Editor;

namespace
{
    constexpr double kStep = 1.0 / 60.0;

    Flight::FrameRow Timed( int frame, double cpuMs, Flight::Phase kind = Flight::Phase::Flight )
    {
        Flight::FrameRow row;
        row.Frame = frame;
        row.Kind  = kind;
        row.CpuMs = cpuMs;
        return row;
    }

    std::size_t Fields( const std::string& line )
    {
        std::size_t commas = 0;
        bool        quoted = false;
        for ( const char c : line )
        {
            quoted ^= c == '"';
            commas += c == ',' && !quoted ? 1 : 0;
        }
        return commas + 1;
    }

    ::Common::ResultStr<CommandLineOptions> Parse( const std::string& line )
    {
        std::istringstream       words( line );
        std::vector<std::string> args;
        for ( std::string word; words >> word; )
            args.push_back( word );
        return ParseCommandLine( args );
    }
} // namespace

// ── Routes ─────────────────────────────────────────────────────────────────────────────────────────────

TEST( FlightRules, ALineIsItsTwoPointsAndACircleIsOneLapOfItsRadius )
{
    const auto line = Flight::ParseRouteSpec( "line:0,200,0:30000,200,40000" );
    ASSERT_TRUE( line.IsSuccess() ) << line.GetError();
    EXPECT_FALSE( line.GetValue().Closed );
    EXPECT_DOUBLE_EQ( Flight::RouteLength( line.GetValue() ), 50000.0 );

    const auto circle = Flight::ParseRouteSpec( "circle:1000,300,-500:25600" );
    ASSERT_TRUE( circle.IsSuccess() ) << circle.GetError();
    EXPECT_TRUE( circle.GetValue().Closed );
    const double lap = 2.0 * 3.14159265358979323846 * 25600.0;
    EXPECT_NEAR( Flight::RouteLength( circle.GetValue() ), lap, lap * 1e-5 );
    // Starts at +X of the centre, at the centre's height, and turns towards +Z.
    const auto start = Flight::PoseAt( circle.GetValue(), 0.0 );
    EXPECT_NEAR( start.Position.x, 1000.0f + 25600.0f, 0.01f );
    EXPECT_FLOAT_EQ( start.Position.y, 300.0f );
    EXPECT_GT( start.Forward.z, 0.99f );
}

TEST( FlightRules, AMalformedRouteIsRefusedWithItsText )
{
    for ( const char* bad : { "line:0,0,0", "line:0,0,0:1,1", "line:0,0,0:0,0,0", "circle:0,0,0:-5",
                              "circle:0,0,0", "spiral:0,0,0:1", "file:", "0,0,0" } )
    {
        const auto route = Flight::ParseRouteSpec( bad );
        ASSERT_FALSE( route.IsSuccess() ) << bad;
        EXPECT_NE( route.GetError().find( bad ), std::string::npos ) << route.GetError();
    }
}

TEST( FlightRules, AFileRouteSkipsCommentsClosesOnARepeatedStartAndNamesABadLine )
{
    const auto spec = Flight::ParseRouteSpec( "file:route.txt" );
    ASSERT_TRUE( spec.IsSuccess() );
    EXPECT_EQ( spec.GetValue().FilePath, "route.txt" );
    EXPECT_TRUE( spec.GetValue().Points.empty() );

    const auto open = Flight::ParseRouteFile( spec.GetValue(), "# a corner\n0,0,0\n\n100,0,0\r\n100,0,100\n" );
    ASSERT_TRUE( open.IsSuccess() ) << open.GetError();
    EXPECT_FALSE( open.GetValue().Closed );
    EXPECT_DOUBLE_EQ( Flight::RouteLength( open.GetValue() ), 200.0 );

    const auto closed = Flight::ParseRouteFile( spec.GetValue(), "0,0,0\n100,0,0\n100,0,100\n0,0,0\n" );
    ASSERT_TRUE( closed.IsSuccess() ) << closed.GetError();
    EXPECT_TRUE( closed.GetValue().Closed );
    EXPECT_EQ( closed.GetValue().Points.size(), 3u );
    EXPECT_NEAR( Flight::RouteLength( closed.GetValue() ), 200.0 + std::sqrt( 2.0 ) * 100.0, 1e-9 );

    const auto bad = Flight::ParseRouteFile( spec.GetValue(), "0,0,0\n# ok\n1,2\n" );
    ASSERT_FALSE( bad.IsSuccess() );
    EXPECT_NE( bad.GetError().find( "line 3" ), std::string::npos ) << bad.GetError();
}

TEST( FlightRules, ThePoseWalksTheSegmentsAndIsClampedToTheEnds )
{
    const auto spec   = Flight::ParseRouteSpec( "file:r" );
    const auto parsed = Flight::ParseRouteFile( spec.GetValue(), "0,0,0\n100,0,0\n100,0,50\n" );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const Flight::Route& route  = parsed.GetValue();
    const auto           before = Flight::PoseAt( route, -10.0 );
    EXPECT_EQ( before.Position, glm::vec3( 0.0f ) );
    const auto first = Flight::PoseAt( route, 40.0 );
    EXPECT_EQ( first.Position, glm::vec3( 40.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( first.Forward, glm::vec3( 1.0f, 0.0f, 0.0f ) );
    const auto second = Flight::PoseAt( route, 125.0 );
    EXPECT_EQ( second.Position, glm::vec3( 100.0f, 0.0f, 25.0f ) );
    EXPECT_EQ( second.Forward, glm::vec3( 0.0f, 0.0f, 1.0f ) );
    const auto after = Flight::PoseAt( route, 1e6 );
    EXPECT_EQ( after.Position, glm::vec3( 100.0f, 0.0f, 50.0f ) );
}

// ── Frames ─────────────────────────────────────────────────────────────────────────────────────────────

// 10 m at 10 m/s is one second = 60 moving steps; the frame after the warm-up that has gone 60 steps is on
// the end, and one more frame exists only so that one can be timed.
TEST( FlightRules, AFlightIsTheWarmUpThenEveryStepToTheEndThenOneFrameToTimeTheLast )
{
    EXPECT_EQ( Flight::FlightFrames( 1000.0, 1000.0, kStep ), Flight::kWarmupFrames + 60 + 2 );
    EXPECT_DOUBLE_EQ( Flight::DistanceAt( 0, 1000.0, kStep ), 0.0 );
    EXPECT_DOUBLE_EQ( Flight::DistanceAt( Flight::kWarmupFrames - 1, 1000.0, kStep ), 0.0 );
    EXPECT_NEAR( Flight::DistanceAt( Flight::kWarmupFrames + 60, 1000.0, kStep ), 1000.0, 1e-9 );
    // A length that is not a whole number of steps rounds UP: the flight reaches the end, not short of it.
    EXPECT_EQ( Flight::FlightFrames( 1001.0, 1000.0, kStep ), Flight::kWarmupFrames + 61 + 2 );
}

// THE ALIGNMENT: numbers read on frame k belong to the row appended on frame k-1.
TEST( FlightRules, ATimingGoesToThePreviousFramesRowAndTheLastRowStaysUntimed )
{
    Flight::FlightLog log;
    log.TimeLast( 99.0, 1.0, 1.0 ); // the first frame: nothing before it to time
    Flight::FrameRow a;
    a.Frame = 0;
    log.Append( a );
    log.TimeLast( 10.0, 4.0, 0.5 );
    Flight::FrameRow b;
    b.Frame = 1;
    log.Append( b );
    log.TimeLast( 20.0, Flight::kNotMeasured, 0.0 );
    Flight::FrameRow c;
    c.Frame = 2;
    log.Append( c );

    const auto rows = log.Rows();
    ASSERT_EQ( rows.size(), 3u );
    EXPECT_DOUBLE_EQ( rows[0].CpuMs, 10.0 );
    EXPECT_DOUBLE_EQ( rows[0].GpuMs, 4.0 );
    EXPECT_DOUBLE_EQ( rows[0].StreamMs, 0.5 );
    EXPECT_DOUBLE_EQ( rows[1].CpuMs, 20.0 );
    EXPECT_TRUE( std::isnan( rows[1].GpuMs ) );
    EXPECT_TRUE( std::isnan( rows[2].CpuMs ) );

    // A second timing on the same frame does not overwrite the first.
    Flight::FlightLog twice;
    twice.Append( a );
    twice.TimeLast( 5.0, 1.0, 0.0 );
    twice.TimeLast( 7.0, 1.0, 0.0 );
    EXPECT_DOUBLE_EQ( twice.Rows()[0].CpuMs, 5.0 );
}

// ── Statistics ─────────────────────────────────────────────────────────────────────────────────────────

TEST( FlightRules, PercentilesAreNearestRankSoEveryOneIsAFrameThatHappened )
{
    std::vector<double> hundred;
    for ( int i = 1; i <= 100; ++i )
        hundred.push_back( i );
    EXPECT_DOUBLE_EQ( Flight::Percentile( hundred, 50.0 ), 50.0 );
    EXPECT_DOUBLE_EQ( Flight::Percentile( hundred, 95.0 ), 95.0 );
    EXPECT_DOUBLE_EQ( Flight::Percentile( hundred, 99.0 ), 99.0 );
    EXPECT_DOUBLE_EQ( Flight::Percentile( hundred, 100.0 ), 100.0 );

    const std::vector<double> ten{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    EXPECT_DOUBLE_EQ( Flight::Percentile( ten, 50.0 ), 5.0 );
    EXPECT_DOUBLE_EQ( Flight::Percentile( ten, 95.0 ), 10.0 ); // rank ceil(9.5) = 10
    EXPECT_DOUBLE_EQ( Flight::Percentile( ten, 0.0 ), 1.0 );

    const std::vector<double> one{ 7.5 };
    EXPECT_DOUBLE_EQ( Flight::Percentile( one, 99.0 ), 7.5 );
}

TEST( FlightRules, TheSummaryLeavesOutWarmUpAndUntimedRowsAndNamesTheWorstFramesCells )
{
    std::vector<Flight::FrameRow> rows;
    rows.push_back( Timed( 0, 500.0, Flight::Phase::Warmup ) ); // shader compilation: not a statement
    for ( int i = 1; i <= 98; ++i )
        rows.push_back( Timed( i, 5.0 ) );
    Flight::FrameRow hitch = Timed( 99, 31.0 );
    hitch.UnitsActivated   = 2;
    hitch.RecordsActivated = 18;
    hitch.ActivationMs     = 24.5;
    hitch.ActivatedUnits   = "L1(3,-2) L1(4,-2)";
    hitch.StreamMs         = 25.0;
    hitch.Entities         = 900;
    rows.push_back( hitch );
    Flight::FrameRow settling = Timed( 99, 12.0, Flight::Phase::Settling );
    settling.Entities         = 950;
    rows.push_back( settling );
    rows.push_back( Timed( 100, Flight::kNotMeasured ) ); // the run ended before it was timed

    const auto summary = Flight::Summarise( rows );
    ASSERT_TRUE( summary.IsSuccess() ) << summary.GetError();
    const Flight::Summary& s = summary.GetValue();
    EXPECT_EQ( s.Frames, 100u );
    EXPECT_EQ( s.SettlingFrames, 1u );
    EXPECT_EQ( s.UntimedFrames, 1u );
    EXPECT_DOUBLE_EQ( s.P50, 5.0 );
    EXPECT_DOUBLE_EQ( s.P99, 12.0 );
    EXPECT_DOUBLE_EQ( s.Max, 31.0 );
    EXPECT_EQ( s.Worst, 99u );
    EXPECT_EQ( s.FramesWithActivation, 1u );
    EXPECT_EQ( s.MaxActivationFrame, 99u );
    EXPECT_DOUBLE_EQ( s.P50WithActivation, 31.0 );
    EXPECT_DOUBLE_EQ( s.P50WithoutActivation, 5.0 );
    EXPECT_EQ( s.MaxEntities, 950u );
    EXPECT_FALSE( s.HasGpu ); // no row measured it

    const std::string text = Flight::Describe( s, rows );
    EXPECT_NE( text.find( "Worst frame 99" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "[L1(3,-2) L1(4,-2)]" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "24.50 ms" ), std::string::npos ) << text;
}

TEST( FlightRules, GpuIsSummarisedOnlyWhenEveryTimedFrameMeasuredIt )
{
    std::vector<Flight::FrameRow> rows{ Timed( 0, 5.0 ), Timed( 1, 6.0 ) };
    rows[0].GpuMs  = 3.0;
    rows[1].GpuMs  = 4.0;
    const auto all = Flight::Summarise( rows );
    ASSERT_TRUE( all.IsSuccess() );
    EXPECT_TRUE( all.GetValue().HasGpu );
    EXPECT_DOUBLE_EQ( all.GetValue().GpuMax, 4.0 );

    rows[1].GpuMs   = Flight::kNotMeasured;
    const auto some = Flight::Summarise( rows );
    ASSERT_TRUE( some.IsSuccess() );
    EXPECT_FALSE( some.GetValue().HasGpu );
}

TEST( FlightRules, ASummaryOfNoTimedFlightFrameIsRefused )
{
    const std::vector<Flight::FrameRow> rows{ Timed( 0, 9.0, Flight::Phase::Warmup ),
                                              Timed( 1, Flight::kNotMeasured ) };
    EXPECT_FALSE( Flight::Summarise( rows ).IsSuccess() );
    EXPECT_FALSE( Flight::Summarise( {} ).IsSuccess() );
}

TEST( FlightRules, TheCsvHasOneColumnPerHeaderFieldAndAnUnmeasuredValueIsEmptyNotZero )
{
    Flight::FrameRow row     = Timed( 7, 16.25 );
    row.Position             = glm::vec3( 100.0f, 200.0f, -300.0f );
    row.ActivatedUnits       = "L0(1,2)";
    row.UnitsActivated       = 1;
    const std::string header = Flight::CsvHeader();
    const std::string line   = Flight::CsvRow( row );
    EXPECT_EQ( Fields( header ), Fields( line ) ) << header << line;
    EXPECT_EQ( line.rfind( "7,flight,", 0 ), 0u ) << line;
    // cpu_ms measured; gpu_ms and stream_ms never were, and a 0 there would read as a free frame.
    EXPECT_NE( line.find( ",16.250,,," ), std::string::npos ) << line;
    EXPECT_NE( line.find( ",\"L0(1,2)\"\n" ), std::string::npos ) << line;
}

// ── The command line ───────────────────────────────────────────────────────────────────────────────────

TEST( FlightRules, TheCommandLineArmsALineFlightAndSetsTheFrameCount )
{
    const auto parsed = Parse( "--play --flight line:0,200,0:1000,200,0 --flight-speed 1000 --flight-csv f.csv" );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const ShotOptions& shot = parsed.GetValue().Shot;
    EXPECT_TRUE( shot.Active() );
    EXPECT_TRUE( shot.HasCamera );
    EXPECT_EQ( shot.Frames, Flight::kWarmupFrames + 60 + 2 );

    // A file route is armed by the process once it has read the file: the parser cannot count its frames.
    const auto file = Parse( "--play --flight file:r.txt --flight-speed 1000 --flight-csv f.csv" );
    ASSERT_TRUE( file.IsSuccess() ) << file.GetError();
    EXPECT_FALSE( file.GetValue().Shot.HasCamera );
}

TEST( FlightRules, AFlightMissingAPartOrFightingAnotherFlagIsRefused )
{
    const std::string flight = " --flight line:0,200,0:1000,200,0";
    for ( const std::string& bad :
          { std::string( "--flight line:0,200,0:1000,200,0 --flight-speed 1000 --flight-csv f.csv" ), // no --play
            "--play" + flight + " --flight-csv f.csv",                                                // no speed
            "--play" + flight + " --flight-speed 1000",                                               // no CSV
            "--play" + flight + " --flight-speed 0 --flight-csv f.csv",
            "--play" + flight + " --flight-speed 1000 --flight-csv f.csv --camera 0,0,0",
            "--play" + flight + " --flight-speed 1000 --flight-csv f.csv --look-to 0,0,-1",
            "--play" + flight + " --flight-speed 1000 --flight-csv f.csv --shot-frames 90",
            std::string( "--play --flight-speed 1000" ), std::string( "--flight-csv f.csv" ) } )
    {
        EXPECT_FALSE( Parse( bad ).IsSuccess() ) << bad;
    }
}
int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
