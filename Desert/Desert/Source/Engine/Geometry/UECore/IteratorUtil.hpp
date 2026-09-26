// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Util/IteratorUtil.h:1-464, adapted: UE Core types
// via UECore.hpp, namespace Desert::Geometry. Port of geometry3cpp iterator_util.h

#pragma once

#include "Engine/Geometry/UECore/IndexTypes.hpp"

namespace Desert::Geometry
{

    /**
     * Wrapper around an object of type IteratorT that provides STL
     * iterator-like semantics, that converts from the iteration type
     * (FromType) to a new type (ToType).
     *
     * Conversion is done via a provided mapping function
     */
    template <typename FromType, typename ToType, typename IteratorT>
    class MappedIterator
    {
        using MapFunctionT = std::function<ToType( FromType )>;

    public:
        MappedIterator() = default;

        bool operator==( const MappedIterator& Other ) const
        {
            return m_Cur == Other.Cur;
        }
        bool operator!=( const MappedIterator& Other ) const
        {
            return m_Cur != Other.m_Cur;
        }

        ToType operator*() const
        {
            return m_MapFunction( *m_Cur );
        }

        const MappedIterator& operator++() // prefix
        {
            m_Cur++;
            return *this;
        }

        MappedIterator( const IteratorT& CurItr, const MapFunctionT& MapFunctionIn ) : m_Cur( CurItr )
        {

            m_MapFunction = MapFunctionIn;
        }

        IteratorT    m_Cur;
        MapFunctionT m_MapFunction;
    };

    /**
     * Wrapper around an existing iterator that skips over
     * values for which the filter_func returns false.
     */
    template <typename ValueType, typename IteratorT>
    class FilteredIterator
    {
        using FilterFunctionT = std::function<bool( ValueType )>;

    public:
        FilteredIterator() = default;

        bool operator==( const FilteredIterator& Other ) const
        {
            return m_Cur == Other.Cur;
        }
        bool operator!=( const FilteredIterator& Other ) const
        {
            return m_Cur != Other.m_Cur;
        }

        ValueType operator*() const
        {
            return *m_Cur;
        }

        const FilteredIterator& operator++() // prefix
        {
            GotoNextElement();
            return *this;
        }

        void GotoNextElement()
        {
            do
            {
                m_Cur++;
            } while ( m_Cur != m_End && !static_cast<bool>( m_FilterFunc( *m_Cur ) ) );
        }

        FilteredIterator( const IteratorT& CurItr, const IteratorT& EndItr, const FilterFunctionT& FilterFuncIn )
             : m_Cur( CurItr )
        {

            m_End              = EndItr;
            this->m_FilterFunc = FilterFuncIn;
            if ( m_Cur != m_End && !static_cast<bool>( m_FilterFunc( *m_Cur ) ) )
            {
                GotoNextElement();
            }
        }

        IteratorT       m_Cur;
        IteratorT       m_End;
        FilterFunctionT m_FilterFunc;
    };

    /**
     * Wrapper around existing iterator that returns multiple values, of potentially
     * different type, for each value that input iterator returns.
     *
     * This is done via an "expansion" function that takes an int reference which
     * indicates "where" we are in the expansion (eg like a state machine).
     * How you use this value is up to you.
     *
     * When the input is -1, you should interpret this as the "beginning" of
     * handling the input value (ie we have not returned any values yet for
     * this input value)
     *
     * When you are "done" with an input value, set the outgoing int reference to -1
     * and the base iterator will be incremented.
     *
     * If you have more values to return for this input value, set it to some positive
     * number of your choosing.
     *
     * See DynamicMesh3::VtxTrianglesItr for an example
     */
    template <typename OutputType, typename InputType, typename InputIteratorT>
    class ExpandIterator
    {
        using ExpandFunctionT = std::function<OutputType( InputType, int& )>;

    public:
        ExpandIterator() = default;

        bool operator==( const ExpandIterator& Other ) const
        {
            return m_Cur == Other.Cur;
        }
        bool operator!=( const ExpandIterator& Other ) const
        {
            return m_Cur != Other.Cur;
        }

        OutputType operator*() const
        {
            return m_CurValue;
        }

        const ExpandIterator& operator++() // prefix
        {
            goto_next();
            return *this;
        }

        void goto_next()
        {
            while ( m_Cur != m_End )
            {
                m_CurValue = ExpandFunc( *m_Cur, m_CurExpandI );
                if ( m_CurExpandI == -1 )
                {
                    ++m_Cur; // done with this base value
                }
                else
                {
                    break; // want caller to see current output value
                }
            }
        }

        ExpandIterator( const InputIteratorT& CurItr, const InputIteratorT& EndItr,
                        const ExpandFunctionT& ExpandFuncIn )
             : m_Cur( CurItr )
        {

            m_End        = EndItr;
            m_ExpandFunc = ExpandFuncIn;
            m_CurExpandI = -1;
            goto_next();
        }

        InputIteratorT  m_Cur;
        InputIteratorT  m_End;
        OutputType      m_CurValue;
        int             m_CurExpandI{};
        ExpandFunctionT m_ExpandFunc;
    };

    /**
     * Generic "enumerable" object that provides begin/end semantics for an ExpandIterator suitable for use with
     * range-based for. You can either provide begin/end iterators, or another "enumerable" object that has
     * begin()/end() functions.
     */
    template <typename OutputType, typename InputType, typename InputIteratorT>
    class ExpandEnumerable
    {
        using ExpandFunctionT = std::function<OutputType( InputType, int& )>;
        using ExpandIteratorT = ExpandIterator<OutputType, InputType, InputIteratorT>;

    public:
        ExpandFunctionT m_ExpandFunc;
        InputIteratorT  m_BeginItr, m_EndItr;

        ExpandEnumerable( const InputIteratorT& BeginIn, const InputIteratorT& EndIn,
                          ExpandFunctionT ExpandFuncIn )
        {
            this->BeginItr   = BeginIn;
            this->EndItr     = EndIn;
            this->ExpandFunc = ExpandFuncIn;
        }

        template <typename IteratorSource>
        ExpandEnumerable( const IteratorSource& Source, ExpandFunctionT ExpandFuncIn )
        {
            this->BeginItr   = Source.begin();
            this->EndItr     = Source.end();
            this->ExpandFunc = ExpandFuncIn;
        }

        ExpandIteratorT begin()
        {
            return ExpandIteratorT( m_BeginItr, m_EndItr, m_ExpandFunc );
        }

        ExpandIteratorT end()
        {
            return ExpandIteratorT( m_EndItr, m_EndItr, m_ExpandFunc );
        }
    };

    /**
     * Wrapper around existing integer iterator that returns either 0, 1, or 2 integers
     * for each value that the original iterator returns.
     *
     * This is specifically used by DynamicMesh3::VtxTrianglesItr, where for each edge
     * around a vertex, between 0 and 2 triangles need to be returned.
     *
     * This is done via the PairExpandFunctionT std::function, which returns a Index2i for
     * a given integer. This pair must be either (a,invalid), (a, b), or (invalid, invalid),
     * where invalid is integer < 0
     */
    template <typename InputIteratorT>
    class PairExpandIterator
    {
        using PairExpandFunctionT = std::function<Index2i( int )>;

    public:
        PairExpandIterator() = default;

        bool operator==( const PairExpandIterator& Other ) const
        {
            return m_Cur == Other.Cur;
        }
        bool operator!=( const PairExpandIterator& Other ) const
        {
            return m_Cur != Other.m_Cur;
        }

        int operator*() const
        {
            return m_CurValue;
        }

        const PairExpandIterator& operator++() // prefix
        {
            goto_next();
            return *this;
        }

        void goto_next()
        {
            while ( m_Cur != m_End )
            {
                if ( m_CurPairI == 0 )
                {
                    m_CurPair = m_PairFunc( *m_Cur );
                    if ( m_CurPair.A >= 0 )
                    {
                        m_CurValue = m_CurPair.A;
                        m_CurPairI = 1; // want to take second branch
                        return;       // let caller see value
                    }

                    m_CurPairI = 0;
                    ++m_Cur; // done with this base value
                }
                else if ( m_CurPairI == 1 )
                {
                    if ( m_CurPair.B >= 0 )
                    {
                        m_CurValue = m_CurPair.B;
                        m_CurPairI = 2; // want to take third branch
                        return;       // let caller see value
                    }

                    m_CurPairI = 0;
                    ++m_Cur; // done with this base value
                }
                else
                {
                    m_CurPairI = 0;
                    ++m_Cur; // done with this base value
                }
            }
        }

        PairExpandIterator( const InputIteratorT& CurItr, const InputIteratorT& EndItr,
                            const PairExpandFunctionT& PairFuncIn )
             : m_Cur( CurItr )
        {

            m_End      = EndItr;
            m_PairFunc = PairFuncIn;
            m_CurPairI = 0;
            goto_next();
        }

        InputIteratorT      m_Cur;
        InputIteratorT      m_End;
        Index2i             m_CurPair;
        int                 m_CurValue{};
        int                 m_CurPairI{};
        PairExpandFunctionT m_PairFunc;
    };

    /**
     * Generic "enumerable" object that provides begin/end semantics for an PairExpandIterator suitable for use
     * with range-based for. You can either provide begin/end iterators, or another "enumerable" object that has
     * begin()/end() functions.
     */
    template <typename InputIteratorT>
    class PairExpandEnumerable
    {
        using ExpandFunctionT = std::function<Index2i( int )>;
        using ExpandIteratorT = PairExpandIterator<InputIteratorT>;

    public:
        ExpandFunctionT m_ExpandFunc;
        InputIteratorT  m_BeginItr, m_EndItr;

        PairExpandEnumerable( const InputIteratorT& BeginIn, const InputIteratorT& EndIn,
                              ExpandFunctionT ExpandFuncIn )
        {
            this->BeginItr   = BeginIn;
            this->EndItr     = EndIn;
            this->ExpandFunc = ExpandFuncIn;
        }

        template <typename IteratorSource>
        PairExpandEnumerable( const IteratorSource& Source, ExpandFunctionT ExpandFuncIn )
        {
            this->m_BeginItr   = Source.begin();
            this->m_EndItr     = Source.end();
            this->m_ExpandFunc = ExpandFuncIn;
        }

        ExpandIteratorT begin()
        {
            return ExpandIteratorT( m_BeginItr, m_EndItr, m_ExpandFunc );
        }

        ExpandIteratorT end()
        {
            return ExpandIteratorT( m_EndItr, m_EndItr, m_ExpandFunc );
        }
    };

    /**
     * ModuloIteration is used to iterate over a range of indices [0,N) using modulo-arithmetic.
     * The iteration proceeds as NextValue = (CurValue + ModuloValue) % N.
     * As long as the ModuloValue is a prime number > N/2, then every integer in the sequence 0...N-1
     * will appear exactly once before 0 re-appears (and in fact any index in the range can be used
     * as the starting value).
     *
     * ModuloIteration computes in 64-bit with (by default) a large enough prime that will work
     * for any 32-bit unsigned integer. If 64-bit iterations are needed, some larger primes can
     * be found here: https://en.wikipedia.org/wiki/P%C3%A9pin%27s_test
     *
     * (The prime does not strictly need to be > N/2, any prime will work as long as it is not a divisor of N.
     *  And it doesn't even need to be a prime number, just a co-prime of N, ie GCD(N, ModuloValue) = 1.
     *  It is possible to check GCD relatively quickly to search for valid constants, for example if
     *  many values were needed to use as seeds/etc)
     *
     * Usage:
     *
            ModuloIteration Iter(N);
            uint32_t Index;
            while (Iter.GetNextIndex(Index)) { ... }
     */
    struct ModuloIteration
    {
        uint64_t MaxIndex    = 0;
        uint64_t ModuloPrime = 4294967311ull; // prime > max_unsigned_int
        uint64_t CurIndex    = 0;
        uint64_t StartIndex  = 0;
        uint64_t Count       = 0;
        uint64_t ModuloNum   = 1;

        ModuloIteration( uint32_t MaxIndexIn, uint32_t StartIndexIn = 0, uint64_t ModuloPrimeIn = 3208642561 )
             : Count( 0 )
        {
            MaxIndex   = static_cast<uint64_t>( std::max( static_cast<uint32_t>( 0 ), MaxIndexIn ) );
            StartIndex = static_cast<uint64_t>( std::max( static_cast<uint32_t>( 0 ), StartIndexIn ) );
            CurIndex   = StartIndex;

            ModuloNum  = std::max( static_cast<uint64_t>( 1 ),
                                   MaxIndex ); // can't be zero or we hit integer-divide. If MaxIndex
                                               // is 0 we will terminate on first iteration anyway
            ModuloPrime = ModuloPrimeIn;
            assert( ModuloPrime > MaxIndex );
        }

        bool GetNextIndex( uint32_t& NextIndexOut )
        {
            NextIndexOut = static_cast<uint32_t>( CurIndex );
            CurIndex     = ( CurIndex + ModuloPrime ) % ModuloNum;
            return ( Count++ != MaxIndex );
        }

        bool GetNextIndex( int32_t& NextIndexOut )
        {
            NextIndexOut = static_cast<int32_t>( CurIndex );
            CurIndex     = ( CurIndex + ModuloPrime ) % ModuloNum;
            return ( Count++ != MaxIndex );
        }
    };

} // namespace Desert::Geometry
