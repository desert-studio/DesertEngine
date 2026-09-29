#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Editor::ThumbnailFraming
{
    /**
     * @brief The framing rule for offscreen thumbnail captures, pure and in one place.
     *
     * The subject is placed and scaled from the capture camera's OWN view/projection, never from an
     * assumed camera pose. That distinction is the whole of Д30. The renderer used to assume its preview
     * camera "sits at ~(-4.33, 6.12, -4.33) looking at the origin (distance ~8.66)" and framed a material
     * by scaling a unit sphere at the world origin to span a fixed 4 units. Both halves of that assumption
     * were metre-era survivors of the centimetre migration, and they compound:
     *
     *   - the shared preview primitive is scaled by Common::Units::UnitsPerMetre, so it is 100 units
     *     across, not one. Framed as if it were unit-sized it was drawn 100x too big: a 400-unit sphere;
     *   - the engine's default EditorCamera moved to eye height — focal (0, 200, 0), eye (-70, 186, -70) —
     *     so a subject left at the world origin now sits 200 units BELOW where the camera aims.
     *
     * Measured under that camera at the capture resolution (fovY 76.3 deg): the subject's centre projects
     * to NDC y = -3.50 against a frame that spans -1..1, and lies 70.0 deg off the view axis against a
     * 38.1 deg half-FOV — outside the frustum. What the capture actually contained was the flank of that
     * 400-unit sphere, whose surface the camera (212 units from its centre, radius 200) was practically
     * resting on: a wall of albedo under sky, with no sphere in it.
     *
     * Old thumbnails captured before the migration masked both, until a material Save deleted one PNG and
     * forced a re-capture with the migrated code, which is when the sphere "disappeared".
     *
     * The cure is to stop assuming: read where the camera IS and where it LOOKS from its matrices, put the
     * subject on that view axis (so it projects to screen centre) and solve the scale that makes its
     * measured extent fill a chosen fraction of the frame under the camera's ACTUAL projection — whatever
     * near/far/FOV or size-anchored editor projection that is. No camera constant appears here to drift.
     */

    // Fraction of the frame's half-height the subject's largest extent should fill. 0.62 reproduces the
    // pre-migration framing (subject ~60% of the frame) that the reference thumbnails were captured with.
    inline constexpr float kFillFraction = 0.62f;

    // Where along the view axis to place the subject, in world units from the camera. Only its RATIO to
    // the solved world span matters on screen (both scale with distance under a perspective projection),
    // so this just needs to sit comfortably between the near plane and the subject's own half-span. 300
    // units (3 m) clears any sane near plane with a subject a metre or two across.
    inline constexpr float kViewAxisDistance = 300.0f;

    struct Frame
    {
        glm::vec3 Center{ 0.0f };
        float     Extent = 0.0f; // largest world-space dimension of the union AABB
        bool      Valid  = false;
    };

    /// The frame of one mesh-space box. An empty box (min above max: no submeshes, no vertices) is NOT a
    /// frame: Valid stays false and the capture is REFUSED with the reason (AssetThumbnailRenderer::Request*).
    /// There is no stand-in extent — a made-up 1-unit subject put the camera inside the real one and
    /// photographed the sky as if it were the asset.
    inline Frame FrameOfBox( const glm::vec3& mn, const glm::vec3& mx )
    {
        Frame frame;
        if ( mx.x < mn.x )
            return frame;

        frame.Center         = ( mn + mx ) * 0.5f;
        const glm::vec3 size = mx - mn;
        frame.Extent         = std::max( size.x, std::max( size.y, size.z ) );
        frame.Valid          = frame.Extent > 0.0f;
        return frame;
    }

    /**
     * @brief Union of the submeshes' AABBs in mesh space: each submesh transform applied to its box's 8
     *        corners. Meshes with per-submesh transforms frame correctly this way (an axis-aligned union
     *        of untransformed boxes came out huge or off-screen for some).
     *
     * Templated on the range so a test can feed a stub with the same two fields (Transform, BoundingBox)
     * without linking mesh code that cannot exist away from a GPU.
     */
    template <typename SubmeshRange>
    inline Frame MeasureSubmeshes( const SubmeshRange& submeshes )
    {
        glm::vec3 mn( 1e9f ), mx( -1e9f );
        for ( const auto& sm : submeshes )
        {
            const glm::vec3 lo = sm.BoundingBox.Min, hi = sm.BoundingBox.Max;
            for ( int corner = 0; corner < 8; ++corner )
            {
                const glm::vec3 p( ( corner & 1 ) ? hi.x : lo.x, ( corner & 2 ) ? hi.y : lo.y,
                                   ( corner & 4 ) ? hi.z : lo.z );
                const glm::vec3 w = glm::vec3( sm.Transform * glm::vec4( p, 1.0f ) );
                mn                = glm::min( mn, w );
                mx                = glm::max( mx, w );
            }
        }

        return FrameOfBox( mn, mx );
    }

    // The world transform (uniform scale + translation) that frames a subject of the given extent/center
    // in the camera's view. Pure: matrices in, transform out — assertable without a device.
    struct Placement
    {
        glm::vec3 Translation{ 0.0f };
        float     Scale = 1.0f;
    };

    /**
     * @brief Frame a subject in the camera's view.
     *
     * @param view       camera world->view matrix (Camera::GetViewMatrix()).
     * @param projection camera view->clip matrix (Camera::GetProjectionMatrix()).
     * @param extent     the subject's largest world-space dimension, MEASURED from the mesh actually drawn.
     * @param center     the subject's own centre in its local space (subtracted so it lands on the axis).
     *
     * The subject centre is put on the view axis at @ref kViewAxisDistance (screen centre), then the
     * on-screen size of one world unit at that depth is measured by projecting points through
     * view*projection on both screen axes, and the scale is solved so @p extent fills @ref kFillFraction of
     * the frame on the TIGHTER of them. A degenerate extent falls back to unit scale rather than dividing
     * by ~zero.
     */
    inline Placement PlaceInView( const glm::mat4& view, const glm::mat4& projection, float extent,
                                  const glm::vec3& center )
    {
        // Camera world pose from the inverse view: translation is the eye, -Z the forward, +X the right,
        // +Y the up.
        const glm::mat4 invView = glm::inverse( view );
        const glm::vec3 eye     = glm::vec3( invView[3] );
        const glm::vec3 forward = -glm::normalize( glm::vec3( invView[2] ) );
        const glm::vec3 right   = glm::normalize( glm::vec3( invView[0] ) );
        const glm::vec3 up      = glm::normalize( glm::vec3( invView[1] ) );

        // Subject centre on the view axis -> projects to the frame centre for any positive distance.
        const glm::vec3 axisPoint = eye + forward * kViewAxisDistance;

        const glm::mat4 viewProj = projection * view;
        auto            toNdc    = [&]( const glm::vec3& p )
        {
            const glm::vec4 clip = viewProj * glm::vec4( p, 1.0f );
            // Behind the camera or on the plane: no meaningful projection. Guard the divide.
            const float w = std::abs( clip.w ) < 1e-6f ? 1e-6f : clip.w;
            return glm::vec2( clip.x / w, clip.y / w );
        };

        // NDC displacement produced by one world unit at the subject's depth, measured on BOTH screen axes
        // and resolved to the TIGHTER of the two. NDC spans [-1, 1] on each axis, so a frame that is not
        // square projects a world unit differently sideways than vertically; solving the scale from the
        // horizontal alone would fit the subject across a 16:9 frame and let it run off the top and bottom.
        // Taking the larger NDC-per-world is what makes @ref kFillFraction a promise about the whole frame
        // rather than about whichever axis was sampled. (The capture target is square today, so this costs
        // one extra projection and buys the rule its generality.)
        const glm::vec2 ndcAt       = toNdc( axisPoint );
        const glm::vec2 ndcSide     = toNdc( axisPoint + right );
        const glm::vec2 ndcUp       = toNdc( axisPoint + up );
        const float     ndcPerWorld = std::max( glm::length( ndcSide - ndcAt ), glm::length( ndcUp - ndcAt ) );

        Placement placement;
        if ( extent <= 1e-4f || ndcPerWorld <= 1e-6f )
        {
            placement.Scale       = 1.0f;
            placement.Translation = axisPoint - center;
            return placement;
        }

        const float worldSpan = ( kFillFraction * 2.0f ) / ndcPerWorld; // world units to fill the frame
        placement.Scale       = worldSpan / extent;
        placement.Translation = axisPoint - center * placement.Scale;
        return placement;
    }
} // namespace Desert::Editor::ThumbnailFraming
