#include <Engine/World/Foliage/Procedural/ProceduralFoliageInstance.hpp>

#include <algorithm>
#include <bit>

namespace Desert::World::Foliage::Procedural
{
    float RandomStream::GetFraction()
    {
        MutateSeed();
        // The 23 high bits of the seed as the mantissa of a float in [1, 2).
        const uint32_t bits = 0x3F800000u | ( m_Seed >> 9 );
        return std::bit_cast<float>( bits ) - 1.0f;
    }

    uint32_t RandomStream::GetUnsignedInt()
    {
        MutateSeed();
        return m_Seed;
    }

    int32_t RandomStream::RandRange( int32_t min, int32_t max )
    {
        const int32_t range = ( max - min ) + 1;
        if ( range <= 0 )
            return min;
        return min + std::min( static_cast<int32_t>( FRand() * static_cast<float>( range ) ), range - 1 );
    }

    float MaxRadius( const Assets::Serialization::FoliageProcedural& type )
    {
        return std::max( type.CollisionRadius, type.ShadeRadius );
    }

    namespace
    {
        // ScaleCurve, piecewise linear between its keys and constant beyond its ends.
        float EvaluateScaleCurve( const std::vector<Assets::Serialization::FoliageScaleCurveKey>& keys,
                                  float                                                           time )
        {
            if ( keys.empty() )
                return 0.0f;
            if ( time <= keys.front().Time )
                return keys.front().Value;
            if ( time >= keys.back().Time )
                return keys.back().Value;
            const auto  upper = std::upper_bound( keys.begin(), keys.end(), time,
                                                  []( float t, const auto& key ) { return t < key.Time; } );
            const auto& b     = *upper;
            const auto& a     = *( upper - 1 );
            const float alpha = ( time - a.Time ) / ( b.Time - a.Time );
            return a.Value + ( b.Value - a.Value ) * alpha;
        }
    } // namespace

    float ScaleForAge( const Assets::Serialization::FoliageProcedural& type, float age )
    {
        const float time  = std::clamp( type.MaxAge == 0.0f ? 1.0f : age / type.MaxAge, 0.0f, 1.0f );
        const float share = EvaluateScaleCurve( type.ScaleCurve, time );
        return type.ProceduralScale.Min + ( type.ProceduralScale.Max - type.ProceduralScale.Min ) * share;
    }

    float InitAge( const Assets::Serialization::FoliageProcedural& type, RandomStream& stream )
    {
        return type.MaxInitialAge * stream.GetFraction();
    }

    float NextAge( const Assets::Serialization::FoliageProcedural& type, float age, int32_t numSteps )
    {
        float next = age;
        for ( int32_t step = 0; step < numSteps; ++step )
        {
            const float grown = next + 1.0f;
            if ( grown > type.MaxAge )
                break;
            next = grown;
        }
        return next;
    }

    bool SpawnsInShade( const Assets::Serialization::FoliageProcedural& type )
    {
        return type.CanGrowInShade && type.SpawnsInShade;
    }

    InstanceRadii RadiiOf( const ProceduralFoliageInstance&                instance,
                           const Assets::Serialization::FoliageProcedural& type )
    {
        return InstanceRadii{ type.CollisionRadius * instance.Scale, type.ShadeRadius * instance.Scale };
    }

    namespace
    {
        // UE GetLessFit: a blocker always wins; then priority, then age, then scale.
        bool AIsLessFit( const ProceduralFoliageInstance& a, const Assets::Serialization::FoliageProcedural& aType,
                         const ProceduralFoliageInstance&                b,
                         const Assets::Serialization::FoliageProcedural& bType )
        {
            if ( a.Blocker )
                return false;
            if ( b.Blocker )
                return true;
            if ( aType.OverlapPriority != bType.OverlapPriority )
                return aType.OverlapPriority < bType.OverlapPriority;
            if ( a.Age != b.Age )
                return a.Age < b.Age;
            return a.Scale < b.Scale;
        }
    } // namespace

    const ProceduralFoliageInstance* Dominated( const ProceduralFoliageInstance&                a,
                                                const Assets::Serialization::FoliageProcedural& aType,
                                                const ProceduralFoliageInstance&                b,
                                                const Assets::Serialization::FoliageProcedural& bType,
                                                OverlapKind                                     kind )
    {
        // Two blockers belong to tiles that place them; the composite tile holding both must not judge between
        // them (UE lets the second one die, and a dead blocker no longer blocks).
        if ( a.Blocker && b.Blocker )
            return nullptr;
        const bool  aLoses    = AIsLessFit( a, aType, b, bType );
        const auto& loserType = aLoses ? aType : bType;
        if ( kind == OverlapKind::Shade && loserType.CanGrowInShade )
            return nullptr;
        return aLoses ? &a : &b;
    }
} // namespace Desert::World::Foliage::Procedural
