// THE PARTITIONER, AND THE TWO THINGS IT IS ALLOWED TO DO TO A WORLD.
//
// WHAT IS ASSERTED, in the order the sections appear:
//
//   1. THE FORMAT IS SILENT WHEN THE WORLD IS NOT PARTITIONED. A `.desce` that says nothing about
//      partitioning must come out of the writer with exactly the keys it went in with - `kSceneVersion`
//      did not move and 144 files on disk must not change a byte. Asserted over the whole corpus by
//      comparing the TOP-LEVEL KEY SET of the file against the key set of the tree written back from
//      it, which is the level the new field lives at and therefore the level it could pollute.
//   2. A WORLD THAT IS PARTITIONED SAYS SO, and the number round-trips. The same instrument as 1,
//      pointed at the opposite case, so that 1's green is a fact about the corpus and not about the
//      instrument being unable to see the key at all.
//   3. CELLS ARE DERIVED FROM COORDINATES - including on the negative side of the origin, where a cast
//      instead of a floor silently makes one cell twice as wide as the others.
//   4. TRANSFORMS COMPOSE DOWN THE HIERARCHY. A record's `Translation` is LOCAL, so partitioning on it
//      directly puts every child of a rotated or moved parent in the wrong cell.
//   5. THE COMPOSITE IS THE UNIT. A parent and child that fall either side of a boundary land in ONE
//      cell, and a socket attachment fuses two hierarchies into one composite.
//   6. THE REFUSAL. A composite that reaches further than one cell from its anchor is refused, the
//      refusal names both entities and the relation, and the SAME records at a larger cell size are
//      not refused - which is the instrument being shown red and green over one input.
//   7. A DANGLING containment reference is reported and is NOT a refusal, and a cyclic one terminates.
//   8. THE REGISTER OF ENTITY REFERENCES IS COMPLETE. Every entity-to-entity reference in every scene
//      the repository ships has a classified row; a synthetic reference that has no row is FOUND, which
//      is the same function being shown red.
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
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::SceneSerialized;
using Desert::Core::WorldPartitionSerialized;
using Desert::Core::Rules::CellOf;
using Desert::Core::Rules::Containment;
using Desert::Core::Rules::DescribeRefusal;
using Desert::Core::Rules::FindUnregisteredEntityReferences;
using Desert::Core::Rules::kEntityReferences;
using Desert::Core::Rules::kEntityReferencesByName;
using Desert::Core::Rules::kNoRecord;
using Desert::Core::Rules::PlanWorldPartition;
using Desert::Core::Rules::ReferenceKind;
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

    WorldPartitionSerialized Cells( float size )
    {
        WorldPartitionSerialized settings;
        settings.CellSize = size;
        return settings;
    }

    std::set<std::string> TopLevelKeys( const std::string& json )
    {
        std::set<std::string> keys;
        const auto            document = rfl::json::read<rfl::Generic>( json );
        if ( !document.has_value() )
            return keys;
        const auto object = document.value().to_object();
        if ( !object.has_value() )
            return keys;
        for ( const auto& [key, value] : object.value() )
            keys.insert( key );
        return keys;
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
} // namespace

// ── 1. THE FORMAT IS SILENT WHEN THE WORLD IS NOT PARTITIONED ───────────────────────────────────────

// 1a. The corpus is where this suite thinks it is. Without this every corpus assertion below is a
// vacuous pass over zero files - the failure mode that lets a corpus test report green while checking
// nothing.
TEST( WorldPartitionFormat, TheScenesAreWhereThisSuiteThinksTheyAre )
{
    EXPECT_GE( RepositoryScenes().size(), 100u );
}

// 1b. A scene nobody partitioned writes no `WorldPartition` key. This is the whole of "an unpartitioned
// .desce keeps loading and saving byte for byte": the field is a std::optional, reflect-cpp omits a
// nullopt field entirely, and so the writer's output is the same text it was before this field existed.
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

// 1c. THE CORPUS. Every `.desce` the repository ships: it states no partition, and the set of top-level
// keys the writer produces from it is EXACTLY the set the file already had. A new top-level field that
// leaked into the output - the one way this change could touch 144 files - fails here by name.
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

// ── 2. A WORLD THAT IS PARTITIONED SAYS SO ─────────────────────────────────────────────────────────

// The same instrument as 1b and 1c, pointed at the opposite case. Without this, 1's green would also be
// produced by a field the writer can never emit at all.
TEST( WorldPartitionFormat, APartitionedSceneStatesTheBlockAndItRoundTrips )
{
    SceneSerialized scene;
    scene.SceneName      = "Partitioned";
    scene.SceneVersion   = Desert::Core::kSceneVersion;
    scene.UnitVersion    = Desert::Core::kUnitVersion;
    scene.WorldPartition = Cells( 25600.0f );

    const std::string written = rfl::json::write( scene );
    EXPECT_TRUE( TopLevelKeys( written ).count( "WorldPartition" ) ) << written;

    const auto read = rfl::json::read<SceneSerialized>( written );
    ASSERT_TRUE( read.has_value() );
    ASSERT_TRUE( read->WorldPartition.has_value() );
    EXPECT_FLOAT_EQ( read->WorldPartition->CellSize, 25600.0f );
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

// SAVING A PARTITIONED WORLD DOES NOT LOSE THE BLOCK - and the mechanism that keeps it is named here
// rather than assumed, because it is not the obvious one. The live Scene has no partition member yet, so
// SceneSerializer::SerializeToJson builds a fresh tree WITHOUT the block; what puts it back is the
// document merge, which keeps every top-level key the writer does not state. This is the assertion that
// turns that from an accident into a property, and it is what would go red the day the merge stopped
// covering the top level.
TEST( WorldPartitionFormat, ThePartitionBlockSurvivesASaveThroughTheDocumentMerge )
{
    SceneSerialized onDisk;
    onDisk.SceneName      = "Partitioned";
    onDisk.SceneVersion   = Desert::Core::kSceneVersion;
    onDisk.UnitVersion    = Desert::Core::kUnitVersion;
    onDisk.WorldPartition = Cells( 51200.0f );

    // What the writer produces today from a live Scene loaded out of that file: the same scene, minus
    // the block, because nothing in memory carries it.
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
    EXPECT_FLOAT_EQ( reread->WorldPartition->CellSize, 51200.0f );
}

// ── 3. CELLS ARE DERIVED FROM COORDINATES ──────────────────────────────────────────────────────────

TEST( WorldPartitionCells, TheGridIsUniformOnBothSidesOfTheOrigin )
{
    EXPECT_EQ( CellOf( 0.0f, 0.0f, 100.0f ).X, 0 );
    EXPECT_EQ( CellOf( 99.0f, 99.0f, 100.0f ).X, 0 );
    EXPECT_EQ( CellOf( 100.0f, 0.0f, 100.0f ).X, 1 );

    // A cast to int truncates towards zero, which would answer 0 here and make the cell spanning the
    // origin twice as wide as every other cell in the world.
    EXPECT_EQ( CellOf( -1.0f, 0.0f, 100.0f ).X, -1 );
    EXPECT_EQ( CellOf( -100.0f, 0.0f, 100.0f ).X, -1 );
    EXPECT_EQ( CellOf( -101.0f, 0.0f, 100.0f ).X, -2 );
    EXPECT_EQ( CellOf( 0.0f, -1.0f, 100.0f ).Z, -1 );
}

// ── 4. TRANSFORMS COMPOSE DOWN THE HIERARCHY ───────────────────────────────────────────────────────

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
    // Both are one composite, anchored on the parent, so the cell is the parent's: 30100 / 10000 = 3.
    EXPECT_EQ( plan.Composites[0].Anchor, 0u );
    EXPECT_EQ( plan.Composites[0].Cell.X, 3 );
    EXPECT_FALSE( plan.Refused() );
}

// Rotation and scale compose too. A child stated 100 units along +X under a parent yawed 90 degrees is
// not 100 units along +X of the world, and a partitioner that ignored rotation would say it was.
TEST( WorldPartitionCells, RotationOfAParentMovesWhereItsChildLands )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Turntable", { 0.0f, 0.0f, 0.0f } ) );
    records[0].Rotation = glm::vec3( 0.0f, glm::radians( 90.0f ), 0.0f );
    records.push_back( Record( 2, "Arm", { 15000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    // Cell size large enough that nothing is refused; what is asserted is WHERE the arm went.
    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 1000000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    ASSERT_TRUE( plan.Refusals.empty() );

    // A yaw of +90 degrees takes local +X onto world -Z. Refuse at a cell size that only a correctly
    // rotated arm crosses on Z, and the refusal's reach is the evidence.
    const WorldPartitionPlan tight = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( tight.Refusals.size(), 1u );
    EXPECT_NEAR( tight.Refusals[0].Reach, 15000.0f, 1.0f );
}

// ── 5. THE COMPOSITE IS THE UNIT ───────────────────────────────────────────────────────────────────

// The whole decision, in one case: two entities that a naive per-entity grid would put in DIFFERENT
// cells are one composite and therefore one cell. This is the assertion that would fail if the
// partitioner ever went back to assigning entities.
TEST( WorldPartitionComposites, AParentAndChildEitherSideOfABoundaryShareOneCell )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Vehicle", { 9900.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "Wheel", { 200.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    // Per entity: the vehicle is in cell 0 (9900/10000) and the wheel is in cell 1 (10100/10000).
    EXPECT_EQ( CellOf( 9900.0f, 0.0f, 10000.0f ).X, 0 );
    EXPECT_EQ( CellOf( 10100.0f, 0.0f, 10000.0f ).X, 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    EXPECT_EQ( plan.Composites[0].Members.size(), 2u );
    EXPECT_EQ( plan.Composites[0].Cell.X, 0 );
    EXPECT_FALSE( plan.Refused() );
}

// A socket attachment is containment, so it FUSES two hierarchies that are otherwise unrelated: the
// character's tree and the weapon's tree become one thing that a cell holds whole.
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

    // And the socket edge is recorded as SOCKET attachment, not as hierarchy - the refusal's sentence
    // depends on telling the two apart.
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

// Two entities with no relation between them are two composites, and a grid still applies to them.
// Without this, "everything is one composite" would pass every assertion above.
TEST( WorldPartitionComposites, UnrelatedEntitiesAreSeparateCompositesInSeparateCells )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "RockA", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "RockB", { 50000.0f, 0.0f, 0.0f } ) );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 2u );
    EXPECT_NE( plan.Composites[CompositeOf( plan, 0 )].Cell, plan.Composites[CompositeOf( plan, 1 )].Cell );
    EXPECT_FALSE( plan.Refused() );
}

// A PREFAB INSTANCE HAS NO TRANSFORM IN A .desce AT ALL - SceneSerializer strips it into overrides
// addressed by ids only the .deprefab resolves - so this partitioner cannot place one. It says so
// instead of quietly answering "cell (0,0)", which is what the number would otherwise look like.
TEST( WorldPartitionComposites, APrefabInstanceWithNoTransformIsListedAsUnplaced )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Ground", { 0.0f, 0.0f, 0.0f } ) );
    EntityData instance;
    instance.id         = Common::UUID( 2 );
    instance.PrefabPath = "Prefabs/Lamp.deprefab";
    records.push_back( instance );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.UnplacedPrefabInstances.size(), 1u );
    EXPECT_EQ( plan.UnplacedPrefabInstances[0], 1u );

    // A prefab record that DOES state a transform is placed like anything else, so the report is about
    // the missing transform and not about the record being a prefab.
    records[1].Translation = glm::vec3( 50000.0f, 0.0f, 0.0f );
    EXPECT_TRUE( PlanWorldPartition( records, Cells( 10000.0f ) ).UnplacedPrefabInstances.empty() );
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

// ── 6. THE REFUSAL ─────────────────────────────────────────────────────────────────────────────────

TEST( WorldPartitionRefusal, AHierarchyThatReachesFurtherThanOneCellIsRefusedByName )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1001, "Bridge", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 1002, "BridgeFarEnd", { 15000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1001 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_TRUE( plan.Refused() );
    ASSERT_EQ( plan.Refusals.size(), 1u );
    EXPECT_EQ( plan.Refusals[0].Anchor, 0u );
    EXPECT_EQ( plan.Refusals[0].Member, 1u );
    EXPECT_EQ( plan.Refusals[0].MemberHolder, 0u );
    EXPECT_EQ( plan.Refusals[0].Relation, Containment::Hierarchy );
    EXPECT_NEAR( plan.Refusals[0].Reach, 15000.0f, 0.5f );

    // BOTH ENTITIES AND THE RELATION, in the sentence a human reads. A refusal nobody can read is a
    // refusal nobody will fix.
    const std::string said = DescribeRefusal( records, plan.Refusals[0] );
    EXPECT_NE( said.find( "Bridge'" ), std::string::npos ) << said;
    EXPECT_NE( said.find( "BridgeFarEnd" ), std::string::npos ) << said;
    EXPECT_NE( said.find( "1001" ), std::string::npos ) << said;
    EXPECT_NE( said.find( "1002" ), std::string::npos ) << said;
    EXPECT_NE( said.find( "hierarchy" ), std::string::npos ) << said;
}

// The same shape through the other containment relation, and the sentence says which one it was.
TEST( WorldPartitionRefusal, ASocketReachingFurtherThanOneCellIsRefusedAndNamesTheSocket )
{
    std::vector<EntityData> records;
    records.push_back( Record( 2001, "Gunner", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2002, "OrphanedRifle", { 0.0f, 0.0f, 40000.0f } ) );
    SocketedTo( records[1], 2001 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Refusals.size(), 1u );
    EXPECT_EQ( plan.Refusals[0].Relation, Containment::SocketAttachment );

    const std::string said = DescribeRefusal( records, plan.Refusals[0] );
    EXPECT_NE( said.find( "socket attachment" ), std::string::npos ) << said;
    EXPECT_NE( said.find( "OrphanedRifle" ), std::string::npos ) << said;
}

// THE SAME RECORDS, NOT REFUSED. An instrument that only ever says no proves nothing about the input:
// the one thing that changes here is the world's own cell size, which is exactly one of the three
// answers the refusal tells the author about.
TEST( WorldPartitionRefusal, TheSameCompositeIsAcceptedWhenTheCellIsBigEnoughToHoldIt )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1001, "Bridge", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 1002, "BridgeFarEnd", { 15000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1001 );

    EXPECT_TRUE( PlanWorldPartition( records, Cells( 10000.0f ) ).Refused() );
    EXPECT_FALSE( PlanWorldPartition( records, Cells( 20000.0f ) ).Refused() );
}

// THE BOUND IS EXACTLY ONE CELL, and the edge of it is accepted. This is the number the promise in
// WorldPartitionRules.hpp rests on - "a cell's contents lie inside its bounds expanded by at most one
// cell size" - so it is pinned rather than left to whichever comparison somebody typed.
TEST( WorldPartitionRefusal, ReachOfExactlyOneCellIsAcceptedAndAnythingBeyondIsNot )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Anchor", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "Reach", { 10000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );
    EXPECT_FALSE( PlanWorldPartition( records, Cells( 10000.0f ) ).Refused() );

    records[1].Translation = glm::vec3( 10001.0f, 0.0f, 0.0f );
    EXPECT_TRUE( PlanWorldPartition( records, Cells( 10000.0f ) ).Refused() );
}

// HEIGHT DOES NOT PARTITION. The grid is two-dimensional, so a composite stacked vertically is not
// refused however tall it is - which is the difference between the pattern and the letter of UE's grid.
TEST( WorldPartitionRefusal, VerticalReachIsNotARefusalBecauseTheGridIsTwoDimensional )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Tower", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "TowerTop", { 0.0f, 500000.0f, 0.0f } ) );
    Under( records[1], 1 );

    EXPECT_FALSE( PlanWorldPartition( records, Cells( 10000.0f ) ).Refused() );
}

// ── 7. DANGLING AND CYCLIC CONTAINMENT ─────────────────────────────────────────────────────────────

// A containment reference naming an entity this file does not contain is REPORTED, not refused: it is
// a defect that already has an owner (the loader counts unresolved parents; AttachmentSystem skips a
// target it cannot find), and refusing to partition a world over one would be this task punishing it.
TEST( WorldPartitionComposites, ADanglingContainmentReferenceIsReportedAndIsNotARefusal )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Lonely", { 0.0f, 0.0f, 0.0f } ) );
    records[0].parent = Common::UUID( 9999 );
    records.push_back( Record( 2, "AlsoLonely", { 0.0f, 0.0f, 0.0f } ) );
    SocketedTo( records[1], 8888 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Dangling.size(), 2u );
    EXPECT_FALSE( plan.Refused() );
    EXPECT_TRUE( plan.Containment.empty() );
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

// ── 8. THE REGISTER OF ENTITY REFERENCES IS COMPLETE ───────────────────────────────────────────────

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

// THE CORPUS, PARTITIONED, BOTH WAYS. At a cell big enough to hold any world we ship, nothing is
// refused; at a cell of one metre, the scenes that actually contain composites ARE refused. Same files,
// same function, opposite verdicts - so the green above is a fact about the corpus and not a function
// that cannot say no.
TEST( WorldPartitionReferences, TheCorpusPartitionsCleanlyAtWorldScaleAndIsRefusedAtOneMetre )
{
    std::size_t refusedAtOneMetre = 0;
    for ( const auto& path : RepositoryScenes() )
    {
        const auto parsed = rfl::json::read<SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();

        const WorldPartitionPlan wide = PlanWorldPartition( parsed->Entities, Cells( 1.0e7f ) );
        EXPECT_FALSE( wide.Refused() ) << path.string();

        if ( PlanWorldPartition( parsed->Entities, Cells( 100.0f ) ).Refused() )
            ++refusedAtOneMetre;
    }
    EXPECT_GT( refusedAtOneMetre, 0u )
         << "no scene in the corpus holds a composite wider than a metre, so this sweep proves nothing";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
