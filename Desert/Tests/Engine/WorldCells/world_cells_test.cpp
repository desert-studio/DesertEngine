// A PARTITIONED WORLD, COOKED INTO CELL FILES AND AN INDEX (Engine/Core/Serialize/WorldCells.hpp, WP8).
//
// WHAT IS ASSERTED:
//
//   1. DETERMINISM. Two cooks of one source are the same files, byte for byte.
//   2. LOCALITY. Editing one record changes exactly its cell's file and the index, and no other file.
//   3. THE ROUND TRIP. The world assembled back from the files holds the source's records (by id) and its
//      scene-wide part; and the cooked source hands the streamer, unit by unit, exactly what the in-memory
//      source does — and that is what the executor activates.
//   4. THE INDEX IS ENOUGH TO DECIDE WITH. Units, ids, the references that cross a unit, the asset closure.
//   5. REFUSALS, BY FILE NAME. A damaged cell, a damaged index, a cell from another cook, another container
//      version, a record without an id, two records with one id.
//   6. THE TOOL. Tools/WorldCook's own RunWorldCook writes a directory, verifies it from disk, and removes what
//      an earlier cook left.

#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <Engine/Assets/ContainerBytes.hpp>

#include <Common/Utilities/Crc32c.hpp>

#include <WorldCookMain.hpp>

#include <gtest/gtest.h>

#include <rflcpp/rfl/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::SceneSerialized;
using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
namespace Cells = Desert::Core::WorldCells;
namespace Rules = Desert::Core::Rules;

namespace
{
    constexpr float kCell = 1000.0f; // 10 m cells

    constexpr std::uint64_t kCameraId   = 1;
    constexpr std::uint64_t kParentId   = 2;
    constexpr std::uint64_t kChildId    = 3;
    constexpr std::uint64_t kShooterId  = 4;
    constexpr std::uint64_t kTargetId   = 5;
    constexpr std::uint64_t kBystander  = 6;
    constexpr std::uint64_t kMaterialId = 0x4D41;
    constexpr std::uint64_t kTextureId  = 0x5445;

    EntityData Record( std::uint64_t id, const char* tag, glm::vec3 translation )
    {
        EntityData data;
        data.id          = Common::UUID( id );
        data.Tag         = tag;
        data.Translation = translation;
        return data;
    }

    void With( EntityData& data, const char* key, const std::string& json )
    {
        const auto block = rfl::json::read<rfl::Generic>( json );
        ASSERT_TRUE( block.has_value() ) << json;
        data.Components[key] = block.value();
    }

    glm::vec3 CellCentre( int column, int row )
    {
        return { ( static_cast<float>( column ) + 0.5f ) * kCell, 0.0f,
                 ( static_cast<float>( row ) + 0.5f ) * kCell };
    }

    // A camera (always-loaded by component), a parent with its child, a shooter observing a target two cells
    // away, a bystander beside the shooter wearing a registered material, and a row of fillers.
    SceneSerialized World()
    {
        SceneSerialized scene;
        scene.SceneName      = "CookMe";
        scene.SceneVersion   = Desert::Core::kSceneVersion;
        scene.UnitVersion    = Desert::Core::kUnitVersion;
        scene.Settings       = rfl::json::read<rfl::Generic>( R"({"Exposure": 1.5})" ).value();
        scene.WorldPartition = WorldPartitionSerialized{ { WorldPartitionGridSerialized{ kCell, 1500.0f } } };

        auto&      records = scene.Entities;
        EntityData camera  = Record( kCameraId, "Camera", { 50.0f, 0.0f, 50.0f } );
        With( camera, "Camera", R"({"IsMainCamera": true})" );
        records.push_back( camera );
        records.push_back( Record( kParentId, "Parent", CellCentre( 5, 1 ) ) );
        EntityData child = Record( kChildId, "Child", { 100.0f, 0.0f, 0.0f } );
        child.parent     = Common::UUID( kParentId );
        records.push_back( child );
        EntityData shooter = Record( kShooterId, "Shooter", CellCentre( 1, 1 ) );
        With( shooter, "Projectile", R"({"Owner": ")" + std::to_string( kTargetId ) + R"("})" );
        records.push_back( shooter );
        records.push_back( Record( kTargetId, "Target", CellCentre( 3, 1 ) ) );
        EntityData bystander = Record( kBystander, "Bystander", CellCentre( 1, 1 ) + glm::vec3( 10.0f, 0, 0 ) );
        With( bystander, "StaticMesh",
              R"({"Primitive": "Cube", "MaterialGuids": [)" + std::to_string( kMaterialId ) + "]}" );
        records.push_back( bystander );
        for ( int column = 0; column < 8; ++column )
            records.push_back(
                 Record( 100 + static_cast<std::uint64_t>( column ), "Filler", CellCentre( column, 2 ) ) );
        return scene;
    }

    Cells::CookedWorld Cook( const SceneSerialized&                        scene,
                             std::span<const Common::Utils::AssetRegistry> registries = {} )
    {
        auto cooked = Cells::CookWorld( scene, registries );
        EXPECT_TRUE( cooked.IsSuccess() ) << ( cooked.IsSuccess() ? "" : cooked.GetError() );
        return cooked.IsSuccess() ? cooked.ExtractValue() : Cells::CookedWorld{};
    }

    std::map<std::string, std::vector<unsigned char>> FilesOf( const Cells::CookedWorld& cooked )
    {
        std::map<std::string, std::vector<unsigned char>> files;
        for ( const auto& file : cooked.Files )
            files[file.Name] = file.Bytes;
        return files;
    }

    Cells::FileReader ReaderOf( const std::map<std::string, std::vector<unsigned char>>& files )
    {
        return [&files]( std::string_view name ) -> Common::ResultStr<std::vector<unsigned char>>
        {
            const auto found = files.find( std::string( name ) );
            if ( found == files.end() )
                return Common::MakeError<std::vector<unsigned char>>( "no file '" + std::string( name ) + "'" );
            return Common::MakeSuccess( found->second );
        };
    }

    Cells::WorldIndex IndexOf( const std::map<std::string, std::vector<unsigned char>>& files )
    {
        auto index =
             Cells::ReadWorldIndex( Cells::kIndexFileName, files.at( std::string( Cells::kIndexFileName ) ) );
        EXPECT_TRUE( index.IsSuccess() ) << ( index.IsSuccess() ? "" : index.GetError() );
        return index.IsSuccess() ? index.ExtractValue() : Cells::WorldIndex{};
    }

    std::string UnitFileOf( const Cells::WorldIndex& index, std::uint64_t id )
    {
        for ( const auto& unit : index.Units )
            for ( const std::uint64_t held : unit.Ids )
                if ( held == id )
                    return unit.File;
        return {};
    }

    std::string AssembleError( const std::map<std::string, std::vector<unsigned char>>& files )
    {
        auto index =
             Cells::ReadWorldIndex( Cells::kIndexFileName, files.at( std::string( Cells::kIndexFileName ) ) );
        if ( !index )
            return index.GetError();
        auto back = Cells::AssembleWorld( index.GetValue(), ReaderOf( files ) );
        return back.IsSuccess() ? std::string() : back.GetError();
    }
} // namespace

// ── 1. Determinism ─────────────────────────────────────────────────────────────────────────────────

TEST( WorldCells, TwoCooksOfOneSourceAreTheSameBytes )
{
    const auto first  = FilesOf( Cook( World() ) );
    const auto second = FilesOf( Cook( World() ) );
    // The always-loaded file, the parent's, the shooter's, the target's, eight fillers' and the index.
    EXPECT_EQ( first.size(), 13u );
    EXPECT_EQ( first, second );
}

// ── 2. Locality ────────────────────────────────────────────────────────────────────────────────────

TEST( WorldCells, EditingOneRecordChangesOnlyItsCellAndTheIndex )
{
    SceneSerialized   source = World();
    const auto        before = FilesOf( Cook( source ) );
    const auto        index  = IndexOf( before );
    const std::string cell   = UnitFileOf( index, kTargetId );
    ASSERT_FALSE( cell.empty() );

    for ( auto& record : source.Entities )
        if ( record.id.has_value() && static_cast<std::uint64_t>( *record.id ) == kTargetId )
            record.Tag = "Target (renamed)";
    const auto after = FilesOf( Cook( source ) );

    ASSERT_EQ( before.size(), after.size() );
    std::vector<std::string> changed;
    for ( const auto& [name, bytes] : before )
        if ( after.at( name ) != bytes )
            changed.push_back( name );
    EXPECT_EQ( changed, ( std::vector<std::string>{ cell, std::string( Cells::kIndexFileName ) } ) );
}

// ── 3. The round trip ─────────────────────────────────────────────────────────────────────────────

TEST( WorldCells, TheWorldAssembledFromItsCellsHoldsTheSourcesRecords )
{
    const SceneSerialized source = World();
    const auto            files  = FilesOf( Cook( source ) );
    const auto            index  = IndexOf( files );
    auto                  back   = Cells::AssembleWorld( index, ReaderOf( files ) );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();

    EXPECT_EQ( Cells::CanonicalRecords( back.GetValue() ), Cells::CanonicalRecords( source ) );
    EXPECT_EQ( back.GetValue().SceneName, source.SceneName );
    EXPECT_EQ( back.GetValue().SceneVersion, source.SceneVersion );
    EXPECT_EQ( back.GetValue().UnitVersion, source.UnitVersion );
    EXPECT_EQ( rfl::json::write( back.GetValue().Settings ), rfl::json::write( source.Settings ) );
    EXPECT_EQ( rfl::json::write( back.GetValue().WorldPartition ), rfl::json::write( source.WorldPartition ) );
}

namespace
{
    // Records which records each activation of a unit brought, and nothing else.
    struct RecordingWorld final : Rules::ResidencyWorld
    {
        std::map<std::size_t, std::vector<std::size_t>> Activated;

        Common::BoolResultStr Activate( std::size_t unit, std::span<const std::size_t> records ) override
        {
            Activated[unit].assign( records.begin(), records.end() );
            return Common::MakeSuccess( true );
        }
        void Destroy( std::span<const std::size_t> ) override
        {
        }
    };
} // namespace

// The three readers of "a unit's records" agree: what the executor activates, what the in-memory source hands
// the streamer today, and what the cooked source will hand it in WP9.
TEST( WorldCells, TheCookedSourceHandsTheStreamerWhatTheMemorySourceDoes )
{
    const SceneSerialized source = World();
    const auto            files  = FilesOf( Cook( source ) );
    const auto            index  = IndexOf( files );
    const auto            plan   = Rules::PlanWorldPartition( source.Entities, *source.WorldPartition );
    ASSERT_EQ( index.Units.size(), Rules::ResidencyUnitCount( plan ) );

    Rules::MemoryCellSource memory( plan, source.Entities );
    Cells::CookedCellSource cooked( index, ReaderOf( files ) );
    for ( std::size_t unit = 0; unit < index.Units.size(); ++unit )
    {
        auto fromMemory = memory.UnitRecords( unit );
        auto fromFiles  = cooked.UnitRecords( unit );
        ASSERT_TRUE( fromMemory.IsSuccess() ) << fromMemory.GetError();
        ASSERT_TRUE( fromFiles.IsSuccess() ) << fromFiles.GetError();
        EXPECT_EQ( rfl::json::write( fromFiles.GetValue() ), rfl::json::write( fromMemory.GetValue() ) )
             << index.Units[unit].Name;
        std::vector<std::uint64_t> ids;
        for ( const auto& record : fromFiles.GetValue() )
            ids.push_back( static_cast<std::uint64_t>( *record.id ) );
        EXPECT_EQ( ids, index.Units[unit].Ids ) << index.Units[unit].Name;
    }

    // The executor's activation of every unit, from nowhere near the world to its middle.
    RecordingWorld               world;
    const Rules::StreamingSource far_away{ glm::vec3( -1.0e6f, 0.0f, -1.0e6f ) };
    auto begun = Rules::ResidencyExecutor::Begin( plan, *source.WorldPartition, source.Entities,
                                                  Rules::ResidencySettings{}, std::span( &far_away, 1 ), world );
    ASSERT_TRUE( begun.IsSuccess() ) << begun.GetError();
    auto executor = begun.ExtractValue();
    for ( int frame = 0; frame < 64; ++frame )
    {
        const Rules::StreamingSource middle{ CellCentre( 3, 1 ) };
        auto                         tick = executor.Tick( std::span( &middle, 1 ), frame / 64.0, world );
        ASSERT_TRUE( tick.IsSuccess() ) << tick.GetError();
    }
    ASSERT_FALSE( world.Activated.empty() );
    for ( const auto& [unit, records] : world.Activated )
        EXPECT_EQ( records, Rules::ResidencyUnitMembers( plan, unit ) ) << index.Units[unit].Name;
}

// ── 4. The index is enough to decide with ─────────────────────────────────────────────────────────

TEST( WorldCells, TheIndexListsTheReferencesThatCrossAUnitAndOnlyThose )
{
    const auto index = IndexOf( FilesOf( Cook( World() ) ) );
    ASSERT_EQ( index.References.size(), 1u );
    const auto& reference = index.References.front();
    EXPECT_EQ( reference.From, kShooterId );
    EXPECT_EQ( reference.To, kTargetId );
    EXPECT_EQ( reference.Component, "Projectile" );
    EXPECT_EQ( reference.Field, "Owner" );
    EXPECT_EQ( index.Units.at( reference.FromUnit ).File, UnitFileOf( index, kShooterId ) );
    EXPECT_EQ( index.Units.at( reference.ToUnit ).File, UnitFileOf( index, kTargetId ) );

    // The parent and its child are one composite: one unit, one file.
    EXPECT_EQ( UnitFileOf( index, kParentId ), UnitFileOf( index, kChildId ) );
    // The camera is always-loaded, and its unit says why.
    EXPECT_EQ( UnitFileOf( index, kCameraId ), Cells::kAlwaysLoadedFileName );
    EXPECT_EQ( index.Units.front().Reason, std::optional<std::string>( "Component" ) );
    EXPECT_EQ( index.Records, World().Entities.size() );
    EXPECT_FALSE( index.AssetClosureKnown );
}

TEST( WorldCells, AUnitsAssetsAreWhatItsRecordsNameAndWhatThoseDependOn )
{
    Common::Utils::AssetRegistry      registry;
    Common::Utils::AssetRegistryEntry material;
    material.Key          = "assets:Materials/M_Brick.demat";
    material.Kind         = "Material";
    material.Identity     = kMaterialId;
    material.Dependencies = { kTextureId };
    Common::Utils::AssetRegistryEntry texture;
    texture.Key      = "assets:Textures/T_Brick.tex";
    texture.Kind     = "Texture";
    texture.Identity = kTextureId;
    ASSERT_TRUE( registry.Insert( material ).IsSuccess() );
    ASSERT_TRUE( registry.Insert( texture ).IsSuccess() );
    const std::vector<Common::Utils::AssetRegistry> registries{ registry };

    const auto index = IndexOf( FilesOf( Cook( World(), registries ) ) );
    EXPECT_TRUE( index.AssetClosureKnown );
    const std::string bystanderCell = UnitFileOf( index, kBystander );
    for ( const auto& unit : index.Units )
    {
        if ( unit.File == bystanderCell )
            EXPECT_EQ( unit.Assets, ( std::vector<std::string>{ material.Key, texture.Key } ) ) << unit.Name;
        else
            EXPECT_TRUE( unit.Assets.empty() ) << unit.Name;
    }
}

// ── 5. Refusals, by file name ─────────────────────────────────────────────────────────────────────

TEST( WorldCells, ADamagedCellIsRefusedByName )
{
    auto              files = FilesOf( Cook( World() ) );
    const std::string cell  = UnitFileOf( IndexOf( files ), kTargetId );
    files[cell][20] ^= 0x01;
    const std::string error = AssembleError( files );
    EXPECT_NE( error.find( cell ), std::string::npos ) << error;
    EXPECT_NE( error.find( "damaged" ), std::string::npos ) << error;
}

TEST( WorldCells, ADamagedIndexIsRefusedByName )
{
    auto files = FilesOf( Cook( World() ) );
    files[std::string( Cells::kIndexFileName )][30] ^= 0x01;
    const std::string error = AssembleError( files );
    EXPECT_NE( error.find( Cells::kIndexFileName ), std::string::npos ) << error;
    EXPECT_NE( error.find( "damaged" ), std::string::npos ) << error;
}

TEST( WorldCells, ACellFromAnotherCookIsRefusedAsStale )
{
    auto            files   = FilesOf( Cook( World() ) );
    SceneSerialized edited  = World();
    edited.Entities[4].Tag  = "Target, edited after the cook";
    const auto        newer = FilesOf( Cook( edited ) );
    const std::string cell  = UnitFileOf( IndexOf( files ), kTargetId );
    files[cell]             = newer.at( cell ); // intact on its own, and not the file this index lists
    const std::string error = AssembleError( files );
    EXPECT_NE( error.find( cell ), std::string::npos ) << error;
    EXPECT_NE( error.find( "stale" ), std::string::npos ) << error;
}

TEST( WorldCells, AnotherContainerVersionIsRefusedByName )
{
    auto  files = FilesOf( Cook( World() ) );
    auto& bytes = files[std::string( Cells::kIndexFileName )];
    // A well-formed file of another version: the number changed and the checksum made true again.
    bytes[4] = static_cast<unsigned char>( Cells::kContainerVersion + 1 );
    bytes.resize( bytes.size() - 4 );
    Desert::Assets::WriteU32( bytes, Common::Utils::Crc32c( bytes.data(), bytes.size() ) );
    const std::string error = AssembleError( files );
    EXPECT_NE( error.find( Cells::kIndexFileName ), std::string::npos ) << error;
    EXPECT_NE( error.find( "version" ), std::string::npos ) << error;
}

TEST( WorldCells, ARecordWithoutAnIdOrWithAnotherRecordsIdIsRefused )
{
    SceneSerialized nameless = World();
    nameless.Entities[3].id.reset();
    auto refused = Cells::CookWorld( nameless, {} );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "'Shooter'" ), std::string::npos ) << refused.GetError();

    SceneSerialized twins = World();
    twins.Entities[4].id  = Common::UUID( kShooterId );
    auto second           = Cells::CookWorld( twins, {} );
    ASSERT_FALSE( second.IsSuccess() );
    EXPECT_NE( second.GetError().find( "'Target'" ), std::string::npos ) << second.GetError();

    SceneSerialized flat = World();
    flat.WorldPartition.reset();
    EXPECT_FALSE( Cells::CookWorld( flat, {} ).IsSuccess() );
}

// ── 6. The tool ────────────────────────────────────────────────────────────────────────────────────

TEST( WorldCells, TheToolCooksADirectoryVerifiesItFromDiskAndRemovesWhatAnEarlierCookLeft )
{
    namespace fs = std::filesystem;
    const fs::path root =
         fs::temp_directory_path() /
         ( "WorldCellsSuite_" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::remove_all( root );
    fs::create_directories( root / "cooked" );
    const fs::path source = root / "CookMe.desce";
    {
        std::ofstream file( source, std::ios::binary );
        file << rfl::json::write( World() );
    }
    // Left by an earlier cook of a bigger world: a cell this world no longer has.
    {
        std::ofstream stale( root / "cooked" / "L0_99_99.dwcell", std::ios::binary );
        stale << "old";
    }
    std::ofstream( root / "cooked" / "notes.txt" ) << "not the cook's";

    std::ostringstream out;
    std::ostringstream err;
    const int          code = Desert::WorldCook::RunWorldCook(
         { source.string(), "--out", ( root / "cooked" ).string(), "--no-registry", "--verify" }, out, err );
    EXPECT_EQ( code, 0 ) << err.str();
    EXPECT_NE( out.str().find( "equal to the source" ), std::string::npos ) << out.str();
    EXPECT_NE( out.str().find( "stale removed: 1" ), std::string::npos ) << out.str();
    EXPECT_FALSE( fs::exists( root / "cooked" / "L0_99_99.dwcell" ) );
    EXPECT_TRUE( fs::exists( root / "cooked" / "notes.txt" ) );
    EXPECT_TRUE( fs::exists( root / "cooked" / std::string( Cells::kIndexFileName ) ) );

    // No registry and no --no-registry: refused, not defaulted.
    std::ostringstream quiet;
    std::ostringstream said;
    EXPECT_EQ( Desert::WorldCook::RunWorldCook( { source.string(), "--out", ( root / "cooked" ).string() }, quiet,
                                                said ),
               2 );
    EXPECT_NE( said.str().find( "--no-registry" ), std::string::npos ) << said.str();
    fs::remove_all( root );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
