// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/GroupTopology.h:18-568,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry; GroupEdge::Span is EdgeSpan from
// MeshRegionBoundaryLoops.hpp; RebuildTopology returns the region-loop failure by name. Not ported: the
// extra-corner hook (ShouldAddExtraCornerAtVert, RebuildTopologyWithSpecificExtraCorners), the MeshTriEdgeID
// overloads, and the frame/bounds helpers (GetGroupFrame, GetSelectionFrame, GetSelectionBounds,
// GetEdgeMidpoint) - no Modeling code reads them yet; they arrive with the gizmo port that does.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include <string>

namespace Desert::Geometry
{
    /** A selection of group-topology elements: groups (faces), corners and group edges, by their topology IDs. */
    struct GroupTopologySelection
    {
        std::unordered_set<int32_t> SelectedGroupIDs;
        std::unordered_set<int32_t> SelectedCornerIDs;
        std::unordered_set<int32_t> SelectedEdgeIDs;

        void Clear()
        {
            SelectedGroupIDs.clear();
            SelectedCornerIDs.clear();
            SelectedEdgeIDs.clear();
        }
        [[nodiscard]] bool IsEmpty() const
        {
            return SelectedGroupIDs.empty() && SelectedCornerIDs.empty() && SelectedEdgeIDs.empty();
        }
    };

    class GroupTopology
    {
    public:
        GroupTopology() = default;
        GroupTopology( const DynamicMesh3* Mesh, bool bAutoBuild );
        GroupTopology( const GroupTopology& )            = default;
        GroupTopology& operator=( const GroupTopology& ) = default;
        virtual ~GroupTopology()                         = default;

        [[nodiscard]] const DynamicMesh3* GetMesh() const
        {
            return m_Mesh;
        }

        /** Rebuilds groups, corners and group edges. False if a group's boundary could not be walked; see Failure.
         */
        virtual bool       RebuildTopology();
        [[nodiscard]] const std::string& Failure() const
        {
            return m_FailureReason;
        }

        [[nodiscard]] virtual int GetGroupID( int TriangleID ) const
        {
            return std::max( 0, m_Mesh->GetTriangleGroup( TriangleID ) );
        }

        /** A corner is a mesh vertex where three or more group edges meet (a mesh-border edge counts as one). */
        struct Corner
        {
            int         VertexID = IndexConstants::InvalidID;
            std::vector<int> NeighbourGroupIDs;
        };
        std::vector<Corner> m_Corners;

        /** One closed boundary of a group, as the ordered group edges around it. */
        struct GroupBoundary
        {
            std::vector<int> GroupEdges;
            std::vector<int> NeighbourGroupIDs;
            bool        bIsOnBoundary = false;
        };

        struct Group
        {
            int                    GroupID = 0;
            std::vector<int>           Triangles;
            std::vector<GroupBoundary> Boundaries;
            std::vector<int>           NeighbourGroupIDs;
        };
        std::vector<Group> m_Groups;

        /** The mesh-edge span between two corners (or a closed loop with no corners) shared by two groups. */
        struct GroupEdge
        {
            Index2i  Groups;
            EdgeSpan Span;
            Index2i  EndpointCorners;

            [[nodiscard]] int OtherGroupID( int GroupID ) const
            {
                assert( Groups.A == GroupID || Groups.B == GroupID );
                return ( Groups.A == GroupID ) ? Groups.B : Groups.A;
            }
            [[nodiscard]] bool IsConnectedToVertices( const std::unordered_set<int>& Vertices ) const;
        };
        std::vector<GroupEdge> m_Edges;

        [[nodiscard]] int                     GetCornerVertexID( int CornerID ) const;
        [[nodiscard]] int32_t GetCornerIDFromVertexID( int32_t VertexID ) const;
        [[nodiscard]] const Group*            FindGroupByID( int GroupID ) const;
        [[nodiscard]] const std::vector<int>& GetGroupTriangles( int GroupID ) const;
        [[nodiscard]] const std::vector<int>& GetGroupNbrGroups( int GroupID ) const;
        [[nodiscard]] int                     FindGroupEdgeID( int MeshEdgeID ) const;
        [[nodiscard]] const std::vector<int>& GetGroupEdgeVertices( int GroupEdgeID ) const;
        [[nodiscard]] const std::vector<int>& GetGroupEdgeEdges( int GroupEdgeID ) const;
        void                  FindEdgeNbrGroups( int GroupEdgeID, std::vector<int>& GroupsOut ) const;
        void                  FindEdgeNbrEdges( int GroupEdgeID, std::vector<int>& EdgesOut ) const;
        [[nodiscard]] bool    IsBoundaryEdge( int32_t GroupEdgeID ) const;
        /** @return arc length of edge, and optionally accumulated arclength distances for each edge vertex */
        double GetEdgeArcLength( int32_t GroupEdgeID, std::vector<double>* PerVertexLengthsOut = nullptr ) const;
        [[nodiscard]] bool IsSimpleGroupEdge( int32_t GroupEdgeID ) const;
        [[nodiscard]] bool IsIsolatedLoop( int32_t GroupEdgeID ) const;
        void               FindCornerNbrGroups( int CornerID, std::vector<int>& GroupsOut ) const;
        void               ForCornerNbrEdges( int                                          CornerID,
                                              const std::function<bool( int32_t EdgeID )>& ReturnTrueToContinue ) const;
        void               FindCornerNbrEdges( int CornerID, std::vector<int>& EdgesOut ) const;
        void               FindCornerNbrCorners( int CornerID, std::vector<int>& CornersOut ) const;
        void               FindVertexNbrGroups( int VertexID, std::vector<int>& GroupsOut ) const;
        void               CollectGroupVertices( int GroupID, std::unordered_set<int>& Vertices ) const;
        void               CollectGroupBoundaryVertices( int GroupID, std::unordered_set<int>& Vertices ) const;
        void               GetSelectedTriangles( const GroupTopologySelection& Selection,
                                                 std::vector<int32_t>&         Triangles ) const;

    protected:
        const DynamicMesh3* m_Mesh = nullptr;
        std::vector<int>    m_GroupIDToGroupIndexMap; // fast lookup of the index in Groups, given a GroupID
        std::vector<int>    m_EmptyArray;
        std::unordered_map<int32_t, int32_t> m_VertexIDToCornerIDMap;
        std::string                          m_FailureReason;

        [[nodiscard]] bool    ShouldVertBeCorner( int VertexID ) const;
        bool                  GenerateBoundaryAndGroupEdges( Group&                                Group,
                                                             std::unordered_map<int32_t, int32_t>& GroupEdgeMinEidToGroupEdgeID,
                                                             std::vector<bool>&                    VertCheckedForCorner );
        [[nodiscard]] Index2i MakeEdgeGroupsPair( int MeshEdgeID ) const;
        void                  GetAllVertexGroups( int32_t VertexID, std::vector<int32_t>& GroupsOut ) const;
    };

    /** Every triangle is its own group: corners are all vertices, group edges are all mesh edges. */
    class TriangleGroupTopology : public GroupTopology
    {
    public:
        TriangleGroupTopology() = default;
        TriangleGroupTopology( const DynamicMesh3* Mesh, bool bAutoBuild );
        bool RebuildTopology() override;
        [[nodiscard]] int GetGroupID( int TriangleID ) const override
        {
            return TriangleID;
        }
    };
} // namespace Desert::Geometry
