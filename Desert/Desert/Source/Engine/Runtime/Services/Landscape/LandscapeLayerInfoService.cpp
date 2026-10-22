#include "LandscapeLayerInfoService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

namespace Desert::Runtime
{
    void LandscapeLayerInfoService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Layers.BindAssetManager( assets );
    }

    LandscapeLayerInfoService::State LandscapeLayerInfoService::StateOf( const Assets::AssetHandle& handle )
    {
        return m_Layers.StateOf( handle );
    }

    const Assets::Serialization::LandscapeLayerInfoData*
    LandscapeLayerInfoService::Get( const Assets::AssetHandle& handle )
    {
        const auto asset = m_Layers.AssetOf( handle );
        return asset ? &asset->GetData() : nullptr;
    }

    Assets::Asset<Assets::LandscapeLayerInfoAsset>
    LandscapeLayerInfoService::AssetOf( const Assets::AssetHandle& handle )
    {
        return m_Layers.AssetOf( handle );
    }

    std::string LandscapeLayerInfoService::ErrorOf( const Assets::AssetHandle& handle ) const
    {
        return m_Layers.ErrorOf( handle );
    }

    Common::BoolResultStr
    LandscapeLayerInfoService::Save( const Assets::AssetHandle&                           handle,
                                     const Assets::Serialization::LandscapeLayerInfoData& data )
    {
        const auto asset = AssetOf( handle );
        if ( !asset )
        {
            const std::string why = m_Layers.ErrorOf( handle );
            return Common::MakeFormattedError<bool>( "layer info {} is not loaded ({}); nothing to save into",
                                                     static_cast<uint64_t>( handle ),
                                                     why.empty() ? std::string( "pending" ) : why );
        }
        const auto manager = m_Layers.Manager();
        if ( !manager )
            return Common::MakeFormattedError<bool>( "layer info {}: no asset manager is bound",
                                                     static_cast<uint64_t>( handle ) );
        if ( auto saved = Assets::LandscapeLayerInfoAsset::Save( asset->GetMetadata().Filepath, data ); !saved )
            return saved;
        // Re-read into the SAME asset, so every holder of GetData() sees the edit; a read that fails leaves the
        // layer Failed with the reason rather than showing numbers the file no longer has.
        (void)asset->Unload();
        if ( auto reread = Assets::LoadThroughLoader( *manager, asset ); !reread )
        {
            m_Layers.Fail( handle, reread.GetError() );
            return reread;
        }
        return BOOLSUCCESS;
    }

    void LandscapeLayerInfoService::Clear()
    {
        m_Layers.Clear();
    }
} // namespace Desert::Runtime
