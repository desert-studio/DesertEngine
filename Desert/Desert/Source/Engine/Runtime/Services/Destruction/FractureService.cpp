#include "FractureService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void FractureService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void FractureService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Ready.contains( handle ) || m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;

        const auto fail = [this, handle]( const std::string& why )
        {
            LOG_ERROR( "[Destruction] Fracture {} cannot be read: {}", static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
        };

        auto created = Assets::CreateFromRegistryRow<Assets::FractureAsset>(
             m_Assets, handle, Common::Content::ContentKind::Fracture );
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
                     m_Ready[handle] = std::static_pointer_cast<Assets::FractureAsset>( loaded );
                 else
                     fail( error );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    Common::ResultStr<std::shared_ptr<const Destruction::FractureData>>
    FractureService::Get( const Assets::AssetHandle& handle )
    {
        using Result = std::shared_ptr<const Destruction::FractureData>;
        Request( handle );
        if ( const auto failed = m_Failed.find( handle ); failed != m_Failed.end() )
            return Common::MakeError<Result>( failed->second );
        const auto found = m_Ready.find( handle );
        if ( found == m_Ready.end() )
            return Common::MakeSuccess( Result{} );
        // Aliasing: the pointer keeps the asset, and so its fracture, alive.
        return Common::MakeSuccess( Result( found->second, &found->second->GetFracture() ) );
    }

    void FractureService::Clear()
    {
        m_Requests.clear();
        m_Ready.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
