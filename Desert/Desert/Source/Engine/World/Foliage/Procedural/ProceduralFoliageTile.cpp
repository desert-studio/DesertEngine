#include <Engine/World/Foliage/Procedural/ProceduralFoliageSpawner.hpp>
#include <Engine/World/Foliage/Procedural/ProceduralFoliageTile.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace Desert::World::Foliage::Procedural
{
    namespace
    {
        constexpr float kSmallNumber = 1.e-8f;
        constexpr float kTwoPi       = 2.0f * std::numbers::pi_v<float>;

        bool ByLocation( const ProceduralFoliageInstance& a, const ProceduralFoliageInstance& b )
        {
            return a.Location.x == b.Location.x ? a.Location.y < b.Location.y : a.Location.x < b.Location.x;
        }
    } // namespace

    const Assets::Serialization::FoliageProcedural& ProceduralFoliageTile::TypeOf( uint32_t typeIndex ) const
    {
        return m_Spawner->Types()[typeIndex].Procedural;
    }

    InstanceRadii ProceduralFoliageTile::RadiiOfId( uint32_t id ) const
    {
        const auto& instance = m_Storage[id];
        return RadiiOf( instance, TypeOf( instance.TypeIndex ) );
    }

    void ProceduralFoliageTile::InitSimulation( const ProceduralFoliageSpawner& spawner, int32_t randomSeed )
    {
        Empty();
        m_Stream.Initialize( randomSeed );
        m_Spawner        = &spawner;
        m_SimulationStep = 0;
        m_Broadphase =
             ProceduralFoliageBroadphase( spawner.Settings().TileSize, spawner.Settings().MinimumQuadTreeSize );
        m_RandomSeed = randomSeed;
    }

    void ProceduralFoliageTile::Simulate( const ProceduralFoliageSpawner& spawner, int32_t randomSeed,
                                          int32_t maxNumSteps )
    {
        InitSimulation( spawner, randomSeed );
        RunSimulation( maxNumSteps, false );
        RunSimulation( maxNumSteps, true );
    }

    void ProceduralFoliageTile::Empty()
    {
        m_Broadphase.Empty();
        m_Storage.clear();
        m_Living.clear();
        m_PendingRemovals.clear();
        m_Array.clear();
    }

    void ProceduralFoliageTile::RunSimulation( int32_t maxNumSteps, bool onlyInShade )
    {
        int32_t maxSteps = 0;
        for ( const auto& type : m_Spawner->Types() )
            if ( SpawnsInShade( type.Procedural ) == onlyInShade )
                maxSteps = std::max( maxSteps, type.Procedural.NumSteps + 1 );
        if ( maxNumSteps >= 0 )
            maxSteps = std::min( maxSteps, maxNumSteps );

        m_SimulationStep = 0;
        m_OnlyInShade    = onlyInShade;
        for ( int32_t step = 0; step < maxSteps; ++step )
        {
            StepSimulation();
            ++m_SimulationStep;
        }
        InstancesToArray();
    }

    void ProceduralFoliageTile::StepSimulation()
    {
        std::vector<uint32_t> newInstances;
        if ( m_SimulationStep == 0 )
            AddRandomSeeds( newInstances );
        else
        {
            AgeSeeds();
            SpreadSeeds( newInstances );
        }
        m_Living.insert( m_Living.end(), newInstances.begin(), newInstances.end() );
        FlushPendingRemovals();
    }

    void ProceduralFoliageTile::AddRandomSeeds( std::vector<uint32_t>& out )
    {
        const auto& settings  = m_Spawner->Settings();
        const float sizeTenM2 = ( settings.TileSize * settings.TileSize ) / ( 1000.0f * 1000.0f );
        const auto  types     = m_Spawner->Types();

        // Per DistributionSeed, the largest grown shade radius: types sharing a seed share initial positions.
        std::map<int32_t, float>  maxShadeRadii;
        std::vector<int32_t>      seedsLeft( types.size(), 0 );
        std::vector<RandomStream> streamPerType( types.size() );
        std::vector<uint32_t>     typesToSeed;
        for ( uint32_t i = 0; i < types.size(); ++i )
        {
            const auto& type = types[i].Procedural;
            if ( SpawnsInShade( type ) != m_OnlyInShade )
                continue;
            const auto numSeeds = static_cast<int32_t>(
                 std::lround( type.InitialSeedDensity * type.InitialSeedDensity * sizeTenM2 ) );
            seedsLeft[i] = numSeeds;
            if ( numSeeds > 0 )
                typesToSeed.push_back( i );
            streamPerType[i].Initialize( type.DistributionSeed + settings.RandomSeed + m_RandomSeed );
            const float maxShade = ScaleForAge( type, type.MaxAge ) * type.ShadeRadius;
            const auto  found    = maxShadeRadii.find( type.DistributionSeed );
            if ( found == maxShadeRadii.end() )
                maxShadeRadii.emplace( type.DistributionSeed, maxShade );
            else
                found->second = std::max( found->second, maxShade );
        }

        const auto numTypes        = static_cast<int32_t>( typesToSeed.size() );
        int32_t    typesLeft       = numTypes;
        int32_t    typeCursor      = -1;
        const auto lastShadeCaster = static_cast<int32_t>( m_Array.size() ) - 1;
        while ( typesLeft > 0 )
        {
            // Cycle through the types so each gets a fair chance at the room.
            typeCursor               = ( typeCursor + 1 ) % numTypes;
            const uint32_t typeIndex = typesToSeed[typeCursor];
            int32_t&       left      = seedsLeft[typeIndex];
            if ( left == 0 )
                continue;
            const auto&   type       = types[typeIndex].Procedural;
            const float   age        = InitAge( type, m_Stream );
            const float   scale      = ScaleForAge( type, age );
            RandomStream& typeStream = streamPerType[typeIndex];
            glm::vec2     init{ 0.0f };
            float         neededRadius = 0.0f;
            if ( m_OnlyInShade && lastShadeCaster >= 0 )
            {
                // Shade growers start beside a plant of the first pass.
                const auto& caster     = m_Array[typeStream.RandRange( 0, lastShadeCaster )];
                const auto& casterType = TypeOf( caster.TypeIndex );
                init                   = caster.Location;
                neededRadius =
                     casterType.CollisionRadius * caster.Scale * ( scale + ScaleForAge( casterType, caster.Age ) );
            }
            else
            {
                init.x       = typeStream.FRandRange( 0.0f, settings.TileSize );
                init.y       = typeStream.FRandRange( 0.0f, settings.TileSize );
                neededRadius = maxShadeRadii[type.DistributionSeed];
            }
            const float     angle    = m_Stream.FRandRange( 0.0f, kTwoPi );
            const float     distance = m_Stream.FRandRange( 0.0f, type.MaxInitialSeedOffset ) + neededRadius;
            const glm::vec2 location = init + distance * glm::vec2( std::cos( angle ), std::sin( angle ) );
            if ( const int64_t id = NewSeed( location, scale, typeIndex, age, false ); id >= 0 )
                out.push_back( static_cast<uint32_t>( id ) );
            if ( --left == 0 )
                --typesLeft;
        }
    }

    void ProceduralFoliageTile::AgeSeeds()
    {
        std::vector<uint32_t>       aged;
        const std::vector<uint32_t> living = m_Living;
        for ( const uint32_t id : living )
        {
            if ( !m_Storage[id].Alive )
                continue;
            const uint32_t typeIndex = m_Storage[id].TypeIndex;
            const auto&    type      = TypeOf( typeIndex );
            if ( m_SimulationStep > type.NumSteps || SpawnsInShade( type ) != m_OnlyInShade )
                continue;
            const float     newAge   = NextAge( type, m_Storage[id].Age, 1 );
            const float     newScale = ScaleForAge( type, newAge );
            const glm::vec2 location = m_Storage[id].Location;
            // The grown plant replaces the younger one and competes again at its new size.
            MarkPendingRemoval( id );
            if ( const int64_t grown = NewSeed( location, newScale, typeIndex, newAge, false ); grown >= 0 )
                aged.push_back( static_cast<uint32_t>( grown ) );
        }
        m_Living.insert( m_Living.end(), aged.begin(), aged.end() );
        FlushPendingRemovals();
    }

    float ProceduralFoliageTile::SeedMinDistance( const ProceduralFoliageInstance& instance, float newAge ) const
    {
        const auto&   type      = TypeOf( instance.TypeIndex );
        const int32_t stepsLeft = static_cast<int32_t>( type.MaxAge - static_cast<float>( m_SimulationStep ) );
        const float   instanceMaxScale = ScaleForAge( type, NextAge( type, instance.Age, stepsLeft ) );
        const float   newMaxScale      = ScaleForAge( type, NextAge( type, newAge, stepsLeft ) );
        return ( instanceMaxScale + newMaxScale ) * MaxRadius( type );
    }

    float ProceduralFoliageTile::RandomGaussian()
    {
        // Box-Muller: mean 0, variance 1.
        const float rand1 = std::max( m_Stream.FRand(), kSmallNumber );
        const float rand2 = std::max( m_Stream.FRand(), kSmallNumber );
        return std::sqrt( -2.0f * std::log( rand1 ) ) * std::cos( rand2 * kTwoPi );
    }

    glm::vec2 ProceduralFoliageTile::SeedOffset( const Assets::Serialization::FoliageProcedural& type,
                                                 float                                           minDistance )
    {
        // 10 % of the seeds land at the extremes: a z score of +-1.64.
        constexpr float kMaxZScore = 1.64f;
        const float     z          = std::clamp( RandomGaussian(), -kMaxZScore, kMaxZScore );
        const float     variation  = z * type.SpreadVariance / kMaxZScore;
        const float     average    = minDistance + type.AverageSpreadDistance;
        const float     angle      = std::max( m_Stream.FRand(), kSmallNumber ) * kTwoPi;
        return glm::vec2( std::cos( angle ), std::sin( angle ) ) * ( average + variation );
    }

    void ProceduralFoliageTile::SpreadSeeds( std::vector<uint32_t>& out )
    {
        const std::vector<uint32_t> living = m_Living;
        for ( const uint32_t id : living )
        {
            if ( !m_Storage[id].Alive )
                continue;
            const uint32_t typeIndex = m_Storage[id].TypeIndex;
            const auto&    type      = TypeOf( typeIndex );
            if ( m_SimulationStep > type.NumSteps || SpawnsInShade( type ) != m_OnlyInShade )
                continue;
            for ( int32_t seed = 0; seed < type.SeedsPerStep; ++seed )
            {
                const float     newAge      = InitAge( type, m_Stream );
                const float     newScale    = ScaleForAge( type, newAge );
                const float     minDistance = SeedMinDistance( m_Storage[id], newAge );
                const glm::vec2 offset      = SeedOffset( type, minDistance );
                if ( offset.x * offset.x + offset.y * offset.y + kSmallNumber <= minDistance * minDistance )
                    continue;
                const glm::vec2 location = m_Storage[id].Location + offset;
                if ( const int64_t child = NewSeed( location, newScale, typeIndex, newAge, false ); child >= 0 )
                    out.push_back( static_cast<uint32_t>( child ) );
            }
        }
    }

    int64_t ProceduralFoliageTile::NewSeed( glm::vec2 location, float scale, uint32_t typeIndex, float age,
                                            bool blocker, const ProceduralFoliageInstance* rotationFrom )
    {
        const auto&               type = m_Spawner->Types()[typeIndex];
        ProceduralFoliageInstance instance;
        instance.Location  = location;
        instance.Age       = age;
        instance.Scale     = scale;
        instance.TypeIndex = typeIndex;
        instance.Blocker   = blocker;
        if ( rotationFrom )
        {
            instance.YawDegrees   = rotationFrom->YawDegrees;
            instance.PitchDegrees = rotationFrom->PitchDegrees;
        }
        else
        {
            // A stream of its own, so a change to one instance's draws does not move every later instance.
            RandomStream local = m_Stream;
            (void)m_Stream.GetUnsignedInt();
            instance.YawDegrees   = local.FRandRange( 0.0f, type.RandomYaw ? 360.0f : 0.0f );
            instance.PitchDegrees = local.FRandRange( 0.0f, type.RandomPitchAngle );
        }

        const InstanceRadii radii = RadiiOf( instance, type.Procedural );
        if ( !m_Broadphase.TestAgainstAABB( location, radii ) )
            return -1;
        const auto id = static_cast<uint32_t>( m_Storage.size() );
        m_Storage.push_back( instance );
        m_Broadphase.Insert( id, location, radii );
        return HandleOverlaps( id ) ? static_cast<int64_t>( id ) : -1;
    }

    bool ProceduralFoliageTile::HandleOverlaps( uint32_t id )
    {
        std::vector<BroadphaseOverlap> overlaps;
        m_Broadphase.GetOverlaps( id, m_Storage[id].Location, RadiiOfId( id ), overlaps );

        const auto dominatedBy = [&]( const BroadphaseOverlap& overlap )
        {
            const auto& a = m_Storage[id];
            const auto& b = m_Storage[overlap.Other];
            return Dominated( a, TypeOf( a.TypeIndex ), b, TypeOf( b.TypeIndex ), overlap.Kind );
        };
        for ( const auto& overlap : overlaps )
            if ( dominatedBy( overlap ) == &m_Storage[id] )
            {
                MarkPendingRemoval( id );
                return false;
            }
        // The newcomer survives every overlap: whoever it dominates dies.
        for ( const auto& overlap : overlaps )
            if ( dominatedBy( overlap ) != nullptr )
                MarkPendingRemoval( overlap.Other );
        return true;
    }

    void ProceduralFoliageTile::MarkPendingRemoval( uint32_t id )
    {
        if ( !m_Storage[id].Alive )
            return;
        m_Broadphase.Remove( id, m_Storage[id].Location, RadiiOfId( id ) );
        m_Storage[id].Alive = false;
        m_PendingRemovals.push_back( id );
    }

    void ProceduralFoliageTile::FlushPendingRemovals()
    {
        if ( m_PendingRemovals.empty() )
            return;
        std::erase_if( m_Living, [&]( uint32_t id ) { return !m_Storage[id].Alive; } );
        m_PendingRemovals.clear();
    }

    void ProceduralFoliageTile::InstancesToArray()
    {
        m_Array.clear();
        for ( const uint32_t id : m_Living )
            if ( !m_Storage[id].Blocker )
                m_Array.push_back( m_Storage[id] );
    }

    std::vector<ProceduralFoliageInstance> ProceduralFoliageTile::PlacedInstances() const
    {
        std::vector<ProceduralFoliageInstance> placed;
        for ( const uint32_t id : m_Living )
            if ( !m_Storage[id].Blocker )
                placed.push_back( m_Storage[id] );
        std::sort( placed.begin(), placed.end(), ByLocation );
        return placed;
    }

    std::vector<ProceduralFoliageInstance> ProceduralFoliageTile::LivingInstances() const
    {
        std::vector<ProceduralFoliageInstance> living;
        living.reserve( m_Living.size() );
        for ( const uint32_t id : m_Living )
            living.push_back( m_Storage[id] );
        return living;
    }

    void ProceduralFoliageTile::CopyInstancesToTile( ProceduralFoliageTile& toTile, const Box2& owned,
                                                     const Box2& blocking, glm::vec2 offset ) const
    {
        std::vector<ProceduralFoliageInstance> copied;
        for ( const uint32_t id : m_Living )
        {
            const auto&     instance = m_Storage[id];
            const glm::vec2 corner   = instance.Location - glm::vec2( RadiiOfId( id ).Max() );
            const bool      isOwned  = corner.x >= owned.Min.x && corner.x < owned.Max.x && corner.y >= owned.Min.y &&
                                 corner.y < owned.Max.y;
            const bool blocks = corner.x >= blocking.Min.x && corner.x <= blocking.Max.x &&
                                corner.y >= blocking.Min.y && corner.y <= blocking.Max.y;
            if ( !isOwned && !blocks )
                continue;
            ProceduralFoliageInstance copy = instance;
            copy.Blocker                   = instance.Blocker || !isOwned;
            copied.push_back( copy );
        }
        // Location order: the composite's result does not depend on how this tile keeps its instances.
        std::sort( copied.begin(), copied.end(), ByLocation );
        for ( const auto& instance : copied )
        {
            const int64_t id = toTile.NewSeed( instance.Location + offset, instance.Scale, instance.TypeIndex,
                                               instance.Age, instance.Blocker, &instance );
            if ( id >= 0 )
                toTile.m_Living.push_back( static_cast<uint32_t>( id ) );
        }
        toTile.FlushPendingRemovals();
    }
} // namespace Desert::World::Foliage::Procedural
