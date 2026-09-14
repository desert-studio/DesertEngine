#pragma once

#include <Engine/Core/Formats/SdfAtlasEncoding.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Pure CPU font baking: TrueType bytes -> a single MULTI-CHANNEL signed-distance glyph atlas + per-glyph
// metrics. Depends ONLY on stb_truetype + Engine/Text/Msdf + the standard library (no engine/GPU types)
// so it is unit-testable in isolation. The engine-side FontService turns a BakedFont into a GPU texture +
// draws quads.
//
// It used to bake a ONE-channel field, and that path is gone rather than optional. A single channel
// cannot encode a corner — the nearest-edge distance is smooth across the corner's bisector — so every
// junction was rounded off at roughly the field's own range: the apex of A, the middle of W and M, the
// serifs, the thin crossings. Invisible at 12 px, plainly visible at a large UI size and unavoidable on a
// world label a camera can walk up to. See Msdf.hpp for how three channels fix it.
namespace Desert::Text
{
    // One glyph in the baked atlas. UVs are normalized [0,1]; the pixel-space fields are relative to
    // the bake size (PixelHeight) and scale linearly with the rendered text size.
    struct Glyph
    {
        float U0 = 0, V0 = 0, U1 = 0, V1 = 0; // atlas sub-rect (normalized)
        float Width = 0, Height = 0;          // glyph quad size in bake pixels (incl. padding)
        float OffsetX = 0, OffsetY = 0;       // pen -> glyph top-left, in bake pixels (Y down)
        float Advance = 0;                    // horizontal pen advance in bake pixels
    };

    struct BakedFont
    {
        uint32_t                            AtlasWidth  = 0;
        uint32_t                            AtlasHeight = 0;
        // AtlasWidth*AtlasHeight*4. RGB is the multi-channel distance field; ALPHA IS 255 EVERYWHERE.
        // The engine has no R8/RGB8 sampled format, so the fourth channel exists whatever we do; making
        // it opaque means a plain alpha-blended debug view of the atlas shows the field's edge colouring,
        // which is how an MSDF is actually read. It deliberately does NOT carry a second, single-channel
        // field: that would be the old path kept alive under a new name.
        std::vector<uint8_t>                AtlasRGBA;
        std::unordered_map<uint32_t, Glyph> Glyphs; // keyed by Unicode codepoint

        float PixelHeight = 0;         // the bake size the metrics are expressed in
        float Ascent = 0, Descent = 0; // scaled to bake pixels (Descent is negative, Y-down)
        float LineGap = 0;
        // Full width, in ATLAS TEXELS, of the distance band the byte range encodes: 0 and 255 sit
        // DistanceRangeTexels/2 outside and inside the outline, 128 is on it. The shader needs it to turn
        // a sampled distance into a screen-space one, so it travels with the atlas rather than being a
        // number agreed by two files. Desert/Tests/Engine/FontBaker pins it against the GLSL constant.
        float DistanceRangeTexels = 0;

        bool Valid() const
        {
            return AtlasWidth > 0 && AtlasHeight > 0 && !AtlasRGBA.empty();
        }
        float LineHeight() const { return Ascent - Descent + LineGap; }
    };

    // The bake parameters. They are INPUTS TO THE CACHE KEY (FontCache.hpp), not facts folded into a
    // version number: an atlas is a function of exactly these plus the .ttf bytes and the glyph set, and
    // keying on anything less is a cache that can return the wrong picture.
    inline constexpr int      kGlyphPadding = 5;   // texels of gutter rasterized around each glyph
    inline constexpr uint32_t kAtlasWidth   = 512; // shelf-packer row width
    // The band is NOT this baker's to choose: the icon atlas is sampled by the same shader with the same
    // divisor, so it belongs to Core/Formats/SdfAtlasEncoding.hpp and is only re-checked here against the
    // gutter this baker actually rasterizes. The atlas carries NO MIPS — see FontService — so the band is
    // also the only minification headroom there is.
    inline constexpr float kDistanceRangeTexels = Core::Formats::kSdfAtlasDistanceRangeTexels;
    static_assert( kDistanceRangeTexels <= 2.0f * kGlyphPadding,
                   "the distance band must fit inside the rasterized gutter" );

    // The ONE bake size the engine requests at runtime (FontService defaults, the UI canvas text path)
    // and therefore the one the game packager cooks. It used to be a literal in four places; the packager
    // baking any other number would ship atlases under keys the runtime never asks for.
    inline constexpr float kDefaultBakePixelHeight = 48.0f;

    // Bakes printable ASCII [32,126] — plus any `extraCodepoints` asked for — into an MSDF atlas via a
    // simple shelf packer. `pixelHeight` is the bake resolution (bigger = sharper minification headroom,
    // larger atlas). Codepoints the font has no glyph for are skipped, so asking for a character a font
    // doesn't carry costs nothing but is not an error either.
    //
    // The extras list is how anything beyond ASCII (Cyrillic, CJK, …) gets into the atlas: a font has
    // thousands of glyphs and baking them all would blow it up, so the text layer asks for exactly the
    // codepoints its strings use. Returns an invalid BakedFont if the TTF cannot be parsed. Never throws.
    BakedFont BakeFontMSDF( const uint8_t* ttf, size_t ttfSize, float pixelHeight = kDefaultBakePixelHeight,
                            const std::vector<uint32_t>& extraCodepoints = {} );

    // Binary (de)serialization of a BakedFont for the on-disk font cache — pure stdlib so it stays
    // engine-independent and unit-testable. Little-endian, layout-tagged.
    //
    // THE VERSION LIVES IN THE FILE AND NOT IN THE CACHE KEY, which is a deliberate reversal. While the
    // version was part of the key, bumping it made every existing atlas UNREACHABLE rather than refused:
    // the runtime looked under a new name, found nothing, silently re-baked, and a shipped .dpak full of
    // old atlases cost every player the bake at every start with nothing to say so. Now the file is found,
    // read, and refused BY PATH with both version numbers in the message.
    inline constexpr uint32_t kBakedFontCacheVersion = 2;

    enum class FontDecodeStatus
    {
        Ok,
        BadMagic,        // not a .dfont at all
        VersionMismatch, // a .dfont of another format generation — the case that must be named, not guessed
        Corrupt          // right magic and version, wrong contents (truncation, size disagreement)
    };

    std::vector<uint8_t> SerializeBakedFont( const BakedFont& font );
    // `fileVersion` is filled in whenever the magic was right, so a caller can name the version it
    // refused. Returns Ok only for a fully-read, valid font.
    FontDecodeStatus DeserializeBakedFont( const uint8_t* data, size_t size, BakedFont& out,
                                           uint32_t* fileVersion = nullptr );
} // namespace Desert::Text
