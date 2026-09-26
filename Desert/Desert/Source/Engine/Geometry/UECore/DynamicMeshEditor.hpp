// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMeshEditor.h and
// Private/DynamicMeshEditor.cpp:267-330 (StitchLoopsInternal, StitchVertexLoopsMinimal), 331-383
// (StitchVertexLoopToTriVidPairSequence, ConvertLoopToTriVidPairSequence), 593-622 (RemoveTriangles), 653-679
// (DuplicateTriangles), 680-804 (DisconnectTriangles), 1176-1230 (ComputeAndSetQuadNormal, SetQuadNormals),
// 1265-1303 (SetTriangleNormals), 1557-1618 (SetQuadUVsFromProjection), 1671-1729 (ReverseTriangleOrientations,
// InvertTriangleNormals), adapted: only the subset OffsetMeshRegion / InsetMeshRegion call; UE Core via
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
    struct DynamicMeshEditResult
    {
        std::vector<int>      NewVertices;
        std::vector<int>      NewTriangles;
        std::vector<Index2i>  NewQuads;
        std::vector<int>      NewGroups;

        void Reset()
        {
            NewVertices.clear();
            NewTriangles.clear();
            NewQuads.clear();
            NewGroups.clear();
        }
        void GetAllTriangles( std::vector<int>& TrianglesOut ) const
        {
            TrianglesOut.insert( TrianglesOut.end(), NewTriangles.begin(), NewTriangles.end() );
            for ( const Index2i& Quad : NewQuads )
            {
                TrianglesOut.push_back( Quad.A );
                TrianglesOut.push_back( Quad.B );
            }
        }
    };

    // (triangle, (index of the first vertex, index of the second vertex)) - UE's TriVidPair.
    using TriVidPair = std::pair<int, std::pair<int8_t, int8_t>>;

    class DynamicMeshEditor
    {
    public:
        DynamicMesh3* m_Mesh;

        explicit DynamicMeshEditor( DynamicMesh3* MeshIn ) : m_Mesh( MeshIn )
        {
        }

        struct LoopPairSet
        {
            std::vector<int> OuterVertices;
            std::vector<int> OuterEdges;
            std::vector<int> InnerVertices;
            std::vector<int> InnerEdges;
            bool        bOuterIncludesIsolatedVertices = false;
        };

        bool        StitchVertexLoopsMinimal( const std::vector<int>& Loop1, const std::vector<int>& Loop2,
                                              DynamicMeshEditResult& ResultOut );
        bool        StitchVertexLoopToTriVidPairSequence( const std::vector<TriVidPair>& TriVidPairs,
                                                          const std::vector<int>&        VertexLoop,
                                                          DynamicMeshEditResult&         ResultOut );
        static bool ConvertLoopToTriVidPairSequence( const DynamicMesh3& Mesh, const std::vector<int>& VidLoop,
                                                     const std::vector<int>&  EdgeLoop,
                                                     std::vector<TriVidPair>& TriVertPairsOut );

        bool RemoveTriangles( const std::vector<int>& Triangles, bool bRemoveIsolatedVerts ) const;

        // New triangles (vertices, groups, every attribute layer) for @p Triangles; OldToNewVertex maps each
        // duplicated vertex.
        void DuplicateTriangles( const std::vector<int>& Triangles, std::unordered_map<int, int>& OldToNewVertex,
                                 DynamicMeshEditResult& ResultOut ) const;

        // Cuts @p Triangles loose along their boundary loops. On failure @p FailureOut names the reason.
        bool DisconnectTriangles( const std::vector<int>& Triangles, std::vector<LoopPairSet>& LoopSetOut,
                                  bool bHandleBoundaryVertices, std::string& FailureOut );
        bool DisconnectTriangles( const std::unordered_set<int>& TriangleSet, const std::vector<EdgeLoop>& Loops,
                                  std::vector<LoopPairSet>& LoopSetOut, bool bHandleBoundaryVertices,
                                  std::string& FailureOut ) const;

        glm::vec3 ComputeAndSetQuadNormal( const Index2i& QuadTris, bool bIsPlanar );
        void      SetQuadNormals( const Index2i& QuadTris, const glm::vec3& Normal ) const;
        void      SetTriangleNormals( const std::vector<int>& Triangles ) const;
        void      SetTriangleNormals( const std::vector<int>& Triangles, const glm::vec3& Normal ) const;
        bool AddTriangleFan_OrderedVertexLoop( int CenterVertex, const std::vector<int>& VertexLoop, int GroupID,
                                               DynamicMeshEditResult& ResultOut );
        /** UE's overload with Frame3d(Origin, Normal), bShiftToOrigin = true, UV layer 0. */
        void SetTriangleUVsFromProjection( const std::vector<int>& Triangles, const glm::dvec3& Origin,
                                           const glm::dvec3& Normal, float UVScaleFactor ) const;
        void SetQuadUVsFromProjection( const Index2i& QuadTris, const glm::dvec3& AxisX, const glm::dvec3& AxisY,
                                       float UVScaleFactor, const glm::vec2& UVTranslation ) const;
        void ReverseTriangleOrientations( const std::vector<int>& Triangles, bool bInvertNormals );
        void InvertTriangleNormals( const std::vector<int>& Triangles ) const;
    };

    // Edges[i] joins Vertices[i] and Vertices[(i+1) % N] (UE EdgeLoop::VertexLoopToEdgeLoop).
    void VertexLoopToEdgeLoop( const DynamicMesh3& Mesh, const std::vector<int>& VertexLoop,
                               std::vector<int>& EdgeLoopOut );
} // namespace Desert::Geometry
