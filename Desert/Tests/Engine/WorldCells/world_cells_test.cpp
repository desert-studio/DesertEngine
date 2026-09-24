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
//   5. REFUSALS, BY FILE NAME. A damaged cell payload, a damaged index header, a cell from another cook,
//      another world format version, a record without an id, two records with one id.
//   7. THE ENVELOPE (AF2). Every cooked file is an AF1 asset envelope whose header, read without the body,
//      names its kind; reading and planning from the index reads no cell.
//   6. THE TOOL. Tools/WorldCook's own RunWorldCook writes a directory, verifies it from disk, and removes what
//      an earlier cook left.

#include <Engine/Core/Serialize/WorldCellLoader.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>

#include <WorldCookMain.hpp>

#include <gtest/gtest.h>

#include <rflcpp/rfl/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
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

namespace
{
    namespace CC = Common::Content;

    constexpr CC::SubsystemVersion kWorldFormat[] = { { Cells::kWorldFormatTag, Cells::kWorldFormatVersion } };

    CC::EnvelopeHeader HeaderOf( const std::vector<unsigned char>& bytes )
    {
        auto header = CC::ReadEnvelopeHeader( std::as_bytes( std::span( bytes ) ), { kWorldFormat } );
        EXPECT_TRUE( header.IsSuccess() ) << ( header.IsSuccess() ? "" : header.GetError() );
        return header.IsSuccess() ? header.ExtractValue() : CC::EnvelopeHeader{};
    }

    // The middle byte of the file's Payload section: past the header, so only the section hash can see it.
    std::size_t PayloadByte( const std::vector<unsigned char>& bytes )
    {
        const auto payload = HeaderOf( bytes ).Find( CC::EnvelopeSection::Payload );
        EXPECT_TRUE( payload.has_value() );
        return payload ? static_cast<std::size_t>( payload->Offset + payload->Size / 2 ) : 0;
    }
} // namespace

TEST( WorldCells, ADamagedCellIsRefusedByName )
{
    auto              files = FilesOf( Cook( World() ) );
    const std::string cell  = UnitFileOf( IndexOf( files ), kTargetId );
    files[cell][PayloadByte( files[cell] )] ^= 0x01;
    const std::string error = AssembleError( files );
    EXPECT_NE( error.find( cell ), std::string::npos ) << error;
    EXPECT_NE( error.find( "hash" ), std::string::npos ) << error;
}

TEST( WorldCells, ADamagedIndexIsRefusedByName )
{
    auto files = FilesOf( Cook( World() ) );
    files[std::string( Cells::kIndexFileName )][30] ^= 0x01; // inside the header: the header CRC sees it
    const std::string error = AssembleError( files );
    EXPECT_NE( error.find( Cells::kIndexFileName ), std::string::npos ) << error;
    EXPECT_NE( error.find( "CRC" ), std::string::npos ) << error;
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

TEST( WorldCells, AnotherWorldFormatVersionIsRefusedByName )
{
    // A well-formed envelope of another world format version, newer and older: rewritten through the envelope
    // writer, so every checksum is true again and only the number differs.
    for ( const std::uint32_t version : { Cells::kWorldFormatVersion + 1, Cells::kWorldFormatVersion - 1 } )
    {
        auto                       files = FilesOf( Cook( World() ) );
        auto&                      bytes = files[std::string( Cells::kIndexFileName )];
        const CC::SubsystemVersion any[] = { { Cells::kWorldFormatTag, 1000 } };
        auto envelope                    = CC::ReadAssetEnvelope( std::as_bytes( std::span( bytes ) ), { any } );
        ASSERT_TRUE( envelope ) << envelope.GetError();
        CC::AssetEnvelope edited = envelope.ExtractValue();
        edited.Asset.Subsystems  = { CC::SubsystemVersion{ Cells::kWorldFormatTag, version } };
        auto rewritten           = CC::WriteAssetEnvelope( edited );
        ASSERT_TRUE( rewritten ) << rewritten.GetError();
        bytes.assign( reinterpret_cast<const unsigned char*>( rewritten.GetValue().data() ),
                      reinterpret_cast<const unsigned char*>( rewritten.GetValue().data() ) +
                           rewritten.GetValue().size() );
        const std::string error = AssembleError( files );
        EXPECT_NE( error.find( Cells::kIndexFileName ), std::string::npos ) << version << ": " << error;
        EXPECT_NE( error.find( "version" ), std::string::npos ) << version << ": " << error;
    }
}

// ── 7. The envelope ──────────────────────────────────────────────────────────────────────────────────

TEST( WorldCells, ACookedFileNamesItsKindInItsHeader )
{
    // The census and the cook name the files alike.
    EXPECT_EQ( CC::KindSpec( CC::ContentKind::WorldCell ).Extension, Cells::kCellExtension );
    EXPECT_EQ( CC::KindSpec( CC::ContentKind::WorldIndex ).Extension,
               std::filesystem::path( Cells::kIndexFileName ).extension().string() );

    const auto              files = FilesOf( Cook( World() ) );
    std::set<std::uint64_t> guids;
    for ( const auto& [name, bytes] : files )
    {
        const bool isIndex = name == Cells::kIndexFileName;
        const auto whole   = HeaderOf( bytes );
        ASSERT_GT( whole.HeaderSize, 0u ) << name;
        // The header prefix ALONE, the body cut off and then a damaged body: the answer cannot depend on it.
        std::vector<unsigned char> prefix( bytes.begin(), bytes.begin() + whole.HeaderSize );
        const auto                 header = HeaderOf( prefix );
        EXPECT_EQ( header.Asset, whole.Asset ) << name;
        EXPECT_EQ( header.Asset.Kind, isIndex ? CC::ContentKind::WorldIndex : CC::ContentKind::WorldCell ) << name;
        EXPECT_EQ( header.Asset.Subsystems, ( std::vector<CC::SubsystemVersion>{
                                                 { Cells::kWorldFormatTag, Cells::kWorldFormatVersion } } ) )
             << name;
        EXPECT_TRUE( header.Asset.Dependencies.empty() ) << name;
        EXPECT_TRUE( header.Find( CC::EnvelopeSection::Payload ).has_value() ) << name;
        guids.insert( header.Asset.Guid.Hi );
    }
    EXPECT_EQ( guids.size(), files.size() ) << "two cooked files share one GUID";

    // The kind is checked on read, not only written: a cell handed in as the index is refused by its kind.
    const std::string cell  = UnitFileOf( IndexOf( files ), kTargetId );
    auto              wrong = Cells::ReadWorldIndex( cell, files.at( cell ) );
    ASSERT_FALSE( wrong.IsSuccess() );
    EXPECT_NE( wrong.GetError().find( "WorldCell asset, expected a WorldIndex" ), std::string::npos )
         << wrong.GetError();
}

// THE WP8 CONTAINER IS GONE, NOT KEPT BESIDE THE ENVELOPE: no source the engine, the tools or the editor build
// spells its magic in either form. The needles are assembled here so this file does not match itself.
TEST( WorldCells, NoSourceSpellsTheRetiredWp8Magic )
{
    namespace fs  = std::filesystem;
    fs::path root = ".";
    for ( int up = 0; up < 8 && !fs::exists( root / "Editor" / "Desert.deproj" ); ++up )
        root /= "..";
    ASSERT_TRUE( fs::exists( root / "Editor" / "Desert.deproj" ) ) << "repository root not found";

    std::vector<std::string> needles;
    for ( const std::string magic : { std::string( "DW" ) + "CL", std::string( "DW" ) + "IX" } )
    {
        needles.push_back( '"' + magic + '"' );
        needles.push_back( std::string( "'" ) + magic[0] + "', '" + magic[1] + "', '" + magic[2] + "', '" +
                           magic[3] + "'" );
    }
    std::size_t       scanned       = 0;
    bool              sawWorldCells = false;
    const char* const trees[]       = { "Desert/Desert/Source", "Desert/Common/Source", "Tools", "Editor/Source",
                                        "Runtime/Source" };
    for ( const char* tree : trees )
    {
        if ( !fs::exists( root / tree ) )
            continue;
        for ( const auto& entry : fs::recursive_directory_iterator( root / tree ) )
        {
            const std::string extension = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( extension != ".cpp" && extension != ".hpp" && extension != ".h" ) )
                continue;
            std::ifstream     file( entry.path(), std::ios::binary );
            std::stringstream text;
            text << file.rdbuf();
            ++scanned;
            sawWorldCells = sawWorldCells || entry.path().filename() == "WorldCells.cpp";
            for ( const std::string& needle : needles )
                EXPECT_EQ( text.str().find( needle ), std::string::npos )
                     << entry.path().string() << " spells the retired WP8 magic " << needle;
        }
    }
    // An instrument that read nothing answers "clean" too: it must have read the file the magic lived in.
    EXPECT_TRUE( sawWorldCells ) << "the census never read WorldCells.cpp";
    EXPECT_GT( scanned, 500u );
}

TEST( WorldCells, TheIndexIsReadAndPlannedWithoutReadingACell )
{
    const auto  files = FilesOf( Cook( World() ) );
    std::size_t reads = 0;
    const auto  inner = ReaderOf( files );
    const auto  index = IndexOf( files );
    ASSERT_TRUE( Cells::PlanFromIndex( index ).IsSuccess() );
    Cells::CookedCellSource source( index,
                                    [&]( std::string_view name )
                                    {
                                        ++reads;
                                        return inner( name );
                                    } );
    EXPECT_EQ( reads, 0u ) << "the index and its plan were enough, and a cell was read anyway";
    // The counter is live: asking for a unit's records is what reads its cell.
    ASSERT_TRUE( source.UnitRecords( 0 ).IsSuccess() );
    EXPECT_EQ( reads, 1u );
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

// ── WP9: a cooked world streamed without its records ────────────────────────────────────────────────

namespace
{
    std::uint64_t IdOf( const EntityData& record )
    {
        return static_cast<std::uint64_t>( *record.id );
    }

    // The ids a residency executor holds live, by the record list it was begun with.
    std::set<std::uint64_t> LiveIds( const Rules::ResidencyExecutor&   executor,
                                     const std::vector<std::uint64_t>& ids )
    {
        std::set<std::uint64_t> live;
        for ( std::size_t record = 0; record < ids.size(); ++record )
            if ( executor.IsLive( record ) )
                live.insert( ids[record] );
        return live;
    }
} // namespace

// The runtime plans from the index alone; its plan must name the same units, cells and members (by id) as the
// planner's over the source records — the query that decides residency reads nothing else.
TEST( WorldCells, ThePlanFromTheIndexIsThePlannersPlan )
{
    const SceneSerialized source = World();
    const auto            files  = FilesOf( Cook( source ) );
    const auto            index  = IndexOf( files );
    const auto            plan   = Rules::PlanWorldPartition( source.Entities, *source.WorldPartition );
    auto                  from   = Cells::PlanFromIndex( index );
    ASSERT_TRUE( from.IsSuccess() ) << from.GetError();
    const Cells::IndexedWorld& indexed = from.GetValue();

    ASSERT_EQ( Rules::ResidencyUnitCount( indexed.Plan ), Rules::ResidencyUnitCount( plan ) );
    ASSERT_EQ( indexed.Plan.AlwaysLoaded.size(), plan.AlwaysLoaded.size() );
    EXPECT_EQ( indexed.RecordIds.size(), source.Entities.size() );
    for ( std::size_t unit = 0; unit < Rules::ResidencyUnitCount( plan ); ++unit )
    {
        EXPECT_EQ( Rules::DescribeResidencyUnit( indexed.Plan, unit ),
                   Rules::DescribeResidencyUnit( plan, unit ) );
        std::vector<std::uint64_t> planned;
        for ( const std::size_t record : Rules::ResidencyUnitMembers( plan, unit ) )
            planned.push_back( IdOf( source.Entities[record] ) );
        std::vector<std::uint64_t> indexedIds;
        for ( const std::size_t record : Rules::ResidencyUnitMembers( indexed.Plan, unit ) )
            indexedIds.push_back( indexed.RecordIds[record] );
        EXPECT_EQ( indexedIds, planned ) << index.Units[unit].Name;
    }
    // The shooter observes the target two cells away: the one crossing observation.
    ASSERT_EQ( indexed.Observations.size(), 1u );
    EXPECT_EQ( indexed.RecordIds[indexed.Observations[0].first], kShooterId );
    EXPECT_EQ( indexed.RecordIds[indexed.Observations[0].second], kTargetId );

    for ( const glm::vec3 at :
          { CellCentre( 0, 0 ), CellCentre( 3, 1 ), CellCentre( 7, 2 ), glm::vec3( -5.0e4f ) } )
    {
        const Rules::StreamingSource where{ at };
        auto planned = Rules::QueryStreamingCells( plan, *source.WorldPartition, std::span( &where, 1 ) );
        auto indexedWish =
             Rules::QueryStreamingCells( indexed.Plan, index.WorldPartition, std::span( &where, 1 ) );
        ASSERT_TRUE( planned.IsSuccess() && indexedWish.IsSuccess() );
        EXPECT_EQ( indexedWish.GetValue().AlwaysLoaded, planned.GetValue().AlwaysLoaded );
        EXPECT_EQ( rfl::json::write( indexedWish.GetValue().Cells ),
                   rfl::json::write( planned.GetValue().Cells ) );
    }
}

// An index the runtime cannot plan from is refused by name, not planned wrongly.
TEST( WorldCells, AnIndexOutOfCellOrderIsRefused )
{
    auto index = IndexOf( FilesOf( Cook( World() ) ) );
    ASSERT_GE( index.Units.size(), 3u );
    std::swap( index.Units[index.Units.size() - 1], index.Units[index.Units.size() - 2] );
    auto from = Cells::PlanFromIndex( index );
    ASSERT_FALSE( from.IsSuccess() );
    EXPECT_NE( from.GetError().find( "out of (level, X, Z) order" ), std::string::npos ) << from.GetError();
}

// The start of a cooked world: only its always-loaded records, and the scene-wide part.
TEST( WorldCells, TheAlwaysLoadedPartIsTheAlwaysLoadedUnitsOnly )
{
    const SceneSerialized source = World();
    const auto            files  = FilesOf( Cook( source ) );
    const auto            index  = IndexOf( files );
    auto                  start  = Cells::AssembleAlwaysLoaded( index, ReaderOf( files ) );
    ASSERT_TRUE( start.IsSuccess() ) << start.GetError();
    ASSERT_EQ( start.GetValue().Entities.size(), 1u ); // the camera
    EXPECT_EQ( IdOf( start.GetValue().Entities[0] ), kCameraId );
    EXPECT_EQ( start.GetValue().SceneName, source.SceneName );
    EXPECT_EQ( rfl::json::write( start.GetValue().WorldPartition ), rfl::json::write( source.WorldPartition ) );
}

// THE TWO BEGINNINGS STREAM THE SAME WORLD. A game begins with only the always-loaded records and reads every
// cell; the editor begins with every record and destroys what is not wanted. After the same flight the same
// ids are live, frame by frame, once the cooked world's reads have landed.
TEST( WorldCells, ACookedBeginningStreamsWhatTheWholeBeginningStreams )
{
    const SceneSerialized source = World();
    const auto            files  = FilesOf( Cook( source ) );
    const auto            index  = IndexOf( files );
    const auto            plan   = Rules::PlanWorldPartition( source.Entities, *source.WorldPartition );
    auto                  from   = Cells::PlanFromIndex( index );
    ASSERT_TRUE( from.IsSuccess() ) << from.GetError();
    Cells::IndexedWorld indexed = from.ExtractValue();

    std::vector<std::uint64_t> sourceIds;
    for ( const auto& record : source.Entities )
        sourceIds.push_back( IdOf( record ) );

    RecordingWorld               whole;
    RecordingWorld               cooked;
    const Rules::StreamingSource start{ CellCentre( 0, 0 ) };
    auto wholeBegun  = Rules::ResidencyExecutor::Begin( plan, *source.WorldPartition, source.Entities,
                                                        Rules::ResidencySettings{}, std::span( &start, 1 ), whole );
    auto cookedBegun = Rules::ResidencyExecutor::BeginFromAlwaysLoaded(
         indexed.Plan, index.WorldPartition, Rules::ResidencySettings{}, indexed.RecordIds.size(),
         indexed.Observations );
    ASSERT_TRUE( wholeBegun.IsSuccess() ) << wholeBegun.GetError();
    ASSERT_TRUE( cookedBegun.IsSuccess() ) << cookedBegun.GetError();
    auto wholeRun  = wholeBegun.ExtractValue();
    auto cookedRun = cookedBegun.ExtractValue();
    EXPECT_EQ( LiveIds( cookedRun, indexed.RecordIds ), std::set<std::uint64_t>{ kCameraId } );

    const glm::vec3 route[] = { CellCentre( 0, 0 ), CellCentre( 3, 1 ), CellCentre( 7, 2 ), CellCentre( 1, 1 ) };
    int             frame   = 0;
    for ( const glm::vec3& at : route )
    {
        const Rules::StreamingSource where{ at };
        for ( int step = 0; step < 32; ++step, ++frame )
        {
            auto a = wholeRun.Tick( std::span( &where, 1 ), frame / 60.0, whole );
            auto b = cookedRun.Tick( std::span( &where, 1 ), frame / 60.0, cooked );
            ASSERT_TRUE( a.IsSuccess() ) << a.GetError();
            ASSERT_TRUE( b.IsSuccess() ) << b.GetError();
        }
        EXPECT_EQ( LiveIds( cookedRun, indexed.RecordIds ), LiveIds( wholeRun, sourceIds ) )
             << "at (" << at.x << ", " << at.z << ")";
    }
}

namespace
{
    // A cell source whose every read waits until the test opens the gate: a read that blocked the main thread
    // would hang the test instead of passing it.
    struct GatedSource final : Rules::WorldCellSource
    {
        std::shared_future<void> Gate;
        bool                     Fail = false;

        Common::ResultStr<std::vector<EntityData>> UnitRecords( std::size_t unit ) const override
        {
            Gate.wait_for(
                 std::chrono::seconds( 3 ) ); // bounded, so a loader that blocked reads as slow, not as a hang
            if ( Fail )
                return Common::MakeError<std::vector<EntityData>>( "unit " + std::to_string( unit ) +
                                                                   ": the disk said no" );
            return Common::MakeSuccess( std::vector<EntityData>{ Record( 1000 + unit, "Read", {} ) } );
        }
    };

    std::vector<Rules::LoadOutcome> WaitForOutcomes( Desert::Core::WorldCellLoader& loader, std::size_t count )
    {
        std::vector<Rules::LoadOutcome> all;
        const auto                      deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
        while ( all.size() < count && std::chrono::steady_clock::now() < deadline )
        {
            for ( auto& outcome : loader.TakeFinished() )
                all.push_back( std::move( outcome ) );
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return all;
    }
} // namespace

// THE MAIN THREAD DOES NOT WAIT FOR A CELL. Start and TakeFinished return while every read is still blocked on a
// worker; the outcomes arrive only after the reads end, and each carries the ticket it was started with.
TEST( WorldCells, TheLoaderNeverBlocksTheMainThreadOnARead )
{
    std::promise<void> open;
    auto               source = std::make_shared<GatedSource>();
    source->Gate              = open.get_future().share();
    Desert::Core::WorldCellLoader loader( source );

    const auto before = std::chrono::steady_clock::now();
    loader.Start( 3, 11 );
    loader.Start( 5, 12 );
    const auto nothingYet = loader.TakeFinished();
    const auto elapsed    = std::chrono::steady_clock::now() - before;
    EXPECT_TRUE( nothingYet.empty() );
    EXPECT_EQ( loader.InFlight(), 2u );
    EXPECT_EQ( loader.Records( 3 ), nullptr );
    EXPECT_LT( elapsed, std::chrono::milliseconds( 500 ) ); // the reads are blocked forever until the gate opens

    open.set_value();
    auto done = WaitForOutcomes( loader, 2 );
    ASSERT_EQ( done.size(), 2u );
    std::map<std::size_t, std::uint64_t> tickets;
    for ( const auto& outcome : done )
    {
        EXPECT_TRUE( outcome.Ok ) << outcome.Reason;
        tickets[outcome.Unit] = outcome.Ticket;
    }
    EXPECT_EQ( tickets, ( std::map<std::size_t, std::uint64_t>{ { 3, 11 }, { 5, 12 } } ) );
    ASSERT_NE( loader.Records( 5 ), nullptr );
    EXPECT_EQ( IdOf( loader.Records( 5 )->at( 0 ) ), 1005u );
    loader.Unload( 5 );
    EXPECT_EQ( loader.Records( 5 ), nullptr );
    EXPECT_EQ( loader.InFlight(), 0u );
}

// A cancelled read reports nothing; a failed one reports the source's reason, for StepResidency's retry.
TEST( WorldCells, TheLoaderDropsACancelledReadAndReportsAFailedOne )
{
    std::promise<void> open;
    auto               source = std::make_shared<GatedSource>();
    source->Gate              = open.get_future().share();
    source->Fail              = true;
    Desert::Core::WorldCellLoader loader( source );
    loader.Start( 1, 7 );
    loader.Start( 2, 8 );
    loader.Cancel( 1, 7 );
    open.set_value();
    auto done = WaitForOutcomes( loader, 1 );
    ASSERT_EQ( done.size(), 1u );
    EXPECT_EQ( done[0].Unit, 2u );
    EXPECT_FALSE( done[0].Ok );
    EXPECT_NE( done[0].Reason.find( "the disk said no" ), std::string::npos ) << done[0].Reason;
    // The cancelled read finished too, and was dropped rather than left in flight.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
    while ( loader.InFlight() > 0 && std::chrono::steady_clock::now() < deadline )
    {
        EXPECT_TRUE( loader.TakeFinished().empty() );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    EXPECT_EQ( loader.InFlight(), 0u );
    EXPECT_EQ( loader.Records( 1 ), nullptr );
}

// A COOKED WORLD READ THROUGH A MOUNTED PAK, the way a game reads it: the files under the scene's cooked world
// directory as pak keys, read by path through the VFS, on the loader's workers.
TEST( WorldCells, ACookedWorldIsReadThroughAMountedPak )
{
    namespace fs                 = std::filesystem;
    const SceneSerialized source = World();
    const auto            cooked = Cook( source );
    const fs::path        dir    = fs::temp_directory_path() / "DesertWorldCellsPak";
    fs::remove_all( dir );
    fs::create_directories( dir );
    const std::string worldDir = Cells::CookedWorldDirectory( "Worlds/CookMe.desce" );
    EXPECT_EQ( worldDir, "Worlds/CookMe.dwworld/" );
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        for ( const auto& file : cooked.Files )
            ASSERT_TRUE( writer.AddData( worldDir + file.Name, file.Bytes.data(), file.Bytes.size() ) );
        ASSERT_GT( writer.Finalize(), 0u );
    }
    const auto mounted = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();
    // No loose file: the only place these bytes exist is the archive.
    ASSERT_FALSE( fs::exists( dir / worldDir ) );

    const std::string       root   = ( dir / worldDir ).generic_string();
    const Cells::FileReader reader = [root]( std::string_view name )
    {
        auto read = Common::Utils::FileSystem::ReadByteFileContent( root + std::string( name ) );
        if ( !read )
            return Common::MakeError<std::vector<unsigned char>>( read.GetError() );
        return Common::MakeSuccess( read.ExtractValue() );
    };
    auto indexBytes = reader( Cells::kIndexFileName );
    ASSERT_TRUE( indexBytes.IsSuccess() ) << indexBytes.GetError();
    auto index = Cells::ReadWorldIndex( Cells::kIndexFileName, indexBytes.GetValue() );
    ASSERT_TRUE( index.IsSuccess() ) << index.GetError();

    const auto                    plan = Rules::PlanWorldPartition( source.Entities, *source.WorldPartition );
    Rules::MemoryCellSource       memory( plan, source.Entities );
    Desert::Core::WorldCellLoader loader( std::make_shared<Cells::CookedCellSource>( index.GetValue(), reader ) );
    const std::size_t             units = index.GetValue().Units.size();
    for ( std::size_t unit = 0; unit < units; ++unit )
        loader.Start( unit, 100 + unit );
    auto done = WaitForOutcomes( loader, units );
    ASSERT_EQ( done.size(), units );
    for ( const auto& outcome : done )
    {
        ASSERT_TRUE( outcome.Ok ) << outcome.Reason;
        ASSERT_NE( loader.Records( outcome.Unit ), nullptr );
        const auto expected = memory.UnitRecords( outcome.Unit );
        ASSERT_TRUE( expected.IsSuccess() ) << expected.GetError();
        EXPECT_EQ( rfl::json::write( *loader.Records( outcome.Unit ) ), rfl::json::write( expected.GetValue() ) );
    }
    Common::Utils::VFS::Unmount();
    fs::remove_all( dir );
}
