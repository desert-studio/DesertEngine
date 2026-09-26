// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/DynamicVector.h:1-790,969-970, adapted: UE
// Core types as std/glm, namespace Desert::Geometry, FArchive serialization (790-968) not ported.
#pragma once

#include "Engine/Geometry/MeshCore/IndexTypes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Desert::Geometry
{

    /*
     * Blocked array with fixed, power-of-two sized blocks.
     *
     * Iterator functions suitable for use with range-based for are provided
     */
    template <typename Type, int32_t BlockSize = 512>
    class DynamicVector
    {
        static_assert( BlockSize > 0, "DynamicVector: BlockSize must be larger than zero." );
        static_assert( ( ( BlockSize & ( BlockSize - 1 ) ) == 0 ),
                       "DynamicVector: BlockSize must be a power of two." );

        static constexpr uint32_t NumBitsNeeded( const uint32_t N )
        {
            uint32_t Bits      = 0;
            uint32_t Remaining = N;
            while ( Remaining > 1 )
            {
                Remaining = ( Remaining + 1 ) / 2;
                ++Bits;
            }
            return Bits;
        }

        static constexpr uint32_t GetBlockIndex( const uint32_t Index )
        {
            constexpr int BlockBitsShift = NumBitsNeeded( BlockSize );
            return Index >> BlockBitsShift;
        }

        static constexpr uint32_t GetIndexInBlock( const uint32_t Index )
        {
            constexpr int BlockBitMask = BlockSize - 1;
            return Index & BlockBitMask;
        }

    public:
        using ElementType = Type;

        DynamicVector()
        {
            AddAllocatedBlock();
        }

        DynamicVector( const DynamicVector& Copy )
             : m_CurBlock( Copy.m_CurBlock ), m_CurBlockUsed( Copy.m_CurBlockUsed )
        {
            const auto N = static_cast<int32_t>( Copy.m_Blocks.size() );
            m_Blocks.reserve( N );
            for ( int32_t k = 0; k < N; ++k )
            {
                m_Blocks.push_back( std::make_unique<Block>( *Copy.m_Blocks[k] ) );
            }
        }

        DynamicVector( DynamicVector&& Moved ) noexcept
             : m_CurBlock( Moved.m_CurBlock ), m_CurBlockUsed( Moved.m_CurBlockUsed ),
               m_Blocks( std::move( Moved.m_Blocks ) )
        {
            // A move steals the blocks and allocates nothing: the source is left holding no block at all, a state
            // every mutator accepts (Add/Resize/Clear allocate the first block on demand).
            Moved.m_CurBlock     = 0;
            Moved.m_CurBlockUsed = 0;
        }

        DynamicVector& operator=( const DynamicVector& Copy )
        {
            if ( this != &Copy )
            {
                const auto N = static_cast<int32_t>( Copy.m_Blocks.size() );
                Empty( N );
                m_CurBlock     = Copy.m_CurBlock;
                m_CurBlockUsed = Copy.m_CurBlockUsed;
                for ( int32_t k = 0; k < N; ++k )
                {
                    m_Blocks.push_back( std::make_unique<Block>( *Copy.m_Blocks[k] ) );
                }
            }
            return *this;
        }

        DynamicVector& operator=( DynamicVector&& Moved ) noexcept
        {
            if ( this != &Moved )
            {
                Empty();

                m_CurBlock     = Moved.m_CurBlock;
                m_CurBlockUsed = Moved.m_CurBlockUsed;
                m_Blocks       = std::move( Moved.m_Blocks );
                Moved.m_Blocks
                     .clear(); // move-assignment leaves the source unspecified; make it the blockless state

                Moved.m_CurBlock     = 0;
                Moved.m_CurBlockUsed = 0;
            }
            return *this;
        }

        DynamicVector( const std::vector<Type>& Array )
        {
            const auto N = static_cast<uint32_t>( Array.Num() );
            SetNum( N );
            const Type* ArrayPtr = Array.GetData();
            for ( uint32_t Idx = 0; Idx < N; ++Idx )
            {
                ( *this )[Idx] = ArrayPtr[Idx];
            }
        }

        DynamicVector( std::span<const Type> Array )
        {
            const auto N = static_cast<uint32_t>( Array.Num() );
            SetNum( N );
            const Type* ArrayPtr = Array.GetData();
            for ( uint32_t Idx = 0; Idx < N; ++Idx )
            {
                ( *this )[Idx] = ArrayPtr[Idx];
            }
        }

        ~DynamicVector()
        {
            Empty();
        }

        inline void Clear();
        inline void Fill( const Type& Value );
        inline void Resize( unsigned int Count );
        inline void Resize( unsigned int Count, const Type& InitValue );
        /// Resize if Num() is less than Count; returns true if resize occurred
        inline bool SetMinimumSize( unsigned int Count, const Type& InitValue );
        void        SetNum( unsigned int Count )
        {
            Resize( Count );
        }

        [[nodiscard]] bool IsEmpty() const
        {
            return m_CurBlock == 0 && m_CurBlockUsed == 0;
        }
        [[nodiscard]] size_t GetLength() const
        {
            return m_CurBlock * BlockSize + m_CurBlockUsed;
        }
        [[nodiscard]] size_t Num() const
        {
            return GetLength();
        }
        static constexpr int32_t GetBlockSize()
        {
            return BlockSize;
        }
        [[nodiscard]] size_t GetByteCount() const
        {
            return static_cast<unsigned long>( static_cast<int32_t>( m_Blocks.size() ) * BlockSize ) *
                   sizeof( Type );
        }

        inline void Add( const Type& Data );
        template <int32_t BlockSizeData>
        void        Add( const DynamicVector<Type, BlockSizeData>& Data );
        void        Add( const std::vector<Type>& Data );
        void        Add( std::span<const Type> Data );
        inline void PopBack();

        inline void  InsertAt( const Type& Data, unsigned int Index );
        inline void  InsertAt( const Type& Data, unsigned int Index, const Type& InitValue );
        inline Type& ElementAt( unsigned int Index, Type InitialValue = Type{} );

        [[nodiscard]] const Type& Front() const
        {
            assert( m_CurBlockUsed > 0 );
            return GetElement( 0, 0 );
        }

        [[nodiscard]] const Type& Back() const
        {
            assert( m_CurBlockUsed > 0 );
            return GetElement( m_CurBlock, m_CurBlockUsed - 1 );
        }

        const Type& operator[]( uint32_t Index ) const
        {
            assert( Index < Num() );
            return GetElement( GetBlockIndex( Index ), GetIndexInBlock( Index ) );
        }

        Type& operator[]( uint32_t Index )
        {
            return const_cast<Type&>( const_cast<const DynamicVector&>( *this )[Index] );
        }

        // apply ApplyFunc() to each member sequentially
        template <typename Func>
        void Apply( const Func& ApplyFunc );

        /*
         * Iterator class iterates over values of vector
         */
        class Iterator
        {
        public:
            const Type& operator*() const
            {
                return ( *m_DVector )[m_Idx];
            }
            Type& operator*()
            {
                return ( *m_DVector )[m_Idx];
            }
            Iterator& operator++() // prefix
            {
                m_Idx++;
                return *this;
            }
            Iterator operator++( int ) // postfix
            {
                Iterator Copy( *this );
                m_Idx++;
                return Copy;
            }
            bool operator==( const Iterator& Itr2 ) const
            {
                return m_DVector == Itr2.m_DVector && m_Idx == Itr2.m_Idx;
            }
            bool operator!=( const Iterator& Itr2 ) const
            {
                return m_DVector != Itr2.m_DVector || m_Idx != Itr2.m_Idx;
            }

        private:
            friend class DynamicVector;
            Iterator( DynamicVector* DVectorIn, unsigned int IdxIn ) : m_DVector( DVectorIn ), m_Idx( IdxIn )
            {
            }
            DynamicVector* m_DVector{};
            unsigned int   m_Idx{ 0 };
        };

        /** @return iterator at beginning of vector */
        Iterator begin()
        {
            return Iterator{ this, 0 };
        }
        /** @return iterator at end of vector */
        Iterator end()
        {
            return Iterator{ this, static_cast<unsigned int>( GetLength() ) };
        }

        /*
         * ConstIterator class iterates over values of vector
         */
        class ConstIterator
        {
        public:
            const Type& operator*() const
            {
                return ( *m_DVector )[m_Idx];
            }
            ConstIterator& operator++() // prefix
            {
                m_Idx++;
                return *this;
            }
            ConstIterator operator++( int ) // postfix
            {
                ConstIterator Copy( *this );
                m_Idx++;
                return Copy;
            }
            bool operator==( const ConstIterator& Itr2 ) const
            {
                return m_DVector == Itr2.m_DVector && m_Idx == Itr2.m_Idx;
            }
            bool operator!=( const ConstIterator& Itr2 ) const
            {
                return m_DVector != Itr2.m_DVector || m_Idx != Itr2.m_Idx;
            }

        private:
            friend class DynamicVector;
            ConstIterator( const DynamicVector* DVectorIn, unsigned int IdxIn )
                 : m_DVector( DVectorIn ), m_Idx( IdxIn )
            {
            }
            const DynamicVector* m_DVector{};
            unsigned int         m_Idx{ 0 };
        };

        /** @return iterator at beginning of vector */
        [[nodiscard]] ConstIterator begin() const
        {
            return ConstIterator{ this, 0 };
        }
        /** @return iterator at end of vector */
        [[nodiscard]] ConstIterator end() const
        {
            return ConstIterator{ this, static_cast<unsigned int>( GetLength() ) };
        }

    private:
        struct Block
        {
            Type Elements[BlockSize];
        };

        unsigned int m_CurBlock{ 0 }; //< Current block index; always points to the block with the last item in the
                                      // vector, or is set to zero if the vector is empty.
        unsigned int m_CurBlockUsed{ 0 }; //< Number of used items in the current block.

        // UE's TArray<Block*> with a delete per block; unique_ptr here, so the vector owns its blocks by type.
        std::vector<std::unique_ptr<Block>> m_Blocks;

        void AddAllocatedBlock()
        {
            m_Blocks.push_back( std::make_unique<Block>() );
        }

        void Empty( int32_t NewReservedBlockCount = 0 )
        {
            m_Blocks.clear();
            m_Blocks.reserve( NewReservedBlockCount );
        }

        [[nodiscard]] const Type& GetElement( int32_t BlockIndex, int32_t IndexInBlock ) const
        {
            assert( 0 <= BlockIndex && BlockIndex < static_cast<int32_t>( m_Blocks.size() ) && 0 <= IndexInBlock &&
                    IndexInBlock < BlockSize );
            return m_Blocks.data()[BlockIndex]->Elements[IndexInBlock];
        }

        Type& GetElement( int32_t BlockIndex, int32_t IndexInBlock )
        {
            return const_cast<Type&>(
                 const_cast<const DynamicVector&>( *this ).GetElement( BlockIndex, IndexInBlock ) );
        }

        // UE's EAllowShrinking argument is dropped: every caller passed No, and std::vector::erase never shrinks.
        void TruncateBlocks( int32_t NewBlockCount )
        {
            if ( static_cast<int32_t>( m_Blocks.size() ) - NewBlockCount <= 0 )
            {
                return;
            }

            m_Blocks.erase( m_Blocks.begin() + NewBlockCount, m_Blocks.end() );
        }

        template <int32_t BlockSizeRhs>
        friend bool operator==( const DynamicVector& Lhs, const DynamicVector<Type, BlockSizeRhs>& Rhs )
        {
            if ( Lhs.Num() != Rhs.Num() )
            {
                return false;
            }

            if ( Lhs.IsEmpty() )
            {
                return true;
            }

            if constexpr ( BlockSize == BlockSizeRhs )
            {
                const uint32_t LhsCurBlock = Lhs.m_CurBlock;
                for ( uint32_t BlockIndex = 0; BlockIndex < LhsCurBlock; ++BlockIndex )
                {
                    const auto* LhsBlock = &Lhs.m_Blocks[BlockIndex]->Elements[0];
                    if ( !std::equal( LhsBlock, LhsBlock + BlockSize, &Rhs.m_Blocks[BlockIndex]->Elements[0] ) )
                    {
                        return false;
                    }
                }
                const auto* LhsLast = &Lhs.m_Blocks[LhsCurBlock]->Elements[0];
                return std::equal( LhsLast, LhsLast + Lhs.m_CurBlockUsed, &Rhs.m_Blocks[LhsCurBlock]->Elements[0] );
            }
            else
            {
                for ( int32_t Index = 0, Num = Lhs.Num(); Index < Num; ++Index )
                {
                    if ( !( Lhs[Index] == Rhs[Index] ) )
                    {
                        return false;
                    }
                }

                return true;
            }
        }

        template <int32_t BlockSizeRhs>
        friend bool operator!=( const DynamicVector& Lhs, const DynamicVector<Type, BlockSizeRhs>& Rhs )
        {
            return !( Lhs == Rhs );
        }

        void SetCurBlock( size_t Count )
        {
            // Reset block index for the last item and used item count within the last block.
            // This is similar to what happens when computing the indices in operator[], but we additionally
            // account for (1) the vector being empty and (2) that the used item count within the last block needs
            // to be one more than the index of the last item.
            const auto LastItemIndex  = static_cast<int32_t>( Count - 1 );
            m_CurBlock                = Count != 0 ? GetBlockIndex( LastItemIndex ) : 0;
            m_CurBlockUsed            = Count != 0 ? GetIndexInBlock( LastItemIndex ) + 1 : 0;
        }
    };

    template <class Type, int N>
    class DynamicVectorN
    {
    public:
        DynamicVectorN()                                        = default;
        DynamicVectorN( const DynamicVectorN& Copy )            = default;
        DynamicVectorN( DynamicVectorN&& Moved )                = default;
        DynamicVectorN& operator=( const DynamicVectorN& Copy ) = default;
        DynamicVectorN& operator=( DynamicVectorN&& Moved )     = default;

        void Clear()
        {
            m_Data.Clear();
        }
        void Fill( const Type& Value )
        {
            m_Data.Fill( Value );
        }
        void Resize( unsigned int Count )
        {
            m_Data.Resize( Count * N );
        }
        void Resize( unsigned int Count, const Type& InitValue )
        {
            m_Data.Resize( Count * N, InitValue );
        }
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Data.IsEmpty();
        }
        [[nodiscard]] size_t GetLength() const
        {
            return m_Data.GetLength() / N;
        }
        [[nodiscard]] int GetBlockSize() const
        {
            return m_Data.GetBlockSize();
        }
        [[nodiscard]] size_t GetByteCount() const
        {
            return m_Data.GetByteCount();
        }

        // simple struct to help pass N-dimensional data without presuming a vector type (e.g. just via initializer
        // list)
        struct ElementVectorN
        {
            Type Data[N];
        };

        void Add( const ElementVectorN& AddData )
        {
            for ( int i = 0; i < N; i++ )
            {
                m_Data.Add( AddData.Data[i] );
            }
        }

        void PopBack()
        {
            // UE calls PopBack() here, recursing into itself forever; the element vector is what must shrink.
            for ( int i = 0; i < N; i++ )
            {
                m_Data.PopBack();
            }
        }

        void InsertAt( const ElementVectorN& AddData, unsigned int Index )
        {
            for ( int i = 1; i <= N; i++ )
            {
                m_Data.InsertAt( AddData.Data[N - i], N * ( Index + 1 ) - i );
            }
        }

        Type& operator()( unsigned int TopIndex, unsigned int SubIndex )
        {
            return m_Data[TopIndex * N + SubIndex];
        }
        const Type& operator()( unsigned int TopIndex, unsigned int SubIndex ) const
        {
            return m_Data[TopIndex * N + SubIndex];
        }
        void SetVector2( unsigned int TopIndex, const glm::vec<2, Type>& V )
        {
            assert( N >= 2 );
            const unsigned int i = TopIndex * N;
            m_Data[i]            = V.x;
            m_Data[i + 1]        = V.y;
        }
        void SetVector3( unsigned int TopIndex, const glm::vec<3, Type>& V )
        {
            assert( N >= 3 );
            const unsigned int i = TopIndex * N;
            m_Data[i]            = V.x;
            m_Data[i + 1]        = V.y;
            m_Data[i + 2]        = V.z;
        }
        [[nodiscard]] glm::vec<2, Type> AsVector2( unsigned int TopIndex ) const
        {
            assert( N >= 2 );
            return glm::vec<2, Type>( m_Data[TopIndex * N + 0], m_Data[TopIndex * N + 1] );
        }
        [[nodiscard]] glm::vec<3, Type> AsVector3( unsigned int TopIndex ) const
        {
            assert( N >= 3 );
            return glm::vec<3, Type>( m_Data[TopIndex * N + 0], m_Data[TopIndex * N + 1],
                                      m_Data[TopIndex * N + 2] );
        }
        [[nodiscard]] Index2i AsIndex2( unsigned int TopIndex ) const
        {
            assert( N >= 2 );
            return { static_cast<int>( m_Data[TopIndex * N + 0] ), static_cast<int>( m_Data[TopIndex * N + 1] ) };
        }
        [[nodiscard]] Index3i AsIndex3( unsigned int TopIndex ) const
        {
            assert( N >= 3 );
            return { static_cast<int>( m_Data[TopIndex * N + 0] ), static_cast<int>( m_Data[TopIndex * N + 1] ),
                     static_cast<int>( m_Data[TopIndex * N + 2] ) };
        }
        [[nodiscard]] Index4i AsIndex4( unsigned int TopIndex ) const
        {
            assert( N >= 4 );
            return { static_cast<int>( m_Data[TopIndex * N + 0] ), static_cast<int>( m_Data[TopIndex * N + 1] ),
                     static_cast<int>( m_Data[TopIndex * N + 2] ), static_cast<int>( m_Data[TopIndex * N + 3] ) };
        }

    private:
        DynamicVector<Type> m_Data;

        friend class Iterator;
    };

    template class DynamicVectorN<double, 2>;

    using DynamicVector3f = DynamicVectorN<float, 3>;
    using DynamicVector2f = DynamicVectorN<float, 2>;
    using DynamicVector3d = DynamicVectorN<double, 3>;
    using DynamicVector2d = DynamicVectorN<double, 2>;
    using DynamicVector3i = DynamicVectorN<int, 3>;
    using DynamicVector2i = DynamicVectorN<int, 2>;

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Clear()
    {
        TruncateBlocks( 1 );
        m_CurBlock     = 0;
        m_CurBlockUsed = 0;
        if ( m_Blocks.empty() )
        {
            AddAllocatedBlock();
        }
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Fill( const Type& Value )
    {
        for ( uint32_t BlockIndex = 0, NumBlocks = static_cast<int32_t>( m_Blocks.size() ); BlockIndex < NumBlocks;
              ++BlockIndex )
        {
            const uint32_t NumElementsInBlock =
                 BlockIndex < NumBlocks - 1 ? BlockSize : GetLength() - BlockSize * ( NumBlocks - 1 );
            for ( uint32_t IndexInBlock = 0; IndexInBlock < NumElementsInBlock; ++IndexInBlock )
            {
                GetElement( BlockIndex, IndexInBlock ) = Value;
            }
        }
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Resize( unsigned int Count )
    {
        if ( GetLength() == Count )
        {
            return;
        }

        // Determine how many blocks we need, but make sure we have at least one block available.
        const bool  bCountIsNotMultipleOfBlockSize = Count % BlockSize != 0;
        const int32_t NumBlocksNeeded =
             std::max( 1, static_cast<int32_t>( Count ) / BlockSize + ( bCountIsNotMultipleOfBlockSize ? 1 : 0 ) );

        // Determine how many blocks are currently allocated.
        auto NumBlocksCurrent = static_cast<int32_t>( m_Blocks.size() );

        // Allocate needed additional blocks.
        while ( NumBlocksCurrent < NumBlocksNeeded )
        {
            AddAllocatedBlock();
            ++NumBlocksCurrent;
        }

        // Remove unneeded blocks.
        if ( NumBlocksCurrent > NumBlocksNeeded )
        {
            TruncateBlocks( NumBlocksNeeded );
        }

        // Set current block.
        SetCurBlock( Count );
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Resize( unsigned int Count, const Type& InitValue )
    {
        const size_t nCurSize = GetLength();
        Resize( Count );
        for ( auto Index = static_cast<unsigned int>( nCurSize ); Index < Count; ++Index )
        {
            ( *this )[Index] = InitValue;
        }
    }

    template <typename Type, int32_t BlockSize>
    bool DynamicVector<Type, BlockSize>::SetMinimumSize( unsigned int Count, const Type& InitValue )
    {
        const size_t nCurSize = GetLength();
        if ( Count <= nCurSize )
        {
            return false;
        }
        Resize( Count );
        for ( auto Index = static_cast<unsigned int>( nCurSize ); Index < Count; ++Index )
        {
            ( *this )[Index] = InitValue;
        }
        return true;
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Add( const Type& Data )
    {
        assert( size_t( std::numeric_limits<uint32_t>::max() ) >= GetLength() + 1 );
        if ( m_Blocks.empty() )
        {
            // Moved-from vector: it gave its blocks away, so the first element needs a fresh one.
            AddAllocatedBlock();
        }
        else if ( m_CurBlockUsed == BlockSize )
        {
            if ( m_CurBlock == static_cast<unsigned int>( static_cast<int32_t>( m_Blocks.size() ) - 1 ) )
            {
                AddAllocatedBlock();
            }
            ++m_CurBlock;
            m_CurBlockUsed = 0;
        }
        GetElement( m_CurBlock, m_CurBlockUsed ) = Data;
        ++m_CurBlockUsed;
    }

    template <typename Type, int32_t BlockSize>
    template <int32_t BlockSizeData>
    void DynamicVector<Type, BlockSize>::Add( const DynamicVector<Type, BlockSizeData>& Data )
    {
        const uint32_t Offset  = Num();
        const auto     DataNum = static_cast<uint32_t>( Data.Num() );
        SetNum( Offset + DataNum );
        for ( uint32_t DataIndex = 0; DataIndex < DataNum; ++DataIndex )
        {
            ( *this )[Offset + DataIndex] = Data[DataIndex];
        }
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Add( const std::vector<Type>& Data )
    {
        const uint32_t Offset  = Num();
        const auto     DataNum = static_cast<uint32_t>( Data.Num() );
        SetNum( Offset + DataNum );
        for ( uint32_t DataIndex = 0; DataIndex < DataNum; ++DataIndex )
        {
            ( *this )[Offset + DataIndex] = Data[DataIndex];
        }
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Add( std::span<const Type> Data )
    {
        const uint32_t Offset  = Num();
        const auto     DataNum = static_cast<uint32_t>( Data.Num() );
        SetNum( Offset + DataNum );
        for ( uint32_t DataIndex = 0; DataIndex < DataNum; ++DataIndex )
        {
            ( *this )[Offset + DataIndex] = Data[DataIndex];
        }
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::PopBack()
    {
        if ( m_CurBlockUsed > 0 )
        {
            m_CurBlockUsed--;
        }
        if ( m_CurBlockUsed == 0 && m_CurBlock > 0 )
        {
            m_CurBlock--;
            m_CurBlockUsed = BlockSize;
        }
    }

    template <typename Type, int32_t BlockSize>
    Type& DynamicVector<Type, BlockSize>::ElementAt( unsigned int Index, Type InitialValue )
    {
        const size_t s = GetLength();
        if ( Index == s )
        {
            Add( InitialValue );
        }
        else if ( Index > s )
        {
            Resize( Index );
            Add( InitialValue );
        }
        return ( *this )[Index];
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::InsertAt( const Type& Data, unsigned int Index )
    {
        const size_t s = GetLength();
        if ( Index == s )
        {
            Add( Data );
        }
        else if ( Index > s )
        {
            Resize( Index );
            Add( Data );
        }
        else
        {
            ( *this )[Index] = Data;
        }
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::InsertAt( const Type& AddData, unsigned int Index, const Type& InitValue )
    {
        const size_t nCurSize = GetLength();
        InsertAt( AddData, Index );
        // initialize all new values up to (but not including) the inserted index
        for ( auto i = static_cast<unsigned int>( nCurSize ); i < Index; ++i )
        {
            ( *this )[i] = InitValue;
        }
    }

    template <typename Type, int32_t BlockSize>
    template <typename Func>
    void DynamicVector<Type, BlockSize>::Apply( const Func& ApplyFunc )
    {
        if ( IsEmpty() )
        {
            return; // a moved-from vector holds no block to read
        }
        for ( uint32_t BlockIndex = 0; BlockIndex <= m_CurBlock; ++BlockIndex )
        {
            Block*         Block       = m_Blocks[BlockIndex].get();
            const uint32_t NumElements = BlockIndex < m_CurBlock ? BlockSize : m_CurBlockUsed;
            for ( uint32_t ElementIndex = 0; ElementIndex < NumElements; ++ElementIndex )
            {
                ApplyFunc( Block->Elements[ElementIndex] ); // UE indexes the block pointer itself, which cannot
                                                            // compile once instantiated
            }
        }
    }

} // namespace Desert::Geometry
