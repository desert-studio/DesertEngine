#pragma once

// THE FFT OCEAN'S CPU MIRROR (WATER-RIVERS-FFT pass 7; UE Water: what floats reads the waves the GPU draws, UE
// UWaterBodyComponent::GetWaterSurfaceInfo over its CPU wave mirror): the GPU's displacement cascades read back
// asynchronously (WaterRenderer::AddWavePasses writes kFFTReadbackSize^2 texels per cascade into a host-readable
// buffer, read MaxFramesInFlight frames later - no GPU wait), the last two reads kept, and the surface between
// and past them EXTRAPOLATED in time per texel. No second FFT on the CPU (Tessendorf 2001's spectrum is evaluated
// once, on the GPU).
//
// The renderer publishes, the physics step reads: FFTSurfaceMirror is the one thread-safe hand-over
// (Runtime::WaterWavesService owns it, as it owns the spectra the snapshots are keyed by).

#include <Engine/Water/FFTOceanSpectrum.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Desert::Water
{
    /// Texels per side per cascade the GPU reads back (every kFFTReadbackStride-th texel of the 256^2 cascade:
    /// 64^2 x 3 x 16 B = 192 KB a frame). The C++ twin of WaterFFTFinalize.shader WATER_FFT_READBACK_SIZE.
    inline constexpr uint32_t kFFTReadbackSize   = 64;
    inline constexpr uint32_t kFFTReadbackStride = kFFTOceanResolution / kFFTReadbackSize;
    static_assert( kFFTReadbackStride * kFFTReadbackSize == kFFTOceanResolution );
    /// vec4 texels in one read: cascade-major, then row-major over (z, x).
    inline constexpr uint32_t kFFTReadbackTexels = kFFTOceanCascades * kFFTReadbackSize * kFFTReadbackSize;

    /// The farthest the mirror extrapolates past its newest read, s: past it the surface is held (a stalled
    /// renderer must not fling what floats with a velocity from seconds ago).
    inline constexpr double kFFTMirrorMaxExtrapolationSeconds = 0.25;

    /// One read of the cascades: xyz = the displacement (lambda Dx, Dy, lambda Dz) at the wave time Time, cm.
    struct FFTSurfaceRead
    {
        double                 Time = 0.0;
        std::vector<glm::vec4> Texels; // kFFTReadbackTexels, or empty (no read)
    };

    /// The mirror of one FFT ocean: its spectrum (the identity a body's wave set names) and its last two reads.
    struct FFTSurfaceSnapshot
    {
        std::shared_ptr<const FFTOceanSpectrum> Spectrum;
        FFTSurfaceRead                          Previous; // empty until a second read landed
        FFTSurfaceRead                          Current;
    };

    struct FFTSurfaceSample
    {
        float     Height = 0.0f;              // above the still plane, cm
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f }; // unit
    };

    /// The FFT surface over the plane point @p plane (relative to the body's WaveOrigin, cm) at wave time @p time:
    /// the choppy displacement inverted (the still point whose displaced position is @p plane, a fixed-point
    /// iteration), the cascades summed, the normal from the displaced surface's central differences. Nullopt when
    /// the snapshot holds no read.
    [[nodiscard]] std::optional<FFTSurfaceSample> FFTSurfaceAt( const FFTSurfaceSnapshot& snapshot,
                                                                glm::vec2 plane, double time ) noexcept;

    /// The renderer-to-physics hand-over: the newest snapshot of the frame's FFT ocean. Publish adopts @p read as
    /// the newest read of @p spectrum's ocean (the previous newest becomes Previous; another spectrum starts
    /// over); Latest answers only for the spectrum a body names.
    class FFTSurfaceMirror
    {
    public:
        void Publish( const std::shared_ptr<const FFTOceanSpectrum>& spectrum, FFTSurfaceRead read ) noexcept;

        [[nodiscard]] std::shared_ptr<const FFTSurfaceSnapshot>
        Latest( const FFTOceanSpectrum* spectrum ) const noexcept;

        void Clear() noexcept;

    private:
        mutable std::mutex                        m_Mutex;
        std::shared_ptr<const FFTSurfaceSnapshot> m_Snapshot; // WaterRenderer draws one FFT ocean a frame
    };
} // namespace Desert::Water
