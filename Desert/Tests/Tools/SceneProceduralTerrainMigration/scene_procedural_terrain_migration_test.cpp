// SceneMigrator v22 -> v23: the procedural terrain (value-noise fBm evaluated by the tessellation shader) is
// baked into a landscape - a `Landscape` root + `LandscapeMaterial` on the same entity and a grid of
// `LandscapeTile` entities whose `.dlht` heights are that fBm at every sample (owner decision O2, LS-6).
//
// What this suite holds:
//   1. THE HEIGHTS. Every sample of every tile, decoded by the loader's own codec, is the fBm at that node
//      to within half a 16-bit step - measured against a SECOND transcription of the retired GLSL written
//      here with glm's vector functions, so the step's scalar port is checked against something it did not
//      produce. The largest difference in centimetres is printed for the record.
//   2. The seams: neighbouring tiles repeat the same edge samples (the renderer relies on it).
//   3. The frame: the root moves to sample (0, 0), the spacing and ZScale are what the grid says, every tile
//      names the root and a file under the assets root.
//   4. The look: the material and the layer modes move to LandscapeMaterial, and the integers written are
//      the REFLECTED LandscapeLayerMode's own - Off stays Off, Manual becomes Auto and is named.
//   5. Refusals (rotated, unequal x/z scale, no source file) leave the block untouched; a second run is a no-op.
//   6. The step is the head; no tracked scene or prefab states a Terrain block; and no source outside the
//      migrator and its suites names the retired path.

#include <SceneMigration.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

#include <glm/glm.hpp>
#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
namespace Landscape = Desert::World::Landscape;
using Desert::Assets::EntityData;

namespace
{
    // THE ORACLE: TerrainTessEval.glslh as it stood at v22, in its own vector form (fract/floor/dot on
    // vec2), so the step's scalar port is compared with a transcription it shares no code with. No product
    // is summed in the same expression it is formed in (see the port's note: a contracted multiply-add
    // moves the lattice hash), which is why mix is spelled out here rather than taken from glm.
    float MixRef( float a, float b, float t )
    {
        const float wa = a * ( 1.0f - t );
        const float wb = b * t;
        return wa + wb;
    }

    float HashRef( glm::vec2 p )
    {
        p = glm::fract( p * glm::vec2( 123.34f, 456.21f ) );
        p += glm::dot( p, p + 45.32f );
        return glm::fract( p.x * p.y );
    }

    float ValueNoiseRef( glm::vec2 p )
    {
        const glm::vec2 i = glm::floor( p );
        const glm::vec2 f = glm::fract( p );
        const glm::vec2 u = f * f * ( 3.0f - 2.0f * f );
        const float     a = HashRef( i + glm::vec2( 0.0f, 0.0f ) );
        const float     b = HashRef( i + glm::vec2( 1.0f, 0.0f ) );
        const float     c = HashRef( i + glm::vec2( 0.0f, 1.0f ) );
        const float     d = HashRef( i + glm::vec2( 1.0f, 1.0f ) );
        return MixRef( MixRef( a, b, u.x ), MixRef( c, d, u.x ), u.y );
    }

    float TerrainHeightRef( glm::vec2 xz, float frequency, int seed, float heightScale )
    {
        const float     freq = std::max( frequency, 0.0001f );
        const glm::vec2 s( static_cast<float>( seed ) * 0.137f, static_cast<float>( seed ) * 0.911f );
        const glm::vec2 p   = xz * freq + s;
        float           sum = 0.0f, amp = 0.5f, f = 1.0f;
        for ( int i = 0; i < 5; ++i )
        {
            const float octave = amp * ValueNoiseRef( p * f );
            sum += octave;
            f *= 2.0f;
            amp *= 0.5f;
        }
        return ( sum - 0.5f ) * 2.0f * heightScale;
    }

    struct V22Terrain
    {
        float       Size           = 5000.0f;
        int         Resolution     = 64;
        float       HeightScale    = 500.0f;
        float       NoiseFrequency = 0.08f;
        int         Seed           = 1337;
        std::string Extra; // raw JSON fields appended to the block, e.g. ,"RockMode":2
    };

    EntityData TerrainEntity( uint64_t id, const V22Terrain& t, glm::vec3 translation = glm::vec3( 0.0f ) )
    {
        EntityData e;
        e.id          = Common::UUID( id );
        e.Tag         = "Ground";
        e.Translation = translation;
        e.Rotation    = glm::vec3( 0.0f );
        e.Scale       = glm::vec3( 1.0f );
        std::ostringstream json;
        json << R"({"Size":)" << t.Size << R"(,"Resolution":)" << t.Resolution << R"(,"HeightScale":)"
             << t.HeightScale << R"(,"NoiseFrequency":)" << t.NoiseFrequency << R"(,"Seed":)" << t.Seed << t.Extra
             << "}";
        e.Components["Terrain"] = rfl::json::read<rfl::Generic>( json.str() ).value();
        return e;
    }

    std::optional<rfl::Object<rfl::Generic>> Block( const EntityData& e, const char* key )
    {
        const auto g = e.Components.get( key );
        if ( !g.has_value() )
            return std::nullopt;
        return g.value().to_object().value();
    }

    double Number( const rfl::Object<rfl::Generic>& o, const char* key )
    {
        const auto g = o.get( key ).value();
        if ( const auto d = g.to_double(); d.has_value() )
            return d.value();
        return static_cast<double>( g.to_int64().value() );
    }

    const std::filesystem::path kSource = "Assets/Scenes/Probe.desce";
    const std::filesystem::path kRoot   = "Assets";

    // Runs the step on one terrain and returns the tiles decoded, in file order (row-major by tile).
    struct Baked
    {
        Migration::ProceduralTerrainMigrationReport Report;
        std::vector<EntityData>                     Entities;
        std::vector<Landscape::LandscapeTileData>   Tiles;
    };

    Baked Bake( const V22Terrain& t, glm::vec3 translation = glm::vec3( 0.0f ) )
    {
        Baked b;
        b.Entities.push_back( TerrainEntity( 4242, t, translation ) );
        b.Report = Migration::MigrateProceduralTerrainV22ToV23( b.Entities, kSource, kRoot );
        for ( const auto& file : b.Report.Files )
        {
            auto tile = Landscape::DecodeLandscapeTile( file.Bytes );
            EXPECT_TRUE( tile.IsSuccess() ) << file.Path.string();
            if ( tile.IsSuccess() )
                b.Tiles.push_back( tile.ExtractValue() );
        }
        return b;
    }

    // The largest |decoded - fBm| over every sample of every tile, in centimetres.
    float MaxHeightError( const V22Terrain& t, const Baked& b )
    {
        const auto  grid  = Migration::ProceduralTerrainGridFor( t.Size, t.Resolution );
        const auto  root  = Block( b.Entities[0], "Landscape" ).value();
        const float z     = static_cast<float>( Number( root, "ZScale" ) );
        const float quads = static_cast<float>( grid.TilesPerSide * grid.QuadsPerTile );
        const float cell  = t.Size / quads;
        float       worst = 0.0f;
        for ( uint32_t tz = 0; tz < grid.TilesPerSide; ++tz )
            for ( uint32_t tx = 0; tx < grid.TilesPerSide; ++tx )
            {
                const auto&    tile = b.Tiles[tz * grid.TilesPerSide + tx];
                const uint32_t side = tile.SamplesX();
                for ( uint32_t sz = 0; sz < side; ++sz )
                    for ( uint32_t sx = 0; sx < side; ++sx )
                    {
                        const float     gx  = static_cast<float>( tx * grid.QuadsPerTile + sx );
                        const float     gz  = static_cast<float>( tz * grid.QuadsPerTile + sz );
                        const glm::vec2 at  = glm::vec2( gx, gz ) * cell;
                        const float     ref = TerrainHeightRef( at - glm::vec2( t.Size * 0.5f ), t.NoiseFrequency,
                                                                t.Seed, t.HeightScale );
                        const float     got = Landscape::LandscapeHeightCm( tile.Samples()[sz * side + sx], z );
                        worst               = std::max( worst, std::abs( got - ref ) );
                    }
            }
        return worst;
    }

    std::optional<int64_t> ReflectedLayerMode( const char* enumerator )
    {
        const auto* type = Desert::Reflection::ReflectionRegistry::Get().Find( "LandscapeMaterialData" );
        if ( type == nullptr )
            return std::nullopt;
        for ( const auto& field : type->Fields )
            if ( field.TypeName == "LandscapeLayerMode" )
                for ( const auto& value : field.EnumValues )
                    if ( value.Name == enumerator )
                        return static_cast<int64_t>( value.Value );
        return std::nullopt;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

// ── 1. The heights ──────────────────────────────────────────────────────────────────────────────────

TEST( SceneProceduralTerrainMigration, EverySampleIsTheFbmToHalfAStep )
{
    // The four parameter sets of the tracked corpus (Terrain_Grass/MatProbe, G3's two, G26), at their own
    // sizes. Resolution 96 is clamped to 64 exactly as the renderer clamped it.
    const std::array<V22Terrain, 4> corpus = { {
         { 30000.0f, 64, 400.0f, 0.0006f, 2026, "" },
         { 20000.0f, 64, 900.0f, 0.0012f, 111, "" },
         { 20000.0f, 64, 2600.0f, 0.0005f, 999, "" },
         { 5000.0f, 96, 900.0f, 0.004f, 111, "" },
    } };
    for ( const V22Terrain& t : corpus )
    {
        const Baked b = Bake( t );
        ASSERT_EQ( b.Report.Entities, 1 ) << ( b.Report.RejectedNames.empty() ? "" : b.Report.RejectedNames[0] );
        const auto grid = Migration::ProceduralTerrainGridFor( t.Size, t.Resolution );
        ASSERT_EQ( b.Tiles.size(), static_cast<size_t>( grid.TilesPerSide * grid.TilesPerSide ) );

        const float step  = t.HeightScale / 256.0f / 128.0f; // ZScale / 128: one 16-bit step in cm
        const float worst = MaxHeightError( t, b );
        std::printf( "[ heights  ] Size %.0f HeightScale %.0f: %u x %u tiles of %u quads, max |h - fBm| = %.4f cm "
                     "(half step %.4f cm)\n",
                     t.Size, t.HeightScale, grid.TilesPerSide, grid.TilesPerSide, grid.QuadsPerTile, worst,
                     step * 0.5f );
        EXPECT_LE( worst, step * 0.5f + 1.0e-3f );
    }
}

TEST( SceneProceduralTerrainMigration, TheGridReproducesTheRenderersFinestSpacing )
{
    // 64 patches x 16 tessellation = 1024 quads a side at most; tiles of one of UE's section sizes, which
    // is all a landscape root accepts (a 205-quad tile was refused at load - the first bake shipped them).
    const auto g = Migration::ProceduralTerrainGridFor( 30000.0f, 64 );
    EXPECT_EQ( g.TilesPerSide, 5u ); // 5 x 255 = 1275 is within 1/8 of 9 x 127 = 1143, the fewest
    EXPECT_EQ( g.QuadsPerTile, 255u );
    EXPECT_LE( g.SpacingCm, 30000.0f / 1024.0f );
    const auto small = Migration::ProceduralTerrainGridFor( 5000.0f, 8 );
    EXPECT_EQ( small.TilesPerSide, 9u ); // 128 quads: 9 x 15 = 135 is the only size within 1/8 of the fewest
    EXPECT_EQ( small.QuadsPerTile, 15u );
    for ( int resolution = 1; resolution <= 64; ++resolution )
    {
        const auto                              any = Migration::ProceduralTerrainGridFor( 1000.0f, resolution );
        Desert::World::Landscape::LandscapeRoot root;
        root.QuadsPerTile = any.QuadsPerTile;
        root.SpacingCm    = any.SpacingCm;
        EXPECT_TRUE( Desert::World::Landscape::ValidateLandscapeRoot( root ).IsSuccess() ) << resolution;
        EXPECT_GE( any.TilesPerSide * any.QuadsPerTile, static_cast<uint32_t>( resolution ) * 16u );
    }
}

// ── 2. The seams ────────────────────────────────────────────────────────────────────────────────────

TEST( SceneProceduralTerrainMigration, NeighbouringTilesShareTheirEdgeSamples )
{
    const V22Terrain t{ 5000.0f, 32, 700.0f, 0.004f, 7, "" };
    const Baked      b    = Bake( t );
    const auto       grid = Migration::ProceduralTerrainGridFor( t.Size, t.Resolution );
    ASSERT_GE( grid.TilesPerSide, 2u );
    const uint32_t side = grid.QuadsPerTile + 1u;
    for ( uint32_t tz = 0; tz < grid.TilesPerSide; ++tz )
        for ( uint32_t tx = 0; tx + 1 < grid.TilesPerSide; ++tx )
        {
            const auto& west = b.Tiles[tz * grid.TilesPerSide + tx].Samples();
            const auto& east = b.Tiles[tz * grid.TilesPerSide + tx + 1].Samples();
            for ( uint32_t z = 0; z < side; ++z )
                ASSERT_EQ( west[z * side + side - 1u], east[z * side] )
                     << "tile " << tx << "," << tz << " row " << z;
        }
}

// ── 3. The frame ────────────────────────────────────────────────────────────────────────────────────

TEST( SceneProceduralTerrainMigration, TheRootSitsAtSampleZeroAndEveryTileNamesIt )
{
    // Several tiles, so every tile's reference to the root is checked; the grid itself is read from
    // ProceduralTerrainGridFor rather than restated (the grid test above pins the rule).
    const V22Terrain t{ 5000.0f, 16, 900.0f, 0.004f, 111, "" };
    const Baked      b    = Bake( t, glm::vec3( -2600.0f, 50.0f, 300.0f ) );
    const auto       grid = Migration::ProceduralTerrainGridFor( t.Size, t.Resolution );
    ASSERT_EQ( b.Report.Entities, 1 );
    ASSERT_EQ( b.Entities.size(), 1u + grid.TilesPerSide * grid.TilesPerSide ); // the root and its tiles

    const EntityData& root = b.Entities[0];
    EXPECT_FALSE( root.Components.get( "Terrain" ).has_value() );
    EXPECT_EQ( root.Translation.value(), glm::vec3( -2600.0f - 2500.0f, 50.0f, 300.0f - 2500.0f ) );
    const auto frame = Block( root, "Landscape" ).value();
    EXPECT_EQ( Number( frame, "QuadsPerTile" ), static_cast<double>( grid.QuadsPerTile ) );
    EXPECT_FLOAT_EQ( static_cast<float>( Number( frame, "SpacingCm" ) ),
                     5000.0f / static_cast<float>( grid.TilesPerSide * grid.QuadsPerTile ) );
    EXPECT_FLOAT_EQ( static_cast<float>( Number( frame, "ZScale" ) ), 900.0f / 256.0f );

    const auto tile = Block( b.Entities[1], "LandscapeTile" ).value();
    EXPECT_EQ( tile.get( "Landscape" ).value().to_string().value(), "4242" )
         << "a save spells an id as a decimal string";
    EXPECT_EQ( Number( tile, "TileX" ), 0.0 );
    const std::string file = tile.get( "HeightFile" ).value().to_string().value();
    const std::string want =
         ( Common::Constants::Path::ASSETS_PATH / "Scenes/Probe_Landscape" ).generic_string() + "/";
    EXPECT_EQ( file.rfind( want, 0 ), 0u ) << file;
    EXPECT_EQ( b.Report.Files[0].Path.parent_path().generic_string(), "Assets/Scenes/Probe_Landscape" );
}

// ── 4. The look ─────────────────────────────────────────────────────────────────────────────────────

TEST( SceneProceduralTerrainMigration, TheLayerModesAreTheReflectedEnumsOwnValues )
{
    ASSERT_TRUE( ReflectedLayerMode( "Auto" ).has_value() )
         << "LandscapeMaterialData reflects no LandscapeLayerMode";
    ASSERT_TRUE( ReflectedLayerMode( "Off" ).has_value() );

    V22Terrain t{ 5000.0f, 8, 900.0f, 0.004f, 111, "" };
    t.Extra        = R"(,"Material":"Materials/M_Probe.demat","GrassMode":1,"RockMode":2,"SnowMode":0)";
    const Baked b  = Bake( t );
    const auto  mt = Block( b.Entities[0], "LandscapeMaterial" ).value();
    EXPECT_EQ( mt.get( "Material" ).value().to_string().value(), "Materials/M_Probe.demat" );
    EXPECT_EQ( mt.get( "GrassMode" ).value().to_int64().value(), *ReflectedLayerMode( "Auto" ) ) << "Manual";
    EXPECT_EQ( mt.get( "RockMode" ).value().to_int64().value(), *ReflectedLayerMode( "Off" ) );
    EXPECT_EQ( mt.get( "SnowMode" ).value().to_int64().value(), *ReflectedLayerMode( "Auto" ) );
    ASSERT_EQ( b.Report.ConvertedNames.size(), 1u );
    EXPECT_NE( b.Report.ConvertedNames[0].find( "GrassMode Manual -> Auto" ), std::string::npos )
         << b.Report.ConvertedNames[0];
}

// ── 5. Refusals and idempotence ─────────────────────────────────────────────────────────────────────

TEST( SceneProceduralTerrainMigration, AFrameTheLandscapeCannotStateIsNamedAndLeftAlone )
{
    const V22Terrain        t{ 5000.0f, 8, 900.0f, 0.004f, 111, "" };
    std::vector<EntityData> rotated{ TerrainEntity( 1, t ) };
    rotated[0].Rotation = glm::vec3( 0.0f, 0.3f, 0.0f );
    std::vector<EntityData> stretched{ TerrainEntity( 2, t ) };
    stretched[0].Scale = glm::vec3( 1.0f, 1.0f, 2.0f );
    std::vector<EntityData> nameless{ TerrainEntity( 3, t ) };

    for ( auto* entities : { &rotated, &stretched } )
    {
        const auto r = Migration::MigrateProceduralTerrainV22ToV23( *entities, kSource, kRoot );
        EXPECT_EQ( r.Rejected, 1 );
        EXPECT_EQ( entities->size(), 1u );
        EXPECT_TRUE( ( *entities )[0].Components.get( "Terrain" ).has_value() );
        EXPECT_TRUE( r.Files.empty() );
    }
    const auto r = Migration::MigrateProceduralTerrainV22ToV23( nameless, {}, kRoot );
    EXPECT_EQ( r.Rejected, 1 );
    EXPECT_TRUE( nameless[0].Components.get( "Terrain" ).has_value() );
}

TEST( SceneProceduralTerrainMigration, ASecondRunDoesNothingAndTheIdsAreStable )
{
    const V22Terrain t{ 5000.0f, 32, 900.0f, 0.004f, 111, "" };
    Baked            first  = Bake( t );
    const Baked      second = Bake( t );
    ASSERT_EQ( first.Entities.size(), second.Entities.size() );
    for ( size_t i = 0; i < first.Entities.size(); ++i )
        EXPECT_EQ( first.Entities[i].id.value(), second.Entities[i].id.value() );

    const auto again = Migration::MigrateProceduralTerrainV22ToV23( first.Entities, kSource, kRoot );
    EXPECT_EQ( again.Entities, 0 );
    EXPECT_TRUE( again.Files.empty() );
}

// ── 6. The head, the corpus and the sources ─────────────────────────────────────────────────────────

TEST( SceneProceduralTerrainMigration, TheStepFollowsEditMeshAndPrecedesTextureAssetRefs )
{
    EXPECT_EQ( Migration::kSceneVersionProceduralTerrain + 1, Migration::kSceneVersionTextureAssetRefs );
    EXPECT_EQ( Migration::kSceneVersionProceduralTerrain, Migration::kSceneVersionEditMesh + 1 );
}

TEST( SceneProceduralTerrainMigration, NoTrackedSceneOrPrefabStatesATerrainBlock )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    int checked = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( root + "Editor/Resources/Assets" ) )
    {
        const auto ext = entry.path().extension();
        if ( ext != ".desce" && ext != ".deprefab" )
            continue;
        if ( entry.path().generic_string().find( "/Autosave/" ) != std::string::npos )
            continue; // the editor's gitignored crash copies are never migrated
        ++checked;
        EXPECT_EQ( ReadAll( entry.path() ).find( "\"Terrain\":{" ), std::string::npos )
             << entry.path().string() << " still states a procedural Terrain block - run Tools/SceneMigrator";
    }
    EXPECT_GT( checked, 0 );
}

// The retired path by its identifiers. The migrator and the migration suites are exempt: they are the one
// place the v22 format is allowed to be known (DEV_CONTRACT §4.3). `Terrain` alone is NOT a retired name -
// it is still the material domain, the shader program and TerrainRenderer that draw every landscape.
TEST( SceneProceduralTerrainMigration, NoSourceNamesTheRetiredPath )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::array<const char*, 11> retired = {
         "TerrainComponent",   "TerrainData",   "TerrainLayerMode",     "TerrainECSSystem",
         "DrawTerrainCommand", "SubmitTerrain", "TerrainMeshFactory",   "TerrainPaintTool",
         "u_SplatMap",         "FBm(",          "kTerrainHeightSource",
    };
    int scanned = 0;
    for ( const char* dir : { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source",
                              "Editor/Resources/Shaders", "Desert/Tests/Engine" } )
    {
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root + dir ) )
        {
            const auto ext = entry.path().extension();
            if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".glsl" && ext != ".glslh" &&
                 ext != ".shader" )
                continue;
            if ( entry.path().generic_string().find( "/Generated/" ) != std::string::npos )
                continue; // regenerated from the annotations on every build
            ++scanned;
            const std::string text = ReadAll( entry.path() );
            for ( const char* name : retired )
                EXPECT_EQ( text.find( name ), std::string::npos ) << entry.path().string() << " names " << name;
        }
    }
    EXPECT_GT( scanned, 100 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
