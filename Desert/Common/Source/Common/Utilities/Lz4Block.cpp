#include "Lz4Block.hpp"

#include <cstring>
#include <vector>

namespace Common::Utils
{
    namespace
    {
        // Positions are indexed by the hash of the four bytes that start them. 16 bits of table is
        // 64 Ki slots against the format's 64 KiB window: one slot per window position, so the table
        // cannot be the thing that loses a match that the window still holds.
        constexpr uint32_t kHashBits   = 16;
        constexpr size_t   kWindow     = 65535; // the offset field is 16 bits and 0 is not a legal offset
        constexpr size_t   kMinMatch   = 4;
        constexpr size_t   kLastLiterals = 5;  // the format forbids a match inside the final 5 bytes
        constexpr size_t   kMatchLimit   = 12; // ... and forbids a match STARTING in the final 12

        uint32_t Hash4( uint32_t v )
        {
            return ( v * 2654435761u ) >> ( 32 - kHashBits );
        }

        uint32_t Load32( const unsigned char* p )
        {
            // Byte-wise, not a 32-bit load: this value only ever feeds the hash table and the equality
            // test below, both of which must answer the same on any host, and a raw load would make the
            // COMPRESSED OUTPUT depend on the machine's word order.
            return static_cast<uint32_t>( p[0] ) | ( static_cast<uint32_t>( p[1] ) << 8 ) |
                   ( static_cast<uint32_t>( p[2] ) << 16 ) | ( static_cast<uint32_t>( p[3] ) << 24 );
        }

        // Writes a length that did not fit its nibble: 255 for every full step, then the remainder.
        bool WriteLengthTail( unsigned char*& op, unsigned char* oend, size_t length )
        {
            while ( length >= 255 )
            {
                if ( op >= oend )
                    return false;
                *op++ = 255;
                length -= 255;
            }
            if ( op >= oend )
                return false;
            *op++ = static_cast<unsigned char>( length );
            return true;
        }
    } // namespace

    size_t Lz4BlockBound( size_t size )
    {
        return size + size / 255 + 16;
    }

    size_t Lz4BlockCompress( const void* src, size_t srcSize, void* dst, size_t dstCapacity )
    {
        const auto* const base = static_cast<const unsigned char*>( src );
        auto* const       obeg = static_cast<unsigned char*>( dst );
        unsigned char*    op   = obeg;
        unsigned char* const oend = obeg + dstCapacity;

        // A position is stored as index+1 so that 0 can mean "never seen". That is why the input is
        // capped at UINT32_MAX-1 rather than UINT32_MAX: the +1 must not wrap into the empty marker.
        if ( srcSize == 0 || srcSize >= 0xFFFFFFFFu )
            return 0;

        const unsigned char* ip     = base;
        const unsigned char* anchor = base;
        const unsigned char* const iend = base + srcSize;

        const auto emitSequence = [&]( const unsigned char* matchRef, size_t matchLength ) -> bool
        {
            const size_t literals = static_cast<size_t>( ip - anchor );
            const size_t mlCode   = matchLength - kMinMatch;
            if ( op >= oend )
                return false;
            *op++ = static_cast<unsigned char>( ( ( literals >= 15 ? 15u : literals ) << 4 ) |
                                                ( mlCode >= 15 ? 15u : mlCode ) );
            if ( literals >= 15 && !WriteLengthTail( op, oend, literals - 15 ) )
                return false;
            if ( static_cast<size_t>( oend - op ) < literals )
                return false;
            std::memcpy( op, anchor, literals );
            op += literals;

            const size_t offset = static_cast<size_t>( ip - matchRef );
            if ( static_cast<size_t>( oend - op ) < 2 )
                return false;
            *op++ = static_cast<unsigned char>( offset & 0xFFu );
            *op++ = static_cast<unsigned char>( ( offset >> 8 ) & 0xFFu );
            if ( mlCode >= 15 && !WriteLengthTail( op, oend, mlCode - 15 ) )
                return false;
            return true;
        };

        if ( srcSize > kMatchLimit + kMinMatch )
        {
            std::vector<uint32_t>      table( size_t{ 1 } << kHashBits, 0u );
            const unsigned char* const searchEnd = iend - kMatchLimit;
            const unsigned char* const matchEnd  = iend - kLastLiterals;

            ++ip; // the first byte can never be a match: there is nothing behind it
            while ( ip < searchEnd )
            {
                const uint32_t sequence = Load32( ip );
                const uint32_t slot     = Hash4( sequence );
                const uint32_t stored   = table[slot];
                table[slot]             = static_cast<uint32_t>( ip - base ) + 1u;

                if ( stored == 0 )
                {
                    ++ip;
                    continue;
                }
                const unsigned char* ref = base + ( stored - 1u );
                if ( static_cast<size_t>( ip - ref ) > kWindow || Load32( ref ) != sequence )
                {
                    ++ip;
                    continue;
                }

                const unsigned char* m = ip + kMinMatch;
                const unsigned char* r = ref + kMinMatch;
                while ( m < matchEnd && *m == *r )
                {
                    ++m;
                    ++r;
                }
                if ( !emitSequence( ref, static_cast<size_t>( m - ip ) ) )
                    return 0;
                ip     = m;
                anchor = m;
            }
        }

        // The final sequence is literals with no match after it, which is how a decoder knows the block
        // has ended — there is no terminator byte in this format.
        const size_t tail = static_cast<size_t>( iend - anchor );
        if ( op >= oend )
            return 0;
        *op++ = static_cast<unsigned char>( ( tail >= 15 ? 15u : tail ) << 4 );
        if ( tail >= 15 && !WriteLengthTail( op, oend, tail - 15 ) )
            return 0;
        if ( static_cast<size_t>( oend - op ) < tail )
            return 0;
        std::memcpy( op, anchor, tail );
        op += tail;
        return static_cast<size_t>( op - obeg );
    }

    bool Lz4BlockDecompress( const void* src, size_t srcSize, void* dst, size_t dstSize )
    {
        const auto*          ip   = static_cast<const unsigned char*>( src );
        const unsigned char* iend = ip + srcSize;
        auto* const          obeg = static_cast<unsigned char*>( dst );
        unsigned char*       op   = obeg;
        unsigned char* const oend = obeg + dstSize;

        if ( srcSize == 0 )
            return dstSize == 0;

        while ( ip < iend )
        {
            const unsigned token = *ip++;

            size_t literals = token >> 4;
            if ( literals == 15 )
            {
                unsigned char step = 0;
                do
                {
                    if ( ip >= iend )
                        return false;
                    step = *ip++;
                    literals += step;
                } while ( step == 255 );
            }
            if ( static_cast<size_t>( iend - ip ) < literals || static_cast<size_t>( oend - op ) < literals )
                return false;
            // OVER-COPY IN FIXED 32-BYTE STEPS while there is room to spill into on BOTH sides. A
            // variable-length memcpy per sequence is a call, and the runs here are a handful of bytes:
            // measured on a 60 000-byte .stmesh block, exact copies decode at 731 MB/s and this at the
            // figure quoted in Lz4Block.hpp. The guard is what keeps it in bounds — the spill is only
            // taken when 32 bytes past the run still lie inside both buffers, and the sequences at the
            // very end of the block fall through to the exact path.
            if ( static_cast<size_t>( iend - ip ) >= literals + 32 &&
                 static_cast<size_t>( oend - op ) >= literals + 32 )
            {
                size_t done = 0;
                do
                {
                    std::memcpy( op + done, ip + done, 32 );
                    done += 32;
                } while ( done < literals );
            }
            else
            {
                std::memcpy( op, ip, literals );
            }
            op += literals;
            ip += literals;

            if ( ip == iend )
                break; // the last sequence carries literals only

            if ( static_cast<size_t>( iend - ip ) < 2 )
                return false;
            const size_t offset = static_cast<size_t>( ip[0] ) | ( static_cast<size_t>( ip[1] ) << 8 );
            ip += 2;
            if ( offset == 0 || static_cast<size_t>( op - obeg ) < offset )
                return false;

            size_t matchLength = token & 15u;
            if ( matchLength == 15 )
            {
                unsigned char step = 0;
                do
                {
                    if ( ip >= iend )
                        return false;
                    step = *ip++;
                    matchLength += step;
                } while ( step == 255 );
            }
            matchLength += kMinMatch;
            if ( static_cast<size_t>( oend - op ) < matchLength )
                return false;

            // A match may OVERLAP its own output — offset 1 is how the format spells a run of one
            // repeated byte — so a single memcpy would read bytes this iteration has not written yet.
            // Copying in chunks of `offset` is the fix that keeps the block copy: every chunk reads
            // only from bytes an earlier chunk already wrote. A byte-at-a-time loop was correct too
            // and measured 693 MB/s, which is BELOW this machine's storage and would have turned the
            // codec's whole case (Lz4Block.hpp: pays when the device is slower than P*(r-1)/r) upside
            // down — the decoder's speed is not an implementation detail here, it is the argument.
            const unsigned char* ref = op - offset;
            if ( offset >= 16 && static_cast<size_t>( oend - op ) >= matchLength + 32 )
            {
                // Sixteen at a time: the step must not exceed the offset or a chunk would read bytes
                // this very loop is still writing.
                size_t done = 0;
                do
                {
                    std::memcpy( op + done, ref + done, 16 );
                    done += 16;
                } while ( done < matchLength );
            }
            else
            {
                // OVERLAP IS LEGAL AND MEANINGFUL HERE: offset 1 is how the format spells a run of one
                // repeated byte, so the copy must go forward one byte at a time and see its own output.
                for ( size_t i = 0; i < matchLength; ++i )
                    op[i] = ref[i];
            }
            op += matchLength;
        }
        return op == oend;
    }
} // namespace Common::Utils
