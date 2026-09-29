#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/QuadricError.h:22-293 (TQuadricError<double>) and
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/MeshSimplification.h with
// Private/MeshSimplification.cpp:49-58,254-275,344-484,601-779,780-1044,1045-1200,1824-2107 and
// Private/MeshRefinerBase.cpp:16-60,175-358 (the collapse checks), adapted: std/glm, one quadric type (UE's
// FQEMSimplification = TMeshSimplification<FQuadricErrord>, the QEM simplifier of SimplifyMeshOp.cpp:261), and
// only what the editor's Simplify uses - the triangle-count and vertex-count targets, collapse mode
// MinimalQuadricPositionError, bPreserveBoundaryShape, bRetainQuadricMemory = false (UE's default: face quadrics
// are recomputed around every collapse). Not ported: the edge-length / max-error / minimal-planar targets,
// reprojection and the geometric-error tolerance (no projection target), attribute-aware quadrics, regularization
// and the custom scale functions (UE's defaults leave them off), RemoveIsolatedTriangle, the change tracker and
// cancellation.

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshConstraints.hpp"
#include "Engine/Geometry/MeshCore/IndexPriorityQueue.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Desert::Geometry
{
    // TQuadricError<double>: the plane-distance quadric p*A*p + 2*dot(p, b) + c with symmetric A.
    struct QuadricErrord
    {
        double Axx = 0, Axy = 0, Axz = 0, Ayy = 0, Ayz = 0, Azz = 0;
        double Bx = 0, By = 0, Bz = 0;
        double C = 0;

        QuadricErrord() = default;
        // The squared distance to the plane through @p point with unit @p normal.
        QuadricErrord( const glm::dvec3& normal, const glm::dvec3& point );

        void                     Add( double weight, const QuadricErrord& other );
        [[nodiscard]] double     Evaluate( const glm::dvec3& point ) const;
        [[nodiscard]] glm::dvec3 MultiplyA( const glm::dvec3& point ) const;
        // The minimiser of Evaluate, when A is invertible past @p minThresh (UE: 1000 * double epsilon).
        [[nodiscard]] bool OptimalPoint( glm::dvec3& out,
                                         double      minThresh = 1000.0 * 2.220446049250313e-16 ) const;
    };

    class QemSimplification
    {
    public:
        explicit QemSimplification( DynamicMesh3& mesh ) : m_Mesh( mesh )
        {
        }

        // The flags the constraints are rebuilt with around each collapse (UE copies the same values it built
        // the external constraints with, SimplifyMeshOp.cpp:91-93).
        BoundaryConstraintFlags Boundaries{};

        void SetExternalConstraints( MeshConstraints constraints )
        {
            m_Constraints = std::move( constraints );
        }

        // FMeshRefinerBase::SetEdgeFlipTolerance: a collapse is refused when the dot of a moved triangle's old and
        // new normals is at most @p tolerance (clamped to [-1, 1]). 0 compares the raw cross products (only the
        // sign counts); any other value compares unit normals.
        void SetEdgeFlipTolerance( double tolerance )
        {
            m_EdgeFlipTolerance = std::clamp( tolerance, -1.0, 1.0 );
        }
        // bPreventTinyTriangles: refuse a collapse that shrinks a triangle to (almost) no area.
        bool PreventTinyTriangles = false;
        // Collapse the cheapest edges until the mesh has at most @p count triangles (UE clamps it to >= 1) or
        // no edge may collapse.
        void SimplifyToTriangleCount( int count );
        // Collapse until the mesh has at most @p count vertices (clamped to >= 3), or no edge may collapse.
        void SimplifyToVertexCount( int count );

        [[nodiscard]] int CollapseCount() const
        {
            return m_Collapses;
        }

    private:
        enum class TargetMode : uint8_t
        {
            TriangleCount,
            VertexCount,
        };
        enum class CollapseResult : uint8_t
        {
            Collapsed,
            Ignored,
            Failed,
        };
        struct QEdge
        {
            QuadricErrord Q;
            glm::dvec3    CollapsePoint{ 0.0 };
        };

        void                         DoSimplify();
        void                         Precompute();
        void                         InitializeTriQuadrics();
        void                         InitializeVertexQuadrics();
        void                         InitializeSeamQuadrics();
        [[nodiscard]] QuadricErrord  SeamQuadric( int edge ) const;
        void                         InitializeQueue();
        [[nodiscard]] QuadricErrord  AssembleEdgeQuadric( const DynamicMesh3::Edge& edge ) const;
        [[nodiscard]] glm::dvec3     OptimalPoint( int edge, const QuadricErrord& q, int a, int b ) const;
        [[nodiscard]] CollapseResult CollapseEdge( int edge, glm::dvec3 newPosition,
                                                   DynamicMeshInfo::EdgeCollapseInfo& info );
        [[nodiscard]] bool           CanCollapseVertex( int a, int b, int& collapseTo ) const;
        [[nodiscard]] bool CanCollapseEdge( int a, int b, int c, int d, int tc, int td, int& collapseTo ) const;
        [[nodiscard]] bool CreatesFlipOrInvalid( int vertex, int other, const glm::dvec3& newPosition, int tc,
                                                 int td ) const;
        [[nodiscard]] bool CreatesTinyTriangle( int vertex, int other, const glm::dvec3& newPosition, int tc,
                                                int td ) const;
        [[nodiscard]] bool           IsConstrainedEdge( int edge ) const;
        [[nodiscard]] int            ConstrainedEdgeCount( int vertex ) const;
        [[nodiscard]] bool CreatesFin( int vertex, int other, const glm::dvec3& newPosition, int c, int d, int tc,
                                       int td ) const;
        void               UpdateNeighborhood( const DynamicMeshInfo::EdgeCollapseInfo& info );
        void               UpdateConstraintsAround( int edge );

        DynamicMesh3&                  m_Mesh;
        std::optional<MeshConstraints> m_Constraints;
        TargetMode                     m_Mode         = TargetMode::TriangleCount;
        int                            m_Target       = 0;
        int                            m_Collapses    = 0;
        bool                           m_HaveBoundary = false;
        std::vector<bool>              m_IsBoundaryVertex;
        std::vector<QuadricErrord>     m_TriQuadrics;
        std::vector<double>            m_TriAreas;
        std::vector<QuadricErrord>     m_VertQuadrics;
        std::vector<QEdge>             m_EdgeQuadrics;
        // TMeshSimplification::seamQuadrics: one quadric per constrained edge while seams may collapse, holding a
        // vertex near the edge's line (SeamEdgeWeight 256).
        std::unordered_map<int, QuadricErrord> m_SeamQuadrics;
        double                                 m_EdgeFlipTolerance = 0.0;
        IndexPriorityQueue                     m_Queue;
    };
} // namespace Desert::Geometry
