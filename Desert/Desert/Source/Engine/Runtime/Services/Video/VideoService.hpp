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
    class MediaAudioOutput;
    class MediaPlayer;
    class MediaTexture;
} // namespace Desert::Media

namespace Desert::Runtime
{
    // One open video: its MediaPlayer (WebM: AV1 + Opus) looping, and the MediaTexture whose STABLE
    // Image2D the UI walk samples — the YUV->RGB conversion happens on the GPU, in place, each new frame.
    struct VideoPlayback
    {
        // Declared before the player: the player holds a raw IMediaAudioSink*, so the sink must outlive it.
        std::unique_ptr<Media::MediaAudioOutput> Sound; // null only for a clip with no audio track / no device
        std::unique_ptr<Media::MediaPlayer>      Player;
        std::unique_ptr<Media::MediaTexture>     Texture;
        bool                                     Valid = false; // false => open failed (negative cache)
        std::chrono::steady_clock::time_point    Last{};        // wall-clock of the previous UpdateAll advance
        // The loudest unmuted request among the panels that drew this clip since the last UpdateAll (panels
        // share one player per file, like UE's MediaPlayer shared by several MediaSoundComponents). A clip
        // no panel drew this frame is silent; its picture and clock keep running.
        float RequestedVolume = 0.0f;

        VideoPlayback();
        ~VideoPlayback();
        VideoPlayback( VideoPlayback&& ) noexcept;
        VideoPlayback& operator=( VideoPlayback&& ) noexcept;
    };

    // Streams WebM videos as UI content. A video opens once on first Resolve (player + media texture);
    // UpdateAll() advances every open player by real elapsed wall-clock time and converts its newest frame.
    // UpdateAll runs in the host update step (outside the render pass) so the render walk only ever samples
    // an already-updated texture. Playback loops (MediaPlayer::SetLooping). A clip with an audio track
    // plays it through its own MediaAudioOutput (UE: MediaSoundComponent), whose consumed-sample count is
    // the player's master clock; each panel asks for its volume (UIPanelComponent::VideoVolume/VideoMuted)
    // when it draws. A machine without an audio device plays the picture only, and says so in the log.
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

        // What a panel that draws a clip asks of its sound. Muted is the explicit "picture only" switch.
        struct SoundRequest
        {
            float Volume = 1.0f;
            bool  Muted  = false;
        };

        // GPU image for `handle`'s current video frame, opening the file on first use. nullptr when the
        // handle is unregistered / the file can't be opened (negatively cached so the check stays cheap).
        // `sound` is this drawer's audio request, applied (loudest unmuted wins) at the next UpdateAll.
        Graphic::Image2D* Resolve( uint64_t handle, SoundRequest sound );

        // Advance every open video by real elapsed time, apply the volume its panels asked for, and convert
        // its newest frame. Call once per frame from the host update loop (runtime layer + editor).
        void UpdateAll();

        void Clear();

    private:
        VideoPlayback* GetOrOpen( const std::string& path );

        std::unordered_map<std::string, VideoPlayback> m_Videos; // open decoders keyed by path
    };
} // namespace Desert::Runtime
