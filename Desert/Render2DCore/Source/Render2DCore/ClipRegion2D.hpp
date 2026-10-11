#pragma once

#include <Render2DCore/Transform2D.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

// The 2D clip region of the whole UI: ONE description of "which screen pixels survive", read FORWARD by
// the draw list (geometry outside it is cut away before it is ever a vertex) and POINTWISE by the hit test
// (a pointer outside it elects nothing). The picture and the pointer cannot disagree about where a clip is
// because there is nothing for them to disagree with — they ask the same object the same question.
//
// WHY IT IS A BOX PLUS HALF-PLANES AND NOT A SCISSOR. Ю8 stored a clip as the screen-space bounding box of
// the (possibly rotated) clipper, because a hardware scissor is axis-aligned and cannot express anything
// else. That is conservative: a panel turned 45 degrees clipped its children to the square AROUND it, so a
// row scrolled out of view stayed visible in the corners. The exact region is the box INTERSECTED WITH the
// clipper's own four edges, and those edges are oblique — so the region is a convex polygon: an axis-aligned
// box (which the scissor still cuts, for free, in hardware) plus the oblique half-planes the scissor cannot.
//
// WHY THE OBLIQUE HALF ENDS UP ON THE CPU rather than in a stencil buffer or a fragment mask, measured and
// written down here because it is the decision this file exists to record:
//
//   * a stencil needs a stencil attachment on the UI's target, stencil state on all three pipelines, and a
//     mask draw plus an unmask draw per clip level — two draw calls per rotated clipper on a batcher whose
//     budget is already measured;
//   * a fragment mask needs the planes in push constants. UI2D/UIText have 64 of their 128 guaranteed bytes
//     free, but UIGlass has ZERO — its block is exactly 128 already — so glass would have had to be either
//     excluded (a rotated clipper that clips every element except the glass one) or moved to a uniform
//     buffer with per-(frame x slot) state;
//   * cutting the geometry on the CPU costs NO draw call, NO pipeline state, NO push-constant byte and NO
//     shader edit: the batch key is unchanged, so two primitives that shared a command still share it. It is
//     also the only one of the three that works identically for all three pipelines, glass included, because
//     glass's mask is evaluated from gl_FragCoord and does not care which triangles cover the fragment.
//
// What it does NOT give, said plainly: the cut edge is hard, decided at the pixel centre, exactly like every
// other edge Render2D draws — an element's own rotated border is aliased too, and so was the scissor this
// replaces. Antialiasing the clip alone would make it the one soft edge in an otherwise hard subsystem.
namespace Desert::Graphic::Render2D
{
    // Four half-planes per rotated clipper, so this is four levels of rotated nesting. Beyond it the extra
    // levels still tighten the BOX and the region stays a conservative superset — the pointer and the
    // picture continue to agree, they just agree about a slightly larger region. IntersectClipRegion says
    // so in its return value rather than dropping the planes quietly.
    inline constexpr uint32_t kMaxClipPlanes = 16;

    // A convex clip region in SCREEN pixels.
    struct ClipRegion2D
    {
        // x, y, w, h. Meaningful only while Bounded; W or H <= 0 then means the region is EMPTY, which is a
        // different statement from "there is no clip" and used to be indistinguishable from it — the backend
        // read `ClipRect.z <= 0` as "unclipped", so two disjoint nested clips drew over the WHOLE viewport
        // while the pointer was refused everywhere. Bounded is what separates the two.
        glm::vec4 Box = { 0.0f, 0.0f, 0.0f, 0.0f };

        // Inside <=> P.x*p.x + P.y*p.y + P.z >= 0, with (P.x,P.y) a UNIT normal, so the value is a signed
        // distance in screen pixels. Only obliques live here: an axis-aligned constraint is exactly what Box
        // already expresses, and folding it there is what keeps the unrotated case free of any plane at all.
        std::array<glm::vec3, kMaxClipPlanes> Planes{};
        uint32_t                              PlaneCount = 0;

        bool Bounded = false;
    };

    inline bool ClipRegionEmpty( const ClipRegion2D& r )
    {
        return r.Bounded && ( r.Box.z <= 0.0f || r.Box.w <= 0.0f );
    }

    // Signed distance from @p p to @p plane's boundary, in screen pixels; positive inside.
    inline float ClipPlaneDistance( const glm::vec3& plane, const glm::vec2& p )
    {
        return plane.x * p.x + plane.y * p.y + plane.z;
    }

    // THE POINTER'S HALF of the region. Inclusive on the box's edges, matching the rect test it replaces.
    inline bool ClipRegionContains( const ClipRegion2D& r, const glm::vec2& p )
    {
        if ( ClipRegionEmpty( r ) )
            return false;
        if ( r.Bounded &&
             ( p.x < r.Box.x || p.x > r.Box.x + r.Box.z || p.y < r.Box.y || p.y > r.Box.y + r.Box.w ) )
            return false;
        for ( uint32_t i = 0; i < r.PlaneCount; ++i )
            if ( ClipPlaneDistance( r.Planes[i], p ) < 0.0f )
                return false;
        return true;
    }

    namespace Detail
    {
        // Is every corner of the axis-aligned box @p b on the inside of @p plane? Then the plane constrains
        // nothing the box does not already constrain and carrying it is waste.
        inline bool BoxInsidePlane( const glm::vec4& b, const glm::vec3& plane )
        {
            return ClipPlaneDistance( plane, { b.x, b.y } ) >= 0.0f &&
                   ClipPlaneDistance( plane, { b.x + b.z, b.y } ) >= 0.0f &&
                   ClipPlaneDistance( plane, { b.x + b.z, b.y + b.w } ) >= 0.0f &&
                   ClipPlaneDistance( plane, { b.x, b.y + b.w } ) >= 0.0f;
        }

        inline bool QuadInsidePlane( const glm::vec2 ( &q )[4], const glm::vec3& plane )
        {
            return ClipPlaneDistance( plane, q[0] ) >= 0.0f && ClipPlaneDistance( plane, q[1] ) >= 0.0f &&
                   ClipPlaneDistance( plane, q[2] ) >= 0.0f && ClipPlaneDistance( plane, q[3] ) >= 0.0f;
        }

        inline bool QuadOutsidePlane( const glm::vec2 ( &q )[4], const glm::vec3& plane )
        {
            return ClipPlaneDistance( plane, q[0] ) < 0.0f && ClipPlaneDistance( plane, q[1] ) < 0.0f &&
                   ClipPlaneDistance( plane, q[2] ) < 0.0f && ClipPlaneDistance( plane, q[3] ) < 0.0f;
        }
    } // namespace Detail

    // Does one of the region's OBLIQUE half-planes reject the whole convex quad @p q — every corner of it
    // on the outside — so that no triangle of it can survive the cut? Asked about the quad and never about
    // its bounding box: the box of an element a rotated clipper removed entirely still overlaps that
    // clipper's box, which is exactly why an introspection column computed from boxes under-reports as soon
    // as a clipper is turned. Conservative in the safe direction — a quad that straddles two planes and
    // survives neither is not detected — so it never calls a visible element fully cut.
    //
    // The BOX half of the region is deliberately not asked here: callers that need it already intersect
    // boxes for other reasons and would otherwise do that work twice.
    inline bool ClipPlanesRejectQuad( const ClipRegion2D& r, const glm::vec2 ( &q )[4] )
    {
        for ( uint32_t i = 0; i < r.PlaneCount; ++i )
            if ( Detail::QuadOutsidePlane( q, r.Planes[i] ) )
                return true;
        return false;
    }

    // Intersect @p region with the rectangle @p min..@p max mapped through @p xform. THE ONE IMPLEMENTATION:
    // the draw list calls it to narrow what it emits, and the canvas walk calls it to narrow what the
    // pointer may elect. A second implementation of this is precisely how a clip that looks right on screen
    // stops agreeing with the cursor.
    //
    // Returns false when the region had to stay LOOSER than the exact intersection because the plane budget
    // was full. Never returns a region that is too tight: a caller that ignores the answer still gets a
    // superset, and the pointer gets the same superset.
    [[nodiscard]] inline bool IntersectClipRegion( ClipRegion2D& region, const glm::mat3& xform,
                                                   const glm::vec2& min, const glm::vec2& max )
    {
        const glm::vec2 quad[4] = {
             TransformPoint2D( xform, { min.x, min.y } ), TransformPoint2D( xform, { max.x, min.y } ),
             TransformPoint2D( xform, { max.x, max.y } ), TransformPoint2D( xform, { min.x, max.y } ) };

        const glm::vec2 mn = glm::min( glm::min( quad[0], quad[1] ), glm::min( quad[2], quad[3] ) );
        const glm::vec2 mx = glm::max( glm::max( quad[0], quad[1] ), glm::max( quad[2], quad[3] ) );

        if ( region.Bounded )
        {
            const float x0 = std::max( region.Box.x, mn.x );
            const float y0 = std::max( region.Box.y, mn.y );
            const float x1 = std::min( region.Box.x + region.Box.z, mx.x );
            const float y1 = std::min( region.Box.y + region.Box.w, mx.y );
            region.Box     = glm::vec4( x0, y0, std::max( 0.0f, x1 - x0 ), std::max( 0.0f, y1 - y0 ) );
        }
        else
        {
            region.Box     = glm::vec4( mn.x, mn.y, mx.x - mn.x, mx.y - mn.y );
            region.Bounded = true;
        }

        // The new box and the new quad may have made planes from OUTER levels redundant — the common shape
        // being a small straight clipper nested inside a large rotated one. Dropping them keeps the budget
        // for levels that still say something, and removes work from every triangle the clipper cuts. Done
        // BEFORE this level's own planes are added: a quad lies exactly ON its own edge planes, so testing
        // them against it would delete the very constraints this call is here to add.
        uint32_t kept = 0;
        for ( uint32_t i = 0; i < region.PlaneCount; ++i )
            if ( !Detail::BoxInsidePlane( region.Box, region.Planes[i] ) &&
                 !Detail::QuadInsidePlane( quad, region.Planes[i] ) )
                region.Planes[kept++] = region.Planes[i];
        region.PlaneCount = kept;

        // AN AXIS-ALIGNED CLIPPER IS ITS OWN BOX, AND THEN THE SCISSOR IS ALREADY THE EXACT CLIP — no plane,
        // no cut, no de-indexed triangle. The question is asked IN PIXELS rather than about the matrix, and
        // that is the whole of it: how far, along the edge's own length, does the quad depart from its box?
        // A matrix comparison against zero would answer "oblique" for a quarter turn, because cos(pi/2) is
        // -4.37e-8 in float and not 0 — a clipper an author drew square would take the cutting path and
        // differ from the box by a ten-thousandth of a pixel. Below kAxisAlignedTolerancePx no sample point
        // can fall on the wrong side of the edge, so the two clips are the same picture and the cheaper one
        // is the right one. Above it, the quad is cut.
        constexpr float kAxisAlignedTolerancePx = 1.0f / 256.0f;
        const float     w                       = max.x - min.x;
        const float     h                       = max.y - min.y;
        const float     alongX                  = std::fabs( xform[0].y * w ) + std::fabs( xform[1].x * h );
        const float     alongY                  = std::fabs( xform[0].x * w ) + std::fabs( xform[1].y * h );
        const bool      axisAligned = alongX <= kAxisAlignedTolerancePx || alongY <= kAxisAlignedTolerancePx;

        bool exact = true;
        if ( !axisAligned )
        {
            const glm::vec2 centre = ( quad[0] + quad[2] ) * 0.5f;
            for ( int i = 0; i < 4; ++i )
            {
                const glm::vec2 a = quad[i], b = quad[( i + 1 ) & 3];
                glm::vec2       n( a.y - b.y, b.x - a.x ); // perpendicular to the edge
                const float     len = std::sqrt( n.x * n.x + n.y * n.y );
                if ( len <= 0.0f )
                    continue; // a collapsed edge constrains nothing the (equally collapsed) box does not
                n /= len;
                glm::vec3 plane( n.x, n.y, -( n.x * a.x + n.y * a.y ) );
                // Orient by the quad's own centre rather than by an assumed winding: an author may type a
                // negative scale, which mirrors the quad and reverses it.
                if ( ClipPlaneDistance( plane, centre ) < 0.0f )
                    plane = -plane;

                if ( Detail::BoxInsidePlane( region.Box, plane ) )
                    continue;
                if ( region.PlaneCount >= kMaxClipPlanes )
                {
                    exact = false;
                    continue;
                }
                region.Planes[region.PlaneCount++] = plane;
            }
        }

        return exact;
    }
} // namespace Desert::Graphic::Render2D
