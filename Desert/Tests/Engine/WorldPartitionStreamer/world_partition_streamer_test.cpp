// A PARTITIONED WORLD IN PLAY: WHICH RECORDS ARE ENTITIES, FRAME BY FRAME.
//
// The executor (Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp) is driven here against a world that
// is a set of live record indices — the engine's world is a Scene, which no test project can compile — over
// plans made by the REAL planner from records spelled the way a .desce spells them.
//
// WHAT IS ASSERTED:
//
//   1. THE START OF PLAY. Every record is an entity in Edit; Begin leaves exactly the always-loaded ones and the
//      source's neighbourhood, destroys the rest in one call, and activates nothing.
//   2. THE FLY-THROUGH. A source crossing the world: every frame the world's live set IS the executor's, no
//      record further than the unload band is alive, the count stays a neighbourhood and not the world, a
//      parent and its child are never apart, the camera is never destroyed, no record is activated twice
//      without a destroy between — and at rest the live set is exactly what the query wants.
//   3. A REFERENCE ACROSS A CELL BOUNDARY. A record activated while its observed target is not resident counts
//      one DEFERRED reference; a target destroyed while its observer stays counts one UNBOUND. Neither is an
//      error, and neither is silent.
//   4. A SCENE WITHOUT A PARTITION BLOCK. No executor, and the world is not touched at all.
//   5. REFUSALS. A record without an id; a world whose activation fails (named by unit).

#include <Common/Json/Document.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::SceneSerialized;
using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
using Desert::Core::Rules::BeginWorldStreaming;
using Desert::Core::Rules::PlanWorldPartition;
using Desert::Core::Rules::QueryStreamingCells;
using Desert::Core::Rules::ResidencyExecutor;
using Desert::Core::Rules::ResidencySettings;
using Desert::Core::Rules::ResidencyTick;
using Desert::Core::Rules::ResidencyWorld;
using Desert::Core::Rules::StreamingSource;
using Desert::Core::Rules::WorldPartitionPlan;

namespace
{
    constexpr float  kCell    = 1000.0f; // 10 m cells
    constexpr float  kRange   = 1500.0f;
    constexpr int    kColumns = 16;
    constexpr int    kRows    = 3;
    constexpr double kFrame   = 1.0 / 64.0;

    // Ids: 1000 + column * 10 + row for the grid records; the rest are named below.
    constexpr std::uint64_t kCameraId  = 1;
    constexpr std::uint64_t kParentId  = 2;
    constexpr std::uint64_t kChildId   = 3;
    constexpr std::uint64_t kShooterId = 4;
    constexpr std::uint64_t kTargetId  = 5;

    WorldPartitionSerialized Grid()
    {
        WorldPartitionSerialized partition;
        partition.Grids.push_back( WorldPartitionGridSerialized{ kCell, kRange } );
        return partition;
    }

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
        const auto block = Common::Json::Parse( json );
        ASSERT_TRUE( block.IsSuccess() ) << json;
        data.Components[key] = block.GetValue();
    }

    glm::vec3 CellCentre( int column, int row )
    {
        return { ( static_cast<float>( column ) + 0.5f ) * kCell, 0.0f,
                 ( static_cast<float>( row ) + 0.5f ) * kCell };
    }

    // A strip of cells along +X, one record per cell, a camera (always-loaded), a parent with its child in
    // column 7, and a shooter in column 0 observing a target in column 10.
    std::vector<EntityData> World()
    {
        std::vector<EntityData> records;
        EntityData              camera = Record( kCameraId, "Camera", { 50.0f, 0.0f, 50.0f } );
        With( camera, "Camera", R"({"IsMainCamera": true})" );
        records.push_back( camera );
        records.push_back( Record( kParentId, "Parent", CellCentre( 7, 1 ) ) );
        // The child's translation is LOCAL to its parent, as in a .desce.
        EntityData child = Record( kChildId, "Child", { 100.0f, 0.0f, 0.0f } );
        child.parent     = Common::UUID( kParentId );
        records.push_back( child );
        EntityData shooter = Record( kShooterId, "Shooter", CellCentre( 0, 1 ) );
        With( shooter, "Projectile", R"({"Owner": ")" + std::to_string( kTargetId ) + R"("})" );
        records.push_back( shooter );
        records.push_back( Record( kTargetId, "Target", CellCentre( 10, 1 ) ) );
        for ( int column = 0; column < kColumns; ++column )
            for ( int row = 0; row < kRows; ++row )
                records.push_back( Record( 1000 + static_cast<std::uint64_t>( column * 10 + row ), "Block",
                                           CellCentre( column, row ) ) );
        return records;
    }

    std::size_t IndexOf( const std::vector<EntityData>& records, std::uint64_t id )
    {
        for ( std::size_t record = 0; record < records.size(); ++record )
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            if ( records[record].id.has_value() && *records[record].id == Common::UUID( id ) )
                // NOLINTEND(bugprone-unchecked-optional-access)
                return record;
        ADD_FAILURE() << "no record " << id;
        return 0;
    }

    // The engine's world is a Scene; this one is the set of records that are entities.
    struct SetWorld final : ResidencyWorld
    {
        std::set<std::size_t> Live;
        std::size_t           ActivateCalls = 0;
        std::size_t           DestroyCalls  = 0;
        std::size_t           Reactivations = 0; // a record activated while already an entity: a duplicate
        std::string           FailWith;

        explicit SetWorld( std::size_t records )
        {
            for ( std::size_t record = 0; record < records; ++record )
                Live.insert( record );
        }

        Common::BoolResultStr Activate( std::size_t /*unit*/, std::span<const std::size_t> records ) override
        {
            ++ActivateCalls;
            if ( !FailWith.empty() )
                return Common::MakeError<bool>( FailWith );
            for ( const std::size_t record : records )
                Reactivations += Live.insert( record ).second ? 0 : 1;
            return Common::MakeSuccess( true );
        }

        void Destroy( std::span<const std::size_t> records ) override
        {
            ++DestroyCalls;
            for ( const std::size_t record : records )
                Live.erase( record );
        }
    };

    ResidencySettings Settings()
    {
        ResidencySettings settings;
        settings.ActivationBudgetMs = 1.0;
        settings.MsPerRecord        = 0.1; // ten records a frame, so a cell row of three fits
        return settings;
    }

    ResidencyExecutor Begin( const std::vector<EntityData>& records, StreamingSource source, SetWorld& world )
    {
        const WorldPartitionPlan plan = PlanWorldPartition( records, Grid() );
        auto begun = ResidencyExecutor::Begin( plan, Grid(), records, Settings(), std::span( &source, 1 ), world );
        EXPECT_TRUE( begun.IsSuccess() ) << begun.GetError();
        return begun.ExtractValue();
    }

    std::set<std::size_t> ExecutorLive( const ResidencyExecutor& executor, std::size_t records )
    {
        std::set<std::size_t> live;
        for ( std::size_t record = 0; record < records; ++record )
            if ( executor.IsLive( record ) )
                live.insert( record );
        return live;
    }

    // What the query wants from @p source, as records: always-loaded plus every wanted cell's members.
    std::set<std::size_t> Wanted( const std::vector<EntityData>& records, StreamingSource source )
    {
        const WorldPartitionPlan plan  = PlanWorldPartition( records, Grid() );
        auto                     query = QueryStreamingCells( plan, Grid(), std::span( &source, 1 ) );
        EXPECT_TRUE( query.IsSuccess() );
        std::set<std::size_t> wanted;
        for ( const std::size_t composite : plan.AlwaysLoaded )
            for ( const std::size_t record : plan.Composites[composite].Members )
                wanted.insert( record );
        for ( const auto& cell : query.GetValue().Cells )
            for ( const std::size_t composite : plan.Cells[cell.Cell].Composites )
                for ( const std::size_t record : plan.Composites[composite].Members )
                    wanted.insert( record );
        return wanted;
    }

    ResidencyTick Tick( ResidencyExecutor& executor, StreamingSource source, double now, SetWorld& world )
    {
        auto tick = executor.Tick( std::span( &source, 1 ), now, world );
        EXPECT_TRUE( tick.IsSuccess() ) << tick.GetError();
        return tick.IsSuccess() ? tick.ExtractValue() : ResidencyTick{};
    }
} // namespace

TEST( WorldPartitionStreamer, BeginKeepsTheNeighbourhoodAndDestroysTheRestAtOnce )
{
    const std::vector<EntityData> records = World();
    SetWorld                      world( records.size() );
    const StreamingSource         source{ CellCentre( 2, 1 ) };

    const ResidencyExecutor executor = Begin( records, source, world );

    EXPECT_EQ( world.Live, Wanted( records, source ) );
    EXPECT_EQ( world.Live, ExecutorLive( executor, records.size() ) );
    EXPECT_EQ( world.DestroyCalls, 1u );
    EXPECT_EQ( world.ActivateCalls, 0u );
    EXPECT_LT( world.Live.size(), records.size() / 2 ) << "the start of Play left most of the world alive";
    EXPECT_TRUE( world.Live.count( IndexOf( records, kCameraId ) ) );
}

TEST( WorldPartitionStreamer, AFlyThroughKeepsOnlyTheNeighbourhoodAlive )
{
    const std::vector<EntityData> records = World();
    SetWorld                      world( records.size() );
    StreamingSource               source{ CellCentre( 0, 1 ) };
    ResidencyExecutor             executor = Begin( records, source, world );

    const std::size_t camera = IndexOf( records, kCameraId );
    const std::size_t parent = IndexOf( records, kParentId );
    const std::size_t child  = IndexOf( records, kChildId );
    const float       band   = kRange * ( 1.0f + Settings().UnloadMargin );

    std::size_t most  = 0;
    double      now   = 0.0;
    int         frame = 0;
    for ( float x = CellCentre( 0, 1 ).x; x <= CellCentre( kColumns - 1, 1 ).x; x += 50.0f, ++frame )
    {
        source.Position.x = x;
        Tick( executor, source, now += kFrame, world );

        ASSERT_EQ( world.Live, ExecutorLive( executor, records.size() ) ) << "frame " << frame;
        ASSERT_TRUE( world.Live.count( camera ) ) << "the always-loaded camera was destroyed at frame " << frame;
        ASSERT_EQ( world.Live.count( parent ), world.Live.count( child ) )
             << "a composite came apart at " << frame;
        for ( const std::size_t record : world.Live )
        {
            if ( record == camera || record == child ) // the child is where its parent is: asserted above
                continue;
            // The distance from the source to the record's level-0 cell square, which is what the band keeps.
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            const float cellMin = std::floor( records[record].Translation->x / kCell ) * kCell;
            // NOLINTEND(bugprone-unchecked-optional-access)
            const float dx = std::max( { cellMin - x, 0.0f, x - ( cellMin + kCell ) } );
            ASSERT_LE( dx, band ) << "record " << record << " alive " << dx << " cm away at frame " << frame;
        }
        most = std::max( most, world.Live.size() );
    }
    for ( int rest = 0; rest < 8; ++rest )
        Tick( executor, source, now += kFrame, world );

    EXPECT_EQ( world.Reactivations, 0u );
    // At rest: everything the query wants is alive, and nothing outside the unload band is — what lies between
    // is the hysteresis, kept on purpose.
    const std::set<std::size_t> wanted = Wanted( records, source );
    const std::set<std::size_t> kept =
         Wanted( records, StreamingSource{ source.Position, 1.0f + Settings().UnloadMargin } );
    EXPECT_TRUE( std::includes( world.Live.begin(), world.Live.end(), wanted.begin(), wanted.end() ) )
         << "at rest a wanted record is not alive";
    EXPECT_TRUE( std::includes( kept.begin(), kept.end(), world.Live.begin(), world.Live.end() ) )
         << "at rest a record outside the unload band is alive";
    EXPECT_LT( most, records.size() / 2 ) << "the most alive at once was " << most << " of " << records.size();
    EXPECT_GT( world.ActivateCalls, static_cast<std::size_t>( kColumns - 4 ) ) << "the flight streamed nothing in";
}

TEST( WorldPartitionStreamer, AReferenceAcrossACellBoundaryIsDeferredThenUnboundAndCounted )
{
    const std::vector<EntityData> records = World();
    SetWorld                      world( records.size() );
    const std::size_t             shooter = IndexOf( records, kShooterId );
    const std::size_t             target  = IndexOf( records, kTargetId );

    // Start at the target: the shooter is not resident.
    ResidencyExecutor executor = Begin( records, StreamingSource{ CellCentre( 10, 1 ) }, world );
    ASSERT_TRUE( world.Live.count( target ) );
    ASSERT_FALSE( world.Live.count( shooter ) );

    // To the shooter: the target leaves, the shooter arrives observing a target that is not there.
    double      now      = 0.0;
    std::size_t deferred = 0;
    std::size_t unbound  = 0;
    for ( int frame = 0; frame < 6; ++frame )
    {
        const ResidencyTick tick = Tick( executor, StreamingSource{ CellCentre( 0, 1 ) }, now += kFrame, world );
        deferred += tick.DeferredReferences;
        unbound += tick.UnboundReferences;
    }
    ASSERT_TRUE( world.Live.count( shooter ) );
    ASSERT_FALSE( world.Live.count( target ) );
    EXPECT_EQ( deferred, 1u );
    EXPECT_EQ( unbound, 0u ) << "the target left while nothing observing it was alive";

    // Wide enough for both: the target arrives, and nothing new is deferred.
    deferred = 0;
    for ( int frame = 0; frame < 12; ++frame )
        deferred += Tick( executor, StreamingSource{ CellCentre( 5, 1 ), 5.0f }, now += kFrame, world )
                         .DeferredReferences;
    ASSERT_TRUE( world.Live.count( target ) );
    ASSERT_TRUE( world.Live.count( shooter ) );
    EXPECT_EQ( deferred, 0u );

    // Back to the shooter alone: the target leaves while the shooter observes it.
    unbound = 0;
    for ( int frame = 0; frame < 6; ++frame )
        unbound += Tick( executor, StreamingSource{ CellCentre( 0, 1 ) }, now += kFrame, world ).UnboundReferences;
    ASSERT_FALSE( world.Live.count( target ) );
    EXPECT_EQ( unbound, 1u );
}

TEST( WorldPartitionStreamer, ASceneWithoutAPartitionBlockIsNotTouched )
{
    SceneSerialized scene;
    scene.SceneName = "Flat";
    scene.Entities  = World();
    SetWorld              world( scene.Entities.size() );
    const StreamingSource source{ CellCentre( 0, 1 ) };

    auto begun = BeginWorldStreaming( scene, {}, Settings(), std::span( &source, 1 ), world );

    ASSERT_TRUE( begun.IsSuccess() ) << begun.GetError();
    EXPECT_FALSE( begun.GetValue().has_value() );
    EXPECT_EQ( world.Live.size(), scene.Entities.size() );
    EXPECT_EQ( world.ActivateCalls + world.DestroyCalls, 0u );

    // The same scene with the block streams — so it is the block, and nothing else, that decides.
    scene.WorldPartition = Grid();
    auto streamed        = BeginWorldStreaming( scene, {}, Settings(), std::span( &source, 1 ), world );
    ASSERT_TRUE( streamed.IsSuccess() ) << streamed.GetError();
    EXPECT_TRUE( streamed.GetValue().has_value() );
    EXPECT_LT( world.Live.size(), scene.Entities.size() );
}

TEST( WorldPartitionStreamer, ARecordWithoutAnIdIsRefusedByName )
{
    SceneSerialized scene;
    scene.SceneName      = "Idless";
    scene.Entities       = World();
    scene.WorldPartition = Grid();
    scene.Entities[5].id.reset();
    scene.Entities[5].Tag = "NoIdHere";
    SetWorld              world( scene.Entities.size() );
    const StreamingSource source{ CellCentre( 0, 1 ) };

    auto begun = BeginWorldStreaming( scene, {}, Settings(), std::span( &source, 1 ), world );

    ASSERT_FALSE( begun.IsSuccess() );
    EXPECT_NE( begun.GetError().find( "NoIdHere" ), std::string::npos ) << begun.GetError();
    EXPECT_NE( begun.GetError().find( "Idless" ), std::string::npos ) << begun.GetError();
    EXPECT_EQ( world.DestroyCalls, 0u ) << "a refused start destroyed entities";
}

TEST( WorldPartitionStreamer, AFailedActivationIsAnErrorNamingTheUnit )
{
    const std::vector<EntityData> records = World();
    SetWorld                      world( records.size() );
    ResidencyExecutor             executor = Begin( records, StreamingSource{ CellCentre( 0, 1 ) }, world );
    world.FailWith                         = "prefab 'Rock.deprefab' is missing";

    std::string error;
    double      now = 0.0;
    for ( int frame = 0; frame < 4 && error.empty(); ++frame )
    {
        const StreamingSource source{ CellCentre( 12, 1 ) };
        auto                  tick = executor.Tick( std::span( &source, 1 ), now += kFrame, world );
        if ( !tick.IsSuccess() )
            error = tick.GetError();
    }
    EXPECT_NE( error.find( "activating unit" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "Rock.deprefab" ), std::string::npos ) << error;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
