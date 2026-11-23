#include "WaterWavesService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/WaterWavesAsset.hpp>
#include <Engine/Water/GerstnerWaterWaves.hpp>
#include <Engine/Water/FFTOceanSpectrum.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace Desert::Runtime
{
    void WaterWavesService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void WaterWavesService::Adopt( const Assets::AssetHandle&                    handle,
                                   const Assets::Asset<Assets::WaterWavesAsset>& asset )
    {
        // Generated once per load: the set's waves are a pure function of its generator (UE RecomputeWaves).
        std::vector<Water::GerstnerWave> waves = Water::GenerateGerstnerWaves( asset->GetData().Generator );

        // A sum of Gerstner waves stays a surface only while the horizontal squeeze of all of them together cannot
        // pass the particle's own spacing: sum of Q k A (= each wave's Steepness) <= 1 (Tessendorf 2001 sec. 4.4;
        // UE's generator does not check it and its steep presets loop). Past it the mesh folds through itself where
        // the crests line up, and the whitecaps (the Jacobian below the material's threshold) are drawn from folds
        // rather than from crests about to break. Refused out loud: the set's author lowers the steepness.
        float steepness = 0.0f;
        for ( const Water::GerstnerWave& wave : waves )
            steepness += wave.Steepness;
        if ( steepness > 1.0f )
        {
            const std::string why = std::format( "its waves' steepness sums to {:.2f} (Q k A summed over {} waves); a "
                                                 "Gerstner sum folds through itself past 1 - lower SmallWaveSteepness/"
                                                 "LargeWaveSteepness",
                                                 steepness, waves.size() );
            LOG_ERROR( "[Water] Wave set {} is refused: {}", static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
            return;
        }
        m_Ready[handle] = std::make_shared<const std::vector<Water::GerstnerWave>>( std::move( waves ) );

        // An FFT set's ocean: its h0 cascades are a pure function of the FFT block, its seed and the scene's wind
        // (SetWind), built by GetSpectrum (3 x 256^2 texels) when the wind changes and uploaded by the water
        // renderer when the pointer does; the parse already refused a block the spectrum cannot honour.
        if ( asset->GetData().Source == Water::WaterWaveSource::FFT )
            m_Shapes[handle] = asset->GetData().FFT;
    }

    void WaterWavesService::SetWind( const Assets::AssetHandle& handle, const glm::vec2& velocityXZ )
    {
        const float speed = glm::length( velocityXZ );
        if ( !std::isfinite( speed ) )
            return; // WindAt sums finite sources; a non-finite sum is no wind to raise a sea by
        Water::FFTOceanWind wind;
        wind.Speed = std::round( speed / 10.0f ) * 10.0f;
        if ( wind.Speed > 0.0f )
            wind.AngleDeg = std::round( glm::degrees( std::atan2( velocityXZ.y, velocityXZ.x ) ) * 2.0f ) * 0.5f;
        const std::lock_guard<std::mutex> lock( m_WindMutex );
        m_Winds[handle] = wind;
    }

    void WaterWavesService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Ready.contains( handle ) || m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;

        const auto fail = [this, handle]( const std::string& why )
        {
            LOG_ERROR( "[Water] Wave set {} cannot be read: {}", static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
        };

        auto created = Assets::CreateFromRegistryRow<Assets::WaterWavesAsset>(
             m_Assets, handle, Common::Content::ContentKind::WaterWaves );
        if ( !created )
        {
            fail( created.GetError() );
            return;
        }
        if ( created.GetValue()->IsReadyForUse() )
        {
            Adopt( handle, created.GetValue() );
            return;
        }
        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             created.GetValue(),
             [this, handle, fail]( const Assets::Asset<Assets::AssetBase>& loaded,
                                   const Assets::LoadOutcome outcome, const std::string& error )
             {
                 m_Requests.erase( handle );
                 if ( outcome == Assets::LoadOutcome::Loaded )
                     Adopt( handle, std::static_pointer_cast<Assets::WaterWavesAsset>( loaded ) );
                 else
                     fail( error );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    Common::ResultStr<WaterWavesService::Waves> WaterWavesService::Get( const Assets::AssetHandle& handle )
    {
        Request( handle );
        if ( const auto failed = m_Failed.find( handle ); failed != m_Failed.end() )
            return Common::MakeError<Waves>( failed->second );
        const auto found = m_Ready.find( handle );
        if ( found == m_Ready.end() )
            return Common::MakeSuccess( Waves{} );
        return Common::MakeSuccess( found->second );
    }

    Common::ResultStr<WaterWavesService::Spectrum> WaterWavesService::GetSpectrum( const Assets::AssetHandle& handle )
    {
        Request( handle );
        if ( const auto failed = m_Failed.find( handle ); failed != m_Failed.end() )
            return Common::MakeError<Spectrum>( failed->second );
        const auto shape = m_Shapes.find( handle );
        if ( shape == m_Shapes.end() )
            return Common::MakeSuccess( Spectrum{} ); // a Gerstner set, or still loading
        // The set's wind this frame (still air until a body states one); the spectrum is rebuilt only when it
        // differs from the wind the current one was raised by.
        Water::FFTOceanWind wind; // still air until a body states one
        {
            const std::lock_guard<std::mutex> lock( m_WindMutex );
            if ( const auto wound = m_Winds.find( handle ); wound != m_Winds.end() )
                wind = wound->second;
        }
        Spectrum& built = m_Spectra[handle];
        if ( built == nullptr || !( built->Wind == wind ) )
            built = std::make_shared<const Water::FFTOceanSpectrum>(
                 Water::BuildFFTOceanSpectrum( shape->second, wind ) );
        return Common::MakeSuccess( built );
    }

    void WaterWavesService::Clear()
    {
        m_Requests.clear();
        m_Ready.clear();
        m_Spectra.clear();
        m_Shapes.clear();
        {
            const std::lock_guard<std::mutex> lock( m_WindMutex );
            m_Winds.clear();
        }
        m_Failed.clear();
        m_Surfaces.Clear();
    }
} // namespace Desert::Runtime
