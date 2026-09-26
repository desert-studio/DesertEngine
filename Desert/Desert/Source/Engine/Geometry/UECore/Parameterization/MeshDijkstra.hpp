// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Parameterization/MeshDijkstra.h (constructor,
// FSeedPoint, ComputeToMaxDistance, GetMaxGraphDistance(PointID), GetNodeForPointSetID, UpdateNeighboursSparse),
// adapted: nodes live in a TArray and are addressed by index (UE holds FGraphNode* across appends, which its
// chunked TDynamicVector keeps stable and a TArray would not); no distance weighting, target or path queries.
#pragma once

#include "Engine/Geometry/UECore/IndexPriorityQueue.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    template <class PointSetType>
    class TMeshDijkstra
    {
    public:
        struct FSeedPoint
        {
            int32_t ExternalID    = -1;
            int32_t PointID       = 0;
            double StartDistance = 0;
        };

        explicit TMeshDijkstra( const PointSetType* PointSetIn ) : PointSet( PointSetIn )
        {
            Queue.Initialize( PointSet->MaxVertexID() );
        }

        void ComputeToMaxDistance( const std::vector<FSeedPoint>& SeedPointsIn, double ComputeToMaxDistanceIn )
        {
            MaxGraphDistance        = 0.0;
            MaxGraphDistancePointID = -1;
            for ( int32_t SeedIndex = 0; SeedIndex < static_cast<int32_t>( SeedPointsIn.size() ); ++SeedIndex )
            {
                const int32_t PointID = SeedPointsIn[SeedIndex].PointID;
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
                const int32_t NodeIndex = GetNodeIndex( Queue.Dequeue(), false );
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
        [[nodiscard]] int32_t GetMaxGraphDistancePointID() const
        {
            return MaxGraphDistancePointID;
        }

    private:
        struct FGraphNode
        {
            int32_t PointID;
            int32_t ParentPointID;
            int32_t SeedPointID;
            double GraphDistance;
            bool   bFrozen;
        };
        const PointSetType* PointSet;
        std::unordered_map<int32_t, int32_t> IDToNodeIndexMap;
        std::vector<FGraphNode>              AllocatedNodes;
        FIndexPriorityQueue Queue;
        double              MaxGraphDistance        = 0.0;
        int32_t                MaxGraphDistancePointID = -1;

        int32_t GetNodeIndex( int32_t PointSetID, bool bCreateIfMissing )
        {
            if ( const int32_t* Found = FindValue( IDToNodeIndexMap, PointSetID ) )
                return *Found;
            if ( !bCreateIfMissing )
                return -1;
            AllocatedNodes.push_back( FGraphNode{ PointSetID, -1, 0, 0.0, false } );
            const int32_t NewIndex = static_cast<int32_t>( AllocatedNodes.size() ) - 1;
            IDToNodeIndexMap.insert_or_assign( PointSetID, NewIndex );
            return NewIndex;
        }
        void UpdateNeighboursSparse( int32_t ParentIndex )
        {
            const int32_t   ParentID   = AllocatedNodes[ParentIndex].PointID;
            const int32_t   ParentSeed = AllocatedNodes[ParentIndex].SeedPointID;
            const double    ParentDist = AllocatedNodes[ParentIndex].GraphDistance;
            const glm::dvec3 ParentPos  = PointSet->GetVertex( ParentID );
            for ( const int32_t NbrPointID : PointSet->VtxVerticesItr( ParentID ) )
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
