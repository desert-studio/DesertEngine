#pragma once

// VIEWPORT CAPTURE (UE: FScreenshotRequest / HighResScreenshot).
//
// Every picture the editor writes out of itself: the resolved viewport as a PNG (`--shot`, every frame of a
// `--shot-sequence`, F9, the channel's `shot.viewport`), the HDR scene colour as a PFM (`--shot-hdr`), the whole
// window with its interface (`shot.window`, recorded while the frame is built and collected after its present),
// and `<project>/.thumbnail.png` for the launcher tile. A member of EditorLayer BY VALUE, declared after the
// SceneWorkspace whose active scene it reads.

#include "Editor/LevelEditor/ShotDirector.hpp" // ShotDirector::RecordedFrame (WriteShotPng)

#include <Engine/Graphic/SwapChain.hpp> // the shot's capture ticket (WindowPngWrite)

#include <chrono>
#include <cstdint>
#include <future>
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

        // Read the resolved viewport back off the GPU NOW and write it to @p path as a PNG, creating the parent
        // directory if it is missing. False on any failure, always with the reason logged. Only for a picture
        // that names no frame (F9, the control channel's `shot.viewport`): a capture frame is WriteShotPng.
        bool WriteViewportPng( const std::string& path );

        // A FRAME OF THE CAPTURE: writes @p frame's own pixels (the copy recorded in its submission,
        // RecordedFrame::Pixels) to @p path. Refused, with the reason logged, when the frame has no copy or the
        // copy has not completed - never a live read in its place.
        bool WriteShotPng( const std::string& path, const ShotDirector::RecordedFrame& frame );

        // `--shot-hdr`: view 0's HDR scene colour as the renderer held it IN @p frame (RecordedFrame::SceneColor,
        // copied in that frame's own submission beside its PNG; never a live read) - before eye adaptation,
        // tonemap, the sRGB encode and the 8-bit quantisation, in pre-exposed units (scene radiance x the frame's
        // PreExposure, which with auto exposure off is the manual exposure scale) - written to @p path as a
        // PFM (32-bit float RGB, little endian). What a measurement of radiance compares: a PNG's tonemap
        // shoulder and clamp bias the mean of a noisy frame low. False on any failure, with the reason logged.
        bool WriteSceneColorPfm( const std::string& path, const ShotDirector::RecordedFrame& frame );

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
        // A window shot whose pixels are collected and whose PNG is being encoded and written on a JobSystem
        // worker (UE FImageWriteQueue: a screenshot's encode never runs on the game thread). Encode yields the
        // write's refusal, empty when the file is on disk.
        struct WindowPngWrite
        {
            std::string                           Path;
            uint32_t                              Width     = 0;
            uint32_t                              Height    = 0;
            double                                JoinMs    = 0.0; // on the frame: the render task join
            double                                WaitMs    = 0.0; // on the frame: the device idle wait
            double                                CollectMs = 0.0; // on the frame: staging detach (read, pack: worker)
            // The capture's ticket on its swap chain: resolved by the worker, released by FinishWindowPng.
            std::shared_ptr<Graphic::SwapChain> Source;
            Graphic::CapturedFrameTicket        Ticket;
            std::chrono::steady_clock::time_point Started;
            std::future<std::string>              Encode;
        };
        // Collects @p frame's copy (its refusal, if the record half was refused) ON THE FRAME and hands its PNG
        // encode + write to a worker (@p out.Encode); the caller answers once that future is ready.
        [[nodiscard]] bool BeginWindowPng( const std::string& path, uint64_t frame, WindowPngWrite& out,
                                           std::string& outError );
        // The write's second half, on the game thread once @p write.Encode is ready: its log line (ms per step).
        // Returns the refusal, empty on success.
        [[nodiscard]] std::string FinishWindowPng( WindowPngWrite& write ) const;
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
