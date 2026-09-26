// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/DynamicVector.h:1-790,969-970, adapted: UE
// Core types via UECore.hpp, namespace Desert::Geometry, FArchive serialization (790-968) not ported.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"
#include "Engine/Geometry/UECore/IndexTypes.hpp"

#include <memory>

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

        DynamicVector( const DynamicVector& Copy ) : CurBlock( Copy.CurBlock ), CurBlockUsed( Copy.CurBlockUsed )
        {
            const int32_t N = static_cast<int32_t>( Copy.Blocks.size() );
            Blocks.reserve( N );
            for ( int32_t k = 0; k < N; ++k )
            {
                Blocks.push_back( std::make_unique<Block>( *Copy.Blocks[k] ) );
            }
        }

        DynamicVector( DynamicVector&& Moved )
             : CurBlock( Moved.CurBlock ), CurBlockUsed( Moved.CurBlockUsed ), Blocks( std::move( Moved.Blocks ) )
        {
            Moved.CurBlock     = 0;
            Moved.CurBlockUsed = 0;
            Moved.AddAllocatedBlock();
        }

        DynamicVector& operator=( const DynamicVector& Copy )
        {
            if ( this != &Copy )
            {
                const int32_t N = static_cast<int32_t>( Copy.Blocks.size() );
                Empty( N );
                CurBlock     = Copy.CurBlock;
                CurBlockUsed = Copy.CurBlockUsed;
                for ( int32_t k = 0; k < N; ++k )
                {
                    Blocks.push_back( std::make_unique<Block>( *Copy.Blocks[k] ) );
                }
            }
            return *this;
        }

        DynamicVector& operator=( DynamicVector&& Moved )
        {
            if ( this != &Moved )
            {
                Empty();

                CurBlock     = Moved.CurBlock;
                CurBlockUsed = Moved.CurBlockUsed;
                Blocks       = std::move( Moved.Blocks );

                Moved.CurBlock     = 0;
                Moved.CurBlockUsed = 0;
                Moved.AddAllocatedBlock();
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
        inline void SetNum( unsigned int Count )
        {
            Resize( Count );
        }

        inline bool IsEmpty() const
        {
            return CurBlock == 0 && CurBlockUsed == 0;
        }
        inline size_t GetLength() const
        {
            return CurBlock * BlockSize + CurBlockUsed;
        }
        inline size_t Num() const
        {
            return GetLength();
        }
        static constexpr int32_t GetBlockSize()
        {
            return BlockSize;
        }
        inline size_t GetByteCount() const
        {
            return static_cast<int32_t>( Blocks.size() ) * BlockSize * sizeof( Type );
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

        inline const Type& Front() const
        {
            assert( CurBlockUsed > 0 );
            return GetElement( 0, 0 );
        }

        inline const Type& Back() const
        {
            assert( CurBlockUsed > 0 );
            return GetElement( CurBlock, CurBlockUsed - 1 );
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
            inline const Type& operator*() const
            {
                return ( *DVector )[Idx];
            }
            inline Type& operator*()
            {
                return ( *DVector )[Idx];
            }
            inline Iterator& operator++() // prefix
            {
                Idx++;
                return *this;
            }
            inline Iterator operator++( int ) // postfix
            {
                Iterator Copy( *this );
                Idx++;
                return Copy;
            }
            inline bool operator==( const Iterator& Itr2 ) const
            {
                return DVector == Itr2.DVector && Idx == Itr2.Idx;
            }
            inline bool operator!=( const Iterator& Itr2 ) const
            {
                return DVector != Itr2.DVector || Idx != Itr2.Idx;
            }

        private:
            friend class DynamicVector;
            Iterator( DynamicVector* DVectorIn, unsigned int IdxIn ) : DVector( DVectorIn ), Idx( IdxIn )
            {
            }
            DynamicVector*  DVector{};
            unsigned int    Idx{ 0 };
        };

        /** @return iterator at beginning of vector */
        Iterator begin()
        {
            return Iterator{ this, 0 };
        }
        /** @return iterator at end of vector */
        Iterator end()
        {
            return Iterator{ this, (unsigned int)GetLength() };
        }

        /*
         * ConstIterator class iterates over values of vector
         */
        class ConstIterator
        {
        public:
            inline const Type& operator*() const
            {
                return ( *DVector )[Idx];
            }
            inline ConstIterator& operator++() // prefix
            {
                Idx++;
                return *this;
            }
            inline ConstIterator operator++( int ) // postfix
            {
                ConstIterator Copy( *this );
                Idx++;
                return Copy;
            }
            inline bool operator==( const ConstIterator& Itr2 ) const
            {
                return DVector == Itr2.DVector && Idx == Itr2.Idx;
            }
            inline bool operator!=( const ConstIterator& Itr2 ) const
            {
                return DVector != Itr2.DVector || Idx != Itr2.Idx;
            }

        private:
            friend class DynamicVector;
            ConstIterator( const DynamicVector* DVectorIn, unsigned int IdxIn )
                 : DVector( DVectorIn ), Idx( IdxIn )
            {
            }
            const DynamicVector*  DVector{};
            unsigned int          Idx{ 0 };
        };

        /** @return iterator at beginning of vector */
        ConstIterator begin() const
        {
            return ConstIterator{ this, 0 };
        }
        /** @return iterator at end of vector */
        ConstIterator end() const
        {
            return ConstIterator{ this, (unsigned int)GetLength() };
        }

    private:
        struct Block
        {
            Type Elements[BlockSize];
        };

        unsigned int CurBlock{ 0 }; //< Current block index; always points to the block with the last item in the
                                    // vector, or is set to zero if the vector is empty.
        unsigned int CurBlockUsed{ 0 }; //< Number of used items in the current block.

        // UE's TArray<Block*> with a delete per block; unique_ptr here, so the vector owns its blocks by type.
        std::vector<std::unique_ptr<Block>> Blocks;

        void AddAllocatedBlock()
        {
            Blocks.push_back( std::make_unique<Block>() );
        }

        void Empty( int32_t NewReservedBlockCount = 0 )
        {
            Blocks.clear();
            Blocks.reserve( NewReservedBlockCount );
        }

        [[nodiscard]] const Type& GetElement( int32_t BlockIndex, int32_t IndexInBlock ) const
        {
            assert( 0 <= BlockIndex && BlockIndex < static_cast<int32_t>( Blocks.size() ) && 0 <= IndexInBlock &&
                    IndexInBlock < BlockSize );
            return Blocks.data()[BlockIndex]->Elements[IndexInBlock];
        }

        Type& GetElement( int32_t BlockIndex, int32_t IndexInBlock )
        {
            return const_cast<Type&>(
                 const_cast<const DynamicVector&>( *this ).GetElement( BlockIndex, IndexInBlock ) );
        }

        void TruncateBlocks( int32_t NewBlockCount, EAllowShrinking AllowShrinking )
        {
            if ( static_cast<int32_t>( Blocks.size() ) - NewBlockCount <= 0 )
            {
                return;
            }

            Blocks.erase( Blocks.begin() + NewBlockCount,
                          Blocks.begin() + NewBlockCount + static_cast<int32_t>( Blocks.size() ) - NewBlockCount );
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
                const uint32_t LhsCurBlock = Lhs.CurBlock;
                for ( uint32_t BlockIndex = 0; BlockIndex < LhsCurBlock; ++BlockIndex )
                {
                    if ( !CompareItems( &Lhs.Blocks[BlockIndex]->Elements[0], &Rhs.Blocks[BlockIndex]->Elements[0],
                                        BlockSize ) )
                    {
                        return false;
                    }
                }
                return CompareItems( &Lhs.Blocks[LhsCurBlock]->Elements[0], &Rhs.Blocks[LhsCurBlock]->Elements[0],
                                     Lhs.CurBlockUsed );
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
            CurBlock                  = Count != 0 ? GetBlockIndex( LastItemIndex ) : 0;
            CurBlockUsed              = Count != 0 ? GetIndexInBlock( LastItemIndex ) + 1 : 0;
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

        inline void Clear()
        {
            Data.Clear();
        }
        inline void Fill( const Type& Value )
        {
            Data.Fill( Value );
        }
        inline void Resize( unsigned int Count )
        {
            Data.Resize( Count * N );
        }
        inline void Resize( unsigned int Count, const Type& InitValue )
        {
            Data.Resize( Count * N, InitValue );
        }
        inline bool IsEmpty() const
        {
            return Data.IsEmpty();
        }
        inline size_t GetLength() const
        {
            return Data.GetLength() / N;
        }
        inline int GetBlockSize() const
        {
            return Data.GetBlockSize();
        }
        inline size_t GetByteCount() const
        {
            return Data.GetByteCount();
        }

        // simple struct to help pass N-dimensional data without presuming a vector type (e.g. just via initializer
        // list)
        struct ElementVectorN
        {
            Type Data[N];
        };

        inline void Add( const ElementVectorN& AddData )
        {
            for ( int i = 0; i < N; i++ )
            {
                Data.Add( AddData.Data[i] );
            }
        }

        inline void PopBack()
        {
            // UE calls PopBack() here, recursing into itself forever; the element vector is what must shrink.
            for ( int i = 0; i < N; i++ )
            {
                Data.PopBack();
            }
        }

        inline void InsertAt( const ElementVectorN& AddData, unsigned int Index )
        {
            for ( int i = 1; i <= N; i++ )
            {
                Data.InsertAt( AddData.Data[N - i], N * ( Index + 1 ) - i );
            }
        }

        inline Type& operator()( unsigned int TopIndex, unsigned int SubIndex )
        {
            return Data[TopIndex * N + SubIndex];
        }
        inline const Type& operator()( unsigned int TopIndex, unsigned int SubIndex ) const
        {
            return Data[TopIndex * N + SubIndex];
        }
        inline void SetVector2( unsigned int TopIndex, const glm::vec<2, Type>& V )
        {
            assert( N >= 2 );
            unsigned int i = TopIndex * N;
            Data[i]        = V.x;
            Data[i + 1]    = V.y;
        }
        inline void SetVector3( unsigned int TopIndex, const glm::vec<3, Type>& V )
        {
            assert( N >= 3 );
            unsigned int i = TopIndex * N;
            Data[i]        = V.x;
            Data[i + 1]    = V.y;
            Data[i + 2]    = V.z;
        }
        inline glm::vec<2, Type> AsVector2( unsigned int TopIndex ) const
        {
            assert( N >= 2 );
            return glm::vec<2, Type>( Data[TopIndex * N + 0], Data[TopIndex * N + 1] );
        }
        inline glm::vec<3, Type> AsVector3( unsigned int TopIndex ) const
        {
            assert( N >= 3 );
            return glm::vec<3, Type>( Data[TopIndex * N + 0], Data[TopIndex * N + 1], Data[TopIndex * N + 2] );
        }
        inline Index2i AsIndex2( unsigned int TopIndex ) const
        {
            assert( N >= 2 );
            return Index2i( (int)Data[TopIndex * N + 0], (int)Data[TopIndex * N + 1] );
        }
        inline Index3i AsIndex3( unsigned int TopIndex ) const
        {
            assert( N >= 3 );
            return Index3i( (int)Data[TopIndex * N + 0], (int)Data[TopIndex * N + 1],
                            (int)Data[TopIndex * N + 2] );
        }
        inline Index4i AsIndex4( unsigned int TopIndex ) const
        {
            assert( N >= 4 );
            return Index4i( (int)Data[TopIndex * N + 0], (int)Data[TopIndex * N + 1], (int)Data[TopIndex * N + 2],
                            (int)Data[TopIndex * N + 3] );
        }

    private:
        DynamicVector<Type> Data;

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
        TruncateBlocks( 1, EAllowShrinking::No );
        CurBlock     = 0;
        CurBlockUsed = 0;
        if ( Blocks.empty() )
        {
            AddAllocatedBlock();
        }
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Fill( const Type& Value )
    {
        for ( uint32_t BlockIndex = 0, NumBlocks = static_cast<int32_t>( Blocks.size() ); BlockIndex < NumBlocks;
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
        int32_t NumBlocksCurrent = static_cast<int32_t>( Blocks.size() );

        // Allocate needed additional blocks.
        while ( NumBlocksCurrent < NumBlocksNeeded )
        {
            AddAllocatedBlock();
            ++NumBlocksCurrent;
        }

        // Remove unneeded blocks.
        if ( NumBlocksCurrent > NumBlocksNeeded )
        {
            TruncateBlocks( NumBlocksNeeded, EAllowShrinking::No );
        }

        // Set current block.
        SetCurBlock( Count );
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Resize( unsigned int Count, const Type& InitValue )
    {
        size_t nCurSize = GetLength();
        Resize( Count );
        for ( unsigned int Index = (unsigned int)nCurSize; Index < Count; ++Index )
        {
            ( *this )[Index] = InitValue;
        }
    }

    template <typename Type, int32_t BlockSize>
    bool DynamicVector<Type, BlockSize>::SetMinimumSize( unsigned int Count, const Type& InitValue )
    {
        size_t nCurSize = GetLength();
        if ( Count <= nCurSize )
        {
            return false;
        }
        Resize( Count );
        for ( unsigned int Index = (unsigned int)nCurSize; Index < Count; ++Index )
        {
            ( *this )[Index] = InitValue;
        }
        return true;
    }

    template <typename Type, int32_t BlockSize>
    void DynamicVector<Type, BlockSize>::Add( const Type& Data )
    {
        assert( size_t( std::numeric_limits<uint32_t>::max() ) >= GetLength() + 1 );
        if ( CurBlockUsed == BlockSize )
        {
            if ( CurBlock == static_cast<unsigned int>( static_cast<int32_t>( Blocks.size() ) - 1 ) )
            {
                AddAllocatedBlock();
            }
            ++CurBlock;
            CurBlockUsed = 0;
        }
        GetElement( CurBlock, CurBlockUsed ) = Data;
        ++CurBlockUsed;
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
        if ( CurBlockUsed > 0 )
        {
            CurBlockUsed--;
        }
        if ( CurBlockUsed == 0 && CurBlock > 0 )
        {
            CurBlock--;
            CurBlockUsed = BlockSize;
        }
    }

    template <typename Type, int32_t BlockSize>
    Type& DynamicVector<Type, BlockSize>::ElementAt( unsigned int Index, Type InitialValue )
    {
        size_t s = GetLength();
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
        size_t s = GetLength();
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
        size_t nCurSize = GetLength();
        InsertAt( AddData, Index );
        // initialize all new values up to (but not including) the inserted index
        for ( unsigned int i = (unsigned int)nCurSize; i < Index; ++i )
        {
            ( *this )[i] = InitValue;
        }
    }

    template <typename Type, int32_t BlockSize>
    template <typename Func>
    void DynamicVector<Type, BlockSize>::Apply( const Func& ApplyFunc )
    {
        for ( uint32_t BlockIndex = 0; BlockIndex <= CurBlock; ++BlockIndex )
        {
            Block*         Block       = Blocks[BlockIndex].get();
            const uint32_t NumElements = BlockIndex < CurBlock ? BlockSize : CurBlockUsed;
            for ( uint32_t ElementIndex = 0; ElementIndex < NumElements; ++ElementIndex )
            {
                ApplyFunc( Block->Elements[ElementIndex] ); // UE indexes the block pointer itself, which cannot
                                                            // compile once instantiated
            }
        }
    }

} // namespace Desert::Geometry
