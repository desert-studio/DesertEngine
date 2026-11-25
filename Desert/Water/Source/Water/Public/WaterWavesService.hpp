#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/Common.hpp>
#include <Water/Public/FFTOceanSpectrum.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Declared, not included: the generated set is shared, not looked into here, and the asset is only named -- so a
// caller of Get() does not compile the Gerstner generator (BUILD-MOD-1).
namespace Desert::Water
{
    struct GerstnerWave;          // Water/Public/GerstnerWaterWaves.hpp
    struct GerstnerWaveGenerator; // Water/Public/GerstnerWaterWaves.hpp
}

namespace Desert::Assets
{
    class WaterWavesAsset; // Water/Public/WaterWavesAsset.hpp
}

namespace Desert::Runtime
{
    /// What a wave set is, as far as it has been read. A set whose LATER re-read failed keeps the kind (and the
    /// waves) it had; Failed is a set that has never been read.
    enum class WaveSetKind : uint8_t
    {
        Pending,
        Gerstner,
        FFT,
        Failed
    };

    /**
     * @brief The `.dwaves` wave sets water bodies name (WATER-W2).
     *
     * FractureService's shape: the first ask creates the asset from its registry row and requests the read
     * through the AsyncAssetLoader; until it lands the wave set is pending. When it lands its generator is run
     * ONCE (UE UGerstnerWaterWaves::RecomputeWaves on load), and every body naming the set shares the result.
     *
     * A cache of assets only, read on the main thread: which of them a world draws, and how, is the world's own
     * record (ECS::WaterWaveSources), the service's one caller.
     */
    class WaterWavesService
    {
    public:
        using Waves    = std::shared_ptr<const std::vector<Water::GerstnerWave>>;
        using Spectrum = std::shared_ptr<const Water::FFTOceanSpectrum>;

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The generated waves when the set is read; success with null while pending; an error, said once,
        /// when it fails. The waves run with the set's wind (SetWind; +X in still air): a turned wind gives a
        /// new pointer, so the renderer re-uploads and the physics re-keys, both on the same waves.
        Common::ResultStr<Waves> Get( const Assets::AssetHandle& handle );

        /// WATER-RIVERS-FFT: the set's FFT ocean (every cascade's initial spectrum, built ONCE per load from its
        /// FFT block and seed) when its Source is FFT; success with null while pending or for a Gerstner set; the
        /// set's error when it failed.
        Common::ResultStr<Spectrum> GetSpectrum( const Assets::AssetHandle& handle );

        /// What the set is (starts its read).
        [[nodiscard]] WaveSetKind KindOf( const Assets::AssetHandle& handle );

        /// Raised whenever a set's kind or waves may have changed: a read landed, failed or was re-read, the sets
        /// were cleared.
        [[nodiscard]] uint64_t ShapeSerial();

        /// WATER-UNDERWATER: THE WIND a set's ocean is raised by - the scene's ECS::WindAt at the first visible
        /// body naming the set (WaterWaveSources, every frame), horizontal, cm/s. Quantised (10 cm/s, 0.5 deg)
        /// so a steady wind never rebuilds; a set no body states a wind for is a flat sea. GetSpectrum rebuilds
        /// the set's h0 when it changes (a new pointer: the renderer re-uploads, the CPU mirror re-keys).
        void SetWind( const Assets::AssetHandle& handle, const glm::vec2& velocityXZ );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );
        void Adopt( const Assets::AssetHandle& handle, const Assets::Asset<Assets::WaterWavesAsset>& asset );
        /// A failed read: said, and the set's error unless it already has waves.
        void Fail( const Assets::AssetHandle& handle, const std::string& why );
        /// Reads the set through the registry row (asynchronously), whatever the service already holds of it.
        void Read( const Assets::AssetHandle& handle );
        /// THE SAVED `.dwaves` (UE's asset reload on change): the sets ContentRegistry says were written since
        /// the last look (AssetHotReload's probe -> ContentRegistry::Update) are re-read and re-adopted, so a
        /// saved edit is on the water the next frame, without a restart. A re-read that fails, or whose waves are
        /// refused, is said and the set keeps the waves it had.
        void FollowWrites();
        /// The wind angle (degrees from +X toward +Z) the set's waves run with: SetWind's, 0 in still air.
        [[nodiscard]] float WindAngleOf( const Assets::AssetHandle& handle ) const;

        // A set's waves as last generated, and the wind angle they were generated for.
        struct BuiltWaves
        {
            Waves Set;
            float AngleDeg = 0.0f;
        };

        std::weak_ptr<Assets::AssetManager>                          m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest> m_Requests;
        std::unordered_map<Assets::AssetHandle, std::shared_ptr<const Water::GerstnerWaveGenerator>> m_Generators;
        std::unordered_map<Assets::AssetHandle, BuiltWaves>                                          m_Ready;
        uint64_t m_SeenWrites = 0; // ContentRegistry::WriteSerial at the last FollowWrites
        std::unordered_map<Assets::AssetHandle, Spectrum>            m_Spectra; // FFT sets only
        // FFT sets only.
        std::unordered_map<Assets::AssetHandle, Water::FFTOceanSpectrumParams> m_Shapes;
        std::unordered_map<Assets::AssetHandle, Water::FFTOceanWind>           m_Winds;
        std::unordered_map<Assets::AssetHandle, std::string>         m_Failed;
        uint64_t m_ShapeSerial = 1;
    };

    // The `.dwaves` wave sets water bodies name (WATER-W2): read on demand, each generated into its Gerstner waves
    // once per load and shared by every body naming it. Bound and cleared with the engine's services through the
    // Water module's hooks (Private/WaterModule.cpp).
    WaterWavesService* GetWaterWavesService();
} // namespace Desert::Runtime
