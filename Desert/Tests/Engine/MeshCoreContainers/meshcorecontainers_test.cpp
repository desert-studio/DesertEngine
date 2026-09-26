// The containers DynamicMesh3 stands on, exercised the way DynamicMesh3 uses them:
// DynamicVector holds per-element attributes addressed by id, RefCountVector hands out
// vertex/triangle/edge ids with holes and reuses them, SmallListSet stores vertex->edge adjacency.
#include <gtest/gtest.h>

#include <Engine/Geometry/MeshCore/RefCountVector.hpp>
#include <Engine/Geometry/MeshCore/SmallListSet.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Desert::Geometry;

namespace
{
    std::vector<int> Live( const RefCountVector& V )
    {
        std::vector<int> Out;
        for ( int Id : V.Indices() )
            Out.push_back( Id );
        return Out;
    }

    std::vector<int> ListOf( const SmallListSet& Set, int32_t ListIndex )
    {
        std::vector<int> Out;
        for ( int Value : Set.Values( ListIndex ) )
            Out.push_back( Value );
        std::sort( Out.begin(), Out.end() );
        return Out;
    }

    std::vector<int> Range( int From, int To )
    {
        std::vector<int> Out;
        for ( int i = From; i < To; ++i )
            Out.push_back( i );
        return Out;
    }
} // namespace

// ---------------------------------------------------------------- DynamicVector

TEST( MeshCoreDynamicVector, AddCrossesBlockBoundariesAndKeepsValues )
{
    DynamicVector<int, 4> V;
    for ( int i = 0; i < 11; ++i )
        V.Add( i * 10 );
    ASSERT_EQ( V.Num(), 11u );
    for ( uint32_t i = 0; i < 11; ++i )
        EXPECT_EQ( V[i], int( i ) * 10 );
    EXPECT_EQ( V.Back(), 100 );
    EXPECT_EQ( V.GetByteCount(), 3u * 4u * sizeof( int ) ); // three blocks of four
}

TEST( MeshCoreDynamicVector, PopBackAcrossABlockBoundaryThenAddAgain )
{
    DynamicVector<int, 4> V;
    for ( int i = 0; i < 5; ++i )
        V.Add( i );
    V.PopBack(); // 5 -> 4: the last block becomes empty, Back() must be the old index 3
    ASSERT_EQ( V.Num(), 4u );
    EXPECT_EQ( V.Back(), 3 );
    V.PopBack();
    EXPECT_EQ( V.Num(), 3u );
    EXPECT_EQ( V.Back(), 2 );
    V.Add( 77 );
    EXPECT_EQ( V.Num(), 4u );
    EXPECT_EQ( V[3], 77u );
}

TEST( MeshCoreDynamicVector, InsertAtPastTheEndGrowsAndInitialisesTheGap )
{
    // DynamicMesh3::AppendVertex/InsertVertex write attributes at an id that may be past the end.
    DynamicVector<double, 4> V;
    V.Add( 1.0 );
    V.InsertAt( 9.0, 6, -1.0 );
    ASSERT_EQ( V.Num(), 7u );
    EXPECT_EQ( V[0], 1.0 );
    for ( uint32_t i = 1; i < 6; ++i )
        EXPECT_EQ( V[i], -1.0 ) << i;
    EXPECT_EQ( V[6], 9.0 );
    V.InsertAt( 5.0, 2 ); // inside: overwrite, no growth
    EXPECT_EQ( V.Num(), 7u );
    EXPECT_EQ( V[2], 5.0 );
}

TEST( MeshCoreDynamicVector, ResizeShrinkGrowFillAndSetMinimumSize )
{
    DynamicVector<int, 4> V;
    V.Resize( 9, 3 );
    EXPECT_EQ( V.Num(), 9u );
    V.Resize( 2 );
    EXPECT_EQ( V.Num(), 2u );
    EXPECT_EQ( V.Back(), 3 );
    EXPECT_FALSE( V.SetMinimumSize( 1, 0 ) );
    EXPECT_TRUE( V.SetMinimumSize( 6, 8 ) );
    EXPECT_EQ( V.Num(), 6u );
    EXPECT_EQ( V[5], 8 );
    V.Fill( 4 );
    for ( int Value : V )
        EXPECT_EQ( Value, 4 );
    V.Clear();
    EXPECT_TRUE( V.IsEmpty() );
}

TEST( MeshCoreDynamicVector, CopyMoveAndEqualityAcrossBlockSizes )
{
    DynamicVector<int, 4> A;
    for ( int i = 0; i < 9; ++i )
        A.Add( i );
    DynamicVector<int, 4> Copy( A );
    EXPECT_TRUE( Copy == A );
    Copy[8] = -1;
    EXPECT_TRUE( Copy != A );
    DynamicVector<int, 8> Other;
    for ( int i = 0; i < 9; ++i )
        Other.Add( i );
    EXPECT_TRUE( A == Other );
    DynamicVector<int, 4> const Moved( std::move( A ) );
    EXPECT_EQ( Moved.Num(), 9u );
    // The moved-from state is the contract under test here, so reading A after the move is the point.
    EXPECT_TRUE( A.IsEmpty() ); // NOLINT(bugprone-use-after-move)
    A.Add( 1 );                 // moved-from must still be usable
    EXPECT_EQ( A.Num(), 1u );
}

// A move hands the blocks over and allocates nothing: the destination keeps the very storage the source had, and
// the source is left holding no block at all (GetByteCount 0), yet every mutator still works on it.
TEST( MeshCoreDynamicVector, MoveStealsTheBlocksAndAllocatesNothing )
{
    DynamicVector<int, 4> A;
    for ( int i = 0; i < 6; ++i )
        A.Add( i );
    const int* const FirstElement = &A[0];
    const int* const LastElement  = &A[5];

    DynamicVector<int, 4> Moved( std::move( A ) );
    EXPECT_EQ( &Moved[0], FirstElement );
    EXPECT_EQ( &Moved[5], LastElement );
    // Reading the source after the move is the contract under test.
    EXPECT_TRUE( A.IsEmpty() );        // NOLINT(bugprone-use-after-move)
    EXPECT_EQ( A.GetByteCount(), 0u ); // no replacement block was allocated
    A.Apply( []( int& ) { FAIL() << "a moved-from vector has no element to visit"; } );

    DynamicVector<int, 4> Assigned;
    Assigned.Add( 42 );
    Assigned = std::move( Moved );
    EXPECT_EQ( &Assigned[0], FirstElement );
    EXPECT_EQ( Assigned[5], 5 );
    EXPECT_EQ( Moved.GetByteCount(), 0u ); // NOLINT(bugprone-use-after-move)

    // Both blockless vectors are still fully usable.
    A.Add( 7 );
    ASSERT_EQ( A.Num(), 1u );
    EXPECT_EQ( A[0], 7 );
    Moved.Resize( 5, 3 );
    EXPECT_EQ( Moved.Num(), 5u );
    EXPECT_EQ( Moved[4], 3 );
    DynamicVector<int, 4> Drained( std::move( Assigned ) );
    Assigned.Clear(); // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE( Assigned.IsEmpty() );
    const DynamicVector<int, 4> Final( std::move( Drained ) );
    EXPECT_TRUE( Final != Assigned );
    EXPECT_EQ( Final[0], 0 );
}

// Iterator equality compares the vector and the position; a range-for only needs !=, so == was never instantiated.
TEST( MeshCoreDynamicVector, IteratorEqualityComparesVectorAndPosition )
{
    DynamicVector<int, 4> A;
    DynamicVector<int, 4> B;
    for ( int i = 0; i < 3; ++i )
    {
        A.Add( i );
        B.Add( i );
    }
    EXPECT_TRUE( A.begin() == A.begin() );
    EXPECT_FALSE( A.begin() == A.end() );
    EXPECT_FALSE( A.begin() == B.begin() );
    auto It = A.begin();
    ++It;
    ++It;
    ++It;
    EXPECT_TRUE( It == A.end() );

    const DynamicVector<int, 4>& ConstA = A;
    EXPECT_TRUE( ConstA.begin() == ConstA.begin() );
    EXPECT_FALSE( ConstA.begin() == ConstA.end() );
    EXPECT_TRUE( ConstA.begin() != ConstA.end() );
    int Sum = 0;
    for ( const int Value : ConstA )
        Sum += Value;
    EXPECT_EQ( Sum, 3 );
}

TEST( MeshCoreDynamicVector, VectorNStoresTuplesPerId )
{
    // DynamicMesh3 stores triangles as DynamicVector<Index3i>; the N-wide variant carries raw tuples.
    DynamicVector3i Tris;
    Tris.Add( { { 0, 1, 2 } } );
    Tris.Add( { { 2, 1, 3 } } );
    EXPECT_EQ( Tris.GetLength(), 2u );
    EXPECT_EQ( Tris.AsIndex3( 1 ), Index3i( 2, 1, 3 ) );
    Tris.PopBack();
    EXPECT_EQ( Tris.GetLength(), 1u );
    EXPECT_EQ( Tris.AsIndex3( 0 ), Index3i( 0, 1, 2 ) );
}

// ---------------------------------------------------------------- RefCountVector

TEST( MeshCoreRefCountVector, FreedIdsLeaveHolesThatIterationSkips )
{
    RefCountVector V;
    for ( int i = 0; i < 6; ++i )
        EXPECT_EQ( V.Allocate(), i );
    V.Decrement( 1 );
    V.Decrement( 4 );
    EXPECT_EQ( V.GetCount(), 4u );
    EXPECT_EQ( V.GetMaxIndex(), 6u );
    EXPECT_FALSE( V.IsDense() );
    EXPECT_FALSE( V.IsValid( 1 ) );
    EXPECT_FALSE( V.IsValid( 4 ) );
    EXPECT_FALSE( V.IsValid( 6 ) );
    EXPECT_FALSE( V.IsValid( -1 ) );
    EXPECT_EQ( Live( V ), ( std::vector<int>{ 0, 2, 3, 5 } ) );
}

TEST( MeshCoreRefCountVector, AllocateReusesTheMostRecentlyFreedIdFirst )
{
    RefCountVector V;
    for ( int i = 0; i < 5; ++i )
        V.Allocate();
    V.Decrement( 1 );
    V.Decrement( 3 );
    EXPECT_EQ( V.Allocate(), 3 ); // free list is a stack
    EXPECT_EQ( V.Allocate(), 1 );
    EXPECT_TRUE( V.IsDense() );
    EXPECT_EQ( V.Allocate(), 5 ); // no holes left: append
    EXPECT_EQ( Live( V ), Range( 0, 6 ) );
}

TEST( MeshCoreRefCountVector, ReferenceCountingKeepsAnIdAliveUntilTheLastRelease )
{
    // A vertex id is referenced once by itself and once per incident triangle.
    RefCountVector V;
    const int      Vid = V.Allocate();
    V.Increment( Vid );
    V.Increment( Vid, 2 );
    EXPECT_EQ( V.GetRefCount( Vid ), 4 );
    V.Decrement( Vid, 3 );
    EXPECT_TRUE( V.IsValid( Vid ) );
    EXPECT_EQ( V.GetRefCount( Vid ), 1 );
    V.Decrement( Vid );
    EXPECT_FALSE( V.IsValid( Vid ) );
    EXPECT_EQ( V.GetRefCount( Vid ), 0 );
    EXPECT_EQ( V.GetRawRefCount( Vid ), RefCountVector::INVALID_REF_COUNT );
}

TEST( MeshCoreRefCountVector, AllocateAtPastTheEndPutsTheGapOnTheFreeList )
{
    RefCountVector V;
    V.Allocate();                     // 0
    EXPECT_TRUE( V.AllocateAt( 4 ) ); // 1..3 become free
    EXPECT_EQ( V.GetCount(), 2u );
    EXPECT_EQ( Live( V ), ( std::vector<int>{ 0, 4 } ) );
    EXPECT_FALSE( V.AllocateAt( 4 ) ); // already live
    EXPECT_TRUE( V.AllocateAt( 2 ) );  // taken out of the free list: the back (3) fills its slot
    EXPECT_EQ( V.Allocate(), 3 );      // so the remaining free ids come back as 3, then 1
    EXPECT_EQ( V.Allocate(), 1 );
    EXPECT_EQ( V.Allocate(), 5 );
}

TEST( MeshCoreRefCountVector, AllocateAtUnsafeThenRebuildFreeList )
{
    RefCountVector V;
    EXPECT_TRUE( V.AllocateAtUnsafe( 3 ) );
    EXPECT_TRUE( V.AllocateAtUnsafe( 0 ) );
    V.RebuildFreeList();
    EXPECT_EQ( V.GetCount(), 2u );
    EXPECT_EQ( Live( V ), ( std::vector<int>{ 0, 3 } ) );
    EXPECT_EQ( V.Allocate(), 2 );
    EXPECT_EQ( V.Allocate(), 1 );
}

TEST( MeshCoreRefCountVector, RebuildCountsReferencesFromTriangles )
{
    // DynamicMesh3 rebuilds vertex ref counts by walking the triangles.
    const std::vector<Index3i> Triangles = { { 0, 1, 2 }, { 0, 2, 4 } };
    RefCountVector             V;
    V.Rebuild(
         6,
         [&]( auto&& Update )
         {
             for ( const Index3i& T : Triangles )
                 for ( int j = 0; j < 3; ++j )
                     Update( T[j] );
         },
         []( unsigned short& RefCount ) { RefCount = 2; }, []( unsigned short& RefCount ) { ++RefCount; } );
    EXPECT_EQ( V.GetCount(), 4u );
    EXPECT_EQ( Live( V ), ( std::vector<int>{ 0, 1, 2, 4 } ) );
    EXPECT_EQ( V.GetRefCount( 0 ), 3 );
    EXPECT_EQ( V.GetRefCount( 1 ), 2 );
    EXPECT_EQ( V.GetRefCount( 2 ), 3 );
    const int First  = V.Allocate();
    const int Second = V.Allocate();
    EXPECT_EQ( std::min( First, Second ), 3 );
    EXPECT_EQ( std::max( First, Second ), 5 );
}

TEST( MeshCoreRefCountVector, AppendOffsetsTheOtherFreeList )
{
    RefCountVector A;
    A.Allocate();
    A.Allocate();
    RefCountVector B;
    B.Allocate();
    B.Allocate();
    B.Allocate();
    B.Decrement( 1 );
    A.Append( B );
    EXPECT_EQ( A.GetCount(), 4u );
    EXPECT_EQ( Live( A ), ( std::vector<int>{ 0, 1, 2, 4 } ) );
    EXPECT_EQ( A.Allocate(), 3 ); // B's hole at 1 is A's hole at 3
}

TEST( MeshCoreRefCountVector, FilteredAndMappedIndicesAndEquality )
{
    RefCountVector V;
    for ( int i = 0; i < 6; ++i )
        V.Allocate();
    V.Decrement( 2 );
    std::vector<int> Even;
    for ( int Id : V.FilteredIndices( []( int Id ) { return Id % 2 == 0; } ) )
        Even.push_back( Id );
    EXPECT_EQ( Even, ( std::vector<int>{ 0, 4 } ) );
    std::vector<double> Mapped;
    for ( double X : V.MappedIndices<double>( []( int Id ) { return Id * 0.5; } ) )
        Mapped.push_back( X );
    EXPECT_EQ( Mapped, ( std::vector<double>{ 0.0, 0.5, 1.5, 2.0, 2.5 } ) );

    RefCountVector Copy( V );
    EXPECT_TRUE( Copy == V );
    Copy.Increment( 0 );
    EXPECT_TRUE( Copy != V );
}

// ---------------------------------------------------------------- SmallListSet

TEST( MeshCoreSmallListSet, AdjacencyListsSpillPastTheBlockAndStayASet )
{
    // A vertex with more edges than BLOCKSIZE (8) spills into the linked list.
    SmallListSet Set;
    Set.Resize( 3 );
    Set.AllocateAt( 0 );
    Set.AllocateAt( 2 );
    for ( int e = 0; e < 13; ++e )
        Set.Insert( 0, 100 + e );
    Set.Insert( 2, 7 );
    EXPECT_EQ( Set.GetCount( 0 ), 13 );
    EXPECT_EQ( Set.GetCount( 1 ), 0 );
    EXPECT_FALSE( Set.IsAllocated( 1 ) );
    EXPECT_EQ( ListOf( Set, 0 ), Range( 100, 113 ) );
    EXPECT_TRUE( Set.Contains( 0, 112 ) );
    EXPECT_TRUE( Set.Contains( 0, 100 ) );
    EXPECT_FALSE( Set.Contains( 0, 113 ) );
    EXPECT_EQ( ListOf( Set, 2 ), ( std::vector<int>{ 7 } ) );
}

TEST( MeshCoreSmallListSet, RemoveFromTheBlockPullsASpilledElementIn )
{
    SmallListSet Set;
    Set.Resize( 1 );
    for ( int e = 0; e < 11; ++e )
        Set.Insert( 0, e );
    EXPECT_TRUE( Set.Remove( 0, 3 ) );  // in the block
    EXPECT_TRUE( Set.Remove( 0, 10 ) ); // in the spill
    EXPECT_FALSE( Set.Remove( 0, 3 ) );
    EXPECT_EQ( Set.GetCount( 0 ), 9 );
    EXPECT_EQ( ListOf( Set, 0 ), ( std::vector<int>{ 0, 1, 2, 4, 5, 6, 7, 8, 9 } ) );
    for ( int e : { 0, 1, 2, 4, 5, 6, 7, 8 } )
        EXPECT_TRUE( Set.Remove( 0, e ) ) << e;
    EXPECT_EQ( ListOf( Set, 0 ), ( std::vector<int>{ 9 } ) );
    EXPECT_EQ( Set.First( 0 ), 9 );
}

TEST( MeshCoreSmallListSet, ClearedListsReturnTheirStorageForReuse )
{
    SmallListSet Set;
    Set.Resize( 2 );
    for ( int e = 0; e < 12; ++e )
        Set.Insert( 0, e );
    const size_t Before = Set.GetByteCount();
    Set.Clear( 0 );
    EXPECT_FALSE( Set.IsAllocated( 0 ) );
    EXPECT_EQ( Set.GetCount( 0 ), 0 );
    for ( int e = 0; e < 12; ++e )
        Set.Insert( 1, 50 + e ); // reuses the freed block and spill nodes
    EXPECT_EQ( Set.GetByteCount(), Before );
    EXPECT_EQ( ListOf( Set, 1 ), Range( 50, 62 ) );
}

TEST( MeshCoreSmallListSet, FindReplaceMoveAndEarlyOut )
{
    SmallListSet Set;
    Set.Resize( 3 );
    for ( int e = 0; e < 10; ++e )
        Set.Insert( 0, e * 2 );
    // the spill is pushed at its head, so the newest spilled value (18) is found before 16
    EXPECT_EQ( Set.Find( 0, []( int32_t v ) { return v > 15; } ), 18 );
    EXPECT_EQ( Set.Find( 0, []( int32_t v ) { return v > 100; }, -7 ), -7 );
    EXPECT_TRUE( Set.Replace( 0, []( int32_t v ) { return v == 18; }, 99 ) ); // lives in the spill
    EXPECT_TRUE( Set.Contains( 0, 99 ) );
    EXPECT_FALSE( Set.Contains( 0, 18 ) );
    int Visited = 0;
    EXPECT_FALSE( Set.EnumerateEarlyOut( 0, [&]( int32_t ) { return ++Visited < 3; } ) );
    EXPECT_EQ( Visited, 3 );
    Set.Move( 0, 2 );
    EXPECT_FALSE( Set.IsAllocated( 0 ) );
    EXPECT_EQ( Set.GetCount( 2 ), 10 );
    std::vector<int> Mapped;
    for ( int const v : Set.MappedValues( 2, []( int32_t v ) { return -v; } ) )
        Mapped.push_back( v );
    EXPECT_EQ( Mapped.size(), 10u );
}

TEST( MeshCoreSmallListSet, CompactAndAppendPreserveEveryList )
{
    SmallListSet Set;
    Set.Resize( 4 );
    for ( int e = 0; e < 10; ++e )
        Set.Insert( 0, e );
    Set.Insert( 1, 42 );
    for ( int e = 0; e < 3; ++e )
        Set.Insert( 3, 200 + e );
    Set.Clear( 1 );
    SmallListSet Compacted( Set );
    Compacted.Compact( 4 );
    EXPECT_TRUE( Compacted == Set );
    EXPECT_LT( Compacted.GetByteCount(), Set.GetByteCount() + 1 );

    SmallListSet Joined( Set );
    Joined.AppendWithElementOffset( Set, 1000 );
    EXPECT_EQ( Joined.Size(), 8u );
    EXPECT_EQ( ListOf( Joined, 4 ), Range( 1000, 1010 ) );
    EXPECT_EQ( ListOf( Joined, 7 ), ( std::vector<int>{ 1200, 1201, 1202 } ) );
    EXPECT_EQ( ListOf( Joined, 0 ), Range( 0, 10 ) );
    Joined.Insert( 5, 1 ); // appended free block must be usable
    EXPECT_EQ( ListOf( Joined, 5 ), ( std::vector<int>{ 1 } ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
