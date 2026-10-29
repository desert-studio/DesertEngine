#include "WaterWavesService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

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

    void WaterWavesService::Clear()
    {
        m_Requests.clear();
        m_Ready.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
