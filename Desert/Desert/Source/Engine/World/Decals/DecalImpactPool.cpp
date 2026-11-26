#include "DecalImpactPool.hpp"

#include <Engine/Graphic/Systems/Scene/Decals/DecalRenderer.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::World
{
    namespace
    {
        // The impact's projection box: z along the hit normal (the DecalComponent's projection axis), x / y a
        // tangent frame rolled around it. The reference axis is world up (+Y), or +X for a floor / ceiling hit, so
        // a wall's texture stays upright the way UE's FRotationMatrix::MakeFromX( -Normal ) keeps it.
        glm::mat4 ImpactBox( const DecalImpactSpawn& spawn, const glm::vec3& normal )
        {
            const glm::vec3 reference =
                 std::abs( normal.y ) < 0.99f ? glm::vec3( 0.0f, 1.0f, 0.0f ) : glm::vec3( 1.0f, 0.0f, 0.0f );
            glm::vec3       x    = glm::normalize( glm::cross( reference, normal ) );
            glm::vec3       y    = glm::cross( normal, x );
            const float     roll = glm::radians( spawn.Roll );
            const float     c    = std::cos( roll );
            const float     s    = std::sin( roll );
            const glm::vec3 rx   = x * c + y * s;
            const glm::vec3 ry   = y * c - x * s;
            return glm::mat4( glm::vec4( rx * spawn.Size.x, 0.0f ), glm::vec4( ry * spawn.Size.y, 0.0f ),
                              glm::vec4( normal * spawn.Size.z, 0.0f ), glm::vec4( spawn.At, 1.0f ) );
        }
    } // namespace

    void DecalImpactPool::SetCapacity( const uint32_t capacity ) noexcept
    {
        const std::lock_guard lock( m_Mutex );
        if ( capacity == m_Ring.size() )
            return;
        // The live records oldest first, then the newest `capacity` of them re-seated from slot 0.
        std::vector<Record> kept;
        kept.reserve( m_Ring.size() );
        for ( std::size_t i = 0; i < m_Ring.size(); ++i )
        {
            const Record& record = m_Ring[( m_Next + i ) % m_Ring.size()];
            if ( record.Live )
                kept.push_back( record );
        }
        const std::size_t drop = kept.size() > capacity ? kept.size() - capacity : 0u;
        m_Ring.assign( capacity, Record{} );
        std::copy( kept.begin() + static_cast<std::ptrdiff_t>( drop ), kept.end(), m_Ring.begin() );
        m_Next = capacity == 0u ? 0u : static_cast<uint32_t>( ( kept.size() - drop ) % capacity );
    }

    Common::BoolResultStr DecalImpactPool::Spawn( const DecalImpactSpawn& spawn ) noexcept
    {
        const float normalLength = glm::length( spawn.Normal );
        if ( !( normalLength > 1.0e-6f ) )
            return Common::MakeError( "World.spawnDecal: Normal is zero" );
        if ( !( spawn.Size.x > 0.0f && spawn.Size.y > 0.0f && spawn.Size.z > 0.0f ) )
            return Common::MakeError( "World.spawnDecal: every Size axis must be > 0 cm" );

        const std::lock_guard lock( m_Mutex );
        if ( m_Ring.empty() )
            return Common::MakeError( "World.spawnDecal: no impact decals at this quality "
                                      "(Effects.DecalImpactPoolSize 0)" );
        Record& record      = m_Ring[m_Next];
        record.DecalToWorld = ImpactBox( spawn, spawn.Normal / normalLength );
        record.Material     = spawn.Material;
        record.Serial       = ++m_Serial;
        record.Age          = 0.0f;
        record.Lifetime     = std::max( spawn.Lifetime, 0.0f );
        record.FadeDuration = std::max( spawn.FadeDuration, 0.0f );
        record.FadeStart    = std::max( spawn.FadeStart, 0.0f );
        record.FadeLength   = std::max( spawn.FadeLength, 0.0f );
        record.SortOrder    = spawn.SortOrder;
        record.Live         = true;
        m_Next              = static_cast<uint32_t>( ( m_Next + 1u ) % m_Ring.size() );
        return BOOLSUCCESS;
    }

    void DecalImpactPool::Advance( const float seconds ) noexcept
    {
        if ( !( seconds > 0.0f ) )
            return;
        const std::lock_guard lock( m_Mutex );
        for ( Record& record : m_Ring )
        {
            if ( !record.Live )
                continue;
            record.Age += seconds;
            if ( record.Lifetime > 0.0f && record.Age >= record.Lifetime )
                record.Live = false;
        }
    }

    void DecalImpactPool::Collect( std::vector<Graphic::System::DecalProxy>& out ) const
    {
        const std::lock_guard lock( m_Mutex );
        for ( std::size_t i = 0; i < m_Ring.size(); ++i )
        {
            const Record& record = m_Ring[( m_Next + i ) % m_Ring.size()];
            if ( !record.Live )
                continue;
            // UE SetFadeOut: full until FadeDuration before the end of Lifetime, then linear to nothing.
            float opacity = 1.0f;
            if ( record.Lifetime > 0.0f && record.FadeDuration > 0.0f )
                opacity = std::clamp( ( record.Lifetime - record.Age ) / record.FadeDuration, 0.0f, 1.0f );
            if ( opacity <= 0.0f )
                continue;
            Graphic::System::DecalProxy proxy;
            proxy.DecalToWorld   = record.DecalToWorld;
            proxy.Material       = record.Material;
            proxy.EntityId       = record.Serial;
            proxy.SortOrder      = record.SortOrder;
            proxy.FadeStart      = record.FadeStart;
            proxy.FadeLength     = record.FadeLength;
            proxy.FadeScreenSize = 0.0f; // a bullet hole is small by design; the distance fade retires it
            proxy.EdgeFade       = 0.25f;
            proxy.Opacity        = opacity;
            out.push_back( proxy );
        }
    }

    void DecalImpactPool::Clear() noexcept
    {
        const std::lock_guard lock( m_Mutex );
        std::fill( m_Ring.begin(), m_Ring.end(), Record{} );
        m_Next = 0;
    }

    uint32_t DecalImpactPool::GetCapacity() const noexcept
    {
        const std::lock_guard lock( m_Mutex );
        return static_cast<uint32_t>( m_Ring.size() );
    }

    uint32_t DecalImpactPool::GetLiveCount() const noexcept
    {
        const std::lock_guard lock( m_Mutex );
        return static_cast<uint32_t>(
             std::count_if( m_Ring.begin(), m_Ring.end(), []( const Record& r ) { return r.Live; } ) );
    }
} // namespace Desert::World
