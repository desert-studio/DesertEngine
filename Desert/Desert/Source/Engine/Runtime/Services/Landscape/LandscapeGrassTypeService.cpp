#include "LandscapeGrassTypeService.hpp"

#include <Engine/Assets/ContentRegistry.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Runtime
{
    void LandscapeGrassTypeService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Types.BindAssetManager( assets );
    }

    LandscapeGrassTypeService::State LandscapeGrassTypeService::StateOf( const Assets::AssetHandle& handle )
    {
        return m_Types.StateOf( handle );
    }

    Assets::Asset<Assets::LandscapeGrassTypeAsset>
    LandscapeGrassTypeService::AssetOf( const Assets::AssetHandle& handle )
    {
        const auto asset = m_Types.AssetOf( handle );
        // A hot reload that failed mid-write leaves the asset not ready: it grows nothing until a good read.
        return asset && asset->IsReadyForUse() ? asset : nullptr;
    }

    std::string LandscapeGrassTypeService::ErrorOf( const Assets::AssetHandle& handle ) const
    {
        return m_Types.ErrorOf( handle );
    }

    void LandscapeGrassTypeService::Clear()
    {
        m_Types.Clear();
    }

    std::vector<Assets::AssetHandle> LandscapeGrassTypeService::InstanceIdListMaterials()
    {
        using Common::Content::ContentKind;
        const std::string grassType( Common::Content::KindName( ContentKind::LandscapeGrassType ) );
        std::vector<Assets::ContentRegistry::ClosureRow> roots;
        for ( const auto& row : Assets::ContentRegistry::Rows( ContentKind::LandscapeGrassType ) )
            roots.push_back( { row.Handle, grassType } );
        const std::string_view           material = Common::Content::KindName( ContentKind::Material );
        std::vector<Assets::AssetHandle> handles;
        for ( const auto& reached : Assets::ContentRegistry::Closure( roots ) )
        {
            if ( reached.Kind != material )
                continue;
            // An edge holds the number the referencing file states (a GUID's fold); the row answers with its own.
            const auto row =
                 Assets::ContentRegistry::RowOf( ContentKind::Material, static_cast<uint64_t>( reached.Handle ) );
            if ( row && std::find( handles.begin(), handles.end(), row->Handle ) == handles.end() )
                handles.push_back( row->Handle );
        }
        return handles;
    }
} // namespace Desert::Runtime
