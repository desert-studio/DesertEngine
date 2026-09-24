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

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <optional>

namespace
{
    // A fixture's text header stating the given generations; an absent one is not stated at all, which is
    // how a file that predates one of the two numbers reads since v26 moved them into the header. One
    // fixed GUID: fixtures built twice must be the same bytes, as two saves of one asset are.
    Common::Content::TextAssetHeaderSerialized FixtureHeader( Common::Content::ContentKind kind,
                                                              std::optional<int>           sceneVersion,
                                                              std::optional<int>           unitVersion )
    {
        std::vector<Common::Content::SubsystemVersion> versions;
        if ( sceneVersion )
            versions.push_back( { Desert::Assets::kSceneSchemaTag, static_cast<uint32_t>( *sceneVersion ) } );
        if ( unitVersion )
            versions.push_back( { Desert::Assets::kUnitSchemaTag, static_cast<uint32_t>( *unitVersion ) } );
        const auto guid = Common::Content::AssetGuidFromText( "0f1e2d3c4b5a69788796a5b4c3d2e1f0" );
        return Common::Content::MakeTextHeader( kind, guid.GetValue(), versions );
    }
} // namespace

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
using Desert::Core::Rules::kLandscapeTileComponent;
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
        for ( auto it = std::filesystem::recursive_directory_iterator( root, ec );
              it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
        {
            // Scenes/Autosave is .gitignore'd: the editor writes it on THIS machine only, so a census that
            // walked it would pin numbers no clean clone can reproduce (it moved the corpus by 17 records).
            if ( it->is_directory() && it->path().filename() == "Autosave" )
            {
                it.disable_recursion_pending();
                continue;
            }
            if ( it->is_regular_file() && it->path().extension() == ".desce" )
                scenes.push_back( it->path() );
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
        const std::regex call( R"((^|[^\w>.:])Register\s*\()" );
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
    scene.Header       = FixtureHeader( Common::Content::ContentKind::Scene, Desert::Core::kSceneVersion,
                                        Desert::Core::kUnitVersion );

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
    scene.Header         = FixtureHeader( Common::Content::ContentKind::Scene, Desert::Core::kSceneVersion,
                                          Desert::Core::kUnitVersion );
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
    ASSERT_EQ( read->WorldPartition->Grids.size(), 1u ); // NOLINT(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( read->WorldPartition->Grids[0].CellSize, 25600.0f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( read->WorldPartition->Grids[0].LoadingRange, 76800.0f );
    // NOLINTEND(bugprone-unchecked-optional-access)
}

// A partitioned world is still at the CURRENT generation - the block is additive, so nothing had to be
// converted and the version gate cannot start refusing files over it.
TEST( WorldPartitionFormat, PartitioningAWorldDoesNotMoveItsVersion )
{
    SceneSerialized scene;
    scene.Header         = FixtureHeader( Common::Content::ContentKind::Scene, Desert::Core::kSceneVersion,
                                          Desert::Core::kUnitVersion );
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
    onDisk.Header         = FixtureHeader( Common::Content::ContentKind::Scene, Desert::Core::kSceneVersion,
                                           Desert::Core::kUnitVersion );
    onDisk.WorldPartition = Cells( 51200.0f, 102400.0f );

    SceneSerialized fresh;
    fresh.SceneName    = onDisk.SceneName;
    fresh.Header       = onDisk.Header;

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
    ASSERT_EQ( reread->WorldPartition->Grids.size(), 1u ); // NOLINT(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( reread->WorldPartition->Grids[0].CellSize, 51200.0f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( reread->WorldPartition->Grids[0].LoadingRange, 102400.0f );
    // NOLINTEND(bugprone-unchecked-optional-access)
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
    EXPECT_EQ( *exact, ( CellCoord{ 1, 0 } ) ); // NOLINT(bugprone-unchecked-optional-access)

    EXPECT_FALSE( SingleCellHolding( CellBounds{ 100.0f, 0.0f, 201.0f, 100.0f }, 100.0 ).has_value() );
    EXPECT_FALSE( SingleCellHolding( CellBounds{ 99.0f, 0.0f, 150.0f, 50.0f }, 100.0 ).has_value() );

    const auto point = SingleCellHolding( CellBounds{ 200.0f, -100.0f, 200.0f, -100.0f }, 100.0 );
    ASSERT_TRUE( point.has_value() );
    EXPECT_EQ( *point, ( CellCoord{ 2, -1 } ) ); // NOLINT(bugprone-unchecked-optional-access)

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
    EXPECT_FLOAT_EQ( plan.Composites[0].Footprint->MaxX, 30100.0f ); // NOLINT(bugprone-unchecked-optional-access)
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
    EXPECT_NEAR( held.Footprint->MinZ, 10000.0f, 1.0f ); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_NEAR( held.Footprint->MaxX, 5000.0f, 1.0f );  // NOLINT(bugprone-unchecked-optional-access)
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
    const CellBounds& footprint = *plan.Composites[0].Footprint; // NOLINT(bugprone-unchecked-optional-access)
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

// THE CORPUS, BEFORE AND AFTER: how many records are placed by their position alone, with no registry and
// with the one the editor gathers. The numbers are the task's acceptance (WP15), derived from the files:
//
//   * 2543 before WP15 - only the Cube, the Terrain and instanced meshes had an extent;
//   * 2462 with no registry - the 81 Spheres now have the factory's box (Geometry::PrimitiveBounds);
//   * with the gathered registry, every mesh-asset record whose row carries Bounds leaves as well;
//   * +4 with M4 (2466 / 2434): M4_RampNormalMap.desce, whose static meshes carry their geometry as an
//     in-scene EditMesh with no asset, so neither the registry nor a primitive box answers for them yet;
//   * -4 with M5 (2462 / 2430): the two Cylinders and two Capsules of Starter and Desert_Sandbox, which
//     the factory built nothing for until the primitives moved onto ShapeGenerators and got their boxes.
//   * +6 with LS-6 (2468 / 2436): the six procedural terrains baked into landscapes (two each in
//     G26_TerrainRockLayer and G3_TwoTerrains, one each in Terrain_Grass and Terrain_MatProbe). The old
//     `Terrain` block carried a Size x Size box; the root that keeps its id holds Landscape and
//     LandscapeMaterial, which have no extent of their own. Its 25 tiles each get their rectangle from the
//     root's frame, so the land is still covered; only the root record is point-only.
//   * AF9 (2468 / 2436, all 32 mesh references answered): the registry is no longer committed; the editor gathers
//   it, and a mesh row's box
//     comes from the mesh's own 64-byte header. Derived in a clean clone of 1de8ad1a, where only tracked
//     meshes can answer - a developer's ignored local meshes under Editor/Cooked would answer too.
//
// The mesh references are resolved as the loader resolves them - handle, else path - and a path is
// relative to the editor's working directory, so the walk runs from there.
namespace
{
    // How many mesh-asset records (StaticMesh or SkinnedMesh naming a file) the corpus has, and how many
    // records stay point-only once the gathered registry answers for them.
    constexpr std::size_t kCorpusMeshReferences        = 32;
    constexpr std::size_t kCorpusPointOnlyWithRegistry = 2436;

    // The editor's project, opened the way the editor opens it: cwd = Editor/ (engine resource roots and
    // scene mesh paths resolve against it) and the project root set from Desert.deproj. Restored on exit.
    class EditorProject
    {
    public:
        explicit EditorProject( const std::string& repoRoot )
             : m_SavedRoot( Common::Constants::Path::CurrentProjectRoot() ),
               m_SavedCwd( std::filesystem::current_path() )
        {
            const std::filesystem::path editorDir = std::filesystem::absolute( repoRoot + "Editor" );
            std::filesystem::current_path( editorDir );
            const auto project = Common::Project::ReadProjectFile( ReadAll( editorDir / "Desert.deproj" ) );
            if ( !project )
                return;
            Common::Constants::Path::SetProjectRoot( editorDir, project.GetValue().AssetsRoot );
            m_Opened = true;
        }
        ~EditorProject()
        {
            Common::Constants::Path::SetProjectRoot( m_SavedRoot.ProjectDir, m_SavedRoot.AssetsRoot );
            std::error_code ec;
            std::filesystem::current_path( m_SavedCwd, ec );
        }
        EditorProject( const EditorProject& )            = delete;
        EditorProject& operator=( const EditorProject& ) = delete;
        bool           Opened() const
        {
            return m_Opened;
        }

    private:
        Common::Constants::Path::ProjectRootState m_SavedRoot;
        std::filesystem::path                     m_SavedCwd;
        bool                                      m_Opened = false;
    };
} // namespace

TEST( WorldPartitionMeshAssets, TheCorpusHasFewerPointOnlyRecordsWithTheGatheredRegistry )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<std::filesystem::path> scenes = RepositoryScenes();
    ASSERT_FALSE( scenes.empty() );

    std::vector<std::filesystem::path> absolute;
    absolute.reserve( scenes.size() );
    for ( const auto& scene : scenes )
        absolute.push_back( std::filesystem::absolute( scene ) );
    const EditorProject project( root );
    ASSERT_TRUE( project.Opened() );
    const Common::Content::GatheredRegistry gathered = Common::Content::GatherContentRegistry( {} );
    ASSERT_TRUE( gathered.Refused.empty() ) << gathered.Refused.front();

    const Common::Utils::AssetRegistry&          rows    = gathered.Registry;
    std::size_t                                  asked   = 0;
    std::size_t                                  answers = 0;
    const Desert::Core::Rules::AssetBoundsSource source =
         [&]( const Common::Content::AssetGuid& guid, std::string_view path ) -> std::optional<Common::Math::AABB>
    {
        ++asked;
        const Common::Utils::AssetRegistryEntry* row = rows.FindByGuidReference( guid, path );
        if ( row == nullptr || !row->Bounds.has_value() )
            return std::nullopt;
        ++answers;
        return row->Bounds;
    };

    std::size_t blind = 0;
    std::size_t seen  = 0;
    for ( const auto& path : absolute )
    {
        const auto parsed = rfl::json::read<SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();
        blind += PlanWorldPartition( parsed->Entities, Cells( 12800.0f ) ).PointOnlyRecords;
        seen += PlanWorldPartition( parsed->Entities, Cells( 12800.0f ), source ).PointOnlyRecords;
    }

    EXPECT_EQ( blind, 2468u );
    EXPECT_EQ( asked, kCorpusMeshReferences ) << "every mesh-asset record of the corpus is asked once";
    EXPECT_EQ( seen, blind - answers ) << "each answered mesh must take exactly one record off the count";
    std::printf( "[corpus] %zu answered of %zu asked; point-only %zu blind, %zu with the registry\n", answers,
                 asked, blind, seen );
    EXPECT_EQ( seen, kCorpusPointOnlyWithRegistry );
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
    EXPECT_FLOAT_EQ( held.Footprint->MaxX, 50000.0f ); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( held.Footprint->MaxZ, 30000.0f ); // NOLINT(bugprone-unchecked-optional-access)
    // X 10..50000 needs an 80000 cell: level 3.
    EXPECT_EQ( held.Level, 3 );
    EXPECT_EQ( plan.PointOnlyRecords, 0u );
}

// THE PRIMITIVE CUBE HAS A FOOTPRINT, and it is the corners through the world matrix. The same record
// naming a Cylinder has the same footprint (both fill the unit box since M5 built the Cylinder), and naming
// a LightCube - which is never a mesh block's primitive and has no box - is a point: the trio shows the
// footprint moving the answer, not an instrument that cannot tell them apart.
TEST( WorldPartitionLevels, APrimitiveCubeIsItsCornersAndAShapelessPrimitiveIsItsPosition )
{
    std::vector<EntityData> records;
    // Centred 40 short of the 10000 edge with a 100 cm half-extent after a scale of 2: 9860..10060.
    records.push_back( Record( 1, "Crate", { 9960.0f, 0.0f, 500.0f } ) );
    records[0].Scale = glm::vec3( 2.0f );
    With( records[0], "StaticMesh", R"({"Primitive":"Cube"})" );

    const WorldPartitionPlan cube = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_TRUE( cube.Composites[0].Footprint.has_value() );
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( cube.Composites[0].Footprint->MinX, 9860.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( cube.Composites[0].Footprint->MaxX, 10060.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( cube.Composites[0].Level, 1 );
    EXPECT_EQ( cube.PointOnlyRecords, 0u );

    With( records[0], "StaticMesh", R"({"Primitive":"Cylinder"})" );
    const WorldPartitionPlan cylinder = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_TRUE( cylinder.Composites[0].Footprint.has_value() );
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( cylinder.Composites[0].Footprint->MinX, 9860.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( cylinder.Composites[0].Footprint->MaxX, 10060.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( cylinder.Composites[0].Level, 1 );
    EXPECT_EQ( cylinder.PointOnlyRecords, 0u );

    With( records[0], "StaticMesh", R"({"Primitive":"LightCube"})" );
    const WorldPartitionPlan shapeless = PlanWorldPartition( records, Cells( 10000.0f ) );
    EXPECT_EQ( shapeless.Composites[0].Level, 0 );
    EXPECT_EQ( shapeless.PointOnlyRecords, 1u );
}

// EVERY PRIMITIVE THE FACTORY DRAWS HAS THE BOX THE FACTORY STAMPS - Geometry::PrimitiveBounds, one
// statement read by both. The Sphere is as wide as the Cube (radius 50); the Plane is a card in XY with no
// depth, so its footprint is 100 in X and a line in Z.
TEST( WorldPartitionLevels, ASphereAndAPlaneHaveTheBoxesTheFactoryStamps )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Ball", { 9960.0f, 0.0f, 500.0f } ) );
    With( records[0], "StaticMesh", R"({"Primitive":"Sphere"})" );
    const WorldPartitionPlan ball = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_TRUE( ball.Composites[0].Footprint.has_value() );
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( ball.Composites[0].Footprint.value().MaxX, 10010.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( ball.Composites[0].Level, 1 ) << "the ball crosses the 10000 edge";
    EXPECT_EQ( ball.PointOnlyRecords, 0u );

    With( records[0], "StaticMesh", R"({"Primitive":"Plane"})" );
    const WorldPartitionPlan card = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_TRUE( card.Composites[0].Footprint.has_value() );
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( card.Composites[0].Footprint.value().MinX, 9910.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( card.Composites[0].Footprint.value().MaxX, 10010.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( card.Composites[0].Footprint.value().MinZ, 500.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( card.Composites[0].Footprint.value().MaxZ, 500.0f, 0.01f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( card.PointOnlyRecords, 0u );

    // The unknown spelling is not a shape, and is its position.
    With( records[0], "StaticMesh", R"({"Primitive":"Dodecahedron"})" );
    EXPECT_EQ( PlanWorldPartition( records, Cells( 10000.0f ) ).PointOnlyRecords, 1u );
}

// ── MESH ASSETS: THE EXTENT THAT LIVES IN ANOTHER FILE (WP15) ─────────────────────────────────────────

namespace
{
    // A registry of one mesh, the way the loader's source answers: by header GUID, else by path.
    Desert::Core::Rules::AssetBoundsSource OneMesh( Common::Content::AssetGuid guid, const std::string& path,
                                                    Common::Math::AABB box )
    {
        // NOLINTNEXTLINE(bugprone-exception-escape): test fixture
        return [=]( const Common::Content::AssetGuid& asked,
                    std::string_view                  named ) -> std::optional<Common::Math::AABB>
        {
            if ( ( !asked.IsNull() && asked == guid ) || ( !named.empty() && named == path ) )
                return box;
            return std::nullopt;
        };
    }

    // A mesh GUID and the text a scene writes it as (SCNE 28: 32 lower-case hex digits, Hi then Lo).
    constexpr Common::Content::AssetGuid kBridgeGuid{ 0x4f1c2a9e7b3d5a10ull, 0x9e8d7c6b5a493827ull };
    constexpr const char*                kBridgeGuidText = "4f1c2a9e7b3d5a109e8d7c6b5a493827";
} // namespace

// THE ACCEPTANCE CASE: A KILOMETRE BRIDGE AS ONE MODEL. Its entity sits at (1000, 0, 1000); the mesh runs
// 100000 units along its local X. Without the registry it is a point and goes to level 0 - the wrong
// answer the task exists to remove. With it, it is the level whose cell holds a kilometre (level 3 for a
// 12800 cell, as the two-record bridge above), and it is no longer counted as point-only.
TEST( WorldPartitionMeshAssets, AKilometreBridgeModelGoesToTheLevelThatHoldsIt )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Bridge", { 1000.0f, 0.0f, 1000.0f } ) );
    With( records[0], "StaticMesh",
          R"({"MeshGuid":"4f1c2a9e7b3d5a109e8d7c6b5a493827","MeshPath":"Cooked/Meshes/Bridge.stmesh"})" );
    records.push_back( Record( 2, "FarRock", { 900000.0f, 0.0f, 900000.0f } ) );

    const WorldPartitionPlan blind = PlanWorldPartition( records, Cells( 12800.0f ) );
    EXPECT_EQ( HeldBy( blind, 0 ).Level, 0 );
    EXPECT_EQ( blind.PointOnlyRecords, 2u );

    const auto source = OneMesh(
         kBridgeGuid, "",
         Common::Math::AABB{ glm::vec3( 0.0f, -500.0f, -300.0f ), glm::vec3( 100000.0f, 800.0f, 300.0f ) } );
    const WorldPartitionPlan seen   = PlanWorldPartition( records, Cells( 12800.0f ), source );
    const PlannedComposite&  bridge = HeldBy( seen, 0 );
    ASSERT_TRUE( bridge.Footprint.has_value() );
    EXPECT_NEAR( bridge.Footprint.value().MaxX, 101000.0f, 0.5f ); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_NEAR( bridge.Footprint.value().MinZ, 700.0f, 0.5f );    // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_EQ( bridge.Level, 3 );
    EXPECT_EQ( seen.PointOnlyRecords, 1u ) << "only the rock, which names no mesh, is still a point";
}

// THE BOX GOES THROUGH THE WHOLE WORLD MATRIX: a parent's quarter turn swings a mesh that runs along local
// X onto world Z, and a scale of 2 doubles it. A footprint that only translated the box would leave it on X.
TEST( WorldPartitionMeshAssets, TheBoxIsCarriedByTheWorldMatrixNotJustThePosition )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Pivot", { 0.0f, 0.0f, 0.0f } ) );
    records[0].Rotation = glm::vec3( 0.0f, glm::half_pi<float>(), 0.0f );
    records.push_back( Record( 2, "Beam", { 500.0f, 0.0f, 500.0f } ) );
    records[1].Scale = glm::vec3( 2.0f );
    Under( records[1], 1 );
    With( records[1], "StaticMesh", R"({"MeshPath":"Cooked/Meshes/Beam.stmesh"})" );

    const auto source =
         OneMesh( {}, "Cooked/Meshes/Beam.stmesh",
                  Common::Math::AABB{ glm::vec3( 0.0f, 0.0f, -10.0f ), glm::vec3( 1000.0f, 10.0f, 10.0f ) } );
    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ), source );
    const PlannedComposite&  held = HeldBy( plan, 1 );
    ASSERT_TRUE( held.Footprint.has_value() );
    // Pivot turns +X onto -Z. Beam at local (500, 0, 500) -> world (500, 0, -500); its box runs 2000 along
    // local X -> world -Z, from -500 to -2500, and is 40 wide in X around 500. The pivot's own position,
    // the origin, is the composite's other corner.
    EXPECT_NEAR( held.Footprint.value().MinZ, -2500.0f, 0.5f ); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_NEAR( held.Footprint.value().MaxZ, 0.0f, 0.5f );     // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_NEAR( held.Footprint.value().MaxX, 520.0f, 0.5f );   // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_NEAR( held.Footprint.value().MinX, 0.0f, 0.5f );     // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_EQ( plan.PointOnlyRecords, 1u ) << "the pivot names nothing and stays a point";
}

// A SKINNED MESH IS ASKED THE SAME WAY, BY ALL 128 BITS OF ITS GUID TEXT.
TEST( WorldPartitionMeshAssets, ASkinnedMeshIsAskedByItsExactGuid )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Rig", { 0.0f, 0.0f, 0.0f } ) );
    With( records[0], "SkinnedMesh", ( std::string( R"({"MeshGuid":")" ) + kBridgeGuidText + R"("})" ).c_str() );

    const auto               source = OneMesh( kBridgeGuid, "",
                                               Common::Math::AABB{ glm::vec3( -30.0f, 0.0f, -20000.0f ), glm::vec3( 30.0f ) } );
    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ), source );
    ASSERT_TRUE( plan.Composites[0].Footprint.has_value() );
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( plan.Composites[0].Footprint.value().MinZ, -20000.0f, 0.5f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( plan.PointOnlyRecords, 0u ) << "the exact GUID was not found";
}

// A SOURCE THAT DOES NOT KNOW THE MESH LEAVES IT A POINT AND COUNTED - never a guessed box.
TEST( WorldPartitionMeshAssets, AnUnknownMeshIsItsPositionAndIsCounted )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Prop", { 20.0f, 0.0f, 20.0f } ) );
    With( records[0], "StaticMesh", ( std::string( R"({"MeshGuid":")" ) + kBridgeGuidText + R"("})" ).c_str() );

    // One bit away in Lo: a different asset.
    const auto source             = OneMesh( Common::Content::AssetGuid{ kBridgeGuid.Hi, kBridgeGuid.Lo ^ 1u }, "",
                                             Common::Math::AABB{ glm::vec3( -1.0e6f ), glm::vec3( 1.0e6f ) } );
    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ), source );
    EXPECT_EQ( plan.PointOnlyRecords, 1u );
    EXPECT_EQ( plan.Composites[0].Level, 0 );
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

// ── 6a. A LANDSCAPE IS PARTITIONED BY TILE ─────────────────────────────────────────────────────────
//
// A root and a 2 x 2 grid of tiles, 63 quads of one metre each — 6300 cm a tile — on a grid of 6300 cm, so
// every tile is exactly one level-0 cell. What must hold: every tile is its own composite, at level 0, in
// the cell its rectangle covers — a tile does not drag its neighbours or its root up a level. The root owns
// the frame and is always-loaded (as UE loads ALandscape). The tile ENTITIES are placed far away on purpose:
// a tile's own transform is read by nothing, and a partitioner that used it would put all four elsewhere.
namespace
{
    constexpr std::uint64_t kLandscapeRootId = 9000;
    constexpr float         kTileCm          = 63.0f * 100.0f;

    EntityData LandscapeRootRecord( glm::vec3 origin, std::int64_t quads = 63 )
    {
        EntityData           root = Record( kLandscapeRootId, "Landscape", origin );
        rfl::Generic::Object block;
        block["QuadsPerTile"]        = rfl::Generic( quads );
        block["SpacingCm"]           = rfl::Generic( 100.0 );
        block["ZScale"]              = rfl::Generic( 100.0 );
        root.Components["Landscape"] = rfl::Generic( block );
        return root;
    }

    EntityData LandscapeTileRecord( std::uint64_t id, std::int64_t x, std::int64_t z,
                                    std::uint64_t root = kLandscapeRootId )
    {
        EntityData           tile = Record( id, "LandscapeTile", { 987654.0f, 0.0f, -987654.0f } );
        rfl::Generic::Object block;
        block["Landscape"]               = rfl::Generic( std::to_string( root ) );
        block["TileX"]                   = rfl::Generic( x );
        block["TileZ"]                   = rfl::Generic( z );
        block["HeightFile"]              = rfl::Generic( "Scenes/L_Landscape/" + std::to_string( id ) + ".dlht" );
        tile.Components["LandscapeTile"] = rfl::Generic( block );
        return tile;
    }

    std::vector<EntityData> TwoByTwoLandscape()
    {
        std::vector<EntityData> records;
        records.push_back( LandscapeRootRecord( glm::vec3( 0.0f ) ) );
        records.push_back( LandscapeTileRecord( 9001, 0, 0 ) );
        records.push_back( LandscapeTileRecord( 9002, 1, 0 ) );
        records.push_back( LandscapeTileRecord( 9003, 0, 1 ) );
        records.push_back( LandscapeTileRecord( 9004, 1, 1 ) );
        return records;
    }
} // namespace

TEST( WorldPartitionLandscape, EveryTileIsItsOwnCompositeAtLevelZeroInItsOwnCell )
{
    const auto               records = TwoByTwoLandscape();
    const WorldPartitionPlan plan    = PlanWorldPartition( records, Cells( kTileCm ) );

    EXPECT_TRUE( plan.UnplacedLandscapeTiles.empty() );
    ASSERT_EQ( plan.Composites.size(), 5u ) << "the root and four tiles, none joined to another";
    for ( std::size_t record = 0; record < records.size(); ++record )
        EXPECT_EQ( HeldBy( plan, record ).Members.size(), 1u ) << records[record].Tag.value_or( "" );

    EXPECT_EQ( HeldBy( plan, 0 ).Reason, AlwaysLoadedReason::Component ) << "the root owns the frame";

    const struct
    {
        std::size_t  Record;
        std::int32_t X, Z;
    } expected[] = { { 1, 0, 0 }, { 2, 1, 0 }, { 3, 0, 1 }, { 4, 1, 1 } };
    for ( const auto& tile : expected )
    {
        const PlannedComposite& held = HeldBy( plan, tile.Record );
        EXPECT_EQ( held.Reason, AlwaysLoadedReason::None ) << "tile record " << tile.Record;
        EXPECT_EQ( held.Level, 0 ) << "tile record " << tile.Record;
        EXPECT_EQ( held.Cell.X, tile.X ) << "tile record " << tile.Record;
        EXPECT_EQ( held.Cell.Z, tile.Z ) << "tile record " << tile.Record;
        // The tile's footprint IS its rectangle: all four corners, nothing of its far-away entity position.
        ASSERT_TRUE( held.Footprint.has_value() );
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        EXPECT_FLOAT_EQ( held.Footprint.value().MinX, tile.X * kTileCm );
        // NOLINTEND(bugprone-unchecked-optional-access)
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        EXPECT_FLOAT_EQ( held.Footprint.value().MaxX, ( tile.X + 1 ) * kTileCm );
        // NOLINTEND(bugprone-unchecked-optional-access)
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        EXPECT_FLOAT_EQ( held.Footprint.value().MinZ, tile.Z * kTileCm );
        // NOLINTEND(bugprone-unchecked-optional-access)
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        EXPECT_FLOAT_EQ( held.Footprint.value().MaxZ, ( tile.Z + 1 ) * kTileCm );
        // NOLINTEND(bugprone-unchecked-optional-access)
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    EXPECT_EQ( plan.Cells.size(), 4u );
    EXPECT_EQ( plan.MaxLevel, 0 ) << "no tile pulls a neighbour or its root up a level";
    EXPECT_TRUE( FindUnregisteredEntityReferences( records ).empty() );
}

// THE NEGATIVE CONTROL for the test above, in the two shapes the refused relation takes. Tiles made CHILDREN
// of the root are one composite with it, and since the root owns the frame that composite is ALWAYS-LOADED —
// the whole terrain resident everywhere. Tiles made children of one another (no root in the union) are one
// composite too, and it goes UP A LEVEL to find a cell wide enough. Either is what the Observation row
// prevents; were the row Containment, the test above would read like one of these.
TEST( WorldPartitionLandscape, TilesJoinedByHierarchyBecomeOneCompositeThatIsResidentOrPromoted )
{
    auto underRoot = TwoByTwoLandscape();
    for ( std::size_t record = 1; record < underRoot.size(); ++record )
    {
        Under( underRoot[record], kLandscapeRootId );
        underRoot[record].Translation = glm::vec3( 0.0f );
    }
    const WorldPartitionPlan resident = PlanWorldPartition( underRoot, Cells( kTileCm ) );
    ASSERT_EQ( resident.Composites.size(), 1u );
    EXPECT_EQ( resident.Composites[0].Reason, AlwaysLoadedReason::Component );
    EXPECT_TRUE( resident.Cells.empty() );

    auto underTile = TwoByTwoLandscape();
    for ( std::size_t record = 2; record < underTile.size(); ++record )
    {
        Under( underTile[record], 9001 );
        underTile[record].Translation = glm::vec3( 0.0f );
    }
    const WorldPartitionPlan promoted = PlanWorldPartition( underTile, Cells( kTileCm ) );
    ASSERT_EQ( promoted.Composites.size(), 2u ) << "the root, and the four tiles as one";
    EXPECT_EQ( HeldBy( promoted, 1 ).Members.size(), 4u );
    EXPECT_GT( HeldBy( promoted, 1 ).Level, 0 );
    EXPECT_GT( promoted.MaxLevel, 0 );
}

// The rectangle follows the root's WORLD position: a moved root moves every tile, negative tile coordinates
// lie on the other side of it, and a root under a moved parent is composed like any other record.
TEST( WorldPartitionLandscape, TilesArePlacedByTheirRootsWorldFrame )
{
    std::vector<EntityData> records;
    records.push_back( Record( 8000, "World", { -2.0f * kTileCm, 0.0f, 0.0f } ) );
    records.push_back( LandscapeRootRecord( { kTileCm, 500.0f, 0.0f } ) );
    Under( records[1], 8000 );                               // root at world (-6300, 500, 0)
    records.push_back( LandscapeTileRecord( 9001, -1, 0 ) ); // [-12600, -6300] x [0, 6300]
    records.push_back( LandscapeTileRecord( 9002, 0, -1 ) ); // [-6300, 0] x [-6300, 0]

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( kTileCm ) );
    EXPECT_TRUE( plan.UnplacedLandscapeTiles.empty() );
    EXPECT_EQ( HeldBy( plan, 2 ).Level, 0 );
    EXPECT_EQ( HeldBy( plan, 2 ).Cell.X, -2 );
    EXPECT_EQ( HeldBy( plan, 2 ).Cell.Z, 0 );
    EXPECT_EQ( HeldBy( plan, 3 ).Level, 0 );
    EXPECT_EQ( HeldBy( plan, 3 ).Cell.X, -1 );
    EXPECT_EQ( HeldBy( plan, 3 ).Cell.Z, -1 );
}

// A tile with no place is LISTED and contributes no footprint: its root is not in the file, or its root
// cannot be tiled (64 quads is not a section size UE offers — the loader refuses the same root).
TEST( WorldPartitionLandscape, ATileWithoutAPlaceableRootIsListedAndHasNoFootprint )
{
    std::vector<EntityData> records;
    records.push_back( LandscapeRootRecord( glm::vec3( 0.0f ), 64 ) );
    records.push_back( LandscapeTileRecord( 9001, 0, 0 ) );            // root cannot be tiled
    records.push_back( LandscapeTileRecord( 9002, 1, 0, 123456789 ) ); // root not in this file

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( kTileCm ) );
    ASSERT_EQ( plan.UnplacedLandscapeTiles.size(), 2u );
    EXPECT_EQ( plan.UnplacedLandscapeTiles[0], 1u );
    EXPECT_EQ( plan.UnplacedLandscapeTiles[1], 2u );
    EXPECT_FALSE( HeldBy( plan, 1 ).Footprint.has_value() );
    EXPECT_FALSE( HeldBy( plan, 2 ).Footprint.has_value() );
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
                              "VolumetricCloud", "HeroCloud", "AlwaysLoaded", "Landscape" } )
        EXPECT_EQ( loadingOf( key ), ComponentLoading::Global ) << key;
    EXPECT_EQ( loadingOf( "UICanvas" ), ComponentLoading::ByField );
    EXPECT_EQ( loadingOf( "AudioSource" ), ComponentLoading::ByField );
    EXPECT_EQ( loadingOf( "StaticMesh" ), ComponentLoading::Spatial );
    EXPECT_EQ( loadingOf( "PointLight" ), ComponentLoading::Spatial );
    EXPECT_EQ( loadingOf( "LandscapeTile" ), ComponentLoading::Spatial );
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

    // The landscape tile's root. OBSERVATION, and WorldPartitionLandscape goes red if it is ever made
    // containment: every tile would join its root's composite and the whole terrain would be one footprint.
    bool landscape = false;
    for ( const auto& row : kEntityReferences )
    {
        if ( row.ComponentKey == "LandscapeTile" && row.Field == "Landscape" )
        {
            landscape = true;
            EXPECT_EQ( row.Kind, ReferenceKind::Observation );
        }
    }
    EXPECT_TRUE( landscape );
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

            // Root records: no parent, so the stated translation IS the world position. A landscape tile is
            // the exception the planner states: its own translation is read by nothing, and its place is the
            // rectangle checked below.
            for ( std::size_t record = 0; record < records.size(); ++record )
            {
                if ( records[record].parent.has_value() && !records[record].parent->IsNull() )
                    continue;
                if ( records[record].Components.get( std::string( kLandscapeTileComponent ) ).has_value() )
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

            // Landscape tiles, their rectangle worked out here by hand — tile coordinate times quads times
            // spacing, from the root's stated translation — and not through LandscapeTileBounds, which is the
            // planner's own reader. Only tiles under an unparented root with every field stated: that is
            // the case this arithmetic covers exactly, and the planner's composition is pinned elsewhere.
            for ( std::size_t record = 0; record < list.size(); ++record )
            {
                const auto tile = list[record].to_object().value().get( std::string( kLandscapeTileComponent ) );
                if ( !tile.has_value() )
                    continue;
                const auto  block  = tile->to_object().value();
                const auto  rootId = block.get( "Landscape" )->to_string().value();
                std::size_t root   = kNoRecord;
                for ( std::size_t other = 0; other < records.size(); ++other )
                    if ( records[other].id.has_value() &&
                         std::to_string( static_cast<std::uint64_t>( *records[other].id ) ) == rootId )
                        root = other;
                if ( root == kNoRecord || ( records[root].parent.has_value() && !records[root].parent->IsNull() ) )
                    continue;
                const auto      rootBlock = list[root].to_object().value().get( "Landscape" )->to_object().value();
                const auto      quads = static_cast<float>( rootBlock.get( "QuadsPerTile" )->to_int64().value() );
                const auto      spacing = static_cast<float>( rootBlock.get( "SpacingCm" )->to_double().value() );
                const auto      tileX   = static_cast<float>( block.get( "TileX" )->to_int64().value() );
                const auto      tileZ   = static_cast<float>( block.get( "TileZ" )->to_int64().value() );
                const glm::vec3 origin  = records[root].Translation.value_or( glm::vec3( 0.0f ) );
                const float     minX    = origin.x + tileX * quads * spacing;
                const float     minZ    = origin.z + tileZ * quads * spacing;
                for ( const float x : { minX, minX + quads * spacing } )
                    for ( const float z : { minZ, minZ + quads * spacing } )
                        Inside( record, x, z );
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
