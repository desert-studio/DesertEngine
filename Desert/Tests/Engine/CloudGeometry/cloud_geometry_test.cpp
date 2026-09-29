// The geometry of the cloud shell, tested against the geometry it claims to be.
//
// Common/CloudGeometry.glslh is compiled here AS C++ (CloudGeometryReference.hpp), so every assertion
// below is about the text the march compiles. The four things this suite exists for are the four a frame
// cannot show:
//
//   * THE ROOTS ARE SIGNED. A ray aimed away from the shell must come back negative and read as a miss.
//     Take the absolute value — the obvious simplification — and every ray "hits" the layer behind the
//     camera, which renders as cloud mirrored below the horizon and looks like a coverage problem.
//   * THE CONSTANT TERM IS FACTORED. At |o| and r both near 6360, dot(o,o) - r*r discards seven of the
//     eight digits float32 has. The factored form is metres where the naive one is tens of metres, and
//     the difference is only visible as jitter on a boundary near the horizon.
//   * THE SEGMENT IS THE SHELL, in all three camera cases, with "nothing to march" encoded as y <= x.
//   * THE HEIGHT FRACTION FOLLOWS THE CURVATURE. Measured from the planet centre, a sample twenty
//     kilometres away at the same ALTITUDE sits at the same fraction as one overhead. Measured on the
//     world Y — which agrees near the camera and is therefore what a close-up frame endorses — the whole
//     layer compresses into its own floor at the horizon.

#include "CloudGeometryReference.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Tests::CloudGeometryRef;

namespace
{
    // A planet and a layer with the component's own defaults in kilometres: 6360 km planet, base at
    // 5 km, ten kilometres thick. Written as numbers rather than read from the component because this
    // suite is about the geometry, not about the defaults — ComponentReflection owns those.
    constexpr float kPlanetKm = 6360.0f;
    constexpr float kBottomKm = 6365.0f;
    constexpr float kTopKm    = 6367.0f;

    CloudLayer MakeTestLayer()
    {
        return CloudMakeLayer( kPlanetKm, kBottomKm - kPlanetKm, kTopKm - kBottomKm );
    }

    // The same quadratic solved in double from the same float inputs. Everything it differs from the
    // shader by is float rounding inside the shader's own arithmetic, which is exactly what the precision
    // test is measuring.
    glm::dvec2 ExactRoots( vec3 originKm, vec3 rayDir, float radiusKm )
    {
        const glm::dvec3 o( originKm );
        const glm::dvec3 d( rayDir );
        const double     r = static_cast<double>( radiusKm );

        const double b    = 2.0 * glm::dot( o, d );
        const double c    = glm::dot( o, o ) - r * r;
        const double disc = b * b - 4.0 * c;

        if ( disc < 0.0 )
            return { 1.0, -1.0 };

        const double root = std::sqrt( disc );
        return { ( -b - root ) * 0.5, ( -b + root ) * 0.5 };
    }

    vec3 DirectionAtElevation( float degrees )
    {
        const float radians = degrees * 3.14159265358979323846f / 180.0f;
        return glm::normalize( vec3( std::cos( radians ), std::sin( radians ), 0.0f ) );
    }

    // The repository root, found by walking up from wherever the binary was started — the same approach
    // SettingConsumers uses, so neither has to be run from one exact directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Engine/Content/Shaders/Common/CloudParams.glslh" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // Reads `#define <name> <number>` out of a shader header. It used to be how the two step-schedule
    // constants were reached at all — they were declared in Common/CloudParams.glslh, which cannot be
    // compiled as C++ because it declares a uniform block, so a test could only read the digits as text.
    // They now live in Common/CloudGeometry.glslh and this suite COMPILES them; the parser survives to
    // assert the other half of that move, that no second copy was left behind.
    double ParseDefine( const std::string& text, const std::string& name )
    {
        const std::string needle = "#define " + name;
        const std::size_t at     = text.find( needle );
        if ( at == std::string::npos )
            return std::nan( "" );

        // strtod rather than a stream extraction, because the value carries GLSL's `f` suffix and the
        // stream's number grammar treats `f` as a hexadecimal digit: it swallows "2.0f", fails to convert
        // it, and hands back a silent zero.
        return std::strtod( text.c_str() + at + needle.size(), nullptr );
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// CloudRaySphere — signs and encodings
// ---------------------------------------------------------------------------------------------------

TEST( CloudGeometryRaySphere, ARayAimedAwayFromTheSphereProducesNEGATIVERootsAndNotAHitAhead )
{
    // The camera is above the shell and looking up. The line still crosses the sphere — behind the
    // camera — so this is NOT the disc < 0 case, and that is the whole point: the only thing separating
    // it from a hit is the SIGN. An implementation that took abs() of the roots here would report a
    // segment 12 700 km long starting three kilometres ahead.
    const vec3 origin( 0.0f, 6370.0f, 0.0f );
    const vec3 up( 0.0f, 1.0f, 0.0f );

    const vec2 roots = CloudRaySphere( origin, up, kTopKm );

    EXPECT_LE( roots.x, roots.y ) << "the line does cross the sphere, so this must not read as disc < 0";
    EXPECT_LT( roots.y, 0.0f ) << "both roots are behind the camera and must stay negative";
    EXPECT_LT( roots.x, roots.y ) << "nearest root first, and both behind";

    // And the same ray pointing the other way is a hit ahead, which is what makes the sign meaningful
    // rather than an accident of this configuration.
    const vec2 down = CloudRaySphere( origin, vec3( 0.0f, -1.0f, 0.0f ), kTopKm );
    EXPECT_GT( down.x, 0.0f );
    EXPECT_NEAR( down.x, 3.0f, 1e-3f );
}

TEST( CloudGeometryRaySphere, AMissIsEncodedAsXGreaterThanYAndAHitCanNeverProduceThatOrder )
{
    // The camera stands 6360 km from the centre and looks along the horizon at a sphere of 6355 — the
    // line passes outside it, so the discriminant is genuinely negative.
    const vec3 origin( 0.0f, kPlanetKm, 0.0f );
    const vec2 miss = CloudRaySphere( origin, vec3( 1.0f, 0.0f, 0.0f ), 6355.0f );
    EXPECT_GT( miss.x, miss.y ) << "a geometric miss must be reported in the order the callers test for";

    // Now sweep: every direction on a circle, against four radii spanning below, around and above the
    // camera. Whenever the exact double solution says the line meets the sphere, the float function must
    // return the roots in order; whenever it says it does not, it must return the miss encoding. The two
    // encodings must never be confusable, because CloudLayerIntersect distinguishes the cases by nothing
    // else.
    for ( int step = 0; step < 360; ++step )
    {
        const vec3 dir = DirectionAtElevation( static_cast<float>( step ) );

        for ( const float radius : { 6355.0f, 6360.5f, kBottomKm, kTopKm } )
        {
            const vec2       actual = CloudRaySphere( origin, dir, radius );
            const glm::dvec2 exact  = ExactRoots( origin, dir, radius );

            const bool actualMiss = actual.x > actual.y;
            const bool exactMiss  = exact.x > exact.y;

            EXPECT_EQ( actualMiss, exactMiss )
                 << "elevation " << step << " deg, radius " << radius << ": the float solution and the "
                 << "exact one disagree about whether the line meets the sphere at all";
        }
    }
}

TEST( CloudGeometryRaySphere, TheFactoredConstantTermResolvesTheShellToMETRESAtSixThousandKilometres )
{
    // WHY THE ORIGINS ARE AXIS-ALIGNED AND INTEGRAL. length() of (0, y, 0) is exact when y*y is exactly
    // representable, which 6360 and 6364 both are. That removes the ONE rounding this test is not about —
    // the length itself — and leaves the quadratic's own arithmetic, which is what the factored constant
    // term exists to protect.
    //
    // The measured worst case below is well under a metre. The naive dot(o,o) - r*r form, dropped into
    // the same configurations, is off by twenty-three metres on the horizontal ray against the 6364.5 km
    // shell — because there the whole discriminant IS the constant term, and the naive form computes it
    // by subtracting two numbers near 4.05e7 whose difference is 6364.
    struct Config
    {
        vec3  Origin;
        float RadiusKm;
    };

    const Config configs[] = {
         { vec3( 0.0f, 6364.0f, 0.0f ), 6364.5f }, // just inside the shell: the constant term is tiny
         { vec3( 0.0f, 6364.0f, 0.0f ), 6365.0f },     { vec3( 0.0f, 6364.0f, 0.0f ), 6370.0f },
         { vec3( 0.0f, kPlanetKm, 0.0f ), kBottomKm }, { vec3( 0.0f, kPlanetKm, 0.0f ), kTopKm },
         { vec3( 0.0f, kPlanetKm, 0.0f ), 6355.0f }, // the camera is OUTSIDE this one
    };

    double worstKm = 0.0;

    for ( const Config& config : configs )
    {
        for ( const float elevation : { -60.0f, -30.0f, -10.0f, -2.0f, 0.0f, 2.0f, 10.0f, 30.0f, 60.0f, 89.0f } )
        {
            const vec3 dir = DirectionAtElevation( elevation );

            const vec2       actual = CloudRaySphere( config.Origin, dir, config.RadiusKm );
            const glm::dvec2 exact  = ExactRoots( config.Origin, dir, config.RadiusKm );

            if ( exact.x > exact.y )
                continue; // a miss carries no roots to compare

            const double nearErr = std::abs( static_cast<double>( actual.x ) - exact.x );
            const double farErr  = std::abs( static_cast<double>( actual.y ) - exact.y );
            worstKm              = std::max( worstKm, std::max( nearErr, farErr ) );

            EXPECT_LT( nearErr, 0.002 ) << "radius " << config.RadiusKm << " elevation " << elevation;
            EXPECT_LT( farErr, 0.002 ) << "radius " << config.RadiusKm << " elevation " << elevation;
        }
    }

    std::printf( "[CloudGeometry] worst root error at ~6360 km: %.4f m\n", worstKm * 1000.0 );
}

// ---------------------------------------------------------------------------------------------------
// CloudLayerIntersect — the three camera cases, and the encoding that says there is nothing to do
// ---------------------------------------------------------------------------------------------------

TEST( CloudGeometryLayer, BelowTheLayerLookingUpEntersAtTheBaseAndLeavesAtTheTop )
{
    const CloudLayer layer = MakeTestLayer();

    // Half a kilometre above the ground, straight up. The base is 4.5 km away and the top 6.5.
    const vec3 origin( 0.0f, kPlanetKm + 0.5f, 0.0f );
    const vec2 segment = CloudLayerIntersect( layer, origin, vec3( 0.0f, 1.0f, 0.0f ) );

    EXPECT_NEAR( segment.x, 4.5f, 1e-2f );
    EXPECT_NEAR( segment.y, 6.5f, 1e-2f );
    EXPECT_GT( segment.y, segment.x ) << "there is something to march";
}

TEST( CloudGeometryLayer, InsideTheLayerTheSegmentStartsAtTheCameraAndReachesTheNearerShell )
{
    const CloudLayer layer = MakeTestLayer();

    // One kilometre up inside a two-kilometre shell.
    const vec3 origin( 0.0f, 6366.0f, 0.0f );

    const vec2 up = CloudLayerIntersect( layer, origin, vec3( 0.0f, 1.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( up.x, 0.0f ) << "the entry root is behind a camera that is already inside";
    EXPECT_NEAR( up.y, 1.0f, 1e-2f ) << "one kilometre to the top";

    const vec2 down = CloudLayerIntersect( layer, origin, vec3( 0.0f, -1.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( down.x, 0.0f );
    EXPECT_NEAR( down.y, 1.0f, 1e-2f ) << "one kilometre to the base";

    // Sideways from inside: the ray stays in the shell for a long chord and must not be clipped by the
    // inner sphere it never reaches.
    const vec2 sideways = CloudLayerIntersect( layer, origin, vec3( 1.0f, 0.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( sideways.x, 0.0f );
    EXPECT_GT( sideways.y, 100.0f ) << "the horizontal chord of a shell at 6366 km is over a hundred km";
}

TEST( CloudGeometryLayer, AboveTheLayerLookingDownEntersAtTheTopAndLeavesAtTheBase )
{
    const CloudLayer layer = MakeTestLayer();

    // Ten kilometres up, three above the layer, looking straight down.
    const vec3 origin( 0.0f, 6370.0f, 0.0f );
    const vec2 segment = CloudLayerIntersect( layer, origin, vec3( 0.0f, -1.0f, 0.0f ) );

    EXPECT_NEAR( segment.x, 3.0f, 1e-2f );
    EXPECT_NEAR( segment.y, 5.0f, 1e-2f );
}

TEST( CloudGeometryLayer, AboveTheLayerLookingUpHasNothingToMarch )
{
    const CloudLayer layer = MakeTestLayer();

    const vec3 origin( 0.0f, 6370.0f, 0.0f );
    const vec2 segment = CloudLayerIntersect( layer, origin, vec3( 0.0f, 1.0f, 0.0f ) );

    EXPECT_LE( segment.y, segment.x ) << "the whole layer is behind the camera; the caller tests y <= x";
}

TEST( CloudGeometryLayer, ARayThatLeavesTheAtmosphereEntirelyHasNothingToMarch )
{
    // Outside the outer shell and aimed away from the planet altogether: the outer sphere is not on the
    // line at all, which is the disc < 0 early-out.
    const CloudLayer layer = MakeTestLayer();

    const vec3 origin( 0.0f, 6380.0f, 0.0f );
    const vec2 segment = CloudLayerIntersect( layer, origin, vec3( 1.0f, 0.5f, 0.0f ) );

    EXPECT_LE( segment.y, segment.x );
}

// THIS TEST FAILS TODAY, AND THE FAILURE IS THE POINT.
//
// CloudLayerIntersect's own header lists three ways to get "nothing to march", and the second is "the ray
// is below the layer and aimed at the ground". The function does not produce it: there is no planet test
// anywhere in the file, and CloudLayer::PlanetRadiusKm is used only to build the two shell radii. A ray
// aimed at the ground from beneath the layer passes THROUGH the planet and meets both shells on the far
// side, so the segment comes back at ~12 725 km — the antipodal cloud deck.
//
// It is not harmless: the march clamps the segment to entry + MaxViewDistance, which keeps 12 725.5 to
// 12 727.5 km inside the window, so the far-side layer is marched at full quality. In any pixel where the
// depth buffer holds no geometry — a scene with no terrain, an ocean gap, the void under a floating
// island — that renders as cloud BELOW the horizon: the exact symptom the file's own header warns about
// as the consequence of taking abs() of the roots.
TEST( CloudGeometryLayer, ARayFromBelowTheLayerAimedAtTheGroundHasNothingToMarch )
{
    const CloudLayer layer = MakeTestLayer();

    const vec3 origin( 0.0f, kPlanetKm + 0.5f, 0.0f );
    const vec2 straightDown = CloudLayerIntersect( layer, origin, vec3( 0.0f, -1.0f, 0.0f ) );

    EXPECT_LE( straightDown.y, straightDown.x ) << "the segment returned starts at " << straightDown.x
                                                << " km, which is the layer on the far side of the planet";

    // A shallow downward ray — the ordinary case of looking a little below the horizon — is the same
    // defect and is the one a viewer actually sees.
    const vec2 shallow = CloudLayerIntersect( layer, origin, DirectionAtElevation( -20.0f ) );
    EXPECT_LE( shallow.y, shallow.x ) << "segment starts at " << shallow.x << " km";
}

TEST( CloudGeometryLayer, TheSegmentNeverStartsBehindTheCamera )
{
    // A relation rather than a case: whatever the geometry, the entry is clamped to zero, because the
    // march adds a jittered offset to it and steps forward. A negative entry would sample the shell
    // behind the viewer and blend it over the frame.
    const CloudLayer layer = MakeTestLayer();

    for ( const float altitude : { 0.1f, 1.0f, 5.5f, 6.0f, 6.9f, 12.0f } )
    {
        const vec3 origin( 0.0f, kPlanetKm + altitude, 0.0f );

        for ( int step = 0; step < 72; ++step )
        {
            const vec2 segment = CloudLayerIntersect( layer, origin, DirectionAtElevation( step * 5.0f ) );
            if ( segment.y <= segment.x )
                continue;

            EXPECT_GE( segment.x, 0.0f ) << "altitude " << altitude << ", elevation " << step * 5;
        }
    }
}

TEST( CloudGeometryLayer, EveryPointOfTheReturnedSegmentIsActuallyInsideTheShell )
{
    // The relation that gives the three case tests above their meaning: whatever the branch taken, the
    // interval handed back has to lie in the shell. Sampling the midpoint and both ends catches an
    // interval built from the wrong pair of roots, which is what the case analysis is easy to get wrong.
    const CloudLayer layer = MakeTestLayer();

    for ( const float altitude : { 0.1f, 1.0f, 5.5f, 6.0f, 6.9f, 12.0f } )
    {
        const vec3 origin( 0.0f, kPlanetKm + altitude, 0.0f );

        for ( int step = 0; step < 72; ++step )
        {
            const vec3 dir     = DirectionAtElevation( step * 5.0f );
            const vec2 segment = CloudLayerIntersect( layer, origin, dir );
            if ( segment.y <= segment.x )
                continue;

            // The far-side segment this suite reports separately would fail here too; skipping it keeps
            // this test about the shell membership rather than about the same defect twice.
            if ( segment.x > 1000.0f )
                continue;

            for ( const float where : { 0.02f, 0.5f, 0.98f } )
            {
                const float radius =
                     glm::length( origin + dir * ( segment.x + ( segment.y - segment.x ) * where ) );
                EXPECT_GE( radius, layer.BottomRadiusKm - 0.01f )
                     << "altitude " << altitude << ", elevation " << step * 5 << ", at " << where;
                EXPECT_LE( radius, layer.TopRadiusKm + 0.01f )
                     << "altitude " << altitude << ", elevation " << step * 5 << ", at " << where;
            }
        }
    }
}

TEST( CloudGeometryLayer, AZeroThicknessLayerIsFlooredRatherThanLeftDegenerate )
{
    // One hand-edited scene away. A shell of exactly zero thickness has coincident roots, and the step
    // schedule then divides by a segment of length zero.
    const CloudLayer layer = CloudMakeLayer( kPlanetKm, 5.0f, 0.0f );
    EXPECT_GT( layer.TopRadiusKm, layer.BottomRadiusKm );
    EXPECT_NEAR( layer.TopRadiusKm - layer.BottomRadiusKm, 0.001f, 1e-4f );
}

// ---------------------------------------------------------------------------------------------------
// CloudHeightFraction — the curvature, which is the whole reason the shell is not a slab
// ---------------------------------------------------------------------------------------------------

TEST( CloudGeometryHeight, ZeroAtTheBaseOneAtTheTopAndClampedOutside )
{
    const CloudLayer layer = MakeTestLayer();

    EXPECT_FLOAT_EQ( CloudHeightFraction( layer, vec3( 0.0f, kBottomKm, 0.0f ) ), 0.0f );
    EXPECT_FLOAT_EQ( CloudHeightFraction( layer, vec3( 0.0f, kTopKm, 0.0f ) ), 1.0f );
    EXPECT_NEAR( CloudHeightFraction( layer, vec3( 0.0f, 0.5f * ( kBottomKm + kTopKm ), 0.0f ) ), 0.5f, 1e-3f );

    EXPECT_FLOAT_EQ( CloudHeightFraction( layer, vec3( 0.0f, kPlanetKm, 0.0f ) ), 0.0f ) << "below the base";
    EXPECT_FLOAT_EQ( CloudHeightFraction( layer, vec3( 0.0f, 6400.0f, 0.0f ) ), 1.0f ) << "above the top";
}

TEST( CloudGeometryHeight, ASampleTwentyKilometresAwayAtTheSameALTITUDEHasTheSameHeightFraction )
{
    // THE TEST THE FLAT-Y IMPLEMENTATION FAILS. Twenty kilometres along the ground is an angle of
    // 20/6366 radians at the planet centre; a point at the same altitude there has dropped 31 metres in
    // world Y, which on a two-kilometre layer is 0.0157 of the fraction — ten times the tolerance below.
    // At fifty kilometres it is 0.098, and by the horizon the whole layer has collapsed into its floor.
    const CloudLayer layer = MakeTestLayer();

    const float overheadRadius   = 6366.0f; // the middle of the shell
    const float overheadFraction = CloudHeightFraction( layer, vec3( 0.0f, overheadRadius, 0.0f ) );
    ASSERT_NEAR( overheadFraction, 0.5f, 1e-3f );

    for ( const float groundDistanceKm : { 5.0f, 20.0f, 50.0f, 200.0f } )
    {
        const float angle = groundDistanceKm / overheadRadius;
        const vec3  away( overheadRadius * std::sin( angle ), overheadRadius * std::cos( angle ), 0.0f );

        EXPECT_NEAR( CloudHeightFraction( layer, away ), overheadFraction, 2e-3f )
             << groundDistanceKm << " km away at the same altitude must be at the same height fraction; "
             << "its world Y is " << away.y << " and reading THAT as the altitude is the flat-slab bug";
    }
}

TEST( CloudGeometryHeight, TheFractionRisesWithAltitudeAndNeverLeavesZeroToOne )
{
    const CloudLayer layer = MakeTestLayer();

    float previous = -1.0f;
    for ( int step = 0; step <= 100; ++step )
    {
        const float radius   = kPlanetKm + 0.1f * static_cast<float>( step );
        const float fraction = CloudHeightFraction( layer, vec3( 0.0f, radius, 0.0f ) );

        EXPECT_GE( fraction, 0.0f );
        EXPECT_LE( fraction, 1.0f );
        EXPECT_GE( fraction, previous ) << "radius " << radius;
        previous = fraction;
    }
}

// ---------------------------------------------------------------------------------------------------
// CloudStepCount — a schedule that has to be predictable, and two constants that live in another file
// ---------------------------------------------------------------------------------------------------

namespace
{
    // The component's Max Steps default, which is the quality tier the shipped library is calibrated
    // against. ComponentReflection owns the default itself; this suite owns what the schedule does at it.
    constexpr float kMaxCount = 256.0f;
} // namespace

TEST( CloudGeometrySteps, TheScheduleIsDeclaredWhereItIsConsumedAndNowhereElse )
{
    // THE MOVE, ASSERTED. All four schedule constants are declared in the one file this suite compiles,
    // and Common/CloudParams.glslh — which cannot be compiled as C++ — declares none of them. A second
    // copy left behind there is the failure this catches: the shader includes both headers, so a stale
    // duplicate would either redefine the macro or, worse, be the one a future reader edits.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found - run from the workspace root";

    const std::string params = ReadFile( root + "Engine/Content/Shaders/Common/CloudParams.glslh" );
    ASSERT_FALSE( params.empty() ) << "Common/CloudParams.glslh could not be read";

    EXPECT_EQ( params.find( "#define CLOUD_MIN_STEPS" ), std::string::npos )
         << "CLOUD_MIN_STEPS is declared in CloudParams.glslh again; the schedule lives in "
            "CloudGeometry.glslh, which is the only one of the two a test can compile";
    EXPECT_EQ( params.find( "#define CLOUD_DISTANCE_TO_MAX_STEPS_KM" ), std::string::npos )
         << "CLOUD_DISTANCE_TO_MAX_STEPS_KM is declared in CloudParams.glslh again";

    const std::string geometry = ReadFile( root + "Engine/Content/Shaders/Common/CloudGeometry.glslh" );
    ASSERT_FALSE( geometry.empty() ) << "Common/CloudGeometry.glslh could not be read";

    EXPECT_FLOAT_EQ( static_cast<float>( ParseDefine( geometry, "CLOUD_MIN_STEPS" ) ), CLOUD_MIN_STEPS );
    EXPECT_FLOAT_EQ( static_cast<float>( ParseDefine( geometry, "CLOUD_DISTANCE_TO_MAX_STEPS_KM" ) ),
                     CLOUD_DISTANCE_TO_MAX_STEPS_KM );

    EXPECT_GT( CLOUD_DISTANCE_TO_MAX_STEPS_KM, 0.0f ) << "the schedule divides by this";
}

TEST( CloudGeometrySteps, TheCountRisesWithTheSegmentAndSaturatesAtTheDeclaredDistance )
{
    float previous = 0.0f;
    for ( int step = 0; step <= 400; ++step )
    {
        const float segmentKm = 0.1f * static_cast<float>( step );
        const float count =
             CloudStepCount( segmentKm, CLOUD_MIN_STEPS, kMaxCount, CLOUD_DISTANCE_TO_MAX_STEPS_KM );

        EXPECT_GE( count, CLOUD_MIN_STEPS ) << "segment " << segmentKm << " km fell below the floor";
        EXPECT_LE( count, kMaxCount ) << "segment " << segmentKm << " km exceeded the ceiling";
        EXPECT_GE( count, previous ) << "segment " << segmentKm << " km: the count went DOWN";

        if ( segmentKm >= CLOUD_DISTANCE_TO_MAX_STEPS_KM )
            EXPECT_FLOAT_EQ( count, kMaxCount ) << "segment " << segmentKm << " km must be saturated";

        previous = count;
    }

    // Strictly rising in the middle of the ramp, which is what "the cost of a ray is a function of its
    // segment" means. A schedule that is flat there is one that spends the same budget on a ray that
    // grazes the layer and one that crosses it. Sampled at a quarter and a half of the saturation
    // distance, so the pair moves with the constant instead of pinning it a second time.
    const float atQuarter = CloudStepCount( CLOUD_DISTANCE_TO_MAX_STEPS_KM * 0.25f, CLOUD_MIN_STEPS, kMaxCount,
                                            CLOUD_DISTANCE_TO_MAX_STEPS_KM );
    const float atHalf    = CloudStepCount( CLOUD_DISTANCE_TO_MAX_STEPS_KM * 0.5f, CLOUD_MIN_STEPS, kMaxCount,
                                            CLOUD_DISTANCE_TO_MAX_STEPS_KM );
    EXPECT_GT( atHalf, atQuarter * 1.5f );
}

// ---------------------------------------------------------------------------------------------------
// WHAT THE MARCH CAN FIND, which is the relation the speckle came out of
// ---------------------------------------------------------------------------------------------------

TEST( CloudGeometrySteps, TheFineStepIsTheSaturationDistanceOverTheCeilingForEveryShortSegment )
{
    // THE PROPERTY THAT MAKES THE SATURATION DISTANCE THE MARCH'S RESOLUTION, and it is not obvious from
    // the formula. Below the saturation distance the count is proportional to the segment, so the segment
    // CANCELS: every ray shorter than that gets the same step, `distanceToMax / maxCount`, whatever its
    // own length is. That is why a shell 400 m thick was marched at the same 58.6 m as one 15 km thick,
    // and why moving this one number is what moves the resolution of every thin layer in the library.
    const float expected = CLOUD_DISTANCE_TO_MAX_STEPS_KM / kMaxCount;

    for ( const float fraction : { 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 0.99f } )
    {
        const float segmentKm = CLOUD_DISTANCE_TO_MAX_STEPS_KM * fraction;
        const float stepKm =
             CloudFineStepKm( segmentKm, CLOUD_MIN_STEPS, kMaxCount, CLOUD_DISTANCE_TO_MAX_STEPS_KM );

        // The floor bites only for segments so short that the ramp would ask for fewer than two samples.
        if ( CloudStepCount( segmentKm, CLOUD_MIN_STEPS, kMaxCount, CLOUD_DISTANCE_TO_MAX_STEPS_KM ) >
             CLOUD_MIN_STEPS )
            EXPECT_NEAR( stepKm, expected, expected * 1e-4f ) << "segment " << segmentKm << " km";
    }

    // And past it the step is the segment over the ceiling, which is the half no schedule constant can
    // improve — the ceiling is the artist's quality tier.
    EXPECT_NEAR( CloudFineStepKm( 60.0f, CLOUD_MIN_STEPS, kMaxCount, CLOUD_DISTANCE_TO_MAX_STEPS_KM ),
                 60.0f / kMaxCount, 1e-5f );
}

TEST( CloudGeometrySteps, TheResolvableChordIsTwoCOARSEStepsAndNotTwoFineOnes )
{
    // THE DISTINCTION THIS FILE EXISTS TO KEEP. The fine step is what the march INTEGRATES at; the coarse
    // step is what it SEARCHES at, because outside cloud it strides coarsely and only drops to fine after
    // a coarse sample has found something. Nyquist against the search lattice is therefore two COARSE
    // steps, and reading it as two fine steps understates the march's blindness by the multiplier — the
    // factor of four that put five of nine shipped types past Nyquist while the fine step said all nine
    // were inside it.
    for ( const float fineStepKm : { 0.0001f, 0.0156f, 0.0586f, 0.234f, 1.0f } )
    {
        EXPECT_FLOAT_EQ( CloudResolvableChordKm( fineStepKm ), 2.0f * CLOUD_COARSE_STEP_MULTIPLIER * fineStepKm );
        EXPECT_GT( CloudResolvableChordKm( fineStepKm ), 2.0f * fineStepKm )
             << "the search lattice is coarser than the integration lattice; a chord bound written "
                "against the fine step is optimistic by the coarse multiplier";
    }

    // The library's own bound: the finest chord the schedule resolves anywhere. Every ray shorter than the
    // saturation distance is marched at exactly this resolution and no ray is ever marched finer.
    EXPECT_FLOAT_EQ( CloudFinestResolvableChordKm( kMaxCount ),
                     CloudResolvableChordKm( CLOUD_DISTANCE_TO_MAX_STEPS_KM / kMaxCount ) );

    // A degenerate ceiling must not become a division by zero: the count is floored, not trusted.
    EXPECT_TRUE( std::isfinite( CloudFinestResolvableChordKm( 0.0f ) ) );
}

TEST( CloudGeometrySteps, TheFloorHoldsForAZeroLengthSegmentAndForADegenerateSaturationDistance )
{
    // A zero segment is reachable — the shell floor makes the thinnest authorable layer 1 m — and the
    // count must still be the minimum rather than zero, because the march turns it into an int and uses
    // it as a divisor.
    EXPECT_FLOAT_EQ( CloudStepCount( 0.0f, 2.0f, 256.0f, 15.0f ), 2.0f );
    EXPECT_FLOAT_EQ( CloudStepCount( -5.0f, 2.0f, 256.0f, 15.0f ), 2.0f );

    // And the division inside is guarded: a saturation distance of zero would otherwise be an infinity
    // that becomes an int of undefined value and a step size of zero.
    const float degenerate = CloudStepCount( 4.0f, 2.0f, 256.0f, 0.0f );
    EXPECT_TRUE( std::isfinite( degenerate ) );
    EXPECT_FLOAT_EQ( degenerate, 256.0f );
}

TEST( CloudGeometrySteps, TheStepSizeTheMarchDerivesIsAlwaysPositiveAndBoundedByTheSegment )
{
    // The relation the loop depends on: length / count is the fine step, and the coarse step is four
    // times it. Both have to be positive and neither may exceed the segment, or a single iteration would
    // step past the exit and the layer would be sampled once.
    for ( const float segmentKm : { 0.001f, 0.1f, 1.0f, 7.5f, 15.0f, 60.0f, 400.0f } )
    {
        const float count =
             CloudStepCount( segmentKm, CLOUD_MIN_STEPS, kMaxCount, CLOUD_DISTANCE_TO_MAX_STEPS_KM );
        ASSERT_GT( count, 0.0f );

        const float stepKm =
             CloudFineStepKm( segmentKm, CLOUD_MIN_STEPS, kMaxCount, CLOUD_DISTANCE_TO_MAX_STEPS_KM );
        EXPECT_GT( stepKm, 0.0f ) << "segment " << segmentKm;
        EXPECT_LE( stepKm, segmentKm ) << "segment " << segmentKm;
        EXPECT_GE( static_cast<int>( count ), 2 ) << "segment " << segmentKm << ": the loop would not run";
    }
}

// ---------------------------------------------------------------------------------------------------
// The two-tier schedule — the relation that keeps the march moving
// ---------------------------------------------------------------------------------------------------
//
// These two constants used to be literals inside CloudRaymarch.shader, a hundred lines apart, and no test
// could reach them because that file is never compiled as C++. They now live in CloudGeometry.glslh, which
// this suite compiles, and what is asserted is the RELATION between them rather than either value: the
// contract's 2.3.1 rule, and the reason is that both numbers are individually reasonable at any setting
// and only their ORDER decides whether the march advances at all.

TEST( CloudGeometryTwoTier, OneExcursionIntoTheFineTierAdvancesTheRayRatherThanReturningItWhereItWas )
{
    // THE INVARIANT. Dropping to the fine tier costs one coarse step BACKWARDS — the step-back that stops
    // a coarse step of cloud from being skipped — and buys CLOUD_EMPTY_FINE_SAMPLES_BEFORE_COARSE fine
    // steps forwards before the tier flips again. If the coarse multiplier ever reaches the sample count,
    // the net advance is zero and the march stands exactly where it started while spending its entire
    // step budget; past it, the march walks backwards and every ray costs its ceiling.
    //
    // Asserted over the whole range of fine steps the schedule can produce — a segment of a metre divided
    // by the 256-step ceiling at one end, and a 400 km grazing segment divided by the two-step floor at
    // the other — because a relation that holds only at one step size is not the relation.
    for ( const float segmentKm : { 0.001f, 0.1f, 1.0f, 7.5f, 15.0f, 60.0f, 400.0f } )
    {
        const float count = CloudStepCount( segmentKm, 2.0f, 256.0f, 15.0f );
        ASSERT_GT( count, 0.0f );

        const float fineStepKm = segmentKm / count;

        EXPECT_GT( CloudTwoTierCycleAdvanceKm( fineStepKm ), 0.0f )
             << "segment " << segmentKm << " km (fine step " << fineStepKm
             << " km): a fine excursion that finds nothing returns the ray to where it started or behind "
                "it, so the march burns its whole budget standing still";

        // And the coarse step is a real stride rather than a rounding of the fine one, which is the other
        // half of what makes the tier worth having.
        EXPECT_GT( CloudCoarseStepKm( fineStepKm ), fineStepKm ) << "segment " << segmentKm;
    }
}

TEST( CloudGeometryTwoTier, TheAdvanceIsExactlyTheDifferenceOfTheTwoConstantsAndScalesWithTheStep )
{
    // Stated as the algebra rather than as a number, so that changing either constant moves this
    // assertion's expectation with it and only the SIGN test above can fail. What must never change is
    // that the advance is (samples - multiplier) fine steps.
    constexpr float kSamples = static_cast<float>( CLOUD_EMPTY_FINE_SAMPLES_BEFORE_COARSE );

    for ( const float fineStepKm : { 0.0005f, 0.01f, 0.25f, 3.0f } )
    {
        EXPECT_FLOAT_EQ( CloudTwoTierCycleAdvanceKm( fineStepKm ),
                         ( kSamples - CLOUD_COARSE_STEP_MULTIPLIER ) * fineStepKm )
             << "fine step " << fineStepKm;
    }

    // The relation in its bare form, so that the failure message names the two numbers rather than a
    // distance. A reader who changes one of them sees this line first.
    EXPECT_LT( CLOUD_COARSE_STEP_MULTIPLIER, kSamples )
         << "the coarse step multiplier (" << CLOUD_COARSE_STEP_MULTIPLIER
         << ") must be strictly below the number of empty fine samples before the march returns to coarse ("
         << kSamples << ")";
}

TEST( CloudGeometryTwoTier, TheMarchRunsTheseConstantsAndNotACopyOfThem )
{
    // Not a tautology, and the reason this suite exists at all: the numbers are only worth asserting if
    // the shader that consumes them reads THESE symbols. CloudRaymarch.shader is read as text — it is the
    // one file in the chain that cannot be compiled here — and what is checked is that the two literals
    // it used to carry are gone and the two names are in their place.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found - run from the workspace root";

    const std::string march = ReadFile( root + "Engine/Content/Shaders/Programs/Clouds/CloudRaymarch.shader" );
    ASSERT_FALSE( march.empty() ) << "CloudRaymarch.shader could not be read";

    EXPECT_NE( march.find( "CloudCoarseStepKm(" ), std::string::npos )
         << "the march no longer derives its coarse step from CloudGeometry.glslh";
    EXPECT_NE( march.find( "CLOUD_EMPTY_FINE_SAMPLES_BEFORE_COARSE" ), std::string::npos )
         << "the march no longer takes its empty-run length from CloudGeometry.glslh";
    EXPECT_EQ( march.find( "stepKm * 4.0f" ), std::string::npos )
         << "the coarse multiplier is a literal in the march again, where no test can reach it";

    // And the fine step itself, which the march used to form as a division of its own. One division in one
    // place is what lets the resolvable-chord relation above be a statement about the shipped march rather
    // than about a function nobody calls.
    EXPECT_NE( march.find( "CloudFineStepKm(" ), std::string::npos )
         << "the march divides out its own fine step again instead of taking CloudGeometry.glslh's";
    EXPECT_NE( march.find( "CLOUD_DISTANCE_TO_MAX_STEPS_KM" ), std::string::npos )
         << "the march no longer takes its saturation distance from CloudGeometry.glslh";

    // AND THE SAMPLE'S OWN VERTICAL, for the same reason: the two functions below are only worth
    // asserting if the per-sample sun transmittance reads THESE and not a local `normalize(pos)` and
    // `pos.y - radius`. The second of those is the world-Y reading, which agrees near the camera and is
    // 1.77 km wrong at the layer's far reach.
    EXPECT_NE( march.find( "CloudLocalUp(" ), std::string::npos )
         << "the march derives its own local zenith again instead of taking CloudGeometry.glslh's";
    EXPECT_NE( march.find( "CloudAltitudeKm(" ), std::string::npos )
         << "the march derives its own sample altitude again instead of taking CloudGeometry.glslh's";

    // THE READER IS THE SKY'S, and this is the assertion that keeps it from being copied. The domain
    // guard, the planet's shadow and the texel-centre clamp the engine's REPEAT samplers make mandatory
    // all live in SkySunAtAltitude — one text, compiled as C++ by Desert/Tests/Engine/SkyScattering, and
    // shared with the environment bake so the visible deck and the baked one cannot drift.
    EXPECT_NE( march.find( "SkySunAtAltitude(" ), std::string::npos )
         << "the march re-derives the transmittance LUT's read instead of taking SkyScattering.glslh's, "
            "which is the arrangement that keeps it in step with the environment bake";

    // THE SAME TEXT, IN THE OTHER PASS THAT MARCHES THIS FIELD. Both light the field from ONE packed
    // block, and PerSampleAtmosphereTransmittance changes what that block's SunColour MEANS. A bake that
    // does not apply the transmittance would light the IBL panorama with the sun as seen from space.
    const std::string bake = ReadFile( root + "Engine/Content/Shaders/Programs/Compute/BakeProceduralSky.shader" );
    ASSERT_FALSE( bake.empty() ) << "BakeProceduralSky.shader could not be read";

    EXPECT_NE( bake.find( "SkySunAtAltitude(" ), std::string::npos )
         << "the environment bake marches the cloud field without applying the per-sample sun "
            "transmittance the packed block it reads may have been prepared for";
}

// ------------------------------------------------------------------------------------------------------
// THE SAMPLE'S OWN VERTICAL — the two functions anything that reads a function of the sun's ELEVATION at a
// sample has to go through.
//
// The failure they exist to prevent is the one CloudHeightFraction's own comment describes, one level up:
// near the camera the local zenith and the world's +Y agree to within rounding, so a frame taken from the
// ground endorses the wrong function completely. What breaks is the FAR deck, and only in a quantity that
// depends on the sun's angle rather than on the sample's height.
// ------------------------------------------------------------------------------------------------------

namespace
{
    // The horizontal reach a layer's Max View Distance can span. The shipped default is 60 km; the
    // slider's top is 150, and 150 is what makes the tilt below biggest.
    constexpr float kFarReachKm = 150.0f;

    // A point at the same ALTITUDE as one overhead, displaced along the planet's surface by an arc of
    // @p arcKm. Built from the angle rather than from a translation, so the altitude is exact by
    // construction and the test is about the function rather than about the fixture.
    vec3 PointAtArc( float radiusKm, float arcKm )
    {
        const float theta = arcKm / kPlanetKm;
        return vec3( radiusKm * std::sin( theta ), radiusKm * std::cos( theta ), 0.0f );
    }
} // namespace

TEST( CloudGeometryVertical, TheLocalZenithTiltsOverTheLayersOwnReach )
{
    // Overhead it IS the world's up, which is what makes the mistake invisible from the ground.
    const vec3 overhead = CloudLocalUp( vec3( 0.0f, kBottomKm, 0.0f ) );
    EXPECT_NEAR( overhead.x, 0.0f, 1e-6f );
    EXPECT_NEAR( overhead.y, 1.0f, 1e-6f );
    EXPECT_NEAR( overhead.z, 0.0f, 1e-6f );

    // At the layer's own far reach it is not, and by more than a degree — which is the whole reason a
    // per-sample sun angle is taken from the SAMPLE. atan(150 / 6360) = 1.351 degrees.
    const vec3  farPoint = CloudLocalUp( PointAtArc( kBottomKm, kFarReachKm ) );
    const float tiltDeg  = std::acos( std::clamp( farPoint.y, -1.0f, 1.0f ) ) * 180.0f / 3.14159265358979f;

    EXPECT_GT( tiltDeg, 1.0f ) << "the local zenith at " << kFarReachKm
                               << " km is within a degree of the world's up, so the farPoint deck would be "
                                  "given the near deck's sun angle";
    EXPECT_NEAR( tiltDeg, 1.351f, 0.01f );

    // Unit length at every reach — anything that dots it against a direction reads a cosine, and a
    // non-unit "cosine" is a transmittance sampled at the wrong zenith.
    for ( const float arcKm : { 0.0f, 1.0f, 40.0f, kFarReachKm, 1000.0f } )
        EXPECT_NEAR( length( CloudLocalUp( PointAtArc( kBottomKm, arcKm ) ) ), 1.0f, 1e-5f );
}

TEST( CloudGeometryVertical, TheAltitudeFollowsTheCurvatureAndTheWorldYDoesNot )
{
    const CloudLayer layer = MakeTestLayer();

    // Overhead, the two agree exactly. This is the reading a close-up frame gives.
    const vec3 overhead = vec3( 0.0f, kBottomKm, 0.0f );
    EXPECT_NEAR( CloudAltitudeKm( layer, overhead ), kBottomKm - kPlanetKm, 1e-3f );
    EXPECT_NEAR( overhead.y - kPlanetKm, kBottomKm - kPlanetKm, 1e-3f );

    // At the farPoint reach they do not. The altitude is still the layer's base — the point was placed on that
    // sphere — while the world Y has fallen by the sagitta, 1.77 km of a 5 km base.
    const vec3  farPoint = PointAtArc( kBottomKm, kFarReachKm );
    const float altitude = CloudAltitudeKm( layer, farPoint );
    const float naive    = farPoint.y - kPlanetKm;

    EXPECT_NEAR( altitude, kBottomKm - kPlanetKm, 1e-2f );
    EXPECT_LT( naive, altitude - 1.5f ) << "the world-Y reading at " << kFarReachKm
                                        << " km is not measurably below the true altitude, so this "
                                           "fixture no longer separates the two";
    EXPECT_NEAR( altitude - naive, 1.769f, 0.02f );

    // MONOTONIC IN THE RADIUS, which is what catches a sign or a swapped subtraction: a sample further
    // from the planet centre is higher, whatever direction it lies in.
    float previous = -1.0f;
    for ( const float radiusKm : { kPlanetKm, kBottomKm, 0.5f * ( kBottomKm + kTopKm ), kTopKm } )
    {
        const float here = CloudAltitudeKm( layer, PointAtArc( radiusKm, kFarReachKm ) );
        EXPECT_GT( here, previous );
        previous = here;
    }
}

TEST( CloudGeometryVertical, TheAltitudeIsNeverNegative )
{
    const CloudLayer layer = MakeTestLayer();

    // The view march never places a sample below the shell, but the SHADOW ray's first segment can step
    // below its own start, and a negative altitude reaching an atmosphere LUT is a coordinate outside the
    // mapping's domain rather than a slightly wrong one.
    for ( const float radiusKm : { 0.0f, 1.0f, kPlanetKm * 0.5f, kPlanetKm - 1.0f, kPlanetKm } )
        EXPECT_GE( CloudAltitudeKm( layer, vec3( 0.0f, radiusKm, 0.0f ) ), 0.0f ) << "radius " << radiusKm;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
