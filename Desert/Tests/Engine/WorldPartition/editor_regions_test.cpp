// WP19 (decision O3): an editor region selects, over the descriptor index, exactly the composites whose footprint
// meets it, plus the always-loaded ones (Core/Serialize/EditorRegions.hpp). Cubes on a 100 m grid.

#include <Engine/Core/Serialize/EditorRegions.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace
{
    using namespace Desert::Core;

    // A 1 m cube at (x, z) cm.
    Rules::EntityDescriptor Cube( std::uint64_t id, float x, float z )
    {
        Rules::EntityDescriptor cube;
        cube.Id          = id;
        cube.Tag         = "Cube";
        cube.Components  = { "StaticMesh" };
        cube.Translation = glm::vec3( x, 0.0f, z );
        cube.Rotation    = glm::vec3( 0.0f );
        cube.Scale       = glm::vec3( 1.0f );
        cube.Boxes.push_back( Common::Math::AABB( glm::vec3( -50.0f ), glm::vec3( 50.0f ) ) );
        return cube;
    }

    struct CubeWorld
    {
        std::vector<Rules::EntityDescriptor> Descriptors;
        std::vector<std::uint64_t>           Ids;
        WorldPartitionSerialized             Partition;

        CubeWorld()
        {
            Partition.Grids.resize( 1 );
            Partition.Grids[0].CellSize     = 12800.0f;
            Partition.Grids[0].LoadingRange = 25600.0f;
            std::uint64_t id                = 1;
            for ( int gx = 0; gx < 10; ++gx )
                for ( int gz = 0; gz < 10; ++gz )
                    Descriptors.push_back( Cube( id++, 500.0f + 10000.0f * gx, 500.0f + 10000.0f * gz ) );
            Rules::EntityDescriptor sun;
            sun.Id          = id++;
            sun.Tag         = "Sun";
            sun.Components  = { "AlwaysLoaded" };
            sun.Translation = glm::vec3( 90000.0f, 0.0f, 90000.0f );
            sun.Reason      = Rules::AlwaysLoadedReason::Author;
            Descriptors.push_back( sun );
            for ( const auto& d : Descriptors )
                Ids.push_back( *d.Id );
        }

        EditorRegions::RegionSelection Select( std::span<const Rules::CellBounds> regions ) const
        {
            const auto plan = Rules::PlanWorldPartition( std::span<const Rules::EntityDescriptor>( Descriptors ),
                                                         Partition );
            return EditorRegions::SelectRecords( plan, Ids, regions );
        }
    };
} // namespace

TEST( EditorRegions, ARegionHoldsExactlyTheCubesItMeetsAndTheAlwaysLoaded )
{
    const CubeWorld world;
    // 0..250 m on X, 0..150 m on Z: cube columns 0,1,2 and rows 0,1 -> six cubes.
    const Rules::CellBounds region{ 0.0f, 0.0f, 25000.0f, 15000.0f };
    const auto              selection = world.Select( std::span<const Rules::CellBounds>( &region, 1 ) );

    std::vector<std::uint64_t> expected;
    for ( int gx = 0; gx < 3; ++gx )
        for ( int gz = 0; gz < 2; ++gz )
            expected.push_back( static_cast<std::uint64_t>( 1 + gx * 10 + gz ) );
    expected.push_back( 101 ); // the sun
    std::sort( expected.begin(), expected.end() );
    std::vector<std::uint64_t> got = selection.Records;
    std::sort( got.begin(), got.end() );
    EXPECT_EQ( got, expected );
    EXPECT_EQ( selection.InRegions, 6u );
    EXPECT_EQ( selection.AlwaysLoaded, 1u );
}

TEST( EditorRegions, NoRegionHoldsOnlyTheAlwaysLoaded )
{
    const CubeWorld world;
    const auto      selection = world.Select( {} );
    EXPECT_EQ( selection.Records, ( std::vector<std::uint64_t>{ 101 } ) );
}

TEST( EditorRegions, ARegionInEmptyGroundHoldsNoCube )
{
    const CubeWorld         world;
    const Rules::CellBounds empty{ 2000.0f, 2000.0f, 9000.0f, 9000.0f };
    const auto selection = world.Select( std::span<const Rules::CellBounds>( &empty, 1 ) );
    EXPECT_EQ( selection.InRegions, 0u );
    EXPECT_EQ( selection.Records, ( std::vector<std::uint64_t>{ 101 } ) );
}
