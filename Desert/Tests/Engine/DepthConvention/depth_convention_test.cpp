// The engine's depth convention: REVERSED-Z, zero-to-one.
//
// This file exists because the convention is not one value in one place — it is an agreement between a
// projection matrix, a clear value, a compare op, a frustum derivation and a shader-language mapping,
// each of which is individually plausible when wrong. Every test below asserts a RELATION between two
// of them rather than the value of either, because a value test passes on both sides of a swap.
//
// The history it guards: the engine rendered a GL-convention [-1,1] projection into a Vulkan [0,1] clip
// volume for its whole life. It looked fine, because the discarded half only covered the first 20 cm in
// front of the lens — but it threw away half the depth buffer, and a 1 km far plane was the most that
// standard-Z on a UNORM24 attachment could carry without z-fighting.

#include <Engine/Core/Frustum.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/PipelineCache.hpp>

#include "ViewRayReference.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using Desert::Core::kDefaultFarPlane;
using Desert::Core::kDefaultNearPlane;
using Desert::Core::kDepthClear;
using Desert::Core::kDepthNear;
using Desert::Core::MakeOrthographic;
using Desert::Core::MakePerspective;

using Desert::Core::Formats::ShaderRenderState;
using Desert::Core::Formats::StateCompare;
using Desert::Graphic::ApplyShaderRenderState;
using Desert::Graphic::CompareOp;
using Desert::Graphic::GraphicsPipelineSpecification;

namespace
{
    // Device depth of a point `distance` in front of a camera at the origin looking down -Z.
    float DeviceDepth( const glm::mat4& projection, float distance )
    {
        const glm::vec4 clip = projection * glm::vec4( 0.0f, 0.0f, -distance, 1.0f );
        return clip.z / clip.w;
    }

    const std::vector<StateCompare>& AllCompares()
    {
        static const std::vector<StateCompare> all = { StateCompare::Never,          StateCompare::Less,
                                                       StateCompare::Equal,          StateCompare::LessOrEqual,
                                                       StateCompare::Greater,        StateCompare::NotEqual,
                                                       StateCompare::GreaterOrEqual, StateCompare::Always };
        return all;
    }

    // Would `op` let a fragment storing `incoming` pass against a buffer holding `stored`?
    bool Passes( CompareOp op, float incoming, float stored )
    {
        switch ( op )
        {
            case CompareOp::Never:
                return false;
            case CompareOp::Less:
                return incoming < stored;
            case CompareOp::Equal:
                return incoming == stored;
            case CompareOp::LessOrEqual:
                return incoming <= stored;
            case CompareOp::Greater:
                return incoming > stored;
            case CompareOp::NotEqual:
                return incoming != stored;
            case CompareOp::GreaterOrEqual:
                return incoming >= stored;
            case CompareOp::Always:
                return true;
        }
        return false;
    }

    // The same question, phrased the way a shader author phrases it: the DSL's compare applied to
    // DISTANCES, where "Less" plainly means "the nearer fragment wins".
    bool PassesByDistance( StateCompare authored, float incoming, float stored )
    {
        switch ( authored )
        {
            case StateCompare::Never:
                return false;
            case StateCompare::Less:
                return incoming < stored;
            case StateCompare::Equal:
                return incoming == stored;
            case StateCompare::LessOrEqual:
                return incoming <= stored;
            case StateCompare::Greater:
                return incoming > stored;
            case StateCompare::NotEqual:
                return incoming != stored;
            case StateCompare::GreaterOrEqual:
                return incoming >= stored;
            case StateCompare::Always:
                return true;
        }
        return false;
    }

    CompareOp MappedDepthCompare( StateCompare authored )
    {
        ShaderRenderState state;
        state.DepthCompare = authored;
        GraphicsPipelineSpecification spec;
        ApplyShaderRenderState( spec, state );
        return spec.DepthCompareOp;
    }
} // namespace

// ---- The projection ---------------------------------------------------------------------------------

TEST( DepthConvention, PerspectivePutsOneAtTheNearPlaneAndZeroAtTheFar )
{
    const glm::mat4 p = MakePerspective( glm::radians( 60.0f ), 16.0f / 9.0f, 10.0f, 500000.0f );

    EXPECT_NEAR( DeviceDepth( p, 10.0f ), kDepthNear, 1e-5f );
    EXPECT_NEAR( DeviceDepth( p, 500000.0f ), kDepthClear, 1e-5f );
}

TEST( DepthConvention, OrthographicPutsOneAtTheNearPlaneAndZeroAtTheFar )
{
    const glm::mat4 p = MakeOrthographic( -100.0f, 100.0f, -100.0f, 100.0f, 10.0f, 500000.0f );

    EXPECT_NEAR( DeviceDepth( p, 10.0f ), kDepthNear, 1e-5f );
    EXPECT_NEAR( DeviceDepth( p, 500000.0f ), kDepthClear, 1e-5f );
}

// The property every depth test in the engine rests on, and the one a half-correct projection can still
// fail: depth must fall MONOTONICALLY with distance over the whole range, with no plateau. A projection
// that is merely "reversed at the endpoints" — say, a standard one negated — satisfies the two tests
// above and breaks here.
TEST( DepthConvention, DepthFallsMonotonicallyWithDistance )
{
    for ( const glm::mat4& p : { MakePerspective( glm::radians( 60.0f ), 1.7778f, 10.0f, 5000000.0f ),
                                 MakeOrthographic( -1.0f, 1.0f, -1.0f, 1.0f, 10.0f, 5000000.0f ) } )
    {
        float previous = 2.0f;
        for ( float d = 10.0f; d <= 5000000.0f; d *= 1.5f )
        {
            const float z = DeviceDepth( p, d );
            EXPECT_LT( z, previous ) << "at distance " << d;
            EXPECT_GE( z, -1e-6f ) << "at distance " << d; // never leaves the clip volume
            EXPECT_LE( z, 1.0f + 1e-6f ) << "at distance " << d;
            previous = z;
        }
    }
}

// EVERYTHING IN FRONT OF THE NEAR PLANE IS INSIDE THE CLIP VOLUME. This is what the old GL-convention
// projection got wrong under Vulkan: half of [-1,1] lay below 0 and was discarded, so the buffer carried
// only the far half of its own range. Here nothing between near and far may fall outside [0,1].
TEST( DepthConvention, TheWholeRangeLandsInsideVulkansClipVolume )
{
    const glm::mat4 p = MakePerspective( glm::radians( 60.0f ), 1.7778f, kDefaultNearPlane, kDefaultFarPlane );

    // A tenth of the way from the near plane to the far one, geometrically — the region that used to be
    // thrown away entirely.
    for ( float d = kDefaultNearPlane; d <= kDefaultFarPlane; d *= 2.0f )
    {
        const float z = DeviceDepth( p, d );
        EXPECT_GE( z, 0.0f ) << "at distance " << d;
        EXPECT_LE( z, 1.0f ) << "at distance " << d;
    }
}

// ---- What the reversal is FOR -----------------------------------------------------------------------

// The claim in Core/Projection.hpp is that reversed-Z on a float attachment keeps the RELATIVE depth
// error near-constant across the range, and that standard-Z does not. This measures both: perturb the
// stored depth by one float ULP, invert the projection, and ask how far the surface moved relative to
// its own distance.
//
// Not a spot value — a BOUND over the whole range, plus the same bound failing for the convention this
// change replaced. A test of reversed-Z alone would pass just as well if the maths were subtly wrong.
namespace
{
    // z(d) for the two zero-to-one conventions. The reversed form is exactly what MakePerspective
    // produces — the tests above pin that against the real matrix, so this closed form is allowed to
    // stand in for it here, where what is being measured is the arithmetic and not the matrix.
    double DepthAtDistance( double nearPlane, double farPlane, double d, bool reversed )
    {
        const double standard = ( farPlane * ( d - nearPlane ) ) / ( d * ( farPlane - nearPlane ) );
        return reversed ? 1.0 - standard : standard;
    }

    // ...and its exact inverse, so the measurement below cannot be limited by a search's resolution.
    double DistanceAtDepth( double nearPlane, double farPlane, double z, bool reversed )
    {
        const double span = farPlane - nearPlane;
        if ( reversed )
            return ( nearPlane * farPlane ) / ( z * span + nearPlane );
        return ( farPlane * nearPlane ) / ( farPlane - z * span );
    }

    // World-space distance error of ONE FLOAT ULP of stored depth, at `distance`, relative to distance.
    // The depth is quantized to float because that is what a DEPTH32F attachment holds; everything else
    // is double so the answer is about the buffer and not about the arithmetic measuring it.
    double RelativeDepthErrorAt( double nearPlane, double farPlane, double distance, bool reversed )
    {
        const float z    = static_cast<float>( DepthAtDistance( nearPlane, farPlane, distance, reversed ) );
        const float zAlt = std::nextafter( z, 2.0f );

        const double d0 = DistanceAtDepth( nearPlane, farPlane, z, reversed );
        const double d1 = DistanceAtDepth( nearPlane, farPlane, zAlt, reversed );
        return std::abs( d1 - d0 ) / distance;
    }
} // namespace

TEST( DepthConvention, ReversedZKeepsTheRelativeErrorBoundedAndStandardZDoesNot )
{
    const double nearPlane = kDefaultNearPlane;
    const double farPlane  = kDefaultFarPlane;

    double worstReversed = 0.0;
    double worstStandard = 0.0;
    for ( double d = 100.0; d <= farPlane * 0.5; d *= 2.0 )
    {
        worstReversed = std::max( worstReversed, RelativeDepthErrorAt( nearPlane, farPlane, d, true ) );
        worstStandard = std::max( worstStandard, RelativeDepthErrorAt( nearPlane, farPlane, d, false ) );
    }

    // A thousandth of a percent, over five decades of distance. The real figure is tighter still; this is
    // a ceiling a correct implementation clears by orders of magnitude and a wrong one does not.
    EXPECT_LT( worstReversed, 1e-5 );

    // And the comparison that justifies the whole change: at the same near/far, on the same float
    // storage, standard-Z is worse — by orders of magnitude, and worst exactly where scenery is.
    EXPECT_GT( worstStandard, worstReversed * 1000.0 );
}

// ---- The shader-DSL mapping -------------------------------------------------------------------------

// THE ONE THIS FILE WAS WRITTEN FOR. A .shader authors `ZTest Less` meaning "the nearer fragment wins".
// Under reversed-Z the arithmetic test that implements that is Greater. The mapping in PipelineCache.hpp
// is therefore NOT the identity, and nothing else in the engine would notice if it became one: every
// affected material would simply render behind the world.
//
// Asserted as an equivalence over DISTANCES rather than as a table of pairs, so it stays true for
// reasons rather than by transcription.
TEST( DepthConvention, TheDslDepthTestMeansTheSameThingItWouldUnderStandardZ )
{
    const glm::mat4 projection =
         MakePerspective( glm::radians( 60.0f ), 1.7778f, kDefaultNearPlane, kDefaultFarPlane );

    const float distances[] = { 50.0f, 500.0f, 5000.0f, 50000.0f, 500000.0f };

    for ( StateCompare authored : AllCompares() )
    {
        const CompareOp mapped = MappedDepthCompare( authored );

        for ( float incoming : distances )
        {
            for ( float stored : distances )
            {
                const bool expected = PassesByDistance( authored, incoming, stored );
                const bool actual =
                     Passes( mapped, DeviceDepth( projection, incoming ), DeviceDepth( projection, stored ) );
                EXPECT_EQ( expected, actual ) << "authored " << static_cast<int>( authored ) << ", incoming "
                                              << incoming << ", stored " << stored;
            }
        }
    }
}

// Mirroring is an involution: applying it twice must give back what was authored. This catches a mapping
// that is wrong only for one direction (Less mirrored but Greater left alone), which the equivalence
// above would also catch — but this one says so in one line and cannot be satisfied by luck.
TEST( DepthConvention, TheDepthMirrorIsItsOwnInverse )
{
    // The round trip needs to read a CompareOp back as the StateCompare a shader would have authored,
    // which is only meaningful while the two enumerations are declared in the same order. Say so, so
    // that reordering either one fails to compile instead of quietly testing nothing.
    static_assert( static_cast<int>( StateCompare::Less ) == static_cast<int>( CompareOp::Less ) );
    static_assert( static_cast<int>( StateCompare::Greater ) == static_cast<int>( CompareOp::Greater ) );
    static_assert( static_cast<int>( StateCompare::LessOrEqual ) == static_cast<int>( CompareOp::LessOrEqual ) );
    static_assert( static_cast<int>( StateCompare::GreaterOrEqual ) ==
                   static_cast<int>( CompareOp::GreaterOrEqual ) );
    static_assert( static_cast<int>( StateCompare::Always ) == static_cast<int>( CompareOp::Always ) );

    const auto asStateCompare = []( CompareOp op ) { return static_cast<StateCompare>( static_cast<int>( op ) ); };

    for ( StateCompare authored : AllCompares() )
        EXPECT_EQ( authored,
                   asStateCompare( MappedDepthCompare( asStateCompare( MappedDepthCompare( authored ) ) ) ) );
}

// The stencil buffer has NO depth convention, so its compare must pass through untouched. If someone
// ever "tidies" the two switches in PipelineCache.hpp into one shared helper, this is what stops it.
TEST( DepthConvention, StencilComparesAreNotMirrored )
{
    ShaderRenderState state;
    state.StencilTest    = true;
    state.StencilCompare = StateCompare::Less;

    GraphicsPipelineSpecification spec;
    ApplyShaderRenderState( spec, state );

    EXPECT_EQ( spec.StencilFront.CompareOp, CompareOp::Less );
    EXPECT_EQ( spec.StencilBack.CompareOp, CompareOp::Less );
}

// The default a pass gets when it says nothing at all has to be the reversed-Z "nearer wins", or every
// renderer that never touches DepthCompareOp draws its geometry behind the world.
TEST( DepthConvention, ThePipelineDefaultIsNearerWins )
{
    const GraphicsPipelineSpecification spec;
    EXPECT_TRUE( Passes( spec.DepthCompareOp, kDepthNear, kDepthClear ) );  // near beats cleared
    EXPECT_FALSE( Passes( spec.DepthCompareOp, kDepthClear, kDepthNear ) ); // far does not beat near
}

// ---- The frustum ------------------------------------------------------------------------------------

// Frustum::Rebuild derives near and far from the clip-space half-spaces, and those are the only two of
// the six planes that depend on the convention. Read the GL way under reversed-Z the pair comes out
// swapped and REJECTS the entire visible range — which is silent, because nothing in the engine
// currently culls with it.
TEST( DepthConvention, TheFrustumAcceptsTheVisibleRangeAndRejectsOutsideIt )
{
    const glm::mat4 projection = MakePerspective( glm::radians( 60.0f ), 1.0f, 100.0f, 100000.0f );
    const glm::mat4 view =
         glm::lookAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );

    const Desert::Core::Frustum frustum( projection, view );

    // Comfortably inside, at three depths spanning the range.
    EXPECT_TRUE( frustum.IsInside( glm::vec3( 0.0f, 0.0f, -200.0f ) ) );
    EXPECT_TRUE( frustum.IsInside( glm::vec3( 0.0f, 0.0f, -10000.0f ) ) );
    EXPECT_TRUE( frustum.IsInside( glm::vec3( 0.0f, 0.0f, -90000.0f ) ) );

    // Nearer than the near plane, past the far plane, and behind the camera.
    EXPECT_FALSE( frustum.IsInside( glm::vec3( 0.0f, 0.0f, -50.0f ) ) );
    EXPECT_FALSE( frustum.IsInside( glm::vec3( 0.0f, 0.0f, -110000.0f ) ) );
    EXPECT_FALSE( frustum.IsInside( glm::vec3( 0.0f, 0.0f, 200.0f ) ) );

    // And outside the sides, so the near/far fix cannot have been made by loosening everything.
    EXPECT_FALSE( frustum.IsInside( glm::vec3( 100000.0f, 0.0f, -200.0f ) ) );
    EXPECT_FALSE( frustum.IsInside( glm::vec3( 0.0f, 100000.0f, -200.0f ) ) );
}

// ------------------------------------------------------------------------------------------------
// THE BACKGROUND RAY. Reconstructing it is an agreement with the projection above: the sky passes
// hand `WorldViewRay` a clip z of 1.0, which is the NEAR plane only because this engine is reversed-Z.
//
// The three tests below are on Editor/Resources/Shaders/Common/ViewRay.glslh compiled AS C++ (see
// ViewRayReference.hpp), so they are statements about the text the GPU runs.
//
// WHY THEY EXIST. The HDR skybox reconstructed its ray as `inverse( Projection * View ) * clip` and
// read `.xyz` with no perspective divide, which adds `cameraPos / near`. With a 10 cm near plane a
// camera 200 cm up contributed a term twenty times longer than the ray, so every pixel of the sky
// sampled one direction — the camera's own position — and the cubemap showed ONE face on all sides.
// No frame taken from a camera at the origin can see that, and no frame at all can see it as a rule.
// ------------------------------------------------------------------------------------------------

namespace
{
    // What a full-screen-quad sky vertex stage actually passes in: NDC xy, near-plane z, w = 1.
    glm::vec4 SkyQuadClip( float ndcX, float ndcY )
    {
        return { ndcX, ndcY, kDepthNear, 1.0f };
    }

    glm::mat4 ViewAt( const glm::vec3& eye, const glm::vec3& forward )
    {
        return glm::lookAt( eye, eye + forward, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    }

    // The five points a sky is judged at: centre and the four corners of the screen.
    const std::vector<glm::vec2>& ScreenCorners()
    {
        static const std::vector<glm::vec2> pts = { { 0.0f, 0.0f },
                                                    { -1.0f, -1.0f },
                                                    { 1.0f, -1.0f },
                                                    { -1.0f, 1.0f },
                                                    { 1.0f, 1.0f } };
        return pts;
    }
} // namespace

// THE DEFECT ITSELF, as a rule. Same orientation, four wildly different camera positions: a background
// at infinity has no parallax, so the ray must be bit-for-bit the same function of the pixel.
TEST( DepthConvention, TheSkyRayIgnoresWhereTheCameraStands )
{
    const glm::mat4 projection =
         MakePerspective( glm::radians( 45.0f ), 16.0f / 9.0f, kDefaultNearPlane, kDefaultFarPlane );
    const glm::vec3 forward( 0.0f, 0.0f, -1.0f );

    const std::vector<glm::vec3> eyes = { { 0.0f, 0.0f, 0.0f },
                                          { 0.0f, 200.0f, 0.0f },     // the probe scene's camera
                                          { -3000.0f, 1500.0f, 4200.0f },
                                          { 0.0f, 500000.0f, 0.0f } }; // 5 km up, still the same sky

    for ( const glm::vec2& ndc : ScreenCorners() )
    {
        const glm::vec3 reference = Desert::Tests::ViewRayRef::WorldViewRay(
             projection, ViewAt( eyes.front(), forward ), SkyQuadClip( ndc.x, ndc.y ) );

        for ( const glm::vec3& eye : eyes )
        {
            const glm::vec3 ray = Desert::Tests::ViewRayRef::WorldViewRay(
                 projection, ViewAt( eye, forward ), SkyQuadClip( ndc.x, ndc.y ) );

            // Absolute, not relative: the ray's own length is about the near plane's, so a tolerance
            // proportional to the eye's distance would grow to admit exactly the defect being excluded.
            EXPECT_NEAR( ray.x, reference.x, 1e-3f ) << "ndc " << ndc.x << ',' << ndc.y << " eye " << eye.y;
            EXPECT_NEAR( ray.y, reference.y, 1e-3f ) << "ndc " << ndc.x << ',' << ndc.y << " eye " << eye.y;
            EXPECT_NEAR( ray.z, reference.z, 1e-3f ) << "ndc " << ndc.x << ',' << ndc.y << " eye " << eye.y;
        }
    }
}

// The other half, and it has to be here: a reconstruction that returned a constant would pass the test
// above perfectly. Through the middle of the screen the ray IS the direction the camera looks.
TEST( DepthConvention, TheSkyRayFollowsWhereTheCameraLooks )
{
    const glm::mat4 projection =
         MakePerspective( glm::radians( 45.0f ), 16.0f / 9.0f, kDefaultNearPlane, kDefaultFarPlane );
    const glm::vec3 eye( 0.0f, 200.0f, 0.0f );

    const std::vector<glm::vec3> forwards = { { 0.0f, 0.0f, -1.0f },  // -Z
                                              { 1.0f, 0.0f, 0.0f },   // +X
                                              { 0.0f, 0.0f, 1.0f },   // +Z
                                              { -1.0f, 0.0f, 0.0f },  // -X
                                              { 0.0f, 0.99f, -0.14f } }; // near the zenith

    for ( const glm::vec3& forward : forwards )
    {
        const glm::vec3 ray = glm::normalize( Desert::Tests::ViewRayRef::WorldViewRay(
             projection, ViewAt( eye, forward ), SkyQuadClip( 0.0f, 0.0f ) ) );
        const glm::vec3 want = glm::normalize( forward );

        EXPECT_NEAR( glm::dot( ray, want ), 1.0f, 1e-4f )
             << "centre ray " << ray.x << ',' << ray.y << ',' << ray.z;
    }
}

// And that the projection is genuinely consulted: the top edge of the screen is half the vertical field
// of view away from the centre. An identity in place of `inverse( projection )` passes both tests above.
TEST( DepthConvention, TheSkyRaySpreadsByTheFieldOfView )
{
    const float     fovY       = glm::radians( 45.0f );
    const glm::mat4 projection = MakePerspective( fovY, 16.0f / 9.0f, kDefaultNearPlane, kDefaultFarPlane );
    const glm::mat4 view       = ViewAt( glm::vec3( 0.0f, 200.0f, 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ) );

    const glm::vec3 centre =
         glm::normalize( Desert::Tests::ViewRayRef::WorldViewRay( projection, view, SkyQuadClip( 0.0f, 0.0f ) ) );
    const glm::vec3 top =
         glm::normalize( Desert::Tests::ViewRayRef::WorldViewRay( projection, view, SkyQuadClip( 0.0f, 1.0f ) ) );

    EXPECT_NEAR( std::acos( std::clamp( glm::dot( centre, top ), -1.0f, 1.0f ) ), fovY * 0.5f, 1e-4f );

    // The horizontal half-angle follows from the aspect, which is what makes this a test of the matrix
    // and not of one number: atan( aspect * tan( fovY/2 ) ).
    const glm::vec3 right =
         glm::normalize( Desert::Tests::ViewRayRef::WorldViewRay( projection, view, SkyQuadClip( 1.0f, 0.0f ) ) );
    const float wantX = std::atan( ( 16.0f / 9.0f ) * std::tan( fovY * 0.5f ) );
    EXPECT_NEAR( std::acos( std::clamp( glm::dot( centre, right ), -1.0f, 1.0f ) ), wantX, 1e-4f );
}

// The ray's LENGTH is a claim too, and without this the perspective divide inside WorldViewRay is
// unobservable: dividing a direction by a positive constant leaves the direction alone, so the divide
// survived a mutation that removed it entirely. What it buys is that the ray lands exactly ON the near
// plane — its component along the view axis is the near distance for EVERY pixel, which is what makes
// the value a near-plane offset rather than an arbitrarily scaled arrow, and what keeps the procedural
// sky's arithmetic unchanged now that it shares this text.
TEST( DepthConvention, TheSkyRayReachesTheNearPlaneAndNoFurther )
{
    const glm::mat4 projection =
         MakePerspective( glm::radians( 45.0f ), 16.0f / 9.0f, kDefaultNearPlane, kDefaultFarPlane );
    const glm::vec3 forward( 0.0f, 0.0f, -1.0f );
    const glm::mat4 view = ViewAt( glm::vec3( 0.0f, 200.0f, 0.0f ), forward );

    for ( const glm::vec2& ndc : ScreenCorners() )
    {
        const glm::vec3 ray =
             Desert::Tests::ViewRayRef::WorldViewRay( projection, view, SkyQuadClip( ndc.x, ndc.y ) );

        EXPECT_NEAR( glm::dot( ray, glm::normalize( forward ) ), kDefaultNearPlane, 1e-3f )
             << "ndc " << ndc.x << ',' << ndc.y;
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
