// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/CompGeom/Delaunay2.h (FDelaunay2) and
// Private/CompGeom/Delaunay2.cpp:21-735 (FDelaunay2Connectivity), 757-937 (InTriCircle, WalkToContainingTri,
// Insert), 1276-1615 (GetFirstCrossingEdge, DigCavity, FillCavity, ConstrainEdges, Update, Triangulate),
// 1617-1838. Adapted: namespace Desert::Geometry, TMap as std::unordered_map, TArrayView as std::vector, double
// only. Ported: what ConstrainedDelaunay2 uses - incremental Bowyer-Watson triangulation with ghost triangles,
// edge constraint by cavity dig + Anglada fill, duplicate-vertex tracking, triangles + adjacency output. Not
// ported: Voronoi cells, GetFilledTriangles (ConstrainedDelaunay2 classifies by its own winding walk), IsDelaunay
// and the float overloads. Insertion order: UE sorts by FBRIOPoints (biased randomised insertion along a Z-order
// curve); this port shuffles uniformly (same expected-case correctness, the BRIO only speeds the point walk).
#pragma once

#include "Engine/Geometry/MeshCore/IndexTypes.hpp"

#include <glm/vec2.hpp>

#include <memory>
#include <random>
#include <vector>

namespace Desert::Geometry
{
    struct Delaunay2Connectivity;

    class Delaunay2
    {
    public:
        Delaunay2();
        ~Delaunay2();
        Delaunay2( const Delaunay2& )            = delete;
        Delaunay2& operator=( const Delaunay2& ) = delete;

        /** If true, edges that reference a duplicate vertex are remapped to the vertex it duplicates. */
        bool m_bAutomaticallyFixEdgesToDuplicateVertices = false;
        /** Keep the vertex -> edge cache after ConstrainEdges (faster further edge queries). */
        bool m_bKeepFastEdgeAdjacencyData = false;
        /** Fail ConstrainEdges / Triangulate when a constraint edge is missing from the result. */
        bool m_bValidateEdges = true;

        /** Delaunay-triangulate Vertices, then constrain Edges into it. @return false if the input is empty or all
         *  collinear, or (with m_bValidateEdges) a constraint edge could not be inserted. */
        bool Triangulate( const std::vector<glm::dvec2>& Vertices, const std::vector<Index2i>& Edges = {} );

        /** Insert Edges into the existing triangulation of Vertices. */
        bool ConstrainEdges( const std::vector<glm::dvec2>& Vertices, const std::vector<Index2i>& Edges );

        /** All non-ghost triangles (counter-clockwise) and, per triangle edge [i] = (Tri[i], Tri[i+1 % 3]), the
         *  index of the triangle across it, or -1 on the hull. */
        void GetTrianglesAndAdjacency( std::vector<Index3i>& Triangles, std::vector<Index3i>& Adjacency ) const;

        void FixDuplicatesOnEdge( Index2i& Edge ) const;
        bool HasEdge( const Index2i& Edge, bool bRemapDuplicates ) const;
        bool HasDuplicates() const;

    private:
        bool ValidateEdgesResult( const std::vector<Index2i>& Edges ) const;

        std::unique_ptr<Delaunay2Connectivity> m_Connectivity;
        std::mt19937                           m_Random{ 0u };
    };
} // namespace Desert::Geometry
