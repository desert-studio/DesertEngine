// FO-8: a Prefab foliage type (UE UFoliageType_Actor). The brush writes transforms into the field exactly as for
// a Mesh type; the prefab instances are REALIZED from those transforms (World::Foliage::ApplyPrefabFoliage).
// These tests drive the real brush (FoliageBrush.cpp, its own PCG32 stream — no std distributions) and the real
// realization rule against a host that keeps a small entity tree per instance, the shape the Scene half gives it:
// a prefab instance is a root with its children, appended as the field's last child.

#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Engine/World/Foliage/FoliageCells.hpp>
#include <Engine/World/Foliage/FoliagePrefabs.hpp>

#include <Common/Core/Core.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using namespace Desert;
    using namespace Desert::Editor::Tools;
    using Desert::Assets::Serialization::FoliageTypeData;
    using Desert::Assets::Serialization::FoliageTypeKind;

    constexpr const char* kPrefabGuid = "5a5a5a5a0b0b0b0b1c1c1c1c2d2d2d2d";

    // The test prefab: a rock root with two parts (a collider and a light), the "entity with children" each
    // instance must be.
    constexpr std::size_t kPartsPerInstance = 2;

    FoliageTypeData PrefabType()
    {
        FoliageTypeData type;
        type.Kind          = FoliageTypeKind::Prefab;
        type.Prefab        = { kPrefabGuid, "Prefabs/Test_Rock.deprefab" };
        type.IncludeInHLOD = false;
        type.Density       = 100.0f;
        return type;
    }

    FoliageBrushWorld Ground()
    {
        FoliageBrushWorld world;
        world.Trace = []( const glm::vec3& a, const glm::vec3& b,
                          const FoliageSurfaceFilter& ) -> std::optional<FoliageTraceHit>
        {
            if ( ( a.y > 0.0f ) == ( b.y > 0.0f ) )
                return std::nullopt;
            const float t = a.y / ( a.y - b.y );
            return FoliageTraceHit{
                 a + t * ( b - a ), { 0.0f, 1.0f, 0.0f }, FoliageSurface::Landscape, std::nullopt };
        };
        world.LayerWeightAt = []( const glm::vec3& ) { return std::optional<float>(); };
        return world;
    }

    FoliageBrushDab DabAt( glm::vec3 centre, float radius )
    {
        FoliageBrushDab dab;
        dab.Center = centre;
        dab.Radius = radius;
        return dab;
    }

    // One prefab instance as the Scene half makes it: an id (to see which entities survive a pass), its
    // transform, and its parts.
    struct Instance
    {
        uint32_t              Id = 0;
        glm::mat4             Transform{ 1.0f };
        std::vector<uint32_t> Parts;
    };

    // The field's children. Spawn appends, DestroyLast takes the last with its parts, as the Scene half does.
    struct TreeHost
    {
        std::vector<Instance> Children;
        uint32_t              NextId  = 1;
        std::size_t           Spawned = 0, Destroyed = 0, Placed = 0;
        bool                  FailSpawn = false;

        World::Foliage::PrefabFoliageHost Host()
        {
            World::Foliage::PrefabFoliageHost host;
            host.Realized = [this]
            {
                std::vector<glm::mat4> out;
                for ( const Instance& child : Children )
                    out.push_back( child.Transform );
                return out;
            };
            host.Spawn = [this]() -> Common::BoolResultStr
            {
                if ( FailSpawn )
                    return Common::MakeError<bool>( "the prefab refused" );
                Instance made;
                made.Id = NextId++;
                for ( std::size_t p = 0; p < kPartsPerInstance; ++p )
                    made.Parts.push_back( NextId++ );
                Children.push_back( std::move( made ) );
                ++Spawned;
                return BOOLSUCCESS;
            };
            host.DestroyLast = [this]
            {
                Children.pop_back();
                ++Destroyed;
            };
            host.Place = [this]( std::size_t index, const glm::mat4& local )
            {
                Children.at( index ).Transform = local;
                ++Placed;
            };
            return host;
        }

        std::size_t Entities() const
        {
            std::size_t n = 0;
            for ( const Instance& child : Children )
                n += 1 + child.Parts.size();
            return n;
        }
    };

    // One stroke of @p dabs dabs along x from the seed, as FoliagePaintTool paints: each dab adds to the field.
    std::vector<glm::mat4> Paint( uint64_t seed, int dabs, float x0 = 0.0f )
    {
        const FoliageTypeData  type  = PrefabType();
        const auto             world = Ground();
        FoliageStroke          stroke( seed );
        std::vector<glm::mat4> field;
        for ( int i = 0; i < dabs; ++i )
        {
            const auto added =
                 FoliageBrushAdd( type, DabAt( { x0 + 200.0f * static_cast<float>( i ), 0.0f, 0.0f }, 400.0f ),
                                  field, stroke.Random(), world );
            field.insert( field.end(), added.begin(), added.end() );
        }
        return field;
    }

    void ExpectRealizes( const TreeHost& tree, const std::vector<glm::mat4>& field )
    {
        ASSERT_EQ( tree.Children.size(), field.size() );
        for ( std::size_t i = 0; i < field.size(); ++i )
        {
            EXPECT_EQ( tree.Children[i].Transform, field[i] ) << "instance " << i;
            EXPECT_EQ( tree.Children[i].Parts.size(), kPartsPerInstance ) << "instance " << i;
        }
    }
} // namespace

TEST( PrefabFoliage, AStrokePlacesOnePrefabInstanceWithItsPartsPerTransform )
{
    const auto field = Paint( 7u, 5 );
    ASSERT_GT( field.size(), 20u ) << "the stroke placed too few instances to say anything";

    TreeHost tree;
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    ExpectRealizes( tree, field );
    EXPECT_EQ( tree.Entities(), field.size() * ( 1 + kPartsPerInstance ) );

    // A second pass over an unchanged field touches nothing (it runs every frame).
    const std::size_t spawned = tree.Spawned, placed = tree.Placed;
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    EXPECT_EQ( tree.Spawned, spawned );
    EXPECT_EQ( tree.Placed, placed );
    EXPECT_TRUE( World::Foliage::PlanPrefabFoliage( field, field ).Empty() );
}

TEST( PrefabFoliage, TheSameSeedPlacesTheSameInstancesBitForBit )
{
    const auto a = Paint( 42u, 4 );
    const auto b = Paint( 42u, 4 );
    const auto c = Paint( 43u, 4 );
    ASSERT_FALSE( a.empty() );
    EXPECT_EQ( a, b );
    EXPECT_NE( a, c ) << "another seed must scatter differently";

    TreeHost ta, tb;
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( a, ta.Host() ) );
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( b, tb.Host() ) );
    ASSERT_EQ( ta.Children.size(), tb.Children.size() );
    for ( std::size_t i = 0; i < ta.Children.size(); ++i )
        EXPECT_EQ( ta.Children[i].Transform, tb.Children[i].Transform );
}

TEST( PrefabFoliage, UndoOfAStrokeTakesItsInstancesAndKeepsTheOthersEntities )
{
    TreeHost tree;
    auto     field = Paint( 3u, 2 );
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    std::vector<uint32_t> before;
    for ( const Instance& child : tree.Children )
        before.push_back( child.Id );

    // A second stroke elsewhere, one undo step (FoliageStroke), then undone: Before is written back.
    const Common::UUID id( 9u );
    FoliageStroke      stroke( 11u );
    stroke.Touch( id, field );
    const auto added = FoliageBrushAdd( PrefabType(), DabAt( { 3000.0f, 0.0f, 0.0f }, 400.0f ), field,
                                        stroke.Random(), Ground() );
    ASSERT_FALSE( added.empty() );
    field.insert( field.end(), added.begin(), added.end() );
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    ExpectRealizes( tree, field );

    const auto entries = stroke.Finish( [&]( const Common::UUID& ) { return &field; } );
    ASSERT_EQ( entries.size(), 1u );
    field = entries[0].Before;
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    ExpectRealizes( tree, field );
    EXPECT_EQ( tree.Destroyed, added.size() );
    for ( std::size_t i = 0; i < before.size(); ++i )
        EXPECT_EQ( tree.Children[i].Id, before[i] ) << "undo must not rebuild the instances it did not touch";
}

TEST( PrefabFoliage, RemoveAndDeleteSelectedTakeExactlyTheirInstances )
{
    TreeHost tree;
    auto     field = Paint( 5u, 6 );
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    const std::size_t all = field.size();

    // Remove (the sphere) — the realized instances follow the field.
    const std::size_t removed = FoliageBrushRemove( field, { 0.0f, 0.0f, 0.0f }, 300.0f );
    ASSERT_GT( removed, 0u );
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    ExpectRealizes( tree, field );
    EXPECT_EQ( tree.Children.size(), all - removed );

    // Select (Lasso) then Delete: the selected ones leave, every other instance keeps its transform.
    FoliageSelection  selected;
    const std::size_t picked = FoliageSelectInSphere( field, { 800.0f, 0.0f, 0.0f }, 250.0f, true, selected );
    ASSERT_GT( picked, 0u );
    ASSERT_EQ( FoliageRemoveSelected( field, selected ), picked );
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, tree.Host() ) );
    ExpectRealizes( tree, field );
    EXPECT_EQ( tree.Children.size(), all - removed - picked );
}

TEST( PrefabFoliage, AFailedSpawnStopsThePassAndNamesTheInstance )
{
    const auto field = Paint( 1u, 1 );
    ASSERT_GE( field.size(), 2u );
    TreeHost tree;
    tree.FailSpawn    = true;
    const auto result = World::Foliage::ApplyPrefabFoliage( field, tree.Host() );
    ASSERT_FALSE( result );
    EXPECT_NE( result.GetError().find( "instance 0 of " + std::to_string( field.size() ) ), std::string::npos )
         << result.GetError();
    EXPECT_NE( result.GetError().find( "the prefab refused" ), std::string::npos ) << result.GetError();
    EXPECT_TRUE( tree.Children.empty() );
}

TEST( PrefabFoliage, ASavedTypeAndFieldLoadIntoTheSameInstances )
{
    // What is saved: the type file and the field's transforms. The realized children are never saved (the
    // serializer skips a foliage field's descendants); a load realizes them afresh from the transforms.
    const FoliageTypeData type = PrefabType();
    const auto parsed = Assets::Serialization::ParseFoliageType( Assets::Serialization::WriteFoliageType( type ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().Kind, FoliageTypeKind::Prefab );
    EXPECT_EQ( parsed.GetValue().Prefab, type.Prefab );

    const auto field = Paint( 21u, 3 );
    TreeHost   edited;
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( field, edited.Host() ) );
    const std::vector<glm::mat4> saved = field;

    TreeHost loaded;
    ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( saved, loaded.Host() ) );
    ExpectRealizes( loaded, field );
    EXPECT_EQ( loaded.Entities(), edited.Entities() );
}

TEST( PrefabFoliage, InstancesAreFiledByCellAndFollowAnInstanceOverTheEdge )
{
    // FO-6: one field per type per cell; each cell field realizes its own instances.
    constexpr double                cellSize = 1000.0;
    const auto                      stroke = Paint( 17u, 8, -700.0f ); // x from -700 to 700: across the x = 0 edge
    World::Foliage::FoliageGathered gathered;
    gathered.Instances = stroke;
    const auto cells   = World::Foliage::ScatterFoliage( gathered, {}, cellSize );
    ASSERT_TRUE( cells ) << cells.GetError();
    ASSERT_GE( cells.GetValue().size(), 2u ) << "the stroke must cross a cell edge";

    std::vector<TreeHost> hosts( cells.GetValue().size() );
    std::size_t           total = 0;
    for ( std::size_t c = 0; c < hosts.size(); ++c )
    {
        const auto& cell = cells.GetValue()[c];
        ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( cell.Instances, hosts[c].Host() ) );
        ExpectRealizes( hosts[c], cell.Instances );
        for ( const Instance& child : hosts[c].Children )
            EXPECT_EQ( World::Foliage::FoliageCellOf( child.Transform, cellSize ), cell.Cell );
        total += hosts[c].Children.size();
    }
    EXPECT_EQ( total, stroke.size() );

    // Move the first instance of cell 0 into cell 1: re-filed, it leaves cell 0's children and joins cell 1's.
    std::vector<World::Foliage::FoliageCellField> fields = cells.GetValue();
    std::vector<World::Foliage::CellCoord>        order;
    for ( const auto& f : fields )
        order.push_back( f.Cell );
    const glm::vec3 target    = World::Foliage::FoliageCellAnchor( fields[1].Cell, cellSize );
    fields[0].Instances[0][3] = glm::vec4( target, 1.0f );
    const auto gatheredAgain  = World::Foliage::GatherFoliage( fields );
    ASSERT_TRUE( gatheredAgain ) << gatheredAgain.GetError();
    const auto refiled = World::Foliage::ScatterFoliage( gatheredAgain.GetValue(), order, cellSize );
    ASSERT_TRUE( refiled ) << refiled.GetError();
    ASSERT_EQ( refiled.GetValue().size(), fields.size() );
    const std::size_t before0 = hosts[0].Children.size(), before1 = hosts[1].Children.size();
    for ( std::size_t c = 0; c < hosts.size(); ++c )
    {
        ASSERT_TRUE( World::Foliage::ApplyPrefabFoliage( refiled.GetValue()[c].Instances, hosts[c].Host() ) );
        ExpectRealizes( hosts[c], refiled.GetValue()[c].Instances );
    }
    EXPECT_EQ( hosts[0].Children.size(), before0 - 1 );
    EXPECT_EQ( hosts[1].Children.size(), before1 + 1 );
}

TEST( PrefabFoliage, APrefabIsNamedByItsHeaderGuid )
{
    const auto dir = std::filesystem::temp_directory_path() / "PrefabFoliageTest";
    std::filesystem::create_directories( dir );
    const auto good = dir / "Test_Rock.deprefab";
    std::ofstream( good ) << R"({
    "Header": { "Kind": "Prefab", "Guid": ")"
                          << kPrefabGuid << R"(", "Versions": { "SCNE": 36, "UNIT": 1 }, "Dependencies": [] },
    "Name": "Test_Rock",
    "Entities": []
})";
    const auto guid = World::Foliage::PrefabFileGuid( good );
    ASSERT_TRUE( guid ) << guid.GetError();
    EXPECT_EQ( guid.GetValue(), kPrefabGuid );

    const auto headless = dir / "Headless.deprefab";
    std::ofstream( headless ) << R"({ "Name": "Headless", "Entities": [] })";
    const auto refused = World::Foliage::PrefabFileGuid( headless );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "Headless.deprefab" ), std::string::npos ) << refused.GetError();
    EXPECT_FALSE( World::Foliage::PrefabFileGuid( dir / "Missing.deprefab" ) );
    std::filesystem::remove_all( dir );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
