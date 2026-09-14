#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure CPU vector-icon pipeline: SVG bytes -> flattened filled contours -> a signed distance field.
// Depends ONLY on the standard library (no engine/GPU/glm types) so it is unit-testable in isolation,
// exactly like Engine/Text/FontBaker. The engine-side IconService turns the SDF into an atlas texture.
//
// SVG is an IMPORT-TIME format here: nothing in the runtime parses XML. An icon is imported once into
// an SDF and from then on it is drawn by the same shader as text, which is what keeps it crisp at any
// size and gives it outline/glow/shadow for free.
namespace Desert::Vector
{
    struct Vec2
    {
        float X = 0.0f, Y = 0.0f;
    };

    // One filled path, flattened to polygons. Curves are already subdivided; holes are separate
    // contours resolved by the NON-ZERO winding rule (the SVG default).
    struct Shape
    {
        std::vector<std::vector<Vec2>> Contours;
        uint32_t                       FillRGBA = 0xFFFFFFFFu; // kept for a future multi-colour path
    };

    struct VectorImage
    {
        std::vector<Shape> Shapes;
        float              Width  = 0.0f; // viewBox extent the contour coordinates live in
        float              Height = 0.0f;

        bool Valid() const
        {
            return Width > 0.0f && Height > 0.0f && !Shapes.empty();
        }
    };

    // Parses the SVG subset an icon needs: <svg viewBox/width/height>, <g>, <path d>, <rect>, <circle>,
    // <ellipse>, <polygon>, <polyline>, <line>, fill (#rgb/#rrggbb/none), and translate/scale/matrix
    // transforms. Anything else (CSS, filters, gradients, text, clip paths) is IGNORED, not an error —
    // icon exports from a design tool stay within this subset. Returns an invalid image on bad input.
    // `curveTolerance` is the flattening error in viewBox units (smaller = more segments).
    VectorImage ParseSvg( const char* xml, size_t size, float curveTolerance = 0.05f );

    // Rasterises the image into a square single-channel SDF of `size`+2*padding texels: the shape is
    // fitted (aspect-preserved) into the inner `size` box, and each texel stores the signed distance to
    // the outline in the ONE encoding the text shader decodes (Core/Formats/SdfAtlasEncoding.hpp) —
    // kSdfAtlasOnEdgeByte at the edge, rising inside, spread over kSdfAtlasDistanceRangeTexels texels.
    // `padding` must therefore be at least half that band, or the field is clipped before its range ends.
    // Distances are exact (per-texel nearest segment) rather than a propagated transform — an icon is small and
    // this runs at import time only. Returns an empty vector if the image is invalid.
    //
    // [shapeBegin, shapeEnd) selects a RANGE of shapes; the default takes them all. A multi-colour icon
    // is baked one colour run at a time, and because the fit box is always derived from the WHOLE image
    // every range lands in the same frame — so the layers stack back up perfectly.
    std::vector<uint8_t> RasterizeSdf( const VectorImage& image, uint32_t size, int padding = 6,
                                       size_t shapeBegin = 0, size_t shapeEnd = static_cast<size_t>( -1 ) );
} // namespace Desert::Vector
