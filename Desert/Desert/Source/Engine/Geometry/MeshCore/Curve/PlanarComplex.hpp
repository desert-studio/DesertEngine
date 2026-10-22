// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Curve/PlanarComplex.h:20-170 (TPlanarComplex:
// FindSolidRegions, ConvertNestToGeneralPolygon), with the TPolygon2 / TGeneralPolygon2 members it calls
// (Polygon2.h SignedArea, Bounds, Contains(point) by winding number, Contains(polygon), Overlaps). Adapted:
// namespace Desert::Geometry, double only, a polygon as std::vector<glm::dvec2>, GeneralPolygon2 as a plain outer
// + holes.
#pragma once

#include <glm/vec2.hpp>

#include <vector>

namespace Desert::Geometry
{
    using Polygon2 = std::vector<glm::dvec2>;

    /** An outer boundary with holes (UE TGeneralPolygon2). Vertex order across the loops: outer, then each hole.
     */
    struct GeneralPolygon2
    {
        Polygon2              Outer;
        std::vector<Polygon2> Holes;
    };

    namespace PolygonUtil
    {
        /** Positive for a counter-clockwise polygon. */
        double SignedArea( const Polygon2& Polygon );
        /** Winding-number containment of a point (UE TPolygon2::Contains). */
        bool Contains( const Polygon2& Polygon, const glm::dvec2& Point );
        /** Every vertex of Inner inside Outer and no edge of one crossing an edge of the other. */
        bool Contains( const Polygon2& Outer, const Polygon2& Inner );
        /** Any edge crossing, or one polygon containing a vertex of the other. */
        bool Overlaps( const Polygon2& A, const Polygon2& B );
    } // namespace PolygonUtil

    struct PlanarComplex
    {
        // Inputs
        std::vector<Polygon2> m_Polygons;
        /** Counter-clockwise loops are outers and clockwise loops holes; holes are only searched for among
         *  clockwise loops. */
        bool m_bTrustOrientations     = true;
        bool m_bAllowOverlappingHoles = false;

        // Outputs
        struct PolygonNesting
        {
            int              OuterIndex = -1;
            std::vector<int> HoleIndices;
        };
        std::vector<PolygonNesting> m_Solids;

        /** Nest the polygons into solids: each outer loop with the holes directly inside it. */
        void FindSolidRegions();

        GeneralPolygon2 ConvertNestToGeneralPolygon( const PolygonNesting& Nest ) const;
    };
} // namespace Desert::Geometry
