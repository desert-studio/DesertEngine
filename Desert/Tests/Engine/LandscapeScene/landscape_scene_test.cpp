// A LANDSCAPE IN A SCENE: THE ROOT, ITS TILES, AND THE TILE FILES BESIDE THE .desce.
//
// WHAT IS ASSERTED:
//
//   1. THE TRIP IS BYTE-EXACT. A root and 2 x 2 tiles with distinct terrain are saved — tile files first,
//      then the scene naming them — loaded back from the text and bytes on disk, and saved again to the
//      same place: the scene and all four tile files are byte-identical to the first save, and every
//      sample came back.
//   2. THE FILES BELONG TO THE SCENE THAT SAVED THEM. The same landscape saved under a second name writes a
//      second set of files and leaves the first scene's untouched — "Save As" does not make two levels share
//      a terrain.
//   3. A TILE THAT DISAGREES WITH ITS ROOT IS REFUSED, by both numbers; a file that is not a tile is refused
//      by name. Neither is loaded at some other size.
//   4. NEIGHBOURING TILES AGREE ON THEIR SEAM. The frame LandscapeTileFrame gives tile (1, 0) starts exactly
//      at tile (0, 0)'s last column, so a point on the seam is answered identically from both sides — the
//      precondition LS-2 left for whoever places tiles (its sampling sees one tile at a time).
//
// The save and load below are the steps SceneSerializer takes (WriteLandscapeTiles, then the registry's
// LandscapeTile block, then CheckLandscapeTiles), made of the same functions; SceneSerializer.cpp itself is
// compiled by no suite (see premake5.lua). The editor run in the LS-3 report covers the glue.

#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace Desert;
using namespace Desert::World::Landscape;
using Desert::Assets::EntityData;
using Desert::Core::SceneSerialized;
using Desert::Core::Serialize::ReadComponent;
using Desert::Core::Serialize::WriteComponent;

namespace
{
    constexpr uint64_t kRootId = 7100;

    // The landscape as the loaded scene holds it: the root's block, and each tile's block plus its heights.
    struct TileEntity
    {
        uint64_t                    Id = 0;
        ECS::LandscapeTileComponent Tile;
    };

    struct LandscapeScene
    {
        ECS::LandscapeComponent Root;
        std::vector<TileEntity> Tiles;
    };

    // A tile of distinct, non-flat terrain: the value depends on the tile AND the sample, so a tile file
    // swapped with its neighbour, or rows transposed, cannot come back equal.
    LandscapeTileData Terrain( uint32_t samples, int32_t tileX, int32_t tileZ )
    {
        std::vector<uint16_t> values( static_cast<size_t>( samples ) * samples );
        for ( uint32_t z = 0; z < samples; ++z )
            for ( uint32_t x = 0; x < samples; ++x )
                values[static_cast<size_t>( z ) * samples + x] =
                     static_cast<uint16_t>( 30000 + 97 * x + 13 * z + 1000 * tileX + 2000 * tileZ );
        auto tile = LandscapeTileData::FromSamples( samples, samples, std::move( values ) );
        EXPECT_TRUE( tile.IsSuccess() );
        return tile.ExtractValue();
    }

    LandscapeScene TwoByTwo()
    {
        LandscapeScene scene;
        scene.Root.QuadsPerTile = 31u;
        scene.Root.SpacingCm    = 50.0f;
        scene.Root.ZScale       = 200.0f;

        const uint32_t samples = scene.Root.QuadsPerTile + 1u;
        uint64_t       id      = kRootId + 1;
        for ( int32_t z = 0; z < 2; ++z )
            for ( int32_t x = 0; x < 2; ++x )
            {
                TileEntity entity;
                entity.Id             = id++;
                entity.Tile.Landscape = Common::UUID( kRootId );
                entity.Tile.TileX     = x;
                entity.Tile.TileZ     = z;
                entity.Tile.Heights   = Terrain( samples, x, z );
                scene.Tiles.push_back( entity );
            }
        return scene;
    }

    std::string ReadText( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // SAVE, in SceneSerializer's order: each tile's heights into the file derived from the destination, the
    // file name into the tile's block, and then the scene that names them.
    void Save( LandscapeScene& scene, const fs::path& scenePath )
    {
        SceneSerialized file;
        file.SceneName    = scenePath.stem().string();
        file.SceneVersion = Core::kSceneVersion;
        file.UnitVersion  = Core::kUnitVersion;

        EntityData root;
        root.id                      = Common::UUID( kRootId );
        root.Tag                     = "Landscape";
        root.Translation             = glm::vec3( 0.0f );
        root.Components["Landscape"] = rfl::Generic( WriteComponent( scene.Root ) );
        file.Entities.push_back( root );

        for ( TileEntity& entity : scene.Tiles )
        {
            const fs::path blob    = LandscapeTileBlobPath( scenePath, entity.Id );
            const auto     written = WriteLandscapeTileFile( blob, *entity.Tile.Heights );
            ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
            entity.Tile.HeightFile = blob.generic_string();

            EntityData tile;
            tile.id                          = Common::UUID( entity.Id );
            tile.Tag                         = "LandscapeTile";
            tile.Components["LandscapeTile"] = rfl::Generic( WriteComponent( entity.Tile ) );
            file.Entities.push_back( tile );
        }

        std::ofstream out( scenePath, std::ios::binary | std::ios::trunc );
        out << rfl::json::write( file );
    }

    // LOAD, from the text and the bytes: the blocks through the component readers, then each tile's file,
    // then each tile checked against its root.
    LandscapeScene Load( const fs::path& scenePath )
    {
        LandscapeScene scene;
        const auto     parsed = rfl::json::read<SceneSerialized>( ReadText( scenePath ) );
        EXPECT_TRUE( parsed.has_value() ) << scenePath;
        if ( !parsed.has_value() )
            return scene;

        for ( const EntityData& record : parsed->Entities )
        {
            if ( const auto block = record.Components.get( "Landscape" ); block.has_value() )
                ReadComponent( block.value().to_object().value(), scene.Root );
            if ( const auto block = record.Components.get( "LandscapeTile" ); block.has_value() )
            {
                TileEntity entity;
                entity.Id = static_cast<uint64_t>( record.id.value() );
                ReadComponent( block.value().to_object().value(), entity.Tile );
                auto loaded = ReadLandscapeTileFile( entity.Tile.HeightFile );
                EXPECT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
                if ( loaded.IsSuccess() )
                    entity.Tile.Heights = loaded.ExtractValue();
                scene.Tiles.push_back( entity );
            }
        }

        LandscapeRoot root;
        root.QuadsPerTile = scene.Root.QuadsPerTile;
        root.SpacingCm    = scene.Root.SpacingCm;
        root.ZScale       = scene.Root.ZScale;
        EXPECT_TRUE( ValidateLandscapeRoot( root ).IsSuccess() );
        for ( const TileEntity& entity : scene.Tiles )
            if ( entity.Tile.Heights )
                EXPECT_TRUE( CheckTileMatchesRoot( *entity.Tile.Heights, root ).IsSuccess() );
        return scene;
    }

    // A directory of this test's own, emptied first: the scratch space is shared with other suites.
    fs::path Workspace( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / "desert_landscape_scene" / name;
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }
} // namespace

// ── 1 ──────────────────────────────────────────────────────────────────────────────────────────────
TEST( LandscapeScene, SaveLoadSaveIsByteIdenticalForTheSceneAndEveryTileFile )
{
    const fs::path dir       = Workspace( "trip" );
    const fs::path scenePath = dir / "Hills.desce";

    LandscapeScene original = TwoByTwo();
    Save( original, scenePath );

    const std::string        firstScene = ReadText( scenePath );
    std::vector<std::string> firstTiles;
    for ( const TileEntity& entity : original.Tiles )
    {
        // Beside the scene, in a directory named after it, by entity id — and NOT inside the JSON.
        EXPECT_EQ( fs::path( entity.Tile.HeightFile ).parent_path(), dir / "Hills_Landscape" );
        EXPECT_EQ( fs::path( entity.Tile.HeightFile ).extension(), ".dlht" );
        firstTiles.push_back( ReadText( entity.Tile.HeightFile ) );
        EXPECT_EQ( firstTiles.back().size(),
                   kLandscapeTileHeaderSize + 2u * 32u * 32u + kLandscapeTileTrailerSize );
    }
    EXPECT_LT( firstScene.size(), 4096u ) << "the samples must not be in the scene text";

    LandscapeScene loaded = Load( scenePath );
    ASSERT_EQ( loaded.Tiles.size(), 4u );
    EXPECT_EQ( loaded.Root.QuadsPerTile, 31u );
    EXPECT_FLOAT_EQ( loaded.Root.SpacingCm, 50.0f );
    EXPECT_FLOAT_EQ( loaded.Root.ZScale, 200.0f );
    for ( size_t index = 0; index < loaded.Tiles.size(); ++index )
    {
        const auto& before = original.Tiles[index];
        const auto& after  = loaded.Tiles[index];
        EXPECT_EQ( after.Id, before.Id );
        EXPECT_EQ( static_cast<uint64_t>( after.Tile.Landscape ), kRootId );
        EXPECT_EQ( after.Tile.TileX, before.Tile.TileX );
        EXPECT_EQ( after.Tile.TileZ, before.Tile.TileZ );
        ASSERT_TRUE( after.Tile.Heights.has_value() );
        EXPECT_EQ( after.Tile.Heights->Samples(), before.Tile.Heights->Samples() ) << "tile " << index;
    }

    Save( loaded, scenePath );
    EXPECT_EQ( ReadText( scenePath ), firstScene );
    for ( size_t index = 0; index < loaded.Tiles.size(); ++index )
        EXPECT_EQ( ReadText( loaded.Tiles[index].Tile.HeightFile ), firstTiles[index] ) << "tile " << index;
}

// ── 2 ──────────────────────────────────────────────────────────────────────────────────────────────
TEST( LandscapeScene, SavingUnderANewNameWritesNewFilesAndLeavesTheOldSceneAlone )
{
    const fs::path dir = Workspace( "save_as" );

    LandscapeScene scene = TwoByTwo();
    Save( scene, dir / "A.desce" );
    const std::string aTile = ReadText( scene.Tiles[0].Tile.HeightFile );
    const fs::path    aPath = scene.Tiles[0].Tile.HeightFile;

    // Edit, then Save As: the edit must land in B's file only.
    scene.Tiles[0].Tile.Heights->SetSample( 3, 4, 12345 );
    Save( scene, dir / "B.desce" );

    EXPECT_EQ( fs::path( scene.Tiles[0].Tile.HeightFile ).parent_path(), dir / "B_Landscape" );
    EXPECT_NE( fs::path( scene.Tiles[0].Tile.HeightFile ), aPath );
    EXPECT_EQ( ReadText( aPath ), aTile ) << "Save As wrote into the old scene's tile file";

    const auto reread = ReadLandscapeTileFile( scene.Tiles[0].Tile.HeightFile );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();
    EXPECT_EQ( reread.GetValue().Sample( 3, 4 ), 12345 );
}

// ── 3 ──────────────────────────────────────────────────────────────────────────────────────────────
TEST( LandscapeScene, ATileOfAnotherSizeThanItsRootIsRefusedByBothNumbers )
{
    LandscapeRoot root;
    root.QuadsPerTile = 63u;

    const auto small   = Terrain( 32u, 0, 0 );
    const auto refused = CheckTileMatchesRoot( small, root );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "32 x 32" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "64 x 64" ), std::string::npos ) << refused.GetError();

    EXPECT_TRUE( CheckTileMatchesRoot( Terrain( 64u, 0, 0 ), root ).IsSuccess() );
}

TEST( LandscapeScene, ARootWhoseTileSizeUEDoesNotOfferIsRefused )
{
    LandscapeRoot root;
    for ( const uint32_t quads : kLandscapeTileQuadsValues )
    {
        root.QuadsPerTile = quads;
        EXPECT_TRUE( ValidateLandscapeRoot( root ).IsSuccess() ) << quads;
    }
    for ( const uint32_t quads : { 0u, 8u, 64u, 256u, 511u } )
    {
        root.QuadsPerTile  = quads;
        const auto refused = ValidateLandscapeRoot( root );
        ASSERT_FALSE( refused.IsSuccess() ) << quads;
        EXPECT_NE( refused.GetError().find( std::to_string( quads ) ), std::string::npos ) << refused.GetError();
    }
}

TEST( LandscapeScene, AFileThatIsNotATileIsRefusedByName )
{
    const fs::path dir  = Workspace( "corrupt" );
    const fs::path file = dir / "broken.dlht";
    {
        std::ofstream out( file, std::ios::binary );
        out << "not a landscape tile";
    }
    const auto refused = ReadLandscapeTileFile( file );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "broken.dlht" ), std::string::npos ) << refused.GetError();

    const auto missing = ReadLandscapeTileFile( dir / "absent.dlht" );
    ASSERT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError().find( "absent.dlht" ), std::string::npos ) << missing.GetError();
}

// ── 4 ──────────────────────────────────────────────────────────────────────────────────────────────
TEST( LandscapeScene, NeighbouringTilesAnswerTheSameHeightOnTheirSharedSeam )
{
    LandscapeRoot root;
    root.Origin            = glm::vec3( -1000.0f, 250.0f, 3000.0f );
    root.QuadsPerTile      = 7u;
    root.SpacingCm         = 100.0f;
    root.ZScale            = 100.0f;
    const uint32_t samples = LandscapeTileSamples( root );

    // Two tiles whose shared column holds the same values, as neighbouring tiles' edges do.
    auto west = Terrain( samples, 0, 0 );
    auto east = Terrain( samples, 1, 0 );
    for ( uint32_t z = 0; z < samples; ++z )
        east.SetSample( 0, z, west.Sample( samples - 1, z ) );

    const LandscapeFrame westFrame = LandscapeTileFrame( root, 0, 0 );
    const LandscapeFrame eastFrame = LandscapeTileFrame( root, 1, 0 );
    EXPECT_FLOAT_EQ( eastFrame.OriginX, westFrame.OriginX + LandscapeTileExtentCm( root ) );
    EXPECT_FLOAT_EQ( eastFrame.OriginZ, westFrame.OriginZ );

    const float seamX = eastFrame.OriginX;
    for ( const float dz : { 0.0f, 130.0f, 355.5f, 700.0f } )
    {
        const float worldZ = westFrame.OriginZ + dz;
        const auto  fromW  = SampleLandscapeHeight( west, westFrame, seamX, worldZ );
        const auto  fromE  = SampleLandscapeHeight( east, eastFrame, seamX, worldZ );
        ASSERT_TRUE( fromW.has_value() && fromE.has_value() ) << dz;
        EXPECT_FLOAT_EQ( *fromW, *fromE ) << "seam at z offset " << dz;
    }

    // And the rectangles the partitioner uses share the SAME edge number.
    EXPECT_EQ( LandscapeTileBounds( root, 0, 0 ).MaxX, LandscapeTileBounds( root, 1, 0 ).MinX );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
