#include "FoliageBrush.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

namespace Desert::Editor::Tools
{
    namespace
    {
        constexpr float kPi      = 3.14159265358979f;
        constexpr float kTwoPi   = 2.0f * kPi;
        constexpr float kSmall   = 1.0e-8f; // UE SMALL_NUMBER
        constexpr int   kBuckets = 10;      // UE NUM_INSTANCE_BUCKETS

        using Clock = std::chrono::steady_clock;

        double MsSince( Clock::time_point start )
        {
            return std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
        }

        struct PotentialInstance
        {
            glm::vec3 Point;
            glm::vec3 Normal;
        };

        // UE FVector::FindBestAxisVectors: two unit axes perpendicular to @p n and to each other.
        void BestAxes( const glm::vec3& n, glm::vec3& u, glm::vec3& v )
        {
            const glm::vec3 a = glm::abs( n );
            // Pick the world axis least aligned with n, as UE does (it tests Z first; Y is up here).
            const glm::vec3 ref =
                 ( a.y > a.x && a.y > a.z ) ? glm::vec3( 1.0f, 0.0f, 0.0f ) : glm::vec3( 0.0f, 1.0f, 0.0f );
            u = glm::normalize( ref - n * glm::dot( ref, n ) );
            v = glm::cross( u, n );
        }

        // UE IsWithinSlopeAngle, on the up component (Y here, Z in UE).
        bool IsWithinSlopeAngle( float normalUp, float minAngle, float maxAngle )
        {
            const float maxNormalAngle = std::cos( glm::radians( maxAngle ) );
            const float minNormalAngle = std::cos( glm::radians( minAngle ) );
            return !( maxNormalAngle > normalUp + kSmall || minNormalAngle < normalUp - kSmall );
        }

        // UE IsFilteredByWeight (inclusion test): true = the instance is rejected. The random draw makes a
        // layer's weight the probability an instance survives above the minimum.
        bool IsFilteredByWeight( float weight, float minimum, FoliageRandom& rng )
        {
            const float needed = std::max( kSmall, std::max( minimum, rng.Next01() ) );
            return weight < needed;
        }

        // The type's height, slope and landscape-layer rules on one traced hit (UE
        // CheckLocationForPotentialInstance_ThreadSafe + IsFilteredByWeight); the layer draw consumes the
        // stream only for a layered hit, as before.
        bool PassesTypeRules( const Assets::Serialization::FoliageTypeData& type, const FoliageTraceHit& hit,
                              bool layered, FoliageRandom& rng )
        {
            if ( hit.Point.y < type.Height.Min || hit.Point.y > type.Height.Max )
                return false;
            if ( !IsWithinSlopeAngle( hit.Normal.y, type.GroundSlopeAngle.Min, type.GroundSlopeAngle.Max ) )
                return false;
            return !( layered && hit.LayerWeight &&
                      IsFilteredByWeight( *hit.LayerWeight, type.MinimumLayerWeight, rng ) );
        }

        int BucketOf( float weight )
        {
            return std::clamp( static_cast<int>( std::lround( weight * static_cast<float>( kBuckets - 1 ) ) ), 0,
                               kBuckets - 1 );
        }

        glm::quat AlignUpToNormal( const glm::vec3& n )
        {
            const glm::vec3 up( 0.0f, 1.0f, 0.0f );
            const float     d = glm::dot( up, n );
            if ( d > 0.9999f )
                return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
            if ( d < -0.9999f )
                return glm::angleAxis( kPi, glm::vec3( 1.0f, 0.0f, 0.0f ) );
            return glm::angleAxis( std::acos( d ), glm::normalize( glm::cross( up, n ) ) );
        }

        // UE FPotentialInstance::PlaceInstance reduced to this type's fields: scale, yaw, pitch, normal
        // alignment and the Z offset along the placed up axis.
        glm::mat4 PlaceInstance( const Assets::Serialization::FoliageTypeData& type, const PotentialInstance& p,
                                 FoliageRandom& rng )
        {
            const float scale   = glm::mix( type.ScaleX.Min, type.ScaleX.Max, rng.Next01() );
            const float zOffset = glm::mix( type.ZOffset.Min, type.ZOffset.Max, rng.Next01() );
            glm::mat4   m       = glm::translate( glm::mat4( 1.0f ), p.Point );
            if ( type.AlignToNormal )
                m *= glm::mat4_cast( AlignUpToNormal( p.Normal ) );
            m = glm::translate( m, glm::vec3( 0.0f, zOffset, 0.0f ) );
            if ( type.RandomYaw )
                m = glm::rotate( m, rng.Next01() * kTwoPi, glm::vec3( 0.0f, 1.0f, 0.0f ) );
            if ( type.RandomPitchAngle > 0.0f )
            {
                const float pitch   = glm::radians( type.RandomPitchAngle ) * rng.Next01();
                const float heading = rng.Next01() * kTwoPi;
                m = glm::rotate( m, pitch, glm::vec3( std::cos( heading ), 0.0f, std::sin( heading ) ) );
            }
            return glm::scale( m, glm::vec3( scale ) );
        }
    } // namespace

    FoliageRandom::FoliageRandom( uint64_t seed )
    {
        // pcg32_srandom with the default stream: state 0, step, add the seed, step.
        m_State = 0u;
        NextU32();
        m_State += seed;
        NextU32();
    }

    uint32_t FoliageRandom::NextU32()
    {
        const uint64_t old    = m_State;
        m_State               = old * 6364136223846793005ull + 1442695040888963407ull;
        const auto xorshifted = static_cast<uint32_t>( ( ( old >> 18u ) ^ old ) >> 27u );
        const auto rot        = static_cast<uint32_t>( old >> 59u );
        return ( xorshifted >> rot ) | ( xorshifted << ( ( 32u - rot ) & 31u ) );
    }

    float FoliageRandom::Next01()
    {
        return static_cast<float>( NextU32() >> 8u ) * ( 1.0f / 16777216.0f );
    }

    bool FoliageSurfaceFilter::AllowsHit( const FoliageTraceHit& hit ) const
    {
        if ( !Allows( hit.Surface ) )
            return false;
        if ( !LayerFiltered || hit.Surface != FoliageSurface::Landscape )
            return true;
        // A landscape without the brush's layers weighs nothing there: filtered out, not waved through.
        return hit.BrushLayerWeight.value_or( 0.0f ) >= MinimumLayerWeight;
    }

    float FoliageBrushDesiredCount( float density, float radius, float paintDensity )
    {
        return kPi * radius * radius * density * paintDensity / ( 1000.0f * 1000.0f );
    }

    std::vector<glm::mat4> FoliageBrushAdd( const Assets::Serialization::FoliageTypeData& type,
                                            const FoliageBrushDab& dab, std::span<const glm::mat4> existing,
                                            FoliageRandom& rng, const FoliageBrushWorld& world,
                                            FoliageBrushStats* stats )
    {
        std::vector<glm::mat4> placed;
        FoliageBrushStats      local;
        FoliageBrushStats&     st = stats ? *stats : local;

        // UE ApplyBrush: "allow a single instance with a random chance, if the brush is smaller than the
        // density".
        const float desiredF = FoliageBrushDesiredCount( type.Density, dab.Radius, dab.PaintDensity );
        const int   desired =
             desiredF > 1.0f ? static_cast<int>( std::lround( desiredF ) ) : ( rng.Next01() < desiredF ? 1 : 0 );

        // UE AddInstancesForBrush: what the sphere already holds, bucketed by its layer weight when the type
        // is tied to layers, all in the last bucket otherwise.
        const bool                layered = !type.LandscapeLayers.empty();
        std::array<int, kBuckets> existingBuckets{};
        int                       numExisting = 0;
        const float               r2          = dab.Radius * dab.Radius;
        const auto                existingAt  = Clock::now();
        for ( const glm::mat4& m : existing )
        {
            const glm::vec3 p = glm::vec3( m[3] );
            const glm::vec3 d = p - dab.Center;
            if ( glm::dot( d, d ) > r2 )
                continue;
            ++numExisting;
            if ( layered && world.LayerWeightAt )
                if ( const auto w = world.LayerWeightAt( p ) )
                    ++existingBuckets[BucketOf( *w )];
        }
        st.ExistingLayerMs += MsSince( existingAt );
        if ( !layered )
            existingBuckets[kBuckets - 1] = numExisting;

        if ( desired <= numExisting || !world.Trace )
            return placed;

        // UE CalculatePotentialInstances: every desired candidate traced through the brush sphere, kept when
        // its surface passes the filter and the type's height, slope and layer rules.
        const glm::vec3 n = glm::normalize( dab.Normal );
        glm::vec3       u, v;
        BestAxes( n, u, v );
        std::array<std::vector<PotentialInstance>, kBuckets> potential;
        for ( int i = 0; i < desired; ++i )
        {
            auto stageAt = Clock::now();
            // UE GetRandomVectorInBrush: a point of the unit disk, and the segment through the sphere there.
            const float     ru    = 2.0f * rng.Next01() - 1.0f;
            const float     rv    = ( 2.0f * rng.Next01() - 1.0f ) * std::sqrt( 1.0f - ru * ru );
            const glm::vec3 point = ru * u + rv * v;
            const glm::vec3 rw    = std::sqrt( std::max( 1.0f - ( ru * ru + rv * rv ), 0.001f ) ) * n;
            const glm::vec3 start = dab.Center + dab.Radius * ( point + rw );
            const glm::vec3 end   = dab.Center + dab.Radius * ( point - rw );

            st.GenerateMs += MsSince( stageAt );
            ++st.Candidates;

            stageAt        = Clock::now();
            const auto hit = world.Trace( start, end, dab.Filter );
            st.TraceMs += MsSince( stageAt );
            if ( !hit || !dab.Filter.AllowsHit( *hit ) )
                continue;
            ++st.Hits;
            stageAt = Clock::now();
            if ( !PassesTypeRules( type, *hit, layered, rng ) )
            {
                st.FilterMs += MsSince( stageAt );
                continue;
            }
            st.FilterMs += MsSince( stageAt );
            ++st.Passed;
            const float weight = layered && hit->LayerWeight ? *hit->LayerWeight : 1.0f;
            potential[BucketOf( weight )].push_back( { hit->Point, glm::normalize( hit->Normal ) } );
        }

        // UE AddInstancesImp: per bucket, the share of what passed that is still missing.
        const auto placeAt = Clock::now();
        for ( int b = 0; b < kBuckets; ++b )
        {
            const auto& bucket   = potential[b];
            const float fraction = static_cast<float>( b + 1 ) / static_cast<float>( kBuckets );
            const int   count    = std::clamp(
                 static_cast<int>( std::lround(
                      fraction * static_cast<float>( static_cast<int>( bucket.size() ) - existingBuckets[b] ) *
                      dab.Pressure ) ),
                 0, static_cast<int>( bucket.size() ) );
            for ( int i = 0; i < count; ++i )
                placed.push_back( PlaceInstance( type, bucket[i], rng ) );
        }
        st.PlaceMs += MsSince( placeAt );
        st.Placed += static_cast<int>( placed.size() );
        return placed;
    }

    void FoliageStroke::Touch( const Common::UUID& entity, const std::vector<glm::mat4>& current,
                               const FoliageSelection& selected )
    {
        for ( const auto& field : m_Touched )
            if ( field.Entity == entity )
                return;
        m_Touched.push_back( { entity, current, {}, selected, {} } );
    }

    std::vector<glm::vec3>& FoliageStroke::Readjusted( const Common::UUID& entity )
    {
        for ( auto& [id, origins] : m_Readjusted )
            if ( id == entity )
                return origins;
        return m_Readjusted.emplace_back( entity, std::vector<glm::vec3>{} ).second;
    }

    std::vector<FoliageStrokeField>
    FoliageStroke::Finish( const std::function<const std::vector<glm::mat4>*( const Common::UUID& )>& current,
                           const std::function<FoliageSelection( const Common::UUID& )>& selected ) const
    {
        std::vector<FoliageStrokeField> changed;
        for ( const auto& field : m_Touched )
        {
            const auto* now = current( field.Entity );
            if ( !now )
                continue;
            FoliageSelection nowSelected = selected ? selected( field.Entity ) : field.SelectedBefore;
            if ( *now != field.Before || nowSelected != field.SelectedBefore )
                changed.push_back(
                     { field.Entity, field.Before, *now, field.SelectedBefore, std::move( nowSelected ) } );
        }
        return changed;
    }

    namespace
    {
        // Drops the instances @p gone marks and renumbers @p selected to the survivors (UE removes by index
        // and rebuilds SelectedIndices the same way).
        size_t RemoveMarked( std::vector<glm::mat4>& instances, const std::vector<bool>& gone,
                             FoliageSelection* selected )
        {
            std::vector<uint32_t> newIndex( instances.size(), UINT32_MAX );
            size_t                kept = 0;
            for ( size_t i = 0; i < instances.size(); ++i )
            {
                if ( gone[i] )
                    continue;
                newIndex[i]       = static_cast<uint32_t>( kept );
                instances[kept++] = instances[i];
            }
            const size_t removed = instances.size() - kept;
            instances.resize( kept );
            if ( selected )
            {
                FoliageSelection renumbered;
                for ( const uint32_t i : *selected )
                    if ( i < newIndex.size() && newIndex[i] != UINT32_MAX )
                        renumbered.push_back( newIndex[i] );
                *selected = std::move( renumbered );
            }
            return removed;
        }

        bool InSphere( const glm::mat4& m, const glm::vec3& center, float r2 )
        {
            const glm::vec3 d = glm::vec3( m[3] ) - center;
            return glm::dot( d, d ) <= r2;
        }
    } // namespace

    size_t FoliageBrushRemove( std::vector<glm::mat4>& instances, const glm::vec3& center, float radius,
                               FoliageSelection* selected )
    {
        const float       r2 = radius * radius;
        std::vector<bool> gone( instances.size(), false );
        for ( size_t i = 0; i < instances.size(); ++i )
            gone[i] = InSphere( instances[i], center, r2 );
        return RemoveMarked( instances, gone, selected );
    }

    std::optional<glm::mat4> FoliageBrushSingle( const Assets::Serialization::FoliageTypeData& type,
                                                 const FoliageBrushDab& dab, FoliageRandom& rng,
                                                 const FoliageBrushWorld& world )
    {
        if ( !world.Trace )
            return std::nullopt;
        // UE: "simply generate a start/end around the brush location so the line check will hit the brush
        // location" - BrushLocation +- BrushNormal.
        const glm::vec3 n   = glm::normalize( dab.Normal );
        const auto      hit = world.Trace( dab.Center + n, dab.Center - n, dab.Filter );
        if ( !hit || !dab.Filter.AllowsHit( *hit ) )
            return std::nullopt;
        if ( !PassesTypeRules( type, *hit, !type.LandscapeLayers.empty(), rng ) )
            return std::nullopt;
        return PlaceInstance( type, { hit->Point, glm::normalize( hit->Normal ) }, rng );
    }

    size_t FoliageSelectInSphere( std::span<const glm::mat4> instances, const glm::vec3& center, float radius,
                                  bool select, FoliageSelection& selected )
    {
        const float       r2 = radius * radius;
        std::vector<bool> in( instances.size(), false );
        for ( const uint32_t i : selected )
            if ( i < in.size() )
                in[i] = true;
        size_t changed = 0;
        for ( size_t i = 0; i < instances.size(); ++i )
            if ( in[i] != select && InSphere( instances[i], center, r2 ) )
            {
                in[i] = select;
                ++changed;
            }
        selected.clear();
        for ( size_t i = 0; i < in.size(); ++i )
            if ( in[i] )
                selected.push_back( static_cast<uint32_t>( i ) );
        return changed;
    }

    std::optional<FoliagePick> FoliagePickInstance( std::span<const glm::mat4> instances,
                                                    const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                                                    const glm::vec3& localMin, const glm::vec3& localMax )
    {
        const glm::vec3            dir = glm::normalize( rayDirection );
        std::optional<FoliagePick> best;
        for ( size_t i = 0; i < instances.size(); ++i )
        {
            // The ray in the instance's own space, slab test against the mesh box there; the entry parameter
            // is the same along the world ray because the local ray is the world ray mapped affinely.
            const glm::mat4 inv = glm::inverse( instances[i] );
            const glm::vec3 o   = glm::vec3( inv * glm::vec4( rayOrigin, 1.0f ) );
            const glm::vec3 d   = glm::vec3( inv * glm::vec4( dir, 0.0f ) );
            float           t0 = 0.0f, t1 = std::numeric_limits<float>::max();
            bool            hit = true;
            for ( int k = 0; k < 3 && hit; ++k )
            {
                if ( std::abs( d[k] ) < kSmall )
                {
                    hit = o[k] >= localMin[k] && o[k] <= localMax[k];
                    continue;
                }
                float ta = ( localMin[k] - o[k] ) / d[k];
                float tb = ( localMax[k] - o[k] ) / d[k];
                if ( ta > tb )
                    std::swap( ta, tb );
                t0  = std::max( t0, ta );
                t1  = std::min( t1, tb );
                hit = t0 <= t1;
            }
            if ( hit && ( !best || t0 < best->Distance ) )
                best = FoliagePick{ static_cast<uint32_t>( i ), t0 };
        }
        return best;
    }

    std::vector<glm::mat4> FoliageFill( const Assets::Serialization::FoliageTypeData& type,
                                        std::span<const FoliageFillTriangle> triangles, float paintDensity,
                                        const FoliageSurfaceFilter& filter, FoliageRandom& rng )
    {
        std::vector<glm::mat4> placed;
        for ( const FoliageFillTriangle& tri : triangles )
        {
            if ( !filter.Allows( tri.Surface ) )
                continue;
            const glm::vec3 e1    = tri.B - tri.A;
            const glm::vec3 e2    = tri.C - tri.A;
            const glm::vec3 cross = glm::cross( e1, e2 );
            const float     twice = glm::length( cross );
            if ( twice <= kSmall )
                continue;
            const glm::vec3 normal = cross / twice;
            if ( !IsWithinSlopeAngle( normal.y, type.GroundSlopeAngle.Min, type.GroundSlopeAngle.Max ) )
                continue;
            // UE: Triangle.Area * Density * PaintDensity / (1000 * 1000), a fraction below one a chance of one.
            const float desiredF = 0.5f * twice * type.Density * paintDensity / ( 1000.0f * 1000.0f );
            const int   desired  = desiredF > 1.0f ? static_cast<int>( std::lround( desiredF ) )
                                                   : ( rng.Next01() < desiredF ? 1 : 0 );
            for ( int i = 0; i < desired; ++i )
            {
                // UE FFoliagePaintBucketTriangle::GetRandomPoint: the parallelogram folded into the triangle.
                float x = rng.Next01();
                float y = rng.Next01();
                if ( x + y > 1.0f )
                {
                    x = 1.0f - x;
                    y = 1.0f - y;
                }
                const FoliageTraceHit hit{ tri.A + x * e1 + y * e2, normal, tri.Surface, std::nullopt };
                if ( !PassesTypeRules( type, hit, false, rng ) )
                    continue;
                placed.push_back( PlaceInstance( type, { hit.Point, normal }, rng ) );
            }
        }
        return placed;
    }

    size_t FoliageBrushThin( std::vector<glm::mat4>& instances, const glm::vec3& center, float radius, int desired,
                             FoliageRandom& rng, FoliageSelection* selected )
    {
        const float           r2 = radius * radius;
        std::vector<uint32_t> inside;
        for ( size_t i = 0; i < instances.size(); ++i )
            if ( InSphere( instances[i], center, r2 ) )
                inside.push_back( static_cast<uint32_t>( i ) );
        const int excess = static_cast<int>( inside.size() ) - std::max( desired, 0 );
        if ( excess <= 0 )
            return 0;
        // A partial Fisher-Yates from the stroke's stream: the first `excess` of the shuffled indices leave.
        std::vector<bool> gone( instances.size(), false );
        for ( int k = 0; k < excess; ++k )
        {
            const auto span = static_cast<uint32_t>( inside.size() ) - static_cast<uint32_t>( k );
            const auto j    = static_cast<uint32_t>( k ) + rng.NextU32() % span;
            std::swap( inside[k], inside[j] );
            gone[inside[k]] = true;
        }
        return RemoveMarked( instances, gone, selected );
    }

    size_t FoliageRemoveSelected( std::vector<glm::mat4>& instances, FoliageSelection& selected )
    {
        std::vector<bool> gone( instances.size(), false );
        for ( const uint32_t i : selected )
            if ( i < gone.size() )
                gone[i] = true;
        const size_t removed = RemoveMarked( instances, gone, nullptr );
        selected.clear();
        return removed;
    }

    void FoliageMoveSelected( std::vector<glm::mat4>& instances, const FoliageSelection& selected,
                              const glm::vec3& offset )
    {
        for ( const uint32_t i : selected )
            if ( i < instances.size() )
                instances[i][3] += glm::vec4( offset, 0.0f );
    }

    FoliageReapplyResult FoliageBrushReapply( const Assets::Serialization::FoliageTypeData& type,
                                              const FoliageReapplySettings& settings, const FoliageBrushDab& dab,
                                              std::vector<glm::mat4>& instances,
                                              std::vector<glm::vec3>& readjusted, FoliageRandom& rng,
                                              const FoliageBrushWorld& world, FoliageSelection* selected )
    {
        FoliageReapplyResult result;
        if ( settings.Density )
        {
            // UE ReapplyInstancesDensityForBrush, against the type's current Density.
            const float desiredF = FoliageBrushDesiredCount( type.Density, dab.Radius, dab.PaintDensity );
            const int   desired  = static_cast<int>( std::lround( desiredF ) );
            result.Thinned       = FoliageBrushThin( instances, dab.Center, dab.Radius, desired, rng, selected );
            if ( result.Thinned == 0 )
            {
                const auto added = FoliageBrushAdd( type, dab, instances, rng, world );
                for ( const glm::mat4& m : added )
                    readjusted.push_back( glm::vec3( m[3] ) );
                instances.insert( instances.end(), added.begin(), added.end() );
                result.Added = added.size();
            }
        }
        const float       r2      = dab.Radius * dab.Radius;
        const bool        layered = !type.LandscapeLayers.empty();
        std::vector<bool> gone( instances.size(), false );
        for ( size_t i = 0; i < instances.size(); ++i )
        {
            glm::mat4&      m      = instances[i];
            const glm::vec3 origin = glm::vec3( m[3] );
            if ( !InSphere( m, dab.Center, r2 ) ||
                 std::find( readjusted.begin(), readjusted.end(), origin ) != readjusted.end() )
                continue;

            // The instance as PlaceInstance built it: origin = ground + align * (0, offset, 0), rotation =
            // align * yaw (* pitch), uniform scale.
            const float     scale = glm::length( glm::vec3( m[1] ) );
            const glm::mat3 rot   = glm::mat3( m ) / std::max( scale, kSmall );
            const glm::vec3 up    = glm::normalize( rot[1] );

            // UE traces along the mesh's Z axis; the offset is not stored per instance here, so the segment
            // spans the brush radius both ways instead of UE's 16 cm around the un-offset location.
            if ( !world.Trace )
                return result;
            const auto hit = world.Trace( origin + dab.Radius * up, origin - dab.Radius * up, dab.Filter );
            if ( !hit || !dab.Filter.Allows( hit->Surface ) )
            {
                ++result.Skipped;
                continue;
            }
            const glm::vec3 ground = hit->Point;
            const glm::vec3 normal = glm::normalize( hit->Normal );

            if ( ( settings.GroundSlope &&
                   !IsWithinSlopeAngle( normal.y, type.GroundSlopeAngle.Min, type.GroundSlopeAngle.Max ) ) ||
                 ( settings.LandscapeLayers && layered && hit->LayerWeight &&
                   IsFilteredByWeight( *hit->LayerWeight, type.MinimumLayerWeight, rng ) ) ||
                 ( settings.Height && ( ground.y < type.Height.Min || ground.y > type.Height.Max ) ) )
            {
                gone[i] = true;
                ++result.Removed;
                continue;
            }

            // What is kept: the up axis (align), the yaw about it, the offset above the ground, the scale.
            const glm::quat keptAlign = AlignUpToNormal( up );
            const glm::mat3 local     = glm::mat3_cast( glm::inverse( keptAlign ) ) * rot;
            const float     keptYaw   = std::atan2( -local[0].z, local[0].x );
            const float     keptZ     = glm::dot( origin - ground, up );

            const glm::quat align = settings.AlignToNormal ? ( type.AlignToNormal ? AlignUpToNormal( normal )
                                                                                  : glm::quat( 1, 0, 0, 0 ) )
                                                           : keptAlign;
            const float     newScale =
                 settings.Scale ? glm::mix( type.ScaleX.Min, type.ScaleX.Max, rng.Next01() ) : scale;
            const float zOffset =
                 settings.ZOffset ? glm::mix( type.ZOffset.Min, type.ZOffset.Max, rng.Next01() ) : keptZ;
            const float yaw = settings.RandomYaw ? ( type.RandomYaw ? rng.Next01() * kTwoPi : 0.0f ) : keptYaw;

            glm::mat4 out = glm::translate( glm::mat4( 1.0f ), ground ) * glm::mat4_cast( align );
            out           = glm::translate( out, glm::vec3( 0.0f, zOffset, 0.0f ) );
            out           = glm::rotate( out, yaw, glm::vec3( 0.0f, 1.0f, 0.0f ) );
            if ( settings.RandomPitch && type.RandomPitchAngle > 0.0f )
            {
                const float pitch   = glm::radians( type.RandomPitchAngle ) * rng.Next01();
                const float heading = rng.Next01() * kTwoPi;
                out = glm::rotate( out, pitch, glm::vec3( std::cos( heading ), 0.0f, std::sin( heading ) ) );
            }
            m = glm::scale( out, glm::vec3( newScale ) );
            readjusted.push_back( glm::vec3( m[3] ) );
            ++result.Updated;
        }
        RemoveMarked( instances, gone, selected );
        return result;
    }
} // namespace Desert::Editor::Tools
