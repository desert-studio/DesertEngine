#pragma once

// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/MeshConstraints.h:18-436
// and MeshConstraintsUtil.h:166 / Private/MeshConstraintsUtil.cpp:43-100,178-260, adapted: std::unordered_map
// instead of TMap, no projection targets (FVertexConstraint::Target, FEdgeConstraint::Target) and no FixedSetID -
// the simplifier here never reprojects - and ConstrainAllBoundariesAndSeams runs serially, seam splits are always
// allowed and seam smoothing follows seam collapse (SimplifyMeshTool.cpp:248-249 sets both from one property).
// Not ported: the ROI / selection helpers.

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"

#include <cstdint>
#include <unordered_map>

namespace Desert::Geometry
{
    // What a refiner may do to an edge (EEdgeRefineFlags).
    enum class EdgeRefineFlags : uint8_t
    {
        NoConstraint = 0,
        NoFlip       = 1,
        NoSplit      = 2,
        NoCollapse   = 4,
        // Disconnected constrained edges must not be joined by an adjacent collapse.
        NoTopologyMerge  = 8,
        FullyConstrained = NoFlip | NoSplit | NoCollapse | NoTopologyMerge,
    };

    [[nodiscard]] constexpr EdgeRefineFlags operator|( EdgeRefineFlags a, EdgeRefineFlags b )
    {
        return static_cast<EdgeRefineFlags>( static_cast<uint8_t>( a ) | static_cast<uint8_t>( b ) );
    }

    struct EdgeConstraint
    {
        EdgeRefineFlags Flags = EdgeRefineFlags::NoConstraint;

        [[nodiscard]] static constexpr bool CanFlip( EdgeRefineFlags flags )
        {
            return ( static_cast<uint8_t>( flags ) & static_cast<uint8_t>( EdgeRefineFlags::NoFlip ) ) == 0;
        }
        [[nodiscard]] static constexpr bool CanCollapse( EdgeRefineFlags flags )
        {
            return ( static_cast<uint8_t>( flags ) & static_cast<uint8_t>( EdgeRefineFlags::NoCollapse ) ) == 0;
        }
        // Disconnected constrained edges may be joined by an adjacent collapse (no NoTopologyMerge).
        [[nodiscard]] bool CanMergeTopology() const
        {
            return ( static_cast<uint8_t>( Flags ) & static_cast<uint8_t>( EdgeRefineFlags::NoTopologyMerge ) ) ==
                   0;
        }
        [[nodiscard]] bool CanFlip() const
        {
            return CanFlip( Flags );
        }
        [[nodiscard]] bool CanCollapse() const
        {
            return CanCollapse( Flags );
        }
        [[nodiscard]] bool IsUnconstrained() const
        {
            return Flags == EdgeRefineFlags::NoConstraint;
        }
        [[nodiscard]] bool NoModifications() const
        {
            return Flags == EdgeRefineFlags::FullyConstrained;
        }
    };

    // FVertexConstraint without a projection target: a vertex that cannot be deleted is kept by a collapse,
    // one that cannot move keeps its position.
    struct VertexConstraint
    {
        bool CannotDelete = false;
        bool CanMove      = true;

        [[nodiscard]] static VertexConstraint Unconstrained()
        {
            return { false, true };
        }
        [[nodiscard]] static VertexConstraint FullyConstrained()
        {
            return { true, false };
        }
        [[nodiscard]] bool IsUnconstrained() const
        {
            return CanMove && !CannotDelete;
        }
        void Combine( const VertexConstraint& other )
        {
            CannotDelete = CannotDelete || other.CannotDelete;
            CanMove      = CanMove && other.CanMove;
        }
    };

    class MeshConstraints
    {
    public:
        [[nodiscard]] bool HasEdgeConstraint( int edge ) const
        {
            return m_Edges.contains( edge );
        }
        [[nodiscard]] EdgeConstraint GetEdgeConstraint( int edge ) const
        {
            const auto found = m_Edges.find( edge );
            return found == m_Edges.end() ? EdgeConstraint{} : found->second;
        }
        [[nodiscard]] VertexConstraint GetVertexConstraint( int vertex ) const
        {
            const auto found = m_Vertices.find( vertex );
            return found == m_Vertices.end() ? VertexConstraint::Unconstrained() : found->second;
        }
        void SetOrUpdateEdgeConstraint( int edge, EdgeConstraint constraint )
        {
            m_Edges[edge] = constraint;
        }
        void SetOrUpdateVertexConstraint( int vertex, VertexConstraint constraint )
        {
            m_Vertices[vertex] = constraint;
        }
        void ClearEdgeConstraint( int edge )
        {
            m_Edges.erase( edge );
        }
        void ClearVertexConstraint( int vertex )
        {
            m_Vertices.erase( vertex );
        }
        [[nodiscard]] size_t EdgeConstraintCount() const
        {
            return m_Edges.size();
        }
        [[nodiscard]] const std::unordered_map<int, EdgeConstraint>& GetEdgeConstraints() const
        {
            return m_Edges;
        }

    private:
        std::unordered_map<int, EdgeConstraint>   m_Edges;
        std::unordered_map<int, VertexConstraint> m_Vertices;
    };

    // The boundary kinds a simplification respects, one flag set per kind (UE's MeshBoundaryConstraint /
    // GroupBoundaryConstraint; MaterialBoundaryConstraint is not ported - the editor's meshes carry one material
    // id layer only through their polygroups). Seams (UV / normal / colour splits) never flip. With
    // AllowSeamCollapse (UE's Simplify default: Preserve Sharp Edges off, SimplifyMeshTool.cpp:248-249) a seam
    // edge may collapse and its vertices may move, except the first and last edge of a seam
    // (MeshConstraintsUtil.cpp:244-263); without it a seam never collapses and its vertices never move.
    struct BoundaryConstraintFlags
    {
        EdgeRefineFlags MeshBoundary      = EdgeRefineFlags::NoFlip;
        EdgeRefineFlags GroupBoundary     = EdgeRefineFlags::NoConstraint;
        bool            AllowSeamCollapse = true;
    };

    // FMeshConstraintsUtil::ConstrainEdgeBoundariesAndSeams: the constraint of edge @p edge and of its two
    // vertices from what the edge is (mesh boundary, group boundary, seam). False when the edge is free.
    bool ConstrainEdgeBoundariesAndSeams( int edge, const DynamicMesh3& mesh, const BoundaryConstraintFlags& flags,
                                          EdgeConstraint& outEdge, VertexConstraint& outA,
                                          VertexConstraint& outB );

    // FMeshConstraintsUtil::ConstrainAllBoundariesAndSeams over every edge of @p mesh, merged into @p constraints.
    void ConstrainAllBoundariesAndSeams( MeshConstraints& constraints, const DynamicMesh3& mesh,
                                         const BoundaryConstraintFlags& flags );
} // namespace Desert::Geometry
