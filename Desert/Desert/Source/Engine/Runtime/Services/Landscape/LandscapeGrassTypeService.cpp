#include "LandscapeGrassTypeService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void LandscapeGrassTypeService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void LandscapeGrassTypeService::Fail( const Assets::AssetHandle& handle, const std::string& why )
    {
        // Said once per handle: a missing grass type is a state of the scene, not an event of a frame.
        LOG_ERROR( "[Landscape] Grass type {} cannot be used; the layers naming it do not grow it: {}",
                   static_cast<uint64_t>( handle ), why );
        m_Failed[handle] = why;
    }

    void LandscapeGrassTypeService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;
        if ( const auto ready = m_Ready.find( handle ); ready != m_Ready.end() )
        {
            // RELEASED BY A SWEEP = READ AGAIN ON THE NEXT ASK (AssetEviction contract: build on miss). An
            // eviction Unload()s the shell this map holds, leaving it not ready and NOT broken; trusting the
            // entry would grow nothing for the rest of the session. A broken shell (a hot reload whose read
            // failed) stays as it is: only a good reload clears it, a reader does not re-read it every frame.
            const auto& held = ready->second;
            if ( held->IsReadyForUse() || held->IsBroken() )
                return;
            LOG_INFO( "[Landscape] Grass type {} ('{}') was released; reading it again.",
                      static_cast<uint64_t>( handle ), held->GetMetadata().Filepath.string() );
            const Assets::Asset<Assets::LandscapeGrassTypeAsset> shell = held;
            m_Ready.erase( ready );
            Read( handle, shell );
            return;
        }

        if ( handle == 0 )
        {
            Fail( handle, "the layer info names no grass type asset" );
            return;
        }
        auto created = Assets::CreateFromRegistryRow<Assets::LandscapeGrassTypeAsset>(
             m_Assets, handle, Common::Content::ContentKind::LandscapeGrassType );
        if ( !created )
        {
            Fail( handle, created.GetError() );
            return;
        }
        if ( created.GetValue()->IsReadyForUse() )
        {
            m_Ready[handle] = created.GetValue();
            return;
        }
        Read( handle, created.GetValue() );
    }

    void LandscapeGrassTypeService::Read( const Assets::AssetHandle&                            handle,
                                          const Assets::Asset<Assets::LandscapeGrassTypeAsset>& shell )
    {
        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             shell,
             [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 m_Requests.erase( handle );
                 if ( outcome == Assets::LoadOutcome::Loaded )
                     m_Ready[handle] = std::static_pointer_cast<Assets::LandscapeGrassTypeAsset>( loaded );
                 else
                     Fail( handle, error );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    LandscapeGrassTypeService::State LandscapeGrassTypeService::StateOf( const Assets::AssetHandle& handle )
    {
        Request( handle );
        if ( m_Ready.contains( handle ) )
            return State::Ready;
        if ( m_Failed.contains( handle ) )
            return State::Failed;
        return State::Pending;
    }

    Assets::Asset<Assets::LandscapeGrassTypeAsset>
    LandscapeGrassTypeService::AssetOf( const Assets::AssetHandle& handle )
    {
        if ( StateOf( handle ) != State::Ready )
            return nullptr;
        const auto& asset = m_Ready.at( handle );
        // A hot reload that failed mid-write leaves the asset not ready: it grows nothing until a good read.
        return asset && asset->IsReadyForUse() ? asset : nullptr;
    }

    std::string LandscapeGrassTypeService::ErrorOf( const Assets::AssetHandle& handle ) const
    {
        const auto found = m_Failed.find( handle );
        return found == m_Failed.end() ? std::string() : found->second;
    }

    void LandscapeGrassTypeService::Clear()
    {
        m_Requests.clear(); // released: no delegate fires into a cleared service
        m_Ready.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
