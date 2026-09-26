#pragma once

// DECODE A CACHED THUMBNAIL ON A WORKER, BEFORE ANYONE ASKS TO DRAW IT.
//
// ThumbnailCache::Get used to do the whole cache-hit path on the main thread inside the ImGui pass: read the
// PNG, stb-decode it, box-filter it, upload it. The first two are pure CPU and file I/O and need no device,
// so the Content Browser hands the pictures of the folder it is about to show to this store, a JobSystem
// worker does the decode, and the main thread is left with the upload only. That is how UE's browser treats
// a thumbnail it already has on disk: loading it is never the frame's work.
//
// NOT A CAPTURE. Nothing here renders or asks for a renderer slot, which is why it runs while the splash is
// still up (Splash::ThumbnailDiskDecodeAllowed) and the capture queue does not (Splash::ThumbnailCaptureAllowed).
//
// Deliberately free of ThumbnailService and of the device, so Desert/Tests/Editor/ThumbnailPrefetch can link it.

#include <cstddef>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor
{
    /// RGBA8 pixels of one decoded thumbnail, already reduced to at most ThumbnailPixels::kMaxDim a side.
    struct ThumbnailPixels
    {
        // The largest texture a thumbnail is ever uploaded at, and — since v9 — also the size the PNG is
        // written at (AssetThumbnailRenderer::kSize). The two are deliberately equal: ThumbnailCache is the
        // only reader of those files, so any pixel above this cap is decoded and box-averaged away on
        // every load and has never reached the screen.
        //
        // 512 rather than the 256 it was, and the arithmetic is worth writing down because it is the whole
        // argument. The asset grid draws a thumbnail at m_GridSize * 0.66, and m_MaxGridSize is 400, so the
        // largest card is 264 ImGui units; the browser's preview pane asks for 140. The window this was
        // measured on is 2056x1261 units against a 4112x2522 framebuffer — a scale of 2 — so those are 528
        // and 280 PHYSICAL pixels. At 256 the cap sat below both: the same "looks like 240p" complaint v3
        // was raised for, arriving again one level down where nobody was looking for it.
        static constexpr int kMaxDim = 512;

        int                        SourceWidth  = 0;
        int                        SourceHeight = 0;
        int                        Width        = 0;
        int                        Height       = 0;
        std::vector<unsigned char> Rgba;
        double                     DecodeMs = 0.0; ///< stb decode + box filter, on whichever thread ran it

        /// Decode `path` and box-average it down to kMaxDim. The ONE implementation: ThumbnailCache::Get uses
        /// it synchronously on a prefetch miss, the prefetch jobs use it on a worker. Empty on failure.
        [[nodiscard]] static std::optional<ThumbnailPixels> Decode( const std::string& path );
    };

    class ThumbnailPrefetch
    {
    public:
        /// One picture a browser tile will ask ThumbnailCache::Get for.
        struct Item
        {
            std::string Picture; ///< the file Get() will be called with (a cached PNG, or a source image)
            /// For a GENERATED thumbnail, the asset it depicts: the worker decodes the PNG only when
            /// ThumbnailFreshness says it is the picture to show. Empty for a user's own image, which is its
            /// own picture and is never stale.
            std::string FreshnessSource;
        };

        static constexpr std::size_t kMaxInFlight = 4;   ///< jobs at once: leave the pool to the settle
        static constexpr std::size_t kMaxReady    = 512; ///< the same bound as ThumbnailCache

        static ThumbnailPrefetch& Get();

        /// The folder on screen changed: replace what is still waiting with these (what is already decoded
        /// stays until taken or evicted). Main thread.
        void Request( std::vector<Item> items );

        /// Collect finished decodes and start the next ones, at most kMaxInFlight at a time. Main thread,
        /// once per frame — splash or not.
        void Tick();

        /// Hand over the decoded pixels for `picture` if a worker finished them from the file as it is now
        /// (`stamp` = its last_write_time); the entry leaves the store either way it matched. Main thread.
        [[nodiscard]] std::optional<ThumbnailPixels> Take( const std::string&              picture,
                                                           std::filesystem::file_time_type stamp );

        /// Everything is dispatched and collected.
        [[nodiscard]] bool Idle() const
        {
            return m_Waiting.empty() && m_InFlight.empty();
        }
        [[nodiscard]] std::size_t ReadyCount() const
        {
            return m_Ready.size();
        }

        /// Block until every started and waiting decode is done (teardown and tests only).
        void Drain();
        void Clear();

    private:
        struct Decoded
        {
            std::filesystem::file_time_type Stamp;
            std::optional<ThumbnailPixels>  Pixels; ///< empty: stale, missing or undecodable — Get decides
        };
        struct InFlight
        {
            std::string          Picture;
            std::future<Decoded> Result;
        };

        static Decoded Run( const Item& item );

        std::vector<Item>                        m_Waiting;
        std::vector<InFlight>                    m_InFlight;
        std::unordered_map<std::string, Decoded> m_Ready;
    };
} // namespace Desert::Editor
