// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/CompGeom/ExactPredicates.h (Orient2, InCircle2),
// which wrap Shewchuk's robust predicates (public domain, "Adaptive Precision Floating-Point Arithmetic and Fast
// Robust Geometric Predicates", 1997). Adapted: namespace Desert::Geometry::ExactPredicates, glm::dvec2; the
// floating-point filter is Shewchuk's stage-A bound and the fallback evaluates the determinant exactly with
// expansion arithmetic (TwoSum / TwoProduct, zero-eliminating sums), so the SIGN is always exact. Only the 2D
// predicates the Delaunay port needs are ported.
#pragma once

#include <glm/vec2.hpp>

namespace Desert::Geometry::ExactPredicates
{
    /** @return > 0 if C is left of the directed line A->B (A, B, C counter-clockwise), < 0 if right, 0 if
     * collinear. The sign is exact; the magnitude is the filtered determinant when the filter decides, else +-1.
     */
    double Orient2( const glm::dvec2& A, const glm::dvec2& B, const glm::dvec2& C );

    /** @return > 0 if D lies inside the circle through A, B, C (counter-clockwise), < 0 outside, 0 on it. The sign
     *  is exact. */
    double InCircle2( const glm::dvec2& A, const glm::dvec2& B, const glm::dvec2& C, const glm::dvec2& D );
} // namespace Desert::Geometry::ExactPredicates
