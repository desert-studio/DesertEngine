#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

// A BAKED FONT AS DATA: the multi-channel distance atlas and the metrics that lay text out with it. Split
// from the baker (FontBaker.hpp) because the two have different readers: the baker, its cache and the
// packager MAKE one, while the UI walk and the 3D text system only READ one -- and a reader has no business
// including stb_truetype's front end to learn what a glyph is.
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
        uint32_t AtlasWidth  = 0;
        uint32_t AtlasHeight = 0;
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

        [[nodiscard]] bool Valid() const
        {
            return AtlasWidth > 0 && AtlasHeight > 0 && !AtlasRGBA.empty();
        }
        [[nodiscard]] float LineHeight() const
        {
            return Ascent - Descent + LineGap;
        }
    };

    // The ONE bake size the engine requests at runtime (FontService defaults, the UI canvas text path)
    // and therefore the one the game packager cooks. It used to be a literal in four places; the packager
    // baking any other number would ship atlases under keys the runtime never asks for.
    inline constexpr float kDefaultBakePixelHeight = 48.0f;
} // namespace Desert::Text
