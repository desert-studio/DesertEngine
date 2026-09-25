// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/IndexTypes.h:9-414, adapted: namespace
// Desert::Geometry; the anonymous-struct unions became plain members (anonymous structs are a compiler extension)
// with operator[] selecting the member; FArchive serialization, FIntVector conversion and TCanBulkSerialize not
// ported; GetTypeHash became std::hash.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

namespace Desert::Geometry
{
    namespace IndexConstants
    {
        inline constexpr int InvalidID = -1;
    }

    /**
     * 2-index tuple. Ported from g3Sharp library, with the intention of
     * maintaining compatibility with existing g3Sharp code. Has an API
     * similar to WildMagic, GTEngine, Eigen, etc.
     */
    struct FIndex2i
    {
        int A = IndexConstants::InvalidID;
        int B = IndexConstants::InvalidID;

        constexpr FIndex2i() = default;
        constexpr FIndex2i( int ValA, int ValB ) : A( ValA ), B( ValB )
        {
        }

        constexpr static FIndex2i Zero()
        {
            return FIndex2i( 0, 0 );
        }
        constexpr static FIndex2i Max()
        {
            return FIndex2i( std::numeric_limits<int>::max(), std::numeric_limits<int>::max() );
        }
        constexpr static FIndex2i Invalid()
        {
            return FIndex2i( IndexConstants::InvalidID, IndexConstants::InvalidID );
        }

        int& operator[]( int Idx )
        {
            return Idx == 0 ? A : B;
        }
        const int& operator[]( int Idx ) const
        {
            return Idx == 0 ? A : B;
        }

        inline bool operator==( const FIndex2i& Other ) const
        {
            return A == Other.A && B == Other.B;
        }
        inline bool operator!=( const FIndex2i& Other ) const
        {
            return A != Other.A || B != Other.B;
        }

        int IndexOf( int Value ) const
        {
            return ( A == Value ) ? 0 : ( ( B == Value ) ? 1 : -1 );
        }
        bool Contains( int Value ) const
        {
            return ( A == Value ) || ( B == Value );
        }

        /** @return whichever of A or B is not Value, or IndexConstants::InvalidID if neither is Value */
        int OtherElement( int Value ) const
        {
            if ( A == Value )
            {
                return B;
            }
            else if ( B == Value )
            {
                return A;
            }
            else
            {
                return IndexConstants::InvalidID;
            }
        }

        inline void Swap()
        {
            std::swap( A, B );
        }

        inline void Sort()
        {
            if ( A > B )
            {
                Swap();
            }
        }
    };

    /**
     * 3-index tuple. Ported from g3Sharp library, with the intention of
     * maintaining compatibility with existing g3Sharp code. Has an API
     * similar to WildMagic, GTEngine, Eigen, etc.
     */
    struct FIndex3i
    {
        int A = IndexConstants::InvalidID;
        int B = IndexConstants::InvalidID;
        int C = IndexConstants::InvalidID;

        constexpr FIndex3i() = default;
        constexpr FIndex3i( int ValA, int ValB, int ValC ) : A( ValA ), B( ValB ), C( ValC )
        {
        }

        constexpr static FIndex3i Zero()
        {
            return FIndex3i( 0, 0, 0 );
        }
        constexpr static FIndex3i Max()
        {
            return FIndex3i( std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                             std::numeric_limits<int>::max() );
        }
        constexpr static FIndex3i Invalid()
        {
            return FIndex3i( IndexConstants::InvalidID, IndexConstants::InvalidID, IndexConstants::InvalidID );
        }

        int& operator[]( int Idx )
        {
            return Idx == 0 ? A : ( Idx == 1 ? B : C );
        }
        const int& operator[]( int Idx ) const
        {
            return Idx == 0 ? A : ( Idx == 1 ? B : C );
        }

        bool operator==( const FIndex3i& Other ) const
        {
            return A == Other.A && B == Other.B && C == Other.C;
        }
        bool operator!=( const FIndex3i& Other ) const
        {
            return A != Other.A || B != Other.B || C != Other.C;
        }

        int IndexOf( int Value ) const
        {
            return ( A == Value ) ? 0 : ( ( B == Value ) ? 1 : ( C == Value ? 2 : -1 ) );
        }
        bool Contains( int Value ) const
        {
            return ( A == Value ) || ( B == Value ) || ( C == Value );
        }

        // Sort the indices from lowest to highest
        void Sort()
        {
            if ( A > B )
            {
                std::swap( A, B );
            }
            if ( B > C )
            {
                std::swap( B, C );
                if ( A > B )
                {
                    std::swap( A, B );
                }
            }
        }

        /** @return offset triplet, with the OffsetIndicesBy value added to each index */
        [[nodiscard]] FIndex3i GetOffsetBy( int32_t OffsetIndicesBy ) const
        {
            return FIndex3i( A + OffsetIndicesBy, B + OffsetIndicesBy, C + OffsetIndicesBy );
        }

        /**
         * @return shifted triplet such that A=WantIndex0Value, and B,C values maintain the same relative ordering
         */
        FIndex3i GetCycled( int32_t WantIndex0Value ) const
        {
            if ( B == WantIndex0Value )
            {
                return FIndex3i( B, C, A );
            }
            else if ( C == WantIndex0Value )
            {
                return FIndex3i( C, A, B );
            }
            return FIndex3i( A, B, C );
        }
    };

    /**
     * 4-index tuple. Ported from g3Sharp library, with the intention of
     * maintaining compatibility with existing g3Sharp code. Has an API
     * similar to WildMagic, GTEngine, Eigen, etc.
     */
    struct FIndex4i
    {
        // UE leaves FIndex4i uninitialised by default; zero-initialised here so no read is undefined.
        int A = 0;
        int B = 0;
        int C = 0;
        int D = 0;

        FIndex4i() = default;
        FIndex4i( int ValA, int ValB, int ValC, int ValD ) : A( ValA ), B( ValB ), C( ValC ), D( ValD )
        {
        }

        static FIndex4i Zero()
        {
            return FIndex4i( 0, 0, 0, 0 );
        }
        static FIndex4i Max()
        {
            return FIndex4i( std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                             std::numeric_limits<int>::max(), std::numeric_limits<int>::max() );
        }
        static FIndex4i Invalid()
        {
            return FIndex4i( IndexConstants::InvalidID, IndexConstants::InvalidID, IndexConstants::InvalidID,
                             IndexConstants::InvalidID );
        }

        int& operator[]( int Idx )
        {
            return Idx == 0 ? A : ( Idx == 1 ? B : ( Idx == 2 ? C : D ) );
        }
        const int& operator[]( int Idx ) const
        {
            return Idx == 0 ? A : ( Idx == 1 ? B : ( Idx == 2 ? C : D ) );
        }

        bool operator==( const FIndex4i& Other ) const
        {
            return A == Other.A && B == Other.B && C == Other.C && D == Other.D;
        }
        bool operator!=( const FIndex4i& Other ) const
        {
            return A != Other.A || B != Other.B || C != Other.C || D != Other.D;
        }

        int IndexOf( int Value ) const
        {
            return ( A == Value ) ? 0
                                  : ( ( B == Value ) ? 1 : ( ( C == Value ) ? 2 : ( ( D == Value ) ? 3 : -1 ) ) );
        }
        bool Contains( int Idx ) const
        {
            return A == Idx || B == Idx || C == Idx || D == Idx;
        }
    };

} // namespace Desert::Geometry

// UE hashes these with a CRC of the bytes; any well-mixed hash serves TSet/TMap the same.
template <>
struct std::hash<Desert::Geometry::FIndex2i>
{
    size_t operator()( const Desert::Geometry::FIndex2i& I ) const noexcept
    {
        return std::hash<uint64_t>{}( ( uint64_t( uint32_t( I.A ) ) << 32 ) | uint32_t( I.B ) );
    }
};

template <>
struct std::hash<Desert::Geometry::FIndex3i>
{
    size_t operator()( const Desert::Geometry::FIndex3i& I ) const noexcept
    {
        const size_t H = std::hash<uint64_t>{}( ( uint64_t( uint32_t( I.A ) ) << 32 ) | uint32_t( I.B ) );
        return H ^ ( std::hash<int>{}( I.C ) + 0x9e3779b97f4a7c15ull + ( H << 6 ) + ( H >> 2 ) );
    }
};

template <>
struct std::hash<Desert::Geometry::FIndex4i>
{
    size_t operator()( const Desert::Geometry::FIndex4i& I ) const noexcept
    {
        const size_t H0 = std::hash<uint64_t>{}( ( uint64_t( uint32_t( I.A ) ) << 32 ) | uint32_t( I.B ) );
        const size_t H1 = std::hash<uint64_t>{}( ( uint64_t( uint32_t( I.C ) ) << 32 ) | uint32_t( I.D ) );
        return H0 ^ ( H1 + 0x9e3779b97f4a7c15ull + ( H0 << 6 ) + ( H0 >> 2 ) );
    }
};
