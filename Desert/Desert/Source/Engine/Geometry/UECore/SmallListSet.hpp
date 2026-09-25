// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/SmallListSet.h:1-337,360-688,697-706, adapted:
// UE Core types via UECore.hpp, namespace Desert::Geometry, FString MemoryUsage and FArchive serialization not
// ported. Port of geometry3cpp small_list_set

#pragma once

#include "Engine/Geometry/UECore/DynamicVector.hpp"

namespace Desert::Geometry
{

    /**
     * FSmallListSet stores a set of short integer-valued variable-size lists.
     * The lists are encoded into a few large TDynamicVector buffers, with internal pooling,
     * so adding/removing lists usually does not involve any new or delete ops.
     *
     * The lists are stored in two parts. The first N elements are stored in a linear
     * subset of a TDynamicVector. If the list spills past these N elements, the extra elements
     * are stored in a linked list (which is also stored in a flat array).
     *
     * Each list stores its count, so list-size operations are constant time.
     * All the internal "pointers" are 32-bit.
     */
    class FSmallListSet
    {
    protected:
        /** This value is used to indicate Null in internal pointers */
        static constexpr int32_t NullValue = -1;

        /** size of initial linear-memory portion of lists */
        static constexpr int32_t BLOCKSIZE = 8;
        /** offset from start of linear-memory portion of list that contains pointer to head of variable-length
         * linked list */
        static constexpr int32_t BLOCK_LIST_OFFSET = BLOCKSIZE + 1;

        /** mapping from list index to offset into ListBlocks that contains list data */
        TDynamicVector<int32_t> ListHeads{};

        /**
         * flat buffer used to store per-list linear-memory blocks.
         * blocks are BLOCKSIZE+2 long, elements are [CurrentCount, item0...itemN, LinkedListPtr]
         */
        TDynamicVector<int32_t> ListBlocks{};

        /** list of free blocks as indices/offsets into ListBlocks */
        TDynamicVector<int32_t> FreeBlocks{};

        /** number of allocated lists */
        int32_t AllocatedCount{ 0 };

        /**
         * flat buffer used to store linked-list "spill" elements
         * each element is [value, next_ptr]
         */
        TDynamicVector<int32_t> LinkedListElements{};

        /** index of first free element in LinkedListElements */
        int32_t FreeHeadIndex{ NullValue };

    public:
        /**
         * @return largest current list index
         */
        size_t Size() const
        {
            return ListHeads.GetLength();
        }

        /**
         * set new number of lists
         */
        void Resize( int32_t NewSize );

        /**
         * resize to a new number of lists and pre-allocate an initial empty block for each list.
         */
        void ResizeAndAllocateBlocks( int32_t NewSize );

        /**
         * Reset to initial state
         */
        void Reset()
        {
            ListHeads.Clear();
            ListBlocks.Clear();
            FreeBlocks.Clear();
            AllocatedCount = 0;
            LinkedListElements.Clear();
            FreeHeadIndex = NullValue;
        }

        /**
         * Clearing any lists at or after MaxListIndex and compact the ListBlocks and LinkedListElements so there
         * are no free blocks or free linked list elements
         */
        void Compact( int32_t MaxListIndex );

        /**
         * @return true if a list has been allocated at the given ListIndex
         */
        bool IsAllocated( int32_t ListIndex ) const
        {
            return ( ListIndex >= 0 && ListIndex < (int32_t)ListHeads.GetLength() &&
                     ListHeads[ListIndex] != NullValue );
        }

        /**
         * Create a list at the given ListIndex
         */
        void AllocateAt( int32_t ListIndex );

        /**
         * Insert Value into list at ListIndex
         */
        void Insert( int32_t ListIndex, int32_t Value );

        /**
         * remove Value from the list at ListIndex
         * @return false if Value was not in this list
         */
        bool Remove( int32_t ListIndex, int32_t Value );

        /**
         * Move list at FromIndex to ToIndex
         */
        void Move( int32_t FromIndex, int32_t ToIndex );

        /**
         * Remove all elements from the list at ListIndex
         */
        void Clear( int32_t ListIndex );

        /**
         * @return the size of the list at ListIndex
         */
        inline int32_t GetCount( int32_t ListIndex ) const
        {
            UE_CHECK_SLOW( ListIndex >= 0 );
            int32_t block_ptr = ListHeads[ListIndex];
            return ( block_ptr == NullValue ) ? 0 : ListBlocks[block_ptr];
        }

        /**
         * @return the first item in the list at ListIndex
         * @warning does not check for zero-size-list!
         */
        inline int32_t First( int32_t ListIndex ) const
        {
            UE_CHECK_SLOW( ListIndex >= 0 );
            int32_t block_ptr = ListHeads[ListIndex];
            return ListBlocks[block_ptr + 1];
        }

        /**
         * Search for the given Value in list at ListIndex
         * @return true if found
         */
        bool Contains( int32_t ListIndex, int32_t Value ) const;

        /**
         * Search the list at ListIndex for a value where PredicateFunc(value) returns true
         * @return the found value, or the InvalidValue argument if not found
         */
        template <typename IntToBoolFunc>
        inline int32_t Find( int32_t ListIndex, const IntToBoolFunc& PredicateFunc,
                             int32_t InvalidValue = -1 ) const
        {
            int32_t block_ptr = ListHeads[ListIndex];
            if ( block_ptr != NullValue )
            {
                int32_t N = ListBlocks[block_ptr];
                if ( N < BLOCKSIZE )
                {
                    int32_t iEnd = block_ptr + N;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        int32_t Value = ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            return Value;
                        }
                    }
                }
                else
                {
                    // we spilled to linked list, have to iterate through it as well
                    int32_t iEnd = block_ptr + BLOCKSIZE;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        int32_t Value = ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            return Value;
                        }
                    }
                    int32_t cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    while ( cur_ptr != NullValue )
                    {
                        int32_t Value = LinkedListElements[cur_ptr];
                        if ( PredicateFunc( Value ) )
                        {
                            return Value;
                        }
                        cur_ptr = LinkedListElements[cur_ptr + 1];
                    }
                }
            }
            return InvalidValue;
        }

        /**
         * Search the list at ListIndex for a value where PredicateFunc(value) returns true, and replace it with
         * NewValue
         * @return true if the value was found and replaced
         */
        template <typename IntToBoolFunc>
        inline bool Replace( int32_t ListIndex, const IntToBoolFunc& PredicateFunc, int32_t NewValue )
        {
            int32_t block_ptr = ListHeads[ListIndex];
            if ( block_ptr != NullValue )
            {
                int32_t N = ListBlocks[block_ptr];
                if ( N < BLOCKSIZE )
                {
                    int32_t iEnd = block_ptr + N;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        int32_t Value = ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            ListBlocks[i] = NewValue;
                            return true;
                        }
                    }
                }
                else
                {
                    // we spilled to linked list, have to iterate through it as well
                    int32_t iEnd = block_ptr + BLOCKSIZE;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        int32_t Value = ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            ListBlocks[i] = NewValue;
                            return true;
                        }
                    }
                    int32_t cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    while ( cur_ptr != NullValue )
                    {
                        int32_t Value = LinkedListElements[cur_ptr];
                        if ( PredicateFunc( Value ) )
                        {
                            LinkedListElements[cur_ptr] = NewValue;
                            return true;
                        }
                        cur_ptr = LinkedListElements[cur_ptr + 1];
                    }
                }
            }
            return false;
        }

        /**
         * Call ApplyFunc on each element of the list at ListIndex
         */
        template <typename IntToVoidFunc>
        inline void Enumerate( int32_t ListIndex, const IntToVoidFunc& ApplyFunc ) const
        {
            int32_t block_ptr = ListHeads[ListIndex];
            if ( block_ptr != NullValue )
            {
                int32_t N = ListBlocks[block_ptr];
                if ( N < BLOCKSIZE )
                {
                    int32_t iEnd = block_ptr + N;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        ApplyFunc( ListBlocks[i] );
                    }
                }
                else
                {
                    // we spilled to linked list, have to iterate through it as well
                    int32_t iEnd = block_ptr + BLOCKSIZE;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        ApplyFunc( ListBlocks[i] );
                    }
                    int32_t cur_ptr = ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    while ( cur_ptr != NullValue )
                    {
                        ApplyFunc( LinkedListElements[cur_ptr] );
                        cur_ptr = LinkedListElements[cur_ptr + 1];
                    }
                }
            }
        }

        /**
         * Append all elements from another small list set, with ElementOffset added to all such appended elements
         * Note: Also directly appends free blocks / free lists; does no compacting
         * @param Other the list to append
         * @param ElementOffset amount to shift all appended elements
         */
        void AppendWithElementOffset( const FSmallListSet& Other, int32_t ElementOffset );

        /**
         * Call ApplyFunc on each element of the list at ListIndex, until ApplyFunc returns false
         * @return true if all elements were processed and ApplyFunc never returned false
         */
        bool EnumerateEarlyOut( int32_t ListIndex, std::function<bool( int32_t )> ApplyFunc ) const;

        friend bool operator==( const FSmallListSet& Lhs, const FSmallListSet& Rhs )
        {
            if ( Lhs.Size() != Rhs.Size() )
            {
                return false;
            }

            for ( int32_t ListIndex = 0, ListNum = Lhs.Size(); ListIndex < ListNum; ++ListIndex )
            {
                if ( Lhs.GetCount( ListIndex ) != Rhs.GetCount( ListIndex ) )
                {
                    return false;
                }

                ValueIterator       ItLhs    = Lhs.BeginValues( ListIndex );
                ValueIterator       ItRhs    = Rhs.BeginValues( ListIndex );
                const ValueIterator ItLhsEnd = Lhs.EndValues( ListIndex );
                const ValueIterator ItRhsEnd = Rhs.EndValues( ListIndex );
                while ( ItLhs != ItLhsEnd && ItRhs != ItRhsEnd )
                {
                    if ( *ItLhs != *ItRhs )
                    {
                        return false;
                    }
                    ++ItLhs;
                    ++ItRhs;
                }
                if ( ItLhs != ItLhsEnd || ItRhs != ItRhsEnd )
                {
                    return false;
                }
            }

            return true;
        }

        friend bool operator!=( const FSmallListSet& Lhs, const FSmallListSet& Rhs )
        {
            return !( Lhs == Rhs );
        }

        //
        // iterator support
        //

        friend class ValueIterator;
        friend class BaseValueIterator;

        /**
         * BaseValueIterator is a base class for ValueIterator and MappedValueIterator below.
         */
        class BaseValueIterator
        {
        public:
            BaseValueIterator()
            {
                ListSet   = nullptr;
                ListIndex = 0;
            }

            inline bool operator==( const BaseValueIterator& Other ) const
            {
                return ListSet == Other.ListSet && ListIndex == Other.ListIndex;
            }
            inline bool operator!=( const BaseValueIterator& Other ) const
            {
                return ListSet != Other.ListSet || ListIndex != Other.ListIndex || iCur != Other.iCur ||
                       cur_ptr != Other.cur_ptr;
            }

        protected:
            inline void GotoNext()
            {
                if ( N == 0 )
                {
                    SetToEnd();
                    return;
                }
                GotoNextOverflow();
            }

            inline void GotoNextOverflow()
            {
                if ( iCur <= iEnd )
                {
                    cur_value = ListSet->ListBlocks[iCur];
                    iCur++;
                }
                else if ( cur_ptr != NullValue )
                {
                    cur_value = ListSet->LinkedListElements[cur_ptr];
                    cur_ptr   = ListSet->LinkedListElements[cur_ptr + 1];
                }
                else
                {
                    SetToEnd();
                }
            }

            BaseValueIterator( const FSmallListSet* ListSetIn, int32_t ListIndex, bool is_end )
            {
                this->ListSet   = ListSetIn;
                this->ListIndex = ListIndex;
                if ( is_end )
                {
                    SetToEnd();
                }
                else
                {
                    block_ptr = ListSet->ListHeads[ListIndex];
                    if ( block_ptr != ListSet->NullValue )
                    {
                        N    = ListSet->ListBlocks[block_ptr];
                        iEnd = ( N < BLOCKSIZE ) ? ( block_ptr + N ) : ( block_ptr + BLOCKSIZE );
                        iCur = block_ptr + 1;
                        cur_ptr =
                             ( N < BLOCKSIZE ) ? NullValue : ListSet->ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                        GotoNext();
                    }
                    else
                    {
                        SetToEnd();
                    }
                }
            }

            inline void SetToEnd()
            {
                block_ptr = ListSet->NullValue;
                N         = 0;
                iCur      = -1;
                cur_ptr   = -1;
            }

            const FSmallListSet* ListSet;
            int32_t              ListIndex;
            int32_t              block_ptr;
            int32_t              N;
            int32_t              iEnd;
            int32_t              iCur;
            int32_t              cur_ptr;
            int32_t              cur_value;
            friend class FSmallListSet;
        };

        /**
         * ValueIterator iterates over the values of a small list
         */
        class ValueIterator : public BaseValueIterator
        {
        public:
            ValueIterator() : BaseValueIterator()
            {
            }

            inline int32_t operator*() const
            {
                return cur_value;
            }

            inline const ValueIterator& operator++() // prefix
            {
                this->GotoNext();
                return *this;
            }

        protected:
            ValueIterator( const FSmallListSet* ListSetIn, int32_t ListIndex, bool is_end )
                 : BaseValueIterator( ListSetIn, ListIndex, is_end )
            {
            }

            friend class FSmallListSet;
        };

        /**
         * @return iterator for start of list at ListIndex
         */
        inline ValueIterator BeginValues( int32_t ListIndex ) const
        {
            return ValueIterator( this, ListIndex, false );
        }

        /**
         * @return iterator for end of list at ListIndex
         */
        inline ValueIterator EndValues( int32_t ListIndex ) const
        {
            return ValueIterator( this, ListIndex, true );
        }

        /**
         * ValueEnumerable is an object that provides begin/end semantics for a small list, suitable for use with a
         * range-based for loop
         */
        class ValueEnumerable
        {
        public:
            const FSmallListSet* ListSet;
            int32_t              ListIndex;
            ValueEnumerable()
            {
            }
            ValueEnumerable( const FSmallListSet* ListSetIn, int32_t ListIndex )
            {
                this->ListSet   = ListSetIn;
                this->ListIndex = ListIndex;
            }
            typename FSmallListSet::ValueIterator begin() const
            {
                return ListSet->BeginValues( ListIndex );
            }
            typename FSmallListSet::ValueIterator end() const
            {
                return ListSet->EndValues( ListIndex );
            }
        };

        /**
         * @return a value enumerable for the given ListIndex
         */
        inline ValueEnumerable Values( int32_t ListIndex ) const
        {
            return ValueEnumerable( this, ListIndex );
        }

        //
        // mapped iterator support - mapped iterator applies an arbitrary function to the iterator value
        //

        friend class MappedValueIterator;

        /**
         * MappedValueIterator iterates over the values of a small list
         * An optional mapping function can be provided which will then be applied to the values returned by the *
         * operator
         */
        class MappedValueIterator : public BaseValueIterator
        {
        public:
            MappedValueIterator() : BaseValueIterator()
            {
                MapFunc = []( int32_t value ) { return value; };
            }

            inline int32_t operator*() const
            {
                return MapFunc( cur_value );
            }

            inline const MappedValueIterator& operator++() // prefix
            {
                this->GotoNext();
                return *this;
            }

        protected:
            MappedValueIterator( const FSmallListSet* ListSetIn, int32_t ListIndex, bool is_end,
                                 std::function<int32_t( int32_t )> MapFuncIn )
                 : BaseValueIterator( ListSetIn, ListIndex, is_end )
            {
                MapFunc = MapFuncIn;
            }

            std::function<int32_t( int32_t )> MapFunc;
            friend class FSmallListSet;
        };

        /**
         * @return iterator for start of list at ListIndex, with given value mapping function
         */
        inline MappedValueIterator BeginMappedValues( int32_t                                  ListIndex,
                                                      const std::function<int32_t( int32_t )>& MapFunc ) const
        {
            return MappedValueIterator( this, ListIndex, false, MapFunc );
        }

        /**
         * @return iterator for end of list at ListIndex, with given value mapping function
         */
        inline MappedValueIterator EndMappedValues( int32_t                                  ListIndex,
                                                    const std::function<int32_t( int32_t )>& MapFunc ) const
        {
            return MappedValueIterator( this, ListIndex, true, MapFunc );
        }

        /**
         * MappedValueEnumerable is an object that provides begin/end semantics for a small list, suitable for use
         * with a range-based for loop
         */
        class MappedValueEnumerable
        {
        public:
            const FSmallListSet*      ListSet;
            int32_t                           ListIndex;
            std::function<int32_t( int32_t )> MapFunc;
            MappedValueEnumerable()
            {
            }
            MappedValueEnumerable( const FSmallListSet* ListSetIn, int32_t ListIndex,
                                   std::function<int32_t( int32_t )> MapFunc )
            {
                this->ListSet   = ListSetIn;
                this->ListIndex = ListIndex;
                this->MapFunc   = std::move( MapFunc );
            }
            typename FSmallListSet::MappedValueIterator begin() const
            {
                return ListSet->BeginMappedValues( ListIndex, MapFunc );
            }
            typename FSmallListSet::MappedValueIterator end() const
            {
                return ListSet->EndMappedValues( ListIndex, MapFunc );
            }
        };

        /**
         * @return a value enumerable for the given ListIndex, with the given value mapping function
         */
        inline MappedValueEnumerable MappedValues( int32_t                           ListIndex,
                                                   std::function<int32_t( int32_t )> MapFunc ) const
        {
            return MappedValueEnumerable( this, ListIndex, MapFunc );
        }

    protected:
        // grab a block from the free list, or allocate a new one
        int32_t AllocateBlock();

        // push a link-node onto the free list
        inline void AddFreeLink( int32_t ptr )
        {
            LinkedListElements[ptr + 1] = FreeHeadIndex;
            FreeHeadIndex               = ptr;
        }

        // remove val from the linked-list attached to block_ptr
        bool RemoveFromLinkedList( int32_t block_ptr, int32_t val );

    public:
        size_t GetByteCount() const
        {
            return ListHeads.GetByteCount() + FreeBlocks.GetByteCount() + ListBlocks.GetByteCount() +
                   LinkedListElements.GetByteCount();
        }
    };

} // namespace Desert::Geometry
