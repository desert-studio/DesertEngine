// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/RefCountVector.h:1-541,603-668, adapted: UE
// Core types via UECore.hpp, namespace Desert::Geometry, FString UsageStats and FArchive serialization (532-602)
// not ported. Port of geometry3cpp RefCountVector

#pragma once

#include "Engine/Geometry/UECore/DynamicVector.hpp"
#include "Engine/Geometry/UECore/IteratorUtil.hpp"

namespace Desert::Geometry
{

    /**
     * RefCountVector is used to keep track of which indices in a linear Index list are in use/referenced.
     * A free list is tracked so that unreferenced indices can be re-used.
     *
     * The enumerator iterates over valid indices (ie where refcount > 0)
     * @warning refcounts are 16-bit ints (shorts) so the maximum count is 65536. behavior is undefined if this
     * overflows.
     * @warning No overflow checking is done in release builds.
     */
    class RefCountVector
    {
    public:
        static constexpr unsigned short INVALID_REF_COUNT = std::numeric_limits<uint16_t>::max();

        RefCountVector()                        = default;
        RefCountVector( const RefCountVector& ) = default;
        RefCountVector( RefCountVector&& From )
             : m_RefCounts( std::move( From.m_RefCounts ) ), m_FreeIndices( std::move( From.m_FreeIndices ) ),
               m_UsedCount( From.m_UsedCount )
        {
            From.m_UsedCount = 0;
        }
        RefCountVector& operator=( const RefCountVector& ) = default;
        RefCountVector& operator=( RefCountVector&& From )
        {
            m_RefCounts      = std::move( From.m_RefCounts );
            m_FreeIndices    = std::move( From.m_FreeIndices );
            m_UsedCount      = From.m_UsedCount;
            From.m_UsedCount = 0;
            return *this;
        }

        bool IsEmpty() const
        {
            return m_UsedCount == 0;
        }

        size_t GetCount() const
        {
            return m_UsedCount;
        }

        size_t GetMaxIndex() const
        {
            return m_RefCounts.GetLength();
        }

        bool IsDense() const
        {
            return m_FreeIndices.GetLength() == 0;
        }

        bool IsValid( int Index ) const
        {
            return ( Index >= 0 && Index < (int)m_RefCounts.GetLength() && IsValidUnsafe( Index ) );
        }

        bool IsValidUnsafe( int Index ) const
        {
            return m_RefCounts[Index] > 0 && m_RefCounts[Index] < INVALID_REF_COUNT;
        }

        int GetRefCount( int Index ) const
        {
            int n = m_RefCounts[Index];
            return ( n == INVALID_REF_COUNT ) ? 0 : n;
        }

        int GetRawRefCount( int Index ) const
        {
            return m_RefCounts[Index];
        }

        // Append all ref counts from another RefCountVector, offsetting FreeIndices to refer to their corresponded
        // new array positions Note this does not try to 'fill in' original free indices, by design -- all existing
        // IDs remain untouched, and all new IDs are simply offsets of the Other's IDs
        void Append( const RefCountVector& Other )
        {
            size_t OrigNum  = m_RefCounts.Num();
            size_t OrigFree = m_FreeIndices.Num();
            m_RefCounts.Add( Other.m_RefCounts );
            m_FreeIndices.Add( Other.m_FreeIndices );
            for ( size_t Idx = OrigFree, N = m_FreeIndices.Num(); Idx < N; ++Idx )
            {
                m_FreeIndices[Idx] += OrigNum;
            }
            m_UsedCount += Other.m_UsedCount;
        }

        int Allocate()
        {
            m_UsedCount++;
            if ( m_FreeIndices.IsEmpty() )
            {
                m_RefCounts.Add( 1 );
                return (int)m_RefCounts.GetLength() - 1;
            }
            else
            {
                int iFree = INDEX_NONE;
                while ( iFree == INDEX_NONE && m_FreeIndices.IsEmpty() == false )
                {
                    iFree = m_FreeIndices.Back();
                    m_FreeIndices.PopBack();
                }
                if ( iFree != INDEX_NONE )
                {
                    m_RefCounts[iFree] = 1;
                    return iFree;
                }
                else
                {
                    m_RefCounts.Add( 1 );
                    return (int)m_RefCounts.GetLength() - 1;
                }
            }
        }

        int Increment( int Index, unsigned short IncrementCount = 1 )
        {
            assert( m_RefCounts[Index] != INVALID_REF_COUNT );
            m_RefCounts[Index] += IncrementCount;
            return m_RefCounts[Index];
        }

        void Decrement( int Index, unsigned short DecrementCount = 1 )
        {
            assert( m_RefCounts[Index] != INVALID_REF_COUNT && m_RefCounts[Index] >= DecrementCount );
            m_RefCounts[Index] -= DecrementCount;
            if ( m_RefCounts[Index] == 0 )
            {
                m_FreeIndices.Add( Index );
                m_RefCounts[Index] = INVALID_REF_COUNT;
                m_UsedCount--;
            }
        }

        /**
         * allocate at specific Index, which must either be larger than current max Index,
         * or on the free list. If larger, all elements up to this one will be pushed onto
         * free list. otherwise we have to do a linear search through free list.
         * If you are doing many of these, it is likely faster to use
         * AllocateAtUnsafe(), and then RebuildFreeList() after you are done.
         */
        bool AllocateAt( int Index )
        {
            if ( Index >= (int)m_RefCounts.GetLength() )
            {
                int j = (int)m_RefCounts.GetLength();
                while ( j < Index )
                {
                    unsigned short InvalidCount =
                         INVALID_REF_COUNT; // required on older clang because a constexpr can't be passed by ref
                    m_RefCounts.Add( InvalidCount );
                    m_FreeIndices.Add( j );
                    ++j;
                }
                m_RefCounts.Add( 1 );
                m_UsedCount++;
                return true;
            }
            else
            {
                if ( IsValidUnsafe( Index ) )
                {
                    return false;
                }
                int N = (int)m_FreeIndices.GetLength();
                for ( int i = 0; i < N; ++i )
                {
                    if ( m_FreeIndices[i] == Index )
                    {
                        m_FreeIndices[i] = m_FreeIndices.Back();
                        m_FreeIndices.PopBack();
                        m_RefCounts[Index] = 1;
                        m_UsedCount++;
                        return true;
                    }
                }
                return false;
            }
        }

        /**
         * allocate at specific Index, which must be free or larger than current max Index.
         * However, we do not update free list. So, you probably need to do RebuildFreeList() after calling this.
         */
        bool AllocateAtUnsafe( int Index )
        {
            if ( Index >= (int)m_RefCounts.GetLength() )
            {
                int j = (int)m_RefCounts.GetLength();
                while ( j < Index )
                {
                    unsigned short InvalidCount =
                         INVALID_REF_COUNT; // required on older clang because a constexpr can't be passed by ref
                    m_RefCounts.Add( InvalidCount );
                    ++j;
                }
                m_RefCounts.Add( 1 );
                m_UsedCount++;
                return true;
            }
            else
            {
                if ( IsValidUnsafe( Index ) )
                {
                    return false;
                }
                m_RefCounts[Index] = 1;
                m_UsedCount++;
                return true;
            }
        }

        const DynamicVector<unsigned short>& GetRawRefCounts() const
        {
            return m_RefCounts;
        }

        /**
         * @warning you should not use this!
         */
        DynamicVector<unsigned short>& GetRawRefCountsUnsafe()
        {
            return m_RefCounts;
        }

        /**
         * @warning you should not use this!
         */
        void SetRefCountUnsafe( int Index, unsigned short ToCount )
        {
            m_RefCounts[Index] = ToCount;
        }

        /**
         * Rebuilds all reference counts from external source data via callables.
         *
         * For example, we could rebuild ref counts for vertices by iterating over all triangles via the Iterate
         * callable, and in the process any vertex we encounter for the first time gets a ref count of 1 via the
         * AllocateRefCount callable, and any vertex we encounter repeatedly has its ref count incremented by one
         * via the IncrementRefCount callable.
         *
         * @param Num Number of items that get referenced.
         * @param Iterate Callable that iterates over all referenced items in the external source data.
         * @param AllocateRefCount Callable for allocating an item, i.e. an item that is referenced for the first
         * time.
         * @param IncrementRefCount Callable for incrementing an item, i.e. an item that has been referenced
         * before.
         */
        template <typename IterateFunc, typename AllocateRefCountFunc, typename IncrementRefCountFunc>
        void Rebuild( unsigned int Num, IterateFunc&& Iterate, AllocateRefCountFunc&& AllocateRefCount,
                      IncrementRefCountFunc&& IncrementRefCount )
        {
            // Initialize ref counts to the given number of elements.
            m_RefCounts.Resize( Num );
            m_RefCounts.Fill( INVALID_REF_COUNT );
            m_UsedCount = 0;

            // Lambda for updating ref count for a given index. This is passed to the external iterate function.
            const auto UpdateRefCount = [this, &AllocateRefCount, &IncrementRefCount]( int32_t Index )
            {
                unsigned short& RefCount = m_RefCounts[Index];
                if ( RefCount == INVALID_REF_COUNT )
                {
                    // Increase used counter and call external function to initialize value.
                    ++m_UsedCount;
                    std::forward<AllocateRefCountFunc>( AllocateRefCount )( RefCount );
                }
                else
                {
                    // Call external function to initialize value.
                    std::forward<IncrementRefCountFunc>( IncrementRefCount )( RefCount );
                }
            };

            // Call external function that iterates over all external pieces of data, which will in turn call the
            // lambda to update ref counts.
            std::forward<IterateFunc>( Iterate )( UpdateRefCount );

            // Add unused elements to free list.
            const unsigned int FreeIndicesNum = Num - m_UsedCount;
            m_FreeIndices.SetNum( FreeIndicesNum );
            unsigned int FreeIndicesIndex = 0;
            for ( unsigned int Index = 0; ( Index < Num ) & ( FreeIndicesIndex < FreeIndicesNum ); ++Index )
            {
                if ( m_RefCounts[Index] == INVALID_REF_COUNT )
                {
                    m_FreeIndices[FreeIndicesIndex++] = Index;
                }
            }
        }

        void RebuildFreeList()
        {
            m_FreeIndices.Clear();
            m_UsedCount = 0;

            int N = (int)m_RefCounts.GetLength();
            for ( int i = 0; i < N; ++i )
            {
                if ( IsValidUnsafe( i ) )
                {
                    m_UsedCount++;
                }
                else
                {
                    m_FreeIndices.Add( i );
                }
            }
        }

        void Trim( int maxIndex )
        {
            m_FreeIndices.Clear();
            m_RefCounts.Resize( maxIndex );
            m_UsedCount = maxIndex;
        }

        void Clear()
        {
            m_FreeIndices.Clear();
            m_RefCounts.Clear();
            m_UsedCount = 0;
        }

        // initialize and set all refcounts to given value
        void InitDense( int Size, uint16_t RefCountValue = 1 )
        {
            m_FreeIndices.Clear();
            m_RefCounts.Clear();
            m_RefCounts.Resize( Size, RefCountValue );
            m_UsedCount = Size;
        }
        //
        // Iterators
        //

        /**
         * base iterator for indices with valid refcount (skips zero-refcount indices)
         */
        class BaseIterator
        {
        public:
            inline BaseIterator()
            {
                m_Vector    = nullptr;
                m_Index     = 0;
                m_LastIndex = 0;
            }

            inline bool operator==( const BaseIterator& Other ) const
            {
                return m_Index == Other.m_Index;
            }
            inline bool operator!=( const BaseIterator& Other ) const
            {
                return m_Index != Other.m_Index;
            }

        protected:
            inline void goto_next()
            {
                m_Index++;
                while ( m_Index < m_LastIndex && m_Vector->IsValidUnsafe( m_Index ) == false )
                {
                    m_Index++;
                }
            }

            inline BaseIterator( const RefCountVector* VectorIn, int IndexIn, int LastIn )
            {
                m_Vector    = VectorIn;
                m_Index     = IndexIn;
                m_LastIndex = LastIn;
                if ( m_Index != m_LastIndex && m_Vector->IsValidUnsafe( m_Index ) == false )
                {
                    goto_next(); // initialize
                }
            }
            const RefCountVector* m_Vector;
            int                   m_Index;
            int                   m_LastIndex;
            friend class RefCountVector;
        };

        /*
         *  iterator over valid indices (ie non-zero refcount)
         */
        class IndexIterator : public BaseIterator
        {
        public:
            inline IndexIterator() : BaseIterator()
            {
            }

            inline int operator*() const
            {
                return this->m_Index;
            }

            inline IndexIterator& operator++() // prefix
            {
                this->goto_next();
                return *this;
            }
            inline IndexIterator operator++( int ) // postfix
            {
                IndexIterator copy( *this );
                this->goto_next();
                return copy;
            }

        protected:
            inline IndexIterator( const RefCountVector* VectorIn, int Index, int Last )
                 : BaseIterator( VectorIn, Index, Last )
            {
            }
            friend class RefCountVector;
        };

        inline IndexIterator BeginIndices() const
        {
            return IndexIterator( this, (int)0, (int)m_RefCounts.GetLength() );
        }

        inline IndexIterator EndIndices() const
        {
            return IndexIterator( this, (int)m_RefCounts.GetLength(), (int)m_RefCounts.GetLength() );
        }

        /**
         * enumerable object that provides begin()/end() semantics, so
         * you can iterate over valid indices using range-based for loop
         */
        class IndexEnumerable
        {
        public:
            const RefCountVector* m_Vector;
            IndexEnumerable()
            {
                m_Vector = nullptr;
            }
            IndexEnumerable( const RefCountVector* VectorIn )
            {
                m_Vector = VectorIn;
            }
            typename RefCountVector::IndexIterator begin() const
            {
                return m_Vector->BeginIndices();
            }
            typename RefCountVector::IndexIterator end() const
            {
                return m_Vector->EndIndices();
            }
        };

        /**
         * returns iteration object over valid indices
         * usage: for (int idx : indices()) { ... }
         */
        inline IndexEnumerable Indices() const
        {
            return IndexEnumerable( this );
        }

        /*
         * enumerable object that maps indices output by Index_iteration to a second type
         */
        template <typename ToType>
        class MappedEnumerable
        {
        public:
            std::function<ToType( int )> m_MapFunc;
            IndexEnumerable              m_enumerable;

            MappedEnumerable( const IndexEnumerable& enumerable, std::function<ToType( int )> MapFunc )
            {
                this->m_enumerable = enumerable;
                this->m_MapFunc    = MapFunc;
            }

            MappedIterator<int, ToType, IndexIterator> begin()
            {
                return MappedIterator<int, ToType, IndexIterator>( m_enumerable.begin(), m_MapFunc );
            }

            MappedIterator<int, ToType, IndexIterator> end()
            {
                return MappedIterator<int, ToType, IndexIterator>( m_enumerable.end(), m_MapFunc );
            }
        };

        /**
         * returns iteration object over mapping applied to valid indices
         * eg usage: for (glm::dvec3 v : mapped_indices(fn_that_looks_up_mesh_vtx_from_id)) { ... }
         */
        template <typename ToType>
        MappedEnumerable<ToType> MappedIndices( std::function<ToType( int )> MapFunc ) const
        {
            return MappedEnumerable<ToType>( Indices(), MapFunc );
        }

        /*
         * iteration object that maps indices output by Index_iteration to a second type
         */
        class FilteredEnumerable
        {
        public:
            std::function<bool( int )> m_FilterFunc;
            IndexEnumerable            m_enumerable;
            FilteredEnumerable( const IndexEnumerable& enumerable, std::function<bool( int )> FilterFuncIn )
            {
                this->m_enumerable = enumerable;
                this->m_FilterFunc = FilterFuncIn;
            }

            FilteredIterator<int, IndexIterator> begin()
            {
                return FilteredIterator<int, IndexIterator>( m_enumerable.begin(), m_enumerable.end(),
                                                             m_FilterFunc );
            }

            FilteredIterator<int, IndexIterator> end()
            {
                return FilteredIterator<int, IndexIterator>( m_enumerable.end(), m_enumerable.end(),
                                                             m_FilterFunc );
            }
        };

        FilteredEnumerable FilteredIndices( std::function<bool( int )> FilterFunc ) const
        {
            return FilteredEnumerable( Indices(), FilterFunc );
        }

        [[nodiscard]] size_t GetByteCount() const
        {
            return m_RefCounts.GetByteCount() + m_FreeIndices.GetByteCount();
        }

        friend bool operator==( const RefCountVector& Lhs, const RefCountVector& Rhs )
        {
            if ( Lhs.GetCount() != Rhs.GetCount() )
            {
                return false;
            }

            const size_t Num = std::max( Lhs.GetMaxIndex(), Rhs.GetMaxIndex() );
            for ( size_t Idx = 0; Idx < Num; ++Idx )
            {
                const bool LhsIsValid = Lhs.IsValid( Idx );
                if ( LhsIsValid != Rhs.IsValid( Idx ) )
                {
                    return false;
                }
                if ( LhsIsValid && Lhs.GetRefCount( Idx ) != Rhs.GetRefCount( Idx ) )
                {
                    return false;
                }
            }

            return true;
        }

        friend bool operator!=( const RefCountVector& Lhs, const RefCountVector& Rhs )
        {
            return !( Lhs == Rhs );
        }

    private:
        DynamicVector<unsigned short> m_RefCounts{};
        DynamicVector<int>            m_FreeIndices{};
        int                           m_UsedCount{ 0 };
    };

} // namespace Desert::Geometry
