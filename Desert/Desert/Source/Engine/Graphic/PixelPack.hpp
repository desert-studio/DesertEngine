#pragma once

#include <Common/Core/Math/Rounding.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief Device pixels -> the 8-bit RGBA a PNG writer wants. ONE implementation, for every readback.
     *
     * WHY IT IS ITS OWN FILE. There are two readbacks in this engine and they answer different questions:
     * an offscreen Image2D holds the SCENE (VulkanImage2D::ReadPixelsRGBA8), and a swapchain image holds
     * the scene AND the interface composited over it (VulkanSwapChain::ReadPresentedFrameRGBA8) — which is
     * the only one of the two that can photograph a menu, a panel or a dialog at all.
     *
     * The two arrive at different formats by different routes: the first from the engine's own ImageFormat,
     * the second from whatever VkFormat the surface negotiated. Writing the pack twice would put the
     * channel-swizzle and the float clamp in two places, and the failure that produces is not a crash — it
     * is a capture whose red and blue are the wrong way round, which reads as a rendering defect and gets
     * reported as one. So the two MAPPINGS live at their own call sites, where the format vocabulary is,
     * and the PACK lives here once.
     *
     * Nothing here includes Vulkan or the renderer, so the arithmetic that decides what a captured pixel
     * says is testable without a device.
     */

    /// How the device laid the bytes out. Deliberately not the engine's ImageFormat and not a VkFormat:
    /// it is the pack's own vocabulary, so neither caller's enum leaks into the other's.
    enum class PackedPixelSource
    {
        RGBA8,   ///< 4 bytes/pixel, already in the wanted order
        BGRA8,   ///< 4 bytes/pixel, red and blue exchanged (what every macOS surface negotiates)
        RGBA32F, ///< 16 bytes/pixel, linear floats that have to be clamped and quantised
    };

    /// Bytes ONE pixel occupies at the source. The size of a readback staging buffer is this times the
    /// pixel count, and getting it from the same enum the pack reads is what stops the two disagreeing.
    [[nodiscard]] constexpr uint32_t BytesPerPixel( PackedPixelSource source ) noexcept
    {
        return source == PackedPixelSource::RGBA32F ? 16u : 4u;
    }

    /**
     * @brief Pack @p pixels pixels of @p source, read from @p raw, into tightly packed 8-bit RGBA.
     *
     * Returns an EMPTY vector when @p raw is null or holds fewer bytes than @p pixels pixels of that
     * source require. A short buffer is the shape a failed readback has, and packing it anyway would
     * produce a plausible image of uninitialised memory — a picture that looks like evidence.
     *
     * @param rawBytes How many bytes @p raw actually holds. Passed rather than assumed, because that is
     *                 the whole of the check above.
     */
    [[nodiscard]] inline std::vector<uint8_t> PackToRGBA8( const uint8_t* raw, std::size_t rawBytes,
                                                           std::size_t pixels, PackedPixelSource source )
    {
        if ( raw == nullptr )
            return {};

        const std::size_t needed = pixels * BytesPerPixel( source );
        if ( rawBytes < needed )
            return {};

        std::vector<uint8_t> out( pixels * 4u );

        if ( source == PackedPixelSource::RGBA32F )
        {
            // memcpy rather than a reinterpret_cast of `raw`: a staging buffer is only guaranteed to be
            // byte-aligned, and reading a float through a misaligned pointer is undefined behaviour that
            // happens to work on x86 and does not on every target this engine builds for.
            for ( std::size_t i = 0; i < pixels * 4u; ++i )
            {
                float value = 0.0f;
                std::memcpy( &value, raw + i * sizeof( float ), sizeof( float ) );
                out[i] = Common::Math::QuantiseUnitToByte( value );
            }
            return out;
        }

        const bool bgra = ( source == PackedPixelSource::BGRA8 );
        for ( std::size_t i = 0; i < pixels; ++i )
        {
            const uint8_t c0 = raw[i * 4u + 0u];
            const uint8_t c1 = raw[i * 4u + 1u];
            const uint8_t c2 = raw[i * 4u + 2u];
            const uint8_t c3 = raw[i * 4u + 3u];

            out[i * 4u + 0u] = bgra ? c2 : c0;
            out[i * 4u + 1u] = c1;
            out[i * 4u + 2u] = bgra ? c0 : c2;
            out[i * 4u + 3u] = c3;
        }
        return out;
    }
} // namespace Desert::Graphic
