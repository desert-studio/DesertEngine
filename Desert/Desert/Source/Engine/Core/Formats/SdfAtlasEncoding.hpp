#pragma once

#include <cstdint>

// HOW A DISTANCE-FIELD ATLAS ENCODES DISTANCE — one definition, every producer, and the one shader that
// samples them all.
//
// Two producers feed Programs/UI/UIText.shader: the glyph atlas (Engine/Text/FontBaker) and the icon
// atlas (Engine/Vector/VectorImage). The shader turns a sampled value back into a screen-space distance
// by dividing by this band, so a producer that used a different one would draw every edge at the wrong
// width — invisibly, because "slightly too crisp" looks like a rendering choice rather than a bug.
//
// THEY ALREADY DISAGREED. The icon rasterizer spread its field over `padding` texels with padding = 6,
// the font baker over padding = 5, and the comment above the icon one said it matched the font baker so
// that one shader would serve both. That is the shape this header exists to make impossible: an
// agreement stated in a comment, honoured by neither side, and unobservable from either.
namespace Desert::Core::Formats
{
    // Full width, in ATLAS TEXELS, of the distance band the byte range spans: byte 0 is
    // kSdfAtlasDistanceRangeTexels/2 texels OUTSIDE the outline, byte 255 the same distance inside, and
    // kSdfAtlasOnEdgeByte is on it.
    //
    // Ten, and the number is load-bearing at the SMALL end. The shader's edge ramp is this many screen
    // pixels wide times (screen pixels per atlas texel); once that falls below one pixel a minified glyph
    // can no longer be resolved and breaks up. At the 48 px glyph bake, ten texels holds the ramp at or
    // above a pixel down to 4.8 px of rendered height — the size a world label reaches near the horizon.
    // Each producer must rasterize at least kSdfAtlasDistanceRangeTexels/2 texels of gutter around its
    // shape, or the band is clipped by the cell before it reaches the ends of its own range.
    inline constexpr float kSdfAtlasDistanceRangeTexels = 10.0f;

    // The byte the outline itself lands on. 128 rather than 127 or 127.5 because a uint8 has no half:
    // the shader thresholds at 0.5 and 128/255 is the first byte above it, so a texel exactly on the
    // outline reads as inside by 1/510 of the band — two hundredths of a texel, and consistent for both
    // producers, which is the part that matters.
    inline constexpr uint8_t kSdfAtlasOnEdgeByte = 128;
} // namespace Desert::Core::Formats
