// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Operations/EmbedSurfacePath.h:22-83,89-121,
// 159-196, adapted: UE Core as std/glm, TFunction as std::function, namespace Desert::Geometry,
// FEmbedSimplePathSettings lives at namespace scope as EmbedSimplePathSettings (a nested struct with default
// member initializers cannot be a default argument inside its own enclosing class). Left out, with reasons:
// - ClosePath and the bIsClosed flag only it sets: no caller; a closed loop is two AddViaPlanarWalk + embeds here.
// - EmbedProjectedPath(s) and Frame3d: outside this port.
// - bUpdatePath: UE never implemented it (its branch is `ensure(false)`), so the path is always consumed.
// - The deprecated (EdgeID, FirstCoordWt) constructor: MakeEdgePoint replaces it in UE 5.8.
// - Validate(): an IsConnected wrapper for the tool framework; callers here call IsConnected.
#pragma once

#include "Engine/Geometry/MeshCore/MathUtil.hpp"
#include "Engine/Geometry/MeshCore/VectorTypes.hpp"

#include <cstdint>
#include <functional>
#include <vector>

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
        int              ElementID = -1;
        glm::dvec3       BaryCoord = glm::dvec3( 0 );
        SurfacePointType PointType = SurfacePointType::Vertex;

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
     * Additional options for controlling simple path embedding
     */
    struct EmbedSimplePathSettings
    {
        // Whether to snap intermediate triangle/edge path points to vertices, using the SnapElementThresholdSq
        bool bSimplifyPathBySnapping = false;

        // Especially after simplification by snapping, we could create paths that revisit the same vertex multiple
        // times -- this option will remove such loops by removing the path between the repeated vertex; i.e. Path
        // A B C D B E becomes just A B E. Note this option should not be used if embedding curved paths, as the
        // loop may be an intentional feature in this case
        bool bRemovePathLoops = false;

        // If an edge split would create an edge smaller than the snap threshold, allow an edge flip instead. Note
        // this is equivalent to splitting then immediately collapsing. Flip will not be performed if it would
        // introduce a fold-over.
        bool bAllowEdgeFlipToCollapseTinyEdges = false;

        /** @return Settings with all path simplification options enabled */
        static EmbedSimplePathSettings WithSimplification()
        {
            EmbedSimplePathSettings Settings;
            Settings.bSimplifyPathBySnapping           = true;
            Settings.bRemovePathLoops                  = true;
            Settings.bAllowEdgeFlipToCollapseTinyEdges = true;
            return Settings;
        }
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
         * Replace m_Path with the shortest walk across the mesh surface from StartPt (on StartTri) to EndPt,
         * staying on the plane through StartPt with normal WalkPlaneNormal (UE resets the path despite the name).
         * The walk crosses edges and vertices where the plane cuts them, so the result satisfies EmbedSimplePath's
         * input assumptions.
         *
         * @param StartVID if not -1, a vertex of StartTri that StartPt sits exactly on
         * @param EndTri triangle holding EndPt, or -1 to accept any triangle within AcceptEndPtOutsideDist of it
         * @param EndVertID if not -1, the walk ends at the first triangle touching this vertex
         * @param VertexToPosnFn position of a vertex in the space of the walk (e.g. UV); mesh positions if empty
         * @param bAllowBackwardsSearch if false, never step behind StartPt as seen along StartPt -> EndPt
         * @param AcceptEndPtOutsideDist squared distance from EndPt at which a triangle counts as the end triangle
         * @param PtOnPlaneThresholdSq |signed plane distance| under which a vertex counts as on the plane
         * @param BackwardsTolerance how far behind StartPt a point may be and still count as forwards
         * @return false if no walk reaches the end
         */
        bool AddViaPlanarWalk( int StartTri, int StartVID, glm::dvec3 StartPt, int EndTri, int EndVertID,
                               glm::dvec3 EndPt, glm::dvec3 WalkPlaneNormal,
                               std::function<glm::dvec3( const DynamicMesh3*, int )> VertexToPosnFn = nullptr,
                               bool                                                  bAllowBackwardsSearch = true,
                               double AcceptEndPtOutsideDist = ZeroTolerance<double>,
                               double PtOnPlaneThresholdSq   = ZeroTolerance<float> * 100,
                               double BackwardsTolerance     = ZeroTolerance<double> * 10 );

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
         * @param SnapElementThresholdSq Squared distance threshold below which path vertices can be snapped to
         *        existing elements
         * @param Settings Additional options controlling how the path is embedded (e.g. to enable more aggressive
         *        snapping)
         * @return true if embedding succeeded.
         */
        bool EmbedSimplePath( std::vector<int>& PathVertices, bool bDoNotDuplicateFirstVertexID = true,
                              double                         SnapElementThresholdSq = ZeroTolerance<float> * 100,
                              const EmbedSimplePathSettings& Settings               = EmbedSimplePathSettings() );
    };
} // namespace Desert::Geometry
