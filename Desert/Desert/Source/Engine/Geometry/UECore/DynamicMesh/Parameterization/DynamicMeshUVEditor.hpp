// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Parameterization/
// DynamicMeshUVEditor.h, adapted: only the ExpMap path the bevel needs (ResetUVs(Triangles), TransformUVElements,
// EstimateGeodesicCenterFrameVertex, SetTriangleUVsFromExpMap) and SetTriangleUVsFromFreeBoundarySpectralConformal
// (the spectral branch of SetTriangleUVsFromConformal; the natural-conformal branch has no caller here);
// FExpMapOptions is not ported (its defaults, zero normal-smoothing rounds, are what every UE caller here passes).
// SetToPerVertexUVs and ScaleUVAreaTo3DArea (with DetermineAreaFromUVs inlined) serve the bevel's MVC patch.
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/FrameTypes.hpp"

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

    private:
        DynamicMesh3*         m_Mesh;
        DynamicMeshUVOverlay* m_UVOverlay;
    };
} // namespace Desert::Geometry
