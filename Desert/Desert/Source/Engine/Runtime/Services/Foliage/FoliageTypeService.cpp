#include "FoliageTypeService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/Prefab/PrefabPlacement.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void FoliageTypeService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Types.BindAssetManager( assets );
    }

    const Assets::Serialization::FoliageTypeData* FoliageTypeService::Get( const Assets::AssetHandle& handle )
    {
        const auto type = m_Types.AssetOf( handle );
        return type ? &type->GetData() : nullptr;
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

        const auto manager = m_Types.Manager();
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
        m_Types.Clear();
        m_PrefabRequests.clear(); // released: no delegate fires into a cleared service
        m_Prefabs.clear();
        m_PrefabFailed.clear();
    }
} // namespace Desert::Runtime
