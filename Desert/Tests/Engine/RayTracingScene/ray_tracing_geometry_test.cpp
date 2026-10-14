// A static mesh's BLAS build input (RT-SCENE): DescribeStaticMeshGeometry is the one rule the Vulkan BLAS object
// maps to VkAccelerationStructureGeometryTrianglesDataKHR + VkAccelerationStructureBuildRangeInfoKHR. Device-free.

#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace Desert;
using namespace Desert::Graphic::RayTracing;

namespace
{
    Submesh MakeSubmesh( uint32_t vertexOffset, uint32_t vertexCount, uint32_t indexOffset, uint32_t indexCount )
    {
        Submesh submesh{};
        submesh.VertexOffset = vertexOffset;
        submesh.VertexCount  = vertexCount;
        submesh.IndexOffset  = indexOffset;
        submesh.IndexCount   = indexCount;
        return submesh;
    }
} // namespace

// Red if the stride or the position offset stops following Desert::Vertex (a BLAS built at the wrong stride reads
// normals as positions), if the index type stops being uint32, or if the byte offset is computed in indices.
TEST( RayTracingGeometry, ASubmeshDescribesTheMeshVertexLayoutAndItsIndexRangeInBytes )
{
    const std::vector<Submesh> submeshes{ MakeSubmesh( 100, 24, 36, 60 ) };
    const auto                 segments = DescribeStaticMeshGeometry( submeshes, 0 );
    ASSERT_EQ( segments.size(), 1u );
    const RayTracingGeometrySegment& s = segments[0];
    EXPECT_EQ( s.VertexFormat, RayTracingVertexFormat::Float3 );
    EXPECT_EQ( s.VertexStride, sizeof( Vertex ) );
    EXPECT_EQ( s.PositionOffset, offsetof( Vertex, Position ) );
    EXPECT_EQ( s.FirstVertex, 100u );
    EXPECT_EQ( s.MaxVertex, 123u );
    EXPECT_EQ( s.IndexType, RayTracingIndexType::UInt32 );
    EXPECT_EQ( s.IndexByteOffset, 36u * 4u );
    EXPECT_EQ( s.PrimitiveCount, 20u );
}

// Red if the BLAS follows the submesh's own range when it has a LOD chain of a different extent, or reads a
// coarser level: a ray sees LOD 0 whatever the raster drew.
TEST( RayTracingGeometry, TheBuildReadsLodZeroOfAChainedSubmesh )
{
    Submesh submesh     = MakeSubmesh( 0, 8, 0, 36 );
    submesh.LODs        = { { 600, 30 }, { 900, 12 } };
    const auto segments = DescribeStaticMeshGeometry( std::vector<Submesh>{ submesh }, 0 );
    ASSERT_EQ( segments.size(), 1u );
    EXPECT_EQ( segments[0].IndexByteOffset, 600u * 4u );
    EXPECT_EQ( segments[0].PrimitiveCount, 10u );
}

// Red if an empty submesh becomes a zero-primitive geometry (legal but a wasted build entry) or a partial triangle
// is counted as one (the build would read past the submesh's indices).
TEST( RayTracingGeometry, EmptySubmeshesAndPartialTrianglesAreNotGeometry )
{
    const std::vector<Submesh> submeshes{ MakeSubmesh( 0, 3, 0, 2 ), MakeSubmesh( 3, 0, 2, 0 ),
                                          MakeSubmesh( 3, 4, 2, 7 ) };
    const auto                 segments = DescribeStaticMeshGeometry( submeshes, 0 );
    ASSERT_EQ( segments.size(), 1u );
    EXPECT_EQ( segments[0].FirstVertex, 3u );
    EXPECT_EQ( segments[0].PrimitiveCount, 2u );
}

// Red if a hidden submesh still contributes a segment (a ray hits what the raster does not draw), if a visible one
// is dropped, if a segment's SubmeshIndex is its position rather than its submesh (the hit's material would
// shift), or if a submesh past the mask's 64 bits reads a wrapped bit and disappears.
TEST( RayTracingGeometry, HiddenSubmeshesContributeNoSegmentAndTheRestKeepTheirSubmeshIndex )
{
    std::vector<Submesh> submeshes;
    for ( uint32_t i = 0; i < 66; ++i )
        submeshes.push_back( MakeSubmesh( i * 3u, 3, i * 3u, 3 ) );

    const uint64_t hidden   = ( 1ull << 0 ) | ( 1ull << 2 ) | ( 1ull << 63 );
    const auto     segments = DescribeStaticMeshGeometry( submeshes, hidden );
    ASSERT_EQ( segments.size(), 63u );
    EXPECT_EQ( segments[0].SubmeshIndex, 1u );
    EXPECT_EQ( segments[0].FirstVertex, 3u );
    EXPECT_EQ( segments[1].SubmeshIndex, 3u );
    EXPECT_EQ( segments[1].IndexByteOffset, 9u * 4u );
    EXPECT_EQ( segments[60].SubmeshIndex, 62u );
    EXPECT_EQ( segments[61].SubmeshIndex, 64u );
    EXPECT_EQ( segments[62].SubmeshIndex, 65u );

    const auto all = DescribeStaticMeshGeometry( submeshes, 0 );
    ASSERT_EQ( all.size(), 66u );
    for ( std::size_t i = 0; i < all.size(); ++i )
        EXPECT_EQ( all[i].SubmeshIndex, i );
}

// Red if a deformed BLAS reads the skin pass's output at the mesh's vertex stride (positions would be read 56
// bytes apart from a packed float3 stream), if it ignores the instance's hidden mask, or if its index and vertex
// ranges stop being the mesh's own (the skin pass keeps vertex order, so the mesh's index buffer addresses it).
TEST( RayTracingGeometry, ADeformedMeshReadsPackedPositionsAndHonoursTheInstanceMask )
{
    const std::vector<Submesh> submeshes{ MakeSubmesh( 0, 6, 0, 9 ), MakeSubmesh( 6, 4, 9, 6 ) };

    const auto deformed = DescribeDeformedMeshGeometry( submeshes, 0b01 );
    ASSERT_EQ( deformed.size(), 1u );
    EXPECT_EQ( deformed[0].VertexStride, 12u );
    EXPECT_EQ( deformed[0].PositionOffset, 0u );
    EXPECT_EQ( deformed[0].SubmeshIndex, 1u );
    EXPECT_EQ( deformed[0].FirstVertex, 6u );
    EXPECT_EQ( deformed[0].MaxVertex, 9u );
    EXPECT_EQ( deformed[0].IndexByteOffset, 9u * 4u );
    EXPECT_EQ( deformed[0].PrimitiveCount, 2u );

    const auto rest = DescribeStaticMeshGeometry( submeshes, 0b01 );
    ASSERT_EQ( rest.size(), 1u );
    EXPECT_EQ( rest[0].VertexStride, sizeof( Vertex ) );
    EXPECT_EQ( rest[0].FirstVertex, deformed[0].FirstVertex );
    EXPECT_EQ( rest[0].IndexByteOffset, deformed[0].IndexByteOffset );
}

// Red if the BLAS or the TLAS lands under a kind that is not its own (or under the same one), if the bytes are not
// recorded on the row, or if dropping the token leaves the row in the ledger (a released BLAS still counted).
TEST( RayTracingGeometry, AnAccelerationStructureIsALedgerRowOfItsOwnKindUntilItsTokenIsReleased )
{
    using Desert::Graphic::ResourceCensus;
    using Desert::Graphic::ResourceKind;
    using Desert::Graphic::ResourceLedger;
    using Desert::Graphic::ResourceOwner;
    using Desert::Graphic::ResourceOwnership;

    const auto count = []( const ResourceCensus& census, ResourceKind kind )
    { return census.PerKind[static_cast<std::size_t>( kind )]; };
    const auto bytes = []( const ResourceCensus& census, ResourceKind kind )
    {
        uint64_t sum = 0;
        for ( std::size_t owner = 0; owner < static_cast<std::size_t>( ResourceOwner::Count ); ++owner )
            sum += census.BytesPerOwnerKind[owner][static_cast<std::size_t>( kind )];
        return sum;
    };

    const ResourceCensus before = ResourceLedger::Take();
    {
        ResourceOwnership blas = RecordAccelerationStructureMemory( AccelerationStructureLevel::Bottom, 65'536 );
        ResourceOwnership tlas = RecordAccelerationStructureMemory( AccelerationStructureLevel::Top, 4'096 );
        ASSERT_TRUE( blas.IsAccounted() );
        ASSERT_TRUE( tlas.IsAccounted() );

        const ResourceCensus during = ResourceLedger::Take();
        EXPECT_EQ( count( during, ResourceKind::BottomLevelAccelerationStructure ) -
                        count( before, ResourceKind::BottomLevelAccelerationStructure ),
                   1u );
        EXPECT_EQ( count( during, ResourceKind::TopLevelAccelerationStructure ) -
                        count( before, ResourceKind::TopLevelAccelerationStructure ),
                   1u );
        EXPECT_EQ( bytes( during, ResourceKind::BottomLevelAccelerationStructure ) -
                        bytes( before, ResourceKind::BottomLevelAccelerationStructure ),
                   65'536u );
        EXPECT_EQ( bytes( during, ResourceKind::TopLevelAccelerationStructure ) -
                        bytes( before, ResourceKind::TopLevelAccelerationStructure ),
                   4'096u );

        // The TLAS's Release resets its token (a reallocation for a larger scene): the row goes, the BLAS stays.
        tlas                          = ResourceOwnership{};
        const ResourceCensus released = ResourceLedger::Take();
        EXPECT_EQ( count( released, ResourceKind::TopLevelAccelerationStructure ),
                   count( before, ResourceKind::TopLevelAccelerationStructure ) );
        EXPECT_EQ( count( released, ResourceKind::BottomLevelAccelerationStructure ) -
                        count( before, ResourceKind::BottomLevelAccelerationStructure ),
                   1u );
    }
    const ResourceCensus after = ResourceLedger::Take();
    EXPECT_EQ( count( after, ResourceKind::BottomLevelAccelerationStructure ),
               count( before, ResourceKind::BottomLevelAccelerationStructure ) );
    EXPECT_EQ( bytes( after, ResourceKind::BottomLevelAccelerationStructure ),
               bytes( before, ResourceKind::BottomLevelAccelerationStructure ) );
}
