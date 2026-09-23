// THE PARTITIONER: WHICH CELL OF WHICH LEVEL HOLDS EACH WHOLE, AND WHAT IS ALWAYS LOADED.
//
// WHAT IS ASSERTED, in the order the sections appear:
//
//   1. THE FORMAT IS SILENT WHEN THE WORLD IS NOT PARTITIONED. A `.desce` that says nothing about
//      partitioning comes out of the writer with exactly the keys it went in with - `kSceneVersion`
//      did not move and no file on disk changes a byte. A partitioned world states `Grids` as a LIST and
//      nothing else, and both numbers of a grid round-trip.
//   2. CELLS ARE DERIVED FROM COORDINATES - on the negative side of the origin too - and a cell is
//      HALF-OPEN, so a footprint ending exactly on an edge is in one cell.
//   3. TRANSFORMS COMPOSE DOWN THE HIERARCHY. A record's `Translation` is LOCAL.
//   4. THE COMPOSITE IS THE UNIT. A parent and child either side of a boundary are one composite, and a
//      socket attachment fuses two hierarchies into one.
//   5. LEVELS (owner decision O1, 2026-09-23). A composite goes to the LOWEST level with one cell holding
//      its footprint: a kilometre-wide one to the level whose cell is wider than a kilometre, one across
//      an edge to the level above, one across an axis to the always-loaded set - and the small thing
//      beside a wide one STAYS at level 0, which is the whole reason levels replaced growth. Footprints
//      come from positions, the primitive cube, a terrain's square and an ISM's instances.
//   6. ALWAYS-LOADED is derived from components (sun, sky, camera, screen canvas, non-spatial sound) and
//      from the author's marker; one global member takes its whole composite.
//   7. THE CENSUS: every component key ComponentRegistry.cpp registers has exactly one row in
//      kComponentLoading - read out of the registry's source, so a new component without a row is red.
//   8. DANGLING and cyclic containment; the REGISTER OF ENTITY REFERENCES is complete; and the whole
//      corpus, partitioned, keeps the level relation: every point read independently of the planner lies
//      inside the square of its composite's cell, and no lower level could have held it.
//
// No Scene, no renderer, no asset manager: the rules are a pure function of parsed records.

#include <Engine/Core/Serialize/ForeignKeys.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::SceneSerialized;
using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
using Desert::Core::Rules::AlwaysLoadedReason;
using Desert::Core::Rules::CellBounds;
using Desert::Core::Rules::CellCoord;
using Desert::Core::Rules::CellOf;
using Desert::Core::Rules::CellsPerLevel;
using Desert::Core::Rules::ComponentLoading;
using Desert::Core::Rules::Containment;
using Desert::Core::Rules::FindUnregisteredEntityReferences;
using Desert::Core::Rules::GridSquareOf;
using Desert::Core::Rules::kComponentLoading;
using Desert::Core::Rules::kEntityReferences;
using Desert::Core::Rules::kEntityReferencesByName;
using Desert::Core::Rules::kInstancePointsComponent;
using Desert::Core::Rules::kInstancePointsField;
using Desert::Core::Rules::kNoRecord;
using Desert::Core::Rules::LevelCellSize;
using Desert::Core::Rules::PlannedCell;
using Desert::Core::Rules::PlannedComposite;
using Desert::Core::Rules::PlanWorldPartition;
using Desert::Core::Rules::ReferenceKind;
using Desert::Core::Rules::SingleCellHolding;
using Desert::Core::Rules::SummarisePartition;
using Desert::Core::Rules::WorldPartitionPlan;

namespace
{
    EntityData Record( std::uint64_t id, const char* tag, glm::vec3 translation )
    {
        EntityData data;
        data.id          = Common::UUID( id );
        data.Tag         = tag;
        data.Translation = translation;
        return data;
    }

    EntityData& Under( EntityData& data, std::uint64_t parent )
    {
        data.parent = Common::UUID( parent );
        return data;
    }

    // A socket block spelled the way AuthoredComponentIO writes one: the id as a DECIMAL STRING,
    // because a 64-bit id does not survive JSON's double.
    EntityData& SocketedTo( EntityData& data, std::uint64_t target )
    {
        rfl::Generic::Object block;
        block["Target"]                     = rfl::Generic( std::to_string( target ) );
        block["BoneName"]                   = rfl::Generic( std::string( "hand_r" ) );
        data.Components["SocketAttachment"] = rfl::Generic( block );
        return data;
    }

    // A component block from JSON text, the way the file would hold it.
    EntityData& With( EntityData& data, const char* key, const char* json )
    {
        const auto block = rfl::json::read<rfl::Generic>( json );
        EXPECT_TRUE( block.has_value() ) << json;
        if ( block.has_value() )
            data.Components[key] = block.value();
        return data;
    }

    // The composite that contains a given record.
    std::size_t CompositeOf( const WorldPartitionPlan& plan, std::size_t record )
    {
        for ( std::size_t index = 0; index < plan.Composites.size(); ++index )
        {
            const auto& members = plan.Composites[index].Members;
            if ( std::find( members.begin(), members.end(), record ) != members.end() )
                return index;
        }
        return kNoRecord;
    }

    const PlannedComposite& HeldBy( const WorldPartitionPlan& plan, std::size_t record )
    {
        return plan.Composites.at( CompositeOf( plan, record ) );
    }

    // One grid, as a world states it.
    WorldPartitionSerialized Cells( float size, float loadingRange = 25600.0f )
    {
        WorldPartitionGridSerialized grid;
        grid.CellSize     = size;
        grid.LoadingRange = loadingRange;
        WorldPartitionSerialized settings;
        settings.Grids.push_back( grid );
        return settings;
    }

    std::set<std::string> KeysOf( const rfl::Generic& value )
    {
        std::set<std::string> keys;
        const auto            object = value.to_object();
        if ( !object.has_value() )
            return keys;
        for ( const auto& [key, field] : object.value() )
            keys.insert( key );
        return keys;
    }

    std::set<std::string> TopLevelKeys( const std::string& json )
    {
        const auto document = rfl::json::read<rfl::Generic>( json );
        return document.has_value() ? KeysOf( document.value() ) : std::set<std::string>{};
    }

    // Walks up from the working directory looking for a file only the repository has - the test
    // runner's working directory is not fixed. Same shape as Desert/Tests/Engine/SceneStitch.
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

    std::vector<std::filesystem::path> RepositoryScenes()
    {
        std::vector<std::filesystem::path> scenes;
        std::error_code                    ec;
        const std::filesystem::path        root = RepoRoot() + "Editor/Resources/Assets/Scenes";
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root, ec ) )
        {
            if ( entry.is_regular_file() && entry.path().extension() == ".desce" )
                scenes.push_back( entry.path() );
        }
        return scenes;
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // THE COMPONENT KEYS THE REGISTRY SERIALISES, read out of its SOURCE, because that is the one list
    // that cannot be out of date: a component is serialised exactly when ComponentRegistry.cpp registers
    // it. Comments are stripped first - a key quoted in prose is not a registration (the census that
    // fired on its own comment is AGENT_BRIEF_COMMON §8.3). Two spellings register a key: a maker call
    // `Make...<...>( "Key"` and a hand-built serializer's `s.Key = "Key"`. `registrations` counts the
    // `Register(` calls, so a third spelling nobody taught this reader shows up as a mismatch.
    std::set<std::string> RegisteredComponentKeys( std::size_t& registrations, std::size_t& keysRead )
    {
        std::string source =
             ReadAll( RepoRoot() + "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp" );
        source = std::regex_replace( source, std::regex( "//[^\n]*" ), "" );

        std::set<std::string> keys;
        keysRead = 0;
        const std::regex maker(
             "\\bMake(?:Reflected|ReflectedSelf|Marker|Flag|Authored)\\s*<[^>]*>\\s*\\(\\s*\"(\\w+)\"" );
        const std::regex manual( "\\bs\\.Key\\s*=\\s*\"(\\w+)\"" );
        for ( const std::regex& pattern : { maker, manual } )
        {
            for ( auto it = std::sregex_iterator( source.begin(), source.end(), pattern );
                  it != std::sregex_iterator(); ++it )
            {
                keys.insert( ( *it )[1].str() );
                ++keysRead;
            }
        }

        // `Register(` as a call: not `::Register(` (the definition) and not `->Register(` (an asset service).
        const std::regex call( "(^|[^\\w>.:])Register\\s*\\(" );
        registrations = static_cast<std::size_t>(
             std::distance( std::sregex_iterator( source.begin(), source.end(), call ), std::sregex_iterator() ) );
        return keys;
    }
} // namespace

// ── 1. THE FORMAT ──────────────────────────────────────────────────────────────────────────────────

// 1a. The corpus is where this suite thinks it is. Without this every corpus assertion below is a
// vacuous pass over zero files.
TEST( WorldPartitionFormat, TheScenesAreWhereThisSuiteThinksTheyAre )
{
    EXPECT_GE( RepositoryScenes().size(), 100u );
}

// 1b. A scene nobody partitioned writes no `WorldPartition` key: the field is a std::optional and
// reflect-cpp omits a nullopt field entirely.
TEST( WorldPartitionFormat, AnUnpartitionedSceneWritesNoPartitionKey )
{
    SceneSerialized scene;
    scene.SceneName    = "Nothing To Partition";
    scene.SceneVersion = Desert::Core::kSceneVersion;
    scene.UnitVersion  = Desert::Core::kUnitVersion;

    const std::string written = rfl::json::write( scene );
    EXPECT_EQ( written.find( "WorldPartition" ), std::string::npos ) << written;
    EXPECT_FALSE( TopLevelKeys( written ).count( "WorldPartition" ) );
}

// 1c. THE CORPUS. Every `.desce` the repository ships states no partition, and the top-level keys the
// writer produces from it are EXACTLY the keys the file had - so the format change touches no file.
TEST( WorldPartitionFormat, NoSceneInTheRepositoryStatesAPartitionOrGrowsAKey )
{
    for ( const auto& path : RepositoryScenes() )
    {
        const std::string text   = ReadAll( path );
        const auto        parsed = rfl::json::read<SceneSerialized>( text );
        ASSERT_TRUE( parsed.has_value() ) << path.string();
        EXPECT_FALSE( parsed->WorldPartition.has_value() ) << path.string();

        EXPECT_EQ( TopLevelKeys( rfl::json::write( parsed.value() ) ), TopLevelKeys( text ) ) << path.string();
    }
}

// 1d. The opposite case through the same instrument, and the SHAPE pinned: the block is an object whose
// only key is `Grids`, a LIST (A7), and a grid states exactly its two numbers. This is also where the
// Data Layers reservation (O4) is held: they arrive as a sibling of `Grids`, so the day they do, this
// test is the one that has to be edited - on purpose, beside the kSceneVersion step SceneFormat.hpp
// names for it.
TEST( WorldPartitionFormat, APartitionedSceneStatesAListOfGridsAndBothNumbersRoundTrip )
{
    SceneSerialized scene;
    scene.SceneName      = "Partitioned";
    scene.SceneVersion   = Desert::Core::kSceneVersion;
    scene.UnitVersion    = Desert::Core::kUnitVersion;
    scene.WorldPartition = Cells( 25600.0f, 76800.0f );

    const std::string written = rfl::json::write( scene );
    const auto        tree    = rfl::json::read<rfl::Generic>( written );
    ASSERT_TRUE( tree.has_value() );
    const auto block = tree->to_object().value().get( "WorldPartition" );
    ASSERT_TRUE( block.has_value() ) << written;
    EXPECT_EQ( KeysOf( block.value() ), ( std::set<std::string>{ "Grids" } ) ) << written;
    const auto grids = block->to_object().value().get( "Grids" );
    ASSERT_TRUE( grids.has_value() && grids->to_array().has_value() ) << "Grids is a list: " << written;
    ASSERT_EQ( grids->to_array()->size(), 1u );
    EXPECT_EQ( KeysOf( grids->to_array()->front() ), ( std::set<std::string>{ "CellSize", "LoadingRange" } ) );

    const auto read = rfl::json::read<SceneSerialized>( written );
    ASSERT_TRUE( read.has_value() );
    ASSERT_TRUE( read->WorldPartition.has_value() );
    ASSERT_EQ( read->WorldPartition->Grids.size(), 1u );
    EXPECT_FLOAT_EQ( read->WorldPartition->Grids[0].CellSize, 25600.0f );
    EXPECT_FLOAT_EQ( read->WorldPartition->Grids[0].LoadingRange, 76800.0f );
}

// A partitioned world is still at the CURRENT generation - the block is additive, so nothing had to be
// converted and the version gate cannot start refusing files over it.
TEST( WorldPartitionFormat, PartitioningAWorldDoesNotMoveItsVersion )
{
    SceneSerialized scene;
    scene.SceneVersion   = Desert::Core::kSceneVersion;
    scene.UnitVersion    = Desert::Core::kUnitVersion;
    scene.WorldPartition = Cells( 12800.0f );
    EXPECT_TRUE( Desert::Core::SceneIsAtCurrentVersion( scene ) );
}

// SAVING A PARTITIONED WORLD DOES NOT LOSE THE BLOCK. The live Scene has no partition member, so the
// writer builds a tree WITHOUT the block; the document merge is what puts it back, and this is what goes
// red the day the merge stops covering the top level.
TEST( WorldPartitionFormat, ThePartitionBlockSurvivesASaveThroughTheDocumentMerge )
{
    SceneSerialized onDisk;
    onDisk.SceneName      = "Partitioned";
    onDisk.SceneVersion   = Desert::Core::kSceneVersion;
    onDisk.UnitVersion    = Desert::Core::kUnitVersion;
    onDisk.WorldPartition = Cells( 51200.0f, 102400.0f );

    SceneSerialized fresh;
    fresh.SceneName    = onDisk.SceneName;
    fresh.SceneVersion = onDisk.SceneVersion;
    fresh.UnitVersion  = onDisk.UnitVersion;

    const auto freshTree  = rfl::json::read<rfl::Generic>( rfl::json::write( fresh ) );
    const auto sourceTree = rfl::json::read<rfl::Generic>( rfl::json::write( onDisk ) );
    ASSERT_TRUE( freshTree.has_value() );
    ASSERT_TRUE( sourceTree.has_value() );
    ASSERT_TRUE( freshTree->to_object().has_value() );
    ASSERT_TRUE( sourceTree->to_object().has_value() );

    const rfl::Generic::Object merged = Desert::Core::Serialize::MergeSceneDocument(
         freshTree->to_object().value(), sourceTree->to_object().value(),
         Desert::Core::Serialize::NothingIsOurs() );

    const auto reread = rfl::json::read<SceneSerialized>( rfl::json::write( rfl::Generic( merged ) ) );
    ASSERT_TRUE( reread.has_value() );
    ASSERT_TRUE( reread->WorldPartition.has_value() );
    ASSERT_EQ( reread->WorldPartition->Grids.size(), 1u );
    EXPECT_FLOAT_EQ( reread->WorldPartition->Grids[0].CellSize, 51200.0f );
    EXPECT_FLOAT_EQ( reread->WorldPartition->Grids[0].LoadingRange, 102400.0f );
}

// ── 2. CELLS ARE DERIVED FROM COORDINATES ──────────────────────────────────────────────────────────

TEST( WorldPartitionCells, TheGridIsUniformOnBothSidesOfTheOrigin )
{
    EXPECT_EQ( CellOf( 0.0f, 0.0f, 100.0 ).X, 0 );
    EXPECT_EQ( CellOf( 99.0f, 99.0f, 100.0 ).X, 0 );
    EXPECT_EQ( CellOf( 100.0f, 0.0f, 100.0 ).X, 1 );

    // A cast to int truncates towards zero, which would answer 0 here and make the cell spanning the
    // origin twice as wide as every other cell in the world.
    EXPECT_EQ( CellOf( -1.0f, 0.0f, 100.0 ).X, -1 );
    EXPECT_EQ( CellOf( -100.0f, 0.0f, 100.0 ).X, -1 );
    EXPECT_EQ( CellOf( -101.0f, 0.0f, 100.0 ).X, -2 );
    EXPECT_EQ( CellOf( 0.0f, -1.0f, 100.0 ).Z, -1 );
}

// A CELL IS HALF-OPEN. A footprint that ends exactly on an edge is in ONE cell, a footprint one unit
// past it is in two, and a point on an edge is in the cell it starts. Without the half-open rule every
// ground tile of a generated world - exactly one cell wide, exactly on the edges - would go up a level.
TEST( WorldPartitionCells, AFootprintEndingExactlyOnAnEdgeIsInOneCell )
{
    const auto exact = SingleCellHolding( CellBounds{ 100.0f, 0.0f, 200.0f, 100.0f }, 100.0 );
    ASSERT_TRUE( exact.has_value() );
    EXPECT_EQ( *exact, ( CellCoord{ 1, 0 } ) );

    EXPECT_FALSE( SingleCellHolding( CellBounds{ 100.0f, 0.0f, 201.0f, 100.0f }, 100.0 ).has_value() );
    EXPECT_FALSE( SingleCellHolding( CellBounds{ 99.0f, 0.0f, 150.0f, 50.0f }, 100.0 ).has_value() );

    const auto point = SingleCellHolding( CellBounds{ 200.0f, -100.0f, 200.0f, -100.0f }, 100.0 );
    ASSERT_TRUE( point.has_value() );
    EXPECT_EQ( *point, ( CellCoord{ 2, -1 } ) );

    // A coordinate whose index no int32 holds fits no cell rather than overflowing.
    EXPECT_FALSE( SingleCellHolding( CellBounds{ 1e30f, 0.0f, 1e30f, 0.0f }, 100.0 ).has_value() );
}

// ── 3. TRANSFORMS COMPOSE DOWN THE HIERARCHY ───────────────────────────────────────────────────────

// A child states a LOCAL translation. Partitioning on the stated number would put this child in cell 0
// when it is standing in cell 3.
TEST( WorldPartitionCells, AChildIsPartitionedByItsWorldPositionNotItsLocalOne )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Parent", { 30000.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "Child", { 100.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    EXPECT_EQ( plan.Composites[0].Anchor, 0u );
    ASSERT_TRUE( plan.Composites[0].Footprint.has_value() );
    EXPECT_FLOAT_EQ( plan.Composites[0].Footprint->MaxX, 30100.0f );
    // 30000..30100 is inside cell 3 of level 0.
    EXPECT_EQ( plan.Composites[0].Level, 0 );
    EXPECT_EQ( plan.Composites[0].Cell.X, 3 );
}

// Rotation composes too. A child stated 15000 units along +X under a parent yawed +90 degrees stands
// 15000 units along world -Z: the footprint reaches DOWN on Z and not out on X, and that decides the level.
// The same records unrotated land on a different level and cell, so the assertion can see rotation.
TEST( WorldPartitionCells, RotationOfAParentMovesWhereItsChildLands )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Turntable", { 5000.0f, 0.0f, 25000.0f } ) );
    records[0].Rotation = glm::vec3( 0.0f, glm::radians( 90.0f ), 0.0f );
    records.push_back( Record( 2, "Arm", { 15000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    const auto& held = plan.Composites[0];
    ASSERT_TRUE( held.Footprint.has_value() );
    EXPECT_NEAR( held.Footprint->MinZ, 10000.0f, 1.0f );
    EXPECT_NEAR( held.Footprint->MaxX, 5000.0f, 1.0f );
    // Z 10000..25000 crosses the 20000 edge of level 1; a 40000 cell holds it: level 2, cell (0, 0).
    EXPECT_EQ( held.Reason, AlwaysLoadedReason::None );
    EXPECT_EQ( held.Level, 2 );
    EXPECT_EQ( held.Cell, ( CellCoord{ 0, 0 } ) );

    // Unrotated, the arm is at X 20000: X 5000..20000 and Z 25000 fit one level-1 cell, (0, 1).
    records[0].Rotation               = glm::vec3( 0.0f );
    const WorldPartitionPlan straight = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_EQ( straight.Composites[0].Level, 1 );
    EXPECT_EQ( straight.Composites[0].Cell, ( CellCoord{ 0, 1 } ) );
}

// THE COMPOSITION, PINNED AGAINST NUMBERS WORKED BY HAND, because ComposeLocal must equal
// TransformComponent::GetTransform (translate * rotate * scale) and this suite cannot include that
// header. Parent at (1000, 0, 2000), yawed +90 degrees, scaled 2; child stated at (300, 0, 0):
// scale gives (600, 0, 0), the yaw takes +X to -Z giving (0, 0, -600), the offset gives (1000, 0, 1400).
TEST( WorldPartitionCells, ARotatedScaledOffsetParentPlacesItsChildWhereTheLoaderWould )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Parent", { 1000.0f, 0.0f, 2000.0f } ) );
    records[0].Rotation = glm::vec3( 0.0f, glm::radians( 90.0f ), 0.0f );
    records[0].Scale    = glm::vec3( 2.0f );
    records.push_back( Record( 2, "Child", { 300.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 1000000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    ASSERT_TRUE( plan.Composites[0].Footprint.has_value() );
    const CellBounds& footprint = *plan.Composites[0].Footprint;
    EXPECT_NEAR( footprint.MinX, 1000.0f, 0.5f );
    EXPECT_NEAR( footprint.MaxX, 1000.0f, 0.5f );
    EXPECT_NEAR( footprint.MinZ, 1400.0f, 0.5f );
    EXPECT_NEAR( footprint.MaxZ, 2000.0f, 0.5f );
}

// ── 4. THE COMPOSITE IS THE UNIT ───────────────────────────────────────────────────────────────────

// Two entities a per-entity grid would put in DIFFERENT cells are one composite - and since together they
// cross the edge at 10000, the composite goes UP ONE LEVEL (a 20000 cell holds 9900..10100) instead of
// dragging cell 0's bounds over its neighbour.
TEST( WorldPartitionComposites, AParentAndChildEitherSideOfABoundaryGoUpOneLevelTogether )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Vehicle", { 9900.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "Wheel", { 200.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    // Per entity: the vehicle is in cell 0 (9900/10000) and the wheel is in cell 1 (10100/10000).
    EXPECT_EQ( CellOf( 9900.0f, 0.0f, 10000.0 ).X, 0 );
    EXPECT_EQ( CellOf( 10100.0f, 0.0f, 10000.0 ).X, 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    EXPECT_EQ( plan.Composites[0].Members.size(), 2u );
    EXPECT_EQ( plan.Composites[0].Level, 1 );
    EXPECT_EQ( plan.Composites[0].Cell.X, 0 );

    ASSERT_EQ( plan.Cells.size(), 1u );
    EXPECT_EQ( plan.Cells[0].Level, 1 );
    EXPECT_FLOAT_EQ( plan.Cells[0].Square.MaxX, 20000.0f ) << "a level-1 cell is two level-0 cells wide";
    EXPECT_EQ( plan.MaxLevel, 1 );
    EXPECT_EQ( plan.MaxLevelComposite, 0u );
}

// A socket attachment is containment, so it FUSES two hierarchies that are otherwise unrelated.
TEST( WorldPartitionComposites, ASocketAttachmentFusesTwoHierarchiesIntoOneComposite )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Character", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "CharacterHat", { 100.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );
    records.push_back( Record( 3, "Rifle", { 50.0f, 0.0f, 0.0f } ) );
    SocketedTo( records[2], 1 );
    records.push_back( Record( 4, "RifleScope", { 10.0f, 0.0f, 0.0f } ) );
    Under( records[3], 3 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    EXPECT_EQ( plan.Composites[0].Members.size(), 4u );

    bool sawSocket = false;
    for ( const auto& edge : plan.Containment )
    {
        if ( edge.Relation == Containment::SocketAttachment )
        {
            sawSocket = true;
            EXPECT_EQ( edge.Member, 2u );
            EXPECT_EQ( edge.Holder, 0u );
        }
    }
    EXPECT_TRUE( sawSocket );
}

// Two entities with no relation between them are two composites in two cells, both at level 0.
TEST( WorldPartitionComposites, UnrelatedEntitiesAreSeparateCompositesInSeparateCells )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "RockA", { 10.0f, 0.0f, 10.0f } ) );
    records.push_back( Record( 2, "RockB", { 50000.0f, 0.0f, 10.0f } ) );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 2u );
    EXPECT_NE( HeldBy( plan, 0 ).Cell, HeldBy( plan, 1 ).Cell );
    EXPECT_EQ( plan.Cells.size(), 2u );
    EXPECT_EQ( plan.MaxLevel, 0 );
    EXPECT_TRUE( plan.AlwaysLoaded.empty() );
}

// A PREFAB INSTANCE HAS NO TRANSFORM IN A .desce AT ALL - SceneSerializer strips it into overrides
// addressed by ids only the .deprefab resolves - so this partitioner cannot place one, and says so.
TEST( WorldPartitionComposites, APrefabInstanceWithNoTransformIsListedAsUnplaced )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Ground", { 10.0f, 0.0f, 10.0f } ) );
    EntityData instance;
    instance.id         = Common::UUID( 2 );
    instance.PrefabPath = "Prefabs/Lamp.deprefab";
    records.push_back( instance );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.UnplacedPrefabInstances.size(), 1u );
    EXPECT_EQ( plan.UnplacedPrefabInstances[0], 1u );

    records[1].Translation = glm::vec3( 50000.0f, 0.0f, 0.0f );
    EXPECT_TRUE( PlanWorldPartition( records, Cells( 10000.0f ) ).UnplacedPrefabInstances.empty() );
}

// AN UNPLACED INSTANCE CONTRIBUTES NO POINT: its "origin" is the absence of a position. A composite that
// is nothing else has no footprint at all, and it does not decide how many levels the grid has.
TEST( WorldPartitionComposites, AnUnplacedPrefabInstanceContributesNoPoint )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Ground", { 5000.0f, 0.0f, 5000.0f } ) );
    EntityData instance;
    instance.id         = Common::UUID( 2 );
    instance.PrefabPath = "Prefabs/Lamp.deprefab";
    records.push_back( instance );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_FALSE( HeldBy( plan, 1 ).Footprint.has_value() );
    EXPECT_EQ( HeldBy( plan, 1 ).Cell, ( CellCoord{ 0, 0 } ) );
    EXPECT_EQ( plan.LevelCount, 1 );
}

// THE CORPUS: exactly the four prefab instance records this repository ships, in two scenes, are
// unplaceable - a named number derived from the files rather than a claim that none exist.
TEST( WorldPartitionComposites, TheCorpusPrefabInstancesAreAllUnplaceableAndAreCounted )
{
    std::size_t unplaced = 0;
    std::size_t scenes   = 0;
    for ( const auto& path : RepositoryScenes() )
    {
        const auto parsed = rfl::json::read<SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();

        const WorldPartitionPlan plan = PlanWorldPartition( parsed->Entities, Cells( 12800.0f ) );
        if ( !plan.UnplacedPrefabInstances.empty() )
        {
            ++scenes;
            unplaced += plan.UnplacedPrefabInstances.size();
        }
    }
    EXPECT_EQ( unplaced, 4u );
    EXPECT_EQ( scenes, 2u );
}

// ── 5. LEVELS (owner decision O1, 2026-09-23) ──────────────────────────────────────────────────────

// A KILOMETRE-WIDE COMPOSITE GOES TO THE LEVEL WHOSE CELL HOLDS A KILOMETRE. Cell 12800 (128 m); the
// bridge runs X 1000..101000. Level 2 cells are 51200 wide (it crosses 51200); level 3 cells are 102400
// and [0, 102400) holds it. A rock far out makes the grid taller than 3 levels, so level 3 is chosen
// because it is the lowest that fits and not because it is the top.
TEST( WorldPartitionLevels, AKilometreWideCompositeGoesToTheLevelWhoseCellHoldsIt )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Bridge", { 1000.0f, 0.0f, 1000.0f } ) );
    records.push_back( Record( 2, "BridgeFarEnd", { 100000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );
    records.push_back( Record( 3, "FarRock", { 900000.0f, 0.0f, 900000.0f } ) );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 12800.0f ) );
    EXPECT_GT( plan.LevelCount, 4 ) << "the far rock must lift the top above level 3";

    const PlannedComposite& bridge = HeldBy( plan, 0 );
    EXPECT_EQ( bridge.Reason, AlwaysLoadedReason::None );
    EXPECT_EQ( bridge.Level, 3 );
    EXPECT_EQ( bridge.Cell, ( CellCoord{ 0, 0 } ) );
    EXPECT_EQ( plan.MaxLevel, 3 );
    EXPECT_EQ( plan.MaxLevelComposite, CompositeOf( plan, 0 ) );

    // And the level's own square holds the footprint - the cell did not grow, it was already big enough.
    const double size = LevelCellSize( 12800.0f, 3 );
    EXPECT_DOUBLE_EQ( size, 102400.0 );
    EXPECT_LE( 101000.0, size );
}

// THE REASON LEVELS REPLACED GROWTH. A small rock in the same square as the bridge's anchor stays at
// level 0 in its own 128 m cell. Under WP1's growing cell the same rock would have been in a cell 101000
// wide and loaded from a kilometre away.
TEST( WorldPartitionLevels, ASmallThingBesideAWideOneStaysAtLevelZero )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Bridge", { 1000.0f, 0.0f, 1000.0f } ) );
    records.push_back( Record( 2, "BridgeFarEnd", { 100000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );
    records.push_back( Record( 3, "Pebble", { 2000.0f, 0.0f, 2000.0f } ) );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 12800.0f ) );
    EXPECT_EQ( HeldBy( plan, 0 ).Level, 3 );
    EXPECT_EQ( HeldBy( plan, 2 ).Level, 0 );
    EXPECT_EQ( HeldBy( plan, 2 ).Cell, ( CellCoord{ 0, 0 } ) );

    // Two cells, on two levels, both called (0, 0): the level is part of a cell's identity.
    ASSERT_EQ( plan.Cells.size(), 2u );
    EXPECT_EQ( plan.Cells[0].Level, 0 );
    EXPECT_EQ( plan.Cells[1].Level, 3 );
    const auto perLevel = CellsPerLevel( plan );
    ASSERT_EQ( perLevel.size(), static_cast<std::size_t>( plan.LevelCount ) );
    EXPECT_EQ( perLevel[0], 1u );
    EXPECT_EQ( perLevel[3], 1u );
}

// THE LEVELS ARE ALIGNED, AND THAT HAS A PRICE, pinned so nobody discovers it: 102400 is an edge of
// levels 0, 1, 2 and 3 at once, so an 8 m composite across it goes all the way to level 4. UE's answer
// is its non-aligned levels (each shifted by half a cell); not taken here - see the file header.
TEST( WorldPartitionLevels, ACompositeAcrossAnEdgeSharedByManyLevelsGoesAboveAllOfThem )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Gate", { 102000.0f, 0.0f, 1000.0f } ) );
    records.push_back( Record( 2, "GateHinge", { 800.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );
    records.push_back( Record( 3, "FarRock", { 900000.0f, 0.0f, 900000.0f } ) );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 12800.0f ) );
    EXPECT_EQ( HeldBy( plan, 0 ).Level, 4 );
}

// A COMPOSITE NO LEVEL HOLDS IS ALWAYS-LOADED. The levels are aligned on the origin to the top, so X = 0
// is an edge of every level and a composite across it fits none. Its reason says so.
TEST( WorldPartitionLevels, ACompositeAcrossTheOriginFitsNoLevelAndIsAlwaysLoaded )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Signpost", { -100.0f, 0.0f, 500.0f } ) );
    records.push_back( Record( 2, "SignpostArm", { 200.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );
    records.push_back( Record( 3, "Rock", { 500.0f, 0.0f, 500.0f } ) );

    const WorldPartitionPlan plan     = PlanWorldPartition( records, Cells( 12800.0f ) );
    const PlannedComposite&  signpost = HeldBy( plan, 0 );
    EXPECT_EQ( signpost.Reason, AlwaysLoadedReason::NoFit );
    EXPECT_EQ( signpost.Because, kNoRecord ) << "no member caused it; the footprint did";
    ASSERT_EQ( plan.AlwaysLoaded.size(), 1u );
    EXPECT_EQ( plan.AlwaysLoaded[0], CompositeOf( plan, 0 ) );

    // It is in no cell; the rock is in one.
    ASSERT_EQ( plan.Cells.size(), 1u );
    EXPECT_EQ( plan.Cells[0].Composites, ( std::vector<std::size_t>{ CompositeOf( plan, 2 ) } ) );
}

// THE LEVEL COUNT IS DERIVED FROM THE WORLD: the top level is the first whose cell reaches the furthest
// footprint. A global composite does not count - the sun 10 km away does not make the grid taller.
TEST( WorldPartitionLevels, TheLevelCountReachesTheFurthestPlacedFootprintAndIgnoresGlobalOnes )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Rock", { 5000.0f, 0.0f, 5000.0f } ) );
    EXPECT_EQ( PlanWorldPartition( records, Cells( 10000.0f ) ).LevelCount, 1 );

    records.push_back( Record( 2, "Sun", { 1000000.0f, 0.0f, 0.0f } ) );
    With( records[1], "DirectionLight", R"({"Intensity":1.0})" );
    EXPECT_EQ( PlanWorldPartition( records, Cells( 10000.0f ) ).LevelCount, 1 );

    records.push_back( Record( 3, "FarRock", { 0.0f, 0.0f, -35000.0f } ) );
    // 10000 * 2^2 = 40000 >= 35000.
    EXPECT_EQ( PlanWorldPartition( records, Cells( 10000.0f ) ).LevelCount, 3 );
}

// HEIGHT DOES NOT PARTITION. The grid is two-dimensional, so a composite stacked vertically stays at
// level 0 however tall it is.
TEST( WorldPartitionLevels, VerticalReachDoesNotPromoteBecauseTheGridIsTwoDimensional )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Tower", { 10.0f, 0.0f, 10.0f } ) );
    records.push_back( Record( 2, "TowerTop", { 0.0f, 500000.0f, 0.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_EQ( plan.Composites[0].Level, 0 );
}

// AN INSTANCED STATIC MESH IS AS WIDE AS ITS INSTANCES, NOT AS ITS ENTITY. The instances are WORLD-space,
// so a foliage field on an entity near the origin covers wherever its instances are. The numbers are JSON
// integers on purpose: rfl reads "50000" as an integer, and a reader that only asked for doubles would drop
// exactly these instances and leave the field at level 0.
TEST( WorldPartitionLevels, AnInstancedMeshIsAsWideAsItsInstancesNotItsEntity )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Foliage", { 10.0f, 0.0f, 10.0f } ) );
    With( records[0], "InstancedStaticMesh",
          R"({"Primitive":"Sphere","InstanceTransforms":[)"
          R"([1,0,0,0, 0,1,0,0, 0,0,1,0, 50000,0,30000,1],)"
          R"([1.0,0,0,0, 0,1.0,0,0, 0,0,1.0,0, 200.5,0.0,300.5,1.0]]})" );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    const PlannedComposite&  held = plan.Composites[0];
    ASSERT_TRUE( held.Footprint.has_value() );
    EXPECT_FLOAT_EQ( held.Footprint->MaxX, 50000.0f );
    EXPECT_FLOAT_EQ( held.Footprint->MaxZ, 30000.0f );
    // X 10..50000 needs an 80000 cell: level 3.
    EXPECT_EQ( held.Level, 3 );
    EXPECT_EQ( plan.PointOnlyRecords, 0u );
}

// THE PRIMITIVE CUBE HAS A FOOTPRINT, and it is the corners through the world matrix. The same record
// drawing a Sphere is a point (no stated extent for it yet) - so the pair shows the footprint moving the
// answer, not an instrument that cannot tell the two apart.
TEST( WorldPartitionLevels, APrimitiveCubeIsItsCornersAndAnotherPrimitiveIsItsPosition )
{
    std::vector<EntityData> records;
    // Centred 40 short of the 10000 edge with a 100 cm half-extent after a scale of 2: 9860..10060.
    records.push_back( Record( 1, "Crate", { 9960.0f, 0.0f, 500.0f } ) );
    records[0].Scale = glm::vec3( 2.0f );
    With( records[0], "StaticMesh", R"({"Primitive":"Cube"})" );

    const WorldPartitionPlan cube = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_TRUE( cube.Composites[0].Footprint.has_value() );
    EXPECT_NEAR( cube.Composites[0].Footprint->MinX, 9860.0f, 0.01f );
    EXPECT_NEAR( cube.Composites[0].Footprint->MaxX, 10060.0f, 0.01f );
    EXPECT_EQ( cube.Composites[0].Level, 1 );
    EXPECT_EQ( cube.PointOnlyRecords, 0u );

    With( records[0], "StaticMesh", R"({"Primitive":"Sphere"})" );
    const WorldPartitionPlan sphere = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_EQ( sphere.Composites[0].Level, 0 );
    EXPECT_EQ( sphere.PointOnlyRecords, 1u );
}

// A TERRAIN IS ITS SQUARE: `Size` wide, centred on its entity (TerrainMeshFactory). 10000 wide at
// (25000, 25000) is exactly cell (2, 2); 12000 wide crosses two edges and needs a level-2 cell.
TEST( WorldPartitionLevels, ATerrainIsItsSquare )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Terrain", { 25000.0f, 0.0f, 25000.0f } ) );
    With( records[0], "Terrain", R"({"Size":10000.0})" );
    const WorldPartitionPlan exact = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_EQ( exact.Composites[0].Level, 0 );
    EXPECT_EQ( exact.Composites[0].Cell, ( CellCoord{ 2, 2 } ) );

    With( records[0], "Terrain", R"({"Size":12000})" );
    const WorldPartitionPlan wider = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_EQ( wider.Composites[0].Level, 2 );
}

// ── 6. ALWAYS-LOADED ───────────────────────────────────────────────────────────────────────────────

// THE SUN, THE SKY AND THE CAMERA ARE NOT IN CELL (0, 0). Each is always-loaded, by Component, and names
// itself; the rock beside them is in a cell.
TEST( WorldPartitionAlwaysLoaded, SunSkyAndCameraAreAlwaysLoadedByTheirComponents )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Sun", { 10.0f, 0.0f, 10.0f } ) );
    With( records[0], "DirectionLight", R"({"Intensity":3.0})" );
    records.push_back( Record( 2, "Sky", { 10.0f, 0.0f, 10.0f } ) );
    With( records[1], "SkyAtmosphere", R"({})" );
    records.push_back( Record( 3, "Camera", { 10.0f, 170.0f, 10.0f } ) );
    With( records[2], "Camera", R"({"IsMainCamera":true})" );
    records.push_back( Record( 4, "Rock", { 10.0f, 0.0f, 10.0f } ) );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 12800.0f ) );
    ASSERT_EQ( plan.AlwaysLoaded.size(), 3u );
    for ( std::size_t record = 0; record < 3; ++record )
    {
        EXPECT_EQ( HeldBy( plan, record ).Reason, AlwaysLoadedReason::Component ) << record;
        EXPECT_EQ( HeldBy( plan, record ).Because, record );
    }
    EXPECT_EQ( HeldBy( plan, 3 ).Reason, AlwaysLoadedReason::None );
    ASSERT_EQ( plan.Cells.size(), 1u );
}

// ONE GLOBAL MEMBER TAKES THE WHOLE COMPOSITE: a camera parented under a character keeps the character
// (and its hat) loaded everywhere, because a composite is never divided.
TEST( WorldPartitionAlwaysLoaded, OneGlobalMemberMakesItsWholeCompositeAlwaysLoaded )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Player", { 50000.0f, 0.0f, 50000.0f } ) );
    records.push_back( Record( 2, "PlayerHat", { 0.0f, 180.0f, 0.0f } ) );
    Under( records[1], 1 );
    records.push_back( Record( 3, "FollowCamera", { 0.0f, 300.0f, -500.0f } ) );
    Under( records[2], 1 );
    With( records[2], "Camera", R"({"IsMainCamera":true})" );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 12800.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    EXPECT_EQ( plan.Composites[0].Reason, AlwaysLoadedReason::Component );
    EXPECT_EQ( plan.Composites[0].Because, 2u );
    EXPECT_TRUE( plan.Cells.empty() );
}

// THE AUTHOR'S MARKER, and it outranks a component: the reason names the author and the marked record.
TEST( WorldPartitionAlwaysLoaded, TheAuthorsMarkerMakesACompositeAlwaysLoadedAndOutranksAComponent )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "GameManager", { 50000.0f, 0.0f, 50000.0f } ) );
    With( records[0], "Script", R"({"Path":"Scripts/GameManager.lua"})" );
    EXPECT_EQ( PlanWorldPartition( records, Cells( 12800.0f ) ).Composites[0].Reason, AlwaysLoadedReason::None );

    With( records[0], "AlwaysLoaded", R"({})" );
    const WorldPartitionPlan marked = PlanWorldPartition( records, Cells( 12800.0f ) );
    EXPECT_EQ( marked.Composites[0].Reason, AlwaysLoadedReason::Author );

    // A camera first in file order and the marker second: the author still names the reason.
    records.insert( records.begin(), Record( 2, "Camera", { 0.0f, 0.0f, 0.0f } ) );
    With( records[0], "Camera", R"({})" );
    Under( records[1], 2 );
    const WorldPartitionPlan both = PlanWorldPartition( records, Cells( 12800.0f ) );
    ASSERT_EQ( both.Composites.size(), 1u );
    EXPECT_EQ( both.Composites[0].Reason, AlwaysLoadedReason::Author );
    EXPECT_EQ( both.Composites[0].Because, 1u );
}

// DECIDED BY A FIELD: a canvas is global unless it is WorldSpace (RenderMode 1), and absent means its
// default, ScreenSpace; a sound is spatial unless `Spatial` is false, and absent means its default, true.
TEST( WorldPartitionAlwaysLoaded, ACanvasAndASoundAreDecidedByTheirField )
{
    const auto reasonOf = []( const char* key, const char* json )
    {
        std::vector<EntityData> records;
        records.push_back( Record( 1, "Thing", { 10.0f, 0.0f, 10.0f } ) );
        With( records[0], key, json );
        return PlanWorldPartition( records, Cells( 12800.0f ) ).Composites[0].Reason;
    };

    EXPECT_EQ( reasonOf( "UICanvas", R"({"ScaleMode":2})" ), AlwaysLoadedReason::Component );
    EXPECT_EQ( reasonOf( "UICanvas", R"({"RenderMode":0})" ), AlwaysLoadedReason::Component );
    EXPECT_EQ( reasonOf( "UICanvas", R"({"RenderMode":1})" ), AlwaysLoadedReason::None );

    EXPECT_EQ( reasonOf( "AudioSource", R"({"Volume":1.0})" ), AlwaysLoadedReason::None );
    EXPECT_EQ( reasonOf( "AudioSource", R"({"Spatial":true})" ), AlwaysLoadedReason::None );
    EXPECT_EQ( reasonOf( "AudioSource", R"({"Spatial":false})" ), AlwaysLoadedReason::Component );
}

// NO USABLE GRID IS NOT REPAIRED: no `Grids` entry, or a non-positive cell size, makes every composite
// always-loaded with the reason NoGrid (a global one keeps its own reason). A second grid is counted as
// unused rather than silently dropped.
TEST( WorldPartitionAlwaysLoaded, NoGridMeansEverythingIsAlwaysLoadedAndSaysWhy )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Rock", { 10.0f, 0.0f, 10.0f } ) );
    records.push_back( Record( 2, "Sun", { 10.0f, 0.0f, 10.0f } ) );
    With( records[1], "DirectionLight", R"({})" );

    const WorldPartitionPlan none = PlanWorldPartition( records, WorldPartitionSerialized{} );
    EXPECT_EQ( none.LevelCount, 0 );
    EXPECT_TRUE( none.Cells.empty() );
    ASSERT_EQ( none.AlwaysLoaded.size(), 2u );
    EXPECT_EQ( HeldBy( none, 0 ).Reason, AlwaysLoadedReason::NoGrid );
    EXPECT_EQ( HeldBy( none, 1 ).Reason, AlwaysLoadedReason::Component );

    EXPECT_EQ( HeldBy( PlanWorldPartition( records, Cells( 0.0f ) ), 0 ).Reason, AlwaysLoadedReason::NoGrid );

    WorldPartitionSerialized two = Cells( 12800.0f );
    two.Grids.push_back( two.Grids.front() );
    const WorldPartitionPlan twoGrids = PlanWorldPartition( records, two );
    EXPECT_EQ( twoGrids.UnusedGrids, 1u );
    EXPECT_EQ( HeldBy( twoGrids, 0 ).Reason, AlwaysLoadedReason::None );
}

// THE ONE-LINE SUMMARY the loader logs and WorldGen prints, read back for the numbers it must carry.
TEST( WorldPartitionAlwaysLoaded, TheSummaryStatesCellsPerLevelAndAlwaysLoadedByReason )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Rock", { 10.0f, 0.0f, 10.0f } ) );
    records.push_back( Record( 2, "Wide", { 9900.0f, 0.0f, 10.0f } ) );
    records.push_back( Record( 3, "WideEnd", { 200.0f, 0.0f, 0.0f } ) );
    Under( records[2], 2 );
    records.push_back( Record( 4, "Sun", { 10.0f, 0.0f, 10.0f } ) );
    With( records[3], "DirectionLight", R"({})" );

    const WorldPartitionSerialized settings = Cells( 10000.0f, 30000.0f );
    const std::string summary = SummarisePartition( PlanWorldPartition( records, settings ), settings );
    EXPECT_NE( summary.find( "3 composite(s): 2 cell(s) over 2 level(s) [L0: 1, L1: 1]" ), std::string::npos )
         << summary;
    EXPECT_NE( summary.find( "1 always-loaded (author 0, component 1, no fit 0, no grid 0)" ), std::string::npos )
         << summary;
    EXPECT_NE( summary.find( "highest promotion: level 1" ), std::string::npos ) << summary;
    EXPECT_NE( summary.find( "cell 10000, loading range 30000" ), std::string::npos ) << summary;
}

// ── 7. THE CENSUS: EVERY SERIALISED COMPONENT IS CLASSIFIED ────────────────────────────────────────

// The key list comes from ComponentRegistry.cpp's source, and the reader is checked against the number of
// `Register(` calls, so a registration spelled in a way this reader does not know cannot silently shrink
// the list. Then the two sets must be EQUAL: a key with no row is a component nobody decided about; a row
// with no key is a classification of something the registry no longer writes.
TEST( WorldPartitionCensus, EveryComponentTheRegistrySerialisesHasExactlyOneLoadingRow )
{
    std::size_t                 registrations = 0;
    std::size_t                 keysRead      = 0;
    const std::set<std::string> registered    = RegisteredComponentKeys( registrations, keysRead );
    ASSERT_GE( registered.size(), 50u )
         << "the registry reader found almost nothing - it is reading the wrong file";
    EXPECT_EQ( keysRead, registrations )
         << "ComponentRegistry.cpp has " << registrations << " Register( calls but " << keysRead
         << " keys were read: a registration is spelled in a way RegisteredComponentKeys does not know";
    EXPECT_EQ( keysRead, registered.size() ) << "a component key is registered twice";

    std::set<std::string> rows;
    for ( const auto& row : kComponentLoading )
        EXPECT_TRUE( rows.insert( std::string( row.ComponentKey ) ).second )
             << "two rows for " << row.ComponentKey;

    for ( const auto& key : registered )
        EXPECT_TRUE( rows.count( key ) ) << "'" << key
                                         << "' is serialised by ComponentRegistry.cpp and has no row in "
                                            "kComponentLoading (WorldPartitionRules.hpp). Decide whether it "
                                            "keeps its entity loaded everywhere: Spatial, Global or ByField.";
    for ( const auto& row : rows )
        EXPECT_TRUE( registered.count( row ) )
             << "kComponentLoading classifies '" << row << "', which ComponentRegistry.cpp does not serialise";
}

// The rows that decide what a world keeps loaded, pinned by name: the brief's sun, sky and camera, and
// the author's marker. A census of set equality would still pass if one of them were quietly Spatial.
TEST( WorldPartitionCensus, TheGlobalRowsAreTheOnesTheWorldNeedsEverywhere )
{
    const auto loadingOf = []( std::string_view key )
    {
        for ( const auto& row : kComponentLoading )
            if ( row.ComponentKey == key )
                return row.Loading;
        return ComponentLoading::Spatial;
    };
    for ( const char* key : { "Camera", "DirectionLight", "Skybox", "SkyAtmosphere", "ExponentialHeightFog",
                              "VolumetricCloud", "HeroCloud", "AlwaysLoaded" } )
        EXPECT_EQ( loadingOf( key ), ComponentLoading::Global ) << key;
    EXPECT_EQ( loadingOf( "UICanvas" ), ComponentLoading::ByField );
    EXPECT_EQ( loadingOf( "AudioSource" ), ComponentLoading::ByField );
    EXPECT_EQ( loadingOf( "StaticMesh" ), ComponentLoading::Spatial );
    EXPECT_EQ( loadingOf( "PointLight" ), ComponentLoading::Spatial );
}

// ── 8. DANGLING AND CYCLIC CONTAINMENT; THE REGISTER OF ENTITY REFERENCES; THE CORPUS ─────────────

// A containment reference naming an entity this file does not contain is REPORTED and nothing more: it
// is a defect that already has an owner (the loader counts unresolved parents; AttachmentSystem skips a
// target it cannot find).
TEST( WorldPartitionComposites, ADanglingContainmentReferenceIsReportedAndJoinsNothing )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Lonely", { 0.0f, 0.0f, 0.0f } ) );
    records[0].parent = Common::UUID( 9999 );
    records.push_back( Record( 2, "AlsoLonely", { 0.0f, 0.0f, 0.0f } ) );
    SocketedTo( records[1], 8888 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Dangling.size(), 2u );
    EXPECT_TRUE( plan.Containment.empty() );
    EXPECT_EQ( plan.Composites.size(), 2u );
}

// A hand-merged file can name a cycle. It must terminate and produce an answer rather than hang; the
// answer is one composite, which is what a cycle of containment is.
TEST( WorldPartitionComposites, ACyclicHierarchyTerminatesAndIsOneComposite )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "A", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "B", { 0.0f, 0.0f, 0.0f } ) );
    Under( records[0], 2 );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_EQ( plan.Composites.size(), 1u );
    EXPECT_EQ( plan.Composites[0].Members.size(), 2u );
}

// The rows themselves, named. A count would be satisfied by editing the count; these are the two
// references the census over the engine's ninety-odd components found, with their classification.
TEST( WorldPartitionReferences, TheRegisterNamesTheReferencesItClassifies )
{
    bool socket     = false;
    bool projectile = false;
    for ( const auto& row : kEntityReferences )
    {
        if ( row.ComponentKey == "SocketAttachment" && row.Field == "Target" )
        {
            socket = true;
            EXPECT_EQ( row.Kind, ReferenceKind::Containment );
        }
        if ( row.ComponentKey == "Projectile" && row.Field == "Owner" )
        {
            projectile = true;
            EXPECT_EQ( row.Kind, ReferenceKind::Observation );
        }
    }
    EXPECT_TRUE( socket );
    EXPECT_TRUE( projectile );
}

// THE BLIND SPOT, PINNED. Two references in this engine address their target by a NAME, so the
// shape-based discovery below cannot see them and they are listed by hand. Pinning the rows is what
// stops the hand-written half being quietly emptied: a count would be satisfied by editing the count.
TEST( WorldPartitionReferences, TheNameAddressedReferencesAreNamedAndClassified )
{
    bool overlay = false;
    bool screen  = false;
    for ( const auto& row : kEntityReferencesByName )
    {
        if ( row.ComponentKey == "UIOverlayTrigger" && row.Field == "Overlay" )
        {
            overlay = true;
            EXPECT_EQ( row.TargetComponentKey, "UIOverlay" );
            // Observation because UIOverlay::OverlayByName returns an error when nothing carries the
            // name and the caller opens nothing - the reader already handles absence.
            EXPECT_EQ( row.Kind, ReferenceKind::Observation );
        }
        if ( row.ComponentKey == "UIScreenStack" && row.Field == "InitialScreen" )
        {
            screen = true;
            EXPECT_EQ( row.TargetComponentKey, "UIScreen" );
            EXPECT_EQ( row.Kind, ReferenceKind::Observation );
        }
    }
    EXPECT_TRUE( overlay );
    EXPECT_TRUE( screen );

    // NEITHER IS CONTAINMENT, and that matters: a name-addressed reference could not be enforced by
    // this partitioner without an index from name to record, so a Containment row here would be a
    // promise nothing keeps. If one ever appears, it costs that index.
    for ( const auto& row : kEntityReferencesByName )
        EXPECT_EQ( row.Kind, ReferenceKind::Observation ) << row.ComponentKey;
}

// A reference nobody wrote a row for is FOUND. This is the defect that would otherwise survive this
// whole task: a component added later that holds an entity id, classified by nobody, cut through by the
// partitioner in silence. The discovery is by the SHAPE of the data, not by a list.
TEST( WorldPartitionReferences, AReferenceWithNoRowInTheRegisterIsFound )
{
    std::vector<EntityData> records;
    records.push_back( Record( 4242, "Door", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 4243, "Lever", { 0.0f, 0.0f, 0.0f } ) );

    rfl::Generic::Object block;
    block["Opens"]                       = rfl::Generic( std::string( "4242" ) );
    records[1].Components["DoorLinkage"] = rfl::Generic( block );

    const auto found = FindUnregisteredEntityReferences( records );
    ASSERT_EQ( found.size(), 1u );
    EXPECT_EQ( found[0].Record, 1u );
    EXPECT_EQ( found[0].ComponentKey, "DoorLinkage" );
    EXPECT_EQ( found[0].Field, "Opens" );

    // And a reference that DOES have a row is not reported - otherwise the check above would also fire
    // on every socket in the repository and the corpus gate below would be meaningless.
    std::vector<EntityData> registered;
    registered.push_back( Record( 4242, "Door", { 0.0f, 0.0f, 0.0f } ) );
    registered.push_back( Record( 4243, "Lever", { 0.0f, 0.0f, 0.0f } ) );
    SocketedTo( registered[1], 4242 );
    EXPECT_TRUE( FindUnregisteredEntityReferences( registered ).empty() );
}

// THE CORPUS GATE. Every scene the repository ships: no entity-to-entity reference that the register
// does not classify. A component added later with a UUID in it turns this red the moment a scene uses
// one, which is the earliest anything can notice.
TEST( WorldPartitionReferences, NoSceneInTheRepositoryHoldsAnUnclassifiedEntityReference )
{
    for ( const auto& path : RepositoryScenes() )
    {
        const auto parsed = rfl::json::read<SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();

        const auto found = FindUnregisteredEntityReferences( parsed->Entities );
        for ( const auto& reference : found )
        {
            ADD_FAILURE() << path.string() << ": entity reference '" << reference.ComponentKey << '.'
                          << reference.Field << "' names entity "
                          << static_cast<std::uint64_t>( reference.NamedId )
                          << " and has no row in kEntityReferences. Classify it as Containment or "
                             "Observation in WorldPartitionRules.hpp.";
        }
    }
}

// THE CORPUS, PARTITIONED, AND CHECKED AS A RELATION RATHER THAN AGAINST ITSELF.
//
// For every scene the repository ships, at a cell of one metre (where composites do cross edges) and at
// the format's default: every composite is in exactly one place - one cell, or the always-loaded set -
// and a placed one is in the cell it names, at the level it names; its footprint lies inside that
// cell's square; no LOWER level has one cell holding it (so the level is the lowest, not merely one that
// works). And - the part computed WITHOUT the planner - every root record's own translation, and every
// instance of every InstancedStaticMesh read straight from the file's JSON, lies inside its cell's square.
//
// The instrument is shown non-zero: at one metre some composite must be promoted, and some scene must
// have a component-global composite (a sun, a camera), or this sweep proves nothing about either.
TEST( WorldPartitionReferences, EveryCorpusPointLiesInsideTheSquareOfItsCompositesCell )
{
    std::size_t promotedAtOneMetre = 0;
    std::size_t globalByComponent  = 0;
    std::size_t instancesSeen      = 0;
    for ( const auto& path : RepositoryScenes() )
    {
        const std::string text   = ReadAll( path );
        const auto        parsed = rfl::json::read<SceneSerialized>( text );
        ASSERT_TRUE( parsed.has_value() ) << path.string();
        const auto& records = parsed->Entities;

        for ( const float cellSize : { 100.0f, 12800.0f } )
        {
            const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( cellSize ) );
            if ( cellSize == 100.0f && plan.MaxLevel > 0 )
                ++promotedAtOneMetre;

            std::vector<std::size_t> cellOfComposite( plan.Composites.size(), kNoRecord );
            for ( std::size_t index = 0; index < plan.Cells.size(); ++index )
            {
                const PlannedCell& cell = plan.Cells[index];
                for ( const std::size_t held : cell.Composites )
                {
                    EXPECT_EQ( cellOfComposite[held], kNoRecord ) << "a composite in two cells: " << path.string();
                    cellOfComposite[held]             = index;
                    const PlannedComposite& composite = plan.Composites[held];
                    EXPECT_EQ( composite.Cell, cell.Cell ) << path.string();
                    EXPECT_EQ( composite.Level, cell.Level ) << path.string();
                    if ( !composite.Footprint.has_value() )
                        continue;
                    const CellBounds& box = *composite.Footprint;
                    EXPECT_TRUE( box.MinX >= cell.Square.MinX && box.MaxX <= cell.Square.MaxX &&
                                 box.MinZ >= cell.Square.MinZ && box.MaxZ <= cell.Square.MaxZ )
                         << path.string() << ": composite " << held << " overhangs its level-" << cell.Level
                         << " cell";
                    if ( cell.Level > 0 )
                        EXPECT_FALSE(
                             SingleCellHolding( box, LevelCellSize( cellSize, cell.Level - 1 ) ).has_value() )
                             << path.string() << ": composite " << held << " fits level " << cell.Level - 1
                             << " but was put on level " << cell.Level;
                }
            }
            for ( const std::size_t held : plan.AlwaysLoaded )
            {
                EXPECT_EQ( cellOfComposite[held], kNoRecord ) << "always-loaded AND in a cell: " << path.string();
                cellOfComposite[held] = plan.Cells.size();
                if ( cellSize == 100.0f && plan.Composites[held].Reason == AlwaysLoadedReason::Component )
                    ++globalByComponent;
            }
            for ( const std::size_t held : cellOfComposite )
                EXPECT_NE( held, kNoRecord ) << "a composite in no cell and not always-loaded: " << path.string();

            const auto Inside = [&]( std::size_t record, float x, float z )
            {
                const std::size_t where = cellOfComposite[CompositeOf( plan, record )];
                if ( where == plan.Cells.size() )
                    return; // always-loaded: no square to be inside
                const CellBounds& square = plan.Cells[where].Square;
                EXPECT_TRUE( x >= square.MinX && x <= square.MaxX && z >= square.MinZ && z <= square.MaxZ )
                     << path.string() << ": record " << record << " at (" << x << ", " << z << ")";
            };

            // Root records: no parent, so the stated translation IS the world position.
            for ( std::size_t record = 0; record < records.size(); ++record )
            {
                if ( records[record].parent.has_value() && !records[record].parent->IsNull() )
                    continue;
                if ( records[record].Translation.has_value() )
                    Inside( record, records[record].Translation->x, records[record].Translation->z );
            }

            // Instances, read from the raw JSON and not through the planner's own reader.
            const auto document = rfl::json::read<rfl::Generic>( text );
            ASSERT_TRUE( document.has_value() );
            const auto entities = document->to_object().value().get( "Entities" );
            ASSERT_TRUE( entities.has_value() );
            const auto list = entities->to_array().value();
            ASSERT_EQ( list.size(), records.size() ) << path.string();
            for ( std::size_t record = 0; record < list.size(); ++record )
            {
                const auto ism = list[record].to_object().value().get( std::string( kInstancePointsComponent ) );
                if ( !ism.has_value() )
                    continue;
                const auto matrices = ism->to_object().value().get( std::string( kInstancePointsField ) );
                if ( !matrices.has_value() )
                    continue;
                // Held by name: `to_array()` returns a temporary, and a range-for over `.value()` of it
                // iterates a destroyed array - which read as "no instances" rather than crashing.
                const auto array = matrices->to_array();
                ASSERT_TRUE( array.has_value() ) << path.string();
                for ( const auto& matrix : array.value() )
                {
                    const auto m = matrix.to_array().value();
                    ASSERT_EQ( m.size(), 16u );
                    const auto Number = []( const rfl::Generic& value )
                    {
                        const auto real = value.to_double();
                        return real.has_value() ? static_cast<float>( real.value() )
                                                : static_cast<float>( value.to_int64().value() );
                    };
                    Inside( record, Number( m[12] ), Number( m[14] ) );
                    ++instancesSeen;
                }
            }
        }
    }
    EXPECT_GT( promotedAtOneMetre, 0u ) << "no scene promotes a composite at one metre, so this sweep proves "
                                           "nothing about levels";
    EXPECT_GT( globalByComponent, 0u ) << "no corpus composite is global by component, so the always-loaded "
                                          "half is vacuous";
    EXPECT_GT( instancesSeen, 0u ) << "no InstancedStaticMesh instance was checked, so the ISM half is vacuous";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
