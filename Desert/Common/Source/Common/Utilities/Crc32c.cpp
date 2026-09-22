#include "Crc32c.hpp"

#include <array>
#include <bit>
#include <climits>
#include <cstring>

// THE FEATURE MACRO, NOT THE ARCHITECTURE MACRO. `__aarch64__` says the instruction set; only
// `__ARM_FEATURE_CRC32` says this compiler will emit the CRC instruction, and MSVC on ARM64 defines
// neither it nor ships <arm_acle.h> — so keying on the architecture would be a build failure on a
// platform this cannot be compiled on here to find out. Without it the portable table runs, which is
// correct and slower, and the equality test still compares the two arms on whatever host it meets.
#if defined( __aarch64__ ) && defined( __ARM_FEATURE_CRC32 )
#define DESERT_CRC32C_ARM 1
#include <arm_acle.h>
#elif defined( __x86_64__ ) || defined( _M_X64 )
#define DESERT_CRC32C_X86 1
#include <nmmintrin.h>
#if defined( _MSC_VER )
#include <intrin.h>
#endif
#endif

namespace Common::Utils
{
    namespace
    {
        static_assert( CHAR_BIT == 8, "the CRC is defined over 8-bit bytes" );

        // Slice-by-8 table for the reflected CRC-32C polynomial. Built once, at first use, from the
        // polynomial itself rather than pasted in as 2048 magic numbers — a pasted table is a second
        // definition of the polynomial that no compiler checks against the first.
        constexpr uint32_t kReflectedPoly = 0x82F63B78u;

        struct Tables
        {
            std::array<std::array<uint32_t, 256>, 8> T{};

            Tables()
            {
                for ( uint32_t i = 0; i < 256; ++i )
                {
                    uint32_t c = i;
                    for ( int k = 0; k < 8; ++k )
                        c = ( c >> 1 ) ^ ( kReflectedPoly & ( ~( c & 1u ) + 1u ) );
                    T[0][i] = c;
                }
                for ( uint32_t i = 0; i < 256; ++i )
                    for ( size_t s = 1; s < 8; ++s )
                        T[s][i] = ( T[s - 1][i] >> 8 ) ^ T[0][T[s - 1][i] & 0xFFu];
            }
        };

        const Tables& Tbl()
        {
            static const Tables t;
            return t;
        }

#if defined( DESERT_CRC32C_X86 )
        bool DetectSse42()
        {
#if defined( _MSC_VER )
            int regs[4] = {};
            __cpuid( regs, 1 );
            return ( regs[2] & ( 1 << 20 ) ) != 0; // ECX bit 20 = SSE4.2
#else
            return __builtin_cpu_supports( "sse4.2" );
#endif
        }
#endif
    } // namespace

    uint32_t Crc32cPortable( const void* data, size_t size )
    {
        const auto*   p = static_cast<const unsigned char*>( data );
        const Tables& t = Tbl();
        uint32_t      c = 0xFFFFFFFFu;
        while ( size >= 8 )
        {
            // The low four bytes are folded into the running value first; the high four index the
            // table directly. Byte-at-a-time indexing rather than a 32-bit load, so the routine reads
            // the same on a big-endian host as on a little-endian one.
            c ^= static_cast<uint32_t>( p[0] ) | ( static_cast<uint32_t>( p[1] ) << 8 ) |
                 ( static_cast<uint32_t>( p[2] ) << 16 ) | ( static_cast<uint32_t>( p[3] ) << 24 );
            c = t.T[7][c & 0xFFu] ^ t.T[6][( c >> 8 ) & 0xFFu] ^ t.T[5][( c >> 16 ) & 0xFFu] ^ t.T[4][c >> 24] ^
                t.T[3][p[4]] ^ t.T[2][p[5]] ^ t.T[1][p[6]] ^ t.T[0][p[7]];
            p += 8;
            size -= 8;
        }
        while ( size-- )
            c = ( c >> 8 ) ^ t.T[0][( c ^ *p++ ) & 0xFFu];
        return ~c;
    }

    bool Crc32cUsesHardware()
    {
#if defined( DESERT_CRC32C_ARM )
        return true; // every ARMv8-A the engine targets has the CRC extension
#elif defined( DESERT_CRC32C_X86 )
        static const bool ok = DetectSse42();
        return ok;
#else
        return false;
#endif
    }

    uint32_t Crc32c( const void* data, size_t size )
    {
#if defined( DESERT_CRC32C_ARM ) || defined( DESERT_CRC32C_X86 )
        // The instruction folds a machine WORD, so the hardware path agrees with the byte-at-a-time
        // table only on a little-endian host. Both shipping targets are little-endian and the archive
        // format pins that anyway (PakFile.cpp, kByteOrderTag); a port that breaks the equality is
        // told here, at compile time, rather than by a test that fails on one platform only.
        static_assert( std::endian::native == std::endian::little,
                       "the hardware CRC32C path assumes little-endian word order" );
        if ( Crc32cUsesHardware() )
        {
            const auto* p = static_cast<const unsigned char*>( data );
            uint32_t    c = 0xFFFFFFFFu;
            while ( size >= 8 )
            {
                uint64_t v = 0;
                std::memcpy( &v, p, 8 ); // the instruction consumes the machine's own word order
#if defined( DESERT_CRC32C_ARM )
                c = __crc32cd( c, v );
#else
                c = static_cast<uint32_t>( _mm_crc32_u64( c, v ) );
#endif
                p += 8;
                size -= 8;
            }
            while ( size-- )
            {
#if defined( DESERT_CRC32C_ARM )
                c = __crc32cb( c, *p++ );
#else
                c = _mm_crc32_u8( c, *p++ );
#endif
            }
            return ~c;
        }
#endif
        return Crc32cPortable( data, size );
    }
} // namespace Common::Utils
