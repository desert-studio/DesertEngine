// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Parameterization/MeshDijkstra.h (constructor,
// FSeedPoint, ComputeToMaxDistance, GetMaxGraphDistance(PointID), GetNodeForPointSetID, UpdateNeighboursSparse),
// adapted: nodes live in a TArray and are addressed by index (UE holds FGraphNode* across appends, which its
// chunked TDynamicVector keeps stable and a TArray would not); no distance weighting, target or path queries.
#pragma once

#include "Engine/Geometry/UECore/IndexPriorityQueue.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    template <class PointSetType>
    class TMeshDijkstra
    {
    public:
        struct FSeedPoint
        {
            int32  ExternalID    = -1;
            int32  PointID       = 0;
            double StartDistance = 0;
        };

        explicit TMeshDijkstra( const PointSetType* PointSetIn ) : PointSet( PointSetIn )
        {
            Queue.Initialize( PointSet->MaxVertexID() );
        }

        void ComputeToMaxDistance( const TArray<FSeedPoint>& SeedPointsIn, double ComputeToMaxDistanceIn )
        {
            MaxGraphDistance        = 0.0;
            MaxGraphDistancePointID = -1;
            for ( int32 SeedIndex = 0; SeedIndex < SeedPointsIn.Num(); ++SeedIndex )
            {
                const int32 PointID = SeedPointsIn[SeedIndex].PointID;
                if ( Queue.Contains( PointID ) )
                    continue; // UE ensure()s on a repeated seed and skips it
                FGraphNode& Node   = AllocatedNodes[GetNodeIndex( PointID, true )];
                Node.GraphDistance = SeedPointsIn[SeedIndex].StartDistance;
                Node.bFrozen       = true;
                Node.SeedPointID   = SeedIndex;
                Queue.Insert( PointID, float( Node.GraphDistance ) );
            }
            while ( Queue.GetCount() > 0 )
            {
                const int32 NodeIndex = GetNodeIndex( Queue.Dequeue(), false );
                FGraphNode& Node      = AllocatedNodes[NodeIndex];
                MaxGraphDistance      = TMathUtil<double>::Max( Node.GraphDistance, MaxGraphDistance );
                if ( MaxGraphDistance > ComputeToMaxDistanceIn )
                    return;
                Node.bFrozen            = true;
                MaxGraphDistancePointID = Node.PointID;
                UpdateNeighboursSparse( NodeIndex );
            }
        }
        [[nodiscard]] double GetMaxGraphDistance() const
        {
            return MaxGraphDistance;
        }
        [[nodiscard]] int32 GetMaxGraphDistancePointID() const
        {
            return MaxGraphDistancePointID;
        }

    private:
        struct FGraphNode
        {
            int32  PointID;
            int32  ParentPointID;
            int32  SeedPointID;
            double GraphDistance;
            bool   bFrozen;
        };
        const PointSetType* PointSet;
        TMap<int32, int32>  IDToNodeIndexMap;
        TArray<FGraphNode>  AllocatedNodes;
        FIndexPriorityQueue Queue;
        double              MaxGraphDistance        = 0.0;
        int32               MaxGraphDistancePointID = -1;

        int32 GetNodeIndex( int32 PointSetID, bool bCreateIfMissing )
        {
            if ( const int32* Found = IDToNodeIndexMap.Find( PointSetID ) )
                return *Found;
            if ( !bCreateIfMissing )
                return -1;
            const int32 NewIndex = AllocatedNodes.Add( FGraphNode{ PointSetID, -1, 0, 0.0, false } );
            IDToNodeIndexMap.Add( PointSetID, NewIndex );
            return NewIndex;
        }
        void UpdateNeighboursSparse( int32 ParentIndex )
        {
            const int32     ParentID   = AllocatedNodes[ParentIndex].PointID;
            const int32     ParentSeed = AllocatedNodes[ParentIndex].SeedPointID;
            const double    ParentDist = AllocatedNodes[ParentIndex].GraphDistance;
            const FVector3d ParentPos  = PointSet->GetVertex( ParentID );
            for ( const int32 NbrPointID : PointSet->VtxVerticesItr( ParentID ) )
            {
                FGraphNode& Nbr = AllocatedNodes[GetNodeIndex( NbrPointID, true )];
                if ( Nbr.bFrozen )
                    continue;
                const double NbrDist = ParentDist + Distance( ParentPos, PointSet->GetVertex( NbrPointID ) );
                if ( Queue.Contains( NbrPointID ) )
                {
                    if ( NbrDist < Nbr.GraphDistance )
                    {
                        Nbr.ParentPointID = ParentID;
                        Nbr.GraphDistance = NbrDist;
                        Nbr.SeedPointID   = ParentSeed;
                        Queue.Update( NbrPointID, static_cast<float>( NbrDist ) );
                    }
                }
                else
                {
                    Nbr.ParentPointID = ParentID;
                    Nbr.GraphDistance = NbrDist;
                    Nbr.SeedPointID   = ParentSeed;
                    Queue.Insert( NbrPointID, static_cast<float>( NbrDist ) );
                }
            }
        }
    };
} // namespace Desert::Geometry
