#pragma once

// THE FFT OCEAN'S SPECTRUM (WATER-RIVERS-FFT): Tessendorf's statistical ocean ("Simulating Ocean Water", 2001,
// sec. 3.3-3.4) over a JONSWAP spectrum (Hasselmann et al. 1973) with a Mitsuyasu cos^2s directional spread, split
// into THREE CASCADES of 256 x 256 (the Atlas / "Wakes, Explosions and Lighting" (GDC 2019) and Jump Trajectory
// FFT-ocean layout): three square patches of non-commensurate sizes, each owning one band of wave numbers, so the
// sum repeats only past the least common multiple of the three - never inside a view.
//
// THIS FILE IS THE CPU HALF: the initial spectrum h0(k) of every cascade, a pure function of the `.dwaves`
// parameters and its seed (the same seed gives the same ocean on every machine - the random numbers are this
// file's own SplitMix64 + Box-Muller, never <random>'s implementation-defined distributions). It is built once per
// wave-set load (Runtime::WaterWavesService) and uploaded once; the per-frame time evolution h(k, t), the inverse
// FFTs and the displacement / slope / Jacobian finalize run on the GPU
// (Shaders/Programs/Water/WaterFFT*.shader, Graphic/Systems/Scene/Water/WaterFFTRenderer).
//
// Units: the project's centimetre. Wave numbers are radians per centimetre, gravity is 980 cm/s^2 (as
// Common/GerstnerWave.glslh), amplitudes are centimetres. Axes: the plane is (world X, world Z) relative to the
// body's WaveOrigin, Y up.

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Desert::Water
{
    /// The grid of one cascade (texels per side). A power of two: the GPU's radix-2 IFFT runs log2 of it stages
    /// in one workgroup's shared memory (WaterFFT.shader WATER_FFT_SIZE, the same number).
    inline constexpr uint32_t kFFTOceanResolution     = 256;
    inline constexpr uint32_t kFFTOceanResolutionLog2 = 8;
    static_assert( ( 1u << kFFTOceanResolutionLog2 ) == kFFTOceanResolution );

    /// The cascades (layers of the GPU arrays; WaterMeshTiles.glslh WATER_FFT_CASCADES).
    inline constexpr uint32_t kFFTOceanCascades = 3;

    /// What a wave set's surface IS (`.dwaves` Source). A Gerstner set is its generator's sum everywhere; an FFT
    /// set is its Tessendorf ocean where the tier allows (Scalability Effects.WaterWaveMethod = FFT) and its
    /// Gerstner generator - the same file's - below that tier and for the CPU's queries (REMAINDER: the GPU
    /// cascades read back for buoyancy).
    enum class WaterWaveSource
    {
        Gerstner,
        FFT,
    };

    /// THE WIND THE OCEAN IS RAISED BY (WATER-UNDERWATER): not authored in the wave set. The scene's one wind
    /// (ECS::WindAt over the WindSource entities, ECS/System/WindField.hpp - the same query foliage, clouds, cloth
    /// and groom ask) at the body's wave origin, horizontal; Runtime::WaterWavesService rebuilds a set's spectrum
    /// when it changes. The `.dwaves` FFT block carries only the spectrum's SHAPE (fetch, peak, spread, cascades).
    struct FFTOceanWind
    {
        /// Wind speed 10 m above the sea (U10), cm/s. 0 = still air: a flat sea (no wind sea is raised).
        float Speed = 0.0f;
        /// The wind's heading, degrees from +X toward +Z.
        float AngleDeg = 0.0f;

        [[nodiscard]] bool operator==( const FFTOceanWind& ) const = default;
    };

    /// The authored ocean's shape (one `.dwaves` FFT block). Defaults: 300 km of fetch, the standard JONSWAP
    /// peak, patches of 250 m / 61 m / 17.3 m (ratios ~4.1 and ~3.5: no common tile short of kilometres).
    struct FFTOceanSpectrumParams
    {
        /// The distance the wind has blown over open water, km (JONSWAP fetch): short fetch = young, choppy sea.
        float FetchKm = 300.0f;
        /// JONSWAP peak enhancement gamma (3.3 standard; 1 = Pierson-Moskowitz).
        float PeakEnhancement = 3.3f;
        /// Mitsuyasu spread exponent s of cos^(2s)(theta / 2): larger = waves more aligned with the wind.
        float DirectionalSpread = 8.0f;
        /// Share [0, 1] of the waves travelling against the wind that is kept (0 = none: a pure wind sea).
        float CounterWaves = 0.1f;
        /// The horizontal (choppy) displacement factor lambda of Tessendorf 3.4; 0 = height field only.
        float Choppiness = 1.0f;
        /// A multiplier of every amplitude (an art knob over the physical spectrum).
        float HeightScale = 1.0f;
        /// Waves shorter than this are left to the material's ripple maps, cm. At least the smallest cascade's
        /// Nyquist wavelength 2 * CascadeSizes[2] / 256 (13.5 cm for the default 1730 cm patch): the default of
        /// 4 cm was below it, so every wave set - authored FFT block or not - was refused (WATER-UNDERWATER).
        float ShortestWavelength = 14.0f;
        // THE WHITECAPS' LIFETIME is not here (WATER-UNDERWATER pass 4): it is the body material's
        // FoamDecaySeconds for the FFT foam history (WaterRenderer::PrepareFFTFrame) and the Gerstner foam alike.
        /// The patch side of each cascade, cm, largest first.
        std::array<float, kFFTOceanCascades> CascadeSizes{ 25000.0f, 6100.0f, 1730.0f };
        int32_t                              Seed = 0;

        [[nodiscard]] bool operator==( const FFTOceanSpectrumParams& ) const = default;
    };

    /// One cascade's initial spectrum: kFFTOceanResolution^2 texels, row-major over (kz, kx), the zero wave number
    /// at (N/2, N/2). Each texel: (h0(k).re, h0(k).im, conj(h0(-k)).re, conj(h0(-k)).im), cm.
    struct FFTOceanCascade
    {
        float                  PatchSize = 0.0f; // cm
        float                  BandLow   = 0.0f; // |k| this cascade starts at, rad/cm (inclusive)
        float                  BandHigh  = 0.0f; // |k| the next cascade starts at, rad/cm (exclusive)
        std::vector<glm::vec4> H0;
    };

    struct FFTOceanSpectrum
    {
        FFTOceanSpectrumParams                         Params;
        FFTOceanWind                                   Wind; // the wind it was raised by
        std::array<FFTOceanCascade, kFFTOceanCascades> Cascades;
        /// An upper bound of the surface's vertical reach (cm): 3 standard deviations of the summed height, the
        /// quadtree's cull extent (the FFT twin of MaxWaveHeight).
        float MaxWaveHeight = 0.0f;
    };

    /// Refuses what the spectrum cannot honour, naming the field: a non-finite or non-positive fetch, cascade
    /// size; cascades not strictly decreasing; gamma below 1; negative spread, choppiness, height scale or
    /// shortest wavelength; a shortest wavelength the smallest cascade cannot resolve; CounterWaves outside
    /// [0, 1]. Empty = valid.
    [[nodiscard]] const char* FFTOceanSpectrumParamsProblem( const FFTOceanSpectrumParams& params ) noexcept;

    /// The JONSWAP directional variance density E(kx, kz) of a wave number (cm^2 per (rad/cm)^2) under @p wind:
    /// what h0 draws its amplitude from. Zero at k = 0, outside [2 pi / patch, 2 pi / ShortestWavelength], and
    /// everywhere in still (or non-finite) air.
    [[nodiscard]] float FFTOceanSpectrumDensity( const FFTOceanSpectrumParams& params, const FFTOceanWind& wind,
                                                 float kx, float kz ) noexcept;

    /// Every cascade's h0 for valid @p params (FFTOceanSpectrumParamsProblem == nullptr) under @p wind.
    /// Deterministic in Seed: the same wind gives the same sea, and a wind change rescales the same random phases.
    [[nodiscard]] FFTOceanSpectrum BuildFFTOceanSpectrum( const FFTOceanSpectrumParams& params,
                                                          const FFTOceanWind&           wind ) noexcept;
} // namespace Desert::Water
