// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/GroupTopology.h:18-568,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry; FGroupEdge::Span is FEdgeSpan from
// MeshRegionBoundaryLoops.hpp; RebuildTopology returns the region-loop failure by name. Not ported: the
// extra-corner hook (ShouldAddExtraCornerAtVert, RebuildTopologyWithSpecificExtraCorners), the FMeshTriEdgeID
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
    struct FGroupTopologySelection
    {
        TSet<int32_t> SelectedGroupIDs;
        TSet<int32_t> SelectedCornerIDs;
        TSet<int32_t> SelectedEdgeIDs;

        void Clear()
        {
            SelectedGroupIDs.Reset();
            SelectedCornerIDs.Reset();
            SelectedEdgeIDs.Reset();
        }
        bool IsEmpty() const
        {
            return SelectedGroupIDs.Num() == 0 && SelectedCornerIDs.Num() == 0 && SelectedEdgeIDs.Num() == 0;
        }
    };

    class FGroupTopology
    {
    public:
        FGroupTopology() = default;
        FGroupTopology( const FDynamicMesh3* Mesh, bool bAutoBuild );
        FGroupTopology( const FGroupTopology& )            = default;
        FGroupTopology& operator=( const FGroupTopology& ) = default;
        virtual ~FGroupTopology()                          = default;

        const FDynamicMesh3* GetMesh() const
        {
            return Mesh;
        }

        /** Rebuilds groups, corners and group edges. False if a group's boundary could not be walked; see Failure.
         */
        virtual bool       RebuildTopology();
        const std::string& Failure() const
        {
            return FailureReason;
        }

        virtual int GetGroupID( int TriangleID ) const
        {
            return std::max( 0, Mesh->GetTriangleGroup( TriangleID ) );
        }

        /** A corner is a mesh vertex where three or more group edges meet (a mesh-border edge counts as one). */
        struct FCorner
        {
            int         VertexID = IndexConstants::InvalidID;
            TArray<int> NeighbourGroupIDs;
        };
        TArray<FCorner> Corners;

        /** One closed boundary of a group, as the ordered group edges around it. */
        struct FGroupBoundary
        {
            TArray<int> GroupEdges;
            TArray<int> NeighbourGroupIDs;
            bool        bIsOnBoundary = false;
        };

        struct FGroup
        {
            int                    GroupID = 0;
            TArray<int>            Triangles;
            TArray<FGroupBoundary> Boundaries;
            TArray<int>            NeighbourGroupIDs;
        };
        TArray<FGroup> Groups;

        /** The mesh-edge span between two corners (or a closed loop with no corners) shared by two groups. */
        struct FGroupEdge
        {
            FIndex2i  Groups;
            FEdgeSpan Span;
            FIndex2i  EndpointCorners;

            int OtherGroupID( int GroupID ) const
            {
                UE_CHECK( Groups.A == GroupID || Groups.B == GroupID );
                return ( Groups.A == GroupID ) ? Groups.B : Groups.A;
            }
            bool IsConnectedToVertices( const TSet<int>& Vertices ) const;
        };
        TArray<FGroupEdge> Edges;

        int                GetCornerVertexID( int CornerID ) const;
        [[nodiscard]] int32_t GetCornerIDFromVertexID( int32_t VertexID ) const;
        const FGroup*      FindGroupByID( int GroupID ) const;
        const TArray<int>& GetGroupTriangles( int GroupID ) const;
        const TArray<int>& GetGroupNbrGroups( int GroupID ) const;
        int                FindGroupEdgeID( int MeshEdgeID ) const;
        const TArray<int>& GetGroupEdgeVertices( int GroupEdgeID ) const;
        const TArray<int>& GetGroupEdgeEdges( int GroupEdgeID ) const;
        void               FindEdgeNbrGroups( int GroupEdgeID, TArray<int>& GroupsOut ) const;
        void               FindEdgeNbrEdges( int GroupEdgeID, TArray<int>& EdgesOut ) const;
        [[nodiscard]] bool    IsBoundaryEdge( int32_t GroupEdgeID ) const;
        /** @return arc length of edge, and optionally accumulated arclength distances for each edge vertex */
        double GetEdgeArcLength( int32_t GroupEdgeID, TArray<double>* PerVertexLengthsOut = nullptr ) const;
        [[nodiscard]] bool IsSimpleGroupEdge( int32_t GroupEdgeID ) const;
        [[nodiscard]] bool IsIsolatedLoop( int32_t GroupEdgeID ) const;
        void               FindCornerNbrGroups( int CornerID, TArray<int>& GroupsOut ) const;
        void               ForCornerNbrEdges( int                                          CornerID,
                                              const std::function<bool( int32_t EdgeID )>& ReturnTrueToContinue ) const;
        void FindCornerNbrEdges( int CornerID, TArray<int>& EdgesOut ) const;
        void FindCornerNbrCorners( int CornerID, TArray<int>& CornersOut ) const;
        void FindVertexNbrGroups( int VertexID, TArray<int>& GroupsOut ) const;
        void CollectGroupVertices( int GroupID, TSet<int>& Vertices ) const;
        void CollectGroupBoundaryVertices( int GroupID, TSet<int>& Vertices ) const;
        void GetSelectedTriangles( const FGroupTopologySelection& Selection, TArray<int32_t>& Triangles ) const;

    protected:
        const FDynamicMesh3* Mesh = nullptr;
        TArray<int>          GroupIDToGroupIndexMap; // fast lookup of the index in Groups, given a GroupID
        TArray<int>          EmptyArray;
        TMap<int32_t, int32_t> VertexIDToCornerIDMap;
        std::string          FailureReason;

        bool     ShouldVertBeCorner( int VertexID ) const;
        bool GenerateBoundaryAndGroupEdges( FGroup& Group, TMap<int32_t, int32_t>& GroupEdgeMinEidToGroupEdgeID,
                                            TArray<bool>& VertCheckedForCorner );
        FIndex2i MakeEdgeGroupsPair( int MeshEdgeID ) const;
        void     GetAllVertexGroups( int32_t VertexID, TArray<int32_t>& GroupsOut ) const;
    };

    /** Every triangle is its own group: corners are all vertices, group edges are all mesh edges. */
    class FTriangleGroupTopology : public FGroupTopology
    {
    public:
        FTriangleGroupTopology() = default;
        FTriangleGroupTopology( const FDynamicMesh3* Mesh, bool bAutoBuild );
        bool RebuildTopology() override;
        int  GetGroupID( int TriangleID ) const override
        {
            return TriangleID;
        }
    };
} // namespace Desert::Geometry
