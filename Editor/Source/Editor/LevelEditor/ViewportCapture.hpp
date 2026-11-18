#pragma once

// VIEWPORT CAPTURE (UE: FScreenshotRequest / HighResScreenshot).
//
// Every picture the editor writes out of itself: the resolved viewport as a PNG (`--shot`, every frame of a
// `--shot-sequence`, F9, the channel's `shot.viewport`), the HDR scene colour as a PFM (`--shot-hdr`), the whole
// window with its interface (`shot.window`, recorded while the frame is built and collected after its present),
// and `<project>/.thumbnail.png` for the launcher tile. A member of EditorLayer BY VALUE, declared after the
// SceneWorkspace whose active scene it reads.

#include <Engine/Desert.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Desert::Editor
{
    class SceneWorkspace;

    class ViewportCapture
    {
    public:
        explicit ViewportCapture( SceneWorkspace& workspace ) : m_Workspace( workspace )
        {
        }

        // Read the resolved viewport back off the GPU and write it to @p path as a PNG, creating the parent
        // directory if it is missing. False on any failure, always with the reason logged.
        bool WriteViewportPng( const std::string& path );

        // `--shot-hdr`: view 0's HDR scene colour as the renderer holds it - before eye adaptation, tonemap, the
        // sRGB encode and the 8-bit quantisation, in pre-exposed units (scene radiance x the frame's
        // PreExposure, which with auto exposure off is the manual exposure scale) - written to @p path as a
        // PFM (32-bit float RGB, little endian). What a measurement of radiance compares: a PNG's tonemap
        // shoulder and clamp bias the mean of a noisy frame low. False on any failure, with the reason logged.
        bool WriteSceneColorPfm( const std::string& path );

        // Writes `<project>/.thumbnail.png` — 512x288, centre-cropped to 16:9, the launcher tile's picture.
        // Called after a scene save and on a clean exit; the callers treat a failure as non-fatal.
        [[nodiscard]] Common::BoolResultStr WriteProjectThumbnail();

        // The two halves of a window capture: recorded into the frame being built (the swapchain image is
        // ours only between acquire and present), collected once that present went out. A refusal while
        // recording is carried to the collect half so it names the real reason.
        // RecordWindowCapture runs ON THE RENDER TASK, behind @p frame's interface (@p frame: the control
        // channel's serial of the frame being built). One copy at a time: while an earlier frame's copy waits
        // for its judgement, this frame records nothing and holds no capture.
        void RecordWindowCapture( uint64_t frame );
        // GAME THREAD, at @p frame's announcement: whether its record half ran - a copy is on its way, or its
        // refusal is kept for the collect half. False for a frame that recorded nothing, which therefore cannot
        // answer a `shot.window` (ControlService judges it with PendingWork::WindowCapture outstanding).
        [[nodiscard]] bool WindowCaptureRecordedFor( uint64_t frame ) const;
        // @p frame was judged and its copy is not the answer: released (after the device has finished the copy),
        // so the next frame can record. No-op when @p frame holds no copy, or it was already collected.
        void DiscardWindowCapture( uint64_t frame );
        // Collects @p frame's copy (its refusal, if the record half was refused) and writes it to @p path.
        [[nodiscard]] bool WriteWindowPng( const std::string& path, uint64_t frame, std::string& outError );
        // HDR-OUTPUT p3: after a window shot of an HDR output, the peak of the tonemap's HDR rendition of the same
        // frame, in paper-white units and nits, into editor.log (the PNG is the SDR rendition).
        void LogHdrRenditionPeak();

    private:
        // The one readback: a capture cannot differ from a dump in flip, format or the device-idle wait.
        [[nodiscard]] Common::BoolResultStr ReadViewportRGBA8( std::vector<uint8_t>& outPixels, uint32_t& outWidth,
                                                               uint32_t& outHeight );

        // What the record half left, written on the render task and read on the game thread at the frame's
        // announcement (with GameRenderPipelining the next frame's record may run meanwhile): under the lock.
        struct WindowCaptureRecord
        {
            uint64_t    Frame    = UINT64_MAX; ///< the serial it was recorded for; UINT64_MAX: none yet
            bool        InFlight = false;      ///< a copy is in the swapchain's staging, not yet collected
            std::string Error;                 ///< the record half's refusal; empty when it recorded
        };
        [[nodiscard]] WindowCaptureRecord WindowRecord() const;

        SceneWorkspace&     m_Workspace;
        mutable std::mutex  m_WindowRecordLock;
        WindowCaptureRecord m_WindowRecord;
    };
} // namespace Desert::Editor
