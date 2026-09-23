// THE PARTITIONER: WHICH CELL HOLDS EACH WHOLE, AND HOW FAR THAT CELL HAD TO GROW.
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
//   6. THE CELL GROWS (owner decision 2026-09-18). A composite wider than a cell is held whole in ONE
//      cell whose bounds reach its far member, on either side and either axis, with no limit; the
//      overhang is a number per cell and the world's worst; an instanced mesh is as wide as its
//      instances. The SAME records at a larger cell size do not grow - zero and non-zero over one input.
//   7. A DANGLING containment reference is reported and changes nothing else, and a cyclic one terminates.
//   8. THE REGISTER OF ENTITY REFERENCES IS COMPLETE. Every entity-to-entity reference in every scene
//      the repository ships has a classified row; a synthetic reference that has no row is FOUND, which
//      is the same function being shown red. And the whole corpus, partitioned, keeps the growth
//      relation: every point it can read independently lies inside the grown bounds of its cell.
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
using Desert::Core::Rules::CellBounds;
using Desert::Core::Rules::CellOf;
using Desert::Core::Rules::Containment;
using Desert::Core::Rules::FindUnregisteredEntityReferences;
using Desert::Core::Rules::GridSquareOf;
using Desert::Core::Rules::kEntityReferences;
using Desert::Core::Rules::kEntityReferencesByName;
using Desert::Core::Rules::kInstancePointsComponent;
using Desert::Core::Rules::kInstancePointsField;
using Desert::Core::Rules::kNoRecord;
using Desert::Core::Rules::PlannedCell;
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
    // Both stand inside cell 3's square, so nothing grew.
    EXPECT_FLOAT_EQ( plan.MaxGrowth, 0.0f );
}

// Rotation composes too. A child stated 15000 units along +X under a parent yawed +90 degrees stands at
// world -Z, and the grown bounds are where that shows: they must reach DOWN on Z and not out on X. (The
// refusal test this replaces asserted only |reach| = 15000, which an unrotated arm also satisfies - it
// could not see rotation at all.)
TEST( WorldPartitionCells, RotationOfAParentMovesWhereItsChildLands )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Turntable", { 0.0f, 0.0f, 0.0f } ) );
    records[0].Rotation = glm::vec3( 0.0f, glm::radians( 90.0f ), 0.0f );
    records.push_back( Record( 2, "Arm", { 15000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 1u );
    ASSERT_TRUE( plan.Cells[0].Content.has_value() );
    EXPECT_NEAR( plan.Cells[0].Content->MinZ, -15000.0f, 1.0f );
    EXPECT_NEAR( plan.Cells[0].Content->MaxX, 0.0f, 1.0f );
    EXPECT_NEAR( plan.Cells[0].Growth, 15000.0f, 1.0f );
}

// THE COMPOSITION, PINNED AGAINST NUMBERS WORKED BY HAND, because ComposeLocal must equal
// TransformComponent::GetTransform (translate * rotate * scale) and this suite cannot include that
// header. Parent at (1000, 0, 2000), yawed +90 degrees, scaled 2; child stated at (300, 0, 0):
// scale gives (600, 0, 0), the yaw takes +X to -Z giving (0, 0, -600), the offset gives (1000, 0, 1400).
// Scale applied AFTER rotation, or not at all, or the rotation's sign flipped, all land elsewhere.
TEST( WorldPartitionCells, ARotatedScaledOffsetParentPlacesItsChildWhereTheLoaderWould )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Parent", { 1000.0f, 0.0f, 2000.0f } ) );
    records[0].Rotation = glm::vec3( 0.0f, glm::radians( 90.0f ), 0.0f );
    records[0].Scale    = glm::vec3( 2.0f );
    records.push_back( Record( 2, "Child", { 300.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 1000000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 1u );
    ASSERT_TRUE( plan.Cells[0].Content.has_value() );
    const CellBounds& content = *plan.Cells[0].Content;
    EXPECT_NEAR( content.MinX, 1000.0f, 0.5f );
    EXPECT_NEAR( content.MaxX, 1000.0f, 0.5f );
    EXPECT_NEAR( content.MinZ, 1400.0f, 0.5f );
    EXPECT_NEAR( content.MaxZ, 2000.0f, 0.5f );
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

    // And the wheel's side of the boundary is paid for by cell 0 growing 100 units, not by cell 1.
    ASSERT_EQ( plan.Cells.size(), 1u );
    EXPECT_FLOAT_EQ( plan.Cells[0].Growth, 100.0f );
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

    // And the socket edge is recorded as SOCKET attachment, not as hierarchy.
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
    EXPECT_EQ( plan.Cells.size(), 2u );
    EXPECT_FLOAT_EQ( plan.MaxGrowth, 0.0f );
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

// AN UNPLACED INSTANCE GROWS NOTHING. Its "origin" is the absence of a position, not a position, so a cell
// it sits in must not report bounds that reach for it. Here the instance is the only thing in cell (0,0),
// so that cell has no Content at all and is exactly its square.
TEST( WorldPartitionComposites, AnUnplacedPrefabInstanceContributesNoPointToItsCell )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Ground", { 35000.0f, 0.0f, 0.0f } ) );
    EntityData instance;
    instance.id         = Common::UUID( 2 );
    instance.PrefabPath = "Prefabs/Lamp.deprefab";
    records.push_back( instance );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 2u );
    const PlannedCell& origin = plan.Cells[0];
    ASSERT_EQ( origin.Cell, CellOf( 0.0f, 0.0f, 10000.0f ) );
    EXPECT_FALSE( origin.Content.has_value() );
    EXPECT_FLOAT_EQ( origin.Growth, 0.0f );
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

// ── 6. THE CELL GROWS ──────────────────────────────────────────────────────────────────────────────
//
// Owner decision 2026-09-18: a composite wider than a cell is held whole and its cell GROWS. These
// replace the refusal tests of 6a122bae one for one: each input that used to be refused is now asserted
// to be held in ONE cell whose bounds reach the far member, with the overhang stated as a number.

// The bridge that used to be refused. One composite, one listed cell, and that cell's bounds reach the
// far end: the grid square is [0, 10000] and the far end stands at 15000, so the growth is 5000.
TEST( WorldPartitionGrowth, AHierarchyWiderThanACellIsHeldWholeAndItsCellGrowsToReachIt )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1001, "Bridge", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 1002, "BridgeFarEnd", { 15000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1001 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Composites.size(), 1u );
    ASSERT_EQ( plan.Cells.size(), 1u ) << "the far end's own square (1,0) must NOT become a cell of its own";

    const PlannedCell& cell = plan.Cells[0];
    EXPECT_EQ( cell.Cell, CellOf( 0.0f, 0.0f, 10000.0f ) );
    EXPECT_FLOAT_EQ( cell.Bounds.MinX, 0.0f );
    EXPECT_FLOAT_EQ( cell.Bounds.MaxX, 15000.0f );
    EXPECT_FLOAT_EQ( cell.Bounds.MinZ, 0.0f );
    EXPECT_FLOAT_EQ( cell.Bounds.MaxZ, 10000.0f );
    ASSERT_TRUE( cell.Content.has_value() );
    EXPECT_FLOAT_EQ( cell.Content->MaxX, 15000.0f );
    EXPECT_FLOAT_EQ( cell.Growth, 5000.0f );
    EXPECT_EQ( cell.Furthest, 1u ) << "the record that grew the cell is the far end";

    EXPECT_FLOAT_EQ( plan.MaxGrowth, 5000.0f );
    EXPECT_EQ( plan.MaxGrowthCell, 0u );
}

// The same through the other containment relation, on the other axis.
TEST( WorldPartitionGrowth, ASocketWiderThanACellGrowsItsCellOnZ )
{
    std::vector<EntityData> records;
    records.push_back( Record( 2001, "Gunner", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2002, "FarRifle", { 0.0f, 0.0f, 40000.0f } ) );
    SocketedTo( records[1], 2001 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 1u );
    EXPECT_FLOAT_EQ( plan.Cells[0].Bounds.MaxZ, 40000.0f );
    EXPECT_FLOAT_EQ( plan.Cells[0].Bounds.MaxX, 10000.0f ) << "growth on Z must not move X";
    EXPECT_FLOAT_EQ( plan.Cells[0].Growth, 30000.0f );
    EXPECT_EQ( plan.Cells[0].Furthest, 1u );
}

// GROWTH IS THE OVERHANG PAST THE SQUARE, NOT THE DISTANCE FROM THE ANCHOR. The same records at a larger
// cell size do not grow at all - which is also the instrument shown at zero and non-zero over one input.
TEST( WorldPartitionGrowth, TheSameCompositeDoesNotGrowACellBigEnoughToHoldIt )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1001, "Bridge", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 1002, "BridgeFarEnd", { 15000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1001 );

    EXPECT_FLOAT_EQ( PlanWorldPartition( records, Cells( 10000.0f ) ).MaxGrowth, 5000.0f );

    const WorldPartitionPlan wide = PlanWorldPartition( records, Cells( 20000.0f ) );
    EXPECT_FLOAT_EQ( wide.MaxGrowth, 0.0f );
    ASSERT_EQ( wide.Cells.size(), 1u );
    EXPECT_EQ( wide.Cells[0].Furthest, kNoRecord );
    EXPECT_FLOAT_EQ( wide.Cells[0].Bounds.MaxX, 20000.0f ) << "an ungrown cell is exactly its square";
}

// THERE IS NO LIMIT. A composite a hundred cells long is still one composite in one cell - the case the
// refusal existed for - and the number says what it costs.
TEST( WorldPartitionGrowth, ACompositeAHundredCellsLongIsStillOneCellAndTheNumberSaysSo )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Pipeline", { 5000.0f, 0.0f, 5000.0f } ) );
    records.push_back( Record( 2, "PipelineEnd", { 1000000.0f, 0.0f, 0.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 1u );
    // The end stands at 1005000; the anchor's square ends at 10000.
    EXPECT_FLOAT_EQ( plan.Cells[0].Growth, 995000.0f );
    EXPECT_FLOAT_EQ( plan.MaxGrowth / 10000.0f, 99.5f );
}

// THE NEGATIVE SIDE GROWS TOO. A member behind the anchor's square pulls the MIN edge out - the one side a
// `max`-only fold would never notice.
TEST( WorldPartitionGrowth, AMemberBehindTheSquarePullsTheMinimumEdgeOut )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Anchor", { 100.0f, 0.0f, 100.0f } ) );
    records.push_back( Record( 2, "Behind", { -700.0f, 0.0f, -300.0f } ) );
    Under( records[1], 1 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 1u );
    // Child world = (100 - 700, 100 - 300) = (-600, -200).
    EXPECT_FLOAT_EQ( plan.Cells[0].Bounds.MinX, -600.0f );
    EXPECT_FLOAT_EQ( plan.Cells[0].Bounds.MinZ, -200.0f );
    EXPECT_FLOAT_EQ( plan.Cells[0].Growth, 600.0f ) << "the worse of the two axes";
}

// THE WORLD'S NUMBER IS THE WORST CELL, and it names which cell.
TEST( WorldPartitionGrowth, TheWorldsGrowthIsItsWorstCellAndNamesIt )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Small", { 500.0f, 0.0f, 500.0f } ) );
    records.push_back( Record( 2, "SmallArm", { 9600.0f, 0.0f, 0.0f } ) ); // world 10100: grows by 100
    Under( records[1], 1 );
    records.push_back( Record( 3, "Big", { 50500.0f, 0.0f, 500.0f } ) );
    records.push_back( Record( 4, "BigArm", { 0.0f, 0.0f, 10200.0f } ) ); // world z 10700: grows by 700
    Under( records[3], 3 );

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 2u );
    EXPECT_FLOAT_EQ( plan.MaxGrowth, 700.0f );
    ASSERT_NE( plan.MaxGrowthCell, kNoRecord );
    EXPECT_EQ( plan.Cells[plan.MaxGrowthCell].Cell.X, 5 );
    EXPECT_EQ( plan.Cells[plan.MaxGrowthCell].Furthest, 3u );

    // And the cells are listed in X order, not in hash-map order.
    EXPECT_EQ( plan.Cells[0].Cell.X, 0 );
    EXPECT_FLOAT_EQ( plan.Cells[0].Growth, 100.0f );
}

// HEIGHT DOES NOT PARTITION. The grid is two-dimensional, so a composite stacked vertically does not grow
// its cell however tall it is - which is the difference between the pattern and the letter of UE's grid.
TEST( WorldPartitionGrowth, VerticalReachDoesNotGrowACellBecauseTheGridIsTwoDimensional )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Tower", { 0.0f, 0.0f, 0.0f } ) );
    records.push_back( Record( 2, "TowerTop", { 0.0f, 500000.0f, 0.0f } ) );
    Under( records[1], 1 );

    EXPECT_FLOAT_EQ( PlanWorldPartition( records, Cells( 10000.0f ) ).MaxGrowth, 0.0f );
}

// AN INSTANCED STATIC MESH IS AS WIDE AS ITS INSTANCES, NOT AS ITS ENTITY. The instances are WORLD-space
// (MeshECSSystem submits them without the entity's transform), so a foliage field authored on an entity
// at the origin covers wherever its instances are, and the cell must grow to them. The numbers are written
// as JSON integers on purpose: rfl reads "50000" as an integer, and a reader that only asked for doubles
// would drop exactly these instances and report zero growth.
TEST( WorldPartitionGrowth, AnInstancedMeshGrowsItsCellToItsInstancesNotToItsEntity )
{
    std::vector<EntityData> records;
    records.push_back( Record( 1, "Foliage", { 0.0f, 0.0f, 0.0f } ) );

    const auto block =
         rfl::json::read<rfl::Generic>( R"({"Primitive":"Cube","InstanceTransforms":[)"
                                        R"([1,0,0,0, 0,1,0,0, 0,0,1,0, 50000,0,-30000,1],)"
                                        R"([1.0,0,0,0, 0,1.0,0,0, 0,0,1.0,0, 200.5,0.0,300.5,1.0]]})" );
    ASSERT_TRUE( block.has_value() );
    records[0].Components[std::string( kInstancePointsComponent )] = block.value();

    const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( 10000.0f ) );
    ASSERT_EQ( plan.Cells.size(), 1u );
    ASSERT_TRUE( plan.Cells[0].Content.has_value() );
    EXPECT_FLOAT_EQ( plan.Cells[0].Content->MaxX, 50000.0f );
    EXPECT_FLOAT_EQ( plan.Cells[0].Content->MinZ, -30000.0f );
    EXPECT_FLOAT_EQ( plan.Cells[0].Growth, 40000.0f );
    EXPECT_EQ( plan.Cells[0].Furthest, 0u );
}

// ── 7. DANGLING AND CYCLIC CONTAINMENT ─────────────────────────────────────────────────────────────

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

// THE CORPUS, PARTITIONED, AND CHECKED AS A RELATION RATHER THAN AGAINST ITSELF.
//
// For every scene the repository ships, at a cell of one metre (where composites do cross squares) and at
// the format's default: every composite is in exactly the cell it names; every cell's bounds contain its
// square and its content; its Growth is exactly how far the bounds overhang the square; and - the part
// computed WITHOUT the planner - every root record's own translation, and every instance of every
// InstancedStaticMesh read straight from the file's JSON, lies inside the bounds of its cell.
//
// And the instrument is shown non-zero: at one metre some scene must grow a cell, or this sweep proves
// nothing about growth at all.
TEST( WorldPartitionReferences, EveryCorpusPointLiesInsideTheGrownBoundsOfItsCell )
{
    std::size_t grownAtOneMetre = 0;
    std::size_t instancesSeen   = 0;
    for ( const auto& path : RepositoryScenes() )
    {
        const std::string text   = ReadAll( path );
        const auto        parsed = rfl::json::read<SceneSerialized>( text );
        ASSERT_TRUE( parsed.has_value() ) << path.string();
        const auto& records = parsed->Entities;

        for ( const float cellSize : { 100.0f, 12800.0f } )
        {
            const WorldPartitionPlan plan = PlanWorldPartition( records, Cells( cellSize ) );
            if ( cellSize == 100.0f && plan.MaxGrowth > 0.0f )
                ++grownAtOneMetre;

            std::vector<std::size_t> cellOfComposite( plan.Composites.size(), kNoRecord );
            float                    worst = 0.0f;
            for ( std::size_t index = 0; index < plan.Cells.size(); ++index )
            {
                const PlannedCell& cell   = plan.Cells[index];
                const CellBounds   square = GridSquareOf( cell.Cell, cellSize );
                EXPECT_LE( cell.Bounds.MinX, square.MinX ) << path.string();
                EXPECT_LE( cell.Bounds.MinZ, square.MinZ ) << path.string();
                EXPECT_GE( cell.Bounds.MaxX, square.MaxX ) << path.string();
                EXPECT_GE( cell.Bounds.MaxZ, square.MaxZ ) << path.string();
                if ( cell.Content.has_value() )
                {
                    EXPECT_LE( cell.Bounds.MinX, cell.Content->MinX ) << path.string();
                    EXPECT_GE( cell.Bounds.MaxX, cell.Content->MaxX ) << path.string();
                }
                const float overhang =
                     std::max( { square.MinX - cell.Bounds.MinX, cell.Bounds.MaxX - square.MaxX,
                                 square.MinZ - cell.Bounds.MinZ, cell.Bounds.MaxZ - square.MaxZ } );
                EXPECT_FLOAT_EQ( cell.Growth, overhang ) << path.string();
                worst = std::max( worst, cell.Growth );

                for ( const std::size_t held : cell.Composites )
                {
                    EXPECT_EQ( cellOfComposite[held], kNoRecord ) << "a composite in two cells: " << path.string();
                    cellOfComposite[held] = index;
                    EXPECT_EQ( plan.Composites[held].Cell, cell.Cell ) << path.string();
                }
            }
            EXPECT_FLOAT_EQ( plan.MaxGrowth, worst ) << path.string();
            for ( const std::size_t held : cellOfComposite )
                EXPECT_NE( held, kNoRecord ) << "a composite in no cell: " << path.string();

            const auto Inside = [&]( std::size_t record, float x, float z )
            {
                const PlannedCell& cell = plan.Cells[cellOfComposite[CompositeOf( plan, record )]];
                EXPECT_TRUE( x >= cell.Bounds.MinX && x <= cell.Bounds.MaxX && z >= cell.Bounds.MinZ &&
                             z <= cell.Bounds.MaxZ )
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
    EXPECT_GT( grownAtOneMetre, 0u )
         << "no scene in the corpus grows a one-metre cell, so this sweep proves nothing about growth";
    EXPECT_GT( instancesSeen, 0u ) << "no InstancedStaticMesh instance was checked, so the ISM half is vacuous";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
