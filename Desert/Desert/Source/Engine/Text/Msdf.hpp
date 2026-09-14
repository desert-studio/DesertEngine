#pragma once

// Multi-channel signed distance field generation for closed outlines.
//
// WHY THIS EXISTS AT ALL. A single-channel SDF cannot represent a corner: the distance to the nearest
// edge is smooth across the corner's bisector, so reconstructing `distance > 0` rounds every junction
// off at roughly the field's own range. At 12 px that is invisible; at 200 px, or on a world label the
// camera walks up to, the joins of A/W/M and every serif visibly melt.
//
// The fix is Chlumsky's: colour each edge of the outline so that adjacent edges around a corner share
// exactly one channel, store one distance field PER CHANNEL, and reconstruct with the MEDIAN of the
// three. Away from a corner all three channels agree and the median is the ordinary distance; at a
// corner two channels disagree and the median follows whichever pair of edges actually meets there, so
// the corner survives as an exact intersection of two half-planes.
//   Algorithm: Viktor Chlumsky, "Shape Decomposition for Multi-Channel Distance Fields", MSc thesis,
//   Czech Technical University in Prague, 2015. Implemented here from the published description — no
//   third-party code is vendored for it, so the engine gains no new dependency and no new licence.
//
// Pure CPU and stdlib-only on purpose, exactly like FontBaker beside it: the outlines come in as plain
// control points (stb_truetype for glyphs), the field goes out as floats, and the whole thing is unit
// testable with no font, no GPU and no engine types.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::Text::Msdf
{
    struct Vec2
    {
        double X = 0.0;
        double Y = 0.0;
    };

    // Channel mask. The three primaries are never used alone for a spline: adjacent splines are given
    // two-channel colours (YELLOW/MAGENTA/CYAN) so that they always overlap in exactly one channel,
    // which is what makes the median continuous along the outline instead of only at the corners.
    enum class EdgeColor : unsigned char
    {
        Black   = 0,
        Red     = 1,
        Green   = 2,
        Yellow  = 3,
        Blue    = 4,
        Magenta = 5,
        Cyan    = 6,
        White   = 7
    };

    // One outline segment: a line (2 points), a quadratic (3) or a cubic (4) Bezier. TrueType `glyf`
    // outlines are quadratic; CFF/OpenType ones are cubic, and .ttf files carrying a CFF table do reach
    // here — so both are first-class rather than one being approximated by the other.
    struct EdgeSegment
    {
        // A fixed four-slot control polygon rather than a vector: an outline has thousands of these and
        // none of them ever needs the heap. `PointCount` says how many slots are live.
        std::array<Vec2, 4> Points{};
        int                 PointCount = 2;
        EdgeColor           Color      = EdgeColor::White;

        [[nodiscard]] Vec2 PointAt( double param ) const;
        [[nodiscard]] Vec2 Direction( double param ) const; // NOT normalized (a derivative, and it may be zero)
    };

    struct Contour
    {
        std::vector<EdgeSegment> Edges;
    };

    struct Shape
    {
        std::vector<Contour> Contours;
    };

    // Beyond this much turn at a join, the join is a CORNER and the two edges must not share all three
    // channels. Three radians is about 172 degrees of straightness, the value the thesis uses and the
    // one type designers draw for: below it a "smooth" join is really a very shallow corner.
    inline constexpr double kDefaultCornerAngleRadians = 3.0;

    // Assign channel colours by corner detection. `angleThresholdRad` is the turn beyond which a join
    // counts as a corner (3 rad ~= 172 deg is the value the thesis uses and what fonts are drawn for).
    // `seed` only chooses WHICH of the three two-channel colours a contour starts on; it changes the
    // colouring but not the reconstructed shape, and is kept deterministic so a bake is reproducible.
    void ColorEdges( Shape& shape, double angleThresholdRad = kDefaultCornerAngleRadians, uint64_t seed = 0 );

    // Rasterize the shape into `outRGB` (w*h*3 floats, row-major, Y as given — the caller supplies the
    // transform, so a Y-down atlas is a Y-down transform and not a flip afterwards).
    //
    // COORDINATES ARE THE OUTPUT'S OWN. `shape` must already be in output-texel space; there is no
    // scale argument, because a scale that lives in two places is a scale that disagrees with itself.
    // `rangeTexels` is the full width of the encodable distance band in those same texels: a returned
    // value of 0.5 is exactly on the outline, 0 and 1 are `rangeTexels/2` outside and inside it.
    //
    // Values are NOT clamped — the caller quantizes, and a test can see how far past the band a texel
    // was. An empty shape leaves the field at the "far outside" value rather than at zero distance.
    void GenerateMSDF( std::vector<float>& outRGB, int width, int height, const Shape& shape, double rangeTexels );

    // The ordinary single-channel field of the same shape, in the same encoding. Not used by the text
    // path — it is the reference the corner tests measure MSDF against, which is the only way to state
    // "the corner survived" as a number rather than as an opinion.
    void GenerateSDF( std::vector<float>& outR, int width, int height, const Shape& shape, double rangeTexels );

    // There is deliberately NO Median() here. The one definition of "what the three channels mean" is
    // Editor/Resources/Shaders/Common/SdfText.glslh, which the shaders compile as GLSL and the tests
    // compile as C++ — a second copy in this header would be a copy the GPU never runs.
} // namespace Desert::Text::Msdf
