// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Util/SmallListSet.cpp:1-483, adapted: namespace
// Desert::Geometry, UE_CHECK* asserts, FArchive Serialize (484-619) not ported.
#include "Engine/Geometry/UECore/SmallListSet.hpp"

namespace Desert::Geometry
{

    void SmallListSet::Resize( int32_t NewSize )
    {
        auto const CurSize = static_cast<int32_t>( ListHeads.GetLength() );
        if ( NewSize > CurSize )
        {
            ListHeads.Resize( NewSize );
            for ( int32_t k = CurSize; k < NewSize; ++k )
            {
                ListHeads[k] = NullValue;
            }
        }
    }

    void SmallListSet::ResizeAndAllocateBlocks( int32_t NewSize )
    {
        Reset();
        Resize( NewSize );
        ListBlocks.Resize( NewSize * ( BLOCK_LIST_OFFSET + 1 ) );
        for ( int32_t i( 0 ); i < NewSize; ++i )
        {
            int32_t const ListHead                   = i * ( BLOCK_LIST_OFFSET + 1 );
            ListHeads[i]                             = ListHead;
            ListBlocks[ListHead]                     = 0;
            ListBlocks[ListHead + BLOCK_LIST_OFFSET] = NullValue;
        }

        for ( int32_t i( 0 ); i < NewSize; ++i )
        {
            assert( ListBlocks[ListHeads[i]] == 0 );
            assert( ListBlocks[ListHeads[i] + BLOCK_LIST_OFFSET] == NullValue );
        }

        AllocatedCount = NewSize;
    }

    void SmallListSet::AllocateAt( int32_t ListIndex )
    {
        assert( ListIndex >= 0 );
        if ( ListIndex >= (int)ListHeads.GetLength() )
        {
            auto j = static_cast<int32_t>( ListHeads.GetLength() );
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
            assert( ( ListHeads[ListIndex] == NullValue ) && "SmallListSet: list at %d is not empty!" );
        }
    }

    void SmallListSet::Compact( int32_t MaxListIndex )
    {
        assert( MaxListIndex >= 0 );
        auto const CurSize = static_cast<int32_t>( ListHeads.GetLength() );
        if ( MaxListIndex < CurSize )
        {
            // We just resize w/out book-keeping what we cleared, since we rebuild the blocks/etc below
            ListHeads.Resize( MaxListIndex );
        }

        AllocatedCount = 0;
        DynamicVector<int32_t> NewBlocks{};
        DynamicVector<int32_t> NewLinkedListElements{};
        for ( int32_t Idx = 0, Num = static_cast<int32_t>( ListHeads.GetLength() ), CurBlockIdx = 0; Idx < Num;
              ++Idx, CurBlockIdx += BLOCK_LIST_OFFSET + 1 )
        {
            int32_t const OrigHead = ListHeads[Idx];
            if ( OrigHead == NullValue )
            {
                continue;
            }
            AllocatedCount++;
            ListHeads[Idx] = CurBlockIdx;
            NewBlocks.InsertAt( NullValue, CurBlockIdx + BLOCK_LIST_OFFSET );
            for ( int32_t SubIdx = 0; SubIdx < BLOCK_LIST_OFFSET; ++SubIdx )
            {
                NewBlocks[CurBlockIdx + SubIdx] = ListBlocks[OrigHead + SubIdx];
            }

            int32_t const OrigLinkStart = ListBlocks[OrigHead + BLOCK_LIST_OFFSET];
            if ( OrigLinkStart == NullValue )
            {
                NewBlocks[CurBlockIdx + BLOCK_LIST_OFFSET] = NullValue;
            }
            else
            {
                int32_t CurPtr                             = OrigLinkStart;
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

        ListBlocks         = std::move( NewBlocks );
        LinkedListElements = std::move( NewLinkedListElements );
        FreeHeadIndex      = NullValue;
        FreeBlocks.Clear();
    }

    void SmallListSet::AppendWithElementOffset( const SmallListSet& Other, int32_t ElementOffset )
    {
        auto const    OrigListBlocksNum         = static_cast<int32_t>( ListBlocks.Num() );
        auto const    OrigListHeadsNum          = static_cast<int32_t>( ListHeads.Num() );
        auto const    OrigLinkedListElementsNum = static_cast<int32_t>( LinkedListElements.Num() );
        int32_t const OrigFreeHeadIndex         = FreeHeadIndex;

        // Append ListHeads indices
        ListHeads.Add( Other.ListHeads );
        for ( int32_t Idx = OrigListHeadsNum, N = static_cast<int32_t>( ListHeads.Num() ); Idx < N; ++Idx )
        {
            // Offset appended non-null indices to point to appended ListBlock indices
            if ( ListHeads[Idx] != NullValue )
            {
                ListHeads[Idx] += OrigListBlocksNum;
            }
        }

        // Append LinkedListElements entries
        LinkedListElements.Add( Other.LinkedListElements );
        for ( int32_t Idx = OrigLinkedListElementsNum, N = static_cast<int32_t>( LinkedListElements.Num() );
              Idx < N; Idx += 2 )
        {
            // Offset the element data
            LinkedListElements[Idx] += ElementOffset;
            // Offset appended non-null pointer indices to refer to appended indices
            int32_t& Link = LinkedListElements[Idx + 1];
            if ( Link != NullValue )
            {
                Link += OrigLinkedListElementsNum;
            }
        }

        // Append ListBlocks entries
        ListBlocks.Add( Other.ListBlocks );
        for ( int32_t Idx = OrigListBlocksNum, N = static_cast<int32_t>( ListBlocks.Num() ); Idx < N;
              Idx += BLOCKSIZE + 2 )
        {
            int32_t const BlockNumEls = ListBlocks[Idx];
            for ( int32_t SubIdx = 0, SubIdxNum = std::min( BLOCKSIZE, BlockNumEls ); SubIdx < SubIdxNum;
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
                int32_t WalkListIndex = FreeHeadIndex;
                while ( true )
                {
                    int32_t const NextIndex = LinkedListElements[WalkListIndex + 1];
                    if ( NextIndex == NullValue )
                    {
                        break;
                    }
                    WalkListIndex = NextIndex;
                }
                assert( LinkedListElements[WalkListIndex + 1] == NullValue );
                LinkedListElements[WalkListIndex + 1] = OrigFreeHeadIndex;
            }
        }

        AllocatedCount += Other.AllocatedCount;
    }

    void SmallListSet::Insert( int32_t ListIndex, int32_t Value )
    {
        assert( 0 <= ListIndex && ListIndex < (int32_t)ListHeads.Num() );
        int32_t block_ptr = ListHeads[ListIndex];
        if ( block_ptr == NullValue )
        {
            block_ptr             = AllocateBlock();
            ListBlocks[block_ptr] = 0;
            ListHeads[ListIndex]  = block_ptr;
        }

        int32_t const N = ListBlocks[block_ptr];
        if ( N < BLOCKSIZE )
        {
            ListBlocks[block_ptr + N + 1] = Value;
        }
        else
        {
            // spill to linked list
            int32_t const cur_head = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];

            if ( FreeHeadIndex == NullValue )
            {
                // allocate linkedlist node
                auto const new_ptr = static_cast<int32_t>( LinkedListElements.GetLength() );
                LinkedListElements.Add( Value );
                LinkedListElements.Add( cur_head );
                ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = new_ptr;
            }
            else
            {
                // pull from free list
                int32_t const free_ptr                    = FreeHeadIndex;
                FreeHeadIndex                             = LinkedListElements[free_ptr + 1];
                LinkedListElements[free_ptr]              = Value;
                LinkedListElements[free_ptr + 1]          = cur_head;
                ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = free_ptr;
            }
        }

        // count element
        ListBlocks[block_ptr] += 1;
    }

    bool SmallListSet::Remove( int32_t ListIndex, int32_t Value )
    {
        assert( ListIndex >= 0 );
        int32_t const block_ptr = ListHeads[ListIndex];
        int32_t const N         = ListBlocks[block_ptr];

        int32_t const iEnd = block_ptr + std::min( N, BLOCKSIZE );
        for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
        {

            if ( ListBlocks[i] == Value )
            {
                // Shifting (rather than moving the last element into the hole) keeps UE's list order.
                for ( int32_t j = i + 1; j <= iEnd; ++j ) // shift left
                {
                    ListBlocks[j - 1] = ListBlocks[j];
                }
                // ListBlocks[iEnd] = -2;     // OPTIONAL

                if ( N > BLOCKSIZE )
                {
                    int32_t const cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
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

    void SmallListSet::Move( int32_t FromIndex, int32_t ToIndex )
    {
        assert( FromIndex >= 0 );
        assert( ToIndex >= 0 );
        assert( ListHeads[ToIndex] == NullValue );
        ListHeads[ToIndex]   = ListHeads[FromIndex];
        ListHeads[FromIndex] = NullValue;
    }

    void SmallListSet::Clear( int32_t ListIndex )
    {
        assert( ListIndex >= 0 );
        int32_t const block_ptr = ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32_t const N = ListBlocks[block_ptr];

            // if we have spilled to linked-list, free nodes
            if ( N > BLOCKSIZE )
            {
                int32_t cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                while ( cur_ptr != NullValue )
                {
                    int32_t const free_ptr = cur_ptr;
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

    bool SmallListSet::Contains( int32_t ListIndex, int32_t Value ) const
    {
        assert( ListIndex >= 0 );
        int32_t const block_ptr = ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32_t const N = ListBlocks[block_ptr];
            if ( N < BLOCKSIZE )
            {
                int32_t const iEnd = block_ptr + N;
                for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
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
                int32_t const iEnd = block_ptr + BLOCKSIZE;
                for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( ListBlocks[i] == Value )
                    {
                        return true;
                    }
                }
                int32_t cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
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

    bool SmallListSet::EnumerateEarlyOut( int32_t                               ListIndex,
                                          const std::function<bool( int32_t )>& ApplyFunc ) const
    {
        int32_t const block_ptr = ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32_t const N = ListBlocks[block_ptr];
            if ( N < BLOCKSIZE )
            {
                int32_t const iEnd = block_ptr + N;
                for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
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
                int32_t const iEnd = block_ptr + BLOCKSIZE;
                for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( !ApplyFunc( ListBlocks[i] ) )
                    {
                        return false;
                    }
                }
                int32_t cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
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

    int32_t SmallListSet::AllocateBlock()
    {
        auto const nfree = static_cast<int32_t>( FreeBlocks.GetLength() );
        if ( nfree > 0 )
        {
            int32_t const ptr = FreeBlocks[nfree - 1];
            FreeBlocks.PopBack();
            return ptr;
        }
        auto const nsize = static_cast<int32_t>( ListBlocks.GetLength() );
        ListBlocks.InsertAt( NullValue, nsize + BLOCK_LIST_OFFSET );
        ListBlocks[nsize] = 0;
        AllocatedCount++;
        return nsize;
    }

    bool SmallListSet::RemoveFromLinkedList( int32_t block_ptr, int32_t val )
    {
        int32_t cur_ptr  = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
        int32_t prev_ptr = NullValue;
        while ( cur_ptr != NullValue )
        {
            if ( LinkedListElements[cur_ptr] == val )
            {
                int32_t const next_ptr = LinkedListElements[cur_ptr + 1];
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
