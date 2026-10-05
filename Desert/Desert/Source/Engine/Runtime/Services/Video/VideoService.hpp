#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Media
{
    class MediaPlayer;
    class MediaTexture;
} // namespace Desert::Media

namespace Desert::Runtime
{
    // One open video: its MediaPlayer (WebM: AV1 + Opus) looping, and the MediaTexture whose STABLE
    // Image2D the UI walk samples — the YUV->RGB conversion happens on the GPU, in place, each new frame.
    struct VideoPlayback
    {
        std::unique_ptr<Media::MediaPlayer>   Player;
        std::unique_ptr<Media::MediaTexture>  Texture;
        bool                                  Valid = false; // false => open failed (negative cache)
        std::chrono::steady_clock::time_point Last{};        // wall-clock of the previous UpdateAll advance

        VideoPlayback();
        ~VideoPlayback();
        VideoPlayback( VideoPlayback&& ) noexcept;
        VideoPlayback& operator=( VideoPlayback&& ) noexcept;
    };

    // Streams WebM videos as UI content. A video opens once on first Resolve (player + media texture);
    // UpdateAll() advances every open player by real elapsed wall-clock time and converts its newest frame.
    // UpdateAll runs in the host update step (outside the render pass) so the render walk only ever samples
    // an already-updated texture. Playback loops (MediaPlayer::SetLooping). The UI path plays the picture
    // only: no audio sink is attached, so the player's clock is its own.
    class VideoService
    {
    public:
        ~VideoService();

        // --- Videos as ASSETS (handle-referenced, mirrors FontService) ----------------------------------
        // UI references a video by an AssetHandle, never a raw path: the user drags a .webm from the Content
        // Browser (RegisterVideo on drop) and the (de)serializer round-trips it as a path through the shared
        // AssetResolver. The handle is AssetHandle::FromKey(path) — deterministic & path-derived, so the same
        // file always maps to the same handle and a saved scene resolves without an import step.

        // Record handle=FromKey(path) -> path and return the handle (idempotent). "" -> 0.
        uint64_t RegisterVideo( const std::string& path );

        // Reverse lookup for display / serialization. "" if unknown.
        std::string PathForHandle( uint64_t handle ) const;

        // GPU image for `handle`'s current video frame, opening the file on first use. nullptr when the
        // handle is unregistered / the file can't be opened (negatively cached so the check stays cheap).
        Graphic::Image2D* Resolve( uint64_t handle );

        // Advance every open video by real elapsed time and convert its newest frame. Call once per frame
        // from the host update loop (runtime layer + editor).
        void UpdateAll();

        void Clear();

    private:
        VideoPlayback* GetOrOpen( const std::string& path );

        std::unordered_map<std::string, VideoPlayback> m_Videos; // open decoders keyed by path
    };
} // namespace Desert::Runtime
