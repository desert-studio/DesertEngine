#include "LandscapeGrassTypeService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void LandscapeGrassTypeService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void LandscapeGrassTypeService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Ready.contains( handle ) || m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;

        const auto fail = [this, handle]( const std::string& why )
        {
            // Said once per handle: a missing grass type is a state of the scene, not an event of a frame.
            LOG_ERROR( "[Landscape] Grass type {} cannot be used; the layer naming it grows no grass: {}",
                       static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
        };

        if ( handle == 0 )
        {
            fail( "the layer info names no grass type asset" );
            return;
        }
        auto created = Assets::CreateFromRegistryRow<Assets::LandscapeGrassTypeAsset>(
             m_Assets, handle, Common::Content::ContentKind::LandscapeGrassType );
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
                     m_Ready[handle] = std::static_pointer_cast<Assets::LandscapeGrassTypeAsset>( loaded );
                 else
                     fail( error );
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

    const Assets::Serialization::LandscapeGrassTypeData*
    LandscapeGrassTypeService::Get( const Assets::AssetHandle& handle )
    {
        if ( StateOf( handle ) != State::Ready )
            return nullptr;
        return &m_Ready.at( handle )->GetData();
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
