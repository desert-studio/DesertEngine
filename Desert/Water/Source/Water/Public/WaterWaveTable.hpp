#pragma once

// THE WORLD'S WAVES THIS FRAME (UE: one GerstnerWaterWaves buffer per world, read by the water vertex factory and
// by UWaterBodyComponent's queries): the distinct (wave set, WaveOrigin) pairs its bodies name and what each is -
// one choice, read by the renderer's wave block, the underwater volume, the CPU queries, the shore and the falls.
// Built by ECS::WaterWaveSources; no ECS types here.

#include <Engine/Assets/Common.hpp>
#include <Water/Public/FFTSurfaceMirror.hpp>

#include <glm/vec2.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Desert::Water
{
    struct GerstnerWave; // Water/Public/GerstnerWaterWaves.hpp

    /// The wave lanes a frame draws. The C++ twin of Water/WaterMeshTiles.glslh WATER_MAX_WAVE_SETS.
    inline constexpr std::size_t kMaxWaveLanes = 4;

    enum class WaterWaveState : uint8_t
    {
        Pending, // not read yet, or which set is the world's ocean is not known yet: flat, the query not ready
        Gerstner,
        FFT
    };

    /// Whether this world's renderers can draw the FFT ocean: Unknown until the first one tried (and for ever in a
    /// world nobody draws), and the sea is then the set's Gerstner generator for everybody.
    /// One renderer's refusal is the world's, for as long as its surface lives (WaterFFTSurface::Report).
    enum class WaterFFTVerdict : uint8_t
    {
        Unknown,
        Drawable,
        Refused
    };

    /// The world's one FFT ocean as its renderers report it: whether they can draw it, and its read-backs.
    struct WaterFFTSurface
    {
        std::atomic<WaterFFTVerdict> Verdict{ WaterFFTVerdict::Unknown };
        FFTSurfaceMirror             Mirror;

        /// A renderer's answer. A refusal stands whoever answers after it, so the sea's waves do not depend on the
        /// order a world's renderers answer in.
        void Report( const bool drawable ) noexcept
        {
            if ( !drawable )
            {
                Verdict.store( WaterFFTVerdict::Refused, std::memory_order_release );
                return;
            }
            WaterFFTVerdict unknown = WaterFFTVerdict::Unknown;
            Verdict.compare_exchange_strong( unknown, WaterFFTVerdict::Drawable, std::memory_order_acq_rel,
                                             std::memory_order_acquire );
        }
    };

    struct WaterWaveLane
    {
        Assets::AssetHandle Asset = Assets::AssetHandle::Null();
        glm::vec2           Origin{ 0.0f }; // the WaveOrigin: the body's entity X, Z
        WaterWaveState      State = WaterWaveState::Pending;
        /// The set's Gerstner waves; null while Pending.
        std::shared_ptr<const std::vector<GerstnerWave>> Waves;
        /// The world's ocean under the FFT tier, whether or not it is drawn yet (State): what a renderer makes its
        /// pipelines and images for, and reports the verdict on. Null for a Gerstner lane.
        std::shared_ptr<const FFTOceanSpectrum> Spectrum;

        [[nodiscard]] bool operator==( const WaterWaveLane& other ) const
        {
            return Asset == other.Asset && Origin == other.Origin && State == other.State &&
                   Waves == other.Waves && Spectrum == other.Spectrum;
        }
    };

    /// Immutable: the zones' draws hold the one they were built with.
    struct WaterWaveTable
    {
        std::vector<WaterWaveLane>       Lanes; // at most kMaxWaveLanes
        std::shared_ptr<WaterFFTSurface> Surface;
    };
} // namespace Desert::Water
