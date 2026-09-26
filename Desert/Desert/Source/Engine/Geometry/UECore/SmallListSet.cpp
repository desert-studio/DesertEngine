// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Util/SmallListSet.cpp:1-483, adapted: namespace
// Desert::Geometry, UE_CHECK* asserts, FArchive Serialize (484-619) not ported.
#include "Engine/Geometry/UECore/SmallListSet.hpp"

namespace Desert::Geometry
{

    void SmallListSet::Resize( int32_t NewSize )
    {
        auto const CurSize = static_cast<int32_t>( m_ListHeads.GetLength() );
        if ( NewSize > CurSize )
        {
            m_ListHeads.Resize( NewSize );
            for ( int32_t k = CurSize; k < NewSize; ++k )
            {
                m_ListHeads[k] = NullValue;
            }
        }
    }

    void SmallListSet::ResizeAndAllocateBlocks( int32_t NewSize )
    {
        Reset();
        Resize( NewSize );
        m_ListBlocks.Resize( NewSize * ( BLOCK_LIST_OFFSET + 1 ) );
        for ( int32_t i( 0 ); i < NewSize; ++i )
        {
            int32_t const ListHead                   = i * ( BLOCK_LIST_OFFSET + 1 );
            m_ListHeads[i]                             = ListHead;
            m_ListBlocks[ListHead]                     = 0;
            m_ListBlocks[ListHead + BLOCK_LIST_OFFSET] = NullValue;
        }

        for ( int32_t i( 0 ); i < NewSize; ++i )
        {
            assert( m_ListBlocks[m_ListHeads[i]] == 0 );
            assert( m_ListBlocks[m_ListHeads[i] + BLOCK_LIST_OFFSET] == NullValue );
        }

        m_AllocatedCount = NewSize;
    }

    void SmallListSet::AllocateAt( int32_t ListIndex )
    {
        assert( ListIndex >= 0 );
        if ( ListIndex >= (int)m_ListHeads.GetLength() )
        {
            auto j = static_cast<int32_t>( m_ListHeads.GetLength() );
            m_ListHeads.InsertAt( NullValue, ListIndex );
            // need to set intermediate values to null!
            while ( j < ListIndex )
            {
                m_ListHeads[j] = NullValue;
                j++;
            }
        }
        else
        {
            assert( ( m_ListHeads[ListIndex] == NullValue ) && "SmallListSet: list at %d is not empty!" );
        }
    }

    void SmallListSet::Compact( int32_t MaxListIndex )
    {
        assert( MaxListIndex >= 0 );
        auto const CurSize = static_cast<int32_t>( m_ListHeads.GetLength() );
        if ( MaxListIndex < CurSize )
        {
            // We just resize w/out book-keeping what we cleared, since we rebuild the blocks/etc below
            m_ListHeads.Resize( MaxListIndex );
        }

        m_AllocatedCount = 0;
        DynamicVector<int32_t> NewBlocks{};
        DynamicVector<int32_t> NewLinkedListElements{};
        for ( int32_t Idx = 0, Num = static_cast<int32_t>( m_ListHeads.GetLength() ), CurBlockIdx = 0; Idx < Num;
              ++Idx, CurBlockIdx += BLOCK_LIST_OFFSET + 1 )
        {
            int32_t const OrigHead = m_ListHeads[Idx];
            if ( OrigHead == NullValue )
            {
                continue;
            }
            m_AllocatedCount++;
            m_ListHeads[Idx] = CurBlockIdx;
            NewBlocks.InsertAt( NullValue, CurBlockIdx + BLOCK_LIST_OFFSET );
            for ( int32_t SubIdx = 0; SubIdx < BLOCK_LIST_OFFSET; ++SubIdx )
            {
                NewBlocks[CurBlockIdx + SubIdx] = m_ListBlocks[OrigHead + SubIdx];
            }

            int32_t const OrigLinkStart = m_ListBlocks[OrigHead + BLOCK_LIST_OFFSET];
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
                    NewLinkedListElements.Add( m_LinkedListElements[CurPtr] );
                    CurPtr = m_LinkedListElements[CurPtr + 1];
                    if ( CurPtr == NullValue )
                    {
                        break;
                    }
                    NewLinkedListElements.Add( NewLinkedListElements.GetLength() + 1 );
                }
                NewLinkedListElements.Add( NullValue );
            }
        }

        m_ListBlocks         = std::move( NewBlocks );
        m_LinkedListElements = std::move( NewLinkedListElements );
        m_FreeHeadIndex      = NullValue;
        m_FreeBlocks.Clear();
    }

    void SmallListSet::AppendWithElementOffset( const SmallListSet& Other, int32_t ElementOffset )
    {
        auto const    OrigListBlocksNum         = static_cast<int32_t>( m_ListBlocks.Num() );
        auto const    OrigListHeadsNum          = static_cast<int32_t>( m_ListHeads.Num() );
        auto const    OrigLinkedListElementsNum = static_cast<int32_t>( m_LinkedListElements.Num() );
        int32_t const OrigFreeHeadIndex         = m_FreeHeadIndex;

        // Append ListHeads indices
        m_ListHeads.Add( Other.m_ListHeads );
        for ( int32_t Idx = OrigListHeadsNum, N = static_cast<int32_t>( m_ListHeads.Num() ); Idx < N; ++Idx )
        {
            // Offset appended non-null indices to point to appended ListBlock indices
            if ( m_ListHeads[Idx] != NullValue )
            {
                m_ListHeads[Idx] += OrigListBlocksNum;
            }
        }

        // Append LinkedListElements entries
        m_LinkedListElements.Add( Other.m_LinkedListElements );
        for ( int32_t Idx = OrigLinkedListElementsNum, N = static_cast<int32_t>( m_LinkedListElements.Num() );
              Idx < N; Idx += 2 )
        {
            // Offset the element data
            m_LinkedListElements[Idx] += ElementOffset;
            // Offset appended non-null pointer indices to refer to appended indices
            int32_t& Link = m_LinkedListElements[Idx + 1];
            if ( Link != NullValue )
            {
                Link += OrigLinkedListElementsNum;
            }
        }

        // Append ListBlocks entries
        m_ListBlocks.Add( Other.m_ListBlocks );
        for ( int32_t Idx = OrigListBlocksNum, N = static_cast<int32_t>( m_ListBlocks.Num() ); Idx < N;
              Idx += BLOCKSIZE + 2 )
        {
            int32_t const BlockNumEls = m_ListBlocks[Idx];
            for ( int32_t SubIdx = 0, SubIdxNum = std::min( BLOCKSIZE, BlockNumEls ); SubIdx < SubIdxNum;
                  ++SubIdx )
            {
                m_ListBlocks[Idx + 1 + SubIdx] += ElementOffset;
            }
            if ( m_ListBlocks[Idx + BLOCKSIZE + 1] != NullValue )
            {
                m_ListBlocks[Idx + BLOCKSIZE + 1] += OrigLinkedListElementsNum;
            }
        }

        // If we had a non-empty free list on Other, need to transfer it too
        if ( Other.m_FreeHeadIndex != NullValue )
        {
            m_FreeHeadIndex = Other.m_FreeHeadIndex + OrigLinkedListElementsNum;
            // If both were non-empty, need to walk Other's free list to attach its tail to our original head
            if ( OrigFreeHeadIndex != NullValue )
            {
                int32_t WalkListIndex = m_FreeHeadIndex;
                while ( true )
                {
                    int32_t const NextIndex = m_LinkedListElements[WalkListIndex + 1];
                    if ( NextIndex == NullValue )
                    {
                        break;
                    }
                    WalkListIndex = NextIndex;
                }
                assert( m_LinkedListElements[WalkListIndex + 1] == NullValue );
                m_LinkedListElements[WalkListIndex + 1] = OrigFreeHeadIndex;
            }
        }

        m_AllocatedCount += Other.m_AllocatedCount;
    }

    void SmallListSet::Insert( int32_t ListIndex, int32_t Value )
    {
        assert( 0 <= ListIndex && ListIndex < (int32_t)m_ListHeads.Num() );
        int32_t block_ptr = m_ListHeads[ListIndex];
        if ( block_ptr == NullValue )
        {
            block_ptr             = AllocateBlock();
            m_ListBlocks[block_ptr] = 0;
            m_ListHeads[ListIndex]  = block_ptr;
        }

        int32_t const N = m_ListBlocks[block_ptr];
        if ( N < BLOCKSIZE )
        {
            m_ListBlocks[block_ptr + N + 1] = Value;
        }
        else
        {
            // spill to linked list
            int32_t const cur_head = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];

            if ( m_FreeHeadIndex == NullValue )
            {
                // allocate linkedlist node
                auto const new_ptr = static_cast<int32_t>( m_LinkedListElements.GetLength() );
                m_LinkedListElements.Add( Value );
                m_LinkedListElements.Add( cur_head );
                m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = new_ptr;
            }
            else
            {
                // pull from free list
                int32_t const free_ptr                      = m_FreeHeadIndex;
                m_FreeHeadIndex                             = m_LinkedListElements[free_ptr + 1];
                m_LinkedListElements[free_ptr]              = Value;
                m_LinkedListElements[free_ptr + 1]          = cur_head;
                m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = free_ptr;
            }
        }

        // count element
        m_ListBlocks[block_ptr] += 1;
    }

    bool SmallListSet::Remove( int32_t ListIndex, int32_t Value )
    {
        assert( ListIndex >= 0 );
        int32_t const block_ptr = m_ListHeads[ListIndex];
        int32_t const N         = m_ListBlocks[block_ptr];

        int32_t const iEnd = block_ptr + std::min( N, BLOCKSIZE );
        for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
        {

            if ( m_ListBlocks[i] == Value )
            {
                // Shifting (rather than moving the last element into the hole) keeps UE's list order.
                for ( int32_t j = i + 1; j <= iEnd; ++j ) // shift left
                {
                    m_ListBlocks[j - 1] = m_ListBlocks[j];
                }
                // ListBlocks[iEnd] = -2;     // OPTIONAL

                if ( N > BLOCKSIZE )
                {
                    int32_t const cur_ptr = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET] =
                         m_LinkedListElements[cur_ptr + 1]; // point32 to cur->next
                    m_ListBlocks[iEnd] = m_LinkedListElements[cur_ptr];
                    AddFreeLink( cur_ptr );
                }

                m_ListBlocks[block_ptr] -= 1;
                return true;
            }
        }

        // search list
        if ( N > BLOCKSIZE )
        {
            if ( RemoveFromLinkedList( block_ptr, Value ) )
            {
                m_ListBlocks[block_ptr] -= 1;
                return true;
            }
        }

        return false;
    }

    void SmallListSet::Move( int32_t FromIndex, int32_t ToIndex )
    {
        assert( FromIndex >= 0 );
        assert( ToIndex >= 0 );
        assert( m_ListHeads[ToIndex] == NullValue );
        m_ListHeads[ToIndex]   = m_ListHeads[FromIndex];
        m_ListHeads[FromIndex] = NullValue;
    }

    void SmallListSet::Clear( int32_t ListIndex )
    {
        assert( ListIndex >= 0 );
        int32_t const block_ptr = m_ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32_t const N = m_ListBlocks[block_ptr];

            // if we have spilled to linked-list, free nodes
            if ( N > BLOCKSIZE )
            {
                int32_t cur_ptr = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                while ( cur_ptr != NullValue )
                {
                    int32_t const free_ptr = cur_ptr;
                    cur_ptr                = m_LinkedListElements[cur_ptr + 1];
                    AddFreeLink( free_ptr );
                }
                m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = NullValue;
            }

            // free our block
            m_ListBlocks[block_ptr] = 0;
            m_FreeBlocks.Add( block_ptr );
            m_ListHeads[ListIndex] = NullValue;
        }
    }

    bool SmallListSet::Contains( int32_t ListIndex, int32_t Value ) const
    {
        assert( ListIndex >= 0 );
        int32_t const block_ptr = m_ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32_t const N = m_ListBlocks[block_ptr];
            if ( N < BLOCKSIZE )
            {
                int32_t const iEnd = block_ptr + N;
                for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( m_ListBlocks[i] == Value )
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
                    if ( m_ListBlocks[i] == Value )
                    {
                        return true;
                    }
                }
                int32_t cur_ptr = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                while ( cur_ptr != NullValue )
                {
                    if ( m_LinkedListElements[cur_ptr] == Value )
                    {
                        return true;
                    }
                    cur_ptr = m_LinkedListElements[cur_ptr + 1];
                }
            }
        }
        return false;
    }

    bool SmallListSet::EnumerateEarlyOut( int32_t                               ListIndex,
                                          const std::function<bool( int32_t )>& ApplyFunc ) const
    {
        int32_t const block_ptr = m_ListHeads[ListIndex];
        if ( block_ptr != NullValue )
        {
            int32_t const N = m_ListBlocks[block_ptr];
            if ( N < BLOCKSIZE )
            {
                int32_t const iEnd = block_ptr + N;
                for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                {
                    if ( !ApplyFunc( m_ListBlocks[i] ) )
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
                    if ( !ApplyFunc( m_ListBlocks[i] ) )
                    {
                        return false;
                    }
                }
                int32_t cur_ptr = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                while ( cur_ptr != NullValue )
                {
                    if ( !ApplyFunc( m_LinkedListElements[cur_ptr] ) )
                    {
                        return false;
                    }
                    cur_ptr = m_LinkedListElements[cur_ptr + 1];
                }
            }
        }
        return true;
    }

    int32_t SmallListSet::AllocateBlock()
    {
        auto const nfree = static_cast<int32_t>( m_FreeBlocks.GetLength() );
        if ( nfree > 0 )
        {
            int32_t const ptr = m_FreeBlocks[nfree - 1];
            m_FreeBlocks.PopBack();
            return ptr;
        }
        auto const nsize = static_cast<int32_t>( m_ListBlocks.GetLength() );
        m_ListBlocks.InsertAt( NullValue, nsize + BLOCK_LIST_OFFSET );
        m_ListBlocks[nsize] = 0;
        m_AllocatedCount++;
        return nsize;
    }

    bool SmallListSet::RemoveFromLinkedList( int32_t block_ptr, int32_t val )
    {
        int32_t cur_ptr  = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
        int32_t prev_ptr = NullValue;
        while ( cur_ptr != NullValue )
        {
            if ( m_LinkedListElements[cur_ptr] == val )
            {
                int32_t const next_ptr = m_LinkedListElements[cur_ptr + 1];
                if ( prev_ptr == NullValue )
                {
                    m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET] = next_ptr;
                }
                else
                {
                    m_LinkedListElements[prev_ptr + 1] = next_ptr;
                }
                AddFreeLink( cur_ptr );
                return true;
            }
            prev_ptr = cur_ptr;
            cur_ptr  = m_LinkedListElements[cur_ptr + 1];
        }
        return false;
    }

} // namespace Desert::Geometry
