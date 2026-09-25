// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/IndexPriorityQueue.h, adapted: a binary
// min-heap stored 1-based in a flat TArray with an ID -> heap index map (UE's node pool and FDynamicVector are not
// needed); Initialize, Clear, Contains, Insert, Update, Dequeue, GetCount only.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include <utility>

namespace Desert::Geometry
{
    class FIndexPriorityQueue
    {
    public:
        void Initialize( int MaxNodeID )
        {
            Nodes.Reset();
            Nodes.Add( {} ); // [0] unused, as UE
            IdToIndex.Init( 0, MaxNodeID );
        }
        void Clear()
        {
            for ( int Index = 1; Index < Nodes.Num(); ++Index )
                IdToIndex[Nodes[Index].Id] = 0;
            Nodes.Reset();
            Nodes.Add( {} );
        }
        [[nodiscard]] int GetCount() const
        {
            return Nodes.Num() - 1;
        }
        [[nodiscard]] bool Contains( int NodeID ) const
        {
            return NodeID >= 0 && NodeID < IdToIndex.Num() && IdToIndex[NodeID] > 0;
        }
        void Insert( int NodeID, float Priority )
        {
            const int Index   = Nodes.Add( { NodeID, Priority } );
            IdToIndex[NodeID] = Index;
            MoveUp( Index );
        }
        void Update( int NodeID, float Priority )
        {
            const int Index       = IdToIndex[NodeID];
            Nodes[Index].Priority = Priority;
            MoveUp( Index );
            MoveDown( IdToIndex[NodeID] );
        }
        int Dequeue()
        {
            const int Head = Nodes[1].Id;
            const int Last = GetCount();
            Swap( 1, Last );
            Nodes.RemoveAt( Last );
            IdToIndex[Head] = 0;
            if ( GetCount() > 0 )
                MoveDown( 1 );
            return Head;
        }

    private:
        struct FNode
        {
            int   Id       = -1;
            float Priority = 0;
        };
        void Swap( int A, int B )
        {
            std::swap( Nodes[A], Nodes[B] );
            IdToIndex[Nodes[A].Id] = A;
            IdToIndex[Nodes[B].Id] = B;
        }
        void MoveUp( int Index )
        {
            while ( Index > 1 && Nodes[Index / 2].Priority > Nodes[Index].Priority )
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
                if ( Left <= Count && Nodes[Left].Priority < Nodes[Smallest].Priority )
                    Smallest = Left;
                if ( Right <= Count && Nodes[Right].Priority < Nodes[Smallest].Priority )
                    Smallest = Right;
                if ( Smallest == Index )
                    return;
                Swap( Index, Smallest );
                Index = Smallest;
            }
        }
        TArray<FNode> Nodes;
        TArray<int>   IdToIndex;
    };
} // namespace Desert::Geometry
