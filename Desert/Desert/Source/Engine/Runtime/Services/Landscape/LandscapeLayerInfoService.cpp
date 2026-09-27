#include "LandscapeLayerInfoService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void LandscapeLayerInfoService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void LandscapeLayerInfoService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Ready.contains( handle ) || m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;

        const auto fail = [this, handle]( const std::string& why )
        {
            // Said once per handle: a missing layer is a state of the scene, not an event of a frame.
            LOG_ERROR( "[Landscape] Layer info {} cannot be used; the landscape's weight planes of that layer are "
                       "neither drawn nor painted: {}",
                       static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
        };

        if ( handle == 0 )
        {
            fail( "the layer slot names no asset" );
            return;
        }
        auto created = Assets::CreateFromRegistryRow<Assets::LandscapeLayerInfoAsset>(
             m_Assets, handle, Common::Content::ContentKind::LandscapeLayerInfo );
        if ( !created )
        {
            fail( created.GetError() );
            return;
        }
        if ( created.GetValue()->IsReadyForUse() )
        {
            m_Ready[handle] = created.GetValue();
            return;
        }
        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             created.GetValue(),
             [this, handle, fail]( const Assets::Asset<Assets::AssetBase>& loaded,
                                   const Assets::LoadOutcome outcome, const std::string& error )
             {
                 m_Requests.erase( handle );
                 if ( outcome == Assets::LoadOutcome::Loaded )
                     m_Ready[handle] = std::static_pointer_cast<Assets::LandscapeLayerInfoAsset>( loaded );
                 else
                     fail( error );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    LandscapeLayerInfoService::State LandscapeLayerInfoService::StateOf( const Assets::AssetHandle& handle )
    {
        Request( handle );
        if ( m_Ready.contains( handle ) )
            return State::Ready;
        if ( m_Failed.contains( handle ) )
            return State::Failed;
        return State::Pending;
    }

    const Assets::Serialization::LandscapeLayerInfoData*
    LandscapeLayerInfoService::Get( const Assets::AssetHandle& handle )
    {
        if ( StateOf( handle ) != State::Ready )
            return nullptr;
        return &m_Ready.at( handle )->GetData();
    }

    Assets::Asset<Assets::LandscapeLayerInfoAsset>
    LandscapeLayerInfoService::AssetOf( const Assets::AssetHandle& handle )
    {
        if ( StateOf( handle ) != State::Ready )
            return nullptr;
        return m_Ready.at( handle );
    }

    std::string LandscapeLayerInfoService::ErrorOf( const Assets::AssetHandle& handle ) const
    {
        const auto found = m_Failed.find( handle );
        return found == m_Failed.end() ? std::string() : found->second;
    }

    Common::BoolResultStr
    LandscapeLayerInfoService::Save( const Assets::AssetHandle&                           handle,
                                     const Assets::Serialization::LandscapeLayerInfoData& data )
    {
        const auto asset = AssetOf( handle );
        if ( !asset )
            return Common::MakeFormattedError<bool>(
                 "layer info {} is not loaded ({}); nothing to save into", static_cast<uint64_t>( handle ),
                 m_Failed.contains( handle ) ? m_Failed.at( handle ) : "pending" );
        const auto manager = m_Assets.lock();
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
            m_Ready.erase( handle );
            m_Failed[handle] = reread.GetError();
            return reread;
        }
        return BOOLSUCCESS;
    }

    void LandscapeLayerInfoService::Clear()
    {
        m_Requests.clear(); // released: no delegate fires into a cleared service
        m_Ready.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
