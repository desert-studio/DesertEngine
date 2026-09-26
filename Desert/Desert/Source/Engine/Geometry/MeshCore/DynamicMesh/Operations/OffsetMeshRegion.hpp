// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/OffsetMeshRegion.h and
// Private/Operations/OffsetMeshRegion.cpp:28-63 (Apply), 64-342 (EdgesAreParallel, FindLoopShiftFromGroupIDs,
// LeftShiftArray, StitchRegionBorderLoopPairs_Version1), 455-512 (angle-weighted normals), 527-761
// (ApplyOffset_Version1), 1082-1108 (EdgesSeparateSameGroupsAndAreColinearAtBorder);
// PolyEditingEdgeUtil.cpp:108-170 (ComputeNewGroupIDsAlongEdgeLoop); QuadGridPatchUtil.cpp:14-110 (normals and UV
// island of a quad patch). Adapted: Version1 only (Legacy dropped) and NumSubdivisions = 0, so the QuadGridPatch
// is one row of quads and is walked directly; CreaseAngleThresholdDeg is not ported (UE's default 180 never
// splits); bowties are refused by MeshRegionBoundaryLoops instead of SplitBowtiesAtTriangles;
// MeshConnectedComponents -> a flood fill over the region; ComputeMaterialIDsForVertexPath -> the material of the
// region triangle on each loop edge; UE Core as std/glm; a failure names its reason in FailureReason.
#pragma once


#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/MeshRegionBoundaryLoops.hpp"

#include <string>

namespace Desert::Geometry
{
    class OffsetMeshRegion
    {
    public:
        enum class VertexExtrusionVectorType
        {
            Zero,
            VertexNormal,
            SelectionTriNormalsAngleWeightedAverage,
            SelectionTriNormalsAngleWeightedAdjusted,
        };

        DynamicMesh3*        m_Mesh;
        std::vector<int32_t> m_Triangles;

        std::function<glm::dvec3( const glm::dvec3& Position, const glm::dvec3& VertexVector, int Vid )>
             OffsetPositionFunc = [this]( const glm::dvec3& Position, const glm::dvec3& VertexVector, int )
        { return Position + VertexVector * this->m_DefaultOffsetDistance; };
        double                                            m_DefaultOffsetDistance = 1.0;
        VertexExtrusionVectorType                         m_ExtrusionVectorType = VertexExtrusionVectorType::Zero;
        std::function<bool( int32_t Eid1, int32_t Eid2 )> LoopEdgesShouldHaveSameGroup =
             [this]( int32_t Eid1, int32_t Eid2 )
        { return EdgesSeparateSameGroupsAndAreColinearAtBorder( m_Mesh, Eid1, Eid2, true ); };
        float  m_UVScaleFactor                        = 1.0f;
        bool   m_bOffsetFullComponentsAsSolids        = true;
        bool   m_bIsPositiveOffset                    = true;
        double m_MaxScaleForAdjustingTriNormalsOffset = 4.0;
        bool   m_bSingleGroupPerArea                  = true;
        bool   m_bUVIslandPerGroup                    = true;
        bool   m_bInferMaterialID                     = true;
        int    m_SetMaterialID                        = 0;

        struct OffsetInfo
        {
            std::vector<int32_t>              OffsetTids;
            std::vector<int32_t>              OffsetGroups;
            bool                  bIsSolid = false;
            std::vector<EdgeLoop>             BaseLoops;
            std::vector<EdgeLoop>             OffsetLoops;
            std::vector<std::vector<int32_t>> StitchTriangles;
            std::vector<std::vector<int32_t>> StitchPolygonIDs;
        };
        std::vector<OffsetInfo> m_OffsetRegions;
        std::vector<int32_t>    m_AllModifiedAndNewTriangles;
        // Why Apply returned false, naming the region and the step. Empty on success.
        std::string m_FailureReason;

        explicit OffsetMeshRegion( DynamicMesh3* MeshIn ) : m_Mesh( MeshIn )
        {
        }

        bool Apply();

        static bool EdgesSeparateSameGroupsAndAreColinearAtBorder( DynamicMesh3* Mesh, int32_t Eid1, int32_t Eid2,
                                                                   bool bCheckColinearityAtBorder );

    protected:
        bool ApplyOffset( OffsetInfo& Region );
    };

    // The connected components of @p Triangles (edge adjacency), each in ascending order.
    void FindConnectedTriangleComponents( const DynamicMesh3& Mesh, const std::vector<int32_t>& Triangles,
                                          std::vector<std::vector<int32_t>>& ComponentsOut );
    // UE ComputeNewGroupIDsAlongEdgeLoop (PolyEditingEdgeUtil.cpp:108).
    void
    ComputeNewGroupIDsAlongEdgeLoop( DynamicMesh3& Mesh, const std::vector<int32_t>& LoopEdgeIDs,
                                     std::vector<int32_t>&                          NewLoopEdgeGroupIDs,
                                     std::vector<int32_t>&                          NewGroupIDsOut,
                                     const std::function<bool( int32_t, int32_t )>& EdgesShouldHaveSameGroupFunc );
} // namespace Desert::Geometry
