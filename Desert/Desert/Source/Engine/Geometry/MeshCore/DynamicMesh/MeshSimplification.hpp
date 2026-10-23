#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/QuadricError.h:22-293 (TQuadricError<double>) and
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/MeshSimplification.h with
// Private/MeshSimplification.cpp:49-58,254-275,344-484,601-779,780-1044,1045-1200,1824-2107 and
// Private/MeshRefinerBase.cpp:16-60,175-358 (the collapse checks), adapted: std/glm, one quadric type (UE's
// FQEMSimplification = TMeshSimplification<FQuadricErrord>, the QEM simplifier of SimplifyMeshOp.cpp:261), and
// only what the editor's Simplify uses - the triangle-count and vertex-count targets, collapse mode
// MinimalQuadricPositionError, bPreserveBoundaryShape, bRetainQuadricMemory = false (UE's default: face quadrics
// are recomputed around every collapse). Not ported: the edge-length / max-error / minimal-planar targets,
// reprojection and the geometric-error tolerance (no projection target), the V2 attribute quadric, regularization
// and the custom scale functions (UE's defaults leave them off), RemoveIsolatedTriangle, the change tracker and
// cancellation.
// Attribute-Aware (UE FAttrMeshSimplification = TMeshSimplification<FAttrBasedQuadricErrord>, SimplifyMeshOp.cpp:274-293):
// the same simplifier over QuadricError.h:304-420 TVolPresQuadricError + :493-800 TAttrBasedQuadricError, with
// MeshSimplification.cpp:58-97 ComputeFaceQuadric and :2366-2402 UpdateVertexAttributes specialised for it.

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshConstraints.hpp"
#include "Engine/Geometry/MeshCore/IndexPriorityQueue.hpp"

#include <glm/mat4x4.hpp>
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
        void               AddSeam( const QuadricErrord& seam ) { Add( 1.0, seam ); }
    };

    // TAttrBasedQuadricError<double> (over TVolPresQuadricError): the plane quadric plus, per normal component, the
    // linear gradient interpolating that component over the face (weighted AttrWeight), so a collapse pays for the
    // normal it would have to bend; the optimal point keeps the enclosed volume. ComputeAttributes reads the
    // normal the quadric predicts at a point (the collapsed vertex's new normal).
    struct AttrQuadricErrord
    {
        QuadricErrord Base;
        glm::dvec3    PlaneN{ 0.0 }; // TVolPresQuadricError::FPlaneData
        double        PlaneDist  = 0.0;
        double        A          = 0.0; // accumulated area weight
        double        AttrWeight = 1.0;
        glm::dvec3    Grad[3]{ glm::dvec3( 0.0 ), glm::dvec3( 0.0 ), glm::dvec3( 0.0 ) };
        double        D[3]{ 0.0, 0.0, 0.0 };

        AttrQuadricErrord() = default;
        // The triangle quadric from its corners @p p, corner normals @p n, unit face normal and centre.
        AttrQuadricErrord( const glm::dvec3 p[3], const glm::dvec3 n[3], const glm::dvec3& faceNormal,
                           const glm::dvec3& centre, double attrWeight );

        void Add( double weight, const AttrQuadricErrord& other );
        // AddSeamQuadric: a seam quadric holds position only.
        void                     AddSeam( const QuadricErrord& seam ) { Base.Add( 1.0, seam ); }
        [[nodiscard]] double     Evaluate( const glm::dvec3& point ) const;
        [[nodiscard]] glm::dvec3 ComputeAttributes( const glm::dvec3& point ) const;
        [[nodiscard]] bool       OptimalPoint( glm::dvec3& out,
                                               double      minThresh = 1000.0 * 2.220446049250313e-16 ) const;
    };

    // TMeshSimplification<QuadricT>: QemSimplification (FQEMSimplification, Standard) and AttrMeshSimplification
    // (FAttrMeshSimplification, Attribute-Aware) are the one simplifier over two quadric types.
    template <typename QuadricT>
    class QemSimplificationT
    {
    public:
        explicit QemSimplificationT( DynamicMesh3& mesh ) : m_Mesh( mesh )
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
        // UE TMeshSimplification::SimplifyToEdgeLength (ETargetModes::MinEdgeLength): every edge shorter than
        // @p minEdgeLength collapses, lowest error first, until the queue runs dry; longer edges are skipped.
        // The length is measured after @p toWorld (the editor's lengths are world cm, like Remesh).
        void SimplifyToEdgeLength( double minEdgeLength, const glm::dmat4& toWorld = glm::dmat4( 1.0 ) );

        [[nodiscard]] int CollapseCount() const
        {
            return m_Collapses;
        }

    private:
        enum class TargetMode : uint8_t
        {
            TriangleCount,
            VertexCount,
            MinEdgeLength,
        };
        enum class CollapseResult : uint8_t
        {
            Collapsed,
            Ignored,
            Failed,
        };
        struct QEdge
        {
            QuadricT   Q;
            glm::dvec3 CollapsePoint{ 0.0 };
        };

        void                         DoSimplify();
        void                         Precompute();
        void                         InitializeTriQuadrics();
        void                         InitializeVertexQuadrics();
        void                         InitializeSeamQuadrics();
        [[nodiscard]] QuadricErrord  SeamQuadric( int edge ) const;
        void                         InitializeQueue();
        [[nodiscard]] QuadricT       ComputeFaceQuadric( int tri, double& area ) const;
        void                         UpdateVertexAttributes( int edge, int kept );
        [[nodiscard]] QuadricT       AssembleEdgeQuadric( const DynamicMesh3::Edge& edge ) const;
        [[nodiscard]] glm::dvec3     OptimalPoint( int edge, const QuadricT& q, int a, int b ) const;
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
        void               UpdateNeighborhood( int collapsedEdge, const DynamicMeshInfo::EdgeCollapseInfo& info );
        void               UpdateConstraintsAround( int edge );

        DynamicMesh3&                  m_Mesh;
        std::optional<MeshConstraints> m_Constraints;
        TargetMode                     m_Mode         = TargetMode::TriangleCount;
        int                            m_Target       = 0;
        double                         m_MinEdgeLength = 0.0;
        glm::dmat4                     m_ToWorld{ 1.0 };
        int                            m_Collapses    = 0;
        bool                           m_HaveBoundary = false;
        std::vector<bool>              m_IsBoundaryVertex;
        std::vector<QuadricT>          m_TriQuadrics;
        std::vector<double>            m_TriAreas;
        std::vector<QuadricT>          m_VertQuadrics;
        std::vector<QEdge>             m_EdgeQuadrics;
        // TMeshSimplification::seamQuadrics: one quadric per constrained edge while seams may collapse, holding a
        // vertex near the edge's line (SeamEdgeWeight 256).
        std::unordered_map<int, QuadricErrord> m_SeamQuadrics;
        double                                 m_EdgeFlipTolerance = 0.0;
        IndexPriorityQueue                     m_Queue;
    };

    extern template class QemSimplificationT<QuadricErrord>;
    extern template class QemSimplificationT<AttrQuadricErrord>;
    using QemSimplification      = QemSimplificationT<QuadricErrord>;
    using AttrMeshSimplification = QemSimplificationT<AttrQuadricErrord>;
} // namespace Desert::Geometry
