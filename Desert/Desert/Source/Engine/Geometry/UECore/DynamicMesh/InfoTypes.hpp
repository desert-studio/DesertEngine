// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/InfoTypes.h:1-145, adapted: UE Core via
// UECore.hpp, namespace Desert::Geometry.

#pragma once

#include "Engine/Geometry/UECore/IndexTypes.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{

    /**
     * VertexInfo stores information about vertex attributes - position, normal, color, UV
     */
    struct VertexInfo
    {
        glm::dvec3 Position{ glm::dvec3( 0 ) };
        glm::vec3  Normal{ glm::vec3( 0 ) };
        glm::vec3  Color{ glm::vec3( 0 ) };
        glm::vec2  UV{ glm::vec2( 0 ) };
        bool      bHaveN{}, bHaveC{}, bHaveUV{};

        VertexInfo() = default;
        VertexInfo( const glm::dvec3& PositionIn ) : Position{ PositionIn }
        {
        }
        VertexInfo( const glm::dvec3& PositionIn, const glm::vec3& NormalIn )
             : Position{ PositionIn }, Normal{ NormalIn }, bHaveN{ true }
        {
        }
        VertexInfo( const glm::dvec3& PositionIn, const glm::vec3& NormalIn, const glm::vec3& ColorIn )
             : Position{ PositionIn }, Normal{ NormalIn }, Color{ ColorIn }, bHaveN{ true }, bHaveC{ true }
        {
        }
        VertexInfo( const glm::dvec3& PositionIn, const glm::vec3& NormalIn, const glm::vec3& ColorIn,
                    const glm::vec2& UVIn )
             : Position{ PositionIn }, Normal{ NormalIn }, Color{ ColorIn }, UV{ UVIn }, bHaveN{ true },
               bHaveC{ true }, bHaveUV{ true }
        {
        }
    };

    /**
     * MeshTriEdgeID identifies an edge in a triangle mesh based on
     * the triangle ID/Index and the "edge index" 0/1/2 in the triangle.
     * If the ordered triangle vertices are [A,B,C], then [A,B]=0, [B,C]=1, and [C,A]=2.
     *
     * This type of edge identifier is applicable on any indexed mesh, even if
     * the mesh does not store explicit edge IDs.
     *
     * Values are stored unsigned, so it is currently *not* possible to store an "invalid"
     * edge identifier as a MeshTriEdgeID (0xFFFFFFFF could potentially be used as such an identifier).
     *
     * The TriangleID is stored in 30 bits, while DynamicMesh3 stores (valid) triangle IDs in 31 bits.
     * So, only ~1 billion triangles are allowed in a mesh when using MeshTriEdgeID, vs ~2 billion in
     * DynamicMesh3. This limit has not been encountered in practice, to date.
     *
     * Note that cycling or permuting the vertices of a triangle will change these indices.
     */
    struct MeshTriEdgeID
    {
        /** The 0/1/2 index of the edge in the triangle's tuple of edges */
        unsigned TriEdgeIndex : 2;
        /** The index of the mesh Triangle */
        unsigned TriangleID : 30;

        MeshTriEdgeID()
        {
            TriEdgeIndex = 0;
            TriangleID   = 0;
        }

        /**
         * Construct a MeshTriEdgeID for the given TriangleID and Edge Index in range
         * @param EdgeIndexIn index in range 0,1,2
         */
        MeshTriEdgeID( int32_t TriangleIDIn, int32_t EdgeIndexIn )
        {
            assert( EdgeIndexIn >= 0 && EdgeIndexIn <= 2 );
            assert( TriangleIDIn >= 0 && TriangleIDIn < ( 1 << 30 ) );
            TriEdgeIndex = (unsigned int)EdgeIndexIn;
            TriangleID   = (unsigned int)TriangleIDIn;
        }

        /**
         * Decode an encoded MeshTriEdgeID from a packed uint32_t created by the Encoded() function
         */
        explicit MeshTriEdgeID( uint32_t EncodedEdgeKey )
        {
            TriangleID   = EncodedEdgeKey & 0x8FFFFFFF;
            TriEdgeIndex = ( EncodedEdgeKey & 0xC0000000 ) >> 30;
        }

        /**
         * @return the (TriangleID, TriEdgeIndex) values packed into a 32 bit integer
         */
        [[nodiscard]] uint32_t Encoded() const
        {
            return ( TriEdgeIndex << 30 ) | TriangleID;
        }
    };

    /**
     * MeshTriOrderedEdgeID identifies an oriented edge in a triangle mesh based on indices
     * into the triangle vertices. IE if a triangle has vertices [A,B,C], then an
     * oriented edge could be (A,B) or (B,A), or any of the other 4 permutations.
     * So the ordered edge in the triangle can be represented as two vertex indices,
     * and the full encoding is (TriangleID, J, K) where J and K are in range 0/1/2.
     *
     * This type of edge identifier is applicable on any indexed mesh, even if
     * the mesh does not store explicit edge IDs. In addition, this identifier is stable
     * across mesh topological changes, ie if two connected triangles are unlinked (ie
     * the shared edge becomes two edges, and the 2 vertices become 4), the MeshTriOrderedEdgeID
     * will still refer to the correct oriented edge, as it does not explicitly depend on
     * the Vertex or Edge IDs, only the ordering within the triangle.
     *
     * Note that cycling or permuting the vertices of a triangle will change/break these indices.
     */
    struct MeshTriOrderedEdgeID
    {
        /** The index of the mesh Triangle */
        int32_t TriangleID;
        /** The 0/1/2 index of the first vertex in the triangles tuple of vertices */
        unsigned VertIndexA : 2;
        /** The 0/1/2 index of the second vertex in the triangles tuple of vertices */
        unsigned VertIndexB : 2;

        MeshTriOrderedEdgeID()
        {
            TriangleID = IndexConstants::InvalidID;
            VertIndexA = 0;
            VertIndexB = 0;
        }

        MeshTriOrderedEdgeID( int32_t TriangleIDIn, int32_t VertexIndexA, int32_t VertexIndexB )
        {
            assert( VertexIndexA >= 0 && VertexIndexA <= 2 );
            assert( VertexIndexB >= 0 && VertexIndexB <= 2 );
            TriangleID = TriangleIDIn;
            VertIndexA = VertexIndexA;
            VertIndexB = VertexIndexB;
        }
    };

} // namespace Desert::Geometry

// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/InfoTypes.h:146-248, adapted: nested in
// namespace Desert::Geometry; the bowtie edge lists are a plain TArray (the shim has no TInlineAllocator).
namespace Desert::Geometry
{

    namespace DynamicMeshInfo
    {

        /** Information about the mesh elements created by a call to SplitEdge() */
        struct EdgeSplitInfo
        {
            int      OriginalEdge;      // the edge that was split
            Index2i  OriginalVertices;  // original edge vertices [a,b]
            Index2i  OtherVertices;     // original opposing vertices [c,d] - d is InvalidID for boundary edges
            Index2i  OriginalTriangles; // original edge triangles [t0,t1]
            bool     bIsBoundary;       // was the split edge a boundary edge?  (redundant)

            int      NewVertex;    // new vertex f that was created
            Index2i  NewTriangles; // new triangles [t2,t3], oriented as explained in SplitEdge() header comment
            Index3i  NewEdges;     // new edges are [f,b], [f,c] and [f,d] if this is not a boundary edge

            double SplitT; // parameter value for NewVertex along original edge
        };

        /** Information about the mesh elements modified by a call to FlipEdge() */
        struct EdgeFlipInfo
        {
            int      EdgeID;        // the edge that was flipped
            Index2i  OriginalVerts; // original verts of the flipped edge, that are no longer connected
            Index2i  OpposingVerts; // the opposing verts of the flipped edge, that are now connected
            Index2i  Triangles;     // the two triangle IDs. Original tris vert [Vert0,Vert1,OtherVert0] and
                               // [Vert1,Vert0,OtherVert1]. New triangles are [OtherVert0, OtherVert1, Vert1] and
                               // [OtherVert1, OtherVert0, Vert0]
        };

        /** Information about mesh elements modified/removed by CollapseEdge() */
        struct EdgeCollapseInfo
        {
            int      KeptVertex;    // the vertex that was kept (ie collapsed "to")
            int      RemovedVertex; // the vertex that was removed
            Index2i  OpposingVerts; // the opposing vertices [c,d]. If the edge was a boundary edge, d is InvalidID
            bool     bIsBoundary;   // was the edge a boundary edge

            int      CollapsedEdge; // the edge that was collapsed/removed
            Index2i  RemovedTris;   // the triangles that were removed in the collapse (second is InvalidID for
                                    // boundary edge)
            Index2i RemovedEdges;   // the edges that were removed (second is InvalidID for boundary edge)
            Index2i KeptEdges;      // the edges that were kept (second is InvalidID for boundary edge)

            double CollapseT; // interpolation parameter along edge for new vertex in range [0,1] where 0 =>
                              // KeptVertex and 1 => RemovedVertex
        };

        /** Information about mesh elements modified by MergeEdges() */
        struct MergeEdgesInfo
        {
            int KeptEdge;    // the edge that was kept
            int RemovedEdge; // the edge that was removed

            Index2i KeptVerts;    // The two vertices that were kept (redundant w/ KeptEdge?)
            Index2i RemovedVerts; // The removed vertices of RemovedEdge. Either may be InvalidID if it was same
                                  // as the paired KeptVert

            Index2i ExtraRemovedEdges; // extra removed edges, see description below. Either may be or InvalidID
            Index2i ExtraKeptEdges;    // extra kept edges, paired with ExtraRemovedEdges

            // Even more Removed and Kept edges, in cases where there were multiple such edges on one or both sides
            // of the merged edge Only possible if the pre-merge mesh had non-manifold vertices (aka bowties), in
            // almost all meshes these arrays will be empty
            std::vector<int> BowtiesRemovedEdges, BowtiesKeptEdges;

            double InterpolationT = 0; // Interpolation parameter for each kept vertex in range [0,1] where 0 =>
                                       // KeptVertex and 1 => RemovedVertex
        };

        /** Information about mesh elements modified by MergeVertices() */
        struct MergeVerticesInfo
        {
            int    KeptVertex;         // the vertex that was kept
            int    RemovedVertex;      // the vertex that was removed
            double InterpolationT = 0; // Interpolation parameter for the kept vertex in range [0,1] where 0 =>
                                       // KeptVertex and 1 => RemovedVertex

            // If the merge resolves as an edge collapse, the information is stored here
            std::optional<EdgeCollapseInfo> EdgeCollapseInfo;
            // If the merge resolves as an edge weld, the information is stored here
            std::optional<MergeEdgesInfo> MergeEdgesInfo;
        };

        /** Information about mesh elements modified/created by PokeTriangle() */
        struct PokeTriangleInfo
        {
            int      OriginalTriangle; // the triangle that was poked
            Index3i  TriVertices;      // vertices of the original triangle

            int      NewVertex;    // the new vertex that was inserted
            Index2i  NewTriangles; // the two new triangles that were added (OriginalTriangle is re-used, see code
                                   // for vertex orders)
            Index3i NewEdges;      // the three new edges connected to NewVertex

            glm::dvec3 BaryCoords{}; // barycentric coords that NewVertex was inserted at
        };

        /** Information about mesh elements modified/created by SplitVertex() */
        struct VertexSplitInfo
        {
            int OriginalVertex;
            int NewVertex;
            // if needed could possibly add information about added edges?  but it would be a dynamic array, and
            // there is no use for it yet. modified triangles are passed as input to the function, no need to store
            // those here.
        };
    } // namespace DynamicMeshInfo
} // namespace Desert::Geometry
