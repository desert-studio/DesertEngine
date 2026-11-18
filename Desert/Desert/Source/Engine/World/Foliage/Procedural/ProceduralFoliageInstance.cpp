#include <Engine/World/Foliage/Procedural/ProceduralFoliageInstance.hpp>

#include <algorithm>
#include <bit>

namespace Desert::World::Foliage::Procedural
{
    float MaxRadius( const Assets::Serialization::FoliageProcedural& type )
    {
        return std::max( type.CollisionRadius, type.ShadeRadius );
    }

    float ScaleForAge( const Assets::Serialization::FoliageProcedural& type, float age )
    {
        const float time  = std::clamp( type.MaxAge == 0.0f ? 1.0f : age / type.MaxAge, 0.0f, 1.0f );
        const float share = Assets::Serialization::EvaluateFoliageCurve( type.ScaleCurve, time );
        return type.ProceduralScale.Min + ( type.ProceduralScale.Max - type.ProceduralScale.Min ) * share;
    }

    float InitAge( const Assets::Serialization::FoliageProcedural& type, Common::Math::RandomStream& stream )
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
