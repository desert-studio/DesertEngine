// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/GeometryAlgorithms/Public/ConstrainedDelaunay2.h
// (TConstrainedDelaunay2: Add(polygon / general polygon), fill rules, Triangulate()) and
// Private/ConstrainedDelaunay2.cpp:69-115 (edge maps), 241-280 (BuildFinalTriangles), 346-468 (Triangulate,
// ConstrainedDelaunayTriangulate). Adapted: namespace Desert::Geometry, double only. Not ported: bowtie splitting
// (UE's bSplitBowties defaults off and no caller here sets it), the FDynamicGraph2 input, the
// KeepTriangle-callback Triangulate overload and AddWithIntersectionResolution (needs UE's polygon boolean).
#pragma once

#include "Engine/Geometry/MeshCore/Curve/PlanarComplex.hpp"
#include "Engine/Geometry/MeshCore/IndexTypes.hpp"

#include <vector>

namespace Desert::Geometry
{
    class ConstrainedDelaunay2
    {
    public:
        // Inputs
        std::vector<glm::dvec2> m_Vertices;
        /** Boundary edges; whether a side is inside is decided by m_FillRule over the winding they make. */
        std::vector<Index2i> m_Edges;
        /** Hole edges: the classification walk never crosses them. */
        std::vector<Index2i> m_HoleEdges;
        bool                 m_bOrientedEdges = true;
        /** Output counter-clockwise triangles; UE's default false outputs clockwise ones (a hole fill seen from
         * the side the plane normal points to). */
        bool m_bOutputCCW = false;

        enum class EFillRule
        {
            Odd = 0,
            // m_bOrientedEdges must be true for the below
            NonZero,
            Positive,
            Negative
        };
        EFillRule m_FillRule = EFillRule::Odd;

        void Add( const Polygon2& Polygon, bool bIsHole = false );
        void Add( const GeneralPolygon2& GPolygon );

        /** @return false if the input does not triangulate or a boundary edge could not be inserted (the result
         * then still holds the triangles the walk could classify). */
        bool Triangulate();

        // Output: indices into m_Vertices
        std::vector<Index3i> m_Triangles;

    private:
        bool ClassifyFromRule( int Winding ) const;
    };

    /** Triangulate the inside of a general polygon (Positive fill: the outer counter-clockwise, holes clockwise).
     *  Vertex indices run over the outer loop, then each hole in order. */
    std::vector<Index3i> ConstrainedDelaunayTriangulate( const GeneralPolygon2& GeneralPolygon );
} // namespace Desert::Geometry
