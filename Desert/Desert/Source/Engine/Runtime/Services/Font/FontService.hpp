#pragma once

#include <Engine/Graphic/Image.hpp>
#include <TextCore/Text/FontBaker.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Runtime
{
    // A baked, GPU-resident font: the SDF atlas texture + the CPU glyph metrics that lay text out.
    struct Font
    {
        std::shared_ptr<Graphic::Image2D> Atlas; // RGBA8; RGB is the multi-channel distance field, A = 255
        Text::BakedFont                   Baked; // metrics: glyph UVs/advances + vertical metrics
    };

    // Owns baked fonts keyed by (ttf path + bake pixel height). First request for a key reads the TTF
    // (VFS-aware), bakes the SDF atlas on the calling thread and uploads it once; later requests hit the
    // cache. Text is niche vs meshes/materials, so no lazy/eviction machinery yet.
    //
    // ANY THREAD (UE: a cache parallel tasks share is guarded by an FRWLock). TextECSSystem lays text out on a
    // worker (CanRunParallel) while the UI canvas and the debug strings resolve the same fonts on their threads.
    // A hit is a shared lock; a bake, a glyph request that re-bakes and a registration take the exclusive one.
    // A Font a re-bake replaces is RETIRED, never freed here (FrameManager::RetireAfterFrames): a raw Font* or
    // atlas pointer stays valid for the frame that resolved it and the frames in flight behind it, never longer.
    class FontService
    {
    public:
        // nullptr if the TTF can't be read or parsed (logged once).
        Font* Get( const std::string& ttfPath, float pixelHeight = Text::kDefaultBakePixelHeight );
        void  Clear();

        // --- Fonts as ASSETS (handle-referenced) --------------------------------------------------------
        // UI/text components reference a font by an AssetHandle, never a raw path: the user drags a .ttf from
        // the Content Browser (RegisterFont on drop) or picks a preloaded one (AvailableFonts). The handle is
        // AssetHandle::FromKey(path) — deterministic & path-derived, so the same .ttf always maps to the same
        // handle and a saved scene resolves without an import step. This registry (handle -> path) is the
        // single source of truth the editor picker, the (de)serializer and the render path all resolve through.

        // Record handle=FromKey(ttfPath) -> ttfPath and return the handle (idempotent). Assigned on drag-drop.
        uint64_t RegisterFont( const std::string& ttfPath );

        // Reverse lookup for display / serialization (path saved to disk keeps scenes portable). "" if unknown.
        std::string PathForHandle( uint64_t handle );

        // Resolve a font handle to its baked atlas (handle -> path -> Get(path,size)). nullptr if unregistered.
        Font* Get( uint64_t handle, float pixelHeight = Text::kDefaultBakePixelHeight );

        // The built-in Roboto-Regular, registered on demand — the fallback when a component has no font set.
        uint64_t DefaultFontHandle();

        // Ask for codepoints OUTSIDE printable ASCII (Cyrillic, CJK, …). The atlas only carries ASCII plus
        // what was requested, because a font has thousands of glyphs and baking them all would blow it up.
        // Anything new drops the baked font so the next Get() re-bakes with it included — call this BEFORE
        // Get() when laying out a string and it is ready the same frame. Returns true if a re-bake is due.
        bool RequestGlyphs( uint64_t handle, const std::vector<uint32_t>& codepoints );

        // Every registered .ttf path (scans the project + engine font roots once). Drives the picker dropdown.
        // A copy taken under the lock: a worker may register a font while the editor walks the list.
        std::vector<std::string> AvailableFonts();

    private:
        // Scan ASSETS_PATH + FONTS_PATH for .ttf once, registering each — the "preloaded at startup" set.
        void EnsurePreloaded();
        // RegisterFont with m_Mutex held exclusively.
        uint64_t RegisterFontLocked( const std::string& ttfPath );

        // Guards every member below. Never held across a call into another FontService entry point.
        mutable std::shared_mutex                              m_Mutex;
        std::unordered_map<std::string, std::shared_ptr<Font>> m_Fonts;       // key = path + '|' + size
        std::unordered_map<std::string, std::vector<uint32_t>> m_ExtraGlyphs; // ttf path -> extra codepoints
        std::vector<std::string>                               m_Available;   // registered paths (for the picker)
        bool                                                   m_Scanned = false;
    };
} // namespace Desert::Runtime
