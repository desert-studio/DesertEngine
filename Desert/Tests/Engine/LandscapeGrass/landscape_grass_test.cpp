// GR-1: landscape grass (UE ULandscapeGrassType) — generated around the camera, never stored.
//
// The generator is pure (World/Landscape/LandscapeGrass), so every property the frame relies on is held here
// on the same inputs the ECS system gives it: one cell coordinate grows one set of instances, the count
// follows the painted weight (zero grows nothing), the streamer keeps exactly the cells around the camera and
// never generates more than its budget in one tick. The two formats it reads are held beside it.

#include <gtest/gtest.h>

#include <Engine/Assets/Serialization/LandscapeGrassType.hpp>
#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeGrass.hpp>

#include <Common/Core/Math/Pcg32.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace Desert::World::Landscape;
using namespace Desert::Assets::Serialization;

namespace
{
    constexpr const char* kMeshGuid  = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
    constexpr const char* kGrassGuid = "11112222333344445555666677778888";

    GrassVariety Variety( float density = 40.0f )
    {
        GrassVariety v;
        v.GrassMesh         = { kMeshGuid, "Assets/Meshes/Blade.stmesh" };
        v.GrassDensity      = density;
        v.StartCullDistance = 4000.0f;
        v.EndCullDistance   = 5000.0f;
        v.ScaleX            = { 0.8f, 1.2f };
        return v;
    }

    GrassSurfaceSampler Uniform( float weight )
    {
        return [weight]( float x, float z ) -> std::optional<GrassSurfaceSample> {
            return GrassSurfaceSample{ 0.01f * x - 0.02f * z, glm::normalize( glm::vec3( 0.1f, 1.0f, 0.05f ) ),
                                       weight };
        };
    }

    // FNV-1a over the matrices' bytes: "the same set" is the same bits, not the same count.
    uint64_t Hash( const std::vector<glm::mat4>& instances )
    {
        uint64_t h = 1469598103934665603ull;
        for ( const glm::mat4& m : instances )
        {
            unsigned char bytes[sizeof( glm::mat4 )];
            std::memcpy( bytes, &m, sizeof( bytes ) );
            for ( const unsigned char b : bytes )
                h = ( h ^ b ) * 1099511628211ull;
        }
        return h;
    }
} // namespace

// ── The generator ──────────────────────────────────────────────────────────────────────────────────────────

TEST( LandscapeGrass, OneCellCoordinateGrowsOneSetOfInstances )
{
    const GrassVariety   v = Variety();
    const GrassCellCoord cell{ 3, -7 };
    const auto           a = GenerateGrassCell( v, cell, 42u, Uniform( 1.0f ) );
    const auto           b = GenerateGrassCell( v, cell, 42u, Uniform( 1.0f ) );
    ASSERT_FALSE( a.empty() );
    EXPECT_EQ( a.size(), b.size() );
    EXPECT_EQ( Hash( a ), Hash( b ) );

    // A neighbour cell, or the same cell under another layer / variety, is another set.
    EXPECT_NE( Hash( a ), Hash( GenerateGrassCell( v, { 4, -7 }, 42u, Uniform( 1.0f ) ) ) );
    EXPECT_NE( Hash( a ), Hash( GenerateGrassCell( v, cell, 43u, Uniform( 1.0f ) ) ) );
}

TEST( LandscapeGrass, TheSeedIsPinnedSoEveryMachineGrowsTheSameGrass )
{
    // Integer-only hash and PCG32: these numbers are the same on clang and MSVC (computed independently, outside
    // the engine). A change to either is a change of every landscape's grass and must be made on purpose. The
    // INTEGERS are pinned, not a hash of the matrices: float maths built from them may contract into FMA
    // differently per toolset.
    EXPECT_EQ( GrassCellSeed( { 0, 0 }, 0u ), 0x642F30E0u );
    EXPECT_EQ( GrassCellSeed( { 1, 0 }, 0u ), 0xF81E1F7Du );
    EXPECT_EQ( GrassCellSeed( { 0, 1 }, 0u ), 0x9BC9A430u );
    EXPECT_EQ( GrassCellSeed( { -3, 5 }, 7u ), 0xF5E4EF4Du );
    Common::Math::Pcg32 random( 42u );
    EXPECT_EQ( random.NextU32(), 0xC2F57BD6u );
    EXPECT_EQ( random.NextU32(), 0x6B07C4A9u );
    EXPECT_EQ( random.NextU32(), 0x72B7B29Bu );
    EXPECT_FLOAT_EQ( GrassHalton( 1u, 2u ), 0.5f );
    EXPECT_FLOAT_EQ( GrassHalton( 2u, 3u ), 2.0f / 3.0f );
    EXPECT_FLOAT_EQ( GrassHalton( 5u, 2u ), 0.625f );
}

TEST( LandscapeGrass, InstancesStayInsideTheirCellAndOnTheSurface )
{
    const GrassCellCoord cell{ -2, 5 };
    const auto           instances = GenerateGrassCell( Variety(), cell, 7u, Uniform( 1.0f ) );
    ASSERT_FALSE( instances.empty() );
    for ( const glm::mat4& m : instances )
    {
        const glm::vec3 p( m[3] );
        EXPECT_GE( p.x, cell.X * kLandscapeGrassCellCm );
        EXPECT_LT( p.x, ( cell.X + 1 ) * kLandscapeGrassCellCm );
        EXPECT_GE( p.z, cell.Z * kLandscapeGrassCellCm );
        EXPECT_LT( p.z, ( cell.Z + 1 ) * kLandscapeGrassCellCm );
        EXPECT_NEAR( p.y, 0.01f * p.x - 0.02f * p.z, 1.0e-2f );
    }
}

TEST( LandscapeGrass, WeightZeroGrowsNothingAndTheCountFollowsTheWeight )
{
    const GrassVariety   v = Variety();
    const GrassCellCoord cell{ 0, 0 };
    const uint32_t       side = GrassCandidatesPerSide( v );
    // UE's maths: 2000 x 2000 cm at 40 per 1000 x 1000 cm is 160 candidates, a 13 x 13 square.
    EXPECT_EQ( side, 13u );

    EXPECT_TRUE( GenerateGrassCell( v, cell, 1u, Uniform( 0.0f ) ).empty() );
    const size_t full = GenerateGrassCell( v, cell, 1u, Uniform( 1.0f ) ).size();
    const size_t half = GenerateGrassCell( v, cell, 1u, Uniform( 0.5f ) ).size();
    EXPECT_EQ( full, side * side ); // weight 1 beats every fraction in [0, 1)
    EXPECT_GT( half, full * 3 / 10 );
    EXPECT_LT( half, full * 7 / 10 );

    // No landscape under a sample is no instance, never one at height zero.
    EXPECT_TRUE(
         GenerateGrassCell( v, cell, 1u, []( float, float ) { return std::optional<GrassSurfaceSample>(); } )
              .empty() );
}

TEST( LandscapeGrass, GrassGrowsOnlyWhereTheLayerIsPainted )
{
    // A real tile, painted on its west half only — the ECS system's sampler, minus the entity lookup.
    const LandscapeFrame frame{ 0.0f, 0.0f, 0.0f, 100.0f, kLandscapeDefaultZScale };
    auto                 created = LandscapeTileData::Create( 21u, 21u );
    auto                 tile    = created.ExtractValue();
    auto                 added   = tile.AddWeightLayer( "Grass" );
    const size_t         layer   = added.ExtractValue();
    const LandscapeRect  west{ 0u, 0u, 10u, 21u };
    ASSERT_TRUE( tile.WriteWeightRegion( layer, west, std::vector<uint8_t>( west.Area(), 255u ) ) );

    const GrassSurfaceSampler surface = [&]( float x, float z ) -> std::optional<GrassSurfaceSample>
    {
        const auto h = SampleLandscapeHeight( tile, frame, x, z );
        const auto w = SampleLandscapeWeight( tile, frame, layer, x, z );
        if ( !h || !w )
            return std::nullopt;
        return GrassSurfaceSample{ *h, glm::vec3( 0.0f, 1.0f, 0.0f ), *w };
    };
    const auto instances = GenerateGrassCell( Variety( 400.0f ), { 0, 0 }, 9u, surface );
    ASSERT_FALSE( instances.empty() );
    for ( const glm::mat4& m : instances )
        EXPECT_LT( m[3].x, 1000.0f ) << "an instance grew on the unpainted half";

    const auto painted = SampleLandscapeWeight( tile, frame, layer, 450.0f, 700.0f );
    const auto edge    = SampleLandscapeWeight( tile, frame, layer, 950.0f, 700.0f );
    const auto bare    = SampleLandscapeWeight( tile, frame, layer, 1500.0f, 700.0f );
    ASSERT_TRUE( painted.has_value() && edge.has_value() && bare.has_value() );
    EXPECT_FLOAT_EQ( *painted, 1.0f ); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( *edge, 0.5f );    // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( *bare, 0.0f );    // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_FALSE( SampleLandscapeWeight( tile, frame, layer, 2000.5f, 700.0f ).has_value() );
}

TEST( LandscapeGrass, APaintStrokeTellsTheGrassConsumer )
{
    auto         created = LandscapeTileData::Create( 5u, 5u );
    auto         tile    = created.ExtractValue();
    auto         added   = tile.AddWeightLayer( "Grass" );
    const size_t layer   = added.ExtractValue();
    (void)tile.TakeDirtyRects( LandscapeDirtyConsumer::Grass );
    const LandscapeRect r{ 1u, 1u, 3u, 3u };
    ASSERT_TRUE( tile.WriteWeightRegion( layer, r, std::vector<uint8_t>( r.Area(), 200u ) ) );
    EXPECT_EQ( tile.TakeDirtyRects( LandscapeDirtyConsumer::Grass ).size(), 1u );
    tile.SetSample( 2u, 2u, 40000u );
    EXPECT_EQ( tile.TakeDirtyRects( LandscapeDirtyConsumer::Grass ).size(), 1u );
}

// ── The streamer ───────────────────────────────────────────────────────────────────────────────────────────

TEST( LandscapeGrass, TheCullDistanceIsTheFoliageFade )
{
    // Grass is drawn through the foliage type's fade (FO-5): all nearer than Start, none from End, a share
    // between — the same function the ISM loop calls, on the variety's two numbers.
    const GrassVariety                          v    = Variety();
    const Desert::Graphic::InstanceCullDistance cull = GrassCullDistance( v );
    EXPECT_EQ( cull.Min, v.StartCullDistance );
    EXPECT_EQ( cull.Max, v.EndCullDistance );
    const glm::vec3 eye( 0.0f );
    uint32_t        nearCount = 0;
    uint32_t        band      = 0;
    uint32_t        farCount  = 0;
    for ( uint32_t i = 0; i < 1000u; ++i )
    {
        nearCount += Desert::Graphic::KeepsInstanceAtDistance( cull, i, { 3000.0f, 0.0f, 0.0f }, eye ) ? 1u : 0u;
        band += Desert::Graphic::KeepsInstanceAtDistance( cull, i, { 4500.0f, 0.0f, 0.0f }, eye ) ? 1u : 0u;
        farCount += Desert::Graphic::KeepsInstanceAtDistance( cull, i, { 5000.0f, 0.0f, 0.0f }, eye ) ? 1u : 0u;
    }
    EXPECT_EQ( nearCount, 1000u );
    EXPECT_GT( band, 300u );
    EXPECT_LT( band, 700u );
    EXPECT_EQ( farCount, 0u );
}

TEST( LandscapeGrass, CellsAroundTheCameraAppearAndDisappearAsItMoves )
{
    GrassCellStreamer           streamer;
    std::vector<GrassCellCoord> generated;
    const auto                  generate = [&]( GrassCellCoord c )
    {
        generated.push_back( c );
        return GenerateGrassCell( Variety(), c, 1u, Uniform( 1.0f ) );
    };
    const float radius = 3000.0f;

    const auto first  = streamer.Tick( { 100.0f, 100.0f }, radius, 1000u, generate );
    const auto wanted = GrassCellsInRange( { 100.0f, 100.0f }, radius );
    EXPECT_EQ( first.Generated, wanted.size() );
    EXPECT_EQ( streamer.Cells().size(), wanted.size() );
    EXPECT_TRUE( streamer.Cells().contains( GrassCellAt( 100.0f, 100.0f ) ) );

    // A still camera generates nothing more.
    generated.clear();
    EXPECT_EQ( streamer.Tick( { 100.0f, 100.0f }, radius, 1000u, generate ).Generated, 0u );

    // Five cells east: the western cells leave, the eastern ones arrive, and the set is exactly the new range.
    const glm::vec2 moved( 100.0f + 5.0f * kLandscapeGrassCellCm, 100.0f );
    const auto      second = streamer.Tick( moved, radius, 1000u, generate );
    EXPECT_GT( second.Evicted, 0u );
    EXPECT_GT( second.Generated, 0u );
    EXPECT_FALSE( streamer.Cells().contains( GrassCellAt( 100.0f, 100.0f ) ) );
    const auto                     now = GrassCellsInRange( moved, radius );
    const std::set<GrassCellCoord> expected( now.begin(), now.end() );
    std::set<GrassCellCoord>       held;
    for ( const auto& [cell, instances] : streamer.Cells() )
        held.insert( cell );
    EXPECT_EQ( held, expected );

    // Coming back regrows the very same grass: nothing was stored, and nothing differs.
    const auto again = GenerateGrassCell( Variety(), GrassCellAt( 100.0f, 100.0f ), 1u, Uniform( 1.0f ) );
    streamer.Tick( { 100.0f, 100.0f }, radius, 1000u, generate );
    EXPECT_EQ( Hash( streamer.Cells().at( GrassCellAt( 100.0f, 100.0f ) ) ), Hash( again ) );
}

TEST( LandscapeGrass, NoTickGeneratesMoreThanItsBudget )
{
    GrassCellStreamer streamer;
    uint32_t          calls    = 0;
    const auto        generate = [&]( GrassCellCoord )
    {
        ++calls;
        return std::vector<glm::mat4>( 1u );
    };
    const size_t wanted = GrassCellsInRange( { 0.0f, 0.0f }, 5000.0f ).size();
    ASSERT_GT( wanted, 9u );

    size_t ticks = 0;
    for ( ;; )
    {
        const uint32_t before = calls;
        const auto     result = streamer.Tick( { 0.0f, 0.0f }, 5000.0f, 3u, generate );
        EXPECT_LE( calls - before, 3u );
        EXPECT_EQ( result.Generated, calls - before );
        ++ticks;
        if ( result.Missing == 0u )
            break;
        ASSERT_LT( ticks, 100u );
    }
    EXPECT_EQ( streamer.Cells().size(), wanted );
    EXPECT_EQ( ticks, ( wanted + 2u ) / 3u );
    EXPECT_EQ( streamer.Instances()->size(), wanted );
}

TEST( LandscapeGrass, TheNearestCellsAreGeneratedFirst )
{
    GrassCellStreamer streamer;
    const auto        generate = []( GrassCellCoord ) { return std::vector<glm::mat4>(); };
    streamer.Tick( { 500.0f, 500.0f }, 8000.0f, 1u, generate );
    ASSERT_EQ( streamer.Cells().size(), 1u );
    EXPECT_TRUE( streamer.Cells().contains( GrassCellAt( 500.0f, 500.0f ) ) );
}

TEST( LandscapeGrass, InvalidateDropsTheCellsUnderAStroke )
{
    GrassCellStreamer streamer;
    const auto        generate = []( GrassCellCoord ) { return std::vector<glm::mat4>( 2u ); };
    streamer.Tick( { 0.0f, 0.0f }, 3000.0f, 1000u, generate );
    const size_t before = streamer.Cells().size();
    streamer.Invalidate( { 10.0f, 10.0f }, { 20.0f, 20.0f } );
    EXPECT_EQ( streamer.Cells().size(), before - 1u );
    EXPECT_FALSE( streamer.Cells().contains( GrassCellCoord{ 0, 0 } ) );
    EXPECT_EQ( streamer.Tick( { 0.0f, 0.0f }, 3000.0f, 1000u, generate ).Generated, 1u );
}

// ── The formats ────────────────────────────────────────────────────────────────────────────────────────────

TEST( LandscapeGrass, AGrassTypeRoundTripsAndStatesItsMeshes )
{
    LandscapeGrassTypeData data;
    data.GrassVarieties = { Variety(), Variety( 10.0f ) };
    const auto read     = ParseLandscapeGrassType( WriteLandscapeGrassType( data ) );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( read.GetValue().GrassVarieties, data.GrassVarieties );
    // Two varieties of one mesh are ONE dependency.
    const auto& header = read.GetValue().Header;
    ASSERT_TRUE( header.has_value() );
    EXPECT_EQ( header->Dependencies,
               std::vector<std::string>{ kMeshGuid } ); // NOLINT(bugprone-unchecked-optional-access)
}

TEST( LandscapeGrass, AGrassTypeRefusesWhatItCannotGrow )
{
    const LandscapeGrassTypeData none;
    EXPECT_FALSE( ValidateLandscapeGrassTypeData( none ) );

    LandscapeGrassTypeData noMesh;
    noMesh.GrassVarieties              = { Variety() };
    noMesh.GrassVarieties[0].GrassMesh = {};
    EXPECT_FALSE( ValidateLandscapeGrassTypeData( noMesh ) );

    LandscapeGrassTypeData zero;
    zero.GrassVarieties = { Variety( 0.0f ) };
    const auto refused  = ValidateLandscapeGrassTypeData( zero );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "GrassDensity" ), std::string::npos );

    LandscapeGrassTypeData fadeBeyondEnd;
    fadeBeyondEnd.GrassVarieties                      = { Variety() };
    fadeBeyondEnd.GrassVarieties[0].StartCullDistance = 6000.0f;
    const auto inverted                               = ValidateLandscapeGrassTypeData( fadeBeyondEnd );
    ASSERT_FALSE( inverted );
    EXPECT_NE( inverted.GetError().find( "StartCullDistance" ), std::string::npos );

    // The header's Dependencies must be exactly the meshes.
    LandscapeGrassTypeData ok;
    ok.GrassVarieties = { Variety() };
    auto parsed       = ParseLandscapeGrassType( WriteLandscapeGrassType( ok ) );
    auto stamped      = parsed.ExtractValue();
    ASSERT_TRUE( stamped.Header.has_value() );
    stamped.Header->Dependencies.clear(); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_FALSE( ParseLandscapeGrassType( Common::Json::Write( stamped ) ) );
}

TEST( LandscapeGrass, ALayerInfoNamesItsGrassAsItsOneDependency )
{
    LandscapeLayerInfoData layer;
    layer.LayerName = "Grass";
    layer.GrassType = { kGrassGuid, "Assets/Landscape/Grass/Meadow.degrasstype" };
    const auto read = ParseLandscapeLayerInfo( WriteLandscapeLayerInfo( layer ) );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( read.GetValue().GrassType, layer.GrassType );
    const auto& header = read.GetValue().Header;
    ASSERT_TRUE( header.has_value() );
    EXPECT_EQ( header->Dependencies,
               std::vector<std::string>{ kGrassGuid } ); // NOLINT(bugprone-unchecked-optional-access)

    // A GUID without a path (or the reverse) is refused, not half-read.
    layer.GrassType.Path.clear();
    EXPECT_FALSE( ValidateLandscapeLayerInfoData( layer ) );
}

TEST( LandscapeGrass, ALayerInfoOfTheFirstGenerationIsRefusedByNumber )
{
    LandscapeLayerInfoData layer;
    layer.LayerName = "Grass";
    auto parsed     = ParseLandscapeLayerInfo( WriteLandscapeLayerInfo( layer ) );
    auto stamped    = parsed.ExtractValue();
    ASSERT_TRUE( stamped.Header.has_value() );
    for ( auto& [tag, version] : stamped.Header->Versions ) // NOLINT(bugprone-unchecked-optional-access)
        version = 1u;
    const auto refused = ParseLandscapeLayerInfo( Common::Json::Write( stamped ) );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( '1' ), std::string::npos );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

TEST( LandscapeGrass, TheShippedGrassTypeIsReadable )
{
    // The one tracked `.degrasstype` (the corpus row CookedRegistryGate requires): it parses at the current
    // version and its mesh is a file this repository tracks.
    const std::filesystem::path root = Desert::TestSupport::RepositoryRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";
    std::ifstream in( root / "Editor/Resources/Assets/Landscape/Grass/Meadow.degrasstype" );
    ASSERT_TRUE( in ) << "Meadow.degrasstype is missing from the checkout";
    const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    const auto        read = ParseLandscapeGrassType( text );
    ASSERT_TRUE( read ) << read.GetError();
    ASSERT_EQ( read.GetValue().GrassVarieties.size(), 1u );
    EXPECT_TRUE(
         std::filesystem::exists( root / "Editor/Resources" / read.GetValue().GrassVarieties[0].GrassMesh.Path ) );
}
