#pragma once

#include <Common/Core/ResultStr.hpp>

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
    // structure those tools stand on. It is plain CPU data - no GPU, no ECS, no attributes: attribute
    // layers, polygroups and material IDs are a separate layer on top (M3), and conversion to the render
    // buffer lives there too.
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
    //     legal and IsManifold() reports it - the importers of M3 need to represent it.
    //
    // Operations do not repair or guess: an edit that would break an invariant is REFUSED with a named
    // result and leaves the mesh untouched. CheckValidity() verifies every invariant from scratch.
    //
    // Units: positions are world-space centimetres like everything else in the engine; the topology does
    // not read them.

    inline constexpr int InvalidId = -1;

    enum class EditResult : uint8_t
    {
        Ok,
        InvalidVertex,           // an argument names a vertex that does not exist
        InvalidTriangle,         // ... a triangle that does not exist
        InvalidEdge,             // ... an edge that does not exist (or the two vertices share none)
        DegenerateTriangle,      // two corners of a new triangle are the same vertex
        DuplicateTriangle,       // a triangle over the same three vertices already exists
        NonManifoldEdge,         // the edit would put a third triangle on an edge
        InconsistentOrientation, // the edit would put two triangles on an edge in the SAME direction
        BoundaryEdge,            // FlipEdge on an edge with one triangle: there is nothing to flip towards
        FlipCreatesExistingEdge, // FlipEdge: the other diagonal already exists, the flip would duplicate it
        CollapseBreaksTopology,  // CollapseEdge: link condition / pinch / ear / tetrahedron - see CollapseEdge
    };

    [[nodiscard]] const char* ToString( EditResult result );

    struct SplitEdgeInfo
    {
        int                OriginalEdge = InvalidId; // keeps its ID, now spans (kept end, NewVertex)
        int                NewVertex    = InvalidId;
        int                NewEdge      = InvalidId; // the other half of the split edge
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
        int                KeptVertex       = InvalidId;
        int                RemovedVertex    = InvalidId;
        int                CollapsedEdge    = InvalidId;                  // removed
        std::array<int, 2> RemovedTriangles{ InvalidId, InvalidId };      // [1] InvalidId on a boundary edge
        std::array<int, 2> RemovedEdges{ InvalidId, InvalidId };          // the edge merged away, per side
        std::array<int, 2> KeptEdges{ InvalidId, InvalidId };             // the edge it merged into, per side
    };

    // Old ID -> new ID for every element kind; a removed/hole ID maps to InvalidId.
    struct CompactMaps
    {
        std::vector<int> Vertices;
        std::vector<int> Triangles;
        std::vector<int> Edges;
    };

    class EditMesh
    {
    public:
        // Iterates the LIVE IDs of one element kind, skipping holes. Invalidated by any edit.
        class IdRange
        {
        public:
            class Iterator
            {
            public:
                Iterator( std::span<const uint8_t> alive, int id ) : m_Alive( alive ), m_Id( id )
                {
                    SkipDead();
                }
                int operator*() const
                {
                    return m_Id;
                }
                Iterator& operator++()
                {
                    ++m_Id;
                    SkipDead();
                    return *this;
                }
                bool operator==( const Iterator& other ) const
                {
                    return m_Id == other.m_Id;
                }
                bool operator!=( const Iterator& other ) const
                {
                    return m_Id != other.m_Id;
                }

            private:
                void SkipDead()
                {
                    while ( m_Id < static_cast<int>( m_Alive.size() ) && m_Alive[m_Id] == 0 )
                        ++m_Id;
                }
                std::span<const uint8_t> m_Alive;
                int                      m_Id;
            };

            explicit IdRange( std::span<const uint8_t> alive ) : m_Alive( alive )
            {
            }
            [[nodiscard]] Iterator begin() const
            {
                return Iterator( m_Alive, 0 );
            }
            [[nodiscard]] Iterator end() const
            {
                return Iterator( m_Alive, static_cast<int>( m_Alive.size() ) );
            }

        private:
            std::span<const uint8_t> m_Alive;
        };

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
        // boundary edge and when the other diagonal already exists.
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
        //     exists (the tetrahedron case).
        [[nodiscard]] EditResult CollapseEdge( int keepVertex, int removeVertex, float t, CollapseEdgeInfo& out );

        // Renumbers every element densely in ascending old-ID order and drops the holes. The ONLY
        // operation that changes surviving IDs.
        CompactMaps Compact();

        // ── queries ──────────────────────────────────────────────────────────────────────────────────
        [[nodiscard]] bool IsVertex( int v ) const
        {
            return v >= 0 && v < static_cast<int>( m_VertexAlive.size() ) && m_VertexAlive[v] != 0;
        }
        [[nodiscard]] bool IsTriangle( int t ) const
        {
            return t >= 0 && t < static_cast<int>( m_TriangleAlive.size() ) && m_TriangleAlive[t] != 0;
        }
        [[nodiscard]] bool IsEdge( int e ) const
        {
            return e >= 0 && e < static_cast<int>( m_EdgeAlive.size() ) && m_EdgeAlive[e] != 0;
        }

        [[nodiscard]] int VertexCount() const
        {
            return m_VertexLive;
        }
        [[nodiscard]] int TriangleCount() const
        {
            return m_TriangleLive;
        }
        [[nodiscard]] int EdgeCount() const
        {
            return m_EdgeLive;
        }
        // One past the highest ID ever handed out: the size an array indexed by ID must have.
        [[nodiscard]] int MaxVertexId() const
        {
            return static_cast<int>( m_VertexAlive.size() );
        }
        [[nodiscard]] int MaxTriangleId() const
        {
            return static_cast<int>( m_TriangleAlive.size() );
        }
        [[nodiscard]] int MaxEdgeId() const
        {
            return static_cast<int>( m_EdgeAlive.size() );
        }

        [[nodiscard]] IdRange VertexIds() const
        {
            return IdRange( m_VertexAlive );
        }
        [[nodiscard]] IdRange TriangleIds() const
        {
            return IdRange( m_TriangleAlive );
        }
        [[nodiscard]] IdRange EdgeIds() const
        {
            return IdRange( m_EdgeAlive );
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

        // Verifies every structural invariant from scratch and names the first one broken, with IDs.
        [[nodiscard]] Common::BoolResultStr CheckValidity() const;

    private:
        static int AllocateId( std::vector<uint8_t>& alive, std::vector<int>& freeList, int& liveCount );
        static void FreeId( std::vector<uint8_t>& alive, std::vector<int>& freeList, int& liveCount, int id );

        int  AddEdge( int a, int b );
        void RemoveEdge( int e );
        void AddEdgeTriangle( int e, int t );
        void RemoveEdgeTriangle( int e, int t );
        void ReplaceEdgeTriangle( int e, int oldT, int newT );
        void ReplaceEdgeVertex( int e, int oldV, int newV );
        void EraseVertexEdge( int v, int e );
        [[nodiscard]] int OtherTriangle( int e, int t ) const;
        // Index j of t's edge e: corners j and j+1 are the edge's ends in t's winding.
        [[nodiscard]] int EdgeSlot( int t, int e ) const;

        std::vector<glm::vec3>          m_Positions;
        std::vector<std::vector<int>>   m_VertexEdges;
        std::vector<std::array<int, 3>> m_TriangleVertices;
        std::vector<std::array<int, 3>> m_TriangleEdges;
        std::vector<std::array<int, 2>> m_EdgeVertices;
        std::vector<std::array<int, 2>> m_EdgeTriangles;

        std::vector<uint8_t> m_VertexAlive;
        std::vector<uint8_t> m_TriangleAlive;
        std::vector<uint8_t> m_EdgeAlive;
        std::vector<int>     m_VertexFree;
        std::vector<int>     m_TriangleFree;
        std::vector<int>     m_EdgeFree;
        int                  m_VertexLive   = 0;
        int                  m_TriangleLive = 0;
        int                  m_EdgeLive     = 0;
    };
} // namespace Desert::Geometry
