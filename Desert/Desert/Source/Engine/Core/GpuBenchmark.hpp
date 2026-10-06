#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/RecommendedQuality.hpp>
#include <Engine/Core/Device.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Engine
{
    /**
     * @brief The recommended-settings benchmark's GPU half (SCAL1; UE FSynthBenchmark's GPU part).
     *
     * Runs a fixed set of synthetic passes (ALU, memory bandwidth: VulkanGpuBenchmark.cpp) on the
     * device the game will render on, timing each with timestamps, and returns a perf index normalised to the
     * reference GPU. At first launch (or when RecommendedQuality's cache key no longer matches) the host runs it
     * before the first level loads, then calls QualityState::ApplyRecommended with RecommendLevels' answer — only
     * when machine.json holds no QualitySelection the player made; a player's choice is never overwritten.
     *
     * Interface, not a class with a body: the Vulkan backend implements it (Graphic/API/Vulkan); tests mock it.
     * When the catalog's Timing is not AnyStage it does NOT run passes: it returns Timed = false with the device
     * class and VRAM (Mac: MoltenVK timestamps only at encoder bounds), and RecommendLevels takes the device-class
     * fallback. A run that fails (device lost, pipeline refused) is an error, not a zero index.
     */
    class GpuBenchmark
    {
    public:
        virtual ~GpuBenchmark() = default;

        [[nodiscard]] virtual Common::ResultStr<Common::Scalability::BenchmarkResult> Run( Device& device ) = 0;

        // The backend's implementation for the active device.
        [[nodiscard]] static std::unique_ptr<GpuBenchmark> Create();
    };

    // The cache key of the device the process created — pass device.GetCapabilities() (identity + the driver
    // version) — with the table's version.
    [[nodiscard]] Common::Scalability::BenchmarkCacheKey MakeBenchmarkCacheKey( const DeviceCapabilities& caps,
                                                                                uint32_t tableVersion );
} // namespace Desert::Engine
