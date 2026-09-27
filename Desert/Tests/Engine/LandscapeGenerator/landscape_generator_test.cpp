// LS-10: UE's New Landscape with a seeded fill and whole-map erosion. The generator's promise is that one seed
// and one settings block give one heightmap byte for byte, cut into tiles that agree on every shared edge, in a
// frame centred where UE centres it.

#include <Engine/World/Landscape/LandscapeGenerator.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
    using namespace Desert::World::Landscape;

    LandscapeGenerateSettings Eroded( uint32_t seed )
    {
        LandscapeGenerateSettings s;
        s.TilesX       = 3;
        s.TilesZ       = 2;
        s.QuadsPerTile = 31u;
        s.Fill         = LandscapeGenerateFill::Noise;
        s.Seed         = seed;
        s.NoiseScale   = 64.0f;
        s.Erosion      = true;
        s.HydroErosion = true;
        return s;
    }

    std::vector<uint16_t> Map( const LandscapeGenerateSettings& s )
    {
        auto made = GenerateLandscapeMap( s );
        EXPECT_TRUE( made.IsSuccess() ) << made.GetError();
        return made.IsSuccess() ? made.GetValue().Samples : std::vector<uint16_t>{};
    }

    int32_t MaxStep( const std::vector<uint16_t>& h, uint32_t w )
    {
        int32_t steepest = 0;
        for ( size_t i = 0; i + 1 < h.size(); ++i )
            if ( ( i + 1 ) % w != 0 )
                steepest = std::max( steepest, std::abs( static_cast<int32_t>( h[i + 1] ) - h[i] ) );
        return steepest;
    }

    /// Sum over X-neighbour pairs of the step above @p threshold: what UE's thermal loop exists to shed.
    int64_t ExcessSlope( const std::vector<uint16_t>& h, uint32_t w, int32_t threshold )
    {
        int64_t excess = 0;
        for ( size_t i = 0; i + 1 < h.size(); ++i )
            if ( ( i + 1 ) % w != 0 )
                excess += std::max( 0, std::abs( static_cast<int32_t>( h[i + 1] ) - h[i] ) - threshold );
        return excess;
    }
} // namespace

// The same seed twice gives the same bytes, through the fill AND both erosion passes; so do the encoded tiles,
// which are what the scene saves.
TEST( LandscapeGenerator, OneSeedIsOneHeightmapByteForByte )
{
    const auto a = Map( Eroded( 7u ) );
    const auto b = Map( Eroded( 7u ) );
    ASSERT_FALSE( a.empty() );
    EXPECT_EQ( a, b );

    auto ta = GenerateLandscape( Eroded( 7u ) );
    auto tb = GenerateLandscape( Eroded( 7u ) );
    ASSERT_TRUE( ta.IsSuccess() && tb.IsSuccess() );
    ASSERT_EQ( ta.GetValue().Tiles.size(), 6u );
    for ( size_t i = 0; i < ta.GetValue().Tiles.size(); ++i )
        EXPECT_EQ( EncodeLandscapeTile( ta.GetValue().Tiles[i].Heights ),
                   EncodeLandscapeTile( tb.GetValue().Tiles[i].Heights ) )
             << "tile " << i;
}

// A different seed is a different map — the seed is not ignored by the fill, nor by the rain.
TEST( LandscapeGenerator, AnotherSeedIsAnotherMap )
{
    EXPECT_NE( Map( Eroded( 7u ) ), Map( Eroded( 8u ) ) );

    // And the hydraulic pass runs at all over the whole map (its rain is laid at the seed's offset).
    auto dry         = Eroded( 7u );
    dry.HydroErosion = false;
    EXPECT_NE( Map( dry ), Map( Eroded( 7u ) ) ) << "the hydraulic pass changed nothing";
}

// UE's defaults and centring: 8 x 8 components of 63 quads, scale 100, Location the landscape's centre.
TEST( LandscapeGenerator, FrameIsUEsNewLandscape )
{
    LandscapeGenerateSettings s;
    EXPECT_EQ( s.TilesX, 8 );
    EXPECT_EQ( s.TilesZ, 8 );
    EXPECT_EQ( s.QuadsPerTile, 63u );
    EXPECT_EQ( s.SpacingCm, 100.0f );
    EXPECT_EQ( s.ZScale, 100.0f );
    s.LocationCm = glm::vec3( 1000.0f, 50.0f, -2000.0f );

    auto made = GenerateLandscape( s );
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();
    const auto& g = made.GetValue();
    EXPECT_FLOAT_EQ( g.Root.Origin.x, 1000.0f - 8.0f * 63.0f * 100.0f / 2.0f );
    EXPECT_FLOAT_EQ( g.Root.Origin.y, 50.0f );
    EXPECT_FLOAT_EQ( g.Root.Origin.z, -2000.0f - 8.0f * 63.0f * 100.0f / 2.0f );
    ASSERT_EQ( g.Tiles.size(), 64u );
    for ( const auto& tile : g.Tiles )
    {
        EXPECT_TRUE( CheckTileMatchesRoot( tile.Heights, g.Root ).IsSuccess() );
        for ( uint16_t v : tile.Heights.Samples() )
            ASSERT_EQ( v, kLandscapeMidSample ) << "a flat New Landscape is UE's mid value";
    }
    EXPECT_EQ( g.Tiles.back().TileX, 7 );
    EXPECT_EQ( g.Tiles.back().TileZ, 7 );
}

// Tiles are cut from one map: every shared edge row and column is the same on both sides.
TEST( LandscapeGenerator, NeighbouringTilesAgreeOnTheirSharedEdge )
{
    auto made = GenerateLandscape( Eroded( 3u ) );
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();
    const auto&    tiles = made.GetValue().Tiles;
    const uint32_t n     = 32u;
    auto at = [&]( int32_t x, int32_t z ) -> const LandscapeTileData& { return tiles[z * 3 + x].Heights; };
    for ( int32_t z = 0; z < 2; ++z )
        for ( int32_t x = 0; x + 1 < 3; ++x )
            for ( uint32_t i = 0; i < n; ++i )
                ASSERT_EQ( at( x, z ).Sample( n - 1, i ), at( x + 1, z ).Sample( 0, i ) ) << x << "," << z;
    for ( int32_t x = 0; x < 3; ++x )
        for ( uint32_t i = 0; i < n; ++i )
            ASSERT_EQ( at( x, 0 ).Sample( i, n - 1 ), at( x, 1 ).Sample( i, 0 ) ) << x;

    // The map is not flat, or the edge check proves nothing.
    EXPECT_GT( MaxStep( at( 0, 0 ).Samples(), n ), 0 );
}

// The thermal pass does what UE's Erosion does: it sheds the part of every step above its threshold. (The maximum
// step is no measure: UE's shed hands the excess to the lower neighbour, which can steepen the step beyond it.)
TEST( LandscapeGenerator, ThermalErosionFlattensTheSteepest )
{
    auto rough                       = Eroded( 11u );
    rough.NoiseScale                 = 8.0f;    // steep: 4 samples per quarter period
    rough.NoiseHeightCm              = 1000.0f; // inside the 16-bit range: no clamped plateaus
    rough.Erosion                    = false;
    rough.HydroErosion               = false;
    auto eroded                      = rough;
    eroded.Erosion                   = true;
    eroded.ErosionSettings.NoiseMode = LandscapeErosionNoiseMode::Both;
    const uint32_t w                 = 3u * 31u + 1u;
    const auto     before            = Map( rough );
    const auto     after             = Map( eroded );
    const int32_t  t                 = eroded.ErosionSettings.Threshold;
    EXPECT_LT( ExcessSlope( after, w, t ) * 2, ExcessSlope( before, w, t ) ) << "not even half shed";
}

// Refusals name the field and the number; UE's size clamp is honoured.
TEST( LandscapeGenerator, RefusesWhatItCannotHonour )
{
    EXPECT_EQ( ClampLandscapeTileCount( 100, 255u ), 32 ); // 8191 / 255
    EXPECT_EQ( ClampLandscapeTileCount( 300, 7u ), 256 );
    EXPECT_EQ( ClampLandscapeTileCount( 0, 63u ), 1 );

    LandscapeGenerateSettings s;
    s.QuadsPerTile = 255u;
    s.TilesX       = 33;
    auto refused   = ValidateLandscapeGenerate( s );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "33 tiles along X" ), std::string::npos ) << refused.GetError();

    s              = {};
    s.QuadsPerTile = 64u;
    EXPECT_FALSE( GenerateLandscape( s ).IsSuccess() );

    s            = {};
    s.Fill       = LandscapeGenerateFill::Noise;
    s.NoiseScale = 600.0f;
    refused      = ValidateLandscapeGenerate( s );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "600" ), std::string::npos ) << refused.GetError();

    s                 = {};
    s.Erosion         = true;
    s.ErosionStrength = 1.5f;
    EXPECT_FALSE( ValidateLandscapeGenerate( s ).IsSuccess() );
}

// The time a typical map takes: UE's default New Landscape (8 x 8 x 63 quads, 505 x 505 samples) with the noise
// fill and both erosion passes at UE's defaults. Reported, and held under a generous bound so a regression that
// multiplies it shows.
TEST( LandscapeGenerator, TypicalMapTime )
{
    LandscapeGenerateSettings s;
    s.Fill          = LandscapeGenerateFill::Noise;
    s.Erosion       = true;
    s.HydroErosion  = true;
    const auto t0   = std::chrono::steady_clock::now();
    auto       made = GenerateLandscape( s );
    const auto ms   = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();
    std::cout << "[LandscapeGenerator] 505x505 noise + erosion (" << made.GetValue().ErosionIterations
              << " it) + hydro (" << made.GetValue().HydroErosionIterations << " it): " << ms << " ms\n";
    RecordProperty( "typical_map_ms", static_cast<int>( ms ) );
    EXPECT_LT( ms, 60000.0 );
}

// LS-10b: the run can be watched and cancelled, and watching changes nothing. The watched run is the unwatched
// run byte for byte, and its count of steps lands exactly on the total it announced.
TEST( LandscapeGenerator, WatchedRunIsTheSameMapAndCountsToItsTotal )
{
    LandscapeGenerateProgress progress;
    auto                      watched = GenerateLandscape( Eroded( 7u ), &progress );
    ASSERT_TRUE( watched.IsSuccess() ) << watched.GetError();
    auto plain = GenerateLandscape( Eroded( 7u ) );
    ASSERT_TRUE( plain.IsSuccess() ) << plain.GetError();
    ASSERT_EQ( watched.GetValue().Tiles.size(), plain.GetValue().Tiles.size() );
    for ( size_t i = 0; i < plain.GetValue().Tiles.size(); ++i )
        EXPECT_EQ( watched.GetValue().Tiles[i].Heights.Samples(), plain.GetValue().Tiles[i].Heights.Samples() )
             << "tile " << i;
    EXPECT_GT( progress.Total.load(), 0u );
    EXPECT_EQ( progress.Done.load(), progress.Total.load() );
    EXPECT_FLOAT_EQ( progress.Fraction(), 1.0f );
}

TEST( LandscapeGenerator, SetCancelRefusesAsCancelled )
{
    LandscapeGenerateProgress progress;
    progress.Cancel = true;
    auto made       = GenerateLandscape( Eroded( 3u ), &progress );
    ASSERT_FALSE( made.IsSuccess() );
    EXPECT_EQ( made.GetError(), kLandscapeGenerateCancelled );
    EXPECT_EQ( progress.Done.load(), 0u ) << "a cancelled run stops before its first step";
}

// The job: the run is on the JobSystem, a second Start while it runs is refused, a cancel mid-erosion hands over
// the cancel, and the next run hands over the very map the synchronous call makes.
TEST( LandscapeGenerator, JobRefusesASecondStartAndCancelsMidErosion )
{
    // UE's default New Landscape with both passes: seconds, so the cancel lands while it is inside the erosion.
    LandscapeGenerateSettings big;
    big.Fill                = LandscapeGenerateFill::Noise;
    big.Erosion             = true;
    big.HydroErosion        = true;
    const uint32_t fillRows = static_cast<uint32_t>( big.TilesZ ) * big.QuadsPerTile + 1u;

    LandscapeGenerateJob job;
    ASSERT_TRUE( job.Start( big ).IsSuccess() );
    EXPECT_TRUE( job.Running() );
    auto second = job.Start( Eroded( 1u ) );
    ASSERT_FALSE( second.IsSuccess() );
    EXPECT_NE( second.GetError().find( "generation in progress" ), std::string::npos ) << second.GetError();

    // Past the fill (every row counted), so inside the erosion loops; then cancel.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 60 );
    while ( job.StepsDone() <= fillRows )
    {
        ASSERT_LT( std::chrono::steady_clock::now(), deadline ) << "the run never got past its fill";
        std::this_thread::sleep_for( std::chrono::microseconds( 200 ) );
    }
    job.Cancel();
    job.Wait();
    auto finished = job.TakeFinished();
    ASSERT_TRUE( finished.has_value() );
    ASSERT_FALSE( finished->IsSuccess() ) << "the run finished before the cancel reached it";
    EXPECT_EQ( finished->GetError(), kLandscapeGenerateCancelled );
    EXPECT_FALSE( job.Running() );
    EXPECT_FALSE( job.TakeFinished().has_value() ) << "handed over once";

    ASSERT_TRUE( job.Start( Eroded( 5u ) ).IsSuccess() );
    job.Wait();
    auto again = job.TakeFinished();
    ASSERT_TRUE( again.has_value() && again->IsSuccess() );
    auto plain = GenerateLandscape( Eroded( 5u ) );
    ASSERT_TRUE( plain.IsSuccess() );
    ASSERT_EQ( again->GetValue().Tiles.size(), plain.GetValue().Tiles.size() );
    for ( size_t i = 0; i < plain.GetValue().Tiles.size(); ++i )
        EXPECT_EQ( again->GetValue().Tiles[i].Heights.Samples(), plain.GetValue().Tiles[i].Heights.Samples() );
}

TEST( LandscapeGenerator, JobRefusesInvalidSettingsAtOnce )
{
    LandscapeGenerateJob      job;
    LandscapeGenerateSettings s;
    s.SpacingCm  = 0.0f;
    auto started = job.Start( s );
    EXPECT_FALSE( started.IsSuccess() );
    EXPECT_FALSE( job.Running() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
