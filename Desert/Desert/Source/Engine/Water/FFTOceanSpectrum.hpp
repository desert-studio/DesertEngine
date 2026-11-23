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
    enum class WaterWaveSource : uint8_t
    {
        Gerstner,
        FFT,
    };

    /// The authored ocean (one `.dwaves` FFT block). Defaults: a fresh 12 m/s breeze over 300 km of fetch, the
    /// standard JONSWAP peak, patches of 250 m / 61 m / 17.3 m (ratios ~4.1 and ~3.5: no common tile short of
    /// kilometres).
    struct FFTOceanSpectrumParams
    {
        /// Wind speed 10 m above the sea (U10), cm/s.
        float WindSpeed = 1200.0f;
        /// The dominant wind direction, degrees from +X toward +Z (as GerstnerWaveGenerator::WindAngleDeg).
        float WindAngleDeg = 0.0f;
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
        /// Waves shorter than this are left to the material's ripple maps, cm.
        float ShortestWavelength = 4.0f;
        /// THE WHITECAPS' LIFETIME, s: the FFT ocean's foam history (Programs/Water/WaterFFTFinalize.shader) keeps
        /// each cascade's compression and fades it e-fold per this many seconds, so the foam trails behind the crest
        /// that folded. The ocean's own, not the material's: the history is accumulated on the GPU before any
        /// material runs (a Gerstner body's foam walks its own past, Pass_SingleLayerWater WaterFoamAmount, with
        /// the material's FoamDecaySeconds).
        float FoamDecaySeconds = 3.0f;
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
        std::array<FFTOceanCascade, kFFTOceanCascades> Cascades;
        /// An upper bound of the surface's vertical reach (cm): 3 standard deviations of the summed height, the
        /// quadtree's cull extent (the FFT twin of MaxWaveHeight).
        float MaxWaveHeight = 0.0f;
    };

    /// Refuses what the spectrum cannot honour, naming the field: a non-finite or non-positive wind speed, fetch,
    /// cascade size; cascades not strictly decreasing; gamma below 1; negative spread, choppiness, height scale or
    /// shortest wavelength; CounterWaves outside [0, 1]. Empty = valid.
    [[nodiscard]] const char* FFTOceanSpectrumParamsProblem( const FFTOceanSpectrumParams& params ) noexcept;

    /// The JONSWAP directional variance density E(kx, kz) of a wave number (cm^2 per (rad/cm)^2): what h0 draws
    /// its amplitude from. Zero at k = 0 and outside [2 pi / patch, 2 pi / ShortestWavelength].
    [[nodiscard]] float FFTOceanSpectrumDensity( const FFTOceanSpectrumParams& params, float kx,
                                                 float kz ) noexcept;

    /// Every cascade's h0 for valid @p params (FFTOceanSpectrumParamsProblem == nullptr). Deterministic in Seed.
    [[nodiscard]] FFTOceanSpectrum BuildFFTOceanSpectrum( const FFTOceanSpectrumParams& params ) noexcept;
} // namespace Desert::Water
