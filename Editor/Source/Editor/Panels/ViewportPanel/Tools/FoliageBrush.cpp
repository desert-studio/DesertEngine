#include "FoliageBrush.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Desert::Editor::Tools
{
    namespace
    {
        constexpr float kPi      = 3.14159265358979f;
        constexpr float kTwoPi   = 2.0f * kPi;
        constexpr float kSmall   = 1.0e-8f; // UE SMALL_NUMBER
        constexpr int   kBuckets = 10;      // UE NUM_INSTANCE_BUCKETS

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

    float FoliageBrushDesiredCount( float density, float radius, float paintDensity )
    {
        return kPi * radius * radius * density * paintDensity / ( 1000.0f * 1000.0f );
    }

    std::vector<glm::mat4> FoliageBrushAdd( const Assets::Serialization::FoliageTypeData& type,
                                            const FoliageBrushDab& dab, std::span<const glm::mat4> existing,
                                            FoliageRandom& rng, const FoliageBrushWorld& world )
    {
        std::vector<glm::mat4> placed;

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
            // UE GetRandomVectorInBrush: a point of the unit disk, and the segment through the sphere there.
            const float     ru    = 2.0f * rng.Next01() - 1.0f;
            const float     rv    = ( 2.0f * rng.Next01() - 1.0f ) * std::sqrt( 1.0f - ru * ru );
            const glm::vec3 point = ru * u + rv * v;
            const glm::vec3 rw    = std::sqrt( std::max( 1.0f - ( ru * ru + rv * rv ), 0.001f ) ) * n;
            const glm::vec3 start = dab.Center + dab.Radius * ( point + rw );
            const glm::vec3 end   = dab.Center + dab.Radius * ( point - rw );

            const auto hit = world.Trace( start, end );
            if ( !hit || !dab.Filter.Allows( hit->Surface ) )
                continue;
            if ( hit->Point.y < type.Height.Min || hit->Point.y > type.Height.Max )
                continue;
            if ( !IsWithinSlopeAngle( hit->Normal.y, type.GroundSlopeAngle.Min, type.GroundSlopeAngle.Max ) )
                continue;
            float weight = 1.0f;
            if ( layered && hit->LayerWeight )
            {
                weight = *hit->LayerWeight;
                if ( IsFilteredByWeight( weight, type.MinimumLayerWeight, rng ) )
                    continue;
            }
            potential[BucketOf( weight )].push_back( { hit->Point, glm::normalize( hit->Normal ) } );
        }

        // UE AddInstancesImp: per bucket, the share of what passed that is still missing.
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
        return placed;
    }

    void FoliageStroke::Touch( const Common::UUID& entity, const std::vector<glm::mat4>& current )
    {
        for ( const auto& field : m_Touched )
            if ( field.Entity == entity )
                return;
        m_Touched.push_back( { entity, current, {} } );
    }

    std::vector<FoliageStrokeField>
    FoliageStroke::Finish( const std::function<const std::vector<glm::mat4>*( const Common::UUID& )>& current ) const
    {
        std::vector<FoliageStrokeField> changed;
        for ( const auto& field : m_Touched )
        {
            const auto* now = current( field.Entity );
            if ( now && *now != field.Before )
                changed.push_back( { field.Entity, field.Before, *now } );
        }
        return changed;
    }

    size_t FoliageBrushErase( std::vector<glm::mat4>& instances, const glm::vec3& center, float radius )
    {
        const float  r2     = radius * radius;
        const size_t before = instances.size();
        std::erase_if( instances,
                       [&]( const glm::mat4& m )
                       {
                           const glm::vec3 d = glm::vec3( m[3] ) - center;
                           return glm::dot( d, d ) <= r2;
                       } );
        return before - instances.size();
    }
} // namespace Desert::Editor::Tools
