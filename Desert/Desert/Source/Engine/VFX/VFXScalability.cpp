#include <Engine/VFX/VFXScalability.hpp>

#include <algorithm>
#include <limits>
#include <map>
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
                const glm::vec3 far( p.x >= 0.0f ? mx.x : mn.x, p.y >= 0.0f ? mx.y : mn.y,
                                     p.z >= 0.0f ? mx.z : mn.z );
                if ( glm::dot( glm::vec3( p ), far ) + p.w < 0.0f )
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
        }
        return "?";
    }

    std::vector<VFXScalabilityVerdict> EvaluateVFXScalability( std::span<const VFXViewReport>          views,
                                                               std::span<const VFXScalabilityCandidate> candidates,
                                                               const double                             tickSeconds )
    {
        std::vector<VFXScalabilityVerdict> out( candidates.size() );
        for ( std::size_t i = 0; i < candidates.size(); ++i )
        {
            const VFXScalabilityCandidate& c = candidates[i];
            VFXScalabilityVerdict&         v = out[i];
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
            return out;

        // THE INSTANCE LIMITS (UE: per system first, then per effect type): of the instances not yet culled, the
        // most significant `limit` are kept. Ties keep the earlier candidate, so the decision is stable.
        const auto applyLimit = [&]( auto keyOf, int32_t Level::*field, const VFXCullReason reason )
        {
            std::map<std::pair<const void*, const void*>, std::vector<std::size_t>> groups;
            for ( std::size_t i = 0; i < candidates.size(); ++i )
                if ( candidates[i].Type && out[i].Reason == VFXCullReason::None )
                    groups[keyOf( candidates[i] )].push_back( i );
            for ( auto& [key, members] : groups )
            {
                const int32_t limit = LimitOf( *candidates[members.front()].Type, views, field );
                if ( limit == 0 || members.size() <= static_cast<std::size_t>( limit ) )
                    continue;
                std::stable_sort( members.begin(), members.end(), [&]( std::size_t a, std::size_t b )
                                  { return out[a].Significance > out[b].Significance; } );
                for ( std::size_t k = static_cast<std::size_t>( limit ); k < members.size(); ++k )
                    out[members[k]].Reason = reason;
            }
        };
        applyLimit( []( const VFXScalabilityCandidate& c ) { return std::pair<const void*, const void*>{ c.Type, c.System }; },
                    &Level::MaxSystemInstances, VFXCullReason::SystemInstanceCount );
        applyLimit( []( const VFXScalabilityCandidate& c ) { return std::pair<const void*, const void*>{ c.Type, nullptr }; },
                    &Level::MaxInstances, VFXCullReason::InstanceCount );
        return out;
    }
} // namespace Desert::VFX
