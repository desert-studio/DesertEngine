// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/IndexPriorityQueue.h, adapted: a binary
// min-heap stored 1-based in a flat TArray with an ID -> heap index map (UE's node pool and FDynamicVector are not
// needed); Initialize, Clear, Contains, Insert, Update, Dequeue, GetCount only.
#pragma once


#include <cstdint>
#include <utility>
#include <vector>

namespace Desert::Geometry
{
    class IndexPriorityQueue
    {
    public:
        void Initialize( int MaxNodeID )
        {
            m_Nodes.clear();
            m_Nodes.push_back( {} ); // [0] unused, as UE
            m_IdToIndex.assign( MaxNodeID, 0 );
        }
        void Clear()
        {
            for ( int Index = 1; Index < static_cast<int32_t>( m_Nodes.size() ); ++Index )
                m_IdToIndex[m_Nodes[Index].Id] = 0;
            m_Nodes.clear();
            m_Nodes.push_back( {} );
        }
        [[nodiscard]] int GetCount() const
        {
            return static_cast<int32_t>( m_Nodes.size() ) - 1;
        }
        [[nodiscard]] bool Contains( int NodeID ) const
        {
            return NodeID >= 0 && NodeID < static_cast<int32_t>( m_IdToIndex.size() ) && m_IdToIndex[NodeID] > 0;
        }
        void Insert( int NodeID, float Priority )
        {
            m_Nodes.push_back( { NodeID, Priority } );
            const int Index     = static_cast<int32_t>( m_Nodes.size() ) - 1;
            m_IdToIndex[NodeID] = Index;
            MoveUp( Index );
        }
        void Update( int NodeID, float Priority )
        {
            const int Index         = m_IdToIndex[NodeID];
            m_Nodes[Index].Priority = Priority;
            MoveUp( Index );
            MoveDown( m_IdToIndex[NodeID] );
        }
        int Dequeue()
        {
            const int Head = m_Nodes[1].Id;
            const int Last = GetCount();
            Swap( 1, Last );
            m_Nodes.erase( m_Nodes.begin() + Last );
            m_IdToIndex[Head] = 0;
            if ( GetCount() > 0 )
                MoveDown( 1 );
            return Head;
        }

    private:
        struct Node
        {
            int   Id       = -1;
            float Priority = 0;
        };
        void Swap( int A, int B )
        {
            std::swap( m_Nodes[A], m_Nodes[B] );
            m_IdToIndex[m_Nodes[A].Id] = A;
            m_IdToIndex[m_Nodes[B].Id] = B;
        }
        void MoveUp( int Index )
        {
            while ( Index > 1 && m_Nodes[Index / 2].Priority > m_Nodes[Index].Priority )
            {
                Swap( Index, Index / 2 );
                Index /= 2;
            }
        }
        void MoveDown( int Index )
        {
            const int Count = GetCount();
            for ( ;; )
            {
                int       Smallest = Index;
                const int Left     = 2 * Index;
                const int Right    = 2 * Index + 1;
                if ( Left <= Count && m_Nodes[Left].Priority < m_Nodes[Smallest].Priority )
                    Smallest = Left;
                if ( Right <= Count && m_Nodes[Right].Priority < m_Nodes[Smallest].Priority )
                    Smallest = Right;
                if ( Smallest == Index )
                    return;
                Swap( Index, Smallest );
                Index = Smallest;
            }
        }
        std::vector<Node> m_Nodes;
        std::vector<int>  m_IdToIndex;
    };
} // namespace Desert::Geometry
