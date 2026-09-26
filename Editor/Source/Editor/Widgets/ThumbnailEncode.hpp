#pragma once

#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// THE HALF OF A THUMBNAIL CAPTURE THAT NEEDS NO DEVICE, and the budget that paces the half that does.
//
// A capture used to cost ~215 ms on the main thread, measured by its own timestamps (TH3, Materials, 16
// captures): device idle 2-12, readback 80, downscale 23, PNG 106-110. Everything after the GPU copy is
// plain CPU work on bytes, so it lives here — free of Vulkan and of the renderer — and runs on a JobSystem
// worker; the suite drives it without a device.
namespace Desert::Editor::ThumbnailEncode
{
    /// Box-filters a square RGBA8 image of side @p from down to side @p to. Refused unless @p from is a
    /// whole multiple of @p to and the buffer holds exactly from*from*4 bytes.
    Common::ResultStr<std::vector<uint8_t>> Downscale( const std::vector<uint8_t>& rgba, uint32_t from, uint32_t to );

    /// Writes a square RGBA8 image to @p png: aside to "<png>.part" first, then renamed, so a reader never
    /// sees a half-written file. Creates the parent directory.
    Common::BoolResultStr WritePng( const std::vector<uint8_t>& rgba, uint32_t side, const std::string& png );

    /// MAIN-THREAD MILLISECONDS a capture may spend per frame, on average: a leaky bucket. Every Tick of the
    /// capture renderer charges what it cost; each frame repays kMainThreadMsPerFrame; a new capture is
    /// dispatched only while nothing is owed. One constant decides the pace: a folder of sixteen materials
    /// cannot stack sixteen captures' main-thread work into consecutive frames.
    class CaptureBudget
    {
    public:
        static constexpr double kMainThreadMsPerFrame = 2.0;

        void                 Spend( double ms ) { m_OwedMs += std::max( ms, 0.0 ); }
        void                 EndFrame() { m_OwedMs = std::max( m_OwedMs - kMainThreadMsPerFrame, 0.0 ); }
        [[nodiscard]] bool   MayDispatch() const { return m_OwedMs <= 0.0; }
        [[nodiscard]] double OwedMs() const { return m_OwedMs; }

    private:
        double m_OwedMs = 0.0;
    };
} // namespace Desert::Editor::ThumbnailEncode
