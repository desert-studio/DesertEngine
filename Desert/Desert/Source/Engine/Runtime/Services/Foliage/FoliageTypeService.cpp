#include "FoliageTypeService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void FoliageTypeService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void FoliageTypeService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Ready.contains( handle ) || m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;

        const auto fail = [this, handle]( const std::string& why )
        {
            // Said once per handle: a type that cannot be read is a state of the scene, not an event of a frame.
            LOG_ERROR(
                 "[Foliage] Foliage type {} cannot be read; its fields are drawn without a cull distance: {}",
                 static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
        };

        auto created = Assets::CreateFromRegistryRow<Assets::FoliageTypeAsset>(
             m_Assets, handle, Common::Content::ContentKind::FoliageType );
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
                     m_Ready[handle] = std::static_pointer_cast<Assets::FoliageTypeAsset>( loaded );
                 else
                     fail( error );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    const Assets::Serialization::FoliageTypeData* FoliageTypeService::Get( const Assets::AssetHandle& handle )
    {
        Request( handle );
        const auto found = m_Ready.find( handle );
        return found == m_Ready.end() ? nullptr : &found->second->GetData();
    }

    void FoliageTypeService::Clear()
    {
        m_Requests.clear(); // released: no delegate fires into a cleared service
        m_Ready.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
