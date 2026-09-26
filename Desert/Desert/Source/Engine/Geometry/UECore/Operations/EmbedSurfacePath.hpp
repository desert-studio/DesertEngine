// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Operations/EmbedSurfacePath.h:22-83,89-104,113-121,
// 160-196, adapted: UE Core via UECore.hpp, namespace Desert::Geometry. Only what GroupEdgeInserter's plane-cut
// embedding (GroupEdgeInserter.cpp:1025-1028) uses is ported: MeshSurfacePoint and
// MeshSurfacePath::EmbedSimplePath, plus IsConnected to check a path before embedding it. Left out, with reasons:
// - AddViaPlanarWalk / ClosePath and the closed-path flag they set (WalkMeshPlanar, ~360 lines): no caller yet;
//   GroupEdgeInserter builds its path itself (GetPlaneCutPath).
// - EmbedProjectedPath(s) and Frame3d: outside this port.
// - FEmbedSimplePathSettings (snap-to-vertex, loop removal, tiny-edge flips): every caller here takes the default
//   (all off), so the options would be settings nothing sets.
// - bUpdatePath: UE never implemented it (its branch is `ensure(false)`), so the path is always consumed.
// - The deprecated (EdgeID, FirstCoordWt) constructor: MakeEdgePoint replaces it in UE 5.8.
// - Validate(): an IsConnected wrapper for the tool framework; callers here call IsConnected.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/MathUtil.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    class DynamicMesh3;

    enum class SurfacePointType
    {
        Vertex   = 0,
        Edge     = 1,
        Triangle = 2
    };

    /**
     * Basic struct to represent a point on a mesh surface, as a vertex, a point on an edge, or a point inside a
     * triangle.
     */
    struct MeshSurfacePoint
    {
        int               ElementID = -1;
        glm::dvec3        BaryCoord = glm::dvec3( 0 );
        SurfacePointType  PointType = SurfacePointType::Vertex;

        MeshSurfacePoint() = default;

        MeshSurfacePoint( int TriangleID, const glm::dvec3& InBaryCoord )
             : ElementID( TriangleID ), BaryCoord( InBaryCoord ), PointType( SurfacePointType::Triangle )
        {
        }

        explicit MeshSurfacePoint( int VertexID ) : ElementID( VertexID ), BaryCoord( 1, 0, 0 )
        {
        }

        MeshSurfacePoint( int InElementID, const glm::dvec3& InBaryCoord, SurfacePointType InPointType )
             : ElementID( InElementID ), BaryCoord( InBaryCoord ), PointType( InPointType )
        {
        }

        /** A point on edge EdgeID at LerpParam from the edge's first vertex (GetEdgeV order) to its second. */
        static MeshSurfacePoint MakeEdgePoint( int32_t EdgeID, double LerpParam )
        {
            return { EdgeID, glm::dvec3( 1 - LerpParam, LerpParam, 0. ), SurfacePointType::Edge };
        }

        /** @return the parameter to pass to DynamicMesh3::SplitEdge to split the edge at this point */
        [[nodiscard]] double GetEdgeSplitParam() const
        {
            assert( PointType == SurfacePointType::Edge );
            return BaryCoord[1];
        }

        glm::dvec3 Pos( const DynamicMesh3* Mesh ) const;
    };

    /**
     * Represent a path on the surface of a mesh via barycentric coordinates and triangle references
     */
    class MeshSurfacePath
    {
    public:
        DynamicMesh3* m_Mesh;
        // Surface points paired with triangle to walk to get to next surface point
        std::vector<std::pair<MeshSurfacePoint, int>> m_Path;

        explicit MeshSurfacePath( DynamicMesh3* InMesh ) : m_Mesh( InMesh )
        {
        }

        /**
         * @return True if the Path exactly sticks to the mesh surface, and never jumps to disconnected elements
         */
        [[nodiscard]] bool IsConnected() const;

        /**
         * Embed a surface path in mesh provided that the path only crosses vertices and edges except at the start
         * and end, so we can add the path easily with local edge splits and possibly two triangle pokes (rather
         * than needing general remeshing machinery). The Path is no longer valid afterwards: the elements it names
         * have been split.
         *
         * @param PathVertices Indices of the vertices on the path are appended here; NOTE these will not be 1:1
         *        with the input Path
         * @param bDoNotDuplicateFirstVertexID Useful if repeatedly calling EmbedSimplePath to extend a path. If
         *        true, will not add the first path vertex if it matches the last vertex of the initial, passed-in
         *        PathVertices.
         * @param SnapElementThresholdSq Squared distance threshold below which a relocated end point snaps to an
         *        existing vertex or edge
         * @return true if embedding succeeded.
         */
        bool EmbedSimplePath( std::vector<int>& PathVertices, bool bDoNotDuplicateFirstVertexID = true,
                              double SnapElementThresholdSq = ZeroTolerance<float> * 100 );
    };
} // namespace Desert::Geometry
