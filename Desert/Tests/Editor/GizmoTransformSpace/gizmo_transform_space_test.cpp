// У5-2 — the gizmo's transform space, and the matrix composition underneath it.
//
// THE POINT OF THIS SUITE IS THE SECOND HALF. A World/Local toggle is easy to add and easy to believe:
// the button lights up, the handles change colour, and it looks done. What a spot check will not catch
// is that a TRS triple CANNOT HOLD A SHEAR, and that scaling a rotated object along a world axis produces
// exactly that. The gizmo's write-back decomposes into translation / euler / scale and drops glm's skew
// output on the floor; when the skew is zero that discard is lossless, and when it is not, the object
// silently changes into a shape the handles never showed.
//
// So the assertions here are RELATIONS between the two spaces, not values in either:
//
//   * local-space scale under a rotated parent -> skew is exactly zero, the discard costs nothing;
//   * world-space scale under the SAME parent  -> skew is NOT zero, and that is why ImGuizmo refuses to
//     honour a World mode for scaling at all (ImGuizmo.cpp:2653);
//   * EffectiveSpace() reports that refusal, so the toolbar and the handles cannot disagree.
//
// The middle one is the negative control. Without it every skew assertion would also pass on a
// WorldToLocalTRS that hard-coded Skew = 0, which is precisely the shape of the defect being guarded.

#include <gtest/gtest.h>

#include <Editor/Core/GizmoState.hpp>
#include <Editor/Panels/ViewportPanel/Tools/GizmoTransformMath.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp> // glm::toMat4
#include <glm/gtx/transform.hpp>

#include <utility>

using Desert::Editor::Core::GizmoState;
using Desert::Editor::Tools::LocalTRS;
using Desert::Editor::Tools::WorldToLocalTRS;

using Operation = GizmoState::Operation;
using Space     = GizmoState::Space;

namespace
{
    // The same composition TransformComponent::GetTransform() performs (Components.hpp:427). Written out
    // here rather than included, because pulling Components.hpp in would drag entt and the whole ECS into
    // a suite whose subject is two headers of maths.
    glm::mat4 TRS( const glm::vec3& t, const glm::vec3& eulerRadians, const glm::vec3& s )
    {
        return glm::translate( glm::mat4( 1.0f ), t ) * glm::toMat4( glm::quat( eulerRadians ) ) *
               glm::scale( glm::mat4( 1.0f ), s );
    }

    // A parent that is ROTATED, which is the only case in which the two spaces differ at all. A test built
    // on an axis-aligned parent would pass with the mode argument ignored entirely.
    glm::mat4 RotatedParent()
    {
        return TRS( { 12.0f, -3.0f, 40.0f }, { 0.4f, 0.7f, -0.25f }, { 1.0f, 1.0f, 1.0f } );
    }

    constexpr float kEps = 1.0e-4f;
} // namespace

// ── The space in force, and the one place that knows ImGuizmo overrules it ───────────────────────────

// ImGuizmo.cpp:2653 passes LOCAL for any operation containing SCALE, whatever mode it was handed. If this
// class reported the stored space during a scale, the toolbar would read "World" while the handles moved
// along the object's own axes -- a control describing a state the frame does not have.
TEST( GizmoTransformSpace, ScalingIsAlwaysLocalWhicheverSpaceTheUserChose )
{
    for ( const Space chosen : { Space::World, Space::Local } )
    {
        GizmoState::SetSpace( chosen );
        EXPECT_EQ( GizmoState::EffectiveSpace( Operation::Scale ), Space::Local )
             << "scale must report Local regardless of the stored space";
        EXPECT_TRUE( GizmoState::SpaceIsForced( Operation::Scale ) );
    }
}

// The mirror of the above: for everything else the user's choice IS the space, or the toggle would be a
// dead control that never reaches a Manipulate() call.
TEST( GizmoTransformSpace, TranslateAndRotateHonourTheChosenSpace )
{
    for ( const Space chosen : { Space::World, Space::Local } )
    {
        GizmoState::SetSpace( chosen );
        for ( const Operation op : { Operation::Translate, Operation::Rotate, Operation::None } )
        {
            EXPECT_EQ( GizmoState::EffectiveSpace( op ), chosen ) << "operation " << static_cast<int>( op );
            EXPECT_FALSE( GizmoState::SpaceIsForced( op ) ) << "operation " << static_cast<int>( op );
        }
    }
    GizmoState::SetSpace( Space::World ); // leave the shared static as the editor starts it
}

// The enum has to keep matching ImGuizmo::MODE, because GizmoController static_casts straight across.
// ImGuizmo.h:209 -- enum MODE { LOCAL, WORLD }.
TEST( GizmoTransformSpace, SpaceValuesMatchImGuizmoMode )
{
    EXPECT_EQ( static_cast<int>( Space::Local ), 0 );
    EXPECT_EQ( static_cast<int>( Space::World ), 1 );
}

// ── The composition: what a drag in each space actually writes back ──────────────────────────────────

// A LOCAL drag post-multiplies in the object's own frame (ImGuizmo's LOCAL mode builds its axes from the
// object's orthonormalized world basis). Under a rotated parent that still lands on a clean TRS: nothing
// is discarded, so what the handles showed is what the entity gets.
TEST( GizmoTransformSpace, LocalScaleUnderARotatedParentLosesNothing )
{
    const glm::mat4 parent = RotatedParent();
    const glm::mat4 local  = TRS( { 5.0f, 0.0f, 2.0f }, { 0.0f, 0.9f, 0.0f }, { 1.0f, 1.0f, 1.0f } );

    // Scale along the object's OWN axes == post-multiplication.
    const glm::mat4 dragged = ( parent * local ) * glm::scale( glm::mat4( 1.0f ), { 2.0f, 3.0f, 0.5f } );

    const LocalTRS out = WorldToLocalTRS( parent, dragged );

    EXPECT_TRUE( out.IsLossless() ) << "a local-axis scale must decompose without shear; skew was (" << out.Skew.x
                                    << ", " << out.Skew.y << ", " << out.Skew.z << ")";
    EXPECT_NEAR( out.Scale.x, 2.0f, kEps );
    EXPECT_NEAR( out.Scale.y, 3.0f, kEps );
    EXPECT_NEAR( out.Scale.z, 0.5f, kEps );
}

// THE NEGATIVE CONTROL, and the reason the assertion above means anything. The identical drag applied
// along WORLD axes is a shear once the object is rotated, so the TRS write-back MUST lose something. If
// this test ever goes green, WorldToLocalTRS has stopped reporting skew and every "lossless" claim in
// this file has quietly become unfalsifiable.
TEST( GizmoTransformSpace, WorldScaleOfARotatedObjectIsAShearAndCannotBeStored )
{
    const glm::mat4 parent = RotatedParent();
    const glm::mat4 local  = TRS( { 5.0f, 0.0f, 2.0f }, { 0.0f, 0.9f, 0.0f }, { 1.0f, 1.0f, 1.0f } );

    // Scale along the WORLD axes == pre-multiplication.
    const glm::mat4 dragged = glm::scale( glm::mat4( 1.0f ), { 2.0f, 3.0f, 0.5f } ) * ( parent * local );

    const LocalTRS out = WorldToLocalTRS( parent, dragged );

    EXPECT_FALSE( out.IsLossless() )
         << "a world-axis scale of a rotated object shears it; a translation/rotation/scale triple cannot "
            "hold that, and this is exactly what ImGuizmo forcing LOCAL for scale exists to prevent";
    EXPECT_GT( glm::length( out.Skew ), kEps );
}

// Rotation is the operation where BOTH spaces are legal, so the write-back has to be exact in both. The
// relation asserted is the one a user would notice: a rotation drag moves nothing but the rotation --
// world position and scale come back bit-for-bit, under a rotated parent, in either space.
TEST( GizmoTransformSpace, RotatingInEitherSpaceMovesNeitherThePositionNorTheScale )
{
    const glm::mat4 parent   = RotatedParent();
    const glm::vec3 localPos = { 5.0f, 0.0f, 2.0f };
    const glm::vec3 localScl = { 1.5f, 2.5f, 0.75f };
    const glm::mat4 local    = TRS( localPos, { 0.0f, 0.9f, 0.0f }, localScl );

    const glm::mat4 world       = parent * local;
    const glm::vec3 worldPosWas = glm::vec3( world[3] );

    // A rotation about the object's own origin, expressed in each space. Both must leave the pivot fixed.
    const glm::quat spin = glm::quat( glm::vec3{ 0.0f, 0.3f, 0.0f } );

    // World-axis spin: turn the whole world matrix about the world Y through the object's own position.
    const glm::mat4 pivot     = glm::translate( glm::mat4( 1.0f ), worldPosWas );
    const glm::mat4 worldDrag = pivot * glm::toMat4( spin ) * glm::inverse( pivot ) * world;

    // Own-axis spin: the object's ROTATION gains the turn, between the translation and the scale.
    //
    // NOT `world * toMat4( spin )`, which is the intuitive spelling and is wrong: post-multiplying the
    // whole TRS applies the turn INSIDE the already-scaled frame, so a non-uniform scale comes back
    // shrunk and sheared (it cost this test a red run: 1.5 arrived as 1.45). ImGuizmo does not do that
    // either -- LOCAL mode builds its axes from mModelLocal, which is ORTHONORMALIZED first
    // (ImGuizmo.cpp:1073), precisely so the scale is not in the rotation's way.
    const glm::mat4 localDrag =
         parent * TRS( localPos, glm::eulerAngles( glm::quat( glm::vec3{ 0.0f, 0.9f, 0.0f } ) * spin ), localScl );

    for ( const auto& [name, dragged] : { std::pair{ "world", worldDrag }, std::pair{ "local", localDrag } } )
    {
        const LocalTRS out = WorldToLocalTRS( parent, dragged );

        EXPECT_TRUE( out.IsLossless() ) << name << ": a rotation must not shear anything";

        // The pivot did not move.
        EXPECT_NEAR( out.Translation.x, localPos.x, kEps ) << name;
        EXPECT_NEAR( out.Translation.y, localPos.y, kEps ) << name;
        EXPECT_NEAR( out.Translation.z, localPos.z, kEps ) << name;

        // ...and neither did the size. A rotation that quietly renormalizes the scale is a classic
        // decompose defect: it looks right for one drag and drifts over a hundred of them.
        EXPECT_NEAR( out.Scale.x, localScl.x, kEps ) << name;
        EXPECT_NEAR( out.Scale.y, localScl.y, kEps ) << name;
        EXPECT_NEAR( out.Scale.z, localScl.z, kEps ) << name;
    }
}

// The conversion has to be the identity when nothing was dragged, for any parent -- otherwise merely
// HOVERING a gizmo on a child entity would drift its transform, which is the shape of defect that only
// shows up after a few hundred frames.
TEST( GizmoTransformSpace, AnUndraggedChildRoundTripsExactly )
{
    const glm::mat4 parent = RotatedParent();
    const glm::vec3 t      = { 5.0f, -1.0f, 2.0f };
    const glm::vec3 r      = { 0.1f, 0.9f, -0.4f };
    const glm::vec3 s      = { 1.5f, 2.5f, 0.75f };

    const LocalTRS out = WorldToLocalTRS( parent, parent * TRS( t, r, s ) );

    EXPECT_TRUE( out.IsLossless() );
    EXPECT_NEAR( out.Translation.x, t.x, kEps );
    EXPECT_NEAR( out.Translation.y, t.y, kEps );
    EXPECT_NEAR( out.Translation.z, t.z, kEps );
    EXPECT_NEAR( out.Scale.x, s.x, kEps );
    EXPECT_NEAR( out.Scale.y, s.y, kEps );
    EXPECT_NEAR( out.Scale.z, s.z, kEps );

    // Euler triples are not unique, so the rotation is compared as the matrix it rebuilds rather than
    // component-wise -- a comparison on the angles themselves would fail on an equivalent orientation.
    const glm::mat4 rebuilt = glm::toMat4( glm::quat( out.Rotation ) );
    const glm::mat4 wanted  = glm::toMat4( glm::quat( r ) );
    for ( int c = 0; c < 3; ++c )
        for ( int rr = 0; rr < 3; ++rr )
            EXPECT_NEAR( rebuilt[c][rr], wanted[c][rr], kEps ) << "column " << c << " row " << rr;
}

// A ROOT entity has no parent, so the conversion must be a plain decomposition. This is the path almost
// every drag in the editor actually takes, and it would be embarrassing to only test the child one.
TEST( GizmoTransformSpace, ARootEntityDecomposesItsWorldMatrixDirectly )
{
    const glm::vec3 t = { 100.0f, 25.0f, -60.0f };
    const glm::vec3 s = { 2.0f, 2.0f, 2.0f };

    const LocalTRS out = WorldToLocalTRS( glm::mat4( 1.0f ), TRS( t, { 0.0f, 1.2f, 0.0f }, s ) );

    EXPECT_TRUE( out.IsLossless() );
    EXPECT_NEAR( out.Translation.x, t.x, kEps );
    EXPECT_NEAR( out.Translation.y, t.y, kEps );
    EXPECT_NEAR( out.Translation.z, t.z, kEps );
    EXPECT_NEAR( out.Scale.x, s.x, kEps );
}
