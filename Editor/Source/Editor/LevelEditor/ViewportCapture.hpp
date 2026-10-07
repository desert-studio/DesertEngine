#pragma once

// VIEWPORT CAPTURE (UE: FScreenshotRequest / HighResScreenshot).
//
// Every picture the editor writes out of itself: the resolved viewport as a PNG (`--shot`, every frame of a
// `--shot-sequence`, F9, the channel's `shot.viewport`), the whole window with its interface (`shot.window`,
// recorded while the frame is built and collected after its present), and `<project>/.thumbnail.png` for
// the launcher tile. A member of EditorLayer BY VALUE, declared after the SceneWorkspace whose active scene
// it reads.

#include <Engine/Desert.hpp>

#include <cstdint>
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

        // Writes `<project>/.thumbnail.png` — 512x288, centre-cropped to 16:9, the launcher tile's picture.
        // Called after a scene save and on a clean exit; the callers treat a failure as non-fatal.
        [[nodiscard]] Common::BoolResultStr WriteProjectThumbnail();

        // The two halves of a window capture: recorded into the frame being built (the swapchain image is
        // ours only between acquire and present), collected once that present went out. A refusal while
        // recording is carried to the collect half so it names the real reason.
        void               RecordWindowCapture();
        [[nodiscard]] bool WriteWindowPng( const std::string& path, std::string& outError );

    private:
        // The one readback: a capture cannot differ from a dump in flip, format or the device-idle wait.
        [[nodiscard]] Common::BoolResultStr ReadViewportRGBA8( std::vector<uint8_t>& outPixels, uint32_t& outWidth,
                                                               uint32_t& outHeight );

        SceneWorkspace& m_Workspace;
        std::string     m_CaptureError;
    };
} // namespace Desert::Editor
