// The lens flare's placement maths, driven against the shader's own text.
//
// Two halves are under test. Common/LensFlare.glslh, compiled here as C++, decides WHERE every feature
// lands; Engine/Graphic/PostProcessing/LensFlareRules.hpp decides how much of it is added back in. The
// second is one multiply and is tested anyway, because "the flare paints when the sun is behind the
// camera" is the way this effect goes wrong and the multiply is the only thing preventing it.
//
// The tests are relations, not spot values: ghosts in order along a named axis, a halo that reads the
// ring it draws, streak taps that overlap the source instead of stamping copies of it. Each of those is
// a property no single rendered frame can establish, and the two that were violated by the first
// implementation (dashed streak, halo sampling the empty far side) are pinned by name below.

#include "LensFlareReference.hpp"

#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace Desert::Tests::LensFlareRef;

using Desert::Graphic::ComputeSunScreen;
using Desert::Graphic::LensFlareStrength;
using Desert::Graphic::SunScreen;

namespace
{
    constexpr float kAspect     = 1280.0f / 766.0f; // the shot resolution these were tuned at
    constexpr float kShotWidth  = 1280.0f;
    constexpr float kShotHeight = 766.0f;
    const glm::vec2 kCentre( 0.5f, 0.5f );

    // The authored ghost train — Core::SceneSettings defaults, which is what Clouds_Demo and
    // Sky_PhysicalShowcase both ship with. The continuity test below is about the train we actually draw,
    // not an invented one.
    constexpr float kGhostCount   = 4.0f;
    constexpr float kGhostSpacing = 0.35f;
    constexpr float kGhostNear    = 1.0f;
    constexpr float kGhostFar     = 3.0f;

    // The largest change in ghost weight allowed between two neighbouring PIXELS of the rendered frame.
    //
    // Where the number comes from. A band is visible when the weight's step times the ghost's own local
    // radiance clears one 8-bit code value, 1/255. The brightest ghost contribution this programme has
    // ever measured in a frame is 0.054 of displayed luminance — that is exactly the size of the Ц9 band,
    // which was a full 1 -> 0 step, so it IS the ghost's local radiance there. Allowing a quarter of the
    // displayed range, a 4.6x margin over that measurement, gives (1/255) / 0.25 = 1/64.
    //
    // A hard `step` cannot come near this and that is the point: it moves the weight by the whole rim
    // value in one pixel — 0.99 with the sun low in the frame — sixty-three times the bound. The fade that
    // replaced it peaks at 0.0078, exactly the 1.5/(w*scale*pixels) above and half of what is allowed here.
    constexpr float kMaxWeightStepPerPixel = 1.0f / 64.0f;

    float Dist( const glm::vec2& a, const glm::vec2& b, float aspect )
    {
        return LensFlareDistance( a, b, aspect );
    }

    struct EdgeWalk
    {
        float MaxStep;    // largest change in weight between neighbouring pixels
        float MaxWeight;  // how alive the ghost was along the walk — a walk through a dead ghost proves nothing
        float AtBoundary; // the weight exactly ON the source frame's edge
    };

    // Walks one ghost's weight straight across the source frame, one rendered PIXEL at a time, and reports
    // the biggest jump it took. The walk is laid out in SOURCE uv (from just outside one edge to just
    // outside the other) and converted back to screen positions, because that is the axis the frame's
    // boundary lives on: sourceUv.y depends only on uv.y, which is why a discontinuity here is a straight
    // horizontal line on the screen rather than a rough edge somewhere.
    EdgeWalk WalkAcrossTheSourceFrame( const glm::vec2& sun, int index, bool vertical )
    {
        const float     scale  = LensFlareGhostScale( float( index ), kGhostCount, kGhostNear, kGhostFar );
        const glm::vec2 centre = LensFlareGhostCenter( sun, float( index ), kGhostSpacing );

        // One screen pixel is this much of the source's uv. A ghost divides screen offsets by its own
        // scale, so a big ghost crosses the source slowly and the smallest authored one — scale 1 — the
        // fastest, which makes it the worst case and the reason the sweep below starts at index 0.
        const float pixels = vertical ? kShotHeight : kShotWidth;
        const float step   = 1.0f / ( pixels * scale );

        EdgeWalk walk{ 0.0f, 0.0f, -1.0f };
        float    previous = -1.0f;
        for ( float s = -0.02f; s <= 1.02f; s += step )
        {
            glm::vec2 uv = centre;
            if ( vertical )
                uv.y = centre.y + ( s - sun.y ) * scale;
            else
                uv.x = centre.x + ( s - sun.x ) * scale;

            const glm::vec2 source = LensFlareGhostSourceUv( uv, centre, scale, sun );
            const float     weight = LensFlareGhostWeight( source, uv, centre, scale, kAspect );

            if ( previous >= 0.0f )
                walk.MaxStep = std::max( walk.MaxStep, std::fabs( weight - previous ) );
            previous       = weight;
            walk.MaxWeight = std::max( walk.MaxWeight, weight );

            // The first sample at or past the near edge is the boundary itself, to within one pixel.
            if ( walk.AtBoundary < 0.0f && s >= 0.0f )
                walk.AtBoundary = weight;
        }
        return walk;
    }
} // namespace

// --- Ghost placement --------------------------------------------------------------------------------

TEST( LensFlareGhosts, WalkTheSunToCentreAxisInOrder )
{
    const glm::vec2 sun( 0.8f, 0.2f );
    const float     spacing = 0.35f;

    // Every ghost is ON the sun->centre line, and each is further along it than the last. "On the line"
    // is the whole claim of the feature: a ghost off the axis is not a reflection about the optical
    // centre, it is a sprite someone placed.
    float previous = -1.0f;
    for ( int i = 0; i < 6; ++i )
    {
        const glm::vec2 centre = LensFlareGhostCenter( sun, static_cast<float>( i ), spacing );

        // Colinearity via the 2D cross product of (centre - sun) and (kCentre - sun).
        const glm::vec2 toGhost  = centre - sun;
        const glm::vec2 toCentre = kCentre - sun;
        const float     cross    = toGhost.x * toCentre.y - toGhost.y * toCentre.x;
        EXPECT_NEAR( cross, 0.0f, 1e-6f ) << "ghost " << i << " left the sun->centre axis";

        const float along = glm::dot( toGhost, toCentre ) / glm::dot( toCentre, toCentre );
        EXPECT_GT( along, previous ) << "ghost " << i << " did not advance along the axis";
        previous = along;

        EXPECT_NEAR( along, spacing * static_cast<float>( i + 1 ), 1e-5f );
    }
}

TEST( LensFlareGhosts, ContinuePastTheCentreOnceSpacingExceedsTheAxis )
{
    // The second half of a real ghost train is on the far side of the optical centre. With spacing 0.35
    // that starts at ghost 2 (3 x 0.35 > 1), and the sign of (centre - screenCentre) must flip.
    const glm::vec2 sun( 0.8f, 0.2f );

    const glm::vec2 nearGhost = LensFlareGhostCenter( sun, 1.0f, 0.35f ); // 0.70 of the way
    const glm::vec2 farGhost  = LensFlareGhostCenter( sun, 2.0f, 0.35f ); // 1.05 of the way

    EXPECT_GT( nearGhost.x, kCentre.x ); // still on the sun's side
    EXPECT_LT( farGhost.x, kCentre.x );  // past it
}

TEST( LensFlareGhosts, SizeAndTintRampTogetherAcrossTheTrain )
{
    // Size and tint share LensFlareGhostRamp precisely so they cannot disagree about which end of the
    // train a ghost is on — the "two implementations of one quantity" defect this project keeps paying
    // for. Ghost 0 is exactly the nearGhost end, the last exactly the farGhost end, and both are monotone.
    const float count = 5.0f;
    const vec3  inner( 1.0f, 0.86f, 0.62f );
    const vec3  outer( 0.45f, 0.68f, 1.0f );

    EXPECT_FLOAT_EQ( LensFlareGhostScale( 0.0f, count, 1.0f, 3.0f ), 1.0f );
    EXPECT_FLOAT_EQ( LensFlareGhostScale( count - 1.0f, count, 1.0f, 3.0f ), 3.0f );

    const vec3 first = LensFlareGhostTint( 0.0f, count, inner, outer );
    const vec3 last  = LensFlareGhostTint( count - 1.0f, count, inner, outer );
    EXPECT_NEAR( first.r, inner.r, 1e-6f );
    EXPECT_NEAR( last.b, outer.b, 1e-6f );

    float previousScale = -1.0f;
    for ( int i = 0; i < 5; ++i )
    {
        const float index = static_cast<float>( i );
        const float scale = LensFlareGhostScale( index, count, 1.0f, 3.0f );
        EXPECT_GT( scale, previousScale );
        previousScale = scale;

        // The tint's own position on the ramp must equal the size's, exactly.
        const vec3  tint     = LensFlareGhostTint( index, count, inner, outer );
        const float sizeRamp = ( scale - 1.0f ) / 2.0f;
        const float tintRamp = ( tint.r - inner.r ) / ( outer.r - inner.r );
        EXPECT_NEAR( sizeRamp, tintRamp, 1e-5f );
    }
}

TEST( LensFlareGhosts, ASingleGhostSitsAtTheNearEndOfTheRamp )
{
    // count == 1 divides by (count - 1). It must not produce NaN, and one ghost is the NEAR end.
    EXPECT_FLOAT_EQ( LensFlareGhostRamp( 0.0f, 1.0f ), 0.0f );
    EXPECT_FLOAT_EQ( LensFlareGhostScale( 0.0f, 1.0f, 1.0f, 3.0f ), 1.0f );
}

TEST( LensFlareGhosts, ReadTheSunAtTheirOwnCentreAndScaleAwayFromIt )
{
    // A ghost is an IMAGE of the source: at its centre it reads the sun, and its magnification is what
    // scale means. Both halves matter — the first is why a ghost is bright at all, the second is why a
    // larger ghost is the same picture enlarged rather than a bigger blob.
    const glm::vec2 sun( 0.8f, 0.2f );
    const glm::vec2 centre = LensFlareGhostCenter( sun, 0.0f, 0.35f );

    const glm::vec2 atCentre = LensFlareGhostSourceUv( centre, centre, 2.0f, sun );
    EXPECT_NEAR( atCentre.x, sun.x, 1e-6f );
    EXPECT_NEAR( atCentre.y, sun.y, 1e-6f );

    // Twice the scale reaches half as far into the source for the same screen offset.
    const glm::vec2 offset( 0.05f, 0.0f );
    const glm::vec2 smallOne = LensFlareGhostSourceUv( centre + offset, centre, 1.0f, sun );
    const glm::vec2 large = LensFlareGhostSourceUv( centre + offset, centre, 2.0f, sun );
    EXPECT_NEAR( large.x - sun.x, ( smallOne.x - sun.x ) * 0.5f, 1e-6f );
}

TEST( LensFlareGhosts, WeightIsZeroOutsideTheSourceFrame )
{
    // The engine-wide sampler is REPEAT, so a source uv outside [0,1] would wrap the opposite edge's
    // bright content back in. The weight, not the sampler, is what stops that.
    const glm::vec2 sun( 0.8f, 0.2f );
    const glm::vec2 centre = LensFlareGhostCenter( sun, 0.0f, 0.35f );

    EXPECT_FLOAT_EQ( LensFlareGhostWeight( glm::vec2( 1.4f, 0.5f ), centre, centre, 1.0f, kAspect ), 0.0f );
    EXPECT_FLOAT_EQ( LensFlareGhostWeight( glm::vec2( 0.5f, -0.2f ), centre, centre, 1.0f, kAspect ), 0.0f );
    EXPECT_GT( LensFlareGhostWeight( sun, centre, centre, 1.0f, kAspect ), 0.0f );

    // And zero ON the boundary, not merely past it. The wrap starts at the first texel outside, so a gate
    // that is still open AT the edge is a gate that hands the opposite edge to the very next sample.
    EXPECT_FLOAT_EQ( LensFlareGhostWeight( glm::vec2( 0.0f, 0.5f ), centre, centre, 1.0f, kAspect ), 0.0f );
    EXPECT_FLOAT_EQ( LensFlareGhostWeight( glm::vec2( 0.5f, 1.0f ), centre, centre, 1.0f, kAspect ), 0.0f );
}

TEST( LensFlareGhosts, WeightReachesZeroAtTheSourceFrameWithoutAStep )
{
    // THE Ц9 defect, as a relation rather than a value. "Zero outside the frame" and "more than zero
    // inside it" are both true of a hard `step`, which is exactly how a full-width band survived to be
    // found by a rendered frame instead of by this suite. What a step cannot satisfy is the relation
    // BETWEEN neighbouring samples: sourceUv.y depends only on uv.y, so the gate closes on one constant
    // screen ROW across the entire width — one straight line per ghost — and the aperture rim beside it
    // can never soften that, because the rim only reaches zero past radius 0.75 while this boundary is
    // crossed at min(sunUv.y, 1 - sunUv.y) <= 0.5, always sooner. In the frame that found it the rim was
    // still 1.000 where the step fired: the ghost switched off at full brightness.
    //
    // So the assertion is continuity at the sampling rate we render at, on BOTH axes — the x boundary
    // draws the same line down a column, and was invisible in that frame only because the source happened
    // to be dark on that side.
    const glm::vec2 suns[] = {
         glm::vec2( 0.165f, 0.146f ), // the Ц9 shot: Clouds_Demo, camera 0,200,0, look 0,0.9,1
         glm::vec2( 0.5f, 0.35f ),    // sun high in a centred frame — the rim is 0.90 at the crossing
         glm::vec2( 0.78f, 0.30f ),   // mirrored, so the far edges (sourceUv -> 1) are walked too
         glm::vec2( 0.30f, 0.72f ),   // sun low: the crossing is the BOTTOM edge
    };

    for ( const glm::vec2& sun : suns )
        for ( int index = 0; index < static_cast<int>( kGhostCount ); ++index )
            for ( bool vertical : { true, false } )
            {
                const EdgeWalk walk = WalkAcrossTheSourceFrame( sun, index, vertical );
                const char*    axis = vertical ? "vertical" : "horizontal";

                // Not a vacuous pass: the walk has to go through a ghost that is actually on. Without
                // this a weight of zero everywhere would satisfy every line below.
                ASSERT_GT( walk.MaxWeight, 0.1f )
                     << "sun " << sun.x << "," << sun.y << " ghost " << index << " " << axis
                     << ": the walk never met a live ghost, so it proves nothing";

                EXPECT_LE( walk.MaxStep, kMaxWeightStepPerPixel )
                     << "sun " << sun.x << "," << sun.y << " ghost " << index << " " << axis
                     << ": the ghost's weight jumps " << walk.MaxStep
                     << " between neighbouring pixels — that is a hard line across the frame, not an edge";

                EXPECT_LE( walk.AtBoundary, kMaxWeightStepPerPixel )
                     << "sun " << sun.x << "," << sun.y << " ghost " << index << " " << axis
                     << ": the ghost is still at " << walk.AtBoundary
                     << " where the source frame ends — it has to have arrived at zero by then, or the "
                        "REPEAT sampler hands it the opposite edge one pixel later";
            }
}

// --- Halo -------------------------------------------------------------------------------------------

TEST( LensFlareHalo, ReadsTheSunExactlyOnTheRing )
{
    // THE bug this pins. The first implementation read the source at sunUv + bearing*radius — the far
    // side of the ring, which is empty sky — and the halo simply never appeared. On the ring the source
    // point must BE the sun, which is what puts the sun's own image on the ring.
    const glm::vec2 sun( 0.4f, 0.45f );
    const float     radius = 0.18f;

    for ( float angle = 0.0f; angle < 6.2f; angle += 0.7f )
    {
        // A screen point exactly `radius` away in aspect-corrected uv, on this bearing.
        const glm::vec2 bearing( std::cos( angle ), std::sin( angle ) );
        const glm::vec2 onRing = sun + ( bearing * radius ) / glm::vec2( kAspect, 1.0f );
        ASSERT_NEAR( Dist( onRing, sun, kAspect ), radius, 1e-4f );

        const glm::vec2 source = LensFlareHaloSourceUv( onRing, sun, radius, kAspect );
        EXPECT_NEAR( source.x, sun.x, 1e-4f ) << "bearing " << angle;
        EXPECT_NEAR( source.y, sun.y, 1e-4f ) << "bearing " << angle;
    }
}

TEST( LensFlareHalo, RingWeightPeaksOnTheRingAndVanishesAtTheSun )
{
    const glm::vec2 sun( 0.4f, 0.45f );
    const float     radius = 0.18f;

    const glm::vec2 atSun  = sun;
    const glm::vec2 onRing = sun + glm::vec2( radius / kAspect, 0.0f );
    const glm::vec2 wayOut = sun + glm::vec2( ( radius * 2.5f ) / kAspect, 0.0f );

    const float centreW = LensFlareHaloWeight( atSun, sun, radius, kAspect );
    const float ringW   = LensFlareHaloWeight( onRing, sun, radius, kAspect );
    const float outW    = LensFlareHaloWeight( wayOut, sun, radius, kAspect );

    EXPECT_NEAR( ringW, 1.0f, 1e-4f );
    EXPECT_LT( centreW, 0.01f ); // the halo is a RING — it must not fill the sun
    EXPECT_FLOAT_EQ( outW, 0.0f );
}

TEST( LensFlareHalo, IsACircleAtAnyAspectRatio )
{
    // Measured in raw uv a ring would be an ellipse on a 16:9 screen. The aspect correction is the only
    // thing making it round, so it is asserted at two very different shapes.
    const glm::vec2 sun( 0.5f, 0.5f );
    const float     radius = 0.2f;

    for ( float aspect : { 1.0f, 16.0f / 9.0f, 21.0f / 9.0f } )
    {
        for ( float angle = 0.0f; angle < 6.2f; angle += 0.9f )
        {
            const glm::vec2 bearing( std::cos( angle ), std::sin( angle ) );
            const glm::vec2 onRing = sun + ( bearing * radius ) / glm::vec2( aspect, 1.0f );
            EXPECT_NEAR( LensFlareHaloWeight( onRing, sun, radius, aspect ), 1.0f, 1e-4f )
                 << "aspect " << aspect << " angle " << angle;
        }
    }
}

// --- Streak -----------------------------------------------------------------------------------------

TEST( LensFlareStreak, TapsSpanTheAxisSymmetricallyAndPeakAtThePixel )
{
    const float taps = 64.0f;

    EXPECT_FLOAT_EQ( LensFlareStreakTapOffset( 0.0f, taps ), -1.0f );
    EXPECT_FLOAT_EQ( LensFlareStreakTapOffset( taps - 1.0f, taps ), 1.0f );

    // Symmetric about the pixel, and the triangular kernel is heaviest there.
    const float middle = LensFlareStreakTapOffset( ( taps - 1.0f ) * 0.5f, taps );
    EXPECT_NEAR( middle, 0.0f, 1e-5f );
    EXPECT_NEAR( LensFlareStreakTapWeight( 0.0f ), 1.0f, 1e-6f );
    EXPECT_NEAR( LensFlareStreakTapWeight( 1.0f ), 0.0f, 1e-6f );
    EXPECT_NEAR( LensFlareStreakTapWeight( -0.5f ), LensFlareStreakTapWeight( 0.5f ), 1e-6f );
}

TEST( LensFlareStreak, ConsecutiveTapsOverlapTheSunDiscRatherThanStampingCopiesOfIt )
{
    // THE other bug this pins. The streak is a convolution of the source with its axis, and it only
    // reads as a line if consecutive taps land closer together than the source feature is wide. At 16
    // taps over a 0.35 half-length the step was 0.047 in uv against a sun about 0.02 wide, and the
    // streak rendered as a row of separate squares — 16 copies of the sun. This asserts the relation
    // that has to hold instead, at the tap count the shader actually uses.
    constexpr float kShaderTaps = 64.0f; // kStreakTaps in LensFlareFeatures.shader
    constexpr float kSunUvWidth = 0.02f; // the 0.545-degree disc at the shot field of view

    const glm::vec2 uv( 0.5f, 0.5f );
    const glm::vec2 axis( 1.0f, 0.0f );
    const float     length = 0.35f;

    float widest = 0.0f;
    for ( float k = 1.0f; k < kShaderTaps; k += 1.0f )
    {
        const glm::vec2 a =
             LensFlareStreakTapUv( uv, axis, LensFlareStreakTapOffset( k - 1.0f, kShaderTaps ), length, kAspect );
        const glm::vec2 b =
             LensFlareStreakTapUv( uv, axis, LensFlareStreakTapOffset( k, kShaderTaps ), length, kAspect );
        widest = std::max( widest, Dist( a, b, kAspect ) );
    }

    EXPECT_LT( widest, kSunUvWidth )
         << "streak taps are further apart than the sun is wide — the streak will render as separate "
            "copies of the disc rather than one smear";
}

TEST( LensFlareStreak, FollowsTheAuthoredAxisAndIsCircularlyMeasured )
{
    // A vertical streak must be vertical, and its extent must be the same length as a horizontal one —
    // the aspect correction applies to the axis too, or a 90-degree Streak Angle would be shorter.
    const glm::vec2 uv( 0.5f, 0.5f );
    const float     length = 0.3f;

    const glm::vec2 h = LensFlareStreakTapUv( uv, glm::vec2( 1.0f, 0.0f ), 1.0f, length, kAspect );
    const glm::vec2 v = LensFlareStreakTapUv( uv, glm::vec2( 0.0f, 1.0f ), 1.0f, length, kAspect );

    EXPECT_NEAR( h.y, uv.y, 1e-6f );
    EXPECT_NEAR( v.x, uv.x, 1e-6f );
    EXPECT_NEAR( Dist( h, uv, kAspect ), Dist( v, uv, kAspect ), 1e-5f );
}

// --- The gate ---------------------------------------------------------------------------------------

TEST( LensFlareStrengthRule, IsZeroForASunBehindTheCamera )
{
    // The single property the whole effect is judged on. ComputeSunScreen returns Fade 0 behind the
    // camera; LensFlareStrength must turn that into no flare at all, whatever the authored intensity.
    const glm::mat4 projection = glm::perspective( glm::radians( 60.0f ), 16.0f / 9.0f, 0.1f, 10000.0f );
    const glm::mat4 view =
         glm::lookAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );

    const SunScreen behind = ComputeSunScreen( projection * view, glm::vec3( 0.0f, 0.0f, 1.0f ) );
    ASSERT_FLOAT_EQ( behind.Fade, 0.0f );
    EXPECT_FLOAT_EQ( LensFlareStrength( behind.Fade, 5.0f ), 0.0f );

    const SunScreen ahead = ComputeSunScreen( projection * view, glm::vec3( 0.0f, 0.0f, -1.0f ) );
    ASSERT_FLOAT_EQ( ahead.Fade, 1.0f );
    EXPECT_GT( LensFlareStrength( ahead.Fade, 0.35f ), 0.0f );
}

TEST( LensFlareStrengthRule, NeverSubtractsLightFromTheFrame )
{
    // The flare is added in HDR before the tonemap. A negative intensity, however it got there, must
    // dim the flare to nothing rather than darken the scene.
    EXPECT_FLOAT_EQ( LensFlareStrength( 1.0f, -2.0f ), 0.0f );
    EXPECT_FLOAT_EQ( LensFlareStrength( -1.0f, 2.0f ), 0.0f );
}

TEST( LensFlareStrengthRule, RisesWithTheSunsScreenFadeSoTheFlareLeavesWithTheSun )
{
    float previous = -1.0f;
    for ( float fade = 0.0f; fade <= 1.0f; fade += 0.25f )
    {
        const float strength = LensFlareStrength( fade, 0.35f );
        EXPECT_GT( strength, previous );
        previous = strength;
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
