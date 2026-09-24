// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/InfoTypes.h:1-145, adapted: UE Core via
// UECore.hpp, namespace Desert::Geometry; the DynamicMeshInfo edit-result structs (146-248) belong to the edit
// operators (task P3).

#pragma once

#include "Engine/Geometry/UECore/IndexTypes.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{

    /**
     * FVertexInfo stores information about vertex attributes - position, normal, color, UV
     */
    struct FVertexInfo
    {
        FVector3d Position{ FVector3d::Zero() };
        FVector3f Normal{ FVector3f::Zero() };
        FVector3f Color{ FVector3f::Zero() };
        FVector2f UV{ FVector2f::Zero() };
        bool      bHaveN{}, bHaveC{}, bHaveUV{};

        FVertexInfo() = default;
        FVertexInfo( const FVector3d& PositionIn ) : Position{ PositionIn }
        {
        }
        FVertexInfo( const FVector3d& PositionIn, const FVector3f& NormalIn )
             : Position{ PositionIn }, Normal{ NormalIn }, bHaveN{ true }
        {
        }
        FVertexInfo( const FVector3d& PositionIn, const FVector3f& NormalIn, const FVector3f& ColorIn )
             : Position{ PositionIn }, Normal{ NormalIn }, Color{ ColorIn }, bHaveN{ true }, bHaveC{ true }
        {
        }
        FVertexInfo( const FVector3d& PositionIn, const FVector3f& NormalIn, const FVector3f& ColorIn,
                     const FVector2f& UVIn )
             : Position{ PositionIn }, Normal{ NormalIn }, Color{ ColorIn }, UV{ UVIn }, bHaveN{ true },
               bHaveC{ true }, bHaveUV{ true }
        {
        }
    };

    /**
     * FMeshTriEdgeID identifies an edge in a triangle mesh based on
     * the triangle ID/Index and the "edge index" 0/1/2 in the triangle.
     * If the ordered triangle vertices are [A,B,C], then [A,B]=0, [B,C]=1, and [C,A]=2.
     *
     * This type of edge identifier is applicable on any indexed mesh, even if
     * the mesh does not store explicit edge IDs.
     *
     * Values are stored unsigned, so it is currently *not* possible to store an "invalid"
     * edge identifier as a FMeshTriEdgeID (0xFFFFFFFF could potentially be used as such an identifier).
     *
     * The TriangleID is stored in 30 bits, while FDynamicMesh3 stores (valid) triangle IDs in 31 bits.
     * So, only ~1 billion triangles are allowed in a mesh when using FMeshTriEdgeID, vs ~2 billion in
     * FDynamicMesh3. This limit has not been encountered in practice, to date.
     *
     * Note that cycling or permuting the vertices of a triangle will change these indices.
     */
    struct FMeshTriEdgeID
    {
        /** The 0/1/2 index of the edge in the triangle's tuple of edges */
        unsigned TriEdgeIndex : 2;
        /** The index of the mesh Triangle */
        unsigned TriangleID : 30;

        FMeshTriEdgeID()
        {
            TriEdgeIndex = 0;
            TriangleID   = 0;
        }

        /**
         * Construct a FMeshTriEdgeID for the given TriangleID and Edge Index in range
         * @param EdgeIndexIn index in range 0,1,2
         */
        FMeshTriEdgeID( int32 TriangleIDIn, int32 EdgeIndexIn )
        {
            UE_CHECK_SLOW( EdgeIndexIn >= 0 && EdgeIndexIn <= 2 );
            UE_CHECK_SLOW( TriangleIDIn >= 0 && TriangleIDIn < ( 1 << 30 ) );
            TriEdgeIndex = (unsigned int)EdgeIndexIn;
            TriangleID   = (unsigned int)TriangleIDIn;
        }

        /**
         * Decode an encoded FMeshTriEdgeID from a packed uint32 created by the Encoded() function
         */
        explicit FMeshTriEdgeID( uint32 EncodedEdgeKey )
        {
            TriangleID   = EncodedEdgeKey & 0x8FFFFFFF;
            TriEdgeIndex = ( EncodedEdgeKey & 0xC0000000 ) >> 30;
        }

        /**
         * @return the (TriangleID, TriEdgeIndex) values packed into a 32 bit integer
         */
        uint32 Encoded() const
        {
            return ( TriEdgeIndex << 30 ) | TriangleID;
        }
    };

    /**
     * FMeshTriOrderedEdgeID identifies an oriented edge in a triangle mesh based on indices
     * into the triangle vertices. IE if a triangle has vertices [A,B,C], then an
     * oriented edge could be (A,B) or (B,A), or any of the other 4 permutations.
     * So the ordered edge in the triangle can be represented as two vertex indices,
     * and the full encoding is (TriangleID, J, K) where J and K are in range 0/1/2.
     *
     * This type of edge identifier is applicable on any indexed mesh, even if
     * the mesh does not store explicit edge IDs. In addition, this identifier is stable
     * across mesh topological changes, ie if two connected triangles are unlinked (ie
     * the shared edge becomes two edges, and the 2 vertices become 4), the FMeshTriOrderedEdgeID
     * will still refer to the correct oriented edge, as it does not explicitly depend on
     * the Vertex or Edge IDs, only the ordering within the triangle.
     *
     * Note that cycling or permuting the vertices of a triangle will change/break these indices.
     */
    struct FMeshTriOrderedEdgeID
    {
        /** The index of the mesh Triangle */
        int32 TriangleID;
        /** The 0/1/2 index of the first vertex in the triangles tuple of vertices */
        unsigned VertIndexA : 2;
        /** The 0/1/2 index of the second vertex in the triangles tuple of vertices */
        unsigned VertIndexB : 2;

        FMeshTriOrderedEdgeID()
        {
            TriangleID = IndexConstants::InvalidID;
            VertIndexA = 0;
            VertIndexB = 0;
        }

        FMeshTriOrderedEdgeID( int32 TriangleIDIn, int32 VertexIndexA, int32 VertexIndexB )
        {
            UE_CHECK_SLOW( VertexIndexA >= 0 && VertexIndexA <= 2 );
            UE_CHECK_SLOW( VertexIndexB >= 0 && VertexIndexB <= 2 );
            TriangleID = TriangleIDIn;
            VertIndexA = VertexIndexA;
            VertIndexB = VertexIndexB;
        }
    };

} // namespace Desert::Geometry