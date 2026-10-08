#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/FractureAsset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.dfrac` fractures destructible entities name, read on demand (DST-03b).
     *
     * FoliageTypeService's shape: the first ask creates the asset from its registry row and requests the read
     * through the AsyncAssetLoader; until it lands the fracture is pending. The service holds the ASSET, and
     * hands out its fracture as a pointer that keeps the asset alive, so a simulation never outlives its data.
     */
    class FractureService
    {
    public:
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );
        /// The manager destruction reads its assets through (the fractures, and the VFX data channels its events
        /// are published into - ECS/System/DestructionVFXEvents.hpp); null before a host bound one.
        [[nodiscard]] std::shared_ptr<Assets::AssetManager> LockAssetManager() const
        {
            return m_Assets.lock();
        }

        /// The fracture when it is read; success with null while pending; an error, said once, when it fails.
        Common::ResultStr<std::shared_ptr<const Destruction::FractureData>>
        Get( const Assets::AssetHandle& handle );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );

        std::weak_ptr<Assets::AssetManager>                                           m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>                  m_Requests;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::FractureAsset>> m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>                          m_Failed;
    };
} // namespace Desert::Runtime
