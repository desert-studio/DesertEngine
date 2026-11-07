#include <Engine/VFX/VFXScalability.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>

namespace Desert::VFX
{
    namespace
    {
        using Level = Assets::Serialization::VFXEffectTypeLevel;

        const Level& LevelOf( const Assets::Serialization::VFXEffectTypeData& type, const int level )
        {
            return type.Levels[static_cast<std::size_t>( std::clamp(
                 level, 0, static_cast<int>( Assets::Serialization::kVFXEffectTypeLevelCount ) - 1 ) )];
        }

        bool InFrustum( const VFXViewReport& view, const glm::vec3& mn, const glm::vec3& mx )
        {
            if ( !view.HasFrustum )
                return true;
            for ( const glm::vec4& p : view.FrustumPlanes )
            {
                // The box corner furthest along the plane's normal: outside if even it is behind the plane.
                const glm::vec3 farCorner( p.x >= 0.0f ? mx.x : mn.x, p.y >= 0.0f ? mx.y : mn.y,
                                     p.z >= 0.0f ? mx.z : mn.z );
                if ( glm::dot( glm::vec3( p ), farCorner ) + p.w < 0.0f )
                    return false;
            }
            return true;
        }

        /// The most generous limit over the views' levels: 0 (no limit) at any view wins.
        int32_t LimitOf( const Assets::Serialization::VFXEffectTypeData& type, std::span<const VFXViewReport> views,
                         int32_t Level::*field )
        {
            int32_t limit = -1;
            for ( const VFXViewReport& v : views )
            {
                const int32_t l = LevelOf( type, v.EffectsLevel ).*field;
                if ( l == 0 )
                    return 0;
                limit = std::max( limit, l );
            }
            return std::max( limit, 0 );
        }
    } // namespace

    const char* VFXCullReasonName( const VFXCullReason reason )
    {
        switch ( reason )
        {
            case VFXCullReason::None: return "not culled";
            case VFXCullReason::Distance: return "MaxDistance";
            case VFXCullReason::ViewFrustum: return "view frustum";
            case VFXCullReason::NotRendered: return "MaxTimeWithoutRender";
            case VFXCullReason::SystemInstanceCount: return "MaxSystemInstances";
            case VFXCullReason::InstanceCount: return "MaxInstances";
            case VFXCullReason::WorldBudget:
                return "Effects.MaxWorldParticles budget";
        }
        return "?";
    }

    void EvaluateVFXScalability( std::span<const VFXViewReport>           views,
                                 std::span<const VFXScalabilityCandidate> candidates, const double tickSeconds,
                                 std::span<VFXScalabilityVerdict> out, VFXScalabilityScratch& scratch ) noexcept
    {
        const std::size_t count = std::min( candidates.size(), out.size() );
        for ( std::size_t i = 0; i < count; ++i )
        {
            const VFXScalabilityCandidate& c = candidates[i];
            VFXScalabilityVerdict&         v = out[i];
            v                                = VFXScalabilityVerdict{}; // the buffer holds the last tick's
            v.NearestCm                      = std::numeric_limits<float>::infinity();
            if ( !c.Type )
            {
                v.Reason          = VFXCullReason::Distance;
                v.SpawnCountScale = 0.0;
                continue;
            }
            if ( views.empty() )
                continue; // headless: the system as authored
            const glm::vec3 centre = 0.5f * ( c.BoundsMinCm + c.BoundsMaxCm );
            bool            inRange = false, kept = false, seen = false;
            float           renderLimit = -1.0f; // the most generous MaxTimeWithoutRender; 0 = never
            v.SpawnCountScale           = 0.0;
            for ( const VFXViewReport& view : views )
            {
                const Level& l     = LevelOf( *c.Type, view.EffectsLevel );
                const float  d     = glm::distance( centre, view.Origin );
                const bool   range = l.MaxDistance <= 0.0f || d <= l.MaxDistance;
                const bool   in    = InFrustum( view, c.BoundsMinCm, c.BoundsMaxCm );
                v.NearestCm        = std::min( v.NearestCm, d );
                v.SpawnCountScale  = std::max( v.SpawnCountScale, view.SpawnCountScale * l.SpawnCountScale );
                inRange            = inRange || range;
                kept               = kept || ( range && ( !l.CullByViewFrustum || in ) );
                seen               = seen || in;
                renderLimit = ( l.MaxTimeWithoutRender <= 0.0f || renderLimit == 0.0f )
                                   ? 0.0f
                                   : std::max( renderLimit, l.MaxTimeWithoutRender );
            }
            v.HiddenSeconds = seen ? 0.0 : c.HiddenSeconds + tickSeconds;
            v.Significance  = c.Type->Significance == Assets::Serialization::VFXSignificance::Age
                                   ? -c.AgeSeconds
                                   : -static_cast<double>( v.NearestCm );
            if ( !inRange )
                v.Reason = VFXCullReason::Distance;
            else if ( !kept )
                v.Reason = VFXCullReason::ViewFrustum;
            else if ( renderLimit > 0.0f && v.HiddenSeconds > renderLimit )
                v.Reason = VFXCullReason::NotRendered;
        }
        if ( views.empty() )
            return;

        // The not-yet-culled candidates in falling significance; ties keep the earlier candidate, so the decision
        // is stable (an index tie-break with std::sort: std::stable_sort would allocate its buffer every tick).
        std::vector<std::size_t>& order       = scratch.Order;
        const auto                gatherAlive = [&]()
        {
            order.clear();
            for ( std::size_t i = 0; i < count; ++i )
                if ( candidates[i].Type && out[i].Reason == VFXCullReason::None )
                    order.push_back( i );
        };
        const auto moreSignificant = [&]( std::size_t a, std::size_t b )
        {
            if ( out[a].Significance != out[b].Significance )
                return out[a].Significance > out[b].Significance;
            return a < b;
        };

        // THE INSTANCE LIMITS (UE: per system first, then per effect type): of the instances not yet culled, the
        // most significant `limit` are kept. Ties keep the earlier candidate, so the decision is stable.
        // Grouped by sorting on (key, significance): each group is one run of the order.
        using GroupKey        = std::pair<const void*, const void*>;
        const auto applyLimit = [&]( auto keyOf, int32_t Level::*field, const VFXCullReason reason )
        {
            gatherAlive();
            std::sort( order.begin(), order.end(),
                       [&]( std::size_t a, std::size_t b )
                       {
                           const GroupKey ka = keyOf( candidates[a] );
                           const GroupKey kb = keyOf( candidates[b] );
                           if ( ka != kb )
                               return std::less<GroupKey>{}( ka, kb );
                           return moreSignificant( a, b );
                       } );
            for ( std::size_t first = 0; first < order.size(); )
            {
                const GroupKey key = keyOf( candidates[order[first]] );
                std::size_t    end = first + 1;
                while ( end < order.size() && keyOf( candidates[order[end]] ) == key )
                    ++end;
                const int32_t limit = LimitOf( *candidates[order[first]].Type, views, field );
                if ( limit != 0 && end - first > static_cast<std::size_t>( limit ) )
                    for ( std::size_t k = first + static_cast<std::size_t>( limit ); k < end; ++k )
                        out[order[k]].Reason = reason;
                first = end;
            }
        };
        applyLimit( []( const VFXScalabilityCandidate& c ) { return std::pair<const void*, const void*>{ c.Type, c.System }; },
                    &Level::MaxSystemInstances, VFXCullReason::SystemInstanceCount );
        applyLimit( []( const VFXScalabilityCandidate& c ) { return std::pair<const void*, const void*>{ c.Type, nullptr }; },
                    &Level::MaxInstances, VFXCullReason::InstanceCount );

        // THE WORLD BUDGET (UE Niagara's global budget; the pool is one per world, so the most generous view's
        // budget holds - 0 at any view = none): the survivors in falling significance take their pool slots; the
        // first that would pass the budget and every less significant one after it are refused. Strict order: a
        // smaller, less significant system never takes the slots a more significant one was refused.
        std::uint64_t budget = 0;
        for ( const VFXViewReport& view : views )
        {
            if ( view.MaxWorldParticles == 0 )
            {
                budget = 0;
                break;
            }
            budget = std::max<std::uint64_t>( budget, view.MaxWorldParticles );
        }
        if ( budget == 0 )
            return;
        gatherAlive();
        std::sort( order.begin(), order.end(), moreSignificant );
        std::uint64_t taken = 0;
        bool          over  = false;
        for ( const std::size_t i : order )
        {
            out[i].BudgetParticles   = budget;
            out[i].BudgetTakenBefore = taken;
            over                     = over || taken + candidates[i].PoolParticles > budget;
            if ( over )
                out[i].Reason = VFXCullReason::WorldBudget;
            else
                taken += candidates[i].PoolParticles;
        }
    }
} // namespace Desert::VFX
