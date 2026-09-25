// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/RefCountVector.h:1-541,603-668, adapted: UE
// Core types via UECore.hpp, namespace Desert::Geometry, FString UsageStats and FArchive serialization (532-602)
// not ported. Port of geometry3cpp FRefCountVector

#pragma once

#include "Engine/Geometry/UECore/DynamicVector.hpp"
#include "Engine/Geometry/UECore/IteratorUtil.hpp"

namespace Desert::Geometry
{

    /**
     * FRefCountVector is used to keep track of which indices in a linear Index list are in use/referenced.
     * A free list is tracked so that unreferenced indices can be re-used.
     *
     * The enumerator iterates over valid indices (ie where refcount > 0)
     * @warning refcounts are 16-bit ints (shorts) so the maximum count is 65536. behavior is undefined if this
     * overflows.
     * @warning No overflow checking is done in release builds.
     */
    class FRefCountVector
    {
    public:
        static constexpr unsigned short INVALID_REF_COUNT = std::numeric_limits<uint16_t>::max();

        FRefCountVector()                         = default;
        FRefCountVector( const FRefCountVector& ) = default;
        FRefCountVector( FRefCountVector&& From )
             : RefCounts( std::move( From.RefCounts ) ), FreeIndices( std::move( From.FreeIndices ) ),
               UsedCount( From.UsedCount )
        {
            From.UsedCount = 0;
        }
        FRefCountVector& operator=( const FRefCountVector& ) = default;
        FRefCountVector& operator=( FRefCountVector&& From )
        {
            RefCounts      = std::move( From.RefCounts );
            FreeIndices    = std::move( From.FreeIndices );
            UsedCount      = From.UsedCount;
            From.UsedCount = 0;
            return *this;
        }

        bool IsEmpty() const
        {
            return UsedCount == 0;
        }

        size_t GetCount() const
        {
            return UsedCount;
        }

        size_t GetMaxIndex() const
        {
            return RefCounts.GetLength();
        }

        bool IsDense() const
        {
            return FreeIndices.GetLength() == 0;
        }

        bool IsValid( int Index ) const
        {
            return ( Index >= 0 && Index < (int)RefCounts.GetLength() && IsValidUnsafe( Index ) );
        }

        bool IsValidUnsafe( int Index ) const
        {
            return RefCounts[Index] > 0 && RefCounts[Index] < INVALID_REF_COUNT;
        }

        int GetRefCount( int Index ) const
        {
            int n = RefCounts[Index];
            return ( n == INVALID_REF_COUNT ) ? 0 : n;
        }

        int GetRawRefCount( int Index ) const
        {
            return RefCounts[Index];
        }

        // Append all ref counts from another RefCountVector, offsetting FreeIndices to refer to their corresponded
        // new array positions Note this does not try to 'fill in' original free indices, by design -- all existing
        // IDs remain untouched, and all new IDs are simply offsets of the Other's IDs
        void Append( const FRefCountVector& Other )
        {
            size_t OrigNum  = RefCounts.Num();
            size_t OrigFree = FreeIndices.Num();
            RefCounts.Add( Other.RefCounts );
            FreeIndices.Add( Other.FreeIndices );
            for ( size_t Idx = OrigFree, N = FreeIndices.Num(); Idx < N; ++Idx )
            {
                FreeIndices[Idx] += OrigNum;
            }
            UsedCount += Other.UsedCount;
        }

        int Allocate()
        {
            UsedCount++;
            if ( FreeIndices.IsEmpty() )
            {
                RefCounts.Add( 1 );
                return (int)RefCounts.GetLength() - 1;
            }
            else
            {
                int iFree = INDEX_NONE;
                while ( iFree == INDEX_NONE && FreeIndices.IsEmpty() == false )
                {
                    iFree = FreeIndices.Back();
                    FreeIndices.PopBack();
                }
                if ( iFree != INDEX_NONE )
                {
                    RefCounts[iFree] = 1;
                    return iFree;
                }
                else
                {
                    RefCounts.Add( 1 );
                    return (int)RefCounts.GetLength() - 1;
                }
            }
        }

        int Increment( int Index, unsigned short IncrementCount = 1 )
        {
            UE_CHECK_SLOW( RefCounts[Index] != INVALID_REF_COUNT );
            RefCounts[Index] += IncrementCount;
            return RefCounts[Index];
        }

        void Decrement( int Index, unsigned short DecrementCount = 1 )
        {
            UE_CHECK_SLOW( RefCounts[Index] != INVALID_REF_COUNT && RefCounts[Index] >= DecrementCount );
            RefCounts[Index] -= DecrementCount;
            if ( RefCounts[Index] == 0 )
            {
                FreeIndices.Add( Index );
                RefCounts[Index] = INVALID_REF_COUNT;
                UsedCount--;
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
            if ( Index >= (int)RefCounts.GetLength() )
            {
                int j = (int)RefCounts.GetLength();
                while ( j < Index )
                {
                    unsigned short InvalidCount =
                         INVALID_REF_COUNT; // required on older clang because a constexpr can't be passed by ref
                    RefCounts.Add( InvalidCount );
                    FreeIndices.Add( j );
                    ++j;
                }
                RefCounts.Add( 1 );
                UsedCount++;
                return true;
            }
            else
            {
                if ( IsValidUnsafe( Index ) )
                {
                    return false;
                }
                int N = (int)FreeIndices.GetLength();
                for ( int i = 0; i < N; ++i )
                {
                    if ( FreeIndices[i] == Index )
                    {
                        FreeIndices[i] = FreeIndices.Back();
                        FreeIndices.PopBack();
                        RefCounts[Index] = 1;
                        UsedCount++;
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
            if ( Index >= (int)RefCounts.GetLength() )
            {
                int j = (int)RefCounts.GetLength();
                while ( j < Index )
                {
                    unsigned short InvalidCount =
                         INVALID_REF_COUNT; // required on older clang because a constexpr can't be passed by ref
                    RefCounts.Add( InvalidCount );
                    ++j;
                }
                RefCounts.Add( 1 );
                UsedCount++;
                return true;
            }
            else
            {
                if ( IsValidUnsafe( Index ) )
                {
                    return false;
                }
                RefCounts[Index] = 1;
                UsedCount++;
                return true;
            }
        }

        const TDynamicVector<unsigned short>& GetRawRefCounts() const
        {
            return RefCounts;
        }

        /**
         * @warning you should not use this!
         */
        TDynamicVector<unsigned short>& GetRawRefCountsUnsafe()
        {
            return RefCounts;
        }

        /**
         * @warning you should not use this!
         */
        void SetRefCountUnsafe( int Index, unsigned short ToCount )
        {
            RefCounts[Index] = ToCount;
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
            RefCounts.Resize( Num );
            RefCounts.Fill( INVALID_REF_COUNT );
            UsedCount = 0;

            // Lambda for updating ref count for a given index. This is passed to the external iterate function.
            const auto UpdateRefCount = [this, &AllocateRefCount, &IncrementRefCount]( int32_t Index )
            {
                unsigned short& RefCount = RefCounts[Index];
                if ( RefCount == INVALID_REF_COUNT )
                {
                    // Increase used counter and call external function to initialize value.
                    ++UsedCount;
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
            const unsigned int FreeIndicesNum = Num - UsedCount;
            FreeIndices.SetNum( FreeIndicesNum );
            unsigned int FreeIndicesIndex = 0;
            for ( unsigned int Index = 0; ( Index < Num ) & ( FreeIndicesIndex < FreeIndicesNum ); ++Index )
            {
                if ( RefCounts[Index] == INVALID_REF_COUNT )
                {
                    FreeIndices[FreeIndicesIndex++] = Index;
                }
            }
        }

        void RebuildFreeList()
        {
            FreeIndices.Clear();
            UsedCount = 0;

            int N = (int)RefCounts.GetLength();
            for ( int i = 0; i < N; ++i )
            {
                if ( IsValidUnsafe( i ) )
                {
                    UsedCount++;
                }
                else
                {
                    FreeIndices.Add( i );
                }
            }
        }

        void Trim( int maxIndex )
        {
            FreeIndices.Clear();
            RefCounts.Resize( maxIndex );
            UsedCount = maxIndex;
        }

        void Clear()
        {
            FreeIndices.Clear();
            RefCounts.Clear();
            UsedCount = 0;
        }

        // initialize and set all refcounts to given value
        void InitDense( int Size, uint16_t RefCountValue = 1 )
        {
            FreeIndices.Clear();
            RefCounts.Clear();
            RefCounts.Resize( Size, RefCountValue );
            UsedCount = Size;
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
                Vector    = nullptr;
                Index     = 0;
                LastIndex = 0;
            }

            inline bool operator==( const BaseIterator& Other ) const
            {
                return Index == Other.Index;
            }
            inline bool operator!=( const BaseIterator& Other ) const
            {
                return Index != Other.Index;
            }

        protected:
            inline void goto_next()
            {
                Index++;
                while ( Index < LastIndex && Vector->IsValidUnsafe( Index ) == false )
                {
                    Index++;
                }
            }

            inline BaseIterator( const FRefCountVector* VectorIn, int IndexIn, int LastIn )
            {
                Vector    = VectorIn;
                Index     = IndexIn;
                LastIndex = LastIn;
                if ( Index != LastIndex && Vector->IsValidUnsafe( Index ) == false )
                {
                    goto_next(); // initialize
                }
            }
            const FRefCountVector* Vector;
            int                    Index;
            int                    LastIndex;
            friend class FRefCountVector;
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
                return this->Index;
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
            inline IndexIterator( const FRefCountVector* VectorIn, int Index, int Last )
                 : BaseIterator( VectorIn, Index, Last )
            {
            }
            friend class FRefCountVector;
        };

        inline IndexIterator BeginIndices() const
        {
            return IndexIterator( this, (int)0, (int)RefCounts.GetLength() );
        }

        inline IndexIterator EndIndices() const
        {
            return IndexIterator( this, (int)RefCounts.GetLength(), (int)RefCounts.GetLength() );
        }

        /**
         * enumerable object that provides begin()/end() semantics, so
         * you can iterate over valid indices using range-based for loop
         */
        class IndexEnumerable
        {
        public:
            const FRefCountVector* Vector;
            IndexEnumerable()
            {
                Vector = nullptr;
            }
            IndexEnumerable( const FRefCountVector* VectorIn )
            {
                Vector = VectorIn;
            }
            typename FRefCountVector::IndexIterator begin() const
            {
                return Vector->BeginIndices();
            }
            typename FRefCountVector::IndexIterator end() const
            {
                return Vector->EndIndices();
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
            std::function<ToType( int )> MapFunc;
            IndexEnumerable          enumerable;

            MappedEnumerable( const IndexEnumerable& enumerable, std::function<ToType( int )> MapFunc )
            {
                this->enumerable = enumerable;
                this->MapFunc    = MapFunc;
            }

            MappedIterator<int, ToType, IndexIterator> begin()
            {
                return MappedIterator<int, ToType, IndexIterator>( enumerable.begin(), MapFunc );
            }

            MappedIterator<int, ToType, IndexIterator> end()
            {
                return MappedIterator<int, ToType, IndexIterator>( enumerable.end(), MapFunc );
            }
        };

        /**
         * returns iteration object over mapping applied to valid indices
         * eg usage: for (FVector3d v : mapped_indices(fn_that_looks_up_mesh_vtx_from_id)) { ... }
         */
        template <typename ToType>
        inline MappedEnumerable<ToType> MappedIndices( std::function<ToType( int )> MapFunc ) const
        {
            return MappedEnumerable<ToType>( Indices(), MapFunc );
        }

        /*
         * iteration object that maps indices output by Index_iteration to a second type
         */
        class FilteredEnumerable
        {
        public:
            std::function<bool( int )> FilterFunc;
            IndexEnumerable        enumerable;
            FilteredEnumerable( const IndexEnumerable& enumerable, std::function<bool( int )> FilterFuncIn )
            {
                this->enumerable = enumerable;
                this->FilterFunc = FilterFuncIn;
            }

            FilteredIterator<int, IndexIterator> begin()
            {
                return FilteredIterator<int, IndexIterator>( enumerable.begin(), enumerable.end(), FilterFunc );
            }

            FilteredIterator<int, IndexIterator> end()
            {
                return FilteredIterator<int, IndexIterator>( enumerable.end(), enumerable.end(), FilterFunc );
            }
        };

        inline FilteredEnumerable FilteredIndices( std::function<bool( int )> FilterFunc ) const
        {
            return FilteredEnumerable( Indices(), FilterFunc );
        }

        size_t GetByteCount() const
        {
            return RefCounts.GetByteCount() + FreeIndices.GetByteCount();
        }

        friend bool operator==( const FRefCountVector& Lhs, const FRefCountVector& Rhs )
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

        friend bool operator!=( const FRefCountVector& Lhs, const FRefCountVector& Rhs )
        {
            return !( Lhs == Rhs );
        }

    private:
        TDynamicVector<unsigned short> RefCounts{};
        TDynamicVector<int>            FreeIndices{};
        int                            UsedCount{ 0 };
    };

} // namespace Desert::Geometry
