#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/WaterWavesAsset.hpp>
#include <Engine/Water/GerstnerWaterWaves.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Runtime
{
    /**
     * @brief The `.dwaves` wave sets water bodies name (WATER-W2).
     *
     * FractureService's shape: the first ask creates the asset from its registry row and requests the read
     * through the AsyncAssetLoader; until it lands the wave set is pending. When it lands its generator is run
     * ONCE (UE UGerstnerWaterWaves::RecomputeWaves on load), and every body naming the set shares the result.
     */
    class WaterWavesService
    {
    public:
        using Waves = std::shared_ptr<const std::vector<Water::GerstnerWave>>;

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The generated waves when the set is read; success with null while pending; an error, said once,
        /// when it fails.
        Common::ResultStr<Waves> Get( const Assets::AssetHandle& handle );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );
        void Adopt( const Assets::AssetHandle& handle, const Assets::Asset<Assets::WaterWavesAsset>& asset );

        std::weak_ptr<Assets::AssetManager>                          m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest> m_Requests;
        std::unordered_map<Assets::AssetHandle, Waves>               m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>         m_Failed;
    };
} // namespace Desert::Runtime
