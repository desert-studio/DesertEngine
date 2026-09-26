// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/SmallListSet.h:1-337,360-688,697-706, adapted:
// UE Core types as std/glm, namespace Desert::Geometry, FString MemoryUsage and FArchive serialization not
// ported. Port of geometry3cpp small_list_set

#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "Engine/Geometry/MeshCore/DynamicVector.hpp"

namespace Desert::Geometry
{

    /**
     * SmallListSet stores a set of short integer-valued variable-size lists.
     * The lists are encoded into a few large DynamicVector buffers, with internal pooling,
     * so adding/removing lists usually does not involve any new or delete ops.
     *
     * The lists are stored in two parts. The first N elements are stored in a linear
     * subset of a DynamicVector. If the list spills past these N elements, the extra elements
     * are stored in a linked list (which is also stored in a flat array).
     *
     * Each list stores its count, so list-size operations are constant time.
     * All the internal "pointers" are 32-bit.
     */
    class SmallListSet
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
        DynamicVector<int32_t> m_ListHeads{};

        /**
         * flat buffer used to store per-list linear-memory blocks.
         * blocks are BLOCKSIZE+2 long, elements are [CurrentCount, item0...itemN, LinkedListPtr]
         */
        DynamicVector<int32_t> m_ListBlocks{};

        /** list of free blocks as indices/offsets into ListBlocks */
        DynamicVector<int32_t> m_FreeBlocks{};

        /** number of allocated lists */
        int32_t m_AllocatedCount{ 0 };

        /**
         * flat buffer used to store linked-list "spill" elements
         * each element is [value, next_ptr]
         */
        DynamicVector<int32_t> m_LinkedListElements{};

        /** index of first free element in LinkedListElements */
        int32_t m_FreeHeadIndex{ NullValue };

    public:
        /**
         * @return largest current list index
         */
        [[nodiscard]] size_t Size() const
        {
            return m_ListHeads.GetLength();
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
            m_ListHeads.Clear();
            m_ListBlocks.Clear();
            m_FreeBlocks.Clear();
            m_AllocatedCount = 0;
            m_LinkedListElements.Clear();
            m_FreeHeadIndex = NullValue;
        }

        /**
         * Clearing any lists at or after MaxListIndex and compact the ListBlocks and LinkedListElements so there
         * are no free blocks or free linked list elements
         */
        void Compact( int32_t MaxListIndex );

        /**
         * @return true if a list has been allocated at the given ListIndex
         */
        [[nodiscard]] bool IsAllocated( int32_t ListIndex ) const
        {
            return ( ListIndex >= 0 && ListIndex < static_cast<int32_t>( m_ListHeads.GetLength() ) &&
                     m_ListHeads[ListIndex] != NullValue );
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
        [[nodiscard]] int32_t GetCount( int32_t ListIndex ) const
        {
            assert( ListIndex >= 0 );
            int32_t const block_ptr = m_ListHeads[ListIndex];
            return ( block_ptr == NullValue ) ? 0 : m_ListBlocks[block_ptr];
        }

        /**
         * @return the first item in the list at ListIndex
         * @warning does not check for zero-size-list!
         */
        [[nodiscard]] int32_t First( int32_t ListIndex ) const
        {
            assert( ListIndex >= 0 );
            int32_t const block_ptr = m_ListHeads[ListIndex];
            return m_ListBlocks[block_ptr + 1];
        }

        /**
         * Search for the given Value in list at ListIndex
         * @return true if found
         */
        [[nodiscard]] bool Contains( int32_t ListIndex, int32_t Value ) const;

        /**
         * Search the list at ListIndex for a value where PredicateFunc(value) returns true
         * @return the found value, or the InvalidValue argument if not found
         */
        template <typename IntToBoolFunc>
        [[nodiscard]] int32_t Find( int32_t ListIndex, const IntToBoolFunc& PredicateFunc,
                                    int32_t InvalidValue = -1 ) const
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
                        const int32_t Value = m_ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            return Value;
                        }
                    }
                }
                else
                {
                    // we spilled to linked list, have to iterate through it as well
                    int32_t const iEnd = block_ptr + BLOCKSIZE;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        const int32_t Value = m_ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            return Value;
                        }
                    }
                    int32_t cur_ptr = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    while ( cur_ptr != NullValue )
                    {
                        const int32_t Value = m_LinkedListElements[cur_ptr];
                        if ( PredicateFunc( Value ) )
                        {
                            return Value;
                        }
                        cur_ptr = m_LinkedListElements[cur_ptr + 1];
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
        bool Replace( int32_t ListIndex, const IntToBoolFunc& PredicateFunc, int32_t NewValue )
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
                        const int32_t Value = m_ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            m_ListBlocks[i] = NewValue;
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
                        const int32_t Value = m_ListBlocks[i];
                        if ( PredicateFunc( Value ) )
                        {
                            m_ListBlocks[i] = NewValue;
                            return true;
                        }
                    }
                    int32_t cur_ptr = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    while ( cur_ptr != NullValue )
                    {
                        const int32_t Value = m_LinkedListElements[cur_ptr];
                        if ( PredicateFunc( Value ) )
                        {
                            m_LinkedListElements[cur_ptr] = NewValue;
                            return true;
                        }
                        cur_ptr = m_LinkedListElements[cur_ptr + 1];
                    }
                }
            }
            return false;
        }

        /**
         * Call ApplyFunc on each element of the list at ListIndex
         */
        template <typename IntToVoidFunc>
        void Enumerate( int32_t ListIndex, const IntToVoidFunc& ApplyFunc ) const
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
                        ApplyFunc( m_ListBlocks[i] );
                    }
                }
                else
                {
                    // we spilled to linked list, have to iterate through it as well
                    int32_t const iEnd = block_ptr + BLOCKSIZE;
                    for ( int32_t i = block_ptr + 1; i <= iEnd; ++i )
                    {
                        ApplyFunc( m_ListBlocks[i] );
                    }
                    int32_t cur_ptr = m_ListBlocks[block_ptr + BLOCK_LIST_OFFSET];
                    while ( cur_ptr != NullValue )
                    {
                        ApplyFunc( m_LinkedListElements[cur_ptr] );
                        cur_ptr = m_LinkedListElements[cur_ptr + 1];
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
        void AppendWithElementOffset( const SmallListSet& Other, int32_t ElementOffset );

        /**
         * Call ApplyFunc on each element of the list at ListIndex, until ApplyFunc returns false
         * @return true if all elements were processed and ApplyFunc never returned false
         */
        bool EnumerateEarlyOut( int32_t ListIndex, const std::function<bool( int32_t )>& ApplyFunc ) const;

        friend bool operator==( const SmallListSet& Lhs, const SmallListSet& Rhs )
        {
            if ( Lhs.Size() != Rhs.Size() )
            {
                return false;
            }

            for ( int32_t ListIndex = 0, ListNum = static_cast<int32_t>( Lhs.Size() ); ListIndex < ListNum;
                  ++ListIndex )
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

        friend bool operator!=( const SmallListSet& Lhs, const SmallListSet& Rhs )
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
            BaseValueIterator() = default;

            bool operator==( const BaseValueIterator& Other ) const
            {
                return m_ListSet == Other.m_ListSet && m_ListIndex == Other.m_ListIndex;
            }
            bool operator!=( const BaseValueIterator& Other ) const
            {
                return m_ListSet != Other.m_ListSet || m_ListIndex != Other.m_ListIndex ||
                       m_iCur != Other.m_iCur || m_cur_ptr != Other.m_cur_ptr;
            }

        protected:
            void GotoNext()
            {
                if ( m_N == 0 )
                {
                    SetToEnd();
                    return;
                }
                GotoNextOverflow();
            }

            void GotoNextOverflow()
            {
                if ( m_iCur <= m_iEnd )
                {
                    m_cur_value = m_ListSet->m_ListBlocks[m_iCur];
                    m_iCur++;
                }
                else if ( m_cur_ptr != NullValue )
                {
                    m_cur_value = m_ListSet->m_LinkedListElements[m_cur_ptr];
                    m_cur_ptr   = m_ListSet->m_LinkedListElements[m_cur_ptr + 1];
                }
                else
                {
                    SetToEnd();
                }
            }

            BaseValueIterator( const SmallListSet* ListSetIn, int32_t ListIndex, bool is_end )
                 : m_ListSet( ListSetIn ), m_ListIndex( ListIndex )
            {

                if ( is_end )
                {
                    SetToEnd();
                }
                else
                {
                    m_block_ptr = m_ListSet->m_ListHeads[ListIndex];
                    if ( m_block_ptr != Desert::Geometry::SmallListSet::NullValue )
                    {
                        m_N       = m_ListSet->m_ListBlocks[m_block_ptr];
                        m_iEnd    = ( m_N < BLOCKSIZE ) ? ( m_block_ptr + m_N ) : ( m_block_ptr + BLOCKSIZE );
                        m_iCur    = m_block_ptr + 1;
                        m_cur_ptr = ( m_N < BLOCKSIZE ) ? NullValue
                                                        : m_ListSet->m_ListBlocks[m_block_ptr + BLOCK_LIST_OFFSET];
                        GotoNext();
                    }
                    else
                    {
                        SetToEnd();
                    }
                }
            }

            void SetToEnd()
            {
                m_block_ptr = Desert::Geometry::SmallListSet::NullValue;
                m_N         = 0;
                m_iCur      = -1;
                m_cur_ptr   = -1;
            }

            const SmallListSet* m_ListSet = nullptr;
            int32_t             m_ListIndex{ 0 };
            int32_t             m_block_ptr{};
            int32_t             m_N{};
            int32_t             m_iEnd{};
            int32_t             m_iCur{};
            int32_t             m_cur_ptr{};
            int32_t             m_cur_value = 0;
            friend class SmallListSet;
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

            int32_t operator*() const
            {
                return m_cur_value;
            }

            const ValueIterator& operator++() // prefix
            {
                this->GotoNext();
                return *this;
            }

        protected:
            ValueIterator( const SmallListSet* ListSetIn, int32_t ListIndex, bool is_end )
                 : BaseValueIterator( ListSetIn, ListIndex, is_end )
            {
            }

            friend class SmallListSet;
        };

        /**
         * @return iterator for start of list at ListIndex
         */
        [[nodiscard]] ValueIterator BeginValues( int32_t ListIndex ) const
        {
            return { this, ListIndex, false };
        }

        /**
         * @return iterator for end of list at ListIndex
         */
        [[nodiscard]] ValueIterator EndValues( int32_t ListIndex ) const
        {
            return { this, ListIndex, true };
        }

        /**
         * ValueEnumerable is an object that provides begin/end semantics for a small list, suitable for use with a
         * range-based for loop
         */
        class ValueEnumerable
        {
        public:
            const SmallListSet* m_ListSet = nullptr;
            int32_t             m_ListIndex{};
            ValueEnumerable() = default;
            ValueEnumerable( const SmallListSet* ListSetIn, int32_t ListIndex )
                 : m_ListSet( ListSetIn ), m_ListIndex( ListIndex )
            {
            }
            [[nodiscard]] typename SmallListSet::ValueIterator begin() const
            {
                return m_ListSet->BeginValues( m_ListIndex );
            }
            [[nodiscard]] typename SmallListSet::ValueIterator end() const
            {
                return m_ListSet->EndValues( m_ListIndex );
            }
        };

        /**
         * @return a value enumerable for the given ListIndex
         */
        [[nodiscard]] ValueEnumerable Values( int32_t ListIndex ) const
        {
            return { this, ListIndex };
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
                m_MapFunc = []( int32_t value ) { return value; };
            }

            int32_t operator*() const
            {
                return m_MapFunc( m_cur_value );
            }

            const MappedValueIterator& operator++() // prefix
            {
                this->GotoNext();
                return *this;
            }

        protected:
            MappedValueIterator( const SmallListSet* ListSetIn, int32_t ListIndex, bool is_end,
                                 std::function<int32_t( int32_t )> MapFuncIn )
                 : BaseValueIterator( ListSetIn, ListIndex, is_end ), m_MapFunc( std::move( MapFuncIn ) )
            {
            }

            std::function<int32_t( int32_t )> m_MapFunc;
            friend class SmallListSet;
        };

        /**
         * @return iterator for start of list at ListIndex, with given value mapping function
         */
        MappedValueIterator BeginMappedValues( int32_t                                  ListIndex,
                                               const std::function<int32_t( int32_t )>& MapFunc ) const
        {
            return { this, ListIndex, false, MapFunc };
        }

        /**
         * @return iterator for end of list at ListIndex, with given value mapping function
         */
        MappedValueIterator EndMappedValues( int32_t                                  ListIndex,
                                             const std::function<int32_t( int32_t )>& MapFunc ) const
        {
            return { this, ListIndex, true, MapFunc };
        }

        /**
         * MappedValueEnumerable is an object that provides begin/end semantics for a small list, suitable for use
         * with a range-based for loop
         */
        class MappedValueEnumerable
        {
        public:
            const SmallListSet*               m_ListSet = nullptr;
            int32_t                           m_ListIndex{};
            std::function<int32_t( int32_t )> m_MapFunc;
            MappedValueEnumerable() = default;
            MappedValueEnumerable( const SmallListSet* ListSetIn, int32_t ListIndex,
                                   std::function<int32_t( int32_t )> MapFunc )
                 : m_ListSet( ListSetIn ), m_ListIndex( ListIndex ), m_MapFunc( std::move( MapFunc ) )
            {
            }
            [[nodiscard]] typename SmallListSet::MappedValueIterator begin() const
            {
                return m_ListSet->BeginMappedValues( m_ListIndex, m_MapFunc );
            }
            [[nodiscard]] typename SmallListSet::MappedValueIterator end() const
            {
                return m_ListSet->EndMappedValues( m_ListIndex, m_MapFunc );
            }
        };

        /**
         * @return a value enumerable for the given ListIndex, with the given value mapping function
         */
        MappedValueEnumerable MappedValues( int32_t ListIndex, std::function<int32_t( int32_t )> MapFunc ) const
        {
            return { this, ListIndex, std::move( MapFunc ) };
        }

    protected:
        // grab a block from the free list, or allocate a new one
        int32_t AllocateBlock();

        // push a link-node onto the free list
        void AddFreeLink( int32_t ptr )
        {
            m_LinkedListElements[ptr + 1] = m_FreeHeadIndex;
            m_FreeHeadIndex               = ptr;
        }

        // remove val from the linked-list attached to block_ptr
        bool RemoveFromLinkedList( int32_t block_ptr, int32_t val );

    public:
        [[nodiscard]] size_t GetByteCount() const
        {
            return m_ListHeads.GetByteCount() + m_FreeBlocks.GetByteCount() + m_ListBlocks.GetByteCount() +
                   m_LinkedListElements.GetByteCount();
        }
    };

} // namespace Desert::Geometry
