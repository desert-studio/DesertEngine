// VFX-07: the world particle pool's device-free bookkeeping - the emitter ranges (ParticlePoolRanges, what
// ParticleRenderer allocates) and the free / alive list contract the ParticleCompact and ParticleSimulate passes
// keep (ParticlePoolLists). Red when a range overlaps another, a released range is not reused or coalesced, a
// compact loses or duplicates an index, or a spawn takes an index that is alive.
#include <Engine/Graphic/Systems/Scene/Particles/ParticlePool.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

using Desert::Graphic::System::ParticlePoolLists;
using Desert::Graphic::System::ParticlePoolRange;
using Desert::Graphic::System::ParticlePoolRanges;

TEST( ParticlePool, RangesAreDisjointReusedAndCoalesced )
{
    ParticlePoolRanges      ranges;
    const ParticlePoolRange a = ranges.Acquire( 1, 100 );
    const ParticlePoolRange b = ranges.Acquire( 2, 50 );
    const ParticlePoolRange c = ranges.Acquire( 3, 10 );
    EXPECT_EQ( a.Base, 0u );
    EXPECT_EQ( b.Base, 100u );
    EXPECT_EQ( c.Base, 150u );
    EXPECT_EQ( ranges.End(), 160u );
    EXPECT_EQ( ranges.Acquire( 2, 50 ).Base, 100u ) << "the same request moved the range";

    ranges.Release( 1 );
    ranges.Release( 2 );
    // Coalesced: 150 particles fit where the two released ranges were, without growing the pool.
    EXPECT_EQ( ranges.Acquire( 4, 150 ).Base, 0u );
    EXPECT_EQ( ranges.End(), 160u );
    // A count change reallocates; nothing free fits 20, so it goes to the end.
    EXPECT_EQ( ranges.Acquire( 3, 20 ).Base, 160u );
    ranges.ReleaseUnless( []( const uint32_t key ) { return key == 3; } );
    EXPECT_EQ( ranges.RangeCount(), 1u );
}

TEST( ParticlePool, SpawnKillCompactKeepsCountsAndNeverReusesAnAliveIndex )
{
    const ParticlePoolRange range{ 32, 16 };
    std::vector<bool>       alive( 64, false );
    ParticlePoolLists       lists( range );
    lists.Compact( alive );
    EXPECT_EQ( lists.Alive().size(), 0u );
    EXPECT_EQ( lists.Free().size(), 16u );

    // Spawn N = 10.
    for ( const uint32_t index : lists.SpawnIndices( 10 ) )
    {
        EXPECT_GE( index, range.Base );
        EXPECT_LT( index, range.Base + range.Count );
        EXPECT_FALSE( alive[index] ) << "spawned over a live particle";
        alive[index] = true;
    }
    lists.Compact( alive );
    ASSERT_EQ( lists.Alive().size(), 10u );
    EXPECT_EQ( lists.Free().size(), 6u );

    // Kill M = 3.
    const std::vector<uint32_t> killed( lists.Alive().begin(), lists.Alive().begin() + 3 );
    for ( const uint32_t index : killed )
        alive[index] = false;
    lists.Compact( alive );
    EXPECT_EQ( lists.Alive().size(), 7u );
    EXPECT_EQ( lists.Free().size(), 9u );

    // Every index of the range is in exactly one list.
    std::set<uint32_t> all( lists.Alive().begin(), lists.Alive().end() );
    all.insert( lists.Free().begin(), lists.Free().end() );
    EXPECT_EQ( all.size(), 16u );

    // A budget larger than the free list spawns only the free list, each index once, none alive.
    const std::vector<uint32_t> spawned = lists.SpawnIndices( 100 );
    EXPECT_EQ( spawned.size(), 9u );
    EXPECT_EQ( std::set<uint32_t>( spawned.begin(), spawned.end() ).size(), spawned.size() );
    for ( const uint32_t index : spawned )
        EXPECT_FALSE( alive[index] ) << index << " was alive";
}
