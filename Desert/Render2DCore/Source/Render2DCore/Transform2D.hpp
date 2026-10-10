#pragma once

#include <glm/glm.hpp>

#include <cmath>

// The 2D affine used by the whole UI: one type, applied FORWARD to the geometry the draw list emits and
// INVERSELY to the pointer that hit-tests it.
//
// WHY IT IS A CPU TYPE AND NOT A PER-BATCH GPU MATRIX. DrawList2D emits absolute pixel positions, so a
// transform costs four multiply-adds per quad at emission and changes NOTHING about batch state: the
// commands still break only on texture / text / clip, which is what keeps a rotated element inside the
// batch it was already in. Putting the matrix in the batch key instead would have made every transform
// change a new draw call on a batcher that is already texture-bound, and it would have left the pointer
// needing a second, CPU-side copy of the same matrix anyway — two things that must agree, which is this
// project's most expensive defect shape. One matrix, two directions.
//
// The identity case is exact rather than approximately exact: 1*x + 0*y + 0 is x in IEEE-754 for every
// finite x, so a transformed identity vertex is bit-identical to an untransformed one. DrawList2D still
// skips the multiply entirely when nothing is pushed, so the guarantee does not rest on that argument.
namespace Desert::Graphic::Render2D
{
    // Column-major glm::mat3 holding an affine map: columns 0/1 are the linear part, column 2 the
    // translation, and the bottom row is (0,0,1). Points go through it as mat * vec3(p, 1).

    // Rotate by @p rotationDegrees and scale by @p scale ABOUT @p pivotPx, in pixels.
    //
    // POSITIVE ROTATION IS CLOCKWISE ON SCREEN, because this space has y pointing DOWN — the same
    // convention CSS `rotate()` and Godot's Control.rotation use, and the one an author gets when they
    // drag a rotation handle to the right. The matrix below is the textbook counter-clockwise one; it is
    // the y-down basis, not the matrix, that makes it read clockwise.
    inline glm::mat3 MakeTransform2D( const glm::vec2& pivotPx, float rotationDegrees, const glm::vec2& scale )
    {
        const float rad = rotationDegrees * 0.01745329251994329577f; // pi/180
        const float c   = std::cos( rad );
        const float s   = std::sin( rad );

        // T(pivot) * R * S * T(-pivot), multiplied out so the translation column is exact for the
        // identity case (pivot - pivot cancels only after the linear part is applied to it).
        const float a = c * scale.x;
        const float b = s * scale.x; // column 0 = R*S applied to (1,0)
        const float d = -s * scale.y;
        const float e = c * scale.y; // column 1 = R*S applied to (0,1)

        glm::mat3 m( 1.0f );
        m[0] = glm::vec3( a, b, 0.0f );
        m[1] = glm::vec3( d, e, 0.0f );
        m[2] = glm::vec3( pivotPx.x - ( a * pivotPx.x + d * pivotPx.y ),
                          pivotPx.y - ( b * pivotPx.x + e * pivotPx.y ), 1.0f );
        return m;
    }

    inline bool IsIdentity2D( const glm::mat3& m )
    {
        return m[0].x == 1.0f && m[0].y == 0.0f && m[1].x == 0.0f && m[1].y == 1.0f && m[2].x == 0.0f &&
               m[2].y == 0.0f;
    }

    inline glm::vec2 TransformPoint2D( const glm::mat3& m, const glm::vec2& p )
    {
        return glm::vec2( m[0].x * p.x + m[1].x * p.y + m[2].x, m[0].y * p.x + m[1].y * p.y + m[2].y );
    }

    // The affine inverse, computed from the 2x2 linear part rather than through the general 3x3 cofactor
    // formula: a singular linear part (scale 0 on an axis, which an author can type into the Details
    // panel) has no inverse at all, and glm::inverse hands back infinities that then travel into a
    // pointer position and out through every comparison made with it. A degenerate transform has
    // collapsed its element onto a line, so no point is inside it and the only thing the answer has to
    // be is FINITE and deterministic — this returns the translation column, which is a point.
    inline glm::mat3 InverseTransform2D( const glm::mat3& m )
    {
        const float a = m[0].x, b = m[0].y, c = m[1].x, d = m[1].y;
        const float det = a * d - b * c;
        if ( det == 0.0f )
        {
            glm::mat3 degenerate( 0.0f );
            degenerate[2] = glm::vec3( m[2].x, m[2].y, 1.0f );
            return degenerate;
        }
        const float inv = 1.0f / det;
        glm::mat3   r( 1.0f );
        r[0] = glm::vec3( d * inv, -b * inv, 0.0f );
        r[1] = glm::vec3( -c * inv, a * inv, 0.0f );
        r[2] = glm::vec3( -( r[0].x * m[2].x + r[1].x * m[2].y ), -( r[0].y * m[2].x + r[1].y * m[2].y ), 1.0f );
        return r;
    }

    // Axis-aligned bounds of the transformed rectangle @p min..@p max. THE ONE IMPLEMENTATION: the scissor
    // (which hardware can only express axis-aligned) and the walk's hit-test clip both go through here, so
    // a rotated clipper cannot clip the picture to one box and the pointer to another.
    inline void TransformedAABB2D( const glm::mat3& m, const glm::vec2& min, const glm::vec2& max,
                                   glm::vec2& outMin, glm::vec2& outMax )
    {
        const glm::vec2 c0 = TransformPoint2D( m, { min.x, min.y } );
        const glm::vec2 c1 = TransformPoint2D( m, { max.x, min.y } );
        const glm::vec2 c2 = TransformPoint2D( m, { max.x, max.y } );
        const glm::vec2 c3 = TransformPoint2D( m, { min.x, max.y } );
        outMin             = glm::min( glm::min( c0, c1 ), glm::min( c2, c3 ) );
        outMax             = glm::max( glm::max( c0, c1 ), glm::max( c2, c3 ) );
    }

    // How many pixels of screen one pixel of this transform's local space covers, as one number: the
    // square root of the linear part's determinant magnitude (the area scale). The glass shader needs it
    // to keep its antialiasing feather one SCREEN pixel wide under a scaled element, and it is exactly 1
    // for an untransformed one, which is what keeps that path bit-identical.
    inline float MeanScale2D( const glm::mat3& m )
    {
        const float det = std::fabs( m[0].x * m[1].y - m[0].y * m[1].x );
        return det > 0.0f ? std::sqrt( det ) : 0.0f;
    }
} // namespace Desert::Graphic::Render2D
