#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/WorldParametersAsset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.dworldparams` collections scenes name (WTH-PARAMS).
     *
     * READ SYNCHRONOUSLY, unlike the wave sets beside it: a collection is a few hundred bytes of JSON, and the
     * world's instance must hold its parameters from the first frame a script can run — a `WorldParameters.set`
     * in OnStart that found the collection still pending would be refused for a reason the author cannot see.
     * Held as the asset, so a save in the editor (which re-reads the asset) reaches the next Get.
     */
    class WorldParametersService
    {
    public:
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The collection's data as loaded now, or the reason it cannot be read. The pointer lives as long as
        /// the asset (the service holds it).
        Common::ResultStr<const Assets::Serialization::WorldParametersData*>
        Get( const Assets::AssetHandle& handle );

        void Clear();

    private:
        std::weak_ptr<Assets::AssetManager>                                                  m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::WorldParametersAsset>> m_Held;
        std::unordered_map<Assets::AssetHandle, std::string>                                 m_Failed;
    };
} // namespace Desert::Runtime
