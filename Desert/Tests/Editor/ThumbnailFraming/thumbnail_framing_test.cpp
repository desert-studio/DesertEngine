// THE RELATION THIS GUARDS: a thumbnail's subject must FILL the frame it is captured into and sit at its
// CENTRE, under the capture camera's own view/projection. That is what makes the swatch the Details panel
// shows be the material -- a sphere with the material's colour -- rather than the two failure modes of Д30:
//
//   * a subject scaled as if it were one unit when the preview primitive is one METRE (100 units): drawn
//     100x too big;
//   * a subject left at the world origin while the editor's default camera looks at eye height (0, 200, 0):
//     200 units below where the camera aims.
//
// They compound. Measured on the real capture camera, the old rule put the subject's centre at NDC
// y = -3.50 (the frame spans -1..1) and 70.0 deg off the view axis against a 38.1 deg half-FOV, and the
// captured PNG was the flank of a 400-unit sphere: a wall of albedo under sky, with no sphere in it.
//
// Both were metre-era assumptions that the centimetre migration falsified, and both were invisible to the
// test suite because the framing lived inline in AssetThumbnailRenderer, reachable only by launching the
// editor and looking at a PNG. Moving the rule into a pure header (ThumbnailFraming) is what lets it be
// asserted here: matrices in, a world transform out, no device.
//
// TWO THINGS THIS FILE DELIBERATELY REFUSES TO DO.
//
// It does not write down the preview primitive's size as a literal. The size is
// Common::Units::UnitsPerMetre, the same symbol PrimitiveMeshFactory scales every primitive by, so if that
// convention ever moves again the expectation moves with it instead of quietly going stale -- which is the
// exact failure being guarded against, one level up.
//
// And it does not rest on a single camera pose. A test that pins one pose is satisfied the day the pose
// changes, while the renderer breaks -- again, the failure being guarded against. The properties below are
// asserted over a FAMILY of cameras (§Cameras), and the migrated editor default is one named member of it,
// kept as the Д30 regression witness rather than as the whole argument.

#include <gtest/gtest.h>

#include <Editor/Widgets/ThumbnailFraming.hpp>
#include <Engine/Geometry/PosedBounds.hpp>

#include <Common/Core/Units.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace
{
    namespace TF = Desert::Editor::ThumbnailFraming;

    // A stand-in for Geometry::Submesh with just the two fields MeasureSubmeshes reads. The header is
    // templated on the range for exactly this reason -- the measurement can be checked without linking
    // mesh code that cannot exist away from a GPU.
    struct StubSubmesh
    {
        glm::mat4 Transform;
        struct
        {
            glm::vec3 Min;
            glm::vec3 Max;
        } BoundingBox;
    };

    // The preview primitive's world size. NOT a literal 100: PrimitiveMeshFactory scales every primitive
    // by this very constant, so the number the framing must be TOLD and the number the geometry actually
    // IS are the same symbol and cannot drift apart.
    constexpr float kPrimitiveExtent = Common::Units::UnitsPerMetre;

    struct TestCamera
    {
        const char* Name;
        glm::vec3   Eye;
        glm::vec3   LookAt;
        float       FovYDegrees;
        float       Aspect;
        glm::mat4   View;
        glm::mat4   Projection;
    };

    TestCamera MakeCamera( const char* name, glm::vec3 eye, glm::vec3 lookAt, float fovYDeg, float aspect )
    {
        TestCamera cam{ name, eye, lookAt, fovYDeg, aspect, glm::mat4( 1.0f ), glm::mat4( 1.0f ) };
        cam.View       = glm::lookAt( eye, lookAt, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        cam.Projection = glm::perspective( glm::radians( fovYDeg ), aspect, 10.0f, 5000000.0f );
        return cam;
    }

    // The capture camera the renderer really uses: the scene's default EditorCamera after the centimetre
    // migration -- eye (-70, 186, -70) looking at eye height (0, 200, 0), and a 76.3 deg effective vertical
    // FOV (the 45 deg authored FOV under the size-anchored editor projection at the 2048 px capture size).
    // Kept as a NAMED member of the family below so the Д30 witness stays legible.
    TestCamera MigratedEditorCamera()
    {
        return MakeCamera( "migrated editor default (Д30)", glm::vec3( -70.0f, 186.0f, -70.0f ),
                           glm::vec3( 0.0f, 200.0f, 0.0f ), 76.3f, 1.0f );
    }

    // §Cameras -- the family every property below is asserted over. PlaceInView reads the matrices it is
    // handed, so the rule must hold for ANY well-formed camera; pinning one pose is how a test survives the
    // migration that breaks the renderer.
    std::vector<TestCamera> CameraFamily()
    {
        return {
             MigratedEditorCamera(),
             // The pre-migration pose, for contrast: near the origin and looking straight at it.
             MakeCamera( "metre-era (pre-migration)", glm::vec3( -4.33f, 6.12f, -4.33f ), glm::vec3( 0.0f ), 45.0f,
                         1.0f ),
             // Far away, aiming somewhere that is not the origin at all.
             MakeCamera( "distant, off-origin aim", glm::vec3( 5000.0f, -1200.0f, 800.0f ),
                         glm::vec3( -300.0f, 900.0f, 40.0f ), 60.0f, 1.0f ),
             // Narrow lens, and a non-square frame (a viewport that is not the capture's own).
             MakeCamera( "narrow lens, wide frame", glm::vec3( 0.0f, 30.0f, 900.0f ),
                         glm::vec3( 0.0f, 30.0f, 0.0f ), 20.0f, 16.0f / 9.0f ),
             // Nearly straight down: the up vector is close to degenerate but still valid.
             MakeCamera( "steep top-down", glm::vec3( 12.0f, 4000.0f, 9.0f ), glm::vec3( 0.0f, 0.0f, 0.0f ), 90.0f,
                         1.0f ),
        };
    }

    // Half-angle of the camera's vertical field of view, in degrees.
    float HalfFovDegrees( const TestCamera& cam )
    {
        return cam.FovYDegrees * 0.5f;
    }

    // Angle between the camera's view axis and the direction to a world point, in degrees. This is the
    // quantity Д30 broke: 70.0 against a 38.1 deg half-FOV.
    float AngleOffViewAxis( const TestCamera& cam, const glm::vec3& world )
    {
        const glm::vec3 forward = glm::normalize( cam.LookAt - cam.Eye );
        const glm::vec3 toPoint = world - cam.Eye;
        const float     cosine  = glm::clamp( glm::dot( glm::normalize( toPoint ), forward ), -1.0f, 1.0f );
        return glm::degrees( std::acos( cosine ) );
    }

    // NDC of a world point under the camera (perspective divide included).
    glm::vec2 ProjectToNdc( const TestCamera& cam, const glm::vec3& world )
    {
        const glm::vec4 clip = cam.Projection * cam.View * glm::vec4( world, 1.0f );
        return glm::vec2( clip.x / clip.w, clip.y / clip.w );
    }
} // namespace

// The measurement half: the preview primitive is one METRE across, and the framing must be TOLD that, not
// assume one. A stub whose box spans half a metre either way measures a whole one -- the number whose
// absence (a hardcoded 1.0) was half of Д30. Expressed in Units::UnitsPerMetre, the same constant
// PrimitiveMeshFactory scales by, so a change to the unit convention moves both sides together.
TEST( ThumbnailFraming, MeasuresTheWholePrimitiveNotAUnit )
{
    const float                half = 0.5f * kPrimitiveExtent;
    std::array<StubSubmesh, 1> submeshes{
         StubSubmesh{ glm::mat4( 1.0f ), { glm::vec3( -half ), glm::vec3( half ) } } };

    const auto frame = TF::MeasureSubmeshes( submeshes );
    ASSERT_TRUE( frame.Valid );
    EXPECT_NEAR( frame.Extent, kPrimitiveExtent, 1e-3f );
    EXPECT_NEAR( glm::length( frame.Center ), 0.0f, 1e-3f );

    // And it must be a metre, not a unit: the assertion that the old `worldSize = 1.0` violated. Stated as
    // a RELATION between the two constants rather than as the number 100.
    EXPECT_GT( frame.Extent, 1.0f );
    EXPECT_NEAR( frame.Extent, Common::Units::Metres( 1.0f ), 1e-3f );
}

// A per-submesh transform must be honoured: a box translated off-origin moves the measured centre with it,
// so the framing recentres the real geometry rather than an axis-aligned guess.
TEST( ThumbnailFraming, HonoursSubmeshTransform )
{
    const float                half = 0.5f * kPrimitiveExtent;
    std::array<StubSubmesh, 1> submeshes{
         StubSubmesh{ glm::translate( glm::mat4( 1.0f ), glm::vec3( 10.0f, 0.0f, 0.0f ) ),
                      { glm::vec3( -half ), glm::vec3( half ) } } };

    const auto frame = TF::MeasureSubmeshes( submeshes );
    ASSERT_TRUE( frame.Valid );
    EXPECT_NEAR( frame.Center.x, 10.0f, 1e-3f );
    EXPECT_NEAR( frame.Extent, kPrimitiveExtent, 1e-3f );
}

// THE PROPERTY Д30 ACTUALLY VIOLATED, and the one worth having: the placed subject lies INSIDE the
// camera's field of view. Not "near the centre" and not "the right size" -- in frame at all. The old rule
// put it 70.0 deg off the view axis against a 38.1 deg half-FOV, which is why the swatch contained no
// sphere; every other assertion here is a refinement of this one.
//
// Asserted over the whole camera family: a rule read from the matrices must hold for any of them, and the
// migrated default is only the pose that happened to catch us.
TEST( ThumbnailFraming, SubjectIsInsideTheFrustumForEveryCamera )
{
    for ( const TestCamera& cam : CameraFamily() )
    {
        SCOPED_TRACE( cam.Name );
        const auto placement = TF::PlaceInView( cam.View, cam.Projection, kPrimitiveExtent, glm::vec3( 0.0f ) );

        const glm::vec3 worldCenter = placement.Translation; // center == 0
        EXPECT_LT( AngleOffViewAxis( cam, worldCenter ), HalfFovDegrees( cam ) );

        // In front of the camera, never behind it: a point behind projects to a plausible-looking NDC
        // after the divide by a negative w, so the angle alone is not enough.
        const glm::vec4 clip = cam.Projection * cam.View * glm::vec4( worldCenter, 1.0f );
        EXPECT_GT( clip.w, 0.0f );
    }
}

// The subject must be inside the frame WITH ITS WHOLE BODY, not merely by its centre: a sphere whose
// centre is in view and whose limb is off the top is still not a preview of anything. Checked on the four
// extremes of the placed silhouette.
TEST( ThumbnailFraming, WholeSubjectFitsTheFrameForEveryCamera )
{
    for ( const TestCamera& cam : CameraFamily() )
    {
        SCOPED_TRACE( cam.Name );
        const auto placement = TF::PlaceInView( cam.View, cam.Projection, kPrimitiveExtent, glm::vec3( 0.0f ) );

        const glm::mat4 invView = glm::inverse( cam.View );
        const glm::vec3 right   = glm::normalize( glm::vec3( invView[0] ) );
        const glm::vec3 up      = glm::normalize( glm::vec3( invView[1] ) );
        const float     half    = 0.5f * kPrimitiveExtent * placement.Scale;

        for ( const glm::vec3& offset : { right * half, -right * half, up * half, -up * half } )
        {
            const glm::vec2 ndc = ProjectToNdc( cam, placement.Translation + offset );
            EXPECT_LT( std::abs( ndc.x ), 1.0f );
            EXPECT_LT( std::abs( ndc.y ), 1.0f );
        }
    }
}

// The framing half: the subject placed by PlaceInView must land at the CENTRE of the frame. Reverting to
// "leave it at the origin" (the pre-fix behaviour) drops it 200 units below the migrated camera's look-at
// and this NDC comes out at y = -3.50.
//
// An off-centre subject is asserted with a non-zero `center` too, because the material branch measures the
// primitive's own centre and hands it in: a rule that only recentred a subject already centred would pass
// the zero case and still frame an off-origin mesh wrongly.
TEST( ThumbnailFraming, SubjectLandsAtFrameCentreForEveryCamera )
{
    for ( const TestCamera& cam : CameraFamily() )
    {
        SCOPED_TRACE( cam.Name );
        for ( const glm::vec3& center : { glm::vec3( 0.0f ), glm::vec3( 37.0f, -12.0f, 400.0f ) } )
        {
            const auto placement = TF::PlaceInView( cam.View, cam.Projection, kPrimitiveExtent, center );

            // The subject's own centre after placement (uniform scale + translation applied to `center`).
            const glm::vec3 worldCenter = placement.Scale * center + placement.Translation;
            const glm::vec2 ndc         = ProjectToNdc( cam, worldCenter );

            EXPECT_NEAR( ndc.x, 0.0f, 0.02f );
            EXPECT_NEAR( ndc.y, 0.0f, 0.02f );
        }
    }
}

// ...and it must FILL the frame to the intended fraction, not overflow it (the 100x blow-up) nor shrink to
// a dot (over-correcting the other way). Measured as the on-screen span of the placed subject on both
// screen axes: the TIGHTER one must come out at kFillFraction, which is the promise the rule makes and the
// one that holds whatever the frame's aspect is.
TEST( ThumbnailFraming, SubjectFillsTheFrameForEveryCamera )
{
    for ( const TestCamera& cam : CameraFamily() )
    {
        SCOPED_TRACE( cam.Name );
        const auto placement = TF::PlaceInView( cam.View, cam.Projection, kPrimitiveExtent, glm::vec3( 0.0f ) );

        const glm::mat4 invView = glm::inverse( cam.View );
        const glm::vec3 right   = glm::normalize( glm::vec3( invView[0] ) );
        const glm::vec3 up      = glm::normalize( glm::vec3( invView[1] ) );

        const glm::vec3 worldCenter = placement.Translation; // center == 0
        const float     halfExtent  = 0.5f * kPrimitiveExtent * placement.Scale;

        const glm::vec2 ndcCentre = ProjectToNdc( cam, worldCenter );
        // Full projected span = 2 * (half-extent projected), against a frame that spans 2 NDC per axis.
        const float spanX = glm::length( ProjectToNdc( cam, worldCenter + right * halfExtent ) - ndcCentre );
        const float spanY = glm::length( ProjectToNdc( cam, worldCenter + up * halfExtent ) - ndcCentre );

        EXPECT_NEAR( std::max( spanX, spanY ), TF::kFillFraction, 0.03f );
    }
}

// The degenerate guard: a subject with no measurable extent must not divide the framing by ~zero. It is
// placed on the view axis at unit scale -- visible, centred, not a NaN.
TEST( ThumbnailFraming, DegenerateExtentStaysFinite )
{
    const TestCamera cam = MigratedEditorCamera();

    const auto placement = TF::PlaceInView( cam.View, cam.Projection, 0.0f, glm::vec3( 0.0f ) );

    EXPECT_FLOAT_EQ( placement.Scale, 1.0f );
    EXPECT_TRUE( std::isfinite( placement.Translation.x ) );
    EXPECT_TRUE( std::isfinite( placement.Translation.y ) );
    EXPECT_TRUE( std::isfinite( placement.Translation.z ) );

    const glm::vec2 ndc = ProjectToNdc( cam, placement.Translation );
    EXPECT_NEAR( ndc.x, 0.0f, 0.02f );
    EXPECT_NEAR( ndc.y, 0.0f, 0.02f );
}

// THM1n-8: no stand-in frame. Nothing to measure -> not a frame (the capture is refused), never a 1-unit subject.
TEST( ThumbnailFraming, EmptyBoundsAreNotAFrame )
{
    EXPECT_FALSE( TF::MeasureSubmeshes( std::vector<StubSubmesh>{} ).Valid );
    EXPECT_FALSE( TF::FrameOfBox( glm::vec3( 1e9f ), glm::vec3( -1e9f ) ).Valid );
    EXPECT_FALSE( TF::FrameOfBox( glm::vec3( 5.0f ), glm::vec3( 5.0f ) ).Valid ); // a point has no extent
}

namespace
{
    struct StubVertexPosition
    {
        glm::vec3 Position{ 0.0f };
    };
    struct StubSkinnedVertex
    {
        StubVertexPosition      StaticVertex;
        std::array<uint32_t, 4> BoneIDs{ 0, 0, 0, 0 };
        std::array<float, 4>    BoneWeights{ 1.0f, 0.0f, 0.0f, 0.0f };
    };
} // namespace

// THM1n-8: a skinned mesh is framed as DRAWN. Raw vertices span 0..80 on Y, the bind scales by 100: the frame
// is 8000 units, not the 80 the raw (submesh-box) space claims -- which put the camera inside the mesh.
TEST( ThumbnailFraming, SkinnedMeshIsFramedByItsPosedVertices )
{
    std::vector<StubSkinnedVertex> verts( 2 );
    verts[1].StaticVertex.Position = glm::vec3( 0.0f, 80.0f, 0.0f );
    const std::vector<glm::mat4> skin{ glm::scale( glm::mat4( 1.0f ), glm::vec3( 100.0f ) ) };

    const auto box   = Desert::Geometry::MeasurePosedVertices( verts, skin );
    const auto frame = TF::FrameOfBox( box.Min, box.Max );
    ASSERT_TRUE( frame.Valid );
    EXPECT_FLOAT_EQ( frame.Extent, 8000.0f );
    EXPECT_FLOAT_EQ( frame.Center.y, 4000.0f );

    EXPECT_FALSE( Desert::Geometry::MeasurePosedVertices( std::vector<StubSkinnedVertex>{}, skin ).Valid() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
