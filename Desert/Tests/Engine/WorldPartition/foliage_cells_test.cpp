// FO-6: foliage filed by World Partition cell. The rule (World/Foliage/FoliageCells.hpp) and what it buys the
// partition: a field per type per cell stays in its cell, is saved as its own file, and leaves with its cell.

#include <Engine/World/Foliage/FoliageCells.hpp>

#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Json/Carry.hpp>
#include <Common/Json/Document.hpp>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace F  = Desert::World::Foliage;
    namespace R  = Desert::Core::Rules;
    namespace EE = Desert::Core::ExternalEntities;
    using Desert::Assets::EntityData;
    using Desert::Core::WorldPartitionGridSerialized;
    using Desert::Core::WorldPartitionSerialized;

    constexpr double kCell = 12800.0; // the default grid: 128 m

    glm::mat4 At( float x, float z )
    {
        return glm::translate( glm::mat4( 1.0f ), glm::vec3( x, 50.0f, z ) );
    }

    WorldPartitionSerialized Grid()
    {
        WorldPartitionSerialized partition;
        partition.Grids.push_back( WorldPartitionGridSerialized{ static_cast<float>( kCell ), 25600.0f } );
        return partition;
    }

    std::string MatrixJson( const glm::mat4& m )
    {
        std::string out = "[";
        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 4; ++r )
                out += ( c + r == 0 ? "" : "," ) + std::to_string( m[c][r] );
        return out + "]";
    }

    std::string InstancesJson( const std::vector<glm::mat4>& instances )
    {
        std::string out = "[";
        for ( std::size_t i = 0; i < instances.size(); ++i )
            out += ( i == 0 ? "" : "," ) + MatrixJson( instances[i] );
        return out + "]";
    }

    constexpr const char* kTypeGuid = "\"5f0e3c1a-0000-4000-8000-00000000f006\"";

    // A foliage field as the scene file holds it: the entity at @p position, its type, its instances.
    EntityData FieldRecord( std::uint64_t id, const glm::vec3& position, const std::vector<glm::mat4>& instances )
    {
        EntityData data;
        data.id          = Common::UUID( id );
        data.Tag         = "Foliage_Grass";
        data.Translation = position;
        const auto ism   = Common::Json::Parse( R"({"InstanceTransforms":)" + InstancesJson( instances ) + "}" );
        EXPECT_TRUE( ism.IsSuccess() );
        data.Components["InstancedStaticMesh"] = ism.GetValue();
        const auto type = Common::Json::Parse( std::string( R"({"FoliageTypeGuid":)" ) + kTypeGuid + "}" );
        EXPECT_TRUE( type.IsSuccess() );
        data.Components["Foliage"] = type.GetValue();
        return data;
    }

    // A dab of a brush across the X edge of cells (0,0) and (1,0), ten instances either side.
    std::vector<glm::mat4> StrokeOverTheEdge()
    {
        std::vector<glm::mat4> dab;
        for ( int i = 0; i < 10; ++i )
        {
            dab.push_back( At( 12800.0f - 10.0f - 20.0f * static_cast<float>( i ), 600.0f ) );
            dab.push_back( At( 12800.0f + 10.0f + 20.0f * static_cast<float>( i ), 600.0f ) );
        }
        return dab;
    }

    std::vector<F::FoliageCellField> Scatter( const F::FoliageGathered& gathered,
                                              std::vector<F::CellCoord> cells = {} )
    {
        auto scattered = F::ScatterFoliage( gathered, cells, kCell );
        EXPECT_TRUE( scattered ) << ( scattered ? "" : scattered.GetError() );
        return scattered ? scattered.ExtractValue() : std::vector<F::FoliageCellField>{};
    }

    // The scattered fields as scene records, each entity standing where the editor puts it.
    std::vector<EntityData> Records( const std::vector<F::FoliageCellField>& fields )
    {
        std::vector<EntityData> records;
        for ( std::size_t i = 0; i < fields.size(); ++i )
            records.push_back(
                 FieldRecord( 100 + i, F::FoliageCellAnchor( fields[i].Cell, kCell ), fields[i].Instances ) );
        return records;
    }
} // namespace

TEST( FoliageCells, AStrokeAcrossACellEdgeLandsInTwoCellFields )
{
    F::FoliageGathered gathered;
    gathered.Instances = StrokeOverTheEdge();
    const auto fields  = Scatter( gathered );
    ASSERT_EQ( fields.size(), 2u );
    EXPECT_EQ( fields[0].Cell, ( F::CellCoord{ 0, 0 } ) );
    EXPECT_EQ( fields[1].Cell, ( F::CellCoord{ 1, 0 } ) );
    EXPECT_EQ( fields[0].Instances.size(), 10u );
    EXPECT_EQ( fields[1].Instances.size(), 10u );
    for ( const auto& field : fields )
        for ( const auto& instance : field.Instances )
            EXPECT_EQ( F::FoliageCellOf( instance, kCell ), field.Cell ) << "an instance outside its field's cell";
}

TEST( FoliageCells, CellFieldsDoNotGrowACellWhereOneFieldDid )
{
    // One field holding the whole stroke (the pre-FO-6 shape) straddles the edge: the planner has to promote
    // it to a level that holds both cells. With a far patch on the other side of the origin no aligned cell of
    // any level holds it, and the field is always loaded (NoFit) — the grass of the whole map never streams.
    // Filed by cell, every field fits a level-0 cell.
    std::vector<glm::mat4> stroke = StrokeOverTheEdge();
    const auto edge = R::PlanWorldPartition( std::vector<EntityData>{ FieldRecord( 1, {}, stroke ) }, Grid() );
    EXPECT_EQ( edge.MaxLevel, 1 ) << "the unsplit field should have grown its cell; the test proves nothing";
    stroke.push_back( At( -30000.0f, -30000.0f ) ); // and a patch far away in cell (-3, -3)
    const auto whole = R::PlanWorldPartition( std::vector<EntityData>{ FieldRecord( 1, {}, stroke ) }, Grid() );
    ASSERT_EQ( whole.AlwaysLoaded.size(), 1u ) << "the unsplit field should not have fitted any cell";
    EXPECT_EQ( whole.Composites[whole.AlwaysLoaded[0]].Reason, R::AlwaysLoadedReason::NoFit );

    F::FoliageGathered gathered;
    gathered.Instances = stroke;
    const auto records = Records( Scatter( gathered ) );
    ASSERT_EQ( records.size(), 3u );
    const auto plan = R::PlanWorldPartition( records, Grid() );
    EXPECT_EQ( plan.MaxLevel, 0 ) << "a cell field was promoted past level 0";
    EXPECT_EQ( plan.Cells.size(), 3u );
    EXPECT_TRUE( plan.AlwaysLoaded.empty() );
}

TEST( FoliageCells, MovingAnInstanceOverTheEdgeMovesItIntoTheNeighbourWithItsSelection )
{
    std::vector<F::FoliageCellField> fields( 2 );
    fields[0].Cell      = { 0, 0 };
    fields[0].Instances = { At( 100.0f, 100.0f ), At( 12700.0f, 100.0f ), At( 200.0f, 100.0f ) };
    fields[0].Selected  = { 1 };
    fields[1].Cell      = { 1, 0 };
    fields[1].Instances = { At( 13000.0f, 100.0f ) };

    auto gathered = F::GatherFoliage( fields );
    ASSERT_TRUE( gathered ) << gathered.GetError();
    ASSERT_EQ( gathered.GetValue().Selected, ( std::vector<std::uint32_t>{ 1 } ) );
    auto moved = gathered.ExtractValue();
    moved.Instances[1][3].x += 200.0f; // the selected instance moved 2 m over the edge

    const auto after = Scatter( moved, { fields[0].Cell, fields[1].Cell } );
    ASSERT_EQ( after.size(), 2u ) << "no new field: the neighbour cell already had one";
    ASSERT_EQ( after[0].Instances.size(), 2u );
    ASSERT_EQ( after[1].Instances.size(), 2u );
    EXPECT_FLOAT_EQ( after[0].Instances[1][3].x, 200.0f ) << "the field left behind kept its order";
    EXPECT_FLOAT_EQ( after[1].Instances[0][3].x, 12900.0f ) << "gathered first, so first in the neighbour";
    EXPECT_TRUE( after[0].Selected.empty() );
    EXPECT_EQ( after[1].Selected, ( std::vector<std::uint32_t>{ 0 } ) ) << "the selection did not follow";
}

TEST( FoliageCells, ACellWhoseInstancesAllLeftComesBackEmptyAndUntouchedFieldsIdentical )
{
    std::vector<F::FoliageCellField> fields( 3 );
    fields[0]     = { { 0, 0 }, { At( 10.0f, 10.0f ) }, {} };
    fields[1]     = { { 5, 5 }, { At( 5.5f * 12800.0f, 5.5f * 12800.0f ) }, {} };
    fields[2]     = { { 1, 0 }, { At( 12900.0f, 10.0f ), At( 13000.0f, 10.0f ) }, {} };
    auto gathered = F::GatherFoliage( fields );
    ASSERT_TRUE( gathered );
    auto g              = gathered.ExtractValue();
    g.Instances[0][3].x = -500.0f; // (0,0) -> (-1,0), a cell with no field yet
    const auto after    = Scatter( g, { fields[0].Cell, fields[1].Cell, fields[2].Cell } );
    ASSERT_EQ( after.size(), 4u );
    EXPECT_TRUE( after[0].Instances.empty() );
    EXPECT_EQ( after[1].Instances, fields[1].Instances );
    EXPECT_EQ( after[2].Instances, fields[2].Instances );
    EXPECT_EQ( after[3].Cell, ( F::CellCoord{ -1, 0 } ) );
}

TEST( FoliageCells, BadInputIsRefusedByName )
{
    F::FoliageGathered gathered;
    gathered.Instances = { At( 0.0f, 0.0f ) };
    gathered.Selected  = { 3 };
    const auto past    = F::ScatterFoliage( gathered, {}, kCell );
    ASSERT_FALSE( past );
    EXPECT_NE( past.GetError().find( "selected instance 3 of 1" ), std::string::npos ) << past.GetError();

    gathered.Selected = {};
    EXPECT_FALSE( F::ScatterFoliage( gathered, {}, 0.0 ) );
    const std::vector<F::CellCoord> twice = { { 1, 1 }, { 1, 1 } };
    const auto                      dup   = F::ScatterFoliage( gathered, twice, kCell );
    ASSERT_FALSE( dup );
    EXPECT_NE( dup.GetError().find( "(1, 1) is listed twice" ), std::string::npos ) << dup.GetError();

    const std::vector<F::FoliageCellField> bad = { { { 2, 0 }, { At( 0.0f, 0.0f ) }, { 1 } } };
    const auto                             g   = F::GatherFoliage( bad );
    ASSERT_FALSE( g );
    EXPECT_NE( g.GetError().find( "cell 2, 0" ), std::string::npos ) << g.GetError();
}

TEST( FoliageCells, TheBrushSeesExactlyTheCellsItsDiscReaches )
{
    EXPECT_TRUE( F::FoliageCellTouchesDisc( { 1, 0 }, kCell, 12790.0f, 100.0f, 20.0f ) );
    EXPECT_FALSE( F::FoliageCellTouchesDisc( { 1, 0 }, kCell, 12700.0f, 100.0f, 20.0f ) );
    EXPECT_TRUE( F::FoliageCellTouchesDisc( { 0, 0 }, kCell, 12700.0f, 100.0f, 20.0f ) );
    // The corner: inside the bounding square of the disc but outside the disc.
    EXPECT_FALSE( F::FoliageCellTouchesDisc( { 1, 1 }, kCell, 12790.0f, 12790.0f, 12.0f ) );
}

// ── OFPA: one file per cell field ──────────────────────────────────────────────────────────────────────

namespace
{
    std::string SceneJson( const std::vector<F::FoliageCellField>& fields )
    {
        std::string records;
        for ( std::size_t i = 0; i < fields.size(); ++i )
            records += ( i == 0 ? "" : "," ) + std::string( R"({"id":)" ) + std::to_string( 100 + i ) +
                       R"(,"Tag":"Foliage_Grass","Foliage":{"FoliageTypeGuid":)" + kTypeGuid +
                       R"(},"InstancedStaticMesh":{"InstanceTransforms":)" + InstancesJson( fields[i].Instances ) +
                       "}}";
        return R"({"SceneName":"World","Entities":[)" + records +
               R"(],"WorldPartition":{"Grids":[{"CellSize":12800.0,"LoadingRange":25600.0}]}})";
    }

    std::map<std::string, std::string> Snapshot( const std::filesystem::path& root )
    {
        std::map<std::string, std::string> files;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
            if ( entry.is_regular_file() )
            {
                std::ifstream      in( entry.path(), std::ios::binary );
                std::ostringstream bytes;
                bytes << in.rdbuf();
                files[entry.path().lexically_relative( root ).generic_string()] = bytes.str();
            }
        return files;
    }

    Common::Json::TextDocument Doc( const std::string& json )
    {
        auto document = Common::Json::TextDocument::Parse( json );
        EXPECT_TRUE( document ) << ( document ? "" : document.GetError() );
        return document ? document.ExtractValue() : Common::Json::TextDocument();
    }
} // namespace

TEST( FoliageCells, EachCellFieldIsItsOwnFileAndPaintingOneCellRewritesOneFile )
{
    const auto dir =
         std::filesystem::temp_directory_path() /
         ( "fo6_ofpa_" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    const auto scene = dir / "World.desce";
    std::filesystem::create_directories( dir );

    F::FoliageGathered gathered;
    gathered.Instances = StrokeOverTheEdge();
    gathered.Instances.push_back( At( 40000.0f, 600.0f ) ); // cell (3, 0)
    auto fields = Scatter( gathered );
    ASSERT_EQ( fields.size(), 3u );

    const auto first = EE::WriteSceneFile( scene, Doc( SceneJson( fields ) ) );
    ASSERT_TRUE( first ) << first.GetError();
    EXPECT_EQ( first.GetValue().Written, 4u ); // the header and one file per cell field
    for ( std::size_t i = 0; i < fields.size(); ++i )
        EXPECT_TRUE( std::filesystem::is_regular_file( EE::FileOf( scene, Common::UUID( 100 + i ) ) ) ) << i;

    // A second dab, only in cell (1, 0): gather that field, add, scatter back into it.
    std::vector<F::FoliageCellField> touched = { fields[1] };
    auto                             g       = F::GatherFoliage( touched );
    ASSERT_TRUE( g );
    auto more = g.ExtractValue();
    more.Instances.push_back( At( 13500.0f, 900.0f ) );
    const std::vector<F::CellCoord> cells = { fields[1].Cell };
    const auto                      back  = Scatter( more, cells );
    ASSERT_EQ( back.size(), 1u );
    fields[1] = back[0];

    const auto before = Snapshot( dir );
    const auto edit   = EE::WriteSceneFile( scene, Doc( SceneJson( fields ) ) );
    ASSERT_TRUE( edit ) << edit.GetError();
    const auto            after = Snapshot( dir );
    std::set<std::string> changed;
    for ( const auto& [file, bytes] : after )
        if ( !before.contains( file ) || before.at( file ) != bytes )
            changed.insert( file );
    const std::string expected =
         EE::FileOf( scene, Common::UUID( 101 ) ).lexically_relative( dir ).generic_string();
    EXPECT_EQ( changed, std::set<std::string>{ expected } );
    EXPECT_EQ( edit.GetValue().Written, 1u );

    std::error_code ec;
    std::filesystem::remove_all( dir, ec );
}

// ── Residency: a cell field leaves with its cell, and so does the last hold on its type ─────────────────

namespace
{
    struct LiveSet final : R::ResidencyWorld
    {
        std::set<std::size_t> Live;
        explicit LiveSet( std::size_t records )
        {
            for ( std::size_t record = 0; record < records; ++record )
                Live.insert( record );
        }
        Common::BoolResultStr Activate( std::size_t, std::span<const std::size_t> records ) override
        {
            Live.insert( records.begin(), records.end() );
            return Common::MakeSuccess( true );
        }
        void Destroy( std::span<const std::size_t> records ) override
        {
            for ( const std::size_t record : records )
                Live.erase( record );
        }
    };

    // How many live records name the foliage type — what keeps it (and through it the mesh) out of the
    // eviction sweep's reach (SceneAssetRoots.cpp marks every FoliageComponent's type).
    std::size_t Holders( const std::vector<EntityData>& records, const LiveSet& world )
    {
        std::size_t holders = 0;
        for ( const std::size_t record : world.Live )
            for ( const auto& [name, payload] : records[record].Components )
                holders += name == "Foliage" ? 1 : 0;
        return holders;
    }
} // namespace

TEST( FoliageCells, AnUnloadedCellTakesItsFoliageAndTheLastCellTakesTheTypesHold )
{
    // Foliage in cells (0,0) and (1,0); a block far out at cell (8,0) that the flight ends on.
    F::FoliageGathered gathered;
    gathered.Instances = StrokeOverTheEdge();
    auto records       = Records( Scatter( gathered ) );
    ASSERT_EQ( records.size(), 2u );
    EntityData far;
    far.id          = Common::UUID( 9 );
    far.Tag         = "Block";
    far.Translation = F::FoliageCellAnchor( { 8, 0 }, kCell );
    records.push_back( far );

    const auto           plan = R::PlanWorldPartition( records, Grid() );
    R::ResidencySettings settings;
    settings.ActivationBudgetMs = 1.0;
    settings.MsPerRecord        = 0.1;
    R::StreamingSource source{ F::FoliageCellAnchor( { 0, 0 }, kCell ) };
    source.RangeScale = 0.0f; // only the cell the source stands in
    LiveSet world( records.size() );
    auto    begun = R::ResidencyExecutor::Begin( plan, Grid(), records, settings, std::span( &source, 1 ), world );
    ASSERT_TRUE( begun ) << begun.GetError();
    auto   executor = begun.ExtractValue();
    double now      = 0.0;
    auto   settle   = [&]( const glm::vec3& at )
    {
        source.Position = at;
        for ( int frame = 0; frame < 30; ++frame )
            ASSERT_TRUE( executor.Tick( std::span( &source, 1 ), now += 1.0 / 60.0, world ) );
    };

    settle( F::FoliageCellAnchor( { 0, 0 }, kCell ) );
    EXPECT_EQ( world.Live, ( std::set<std::size_t>{ 0 } ) ) << "standing in (0,0): only its foliage is live";
    EXPECT_EQ( Holders( records, world ), 1u );

    settle( F::FoliageCellAnchor( { 1, 0 }, kCell ) );
    EXPECT_EQ( world.Live, ( std::set<std::size_t>{ 1 } ) ) << "(0,0)'s instances left with their cell";
    EXPECT_EQ( Holders( records, world ), 1u ) << "the type is still held by (1,0)";

    settle( F::FoliageCellAnchor( { 8, 0 }, kCell ) );
    EXPECT_EQ( world.Live, ( std::set<std::size_t>{ 2 } ) );
    EXPECT_EQ( Holders( records, world ), 0u ) << "no resident cell holds the type: its mesh may be evicted";
}
