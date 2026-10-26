#include <Engine/Graphic/SubsurfaceProfiles/SubsurfaceProfileRegistry.hpp>

#include <cmath>

namespace Desert::Graphic
{
    namespace
    {
        std::pair<uint64_t, uint64_t> Key( const Common::Content::AssetGuid& guid )
        {
            return { guid.Hi, guid.Lo };
        }

        // UE GetPerpendicularScalingFactor (BurleyNormalizedSSSCommon.ush): Christensen & Burley 2015's fit of the
        // shape parameter s to the surface albedo A, for the searchlight configuration.
        float BurleyScalingFactor( float albedo )
        {
            const float a = albedo - 0.8f;
            return 1.85f - albedo + 7.0f * std::abs( a * a * a );
        }

        // Christensen-Burley normalized diffusion integrated over the far side of a slab of thickness t (UE
        // ComputeTransmissionProfileBurley): A/4 * ( e^(-s t / d) + 3 e^(-s t / 3d) ), d the channel's mean free path.
        float BurleyTransmission( float albedo, float meanFreePath, float thickness )
        {
            const float s = BurleyScalingFactor( albedo ) / std::max( meanFreePath, 1e-4f );
            return 0.25f * albedo * ( std::exp( -s * thickness ) + 3.0f * std::exp( -s * thickness / 3.0f ) );
        }
    } // namespace

    void EncodeBurleyRow( const Assets::Serialization::SubsurfaceProfileData& p, SubsurfaceProfileRow& row )
    {
        namespace L = SubsurfaceProfileLayout;
        row.fill( glm::vec4( 0.0f ) );
        row[L::kTint]          = glm::vec4( p.Tint, static_cast<float>( p.Implementation ) );
        row[L::kSurfaceAlbedo] = glm::vec4( p.SurfaceAlbedo, 1.0f );
        // Centimetres, the project unit (CONTRACT §2.5, Q2): no WorldUnitScale, no mm.
        row[L::kDiffuseMeanFreePath] = glm::vec4( p.MeanFreePathColor * p.MeanFreePathDistance, p.MeanFreePathDistance );
        row[L::kTransmission] =
             glm::vec4( p.ExtinctionScale, p.NormalScale, p.ScatteringDistribution, 1.0f / p.IOR );
        row[L::kBoundaryColorBleed] = glm::vec4( p.BoundaryColorBleed, 1.0f );
        row[L::kDualSpecular]       = glm::vec4( p.Roughness0, p.Roughness1, p.LobeMix, 0.0f );

        // The transmission over thickness, 0 .. kMaxTransmissionDistance (scaled by DistanceScale), tinted; a = the
        // texel's thickness so the shader needs no second constant.
        const glm::vec3 mfp = p.MeanFreePathColor * p.MeanFreePathDistance;
        for ( uint32_t i = 0; i < L::kTransmissionProfileSize; ++i )
        {
            const float t =
                 L::kMaxTransmissionDistance * static_cast<float>( i ) / float( L::kTransmissionProfileSize - 1 );
            const float scaled = t * p.DistanceScale * p.ExtinctionScale;
            glm::vec3   transmission;
            for ( int c = 0; c < 3; ++c )
                transmission[c] = BurleyTransmission( p.SurfaceAlbedo[c], mfp[c], scaled );
            row[L::kTransmissionProfile + i] = glm::vec4( transmission * p.TransmissionTintColor, t );
        }
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
