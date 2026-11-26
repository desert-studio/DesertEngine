#pragma once

#include <cstdint>
#include <string>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Runtime
{
    // THE UI'S VIDEO CLIPS AS ENGINE SEES THEM (UE: UMediaPlayer / UMediaTexture behind the MediaAssets module's
    // interfaces). The implementation is the Media module's VideoService (Services/Video/VideoService.hpp: one
    // MediaPlayer + MediaTexture + MediaAudioOutput per open WebM); the module's StartupModule hands its instance
    // to ResourceRegistry, which is how Engine code (the asset resolver, the UI canvas) reaches it without naming
    // the module.
    class IVideoService
    {
    public:
        virtual ~IVideoService() = default;

        // What a panel that draws a clip asks of its sound. Muted is the explicit "picture only" switch.
        struct SoundRequest
        {
            float Volume = 1.0f;
            bool  Muted  = false;
        };

        // Record handle=FromKey(path) -> path and return the handle (idempotent). "" -> 0.
        virtual uint64_t RegisterVideo( const std::string& path ) = 0;

        // Reverse lookup for display / serialization. "" if unknown.
        [[nodiscard]] virtual std::string PathForHandle( uint64_t handle ) const = 0;

        // GPU image for `handle`'s current video frame, opening the file on first use. nullptr when the handle is
        // unregistered / the file can't be opened (negatively cached so the check stays cheap). `sound` is this
        // drawer's audio request, applied (loudest unmuted wins) at the next UpdateAll.
        virtual Graphic::Image2D* Resolve( uint64_t handle, SoundRequest sound ) = 0;

        // Advance every open video by real elapsed time, apply the volume its panels asked for, and convert its
        // newest frame. Call once per frame from the host update loop (runtime layer + editor).
        virtual void UpdateAll() = 0;

        virtual void Clear() = 0;
    };
} // namespace Desert::Runtime
