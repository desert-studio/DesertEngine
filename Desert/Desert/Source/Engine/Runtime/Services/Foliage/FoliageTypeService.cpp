#include "FoliageTypeService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/Prefab/PrefabPlacement.hpp>
#include <Engine/World/Foliage/FoliagePrefabs.hpp>
#include <Common/Core/Constants.hpp>

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
        if ( !type.IsPrefab() || m_PrefabFailed.contains( guid ) )
            return nullptr;
        if ( const auto found = m_Prefabs.find( guid ); found != m_Prefabs.end() )
            return found->second;

        const auto manager = m_Assets.lock();
        if ( !manager )
            return nullptr;
        // By path, and the GUID the file states must be the one the type names: a prefab renamed over another
        // would otherwise be placed in its stead without a word.
        const std::filesystem::path file = Common::Constants::Path::ASSETS_PATH / type.Prefab.Path;
        const auto                  stated = World::Foliage::PrefabFileGuid( file );
        if ( !stated )
        {
            RefusePrefab( guid, stated.GetError() );
            return nullptr;
        }
        if ( stated.GetValue() != guid )
        {
            RefusePrefab( guid, "'" + file.string() + "' states GUID " + stated.GetValue() +
                                     ", not the one the foliage type names" );
            return nullptr;
        }
        auto prefab = manager->FindByPath<Assets::PrefabAsset>( file );
        if ( !prefab )
            prefab = manager->CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High, file );
        if ( !prefab )
        {
            RefusePrefab( guid, "'" + file.string() + "' could not be created as a prefab asset" );
            return nullptr;
        }
        if ( !prefab->IsReadyForUse() )
            if ( const auto loaded = prefab->Load(); !loaded )
            {
                RefusePrefab( guid, loaded.GetError() );
                return nullptr;
            }
        if ( prefab->GetEntities().empty() ||
             Assets::ClassifyPrefabRoot( prefab->GetEntities().front() ) != Assets::PrefabRootKind::World )
        {
            RefusePrefab( guid, "'" + file.string() + "' is empty or a UI prefab, which draws only under a canvas" );
            return nullptr;
        }
        m_Prefabs[guid] = prefab;
        return prefab;
    }

    void FoliageTypeService::Clear()
    {
        m_Requests.clear(); // released: no delegate fires into a cleared service
        m_Ready.clear();
        m_Failed.clear();
        m_Prefabs.clear();
        m_PrefabFailed.clear();
    }
} // namespace Desert::Runtime
