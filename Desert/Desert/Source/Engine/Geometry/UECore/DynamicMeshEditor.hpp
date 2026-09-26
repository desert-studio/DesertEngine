// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMeshEditor.h and
// Private/DynamicMeshEditor.cpp:267-330 (StitchLoopsInternal, StitchVertexLoopsMinimal), 331-383
// (StitchVertexLoopToTriVidPairSequence, ConvertLoopToTriVidPairSequence), 593-622 (RemoveTriangles), 653-679
// (DuplicateTriangles), 680-804 (DisconnectTriangles), 1176-1230 (ComputeAndSetQuadNormal, SetQuadNormals),
// 1265-1303 (SetTriangleNormals), 1557-1618 (SetQuadUVsFromProjection), 1671-1729 (ReverseTriangleOrientations,
// InvertTriangleNormals), adapted: only the subset FOffsetMeshRegion / FInsetMeshRegion call; UE Core via
// UECore.hpp; TPair -> std::pair; FMeshIndexMappings -> plain maps inside DuplicateTriangles (CopyAttributes
// folded in); the projection frame of SetQuadUVsFromProjection is passed as its two in-plane axes (the frame
// origin is always zero at the call sites); a failed DisconnectTriangles names its reason instead of ensure().
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include <string>
#include <utility>

namespace Desert::Geometry
{
    struct FDynamicMeshEditResult
    {
        TArray<int>      NewVertices;
        TArray<int>      NewTriangles;
        TArray<FIndex2i> NewQuads;
        TArray<int>      NewGroups;

        void Reset()
        {
            NewVertices.Reset();
            NewTriangles.Reset();
            NewQuads.Reset();
            NewGroups.Reset();
        }
        void GetAllTriangles( TArray<int>& TrianglesOut ) const
        {
            TrianglesOut.Append( NewTriangles );
            for ( const FIndex2i& Quad : NewQuads )
            {
                TrianglesOut.Add( Quad.A );
                TrianglesOut.Add( Quad.B );
            }
        }
    };

    // (triangle, (index of the first vertex, index of the second vertex)) - UE's TriVidPair.
    using FTriVidPair = std::pair<int, std::pair<int8_t, int8_t>>;

    class FDynamicMeshEditor
    {
    public:
        FDynamicMesh3* Mesh;

        explicit FDynamicMeshEditor( FDynamicMesh3* MeshIn ) : Mesh( MeshIn )
        {
        }

        struct FLoopPairSet
        {
            TArray<int> OuterVertices;
            TArray<int> OuterEdges;
            TArray<int> InnerVertices;
            TArray<int> InnerEdges;
            bool        bOuterIncludesIsolatedVertices = false;
        };

        bool        StitchVertexLoopsMinimal( const TArray<int>& Loop1, const TArray<int>& Loop2,
                                              FDynamicMeshEditResult& ResultOut );
        bool        StitchVertexLoopToTriVidPairSequence( const TArray<FTriVidPair>& TriVidPairs,
                                                          const TArray<int>&         VertexLoop,
                                                          FDynamicMeshEditResult&    ResultOut );
        static bool ConvertLoopToTriVidPairSequence( const FDynamicMesh3& Mesh, const TArray<int>& VidLoop,
                                                     const TArray<int>&   EdgeLoop,
                                                     TArray<FTriVidPair>& TriVertPairsOut );

        bool RemoveTriangles( const TArray<int>& Triangles, bool bRemoveIsolatedVerts );

        // New triangles (vertices, groups, every attribute layer) for @p Triangles; OldToNewVertex maps each
        // duplicated vertex.
        void DuplicateTriangles( const TArray<int>& Triangles, TMap<int, int>& OldToNewVertex,
                                 FDynamicMeshEditResult& ResultOut );

        // Cuts @p Triangles loose along their boundary loops. On failure @p FailureOut names the reason.
        bool DisconnectTriangles( const TArray<int>& Triangles, TArray<FLoopPairSet>& LoopSetOut,
                                  bool bHandleBoundaryVertices, std::string& FailureOut );
        bool DisconnectTriangles( const TSet<int>& TriangleSet, const TArray<FEdgeLoop>& Loops,
                                  TArray<FLoopPairSet>& LoopSetOut, bool bHandleBoundaryVertices,
                                  std::string& FailureOut );

        glm::vec3 ComputeAndSetQuadNormal( const FIndex2i& QuadTris, bool bIsPlanar );
        void      SetQuadNormals( const FIndex2i& QuadTris, const glm::vec3& Normal );
        void      SetTriangleNormals( const TArray<int>& Triangles );
        void      SetTriangleNormals( const TArray<int>& Triangles, const glm::vec3& Normal );
        bool      AddTriangleFan_OrderedVertexLoop( int CenterVertex, const TArray<int>& VertexLoop, int GroupID,
                                                    FDynamicMeshEditResult& ResultOut );
        /** UE's overload with FFrame3d(Origin, Normal), bShiftToOrigin = true, UV layer 0. */
        void SetTriangleUVsFromProjection( const TArray<int>& Triangles, const glm::dvec3& Origin,
                                           const glm::dvec3& Normal, float UVScaleFactor );
        void SetQuadUVsFromProjection( const FIndex2i& QuadTris, const glm::dvec3& AxisX, const glm::dvec3& AxisY,
                                       float UVScaleFactor, const glm::vec2& UVTranslation );
        void ReverseTriangleOrientations( const TArray<int>& Triangles, bool bInvertNormals );
        void InvertTriangleNormals( const TArray<int>& Triangles );
    };

    // Edges[i] joins Vertices[i] and Vertices[(i+1) % N] (UE FEdgeLoop::VertexLoopToEdgeLoop).
    void VertexLoopToEdgeLoop( const FDynamicMesh3& Mesh, const TArray<int>& VertexLoop,
                               TArray<int>& EdgeLoopOut );
} // namespace Desert::Geometry
