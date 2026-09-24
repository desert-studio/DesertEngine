// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Util/SmallListSet.cpp:1-483, adapted: namespace
// Desert::Geometry, UE_CHECK* asserts, FArchive Serialize (484-619) not ported.
#include "Engine/Geometry/UECore/SmallListSet.hpp"

namespace Desert::Geometry
{

    void FSmallListSet::Resize( int32 NewSize )
    {
        int32 CurSize = (int32)ListHeads.GetLength();
        if ( NewSize > CurSize )
        {
            ListHeads.Resize( NewSize );
            for ( int32 k = CurSize; k < NewSize; ++k )
            {
                ListHeads[k] = NullValue;
            }
        }
    }

    void FSmallListSet::ResizeAndAllocateBlocks( int32 NewSize )
    {
        Reset();
        Resize( NewSize );
        ListBlocks.Resize( NewSize * ( BLOCK_LIST_OFFSET + 1 ) );
        for ( int32 i( 0 ); i < NewSize; ++i )
        {
            int32 ListHead                           = i * ( BLOCK_LIST_OFFSET + 1 );
            ListHeads[i]                             = ListHead;
            ListBlocks[ListHead]                     = 0;
            ListBlocks[ListHead + BLOCK_LIST_OFFSET] = NullValue;
        }

        for ( int32 i( 0 ); i < NewSize; ++i )
        {
            UE_CHECK_SLOW( ListBlocks[ListHeads[i]] == 0 );
            UE_CHECK_SLOW( ListBlocks[ListHeads[i] + BLOCK_LIST_OFFSET] == NullValue );
        }

        AllocatedCount = NewSize;
    }

    void FSmallListSet::AllocateAt( int32 ListIndex )
    {
        UE_CHECK_SLOW( ListIndex >= 0 );
        if ( ListIndex >= (int)ListHeads.GetLength() )
        {
            int32 j = (int32)ListHeads.GetLength();
            ListHeads.InsertAt( NullValue, ListIndex );
            // need to set intermediate values to null!
            while ( j < ListIndex )
            {
                ListHeads[j] = NullValue;
                j++;
            }
        }
        else
        {
            UE_CHECKF( ListHeads[ListIndex] == NullValue, "FSmallListSet: list at %d is not empty!", ListIndex );
        }
    }

    void FSmallListSet::Compact( int32 MaxListIndex )
    {
        UE_CHECK_SLOW( MaxListIndex >= 0 );
        int32 CurSize = (int32)ListHeads.GetLength();
        if ( MaxListIndex < CurSize )
        {
            // We just resize w/out book-keeping what we cleared, since we rebuild the blocks/etc below
            ListHeads.Resize( MaxListIndex );
        }

        AllocatedCount = 0;
        TDynamicVector<int32> NewBlocks{};
        TDynamicVector<int32> NewLinkedListElements{};
        for ( int32 Idx = 0, Num = (int32)ListHeads.GetLength(), CurBlockIdx = 0; Idx < Num;
              ++Idx, CurBlockIdx += BLOCK_LIST_OFFSET + 1 )
        {
            int32 OrigHead = ListHeads[Idx];
            if ( OrigHead == NullValue )
            {
                continue;
            }
            AllocatedCount++;
            ListHeads[Idx] = CurBlockIdx;
            NewBlocks.InsertAt( NullValue, CurBlockIdx + BLOCK_LIST_OFFSET );
            for ( int32 SubIdx = 0; SubIdx < BLOCK_LIST_OFFSET; ++SubIdx )
            {
                NewBlocks[CurBlockIdx + SubIdx] = ListBlocks[OrigHead + SubIdx];
            }

            int32 OrigLinkStart = ListBlocks[OrigHead + BLOCK_LIST_OFFSET];
            if ( OrigLinkStart == NullValue )
            {
                NewBlocks[CurBlockIdx + BLOCK_LIST_OFFSET] = NullValue;
            }
            else
            {
                int32 CurPtr                               = OrigLinkStart;
                NewBlocks[CurBlockIdx + BLOCK_LIST_OFFSET] = NewLinkedListElements.GetLength();
                while ( true )
                {
                    NewLinkedListElements.Add( LinkedListElements[CurPtr] );
                    CurPtr = LinkedListElements[CurPtr + 1];
                    if ( CurPtr == NullValue )
                    {
                        break;
                    }
                    NewLinkedListElements.Add( NewLinkedListElements.GetLength() + 1 );
                }
                NewLinkedListElements.Add( NullValue );
            }
        }

        ListBlocks         = MoveTemp( NewBlocks );
        LinkedListElements = MoveTemp( NewLinkedListElements );
        FreeHeadIndex      = NullValue;
        FreeBlocks.Clear();
    }

    void FSmallListSet::AppendWithElementOffset( const FSmallListSet& Other, int32 ElementOffset )
    {
        int32 OrigListBlocksNum         = ListBlocks.Num();
        int32 OrigListHeadsNum          = ListHeads.Num();
        int32 OrigLinkedListElementsNum = LinkedListElements.Num();
        int32 OrigFreeHeadIndex         = FreeHeadIndex;

        // Append ListHeads indices
        ListHeads.Add( Other.ListHeads );
        for ( int32 Idx = OrigListHeadsNum, N = ListHeads.Num(); Idx < N; ++Idx )
        {
            // Offset appended non-null indices to point to appended ListBlock indices
            if ( ListHeads[Idx] != NullValue )
            {
                ListHeads[Idx] += (int32)OrigListBlocksNum;
            }
        }

        // Append LinkedListElements entries
        LinkedListElements.Add( Other.LinkedListElements );
        for ( int32 Idx = OrigLinkedListElementsNum, N = LinkedListElements.Num(); Idx < N; Idx += 2 )
        {
            // Offset the element data
            LinkedListElements[Idx] += ElementOffset;
            // Offset appended non-null pointer indices to refer to appended indices
            int32& Link = LinkedListElements[Idx + 1];
            if ( Link != NullValue )
            {
                Link += OrigLinkedListElementsNum;
            }
        }

        // Append ListBlocks entries
        ListBlocks.Add( Other.ListBlocks );
        for ( int32 Idx = OrigListBlocksNum, N = ListBlocks.Num(); Idx < N; Idx += BLOCKSIZE + 2 )
        {
            int32 BlockNumEls = ListBlocks[Idx];
            for ( int32 SubIdx = 0, SubIdxNum = FMath::Min( BLOCKSIZE, BlockNumEls ); SubIdx < SubIdxNum;
                  ++SubIdx )
            {
                ListBlocks[Idx + 1 + SubIdx] += ElementOffset;
            }
            if ( ListBlocks[Idx + BLOCKSIZE + 1] != NullValue )
            {
                ListBlocks[Idx + BLOCKSIZE + 1] += OrigLinkedListElementsNum;
            }
        }

        // If we had a non-empty free list on Other, need to transfer it too
        if ( Other.FreeHeadIndex != NullValue )
        {
            FreeHeadIndex = Other.FreeHeadIndex + OrigLinkedListElementsNum;
            // If both were non-empty, need to walk Other's free list to attach its tail to our original head
            if ( OrigFreeHeadIndex != NullValue )
            {
                int32 WalkListIndex = FreeHeadIndex;
                while ( true )
                {
                    int32 NextIndex = LinkedListElements[WalkListIndex + 1];
                    if ( NextIndex == NullValue )
                    {
                        break;
                    }
                    WalkListIndex = NextIndex;
                }
                UE_CHECK_SLOW( LinkedListElements[WalkListIndex + 1] == NullValue );
                LinkedListElements[WalkListIndex + 1] = OrigFreeHeadIndex;
            }
        }

        AllocatedCount += Other.AllocatedCount;
    }

    void FSmallListSet::Insert( int32 ListIndex, int32 Value )
    {
        UE_CHECK_SLOW( 0 <= ListIndex && ListIndex < (int32)ListHeads.Num() );
        int32 block_ptr = ListHeads[ListIndex];
        if ( block_ptr == NullValue )
        {
            block_ptr             = AllocateBlock();
            ListBlocks[block_ptr] = 0;
            ListHeads[ListIndex]  = block_ptr;
        }

        int32 N = ListBlocks[block_ptr];
        if ( N < BLOCKSIZE )
        {
            ListBlocks[block_ptr + N + 1] = Value;
        }
        else
        {
            // spill to linked list
            int32 cur_head = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];

            if ( FreeHeadIndex == NullValue )
            {
                // allocate linkedlist node
                int32 new_ptr = (int32)LinkedListElements.GetLength();
                LinkedListElements.Add( Value );
                LinkedListElements.Add( cur_head );
                ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = new_ptr;
            }
            else
            {
                // pull from free list
                int32 free_ptr                            = FreeHeadIndex;
                FreeHeadIndex                             = LinkedListElements[free_ptr + 1];
                LinkedListElements[free_ptr]              = Value;
                LinkedListElements[free_ptr + 1]          = cur_head;
                ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = free_ptr;
            }
        }

        // count element
        ListBlocks[block_ptr] += 1;
    }

    bool FSmallListSet::Remove( int32 ListIndex, int32 Value )
    {
        UE_CHECK_SLOW( ListIndex >= 0 );
        int32 block_ptr = ListHeads[ListIndex];
        int32 N         = ListBlocks[block_ptr];

        int32 iEnd = block_ptr + FMath::Min( N, BLOCKSIZE );
        for ( int32 i = block_ptr + 1; i <= iEnd; ++i )
        {

            if ( ListBlocks[i] == Value )
            {
                // Shifting (rather than moving the last element into the hole) keeps UE's list order.
                for ( int32 j = i + 1; j <= iEnd; ++j ) // shift left
                {
                    ListBlocks[j - 1] = ListBlocks[j];
                }
                // ListBlocks[iEnd] = -2;     // OPTIONAL

                if ( N > BLOCKSIZE )
                {
                    int32 cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    ListBlocks[block_ptr + BLOCK_LIST_OFFSET] =
                         LinkedListElements[cur_ptr + 1]; // point32 to cur->next
                    ListBlocks[iEnd] = LinkedListElements[cur_ptr];
                    AddFreeLink( cur_ptr );
                }

                ListBlocks[block_ptr] -= 1;
                return true;
            }
        }

        // search list
        if ( N > BLOCKSIZE )
        {
            if ( RemoveFromLinkedList( block_ptr, Value ) )
            {
                ListBlocks[block_ptr] -= 1;
                return true;
            }
        }

        return false;
    }

    void FSmallListSet::Move( int32 FromIndex, int32 ToIndex )
    {
        UE_CHECK_SLOW( FromIndex >= 0 );
        UE_CHECK_SLOW( ToIndex >= 0 );
        UE_CHECK_SLOW( ListHeads[ToIndex] == NullValue );
        ListHeads[ToIndex]   = ListHeads[FromIndex];
        ListHeads[FromIndex] = NullValue;
    }

    void FSmallListSet::Clear( int32 ListIndex )
    {
        UE_CHECK_SLOW( ListIndex >= 0 );
        int32 block_ptr = ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32 N = ListBlocks[block_ptr];

            // if we have spilled to linked-list, free nodes
            if ( N > BLOCKSIZE )
            {
                int32 cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                while ( cur_ptr != NullValue )
                {
                    int32 free_ptr = cur_ptr;
                    cur_ptr        = LinkedListElements[cur_ptr + 1];
                    AddFreeLink( free_ptr );
                }
                ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = NullValue;
            }

            // free our block
            ListBlocks[block_ptr] = 0;
            FreeBlocks.Add( block_ptr );
            ListHeads[ListIndex] = NullValue;
        }
    }

    bool FSmallListSet::Contains( int32 ListIndex, int32 Value ) const
    {
        UE_CHECK_SLOW( ListIndex >= 0 );
        int32 block_ptr = ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32 N = ListBlocks[block_ptr];
            if ( N < BLOCKSIZE )
            {
                int32 iEnd = block_ptr + N;
                for ( int32 i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( ListBlocks[i] == Value )
                    {
                        return true;
                    }
                }
            }
            else
            {
                // we spilled to linked list, have to iterate through it as well
                int32 iEnd = block_ptr + BLOCKSIZE;
                for ( int32 i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( ListBlocks[i] == Value )
                    {
                        return true;
                    }
                }
                int32 cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                while ( cur_ptr != NullValue )
                {
                    if ( LinkedListElements[cur_ptr] == Value )
                    {
                        return true;
                    }
                    cur_ptr = LinkedListElements[cur_ptr + 1];
                }
            }
        }
        return false;
    }

    bool FSmallListSet::EnumerateEarlyOut( int32 ListIndex, TFunctionRef<bool( int32 )> ApplyFunc ) const
    {
        int32 block_ptr = ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32 N = ListBlocks[block_ptr];
            if ( N < BLOCKSIZE )
            {
                int32 iEnd = block_ptr + N;
                for ( int32 i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( !ApplyFunc( ListBlocks[i] ) )
                    {
                        return false;
                    }
                }
            }
            else
            {
                // we spilled to linked list, have to iterate through it as well
                int32 iEnd = block_ptr + BLOCKSIZE;
                for ( int32 i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( !ApplyFunc( ListBlocks[i] ) )
                    {
                        return false;
                    }
                }
                int32 cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                while ( cur_ptr != NullValue )
                {
                    if ( !ApplyFunc( LinkedListElements[cur_ptr] ) )
                    {
                        return false;
                    }
                    cur_ptr = LinkedListElements[cur_ptr + 1];
                }
            }
        }
        return true;
    }

    int32 FSmallListSet::AllocateBlock()
    {
        int32 nfree = (int32)FreeBlocks.GetLength();
        if ( nfree > 0 )
        {
            int32 ptr = FreeBlocks[nfree - 1];
            FreeBlocks.PopBack();
            return ptr;
        }
        int32 nsize = (int32)ListBlocks.GetLength();
        ListBlocks.InsertAt( NullValue, nsize + BLOCK_LIST_OFFSET );
        ListBlocks[nsize] = 0;
        AllocatedCount++;
        return nsize;
    }

    bool FSmallListSet::RemoveFromLinkedList( int32 block_ptr, int32 val )
    {
        int32 cur_ptr  = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
        int32 prev_ptr = NullValue;
        while ( cur_ptr != NullValue )
        {
            if ( LinkedListElements[cur_ptr] == val )
            {
                int32 next_ptr = LinkedListElements[cur_ptr + 1];
                if ( prev_ptr == NullValue )
                {
                    ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = next_ptr;
                }
                else
                {
                    LinkedListElements[prev_ptr + 1] = next_ptr;
                }
                AddFreeLink( cur_ptr );
                return true;
            }
            prev_ptr = cur_ptr;
            cur_ptr  = LinkedListElements[cur_ptr + 1];
        }
        return false;
    }

} // namespace Desert::Geometry
