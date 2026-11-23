#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    // THE CONVOLUTION BLOOM'S FFT SIZE (UE PostProcessFFTBloom: the FFT buffer is the bloom input plus a zero
    // border of BloomConvolutionBufferScale, rounded up to a power of two). One answer for the two that need it:
    // BloomRenderer::PrepareConvolution, which allocates the spectra at this size, and the view's memory census
    // (ViewMemory.hpp ViewTargetCensus), which lists them. A header without a renderer so the census can include
    // it.

    // The longest FFT line Common/BloomFFT.glslh's group-shared buffer holds (kBloomFFTMaxLength there).
    inline constexpr uint32_t kBloomFFTMaxLength = 1024;
    // UE BloomConvolutionBufferScale's default: the zero border, as a fraction of the content's larger side.
    inline constexpr float kBloomConvolutionBufferScale = 0.133f;

    struct BloomFFTSize
    {
        uint32_t Width  = 0;    // N.x, a power of two <= kBloomFFTMaxLength
        uint32_t Height = 0;    // N.y
        float    Pad    = 0.0f; // the zero border, input texels
    };

    // @p inputWidth x @p inputHeight = the bloom chain's mip 0 (half the view). Both spectra the convolution holds
    // - the frame's (a graph transient) and the kernel's (kept by BloomRenderer) - are (2 N.x) x N.y texels of
    // ViewTargetFormats::kBloomSpectrum: a complex pair per channel pair, packed two complex values per texel.
    [[nodiscard]] inline BloomFFTSize BloomConvolutionFFTSize( const uint32_t inputWidth,
                                                               const uint32_t inputHeight ) noexcept
    {
        const float bw     = static_cast<float>( inputWidth );
        const float bh     = static_cast<float>( inputHeight );
        const float pad    = std::ceil( kBloomConvolutionBufferScale * std::max( bw, bh ) );
        const auto  length = [pad]( const float content ) noexcept
        {
            return std::min( kBloomFFTMaxLength,
                             std::bit_ceil( std::max( 1u, static_cast<uint32_t>( content + pad ) ) ) );
        };
        return BloomFFTSize{ .Width = length( bw ), .Height = length( bh ), .Pad = pad };
    }
} // namespace Desert::Graphic
