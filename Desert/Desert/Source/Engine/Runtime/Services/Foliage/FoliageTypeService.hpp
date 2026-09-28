#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/FoliageTypeAsset.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>

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

        /// The prefab a Prefab type places (FO-8): resolved by its GUID through the content registry and read by
        /// the AsyncAssetLoader on first ask, then kept. nullptr while it is read, and for good when it cannot be
        /// — no registry row states the GUID, the body does not read, or it is a UI prefab — said once.
        Assets::Asset<Assets::PrefabAsset> GetPrefab( const Assets::Serialization::FoliageTypeData& type );

        /// The manager the service creates in; null when none is bound.
        [[nodiscard]] std::shared_ptr<Assets::AssetManager> Manager() const
        {
            return m_Assets.lock();
        }

        /// Stops realizing @p prefabGuid after a failed instantiation, saying why once.
        void RefusePrefab( const std::string& prefabGuid, const std::string& why );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );

        std::weak_ptr<Assets::AssetManager>                                              m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>                     m_Requests;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::FoliageTypeAsset>> m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>                             m_Failed;
        std::unordered_map<std::string, Assets::LoadRequest>                             m_PrefabRequests;
        std::unordered_map<std::string, Assets::Asset<Assets::PrefabAsset>>              m_Prefabs;
        std::unordered_map<std::string, std::string>                                     m_PrefabFailed;
    };
} // namespace Desert::Runtime
