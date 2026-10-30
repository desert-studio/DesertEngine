#include "LandscapeGrassTypeService.hpp"

#include <memory>
#include <string>

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
} // namespace Desert::Runtime
