// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Parameterization/MeshDijkstra.h (constructor,
// SeedPoint, bEnableDistanceWeighting + GetWeightedDistanceFunc, ComputeToMaxDistance, ComputeToTargetPoint,
// FindPathToNearestSeed, GetMaxGraphDistance(PointID), GetNodeForPointSetID, UpdateNeighboursSparse), adapted:
// nodes live in a TArray and are addressed by index (UE holds GraphNode* across appends, which its chunked
// DynamicVector keeps stable and a TArray would not); GetPositionFunc is not ported (every caller here reads the
// point set's own positions). ComputeToTargetPoint and FindPathToNearestSeed serve the UV Seams paths.
#pragma once

#include "Engine/Geometry/MeshCore/IndexPriorityQueue.hpp"
#include "Engine/Geometry/MeshCore/MapLookup.hpp"
#include "Engine/Geometry/MeshCore/VectorTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Desert::Geometry
{
    template <class PointSetType>
    class MeshDijkstra
    {
    public:
        struct SeedPoint
        {
            int32_t ExternalID    = -1;
            int32_t PointID       = 0;
            double  StartDistance = 0;
        };

        /** If enabled, GetWeightedDistanceFunc is called with each neighbour pair's Euclidean distance, to allow
         *  for alternative metrics (UE MeshDijkstra.h:38-48). */
        bool bEnableDistanceWeighting = false;
        /** (FromVID, ToVID, SeedVID: the seed FromVID's value was propagated from, EuclideanDistance) -> distance
         */
        std::function<double( int32_t FromVID, int32_t ToVID, int32_t SeedVID, double EuclideanDistance )>
             GetWeightedDistanceFunc = []( int32_t, int32_t, int32_t, double EuclideanDistance )
        { return EuclideanDistance; };

        explicit MeshDijkstra( const PointSetType* PointSetIn ) : m_PointSet( PointSetIn )
        {
            m_Queue.Initialize( m_PointSet->MaxVertexID() );
        }

        void ComputeToMaxDistance( const std::vector<SeedPoint>& SeedPointsIn, double ComputeToMaxDistanceIn )
        {
            QueueSeedPoints( SeedPointsIn );
            while ( m_Queue.GetCount() > 0 )
            {
                const int32_t NodeIndex = GetNodeIndex( m_Queue.Dequeue(), false );
                GraphNode&    Node      = m_AllocatedNodes[NodeIndex];
                m_MaxGraphDistance      = std::max<double>( Node.GraphDistance, m_MaxGraphDistance );
                if ( m_MaxGraphDistance > ComputeToMaxDistanceIn )
                    return;
                Node.bFrozen              = true;
                m_MaxGraphDistancePointID = Node.PointID;
                UpdateNeighboursSparse( NodeIndex );
            }
        }

        /** Grows from the seeds until TargetPointID is frozen (true), or until the queue empties or the graph
         *  distance passes ComputeToMaxDistanceIn first (false). */
        bool ComputeToTargetPoint( const std::vector<SeedPoint>& SeedPointsIn, int32_t TargetPointID,
                                   double ComputeToMaxDistanceIn = std::numeric_limits<double>::max() )
        {
            QueueSeedPoints( SeedPointsIn );
            while ( m_Queue.GetCount() > 0 )
            {
                const int32_t NodeIndex = GetNodeIndex( m_Queue.Dequeue(), false );
                GraphNode&    Node      = m_AllocatedNodes[NodeIndex];
                m_MaxGraphDistance      = std::max<double>( Node.GraphDistance, m_MaxGraphDistance );
                if ( m_MaxGraphDistance > ComputeToMaxDistanceIn )
                    return false;
                Node.bFrozen              = true;
                m_MaxGraphDistancePointID = Node.PointID;
                if ( Node.PointID == TargetPointID )
                    return true;
                UpdateNeighboursSparse( NodeIndex );
            }
            return false;
        }

        /** The parent chain from PointID (computed and frozen) back to its seed: PointID first, the seed last.
         *  False with an empty path when PointID or a node on the chain was not reached. UE returns the chain cut
         *  at MaxLength as found; here a chain longer than MaxLength is refused (false, empty). */
        bool FindPathToNearestSeed( int32_t PointID, std::vector<int32_t>& PathToSeedOut,
                                    int32_t MaxLength = 100000 )
        {
            PathToSeedOut.clear();
            int32_t NodeIndex = GetNodeIndex( PointID, false );
            if ( NodeIndex < 0 || !m_AllocatedNodes[NodeIndex].bFrozen )
                return false;
            PathToSeedOut.push_back( PointID );
            for ( int32_t IterCount = 0; IterCount < MaxLength; ++IterCount )
            {
                const int32_t ParentID = m_AllocatedNodes[NodeIndex].ParentPointID;
                if ( ParentID == -1 )
                    return true;
                PathToSeedOut.push_back( ParentID );
                NodeIndex = GetNodeIndex( ParentID, false );
                if ( NodeIndex < 0 || !m_AllocatedNodes[NodeIndex].bFrozen )
                    break;
            }
            PathToSeedOut.clear();
            return false;
        }

        [[nodiscard]] double GetMaxGraphDistance() const
        {
            return m_MaxGraphDistance;
        }
        [[nodiscard]] int32_t GetMaxGraphDistancePointID() const
        {
            return m_MaxGraphDistancePointID;
        }

    private:
        struct GraphNode
        {
            int32_t PointID;
            int32_t ParentPointID;
            int32_t SeedPointID;
            double  GraphDistance;
            bool    bFrozen;
        };
        const PointSetType*                  m_PointSet;
        std::unordered_map<int32_t, int32_t> m_IDToNodeIndexMap;
        std::vector<GraphNode>               m_AllocatedNodes;
        IndexPriorityQueue                   m_Queue;
        std::vector<SeedPoint>               m_SeedPoints;
        double                               m_MaxGraphDistance        = 0.0;
        int32_t                              m_MaxGraphDistancePointID = -1;

        // The seeding both Compute* share (UE repeats it in each): every seed frozen at its StartDistance, a
        // repeated seed skipped (UE ensure()s on it); the seeds are kept for the distance weighting's SeedVID.
        void QueueSeedPoints( const std::vector<SeedPoint>& SeedPointsIn )
        {
            m_MaxGraphDistance        = 0.0;
            m_MaxGraphDistancePointID = -1;
            m_SeedPoints              = SeedPointsIn;
            for ( int32_t SeedIndex = 0; SeedIndex < static_cast<int32_t>( SeedPointsIn.size() ); ++SeedIndex )
            {
                const int32_t PointID = SeedPointsIn[SeedIndex].PointID;
                if ( m_Queue.Contains( PointID ) )
                    continue;
                GraphNode& Node    = m_AllocatedNodes[GetNodeIndex( PointID, true )];
                Node.GraphDistance = SeedPointsIn[SeedIndex].StartDistance;
                Node.bFrozen       = true;
                Node.SeedPointID   = SeedIndex;
                m_Queue.Insert( PointID, float( Node.GraphDistance ) );
            }
        }

        int32_t GetNodeIndex( int32_t PointSetID, bool bCreateIfMissing )
        {
            if ( const int32_t* Found = FindValue( m_IDToNodeIndexMap, PointSetID ) )
                return *Found;
            if ( !bCreateIfMissing )
                return -1;
            m_AllocatedNodes.push_back( GraphNode{ PointSetID, -1, 0, 0.0, false } );
            const int32_t NewIndex = static_cast<int32_t>( m_AllocatedNodes.size() ) - 1;
            m_IDToNodeIndexMap.insert_or_assign( PointSetID, NewIndex );
            return NewIndex;
        }
        void UpdateNeighboursSparse( int32_t ParentIndex )
        {
            const int32_t    ParentID   = m_AllocatedNodes[ParentIndex].PointID;
            const int32_t    ParentSeed = m_AllocatedNodes[ParentIndex].SeedPointID;
            const double     ParentDist = m_AllocatedNodes[ParentIndex].GraphDistance;
            const glm::dvec3 ParentPos  = m_PointSet->GetVertex( ParentID );
            for ( const int32_t NbrPointID : m_PointSet->VtxVerticesItr( ParentID ) )
            {
                GraphNode& Nbr = m_AllocatedNodes[GetNodeIndex( NbrPointID, true )];
                if ( Nbr.bFrozen )
                    continue;
                double LocalDist = Distance( ParentPos, m_PointSet->GetVertex( NbrPointID ) );
                if ( bEnableDistanceWeighting )
                    LocalDist = GetWeightedDistanceFunc( ParentID, NbrPointID, m_SeedPoints[ParentSeed].PointID,
                                                         LocalDist );
                const double NbrDist = ParentDist + LocalDist;
                if ( m_Queue.Contains( NbrPointID ) )
                {
                    if ( NbrDist < Nbr.GraphDistance )
                    {
                        Nbr.ParentPointID = ParentID;
                        Nbr.GraphDistance = NbrDist;
                        Nbr.SeedPointID   = ParentSeed;
                        m_Queue.Update( NbrPointID, static_cast<float>( NbrDist ) );
                    }
                }
                else
                {
                    Nbr.ParentPointID = ParentID;
                    Nbr.GraphDistance = NbrDist;
                    Nbr.SeedPointID   = ParentSeed;
                    m_Queue.Insert( NbrPointID, static_cast<float>( NbrDist ) );
                }
            }
        }
    };
} // namespace Desert::Geometry
