#include "FoliageTypeService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/Prefab/PrefabPlacement.hpp>
#include <Common/Content/TextAssetHeader.hpp>

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

    void FoliageTypeService::RefusePrefab( const std::string& prefabGuid, const std::string& why )
    {
        if ( m_PrefabFailed.emplace( prefabGuid, why ).second )
            LOG_ERROR( "[Foliage] Prefab {} is not placed as foliage: {}", prefabGuid, why );
        m_Prefabs.erase( prefabGuid );
    }

    Assets::Asset<Assets::PrefabAsset>
    FoliageTypeService::GetPrefab( const Assets::Serialization::FoliageTypeData& type )
    {
        const std::string& guid = type.Prefab.Guid;
        if ( !type.IsPrefab() || m_PrefabFailed.contains( guid ) || m_PrefabRequests.contains( guid ) )
            return nullptr;
        if ( const auto found = m_Prefabs.find( guid ); found != m_Prefabs.end() )
            return found->second;

        const auto manager = m_Assets.lock();
        if ( !manager )
            return nullptr;
        // Resolved through the registry row whose header states the GUID (UE: the soft reference resolved
        // through the asset registry), not through the path the type also carries: a prefab renamed or moved
        // keeps its GUID, and a different file put at the old path is not placed in its stead.
        const auto parsed = Common::Content::AssetGuidFromText( guid );
        if ( !parsed )
        {
            RefusePrefab( guid, parsed.GetError() );
            return nullptr;
        }
        auto prefab = Assets::CreateFromRegistryGuid<Assets::PrefabAsset>( *manager, parsed.GetValue(),
                                                                           Common::Content::ContentKind::Prefab );
        if ( !prefab )
        {
            RefusePrefab( guid, "no prefab in the content registry states this GUID (the type names '" +
                                     type.Prefab.Path + "')" );
            return nullptr;
        }
        // A UI prefab draws only under a canvas; a foliage field is not one.
        const auto accept = [this, guid]( const Assets::Asset<Assets::PrefabAsset>& read ) -> bool
        {
            if ( read->GetEntities().empty() ||
                 Assets::ClassifyPrefabRoot( read->GetEntities().front() ) != Assets::PrefabRootKind::World )
            {
                RefusePrefab( guid, "'" + read->GetMetadata().Filepath.generic_string() +
                                         "' is empty or a UI prefab, which draws only under a canvas" );
                return false;
            }
            m_Prefabs[guid] = read;
            return true;
        };
        if ( prefab->IsReadyForUse() )
            return accept( prefab ) ? prefab : nullptr;

        // Read by the one loader, off the frame: the fields stay unrealized until the body is in, as a mesh
        // field draws nothing until its mesh is.
        m_PrefabRequests[guid] = Assets::AsyncAssetLoader::Get().Request(
             prefab,
             [this, guid, accept]( const Assets::Asset<Assets::AssetBase>& loaded,
                                   const Assets::LoadOutcome outcome, const std::string& error )
             {
                 m_PrefabRequests.erase( guid );
                 if ( outcome == Assets::LoadOutcome::Loaded )
                     accept( std::static_pointer_cast<Assets::PrefabAsset>( loaded ) );
                 else
                     RefusePrefab( guid, error );
             },
             [this, guid] { m_PrefabRequests.erase( guid ); } );
        return nullptr;
    }

    void FoliageTypeService::Clear()
    {
        m_Requests.clear(); // released: no delegate fires into a cleared service
        m_Ready.clear();
        m_Failed.clear();
        m_PrefabRequests.clear(); // released first, as m_Requests
        m_Prefabs.clear();
        m_PrefabFailed.clear();
    }
} // namespace Desert::Runtime
