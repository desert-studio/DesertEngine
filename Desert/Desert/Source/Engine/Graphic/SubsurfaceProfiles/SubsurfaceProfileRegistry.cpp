#include <Engine/Graphic/SubsurfaceProfiles/SubsurfaceProfileRegistry.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Graphic
{
    namespace
    {
        std::pair<uint64_t, uint64_t> Key( const Common::Content::AssetGuid& guid )
        {
            return { guid.Hi, guid.Lo };
        }

        // UE GetPerpendicularScalingFactor (SubsurfaceScatteringMath.h): Christensen & Burley 2015's fit of the
        // shape parameter s to the surface albedo A, light entering perpendicular to the surface.
        float PerpendicularScalingFactor( float albedo )
        {
            const float a = albedo - 0.8f;
            return 1.85f - albedo + 7.0f * std::abs( a * a * a );
        }

        // UE GetSearchLightDiffuseScalingFactor: s for the searchlight configuration, the one the screen pass and
        // the transmission profile shape the diffusion with (SubsurfaceProfileCommon.ush LIGHT_PERPENDICULAR_DIFFUSE_SURFACE).
        float SearchLightScalingFactor( float albedo )
        {
            const float a  = albedo - 0.33f;
            const float a2 = a * a;
            return 3.5f + 100.0f * a2 * a2;
        }

        // UE BurleyNormalized::TransmissionProfile: the normalized diffusion integrated from r to infinity,
        // A/4 * ( e^(-s r / L) + 3 e^(-s r / 3L) ), L the channel's DIFFUSE mean free path.
        float BurleyTransmission( float albedo, float scaling, float dmfp, float radius )
        {
            if ( dmfp <= 0.0f )
                return 0.0f;
            return 0.25f * albedo * ( std::exp( -scaling * radius / dmfp ) + 3.0f * std::exp( -scaling * radius / ( 3.0f * dmfp ) ) );
        }

        // UE ProfileRadiusOffset (BurleyNormalizedSSS.cpp:20), cm: the half-texel the profile is sampled past.
        constexpr float kProfileRadiusOffset = 0.06f;
        constexpr float kCmToMm              = 10.0f;
    } // namespace

    glm::vec3 DiffuseMeanFreePathFromMeanFreePath( const glm::vec3& surfaceAlbedo, const glm::vec3& meanFreePath )
    {
        constexpr float kDmfp2MfpMagicNumber = 0.6f; // UE's Dmfp2MfpMagicNumber, matched to its path tracer
        glm::vec3       dmfp;
        for ( int c = 0; c < 3; ++c )
            dmfp[c] = meanFreePath[c] * SearchLightScalingFactor( surfaceAlbedo[c] ) /
                      ( PerpendicularScalingFactor( surfaceAlbedo[c] ) * kDmfp2MfpMagicNumber );
        return dmfp;
    }

    void EncodeBurleyRow( const Assets::Serialization::SubsurfaceProfileData& p, SubsurfaceProfileRow& row )
    {
        namespace L = SubsurfaceProfileLayout;
        row.fill( glm::vec4( 0.0f ) );
        row[L::kTint]          = glm::vec4( p.Tint, static_cast<float>( p.Implementation ) );
        row[L::kSurfaceAlbedo] = glm::vec4( p.SurfaceAlbedo, 1.0f );
        // UE stores the DIFFUSE mean free path (SubsurfaceProfile.cpp:541-553); centimetres, the project unit
        // (CONTRACT §2.5, Q2): no WorldUnitScale, no mm. a = the widest channel's (UE SetupSurfaceAlbedoAndDiffuseMeanFreePath).
        const glm::vec3 dmfp = DiffuseMeanFreePathFromMeanFreePath( p.SurfaceAlbedo, p.MeanFreePathColor * p.MeanFreePathDistance );
        row[L::kDiffuseMeanFreePath] = glm::vec4( dmfp, std::max( dmfp.r, std::max( dmfp.g, dmfp.b ) ) );
        row[L::kTransmission] =
             glm::vec4( p.ExtinctionScale, p.NormalScale, p.ScatteringDistribution, 1.0f / p.IOR );
        row[L::kBoundaryColorBleed] = glm::vec4( p.BoundaryColorBleed, 1.0f );
        row[L::kDualSpecular]       = glm::vec4( p.Roughness0, p.Roughness1, p.LobeMix, 0.0f );

        // UE ComputeTransmissionProfileBurley (BurleyNormalizedSSS.cpp:171-219): texel i = thickness
        // i / size * kMaxTransmissionDistance, the profile read at that radius (+ the offset) over DistanceScale, the
        // same DMFP as the screen pass and searchlight s, albedo white (the recombine applies the base colour),
        // tinted; a = UE's shadow term e^(-thickness(mm) * ExtinctionScale). The last texel is black.
        glm::vec3 scaling;
        for ( int c = 0; c < 3; ++c )
            scaling[c] = SearchLightScalingFactor( p.SurfaceAlbedo[c] );
        const float invUnitScale = 1.0f / std::max( p.DistanceScale, 1e-4f );
        for ( uint32_t i = 0; i < L::kTransmissionProfileSize; ++i )
        {
            const float thickness = L::kMaxTransmissionDistance * static_cast<float>( i ) / float( L::kTransmissionProfileSize );
            const float radius    = ( thickness + kProfileRadiusOffset ) * invUnitScale;
            glm::vec3   transmission;
            for ( int c = 0; c < 3; ++c )
                transmission[c] = BurleyTransmission( 1.0f, scaling[c], dmfp[c], radius );
            const float shadow = std::exp( -thickness * invUnitScale * kCmToMm * p.ExtinctionScale );
            row[L::kTransmissionProfile + i] = glm::vec4( transmission * p.TransmissionTintColor, shadow );
        }
        row[L::kTransmissionProfile + L::kTransmissionProfileSize - 1] = glm::vec4( 0.0f );
    }

    Common::ResultStr<uint32_t> SubsurfaceProfileRegistry::Acquire(
         const Common::Content::AssetGuid& guid, const Assets::Serialization::SubsurfaceProfileData& data )
    {
        if ( guid.IsNull() )
            return Common::MakeFormattedError<uint32_t>( "a subsurface profile with a null GUID has no row" );
        if ( const auto it = m_ByGuid.find( Key( guid ) ); it != m_ByGuid.end() )
            return Common::MakeSuccess( it->second );

        uint32_t index = 0;
        if ( !m_Free.empty() )
        {
            index = m_Free.back();
            m_Free.pop_back();
        }
        else
        {
            if ( m_Rows.size() >= kMaxProfiles )
                return Common::MakeFormattedError<uint32_t>(
                     "the subsurface profile atlas is full: {} profiles are named (UE MAX_SUBSURFACE_PROFILE_COUNT)",
                     kMaxProfiles );
            index = static_cast<uint32_t>( m_Rows.size() );
            m_Rows.emplace_back();
        }
        EncodeBurleyRow( data, m_Rows[index] );
        m_ByGuid.emplace( Key( guid ), index );
        m_Dirty = true;
        return Common::MakeSuccess( index );
    }

    void SubsurfaceProfileRegistry::Update( const Common::Content::AssetGuid&                    guid,
                                            const Assets::Serialization::SubsurfaceProfileData& data )
    {
        if ( const auto it = m_ByGuid.find( Key( guid ) ); it != m_ByGuid.end() )
        {
            EncodeBurleyRow( data, m_Rows[it->second] );
            m_Dirty = true;
        }
    }

    void SubsurfaceProfileRegistry::Release( const Common::Content::AssetGuid& guid )
    {
        if ( const auto it = m_ByGuid.find( Key( guid ) ); it != m_ByGuid.end() )
        {
            m_Free.push_back( it->second );
            m_ByGuid.erase( it );
        }
    }

    uint32_t SubsurfaceProfileRegistry::IndexOf( const Common::Content::AssetGuid& guid ) const
    {
        const auto it = m_ByGuid.find( Key( guid ) );
        return it != m_ByGuid.end() ? it->second : kInvalidIndex;
    }

    void SubsurfaceProfileRegistry::Clear()
    {
        m_ByGuid.clear();
        m_Rows.clear();
        m_Free.clear();
        m_Dirty = true;
    }
} // namespace Desert::Graphic
