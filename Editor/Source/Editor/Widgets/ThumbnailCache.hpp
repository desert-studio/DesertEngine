#pragma once

#include <filesystem>

#include <Engine/Graphic/Image.hpp>

#include <Common/Utilities/WriteWatch.hpp>
#include <Editor/Widgets/ThumbnailOutdated.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Editor
{
    // Decodes image files (any png/tga/jpg/hdr the asset browser shows) into small GPU textures for
    // thumbnails — independent of the cook pipeline, so EVERY image previews consistently (not only
    // already-cooked ones). Keyed by the picture's path (ThumbnailKey::DiskPath for a rendered one).
    //
    // RESIDENT, NOT BOUNDED (THM1n-13, owner 09-29 "all assets on the splash"): the browser's cache holds every
    // picture of the project from the hand-over on — the splash uploads them all — and never drops one to make
    // room, so entering a folder draws what is here instead of decoding it again. A picture leaves only when
    // its file changes (WriteWatch, below) or its owner is torn down. Unloading for very large projects is a
    // separate owner decision, not a silent cap here.
    class ThumbnailCache
    {
    public:
        // Registers/unregisters with the live set that ReleaseAll() walks. Not copyable or movable: the set
        // holds raw pointers, and a copy would put an address in it that nothing owns.
        ThumbnailCache();
        ~ThumbnailCache();

        ThumbnailCache( const ThumbnailCache& )            = delete;
        ThumbnailCache& operator=( const ThumbnailCache& ) = delete;

        // Returns a cached thumbnail image for the source path (decoding + downscaling on first request),
        // or null if the file can't be decoded (caller falls back to an icon). Null results are cached too.
        // A file that changed since it was decoded (Common::Utils::WriteWatch) is decoded again: a reader may keep
        // drawing an outdated rendered PNG while its replacement is captured (ThumbnailFreshness::Choose), and the
        // new file must reach the screen without every reader keeping its own table of stamps.
        std::shared_ptr<Graphic::Image2D> Get( const std::string& sourcePath );

        // Drop the cached entry for one path so the next Get() re-decodes it (used when a thumbnail PNG was
        // regenerated on disk). No-op if not cached.
        void Invalidate( const std::string& sourcePath );

        // Whether `sourcePath` has an entry that needs no decode: cached (a picture, or a remembered failure)
        // and not outdated. What the browser asks before handing a picture to the worker decode.
        [[nodiscard]] bool Holds( const std::string& sourcePath ) const
        {
            return m_Cache.contains( sourcePath ) && !m_Outdated.Contains( sourcePath );
        }

        // The pictures held (entries with an image; remembered failures are not pictures).
        [[nodiscard]] std::size_t ResidentCount() const;

        void Clear();

        /**
         * @brief Clear EVERY live cache's GPU images, while the device is still alive. Called from
         *        EditorLayer::OnDetach.
         *
         * WHY A STATIC SWEEP RATHER THAN A CALL PER OWNER. Most caches belong to a panel and go down with
         * `m_Panels.clear()`, which is safely inside the editor's teardown. Five do NOT: the component
         * widgets keep theirs in FUNCTION-STATICS —
         *
         *     StaticMeshComponent.cpp           `static MaterialComponentWidget materialComponent;`
         *     StaticMeshComponent.cpp           `static ThumbnailCache s_Thumbnails;`
         *     SkinnedMeshComponentWidget.cpp    `static MaterialComponentWidget materials;`
         *     SkinnedMeshComponentWidget.cpp    `static ThumbnailCache s_Pictures;` (SlotPictures)
         *     ComponentEditorRegistrations.cpp  `static MaterialComponentWidget s_InstancedMaterials;`
         *
         * (the fourth is the Instanced Static Mesh editor's material slots, added when an ISM gained a
         * material UI at all — it is listed here because this paragraph is a census and a census that
         * quietly falls behind is worse than none; `ReleaseAll` itself needed no change, because the
         * cache knows its own instances, which is the whole argument below.)
         *
         * — so they are destroyed at `__cxa_finalize`, after ~Application has taken the device and the VMA
         * allocator with it. `~VulkanImage2D` then releases through a freed allocator and the process
         * segfaults. Measured: selecting a mesh with a material and quitting exited 139, with the backtrace
         * naming ~ThumbnailCache <- ~MaterialComponentWidget <- __cxa_finalize_ranges.
         *
         * This is the same family 0bfdeccf fixed for the engine-side registries, and the editor side had
         * FOUR members of it: ThumbnailService's renderer (released by its own Shutdown) and these three
         * image caches. A per-owner call would mean reaching into three widget files to add a hook each, and
         * would silently miss the fourth widget somebody writes next year. The cache knows its own
         * instances, so it is the thing that can promise all of them.
         *
         * Idempotent, and NOT a one-way switch — a cleared cache simply re-decodes on the next Get().
         *
         * Main thread only, like every other thumbnail path (decode and upload happen during the ImGui pass).
         */
        static void ReleaseAll();

        // --- Shared rendered-thumbnail disk-cache layout ---------------------------------------------
        //
        // WHERE A THUMBNAIL LIVES IS NOT DECIDED HERE ANY MORE. `CacheVersion()` and `DiskPath()` moved
        // to Editor/Widgets/ThumbnailKey.hpp in M11, next to the rule that names the file, because this
        // translation unit includes Engine/Graphic/Image.hpp and so cannot be linked without a renderer —
        // and the background sweep, which decides what has no picture, must be drivable by a test. Ask
        // ThumbnailKey::DiskPath. There is no forwarder here: two names for one answer is how two answers
        // start.
        //
        // What is left below genuinely needs the device or the disk: decoding a PNG into an Image2D, and
        // deleting the folders of superseded cache versions so they regenerate cleanly.
        static void PurgeOldVersions(); // drop everything except the current version

    private:
        // Is this path a file WE generated, i.e. does it live under the versioned thumbnail tree? Get()
        // deletes an undecodable file, and this is what keeps that from reaching the user's own images —
        // the same Get() decodes those for the browser's texture previews. See its use for the argument.
        static bool IsOurGeneratedThumbnail( const std::string& path );

        std::unordered_map<std::string, std::shared_ptr<Graphic::Image2D>> m_Cache;
        Common::Utils::WriteWatch                                          m_Watch; // the file as decoded
        ThumbnailOutdated m_Outdated;                                               // rewritten, not re-read yet

        // Every constructed cache, so ReleaseAll() can reach the ones no panel owns. Raw pointers to
        // objects that deregister themselves; this set outlives them all and holds nothing that needs a
        // device, so its own static destruction is harmless.
        static std::unordered_set<ThumbnailCache*>& Live();
    };
} // namespace Desert::Editor
