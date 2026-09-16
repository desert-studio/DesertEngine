// THE MANIPULATOR, AND THE THREE THINGS A PICTURE CANNOT PROVE.
//
// T5.2 draws control shapes in 3D and lets them be dragged. A screenshot can show that something round
// appeared; it cannot show that the round thing is where the control is rather than near it, that the
// drag wrote the authored side rather than a remembered global, or that the hit test ever says no. So
// those three are arithmetic here:
//
//   * PLACEMENT — the drawn geometry's centroid IS the control's global origin, and moving the control
//     moves it by exactly the same vector. The negative control is that the drawn points are not the
//     library's unit points, i.e. a manipulator that forgot the transform fails.
//   * THE DRAG WRITES LOCAL — after a drag the control's `Pose` changed, its `Offset` did not, and
//     re-evaluating the rig from scratch (which drops every cache) reproduces the same global. Plus the
//     one that matters most: with the POINTER STILL and the PARENT MOVED, the pose is unchanged and the
//     control follows its parent. A manipulator holding the grab-time global would pin it instead, and
//     that is T5.1's hole seen from the writing side.
//   * THE HIT TEST SAYS NO — the positive control is a pointer far from any shape, and the sharper one
//     is a pointer at the CENTRE of a big circle: a bounding-box test passes "I grabbed it" there, and
//     a rim test must not.
//   * SIZE IS NOT PLACEMENT (A13). A per-control `ShapeTransform` scales the drawing and must leave the
//     control's global BIT-IDENTICAL. That second half is the whole test: a suite that only checked the
//     drawn radius would pass on the implementation this field exists to replace — sizing through
//     `Offset.Scale`, which moved a control to 182.9 units where the animator asked for 26.4.
//
// The camera is built through `Desert::Core::MakePerspective`, so the suite runs against the engine's
// real reversed-Z convention rather than a convenient one.

#include <Engine/Animation/Rig/ControlManipulator.hpp>
#include <Engine/Animation/Rig/ControlShape.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Core/Projection.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <utility>

using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTransform;
using Desert::Animation::BuildFrame;
using Desert::Animation::ComponentPose;
using Desert::Animation::ControlDrag;
using Desert::Animation::ControlElement;
using Desert::Animation::ControlHierarchy;
using Desert::Animation::ControlShape;
using Desert::Animation::ControlShapeLibrary;
using Desert::Animation::ControlShapePolyline;
using Desert::Animation::ControlSpace;
using Desert::Animation::ControlSpaceKind;
using Desert::Animation::HitTest;
using Desert::Animation::LocalPose;
using Desert::Animation::ManipulatorFrame;
using Desert::Animation::ManipulatorMode;
using Desert::Animation::ManipulatorView;
using Desert::Animation::ProjectToViewport;
using Desert::Animation::Skeleton;

namespace
{
    constexpr float kViewportWidth  = 800.0F;
    constexpr float kViewportHeight = 600.0F;

    /// Two bones under a root, so a control can hang off a bone that the test can then move.
    Skeleton MakeRig()
    {
        std::vector<BoneInfo> bones;

        BoneInfo root;
        root.Name = "root";
        bones.push_back( root );

        BoneInfo chest;
        chest.Name         = "chest";
        chest.ParentBoneID = 0;
        bones.push_back( chest );

        return Skeleton( std::move( bones ) );
    }

    LocalPose PoseWithChestAt( const glm::vec3& chest )
    {
        LocalPose pose;
        pose.Resize( 2 );
        pose[1].Translation = chest;
        return pose;
    }

    /// The editor camera this suite measures through: 60 degrees, 800x600, three metres back from the
    /// point of interest. World units are centimetres, so 300 is 3 m.
    ManipulatorView MakeView( const glm::vec3& eye, const glm::vec3& target )
    {
        ManipulatorView view;
        view.ViewProjection =
             Desert::Core::MakePerspective( glm::radians( 60.0F ), kViewportWidth / kViewportHeight,
                                            Desert::Core::kDefaultNearPlane, Desert::Core::kDefaultFarPlane ) *
             glm::lookAt( eye, target, glm::vec3( 0.0F, 1.0F, 0.0F ) );
        view.ViewportOrigin = glm::vec2( 0.0F, 0.0F );
        view.ViewportSize   = glm::vec2( kViewportWidth, kViewportHeight );
        return view;
    }

    ControlElement MakeControl( const char* name, const char* shape, std::vector<ControlSpace> parents )
    {
        ControlElement element;
        element.Name      = name;
        element.ShapeName = shape;
        element.Parents   = std::move( parents );
        return element;
    }

    uint32_t MustAdd( ControlHierarchy& rig, ControlElement element )
    {
        auto added = rig.Add( std::move( element ) );
        EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
        return added.IsSuccess() ? added.GetValue() : ControlHierarchy::INVALID;
    }

    ControlShapeLibrary MustBuiltIn()
    {
        auto library = ControlShapeLibrary::BuiltIn();
        EXPECT_TRUE( library.IsSuccess() ) << library.GetError();
        return library.IsSuccess() ? library.GetValue() : ControlShapeLibrary();
    }

    /**
     * @brief The built-ins plus one entry that is @p base scaled by @p size.
     *
     * A LIBRARY entry's size is shared by every control that names it; A13 added the per-control term
     * beside it. Both are here so the suite can assert they COMPOSE rather than that either one works
     * alone. Neither is `Offset.Scale`, which was tried first and is measured wrong: the offset's scale
     * also multiplies the pose's translation, so a control sized 8x moved 182.9 units where the animator
     * asked for 26.4. See the note at the top of ControlShape.hpp.
     */
    ControlShapeLibrary LibraryWithSized( const char* newName, const char* base, float size )
    {
        ControlShapeLibrary library = MustBuiltIn();
        const ControlShape* source  = library.Find( base );
        EXPECT_NE( source, nullptr ) << base;
        if ( source == nullptr )
        {
            return library;
        }
        ControlShape sized = *source;
        sized.Transform    = glm::scale( glm::mat4( 1.0F ), glm::vec3( size ) ) * sized.Transform;
        auto added         = library.Add( newName, std::move( sized ) );
        EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
        return library;
    }

    [[nodiscard]] glm::vec3 PositionOf( const glm::mat4& m )
    {
        return { m[3].x, m[3].y, m[3].z };
    }

    [[nodiscard]] glm::vec3 CentroidOf( const std::vector<glm::vec3>& points )
    {
        glm::vec3 sum( 0.0F );
        for ( const glm::vec3& point : points )
        {
            sum += point;
        }
        return points.empty() ? sum : sum / static_cast<float>( points.size() );
    }
} // namespace

// ── the shape library ──────────────────────────────────────────────────────────────────────────────

TEST( ControlManipulatorTest, TheBuiltInShapesAreAValidLibrary )
{
    auto built = ControlShapeLibrary::BuiltIn();
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
    const ControlShapeLibrary& library = built.GetValue();

    EXPECT_EQ( library.Size(), 6U );
    for ( const char* name : { "CircleXY", "CircleXZ", "CircleYZ", "Sphere", "Box", "Diamond" } )
    {
        EXPECT_NE( library.Find( name ), nullptr ) << name;
    }
    EXPECT_EQ( library.Find( "NotAShape" ), nullptr );

    // A sphere is three runs; the circle planes are ONE run under the entry transform, which is the whole
    // reason `ControlShape::Transform` exists and therefore worth pinning.
    ASSERT_NE( library.Find( "Sphere" ), nullptr );
    EXPECT_EQ( library.Find( "Sphere" )->Polylines.size(), 3U );
    ASSERT_NE( library.Find( "CircleXZ" ), nullptr );
    EXPECT_EQ( library.Find( "CircleXZ" )->Polylines.size(), 1U );
    EXPECT_NE( library.Find( "CircleXZ" )->Transform, glm::mat4( 1.0F ) );
    EXPECT_EQ( library.Find( "CircleXY" )->Transform, glm::mat4( 1.0F ) );
}

TEST( ControlManipulatorTest, ADegenerateShapeIsRefusedRatherThanRegistered )
{
    ControlShapeLibrary library;

    ControlShapePolyline line;
    line.Points = { glm::vec3( 0.0F ), glm::vec3( 1.0F, 0.0F, 0.0F ) };
    ControlShape good;
    good.Polylines = { line };

    EXPECT_FALSE( library.Add( "", good ).IsSuccess() ) << "a nameless shape cannot be referenced";
    ASSERT_TRUE( library.Add( "Line", good ).IsSuccess() );
    EXPECT_FALSE( library.Add( "Line", good ).IsSuccess() ) << "a duplicate name is two things to select";

    EXPECT_FALSE( library.Add( "Empty", ControlShape{} ).IsSuccess() );

    ControlShapePolyline single;
    single.Points = { glm::vec3( 0.0F ) };
    ControlShape onePoint;
    onePoint.Polylines = { single };
    EXPECT_FALSE( library.Add( "OnePoint", onePoint ).IsSuccess() ) << "a run of one point draws nothing";

    ControlShapePolyline broken;
    broken.Points = { glm::vec3( 0.0F ), glm::vec3( std::nanf( "" ), 0.0F, 0.0F ) };
    ControlShape notFinite;
    notFinite.Polylines = { broken };
    EXPECT_FALSE( library.Add( "NotFinite", notFinite ).IsSuccess() );

    EXPECT_EQ( library.Size(), 1U ) << "a refused shape must not be half-registered";
}

// ── placement ──────────────────────────────────────────────────────────────────────────────────────

TEST( ControlManipulatorTest, TheShapeIsDrawnWhereTheControlIsAndNotNearIt )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F, 100.0F, 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Circle8", "CircleXY", 8.0F );

    ControlHierarchy rig;
    ControlElement   hand =
         MakeControl( "hand_ctrl", "Circle8", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } );
    hand.Offset.Translation = glm::vec3( 0.0F, 0.0F, 10.0F );
    const uint32_t control  = MustAdd( rig, hand );
    ASSERT_NE( control, ControlHierarchy::INVALID );

    rig.Evaluate( skeleton, pose );

    const ManipulatorView view = MakeView( glm::vec3( 0.0F, 100.0F, 300.0F ), glm::vec3( 0.0F, 100.0F, 0.0F ) );
    ManipulatorFrame      frame;
    BuildFrame( rig, library, view, frame );

    ASSERT_EQ( frame.Shapes.size(), 1U );
    EXPECT_TRUE( frame.UnknownShapes.empty() );
    const auto& draw = frame.Shapes.front();
    EXPECT_EQ( draw.Control, control );

    // THE CLAIM: the drawn geometry is centred on the control, not on the origin and not on its bone.
    const glm::vec3 global = PositionOf( rig.GetGlobalTransform( control ) );
    EXPECT_EQ( global, glm::vec3( 0.0F, 100.0F, 10.0F ) );
    EXPECT_LT( glm::length( CentroidOf( draw.WorldPoints ) - global ), 1e-3F );

    // ...and every point is the offset's scale away from it, so the size came from the one place it lives.
    for ( const glm::vec3& point : draw.WorldPoints )
    {
        EXPECT_NEAR( glm::length( point - global ), 8.0F, 1e-3F );
    }

    // THE NEGATIVE CONTROL. A layer that forgot the placement would hand back the library's unit points;
    // these are not those.
    const ControlShape* circle = library.Find( "Circle8" );
    ASSERT_NE( circle, nullptr );
    ASSERT_EQ( draw.WorldPoints.size(), circle->Polylines.front().Points.size() );
    EXPECT_GT( glm::length( draw.WorldPoints.front() - circle->Polylines.front().Points.front() ), 1.0F );

    // ...and the composition is exactly `global * shapeTransform * libraryTransform * point`, term for
    // term. This control's shape transform is identity, which is what a rig that names no size means.
    const glm::mat4 placement =
         rig.GetGlobalTransform( control ) * rig.Get( control ).ShapeTransform.ToMatrix() * circle->Transform;
    for ( size_t i = 0; i < draw.WorldPoints.size(); ++i )
    {
        const glm::vec3 expected = glm::vec3( placement * glm::vec4( circle->Polylines.front().Points[i], 1.0F ) );
        EXPECT_LT( glm::length( draw.WorldPoints[i] - expected ), 1e-3F ) << "point " << i;
    }

    // MOVING THE CONTROL MOVES THE DRAWING BY THE SAME VECTOR — the arithmetic version of "it follows".
    // Exactly the same vector, because the size is the LIBRARY's and the offset's scale is 1: that is the
    // whole reason the size does not live on the control (see LibraryWithSized).
    BoneTransform animated;
    animated.Translation = glm::vec3( 25.0F, -7.0F, 3.0F );
    ASSERT_TRUE( rig.SetPose( control, animated ).IsSuccess() );
    ManipulatorFrame moved;
    BuildFrame( rig, library, view, moved );
    ASSERT_EQ( moved.Shapes.size(), 1U );
    EXPECT_LT( glm::length( ( CentroidOf( moved.Shapes.front().WorldPoints ) - CentroidOf( draw.WorldPoints ) ) -
                            animated.Translation ),
               1e-3F );
    EXPECT_LT( glm::length( CentroidOf( moved.Shapes.front().WorldPoints ) -
                            PositionOf( rig.GetGlobalTransform( control ) ) ),
               1e-3F )
         << "the drawing must still be centred on the control after it moved";
}

// ── size (A13) ─────────────────────────────────────────────────────────────────────────────────────

TEST( ControlManipulatorTest, TheShapeTransformSizesTheDrawingAndTheSizeIsArithmeticNotImpression )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F, 100.0F, 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Circle3", "CircleXY", 3.0F );

    // Two controls at the same place, differing ONLY in their shape transform. Same library entry, so
    // whatever the drawn radius turns out to be, the ratio between them is this field and nothing else.
    ControlHierarchy rig;
    ControlElement   bare =
         MakeControl( "bare_ctrl", "Circle3", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    const uint32_t unsized = MustAdd( rig, bare );

    ControlElement big =
         MakeControl( "big_ctrl", "Circle3", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    big.ShapeTransform.Scale = glm::vec3( 8.0F );
    const uint32_t sized     = MustAdd( rig, big );
    ASSERT_NE( sized, ControlHierarchy::INVALID );

    rig.Evaluate( skeleton, pose );

    const ManipulatorView view = MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) );
    ManipulatorFrame      frame;
    BuildFrame( rig, library, view, frame );
    ASSERT_EQ( frame.Shapes.size(), 2U );

    // THE NUMBER, NOT "IT GOT BIGGER". A unit circle under a library entry scaled 3 and a control scaled
    // 8 is a circle of radius 24 world units — 24 cm — and every drawn point is exactly that far out.
    for ( const auto& draw : frame.Shapes )
    {
        const float expected = ( draw.Control == sized ) ? 24.0F : 3.0F;
        const glm::vec3 origin = PositionOf( draw.World );
        for ( const glm::vec3& point : draw.WorldPoints )
        {
            EXPECT_NEAR( glm::length( point - origin ), expected, 1e-3F ) << "control " << draw.Control;
        }
    }

    // THE TWO TERMS COMPOSE, they do not replace each other: the sized control is the library's own size
    // times its own, which is the term-for-term claim report 01 §(a)6 makes.
    const ControlShape* entry = library.Find( "Circle3" );
    ASSERT_NE( entry, nullptr );
    for ( const auto& draw : frame.Shapes )
    {
        const glm::mat4 placement =
             draw.World * rig.Get( draw.Control ).ShapeTransform.ToMatrix() * entry->Transform;
        for ( size_t i = 0; i < draw.WorldPoints.size(); ++i )
        {
            const glm::vec3 expected =
                 glm::vec3( placement * glm::vec4( entry->Polylines.front().Points[i], 1.0F ) );
            EXPECT_LT( glm::length( draw.WorldPoints[i] - expected ), 1e-3F ) << "point " << i;
        }
    }

    // AND IT REACHES THE PIXELS THE ANIMATOR GRABS. The unsized control's rim is inside the hit radius of
    // its own ORIGIN, so a test on screen distance alone would pass for both; the sized one's rim is
    // tens of pixels out, and the hit test finds it there. This is the "visible and hittable" half.
    const auto& sizedDraw = ( frame.Shapes[0].Control == sized ) ? frame.Shapes[0] : frame.Shapes[1];
    const auto& bareDraw  = ( frame.Shapes[0].Control == sized ) ? frame.Shapes[1] : frame.Shapes[0];
    // Both circles lie in the same z plane at the same depth, so the pixel ratio is the world ratio
    // exactly rather than approximately; asserting 8 to a tenth of a pixel is therefore honest.
    EXPECT_NEAR( sizedDraw.ScreenRadius, 8.0F * bareDraw.ScreenRadius, 0.1F );
    EXPECT_GT( sizedDraw.ScreenRadius, 10.0F ) << "a control smaller than the grab radius cannot be aimed at";
    EXPECT_LT( bareDraw.ScreenRadius, 10.0F ) << "the premise: an unsized control is a mark, and this is it";

    const glm::vec2 rim = sizedDraw.Origin.Pixel + glm::vec2( sizedDraw.ScreenRadius, 0.0F );
    EXPECT_EQ( HitTest( frame, rim, 4.0F ).Control, sized ) << "the rim of a sized control is grabbable";
    EXPECT_EQ( unsized, rig.Find( "bare_ctrl" ) );
}

TEST( ControlManipulatorTest, SizingAControlDoesNotMoveIt )
{
    // THE POSITIVE CONTROL AGAINST THE DEFECT THIS FIELD EXISTS TO AVOID. Both halves are here on
    // purpose: the first asserts the shape transform leaves the pose alone, and the second BUILDS the
    // rejected implementation — size through `Offset.Scale` — and shows this same assertion catching it.
    // Without the second half the first is a test that would have passed on the defect.
    const Skeleton  skeleton = MakeRig();
    const LocalPose local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose   pose( skeleton, local );

    BoneTransform animated;
    animated.Translation = glm::vec3( 26.4F, 0.0F, 0.0F );

    const auto globalOf = [&skeleton, &local]( const ControlElement& authored, const BoneTransform& animatedPose )
    {
        ComponentPose    scratch( skeleton, local );
        ControlHierarchy rig;
        auto             added = rig.Add( authored );
        EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
        rig.Evaluate( skeleton, scratch );
        EXPECT_TRUE( rig.SetPose( added.GetValue(), animatedPose ).IsSuccess() );
        return rig.GetGlobalTransform( added.GetValue() );
    };

    ControlElement plain =
         MakeControl( "hand_ctrl", "CircleXY", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );

    ControlElement sized      = plain;
    sized.ShapeTransform.Scale = glm::vec3( 8.0F );

    ControlElement viaOffset = plain;
    viaOffset.Offset.Scale   = glm::vec3( 8.0F );

    const glm::mat4 plainGlobal  = globalOf( plain, animated );
    const glm::mat4 sizedGlobal  = globalOf( sized, animated );
    const glm::mat4 offsetGlobal = globalOf( viaOffset, animated );

    // BIT-IDENTICAL, not "close". The shape transform is read by the drawing and by nothing else, so
    // there is no float path along which it could perturb the resolve, and == says exactly that.
    EXPECT_EQ( sizedGlobal, plainGlobal ) << "a resize must not move the control";
    EXPECT_EQ( PositionOf( sizedGlobal ), glm::vec3( 26.4F, 0.0F, 0.0F ) );

    // ...and the rejected route moves it, by the factor it was sized with: 26.4 asked, 211.2 delivered.
    EXPECT_NE( offsetGlobal, plainGlobal );
    EXPECT_NEAR( PositionOf( offsetGlobal ).x, 211.2F, 1e-3F )
         << "the offset's scale multiplies the POSE's translation; that is why size is not authored there";
}

TEST( ControlManipulatorTest, AShapeNameTheLibraryDoesNotHaveIsReportedAndNotSilentlySkipped )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = MustBuiltIn();

    ControlHierarchy rig;
    MustAdd( rig, MakeControl( "typo_ctrl", "Circl", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } ) );
    MustAdd( rig, MakeControl( "quiet_ctrl", "", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } ) );
    rig.Evaluate( skeleton, pose );

    ManipulatorFrame frame;
    BuildFrame( rig, MustBuiltIn(), MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) ), frame );

    EXPECT_TRUE( frame.Shapes.empty() );
    ASSERT_EQ( frame.UnknownShapes.size(), 1U ) << "the empty name is a choice; the typo is a defect";
    EXPECT_EQ( frame.UnknownShapes.front(), "Circl" );
}

// ── projection ─────────────────────────────────────────────────────────────────────────────────────

TEST( ControlManipulatorTest, TheProjectionAgreesWithTheCameraAndRefusesWhatIsBehindIt )
{
    const ManipulatorView view = MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) );

    // Dead centre of the viewport, because that is where the camera is looking.
    const auto centre = ProjectToViewport( view, glm::vec3( 0.0F ) );
    ASSERT_TRUE( centre.InFront );
    EXPECT_NEAR( centre.Pixel.x, kViewportWidth * 0.5F, 1e-2F );
    EXPECT_NEAR( centre.Pixel.y, kViewportHeight * 0.5F, 1e-2F );

    // Y GROWS DOWNWARD. A point above the target must land ABOVE the centre, i.e. at a smaller pixel y.
    const auto above = ProjectToViewport( view, glm::vec3( 0.0F, 50.0F, 0.0F ) );
    ASSERT_TRUE( above.InFront );
    EXPECT_LT( above.Pixel.y, centre.Pixel.y );
    const auto right = ProjectToViewport( view, glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ASSERT_TRUE( right.InFront );
    EXPECT_GT( right.Pixel.x, centre.Pixel.x );

    // BEHIND THE EYE IS "NO", not a mirrored pixel.
    EXPECT_FALSE( ProjectToViewport( view, glm::vec3( 0.0F, 0.0F, 600.0F ) ).InFront );

    // The viewport's own origin is honoured rather than assumed zero — the panel is not at (0,0).
    ManipulatorView offset = view;
    offset.ViewportOrigin  = glm::vec2( 37.0F, 91.0F );
    const auto shifted     = ProjectToViewport( offset, glm::vec3( 0.0F ) );
    ASSERT_TRUE( shifted.InFront );
    EXPECT_NEAR( shifted.Pixel.x - centre.Pixel.x, 37.0F, 1e-3F );
    EXPECT_NEAR( shifted.Pixel.y - centre.Pixel.y, 91.0F, 1e-3F );
}

TEST( ControlManipulatorTest, TheProjectionIsTheCONVENTIONITREPLACED )
{
    // THE ONE THING A REFACTOR OWES: that the value did not move. `ProjectToViewport` replaced a private
    // copy in `LightGizmoRenderer.cpp` (and a third, dead and unsafe, in `Common::Math::SpaceTransformer`),
    // and twenty-odd call sites in the viewport overlay now go through it. The old body is written out
    // here as a GOLDEN REFERENCE rather than trusted to have been copied correctly, because "I moved the
    // same lines" is exactly the claim that an eye cannot check and that a picture of a bone gizmo landing
    // roughly on a joint cannot either.
    const ManipulatorView view = MakeView( glm::vec3( 120.0F, 90.0F, 400.0F ), glm::vec3( -30.0F, 10.0F, 0.0F ) );

    const std::vector<glm::vec3> probes = { glm::vec3( 0.0F ),
                                            glm::vec3( 50.0F, 0.0F, 0.0F ),
                                            glm::vec3( -50.0F, 0.0F, 0.0F ),
                                            glm::vec3( 0.0F, 80.0F, 0.0F ),
                                            glm::vec3( 0.0F, -80.0F, 0.0F ),
                                            glm::vec3( 17.0F, -33.0F, 210.0F ),
                                            glm::vec3( -400.0F, 250.0F, -900.0F ) };

    for ( const glm::vec3& world : probes )
    {
        // Verbatim from the deleted LightGizmoRenderer::ProjectToScreen, viewport-local (origin at zero).
        const glm::vec4 clip                = view.ViewProjection * glm::vec4( world, 1.0f );
        const bool      inFrontByTheOldRule = clip.w > 1e-4f;

        const auto projected = ProjectToViewport( view, world );
        ASSERT_EQ( projected.InFront, inFrontByTheOldRule )
             << "at " << world.x << "," << world.y << "," << world.z;
        if ( !inFrontByTheOldRule )
        {
            continue;
        }

        const glm::vec3 ndc  = glm::vec3( clip ) / clip.w;
        const float     oldX = ( ndc.x * 0.5f + 0.5f ) * kViewportWidth;
        const float     oldY = ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * kViewportHeight;
        EXPECT_FLOAT_EQ( projected.Pixel.x, oldX );
        EXPECT_FLOAT_EQ( projected.Pixel.y, oldY );
    }
}

TEST( ControlManipulatorTest, AShapeBehindTheCameraDrawsNothingAtAll )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Sphere10", "Sphere", 10.0F );

    ControlHierarchy rig;
    ControlElement   behind =
         MakeControl( "behind_ctrl", "Sphere10", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    behind.Offset.Translation = glm::vec3( 0.0F, 0.0F, 900.0F ); // the camera is at z = 300, looking at 0
    MustAdd( rig, behind );
    rig.Evaluate( skeleton, pose );

    ManipulatorFrame frame;
    BuildFrame( rig, library, MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) ), frame );

    ASSERT_EQ( frame.Shapes.size(), 1U ) << "the control still exists; it is its DRAWING that is empty";
    EXPECT_FALSE( frame.Shapes.front().Origin.InFront );
    EXPECT_TRUE( frame.Shapes.front().Screen.empty() ) << "a wholly behind shape must not wrap onto the screen";
    EXPECT_FALSE( frame.Shapes.front().WorldPoints.empty() ) << "the world side is still the truth";
}

TEST( ControlManipulatorTest, AShapeStraddlingTheNearPlaneIsCutRatherThanStreaked )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Sphere100", "Sphere", 100.0F );

    ControlHierarchy rig;
    ControlElement   around =
         MakeControl( "around_ctrl", "Sphere100", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    around.Offset.Translation = glm::vec3( 0.0F, 0.0F, 300.0F ); // centred ON the eye
    MustAdd( rig, around );
    rig.Evaluate( skeleton, pose );

    ManipulatorFrame frame;
    BuildFrame( rig, library, MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) ), frame );

    ASSERT_EQ( frame.Shapes.size(), 1U );
    const auto& draw = frame.Shapes.front();
    EXPECT_FALSE( draw.Screen.empty() ) << "the half in front of the eye is drawable";
    EXPECT_LT( draw.Screen.size(), draw.Segments.size() ) << "the half behind it is not";
    for ( const auto& segment : draw.Screen )
    {
        EXPECT_TRUE( std::isfinite( segment.A.x ) && std::isfinite( segment.A.y ) );
        EXPECT_TRUE( std::isfinite( segment.B.x ) && std::isfinite( segment.B.y ) );
    }
}

// ── hit testing ────────────────────────────────────────────────────────────────────────────────────

TEST( ControlManipulatorTest, TheHitTestAnswersNoWhereThereIsNoWire )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Circle40", "CircleXY", 40.0F );

    ControlHierarchy     rig;
    const ControlElement ctrl =
         MakeControl( "big_ctrl", "Circle40", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    const uint32_t index = MustAdd( rig, ctrl );
    rig.Evaluate( skeleton, pose );

    const ManipulatorView view = MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) );
    ManipulatorFrame      frame;
    BuildFrame( rig, library, view, frame );
    ASSERT_EQ( frame.Shapes.size(), 1U );
    const auto& draw = frame.Shapes.front();
    ASSERT_TRUE( draw.Origin.InFront );
    ASSERT_GT( draw.ScreenRadius, 30.0F ) << "the shape must be big enough for the centre to be far off the rim";

    // POSITIVE: on the rim.
    const auto onRim = ProjectToViewport( view, draw.WorldPoints.front() );
    ASSERT_TRUE( onRim.InFront );
    const auto hit = HitTest( frame, onRim.Pixel, 6.0F );
    EXPECT_EQ( hit.Control, index );
    EXPECT_LE( hit.DistancePx, 6.0F );

    // NEGATIVE, AND IT IS THE SHARP ONE: the centre of the circle is INSIDE the shape's bounding box and
    // nowhere near its wire. A box test, or a "distance to the control's origin" test, says yes here.
    EXPECT_EQ( HitTest( frame, draw.Origin.Pixel, 6.0F ).Control, ControlHierarchy::INVALID );

    // NEGATIVE: the corner of the viewport, where nothing is drawn at all.
    EXPECT_EQ( HitTest( frame, glm::vec2( 2.0F, 2.0F ), 6.0F ).Control, ControlHierarchy::INVALID );

    // NEGATIVE: just outside the radius of a point that is just inside it.
    EXPECT_EQ( HitTest( frame, onRim.Pixel + glm::vec2( 20.0F, 0.0F ), 6.0F ).Control, ControlHierarchy::INVALID );

    // And an empty frame cannot hit anything, which is the degenerate case a "nearest" search gets wrong.
    EXPECT_EQ( HitTest( ManipulatorFrame{}, onRim.Pixel, 6.0F ).Control, ControlHierarchy::INVALID );
}

// ── dragging ───────────────────────────────────────────────────────────────────────────────────────

TEST( ControlManipulatorTest, ATranslateDragPutsTheControlUnderThePointer )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Circle20", "CircleXY", 20.0F );

    ControlHierarchy     rig;
    const ControlElement ctrl =
         MakeControl( "root_ctrl", "Circle20", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    const uint32_t index = MustAdd( rig, ctrl );
    rig.Evaluate( skeleton, pose );

    const ManipulatorView view   = MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) );
    const auto            origin = ProjectToViewport( view, glm::vec3( 0.0F ) );
    ASSERT_TRUE( origin.InFront );

    ControlDrag drag;
    auto        begun = drag.Begin( rig, index, ManipulatorMode::Translate, view, origin.Pixel, 40.0F );
    ASSERT_TRUE( begun.IsSuccess() ) << begun.GetError();
    EXPECT_TRUE( drag.Active() );
    EXPECT_EQ( drag.Control(), index );

    // A POINTER THAT DID NOT MOVE WRITES NOTHING. Exact equality on purpose: the delta is exactly zero,
    // so a pose that drifts here is a pose being reconstructed rather than left alone.
    auto still = drag.Update( rig, view, origin.Pixel );
    ASSERT_TRUE( still.IsSuccess() ) << still.GetError();
    EXPECT_EQ( rig.Get( index ).Pose.Translation, glm::vec3( 0.0F ) );

    // Grabbed exactly on the control's origin, so the control must end up exactly under the new pointer.
    const glm::vec2 moved  = origin.Pixel + glm::vec2( 120.0F, -45.0F );
    auto            update = drag.Update( rig, view, moved );
    ASSERT_TRUE( update.IsSuccess() ) << update.GetError();

    const auto landed = ProjectToViewport( view, PositionOf( rig.GetGlobalTransform( index ) ) );
    ASSERT_TRUE( landed.InFront );
    EXPECT_NEAR( landed.Pixel.x, moved.x, 0.05F );
    EXPECT_NEAR( landed.Pixel.y, moved.y, 0.05F );

    // THE AUTHORED SIDE IS THE LOCAL ONE. The offset — the rig author's — was not touched, and nothing
    // global was stored: a fresh evaluation, which drops every cache, rebuilds the same global from the
    // pose alone.
    EXPECT_EQ( rig.Get( index ).Offset.Translation, glm::vec3( 0.0F ) );
    EXPECT_NE( rig.Get( index ).Pose.Translation, glm::vec3( 0.0F ) );
    const glm::vec3 beforeReset = PositionOf( rig.GetGlobalTransform( index ) );
    pose.Invalidate();
    rig.Evaluate( skeleton, pose );
    EXPECT_LT( glm::length( PositionOf( rig.GetGlobalTransform( index ) ) - beforeReset ), 1e-3F );

    drag.End();
    EXPECT_FALSE( drag.Active() );
    EXPECT_FALSE( drag.Update( rig, view, moved ).IsSuccess() ) << "an ended drag must refuse to write";
}

TEST( ControlManipulatorTest, AParentMovingMidDragDoesNotPinTheControlToWhereItWas )
{
    // THE HOLE T5.1 CLOSED, SEEN FROM THE WRITING SIDE. A manipulator that remembered the control's
    // GLOBAL at the grab and re-asserted it every frame would hold the control at the parent's old place
    // — the dragged hand that stops following the arm. What is remembered here is the LOCAL pose, so a
    // still pointer leaves it bit-identical and the control travels with its parent.
    const Skeleton            skeleton = MakeRig();
    LocalPose                 local    = PoseWithChestAt( glm::vec3( 0.0F, 100.0F, 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Circle15", "CircleXY", 15.0F );

    ControlHierarchy     rig;
    const ControlElement hand =
         MakeControl( "hand_ctrl", "Circle15", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } );
    const uint32_t index = MustAdd( rig, hand );
    rig.Evaluate( skeleton, pose );

    const ManipulatorView view   = MakeView( glm::vec3( 0.0F, 100.0F, 300.0F ), glm::vec3( 0.0F, 100.0F, 0.0F ) );
    const auto            grabAt = ProjectToViewport( view, PositionOf( rig.GetGlobalTransform( index ) ) );
    ASSERT_TRUE( grabAt.InFront );

    ControlDrag drag;
    ASSERT_TRUE( drag.Begin( rig, index, ManipulatorMode::Translate, view, grabAt.Pixel, 20.0F ).IsSuccess() );
    const BoneTransform atGrab = drag.PoseAtGrab();
    EXPECT_EQ( atGrab.Translation, glm::vec3( 0.0F ) );

    // The chest moves 30 cm up while the drag is held and the pointer has not moved.
    local[1].Translation = glm::vec3( 0.0F, 130.0F, 0.0F );
    pose.Invalidate();
    rig.Evaluate( skeleton, pose );

    auto update = drag.Update( rig, view, grabAt.Pixel );
    ASSERT_TRUE( update.IsSuccess() ) << update.GetError();

    EXPECT_EQ( rig.Get( index ).Pose.Translation, atGrab.Translation ) << "the drag rewrote a still pointer";
    const glm::vec3 now = PositionOf( rig.GetGlobalTransform( index ) );
    EXPECT_LT( glm::length( now - glm::vec3( 0.0F, 130.0F, 0.0F ) ), 1e-3F )
         << "the control was pinned to where its parent used to be";
    EXPECT_GT( now.y, 100.5F ) << "the negative control: 100 is the parent's OLD position";
}

TEST( ControlManipulatorTest, ARotateDragTurnsTheControlWithoutMovingIt )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = LibraryWithSized( "Sphere30", "Sphere", 30.0F );

    ControlHierarchy     rig;
    const ControlElement ctrl =
         MakeControl( "spin_ctrl", "Sphere30", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    const uint32_t index = MustAdd( rig, ctrl );
    rig.Evaluate( skeleton, pose );

    const ManipulatorView view   = MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) );
    const auto            origin = ProjectToViewport( view, glm::vec3( 0.0F ) );
    ASSERT_TRUE( origin.InFront );

    const glm::quat before = rig.Get( index ).Pose.Rotation;

    ControlDrag drag;
    ASSERT_TRUE( drag.Begin( rig, index, ManipulatorMode::Rotate, view, origin.Pixel, 100.0F ).IsSuccess() );

    // A still pointer is still no rotation.
    ASSERT_TRUE( drag.Update( rig, view, origin.Pixel ).IsSuccess() );
    EXPECT_LT( glm::angle( glm::normalize( rig.Get( index ).Pose.Rotation * glm::inverse( before ) ) ), 1e-4F );

    // Centre to rim is a quarter turn: the grab vector points out of the screen, the pointer at +radius
    // in x points along screen-right, and those are ninety degrees apart on the trackball.
    ASSERT_TRUE( drag.Update( rig, view, origin.Pixel + glm::vec2( 100.0F, 0.0F ) ).IsSuccess() );

    const glm::quat after = glm::normalize( rig.Get( index ).Pose.Rotation );
    const glm::quat delta = glm::normalize( after * glm::inverse( before ) );
    EXPECT_NEAR( glm::angle( delta ), glm::half_pi<float>(), 1e-3F );

    // The camera looks down -Z with world up as screen up, so the turn is about world Y.
    EXPECT_GT( std::abs( glm::dot( glm::axis( delta ), glm::vec3( 0.0F, 1.0F, 0.0F ) ) ), 0.999F );

    // A ROTATION ABOUT THE CONTROL'S OWN ORIGIN DOES NOT MOVE IT.
    EXPECT_LT( glm::length( PositionOf( rig.GetGlobalTransform( index ) ) ), 1e-3F );
    EXPECT_EQ( rig.Get( index ).Pose.Translation, glm::vec3( 0.0F ) );
}

TEST( ControlManipulatorTest, ADragRefusesWhatItCannotMeasure )
{
    const Skeleton            skeleton = MakeRig();
    const LocalPose           local    = PoseWithChestAt( glm::vec3( 0.0F ) );
    ComponentPose             pose( skeleton, local );
    const ControlShapeLibrary library = MustBuiltIn();

    ControlHierarchy     rig;
    const ControlElement ctrl =
         MakeControl( "ctrl", "CircleXY", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    const uint32_t index = MustAdd( rig, ctrl );

    ControlElement behind =
         MakeControl( "behind", "CircleXY", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } );
    behind.Offset.Translation  = glm::vec3( 0.0F, 0.0F, 900.0F );
    const uint32_t behindIndex = MustAdd( rig, behind );
    rig.Evaluate( skeleton, pose );

    const ManipulatorView view = MakeView( glm::vec3( 0.0F, 0.0F, 300.0F ), glm::vec3( 0.0F ) );
    ControlDrag           drag;

    EXPECT_FALSE(
         drag.Begin( rig, 99U, ManipulatorMode::Translate, view, glm::vec2( 400.0F, 300.0F ), 20.0F ).IsSuccess() )
         << "a control that is not in the rig";
    EXPECT_FALSE(
         drag.Begin( rig, behindIndex, ManipulatorMode::Translate, view, glm::vec2( 400.0F, 300.0F ), 20.0F )
              .IsSuccess() )
         << "a control behind the camera has no screen position to measure from";
    EXPECT_FALSE( drag.Update( rig, view, glm::vec2( 400.0F, 300.0F ) ).IsSuccess() )
         << "a refused Begin must not leave a drag armed";

    ManipulatorView noPixels = view;
    noPixels.ViewportSize    = glm::vec2( 0.0F, 0.0F );
    EXPECT_FALSE(
         drag.Begin( rig, index, ManipulatorMode::Translate, noPixels, glm::vec2( 0.0F ), 20.0F ).IsSuccess() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
