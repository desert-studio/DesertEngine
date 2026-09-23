#pragma once

#include "EditMeshAttributes.hpp"
#include "EditMeshTypes.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    // THE EDITABLE MESH: a triangle mesh that knows its own topology.
    //
    // DynamicMesh (Engine/Geometry/DynamicMesh.hpp) is a RENDER buffer: it has vertices and an index list
    // and nothing else, so every modeling tool that needs "the triangle across this edge" has to re-derive
    // it (PolyEditTool floods coplanar triangles on every click for exactly this reason). EditMesh is the
    // structure those tools stand on. It is plain CPU data - no GPU, no ECS. Attribute layers (normals,
    // tangents, colours, UV channels as overlays with seams), polygroups and material IDs live in the
    // EditMeshAttributes it OWNS (EditMeshAttributes.hpp): every edit below updates them in the same call,
    // and refuses with EditResult::AttributeSeam where the attribute outcome would be meaningless.
    // Conversion to and from the render buffer is EditMeshConversion.hpp.
    //
    // THE SHAPE IS UE's FDynamicMesh3, re-implemented rather than ported. It was measured before choosing:
    // the port would have been ~7.4k lines (DynamicMesh3.h/.cpp/_Edits/_Queries + DynamicVector,
    // RefCountVector, SmallListSet, IteratorUtil) written against UE Core (TArray x32, FString x32,
    // TFunction x31, FArchive x22, FVector3d/FIndex3i/FIndex2i x232) and wired to the attribute set
    // (x40), and the licence waiver that would allow it expires with the current commercial project - an
    // expiry is not acceptable under the one structure every modeling tab stands on. What is kept is the
    // PATTERN, because it is the right one for tools rather than for a half-edge textbook:
    //
    //   * triangles over SHARED vertices, with an explicit edge table: an edge is an unordered vertex pair
    //     that owns the one or two triangles on it, and each vertex owns the list of its edges;
    //   * IDs are STABLE: removing an element leaves a hole, nothing is renumbered, and a freed ID may be
    //     reused by a later append. Selections, undo records and tool state hold IDs across edits, so a
    //     removal renumbering the survivors would silently re-point every one of them. Compact() is the
    //     one explicit operation that renumbers, and it returns the maps;
    //   * an edge carries AT MOST TWO triangles, and the two traverse it in opposite directions. This is
    //     enforced at AppendTriangle (refused, never repaired), so every edit operation can rely on it.
    //     Vertices may still be non-manifold (a "bowtie": two triangle fans meeting at one vertex); that is
    //     legal and IsManifold() reports it - FromRenderMesh (EditMeshConversion.hpp) welds them from real data.
    //
    // Operations do not repair or guess: an edit that would break an invariant is REFUSED with a named
    // result and leaves the mesh untouched. CheckValidity() verifies every invariant from scratch.
    //
    // Units: positions are world-space centimetres like everything else in the engine; the topology does
    // not read them.

    struct SplitEdgeInfo
    {
        int                OriginalEdge = InvalidId; // keeps its ID, now spans (kept end, NewVertex)
        int                NewVertex    = InvalidId;
        int                NewEdge      = InvalidId;             // the other half of the split edge
        std::array<int, 2> NewTriangles{ InvalidId, InvalidId }; // [1] is InvalidId on a boundary edge
        std::array<int, 2> NewSpokes{ InvalidId, InvalidId };    // NewVertex -> opposite corner, per side
    };

    struct FlipEdgeInfo
    {
        int                Edge = InvalidId; // keeps its ID, now joins the two formerly opposite vertices
        std::array<int, 2> OldVertices{ InvalidId, InvalidId };
        std::array<int, 2> NewVertices{ InvalidId, InvalidId };
        std::array<int, 2> Triangles{ InvalidId, InvalidId }; // both keep their IDs
    };

    struct CollapseEdgeInfo
    {
        int                KeptVertex    = InvalidId;
        int                RemovedVertex = InvalidId;
        int                CollapsedEdge = InvalidId;                // removed
        std::array<int, 2> RemovedTriangles{ InvalidId, InvalidId }; // [1] InvalidId on a boundary edge
        std::array<int, 2> RemovedEdges{ InvalidId, InvalidId };     // the edge merged away, per side
        std::array<int, 2> KeptEdges{ InvalidId, InvalidId };        // the edge it merged into, per side
    };

    class EditMesh
    {
    public:
        // ── construction ─────────────────────────────────────────────────────────────────────────────
        int AppendVertex( const glm::vec3& position );

        // Winding a -> b -> c. On success `outTriangle` receives the new ID; on refusal it is untouched
        // and so is the mesh.
        [[nodiscard]] EditResult AppendTriangle( int a, int b, int c, int& outTriangle );

        // Removes the triangle and every edge left with no triangle. With removeIsolatedVertices, a corner
        // left with no edge is removed too (a vertex that was isolated BEFORE the call is never touched).
        [[nodiscard]] EditResult RemoveTriangle( int triangle, bool removeIsolatedVertices = true );

        // ── edits ────────────────────────────────────────────────────────────────────────────────────
        // Inserts a vertex on the edge at lerp(v0, v1, t) where (v0, v1) = GetEdgeVertices(edge), and splits
        // each adjacent triangle in two. Every existing ID survives; the original edge keeps the v0 half.
        [[nodiscard]] EditResult SplitEdge( int edge, float t, SplitEdgeInfo& out );

        // Replaces the edge between two triangles with the other diagonal of their quad. Refused on a
        // boundary edge and when the other diagonal already exists. Refused with AttributeSeam when the two
        // triangles differ in polygroup or material, or the edge is a seam of any overlay (including one
        // side set and the other not): the flip would hand part of one side's area to the other.
        [[nodiscard]] EditResult FlipEdge( int edge, FlipEdgeInfo& out );

        // Merges removeVertex into keepVertex (they must share an edge), moving keepVertex to
        // lerp(keep, remove, t). Refused, with the mesh untouched, when the result would not be a valid
        // EditMesh or would pinch a surface:
        //   * the two ends share a neighbour other than the edge's opposite corners (link condition -
        //     the collapse would fold two edges onto one and put 3+ triangles on it);
        //   * the edge is interior but both ends are on the boundary (the collapse would pinch the surface
        //     into a bowtie);
        //   * a triangle on the edge has both other edges on the boundary (an "ear": its two edges would
        //     merge into an edge with no triangle);
        //   * the two opposite corners are the same vertex, or a renamed triangle would duplicate one that
        //     exists (the tetrahedron case);
        //   * (AttributeSeam) the edge is a seam END in some overlay - split at one end only. Collapsing it
        //     either merges two sides of a seam that continues elsewhere or needs an arbitrary split; a
        //     seam split at BOTH ends collapses fine, each side blending its own pair of elements.
        // Polygroup and material boundaries are not refused: they move with the vertex, like the geometry.
        [[nodiscard]] EditResult CollapseEdge( int keepVertex, int removeVertex, float t, CollapseEdgeInfo& out );

        // Renumbers every element (overlay elements included) densely in ascending old-ID order and drops
        // the holes. The ONLY operation that changes surviving IDs.
        CompactMaps Compact();

        // ── queries ──────────────────────────────────────────────────────────────────────────────────
        [[nodiscard]] bool IsVertex( int v ) const
        {
            return m_VertexPool.Contains( v );
        }
        [[nodiscard]] bool IsTriangle( int t ) const
        {
            return m_TrianglePool.Contains( t );
        }
        [[nodiscard]] bool IsEdge( int e ) const
        {
            return m_EdgePool.Contains( e );
        }

        [[nodiscard]] int VertexCount() const
        {
            return m_VertexPool.Live;
        }
        [[nodiscard]] int TriangleCount() const
        {
            return m_TrianglePool.Live;
        }
        [[nodiscard]] int EdgeCount() const
        {
            return m_EdgePool.Live;
        }
        // One past the highest ID ever handed out: the size an array indexed by ID must have.
        [[nodiscard]] int MaxVertexId() const
        {
            return m_VertexPool.Size();
        }
        [[nodiscard]] int MaxTriangleId() const
        {
            return m_TrianglePool.Size();
        }
        [[nodiscard]] int MaxEdgeId() const
        {
            return m_EdgePool.Size();
        }

        [[nodiscard]] IdRange VertexIds() const
        {
            return m_VertexPool.Ids();
        }
        [[nodiscard]] IdRange TriangleIds() const
        {
            return m_TrianglePool.Ids();
        }
        [[nodiscard]] IdRange EdgeIds() const
        {
            return m_EdgePool.Ids();
        }

        // The accessors below take a LIVE ID; passing anything else is a caller defect, checked by assert.
        [[nodiscard]] const glm::vec3& GetPosition( int v ) const;
        void                           SetPosition( int v, const glm::vec3& position );

        [[nodiscard]] const std::array<int, 3>& GetTriangle( int t ) const;
        // Edge j joins corner j and corner (j + 1) % 3.
        [[nodiscard]] const std::array<int, 3>& GetTriangleEdges( int t ) const;
        // Ascending: [0] < [1].
        [[nodiscard]] const std::array<int, 2>& GetEdgeVertices( int e ) const;
        // [1] is InvalidId on a boundary edge.
        [[nodiscard]] const std::array<int, 2>& GetEdgeTriangles( int e ) const;
        [[nodiscard]] std::span<const int>      GetVertexEdges( int v ) const;

        [[nodiscard]] std::vector<int> GetVertexTriangles( int v ) const;
        [[nodiscard]] std::vector<int> GetVertexNeighbours( int v ) const;

        [[nodiscard]] int FindEdge( int a, int b ) const;
        // Any winding of the three vertices.
        [[nodiscard]] int FindTriangle( int a, int b, int c ) const;

        [[nodiscard]] bool IsBoundaryEdge( int e ) const;
        [[nodiscard]] bool IsBoundaryVertex( int v ) const;
        // More than one fan of triangles meets at v (an isolated vertex is not a bowtie).
        [[nodiscard]] bool IsBowtieVertex( int v ) const;
        // No bowtie vertex. (Edges are manifold by construction.)
        [[nodiscard]] bool IsManifold() const;

        // ── attributes ───────────────────────────────────────────────────────────────────────────────
        [[nodiscard]] EditMeshAttributes& Attributes()
        {
            return m_Attributes;
        }
        [[nodiscard]] const EditMeshAttributes& Attributes() const
        {
            return m_Attributes;
        }

        // Verifies every structural invariant from scratch, attribute layers included, and names the first
        // one broken, with IDs.
        [[nodiscard]] Common::BoolResultStr CheckValidity() const;

    private:
        int AllocateTriangle();

        int               AddEdge( int a, int b );
        void              RemoveEdge( int e );
        void              AddEdgeTriangle( int e, int t );
        void              RemoveEdgeTriangle( int e, int t );
        void              ReplaceEdgeTriangle( int e, int oldT, int newT );
        void              ReplaceEdgeVertex( int e, int oldV, int newV );
        void              EraseVertexEdge( int v, int e );
        [[nodiscard]] int OtherTriangle( int e, int t ) const;
        // Index j of t's edge e: corners j and j+1 are the edge's ends in t's winding.
        [[nodiscard]] int EdgeSlot( int t, int e ) const;

        std::vector<glm::vec3>          m_Positions;
        std::vector<std::vector<int>>   m_VertexEdges;
        std::vector<std::array<int, 3>> m_TriangleVertices;
        std::vector<std::array<int, 3>> m_TriangleEdges;
        std::vector<std::array<int, 2>> m_EdgeVertices;
        std::vector<std::array<int, 2>> m_EdgeTriangles;

        IdPool m_VertexPool;
        IdPool m_TrianglePool;
        IdPool m_EdgePool;

        EditMeshAttributes m_Attributes;
    };
} // namespace Desert::Geometry
