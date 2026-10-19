// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Parameterization/
// DynamicMeshUVEditor.h, adapted: only the ExpMap path the bevel needs (ResetUVs(Triangles), TransformUVElements,
// EstimateGeodesicCenterFrameVertex, SetTriangleUVsFromExpMap) and SetTriangleUVsFromFreeBoundarySpectralConformal
// (the spectral branch of SetTriangleUVsFromConformal; the natural-conformal branch has no caller here);
// FExpMapOptions is not ported (its defaults, zero normal-smoothing rounds, are what every UE caller here passes).
// SetToPerVertexUVs and ScaleUVAreaTo3DArea (with DetermineAreaFromUVs inlined) serve the bevel's MVC patch.
// The projections (SetTriangleUVsFromPlanarProjection :282-330, SetTriangleUVsFromBoxProjection :1159-1304,
// SetTriangleUVsFromCylinderProjection :1322-1459), SplitBowtiesOnUVElements (:1306-1320), AutoOrientUVArea
// (:1562-1669) and QuickPack (:1671-1684, with FDynamicMeshUVPacker::StandardPack of MeshUVPacking.cpp:62-120)
// serve the Modeling palette's UVs tools; the box projection's MinIslandTriCount region merging (FMeshRegionGraph)
// is not ported, every triangle keeps its own box face.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/FrameTypes.hpp"

#include <functional>

namespace Desert::Geometry
{
    struct UVEditResult
    {
        std::vector<int32_t> NewUVElements;
    };

    class DynamicMeshUVEditor
    {
    public:
        DynamicMeshUVEditor( DynamicMesh3* MeshIn, DynamicMeshUVOverlay* UVOverlayIn )
             : m_Mesh( MeshIn ), m_UVOverlay( UVOverlayIn )
        {
        }

        void ResetUVs( const std::vector<int32_t>& Triangles );
        void TransformUVElements( const std::vector<int32_t>&                         ElementIDs,
                                  const std::function<glm::vec2( const glm::vec2& )>& TransformFunc );

        /** Frame at the vertex farthest (Dijkstra) from the longest boundary loop; false without a boundary. */
        static bool EstimateGeodesicCenterFrameVertex( const DynamicMesh3& Mesh, Frame3d& FrameOut,
                                                       int32_t& VertexIDOut, bool bAlignToUnitAxes = true );

        /**
         * New UV island for the (connected) Triangles from one discrete exponential map centred at
         * EstimateGeodesicCenterFrameVertex. False if the frame was a fallback or any triangle stayed unset.
         */
        bool SetTriangleUVsFromExpMap( const std::vector<int32_t>& Triangles, UVEditResult* Result = nullptr );

        /**
         * Spectral conformal UVs with a free boundary (nothing pinned; the longest boundary loop is the free set).
         * bUseExistingUVTopology keeps the triangles' current UV elements and only moves them (triangles without
         * UVs are skipped); otherwise a new island is made. False without a boundary or if the solve failed.
         */
        bool SetTriangleUVsFromFreeBoundarySpectralConformal( const std::vector<int32_t>& Triangles,
                                                              bool                        bUseExistingUVTopology,
                                                              bool                        bPreserveIrregularity,
                                                              UVEditResult*               Result = nullptr );

        /**
         * One UV element per mesh vertex, set on every triangle (existing elements are cleared). VertexToUVOut
         * maps vertex ID -> element ID; bIsIdentityMapOut is true when the two coincide.
         */
        void SetToPerVertexUVs( std::vector<int32_t>& VertexToUVOut, bool& bIsIdentityMapOut );

        /**
         * Scale the Triangles' UVs uniformly about their UV bounding-box centre so their UV area is ScaleFactor^2
         * times their 3D area; bRecenterAtOrigin puts that centre at the origin. False on a zero or non-finite
         * area.
         */
        bool ScaleUVAreaTo3DArea( const std::vector<int32_t>& Triangles, bool bRecenterAtOrigin,
                                  float ScaleFactor = 1.0f );

        /**
         * Planar projection: each vertex's PointTransform(position), in ProjectionFrame's X/Y plane, divided by
         * Dimensions. One new island (the triangles' UVs are reset first).
         */
        void
        SetTriangleUVsFromPlanarProjection( const std::vector<int32_t>&                           Triangles,
                                            const std::function<glm::dvec3( const glm::dvec3& )>& PointTransform,
                                            const Frame3d& ProjectionFrame, const glm::dvec2& Dimensions,
                                            UVEditResult* Result = nullptr );

        /**
         * Box projection: each triangle onto the face of the box (BoxFrame, BoxDimensions) its normal points at
         * most; one island per face run, bowties split.
         */
        void SetTriangleUVsFromBoxProjection( const std::vector<int32_t>&                           Triangles,
                                              const std::function<glm::dvec3( const glm::dvec3& )>& PointTransform,
                                              const Frame3d& BoxFrame, const glm::dvec3& BoxDimensions,
                                              UVEditResult* Result = nullptr );

        /**
         * Cylinder projection around BoxFrame's Z: triangles whose normal is within CylinderSplitAngle degrees of
         * +/-Z get the caps' planar projection, the others the unrolled side (split at the frame's -X).
         */
        void
        SetTriangleUVsFromCylinderProjection( const std::vector<int32_t>&                           Triangles,
                                              const std::function<glm::dvec3( const glm::dvec3& )>& PointTransform,
                                              const Frame3d& BoxFrame, const glm::dvec3& BoxDimensions,
                                              float CylinderSplitAngle, UVEditResult* Result = nullptr );

        /** Splits the overlay bowties at the parent vertices of UVElementIDs; new elements are appended to it. */
        void SplitBowtiesOnUVElements( std::vector<int32_t>& UVElementIDs, bool bAddNewElementsToInputArray );

        /** Rotates the Triangles' UVs so their bounding box is the smallest one (rotating calipers on the hull).
         */
        bool AutoOrientUVArea( const std::vector<int32_t>& Triangles );

        /** Packs every UV island of the overlay into the unit square (bowties split first). */
        bool QuickPack( int32_t TargetTextureResolution = 512, float GutterSize = 1.0f );

    private:
        DynamicMesh3*         m_Mesh;
        DynamicMeshUVOverlay* m_UVOverlay;
    };
} // namespace Desert::Geometry
