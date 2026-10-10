#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.dfx` systems VFX components name (VFX-HOST).
     *
     * WaterWavesService's shape: the first ask creates the asset from its registry row and requests the read
     * through the AsyncAssetLoader; until it lands the system is pending (null). The data is copied out once, so
     * the asset eviction sweep may drop the asset while every component naming the system shares the copy. A new
     * copy (a reload) is a new pointer: consumers key what they built from it by that pointer.
     */
    class VFXSystemService
    {
    public:
        using System = std::shared_ptr<const Assets::Serialization::VFXSystemData>;

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The system @p handle names: null while it is being read; an error once it cannot be read.
        Common::ResultStr<System> Get( const Assets::AssetHandle& handle );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );
        void Adopt( const Assets::AssetHandle& handle, const Assets::Asset<Assets::VFXSystemAsset>& asset );

        std::weak_ptr<Assets::AssetManager>                          m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest> m_Requests;
        std::unordered_map<Assets::AssetHandle, System>              m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>         m_Failed;
    };
} // namespace Desert::Runtime
