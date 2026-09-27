#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/FoliageTypeAsset.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.defoliage` assets foliage fields name, read on demand for the renderer (FO-5).
     *
     * A field holds its type as a handle (ECS::FoliageComponent::FoliageType); a scene load creates the asset
     * from its registry row without reading it. The first frame that draws the field asks here, which requests
     * the read through the AsyncAssetLoader; until it lands the type is pending. The service holds the ASSET,
     * not a copy of its data, so the paint panel's edit — saved and re-read into the same asset — is what the
     * next frame culls by. The shape of LandscapeLayerInfoService, which does the same for `.delayerinfo`.
     */
    class FoliageTypeService
    {
    public:
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The type's data when it is read; nullptr while it is pending or when it failed (said once, logged).
        const Assets::Serialization::FoliageTypeData* Get( const Assets::AssetHandle& handle );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );

        std::weak_ptr<Assets::AssetManager>                                              m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>                     m_Requests;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::FoliageTypeAsset>> m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>                             m_Failed;
    };
} // namespace Desert::Runtime
