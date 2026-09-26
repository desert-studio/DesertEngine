// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Parameterization/MeshDijkstra.h (constructor,
// SeedPoint, ComputeToMaxDistance, GetMaxGraphDistance(PointID), GetNodeForPointSetID, UpdateNeighboursSparse),
// adapted: nodes live in a TArray and are addressed by index (UE holds GraphNode* across appends, which its
// chunked DynamicVector keeps stable and a TArray would not); no distance weighting, target or path queries.
#pragma once

#include "Engine/Geometry/MeshCore/IndexPriorityQueue.hpp"
#include "Engine/Geometry/MeshCore/MapLookup.hpp"
#include "Engine/Geometry/MeshCore/VectorTypes.hpp"

#include <algorithm>
#include <cstdint>
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

        explicit MeshDijkstra( const PointSetType* PointSetIn ) : m_PointSet( PointSetIn )
        {
            m_Queue.Initialize( m_PointSet->MaxVertexID() );
        }

        void ComputeToMaxDistance( const std::vector<SeedPoint>& SeedPointsIn, double ComputeToMaxDistanceIn )
        {
            m_MaxGraphDistance        = 0.0;
            m_MaxGraphDistancePointID = -1;
            for ( int32_t SeedIndex = 0; SeedIndex < static_cast<int32_t>( SeedPointsIn.size() ); ++SeedIndex )
            {
                const int32_t PointID = SeedPointsIn[SeedIndex].PointID;
                if ( m_Queue.Contains( PointID ) )
                    continue; // UE ensure()s on a repeated seed and skips it
                GraphNode& Node    = m_AllocatedNodes[GetNodeIndex( PointID, true )];
                Node.GraphDistance = SeedPointsIn[SeedIndex].StartDistance;
                Node.bFrozen       = true;
                Node.SeedPointID   = SeedIndex;
                m_Queue.Insert( PointID, float( Node.GraphDistance ) );
            }
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
        double                               m_MaxGraphDistance        = 0.0;
        int32_t                              m_MaxGraphDistancePointID = -1;

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
                const double NbrDist = ParentDist + Distance( ParentPos, m_PointSet->GetVertex( NbrPointID ) );
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
