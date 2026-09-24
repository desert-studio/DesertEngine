// UE Core shim for the ported GeometryCore (FDynamicMesh3 and its containers).
// Not a port of a single UE file: it re-expresses, over std and glm, exactly the subset of UE Core
// (Runtime/Core: Containers/Array.h, ArrayView.h, Set.h, Map.h, Templates/Function.h,
// Templates/UnrealTemplate.h, Math/NumericLimits.h, Math/UnrealMathUtility.h, Math/Vector.h,
// Math/Vector2D.h, Misc/AssertionMacros.h) that the ported sources call, under the UE names, so the
// ported algorithm bodies stay line-for-line comparable with UE. Grow it only when a port needs it.
#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// UE's check/checkSlow/checkf/ensure, renamed: a bare `check(` macro would rewrite every
// `x.check(` in any translation unit that includes a ported header (and collides with
// Apple's AssertMacros.h). checkSlow is a debug-only assert in UE, as here.
#define UE_CHECK( Expr ) assert( Expr )
#define UE_CHECK_SLOW( Expr ) assert( Expr )
#define UE_CHECKF( Expr, Format, ... ) assert( ( Expr ) && Format )
// ensure() evaluates to the condition in UE so it can guard a branch, and unlike check() it is NOT fatal:
// UE reports and carries on into the recovery branch (e.g. RemoveTriangle on a dead id returns
// Failed_NotATriangle). So it reports to stderr and returns the value; the condition is evaluated once.
#define UE_ENSURE( Expr )                                                                                         \
    ::Desert::Geometry::UEEnsureReport( static_cast<bool>( Expr ), #Expr, __FILE__, __LINE__ )
#define UE_ENSURE_MSGF( Expr, Format, ... ) UE_ENSURE( Expr )

namespace Desert::Geometry
{
    inline bool UEEnsureReport( const bool bCondition, const char* Expr, const char* File, const int Line )
    {
        if ( !bCondition )
        {
            std::fprintf( stderr, "ensure(%s) failed at %s:%d\n", Expr, File, Line );
        }
        return bCondition;
    }

    using int8   = std::int8_t;
    using int16  = std::int16_t;
    using int32  = std::int32_t;
    using int64  = std::int64_t;
    using uint8  = std::uint8_t;
    using uint16 = std::uint16_t;
    using uint32 = std::uint32_t;
    using uint64 = std::uint64_t;
    using SIZE_T = std::size_t;

    inline constexpr int32  INDEX_NONE = -1;
    inline constexpr uint16 MAX_uint16 = std::numeric_limits<uint16>::max();
    inline constexpr uint32 MAX_uint32 = std::numeric_limits<uint32>::max();

    enum class EAllowShrinking : uint8
    {
        No,
        Yes
    };

    template <typename T>
    constexpr std::remove_reference_t<T>&& MoveTemp( T&& Value ) noexcept
    {
        return std::move( Value );
    }

    template <typename T>
    constexpr T&& Forward( std::remove_reference_t<T>& Value ) noexcept
    {
        return std::forward<T>( Value );
    }

    template <typename T>
    inline void Swap( T& A, T& B )
    {
        std::swap( A, B );
    }

    // Templates/MemoryOps.h: element-wise equality of two runs.
    template <typename T>
    bool CompareItems( const T* A, const T* B, SIZE_T Count )
    {
        return std::equal( A, A + Count, B );
    }

    template <typename Signature>
    using TFunction = std::function<Signature>;
    // UE's TFunctionRef is non-owning; std::function owns a copy. Same call semantics, and the
    // ported code only ever passes it down a call chain, never stores it.
    template <typename Signature>
    using TFunctionRef = std::function<Signature>;

    template <typename T>
    struct TNumericLimits
    {
        static constexpr T Min()
        {
            return std::numeric_limits<T>::min();
        }
        static constexpr T Max()
        {
            return std::numeric_limits<T>::max();
        }
        static constexpr T Lowest()
        {
            return std::numeric_limits<T>::lowest();
        }
    };

    struct FMath
    {
        template <typename T>
        static constexpr T Min( const T A, const T B )
        {
            return ( A <= B ) ? A : B;
        }
        template <typename T>
        static constexpr T Max( const T A, const T B )
        {
            return ( A >= B ) ? A : B;
        }
        template <typename T>
        static constexpr T Clamp( const T X, const T Lo, const T Hi )
        {
            return ( X < Lo ) ? Lo : ( X < Hi ? X : Hi );
        }
        template <typename T>
        static constexpr T Abs( const T A )
        {
            return ( A < T( 0 ) ) ? -A : A;
        }
        template <typename T>
        static T Sqrt( const T A )
        {
            return std::sqrt( A );
        }
    };

    // TArray over std::vector: UE's names on top, index type int32 as in UE.
    template <typename T>
    class TArray
    {
    public:
        using ElementType = T;

        TArray() = default;
        TArray( std::initializer_list<T> Init ) : Data( Init )
        {
        }

        int32 Num() const
        {
            return static_cast<int32>( Data.size() );
        }
        bool IsEmpty() const
        {
            return Data.empty();
        }
        bool IsValidIndex( int32 Index ) const
        {
            return Index >= 0 && Index < Num();
        }
        T* GetData()
        {
            return Data.data();
        }
        const T* GetData() const
        {
            return Data.data();
        }

        void SetNumZeroed( int32 NewNum, EAllowShrinking = EAllowShrinking::Yes )
        {
            Data.assign( static_cast<size_t>( NewNum ), T{} );
        }

        // std::vector's reference type, so TArray<bool> (a bit-vector underneath) indexes too.
        typename std::vector<T>::reference operator[]( int32 Index )
        {
            UE_CHECK_SLOW( IsValidIndex( Index ) );
            return Data[static_cast<size_t>( Index )];
        }
        typename std::vector<T>::const_reference operator[]( int32 Index ) const
        {
            UE_CHECK_SLOW( IsValidIndex( Index ) );
            return Data[static_cast<size_t>( Index )];
        }

        int32 Add( const T& Item )
        {
            Data.push_back( Item );
            return Num() - 1;
        }
        int32 Add( T&& Item )
        {
            Data.push_back( std::move( Item ) );
            return Num() - 1;
        }
        template <typename... Args>
        int32 Emplace( Args&&... InArgs )
        {
            Data.emplace_back( std::forward<Args>( InArgs )... );
            return Num() - 1;
        }
        int32 AddUnique( const T& Item )
        {
            const int32 Found = Find( Item );
            return Found != INDEX_NONE ? Found : Add( Item );
        }
        int32 AddUninitialized( int32 Count = 1 )
        {
            const int32 First = Num();
            Data.resize( Data.size() + static_cast<size_t>( Count ) );
            return First;
        }
        void Append( const TArray& Other )
        {
            Data.insert( Data.end(), Other.Data.begin(), Other.Data.end() );
        }
        void Insert( const T& Item, int32 Index )
        {
            Data.insert( Data.begin() + Index, Item );
        }

        void SetNum( int32 NewNum, EAllowShrinking = EAllowShrinking::Yes )
        {
            Data.resize( static_cast<size_t>( NewNum ) );
        }
        void SetNumUninitialized( int32 NewNum, EAllowShrinking = EAllowShrinking::Yes )
        {
            Data.resize( static_cast<size_t>( NewNum ) );
        }
        void Init( const T& Value, int32 Count )
        {
            Data.assign( static_cast<size_t>( Count ), Value );
        }
        void Reserve( int32 Count )
        {
            Data.reserve( static_cast<size_t>( Count ) );
        }
        void Empty( int32 Slack = 0 )
        {
            std::vector<T>().swap( Data );
            Data.reserve( static_cast<size_t>( Slack ) );
        }
        void Reset( int32 NewSize = 0 )
        {
            Data.clear();
            Data.reserve( static_cast<size_t>( NewSize ) );
        }

        void RemoveAt( int32 Index, int32 Count = 1, EAllowShrinking = EAllowShrinking::Yes )
        {
            UE_CHECK_SLOW( Index >= 0 && Count >= 0 && Index + Count <= Num() );
            Data.erase( Data.begin() + Index, Data.begin() + Index + Count );
        }
        // Order is not kept: the tail fills the hole, as in UE.
        void RemoveAtSwap( int32 Index, int32 Count = 1, EAllowShrinking = EAllowShrinking::Yes )
        {
            UE_CHECK_SLOW( Index >= 0 && Count >= 0 && Index + Count <= Num() );
            for ( int32 k = 0; k < Count; ++k )
            {
                const int32 Last = Num() - 1;
                if ( Index + k != Last )
                    Data[static_cast<size_t>( Index + k )] = std::move( Data[static_cast<size_t>( Last )] );
                Data.pop_back();
            }
        }
        int32 Remove( const T& Item )
        {
            const size_t Before = Data.size();
            Data.erase( std::remove( Data.begin(), Data.end(), Item ), Data.end() );
            return static_cast<int32>( Before - Data.size() );
        }
        T Pop( EAllowShrinking = EAllowShrinking::Yes )
        {
            T Result = std::move( Data.back() );
            Data.pop_back();
            return Result;
        }

        T& Last( int32 IndexFromEnd = 0 )
        {
            return Data[Data.size() - 1 - static_cast<size_t>( IndexFromEnd )];
        }
        const T& Last( int32 IndexFromEnd = 0 ) const
        {
            return Data[Data.size() - 1 - static_cast<size_t>( IndexFromEnd )];
        }

        void Push( const T& Item )
        {
            Data.push_back( Item );
        }
        // Removes the first occurrence, filling the hole with the last element (order not preserved).
        int32 RemoveSingleSwap( const T& Item )
        {
            const int32 Index = Find( Item );
            if ( Index == INDEX_NONE )
            {
                return 0;
            }
            RemoveAtSwap( Index );
            return 1;
        }
        int32 Find( const T& Item ) const
        {
            const auto It = std::find( Data.begin(), Data.end(), Item );
            return It == Data.end() ? INDEX_NONE : static_cast<int32>( It - Data.begin() );
        }
        bool Contains( const T& Item ) const
        {
            return Find( Item ) != INDEX_NONE;
        }

        auto begin()
        {
            return Data.begin();
        }
        auto end()
        {
            return Data.end();
        }
        auto begin() const
        {
            return Data.begin();
        }
        auto end() const
        {
            return Data.end();
        }

        bool operator==( const TArray& Other ) const
        {
            return Data == Other.Data;
        }

    private:
        std::vector<T> Data;
    };

    template <typename T>
    class TArrayView
    {
    public:
        TArrayView() = default;
        TArrayView( const TArray<std::remove_const_t<T>>& Array )
             : View( Array.GetData(), static_cast<size_t>( Array.Num() ) )
        {
        }
        TArrayView( T* InData, int32 InNum ) : View( InData, static_cast<size_t>( InNum ) )
        {
        }

        int32 Num() const
        {
            return static_cast<int32>( View.size() );
        }
        T* GetData() const
        {
            return View.data();
        }
        T& operator[]( int32 Index ) const
        {
            return View[static_cast<size_t>( Index )];
        }
        bool Contains( const std::remove_const_t<T>& Item ) const
        {
            return std::find( View.begin(), View.end(), Item ) != View.end();
        }
        auto begin() const
        {
            return View.begin();
        }
        auto end() const
        {
            return View.end();
        }

    private:
        std::span<T> View;
    };

    template <typename T>
    class TSet
    {
    public:
        void Add( const T& Item )
        {
            Data.insert( Item );
        }
        bool Contains( const T& Item ) const
        {
            return Data.count( Item ) != 0;
        }
        int32 Remove( const T& Item )
        {
            return static_cast<int32>( Data.erase( Item ) );
        }
        int32 Num() const
        {
            return static_cast<int32>( Data.size() );
        }
        void Empty()
        {
            Data.clear();
        }
        void Reserve( int32 Count )
        {
            Data.reserve( static_cast<size_t>( Count ) );
        }
        auto begin() const
        {
            return Data.begin();
        }
        auto end() const
        {
            return Data.end();
        }

    private:
        std::unordered_set<T> Data;
    };

    template <typename K, typename V>
    class TMap
    {
    public:
        V& Add( const K& Key, const V& Value )
        {
            return Data.insert_or_assign( Key, Value ).first->second;
        }
        V* Find( const K& Key )
        {
            auto It = Data.find( Key );
            return It == Data.end() ? nullptr : &It->second;
        }
        const V* Find( const K& Key ) const
        {
            auto It = Data.find( Key );
            return It == Data.end() ? nullptr : &It->second;
        }
        V& FindOrAdd( const K& Key )
        {
            return Data[Key];
        }
        bool Contains( const K& Key ) const
        {
            return Data.count( Key ) != 0;
        }
        int32 Remove( const K& Key )
        {
            return static_cast<int32>( Data.erase( Key ) );
        }
        int32 Num() const
        {
            return static_cast<int32>( Data.size() );
        }
        void Empty()
        {
            Data.clear();
        }
        void Reserve( int32 Count )
        {
            Data.reserve( static_cast<size_t>( Count ) );
        }
        // UE's operator[] asserts the key exists (FindChecked); it never inserts.
        V& operator[]( const K& Key )
        {
            V* Found = Find( Key );
            UE_CHECK( Found != nullptr );
            return *Found;
        }
        auto begin() const
        {
            return Data.begin();
        }
        auto end() const
        {
            return Data.end();
        }

    private:
        std::unordered_map<K, V> Data;
    };

    // UE::Math::TVector / TVector2 with UE's member names (X, Y, Z); positions are double
    // (FVector3d), as in UE's FDynamicMesh3. glm conversions sit at the boundary to the engine.
    // TOptional over std::optional (Misc/Optional.h names).
    template <typename T>
    class TOptional
    {
    public:
        TOptional() = default;
        TOptional( const T& InValue ) : Value( InValue )
        {
        }
        TOptional( T&& InValue ) : Value( std::move( InValue ) )
        {
        }
        bool IsSet() const
        {
            return Value.has_value();
        }
        T& GetValue()
        {
            UE_CHECK( IsSet() );
            return *Value;
        }
        const T& GetValue() const
        {
            UE_CHECK( IsSet() );
            return *Value;
        }
        template <typename... ArgTypes>
        T& Emplace( ArgTypes&&... Args )
        {
            return Value.emplace( std::forward<ArgTypes>( Args )... );
        }
        void Reset()
        {
            Value.reset();
        }
        T* operator->()
        {
            return &GetValue();
        }
        const T* operator->() const
        {
            return &GetValue();
        }
        T& operator*()
        {
            return GetValue();
        }
        const T& operator*() const
        {
            return GetValue();
        }

    private:
        std::optional<T> Value;
    };

    template <typename T>
    using TConstArrayView = TArrayView<const T>;

    template <typename T>
    struct TVector2
    {
        T X{};
        T Y{};

        constexpr TVector2() = default;
        constexpr TVector2( T InX, T InY ) : X( InX ), Y( InY )
        {
        }
        explicit TVector2( const glm::vec<2, T>& V ) : X( V.x ), Y( V.y )
        {
        }
        explicit operator glm::vec<2, T>() const
        {
            return { X, Y };
        }

        static constexpr TVector2 Zero()
        {
            return { T( 0 ), T( 0 ) };
        }
        T& operator[]( int Index )
        {
            return Index == 0 ? X : Y;
        }
        const T& operator[]( int Index ) const
        {
            return Index == 0 ? X : Y;
        }
        TVector2 operator+( const TVector2& O ) const
        {
            return { X + O.X, Y + O.Y };
        }
        TVector2 operator-( const TVector2& O ) const
        {
            return { X - O.X, Y - O.Y };
        }
        TVector2 operator*( T S ) const
        {
            return { X * S, Y * S };
        }
        bool operator==( const TVector2& O ) const
        {
            return X == O.X && Y == O.Y;
        }
        bool operator!=( const TVector2& O ) const
        {
            return !( *this == O );
        }
    };

    template <typename T>
    struct TVector
    {
        T X{};
        T Y{};
        T Z{};

        constexpr TVector() = default;
        constexpr TVector( T InX, T InY, T InZ ) : X( InX ), Y( InY ), Z( InZ )
        {
        }
        explicit TVector( const glm::vec<3, T>& V ) : X( V.x ), Y( V.y ), Z( V.z )
        {
        }
        explicit operator glm::vec<3, T>() const
        {
            return { X, Y, Z };
        }

        static constexpr TVector Zero()
        {
            return { T( 0 ), T( 0 ), T( 0 ) };
        }
        static constexpr TVector One()
        {
            return { T( 1 ), T( 1 ), T( 1 ) };
        }
        static constexpr TVector UnitX()
        {
            return { T( 1 ), T( 0 ), T( 0 ) };
        }
        static constexpr TVector UnitY()
        {
            return { T( 0 ), T( 1 ), T( 0 ) };
        }
        static constexpr TVector UnitZ()
        {
            return { T( 0 ), T( 0 ), T( 1 ) };
        }
        // UE converts between FVector3f and FVector3d explicitly (vertex normals/colours are float).
        template <typename U>
        explicit constexpr TVector( const TVector<U>& V ) : X( T( V.X ) ), Y( T( V.Y ) ), Z( T( V.Z ) )
        {
        }
        T& operator[]( int Index )
        {
            return Index == 0 ? X : ( Index == 1 ? Y : Z );
        }
        const T& operator[]( int Index ) const
        {
            return Index == 0 ? X : ( Index == 1 ? Y : Z );
        }
        TVector operator+( const TVector& O ) const
        {
            return { X + O.X, Y + O.Y, Z + O.Z };
        }
        TVector operator-( const TVector& O ) const
        {
            return { X - O.X, Y - O.Y, Z - O.Z };
        }
        TVector operator-() const
        {
            return { -X, -Y, -Z };
        }
        TVector operator*( T S ) const
        {
            return { X * S, Y * S, Z * S };
        }
        TVector operator/( T S ) const
        {
            return { X / S, Y / S, Z / S };
        }
        TVector& operator+=( const TVector& O )
        {
            return *this = *this + O;
        }
        TVector& operator-=( const TVector& O )
        {
            return *this = *this - O;
        }
        TVector& operator*=( T S )
        {
            return *this = *this * S;
        }
        TVector& operator/=( T S )
        {
            return *this = *this / S;
        }
        bool operator==( const TVector& O ) const
        {
            return X == O.X && Y == O.Y && Z == O.Z;
        }
        bool operator!=( const TVector& O ) const
        {
            return !( *this == O );
        }

        T Dot( const TVector& O ) const
        {
            return X * O.X + Y * O.Y + Z * O.Z;
        }
        TVector Cross( const TVector& O ) const
        {
            return { Y * O.Z - Z * O.Y, Z * O.X - X * O.Z, X * O.Y - Y * O.X };
        }
        T SquaredLength() const
        {
            return Dot( *this );
        }
        T Length() const
        {
            return std::sqrt( SquaredLength() );
        }
    };

    // Scalar on the left, any arithmetic type (UE: `double * FVector3f` compiles and keeps the vector's type).
    template <typename T, typename S>
        requires std::is_arithmetic_v<S>
    TVector<T> operator*( S Scale, const TVector<T>& V )
    {
        return V * T( Scale );
    }
    template <typename T, typename S>
        requires std::is_arithmetic_v<S>
    TVector2<T> operator*( S Scale, const TVector2<T>& V )
    {
        return V * T( Scale );
    }

    // UE::Math::TVector4 (Math/Vector4.h): the colour overlay's element type; only what the overlay reads.
    template <typename T>
    struct TVector4
    {
        T X{};
        T Y{};
        T Z{};
        T W{};

        constexpr TVector4() = default;
        constexpr TVector4( T InX, T InY, T InZ, T InW ) : X( InX ), Y( InY ), Z( InZ ), W( InW )
        {
        }
        T& operator[]( int Index )
        {
            UE_CHECK_SLOW( Index >= 0 && Index < 4 );
            return Index == 0 ? X : Index == 1 ? Y : Index == 2 ? Z : W;
        }
        const T& operator[]( int Index ) const
        {
            UE_CHECK_SLOW( Index >= 0 && Index < 4 );
            return Index == 0 ? X : Index == 1 ? Y : Index == 2 ? Z : W;
        }
        bool operator==( const TVector4& O ) const
        {
            return X == O.X && Y == O.Y && Z == O.Z && W == O.W;
        }
        bool operator!=( const TVector4& O ) const
        {
            return !( *this == O );
        }
    };

    // UE's FName is an interned, case-insensitive name; attribute names here are plain strings.
    using FName = std::string;

    using FVector2f = TVector2<float>;
    using FVector2d = TVector2<double>;
    using FVector3f = TVector<float>;
    using FVector3d = TVector<double>;
    using FVector4f = TVector4<float>;

} // namespace Desert::Geometry
