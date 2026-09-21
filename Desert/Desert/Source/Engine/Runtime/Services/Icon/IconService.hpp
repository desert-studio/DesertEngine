#pragma once

#include <Engine/Graphic/Image.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Runtime
{
    // One colour run of an icon: the sub-rect of the shared atlas holding its distance field, plus the
    // fill the .svg authored. A plain monochrome icon has exactly one layer (white), so it tints as a
    // whole; a multi-colour icon has one layer per colour, drawn back-to-front in document order.
    struct IconLayer
    {
        float    U0 = 0.0f, V0 = 0.0f;
        float    U1 = 1.0f, V1 = 1.0f;
        uint32_t RGBA = 0xFFFFFFFFu;
    };

    // A baked icon. Icons are drawn by the TEXT shader — an SDF is an SDF — so they stay crisp at any
    // size and inherit outline / glow / shadow for free.
    struct Icon
    {
        std::vector<IconLayer> Layers;
        float                  Aspect = 1.0f; // source viewBox width / height

        bool Valid() const
        {
            return !Layers.empty();
        }
    };

    // Owns icons baked from .svg, keyed by asset handle. Mirrors FontService on purpose: the handle is
    // AssetHandle::FromKey(path) — deterministic and path-derived, so the same file always maps to the
    // same handle and a saved scene resolves with no import step. SVG is parsed HERE, at import time
    // only; the runtime draw path never sees XML.
    //
    // Every icon shares ONE atlas texture, so the whole UI's icons draw in a single batch instead of one
    // per texture. Importing a new icon repacks and re-uploads the page — the same amortised trick the
    // font atlas uses for a new glyph: a scene's icon set settles in the first frames and then holds.
    class IconService
    {
    public:
        // Record handle=FromKey(svgPath) -> svgPath and return the handle (idempotent). Assigned on drop.
        uint64_t RegisterIcon( const std::string& svgPath );

        // Reverse lookup for display / serialization ("" if the handle is unknown).
        std::string PathForHandle( uint64_t handle );

        // Resolve a handle to its baked layers (imports + repacks the atlas on first use). nullptr if
        // unregistered or unparseable — a broken icon draws nothing rather than taking the frame down.
        Icon* Get( uint64_t handle );

        // The page every layer's UVs address. Null until the first icon is imported.
        const std::shared_ptr<Graphic::Image2D>& Atlas() const
        {
            return m_Atlas;
        }

        // Every registered .svg path (scans the project + engine icon roots once). Drives the picker.
        const std::vector<std::string>& AvailableIcons();

        void Clear();

    private:
        void EnsurePreloaded();
        // Re-shelf every imported layer into one page and re-upload it, rewriting each icon's UVs.
        bool RepackAtlas();

        // CPU-side distance field of one colour run, kept so a repack never re-parses the .svg.
        struct LayerBitmap
        {
            std::vector<uint8_t> Sdf; // Dim*Dim, single channel
            uint32_t             Dim  = 0;
            uint32_t             RGBA = 0xFFFFFFFFu;
            std::string          Owner; // svg path this run belongs to
        };

        std::unordered_map<std::string, std::unique_ptr<Icon>> m_Icons;        // svg path -> baked icon
        std::vector<LayerBitmap>                               m_Bitmaps;      // every imported colour run
        std::vector<std::string>                               m_Available;    // registered paths
        std::shared_ptr<Graphic::Image2D>                      m_Atlas;
        // Repacking happens WHILE the UI draw list is being built, so the page it replaces may still be
        // referenced by an already-recorded command buffer (and by ImGui's descriptor cache, keyed by
        // image view). Retired pages are therefore held until the icon set is dropped wholesale, never
        // freed mid-frame. A session imports its icons once, so this is a few hundred KB at worst.
        std::vector<std::shared_ptr<Graphic::Image2D>> m_Retired;
        uint32_t                                       m_AtlasSize = 0;
        bool                                           m_Scanned   = false;
    };
} // namespace Desert::Runtime
