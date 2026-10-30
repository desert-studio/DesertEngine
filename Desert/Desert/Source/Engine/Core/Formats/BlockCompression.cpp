#include "BlockCompression.hpp"

#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <cmath>
// `std::abs( int )` is declared here and NOT by <cmath>, which carries only the floating-point
// overloads. libc++ drags it in anyway; MSVC does not, and the Windows-only defect register already
// holds two entries of exactly this shape (<algorithm> for std::clamp, twice).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

// The BC7 encode (all eight modes) and its decode, both vendored, MIT; see the BC7 section below.
#include "../../../../../../ThirdParty/compressonator/cmp_core/source/cmp_core.h"
// BCDEC_STATIC keeps bcdec's symbols internal to this translation unit, as in Editor/Import/DdsSource.cpp, which
// compiles its own copy for the .dds sources; the two never meet at link time.
// Every entry point but bcdec_bc7 is then an unused static.
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#if defined( __clang__ ) || defined( __GNUC__ )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4505 )
#endif
#include "../../../../../../ThirdParty/bcdec/bcdec.h" // vendored, MIT (ThirdParty/bcdec/LICENSE)
#if defined( __clang__ ) || defined( __GNUC__ )
#pragma GCC diagnostic pop
#elif defined( _MSC_VER )
#pragma warning( pop )
#endif

namespace Desert::Core::Formats
{
    namespace
    {
        /// The 4-bit interpolation weights BOTH formats use, out of 64. This table is the format's, not
        /// ours: a GPU decoding these blocks uses exactly these numbers, so the encoder has to search
        /// against them and the decoder below has to reproduce them.
        constexpr int kWeight4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

        /// One 128-bit block, written and read LSB-first — which is the order both BPTC formats define
        /// their fields in, and the order the bytes then sit in the file.
        ///
        /// IT IS A CLASS AND NOT THREE LINES OF SHIFTING AT EACH FIELD. Every field of both formats is
        /// written through `Put` and read through `Get`, so the bit cursor cannot be advanced by one
        /// amount and the value shifted by another — which is the entire failure mode of hand-packed
        /// bitfields and is invisible until a GPU decodes the result.
        class BlockBits
        {
        public:
            void Put( const uint32_t value, const int bits )
            {
                for ( int i = 0; i < bits; ++i )
                {
                    if ( ( value >> i ) & 1u )
                        m_Bytes[m_At >> 3] |= static_cast<unsigned char>( 1u << ( m_At & 7 ) );
                    ++m_At;
                }
            }

            [[nodiscard]] uint32_t Get( const int bits )
            {
                uint32_t value = 0;
                for ( int i = 0; i < bits; ++i )
                {
                    if ( ( m_Bytes[m_At >> 3] >> ( m_At & 7 ) ) & 1u )
                        value |= 1u << i;
                    ++m_At;
                }
                return value;
            }

            void CopyOut( unsigned char* out ) const
            {
                std::memcpy( out, m_Bytes, 16 );
            }

            void CopyIn( const unsigned char* in )
            {
                std::memcpy( m_Bytes, in, 16 );
                m_At = 0;
            }

        private:
            unsigned char m_Bytes[16] = {};
            int           m_At        = 0;
        };

        // ── HALF FLOAT ────────────────────────────────────────────────────────────────────────────
        //
        // Written here rather than borrowed, because BC6H does not want a `float` at either end: it
        // stores a value that a GPU reinterprets AS A HALF'S BIT PATTERN, so the encoder's real input is
        // the 16-bit pattern and not the number. Only the non-negative range is handled — the format
        // written is BC6H_UFLOAT and radiance is non-negative; a negative input is clamped to zero at
        // the one place that produces these patterns, and named there.

        uint16_t FloatToHalfBits( float value )
        {
            if ( !( value > 0.0f ) ) // also catches NaN
                return 0;
            if ( value > kBC6HLargestValue )
                value = kBC6HLargestValue;

            uint32_t bits = 0;
            std::memcpy( &bits, &value, sizeof( bits ) );

            const int32_t  exponent = static_cast<int32_t>( ( bits >> 23 ) & 0xFF ) - 127 + 15;
            const uint32_t mantissa = bits & 0x7FFFFFu;

            if ( exponent <= 0 )
            {
                // Subnormal half, or smaller than one. Shift the implied 1 back in and round.
                const int32_t shift = 1 - exponent;
                if ( shift > 24 )
                    return 0;
                const uint32_t full = mantissa | 0x800000u;
                return static_cast<uint16_t>( ( full + ( 1u << ( shift + 12 ) ) ) >> ( shift + 13 ) );
            }
            if ( exponent >= 31 )
                return 0x7BFF; // clamped above, so this is the largest finite half

            uint16_t half =
                 static_cast<uint16_t>( ( static_cast<uint32_t>( exponent ) << 10 ) | ( mantissa >> 13 ) );
            if ( ( mantissa & 0x1FFFu ) > 0x1000u ) // round to nearest
                ++half;
            return half;
        }

        float HalfBitsToFloat( const uint16_t half )
        {
            const uint32_t exponent = ( half >> 10 ) & 0x1Fu;
            const uint32_t mantissa = half & 0x3FFu;

            uint32_t bits = 0;
            if ( exponent == 0 )
            {
                if ( mantissa == 0 )
                    return 0.0f;
                // Subnormal: normalise it into a float's exponent range.
                int32_t  e = -1;
                uint32_t m = mantissa;
                do
                {
                    ++e;
                    m <<= 1;
                } while ( ( m & 0x400u ) == 0 );
                bits = ( static_cast<uint32_t>( 127 - 15 - e ) << 23 ) | ( ( m & 0x3FFu ) << 13 );
            }
            else
            {
                bits = ( ( exponent + 127 - 15 ) << 23 ) | ( mantissa << 13 );
            }

            float value = 0.0f;
            std::memcpy( &value, &bits, sizeof( value ) );
            return value;
        }

        // ── BC6H's TWO TRANSFORMS, STRAIGHT OUT OF THE SPECIFICATION ─────────────────────────────
        //
        // A BC6H endpoint is not a number, it is a 10-bit code for one. `Unquantize10` is what a GPU
        // does to it before interpolating, and `FinishUnquantize` is what it does to the interpolated
        // result before calling the 16 bits a half. Both are reproduced exactly; the encoder searches
        // in the space BETWEEN them, which is the only space where "closer" means what it says.

        constexpr int Unquantize10( const int comp )
        {
            if ( comp == 0 )
                return 0;
            if ( comp == 1023 )
                return 0xFFFF;
            return ( ( comp << 16 ) + 0x8000 ) >> 10;
        }

        constexpr int Quantize10( const int internalValue )
        {
            const int guess = ( internalValue * 1023 + 32767 ) / 65535;
            return std::clamp( guess, 0, 1023 );
        }

        constexpr uint16_t FinishUnquantize( const int interpolated )
        {
            return static_cast<uint16_t>( ( interpolated * 31 ) >> 6 );
        }

        /// The inverse of `FinishUnquantize`, which is what turns a half pattern into something the
        /// endpoint search can measure distances in.
        constexpr int StartQuantize( const uint16_t halfBits )
        {
            return std::min( ( static_cast<int>( halfBits ) * 64 + 15 ) / 31, 0xFFFF );
        }

        constexpr int Interpolate( const int a, const int b, const int weight )
        {
            return ( a * ( 64 - weight ) + b * weight + 32 ) >> 6;
        }

        // ── THE SHARED ENDPOINT SEARCH ───────────────────────────────────────────────────────────
        //
        // Both encoders below fit ONE line through sixteen points and then snap each point to the
        // nearest of sixteen positions along it. They differ in how many channels the point has, how
        // the endpoints are quantized, and what "distance" means — not in the shape of the search, so
        // the shape is written once.

        /// The two texels furthest apart along the bounding box's diagonal. A principal-axis fit would
        /// be better on a block whose colours are not axis-aligned; this is the cheap approximation and
        /// the refit below is what recovers most of the difference.
        template <int Channels>
        void DiagonalEndpoints( const float points[16][Channels], float lowOut[Channels], float highOut[Channels] )
        {
            float lowest[Channels];
            float highest[Channels];
            for ( int c = 0; c < Channels; ++c )
            {
                lowest[c]  = points[0][c];
                highest[c] = points[0][c];
            }
            for ( int i = 1; i < 16; ++i )
            {
                for ( int c = 0; c < Channels; ++c )
                {
                    lowest[c]  = std::min( lowest[c], points[i][c] );
                    highest[c] = std::max( highest[c], points[i][c] );
                }
            }

            float axis[Channels];
            float axisLength = 0.0f;
            for ( int c = 0; c < Channels; ++c )
            {
                axis[c] = highest[c] - lowest[c];
                axisLength += axis[c] * axis[c];
            }

            if ( axisLength <= 0.0f )
            {
                for ( int c = 0; c < Channels; ++c )
                {
                    lowOut[c]  = lowest[c];
                    highOut[c] = lowest[c];
                }
                return;
            }

            float minProjection = std::numeric_limits<float>::max();
            float maxProjection = -std::numeric_limits<float>::max();
            int   minAt = 0, maxAt = 0;
            for ( int i = 0; i < 16; ++i )
            {
                float projection = 0.0f;
                for ( int c = 0; c < Channels; ++c )
                    projection += ( points[i][c] - lowest[c] ) * axis[c];
                if ( projection < minProjection )
                {
                    minProjection = projection;
                    minAt         = i;
                }
                if ( projection > maxProjection )
                {
                    maxProjection = projection;
                    maxAt         = i;
                }
            }

            for ( int c = 0; c < Channels; ++c )
            {
                lowOut[c]  = points[minAt][c];
                highOut[c] = points[maxAt][c];
            }
        }

        /// Least squares refit of the two endpoints, GIVEN the indices already chosen. This is the one
        /// step that recovers the quality the diagonal heuristic gives away: the diagonal picks two of
        /// the sixteen points, and the best line through sixteen points generally passes through none.
        template <int Channels>
        bool RefitEndpoints( const float points[16][Channels], const int indices[16], float lowOut[Channels],
                             float highOut[Channels] )
        {
            float a = 0.0f, b = 0.0f, c2 = 0.0f;
            float p[Channels] = {};
            float q[Channels] = {};
            for ( int i = 0; i < 16; ++i )
            {
                const float w = static_cast<float>( kWeight4[indices[i]] ) / 64.0f;
                a += ( 1.0f - w ) * ( 1.0f - w );
                b += ( 1.0f - w ) * w;
                c2 += w * w;
                for ( int c = 0; c < Channels; ++c )
                {
                    p[c] += ( 1.0f - w ) * points[i][c];
                    q[c] += w * points[i][c];
                }
            }
            const float determinant = a * c2 - b * b;
            if ( std::abs( determinant ) < 1e-6f )
                return false;
            for ( int c = 0; c < Channels; ++c )
            {
                lowOut[c]  = ( c2 * p[c] - b * q[c] ) / determinant;
                highOut[c] = ( a * q[c] - b * p[c] ) / determinant;
            }
            return true;
        }

        // ── BC7, ALL EIGHT MODES ─────────────────────────────────────────────────────────────────────
        //
        // The encode is AMD Compressonator's CMP_Core BC7 kernel (ThirdParty/compressonator, MIT), which
        // searches every mode the format has: the one-subset modes 4/5/6, the two-subset modes 1/3/7 over
        // their 64 partitions and the three-subset modes 0/2. A block that holds a yellow petal, a green
        // blade and a dry tip is three colours that sit on no single line in RGBA, and only a partitioned
        // mode keeps all three; the mode-6-only encoder this replaced could not, and six of FieldBarn's
        // nine masked albedos failed the gate on exactly those blocks (GRASS-ASSET p7). UE's cook does the
        // same search (ISPC texcomp / Oodle Texture).
        //
        // THE EFFORT IS THE CALLER'S, not a constant here: the project's Texture Encoding settings
        // (Project::TextureEncodingSettings, UE UTextureEncodingProjectSettings) give a fast effort for the
        // editor's DDC misses and a final one for the cook, and the effort is part of the texture's DDC key.
        // Measured on celandine_01_diff_2k (whole image, bcdec, ten cores): 0.05 -> 50.51 dB / worst 24 in
        // 0.6 s, 0.5 -> 51.41 / 23 in 2.0 s, 1.0 -> 52.33 / 23 in 9.1 s.
        //
        // The kernel is patched in two places (ThirdParty/compressonator/cmp_core/shaders/bc7_encode_kernel.cpp,
        // "DesertEngine patch"): its solid-colour endpoint table left the second endpoint at a 255 sentinel for
        // values just under a ramp value, and dandelion_01_diff_2k decoded blue 2 as 174 in a mode-2 block.

        /// The kernel's options for one effort, built once per effort: it reads them and never writes them,
        /// so every worker encoding a row shares the instance.
        const void* Bc7Options( const float effort )
        {
            static std::mutex                             mutex;
            static std::vector<std::pair<float, void*>>   built;
            const std::lock_guard<std::mutex>             lock( mutex );
            for ( const auto& [e, options] : built )
                if ( e == effort )
                    return options;
            void* created = nullptr;
            CreateOptionsBC7( &created );
            SetQualityBC7( created, effort );
            built.emplace_back( effort, created );
            return created;
        }

        void EncodeBc7Block( const unsigned char rgba[16 * 4], unsigned char* out, const void* options )
        {
            CompressBlockBC7( rgba, 4u * 4u, out, options );
        }

        /// The decode is bcdec's (ThirdParty/bcdec, MIT), which implements every mode — so what the gate
        /// measures is what any conforming GPU would sample, whichever mode the encoder chose.
        void DecodeBc7Block( const unsigned char* in, unsigned char rgba[16 * 4] )
        {
            bcdec_bc7( in, rgba, 4 * 4 );
        }

        // ── BC6H, MODE 11 ────────────────────────────────────────────────────────────────────────
        //
        // One subset, three channels, endpoints stored ABSOLUTE at ten bits each (the "no delta" mode),
        // sixteen 4-bit indices, same anchor rule. 5 + 60 + 63 = 128.
        //
        // ALPHA DOES NOT EXIST IN THIS FORMAT. A source's fourth channel is dropped here, which is
        // correct for every consumer this engine has (a radiance cube's alpha is 1) and is named rather
        // than left for someone to discover: `BlockDecompressImage` writes 1.0 back into it.

        void EncodeBc6hBlock( const int texels[16][3], unsigned char* out )
        {
            float points[16][3];
            for ( int t = 0; t < 16; ++t )
                for ( int c = 0; c < 3; ++c )
                    points[t][c] = static_cast<float>( texels[t][c] );

            float low[3], high[3];
            DiagonalEndpoints<3>( points, low, high );

            int      bestLow[3] = {}, bestHigh[3] = {};
            int      bestIndices[16] = {};
            uint64_t bestError       = ~0ull;

            for ( int pass = 0; pass < 2; ++pass )
            {
                // ── WHY THE ENDPOINTS ARE ALSO TRIED PUSHED APART ───────────────────────────────
                //
                // Mode 11 stores each endpoint as TEN bits of a sixteen-bit internal value, so the
                // endpoint grid has a step of 64 and a value that lands between two codes is stuck
                // with the nearer one. Measured: a flat block at 60000 came back as 59072, 1.5 % out,
                // and it is flat -- there is nothing hard about it.
                //
                // The sixteen interpolated positions are a FINER grid than the endpoints are. Pushing
                // the pair one or two codes apart and letting the index land in between reaches values
                // the endpoints themselves cannot express: the same 60000 comes back within 0.1 %. It
                // costs three extra evaluations of a sixteen-texel loop and is the difference between
                // "BC6H is accurate to 1.5 %" and "to 0.1 %" on the smooth content this is for.
                for ( int spread = 0; spread < 4; ++spread )
                {
                    int lowCode[3], highCode[3];
                    int lowValue[3], highValue[3];
                    for ( int c = 0; c < 3; ++c )
                    {
                        const int lowAt =
                             Quantize10( std::clamp( static_cast<int>( std::lround( low[c] ) ), 0, 0xFFFF ) );
                        const int highAt =
                             Quantize10( std::clamp( static_cast<int>( std::lround( high[c] ) ), 0, 0xFFFF ) );
                        // Pushed apart in the direction each already lies; a pair that is the wrong way
                        // round after the refit is left alone rather than crossed over.
                        const int direction = highAt >= lowAt ? 1 : -1;
                        lowCode[c]          = std::clamp( lowAt - direction * spread, 0, 1023 );
                        highCode[c]         = std::clamp( highAt + direction * spread, 0, 1023 );
                        lowValue[c]         = Unquantize10( lowCode[c] );
                        highValue[c]        = Unquantize10( highCode[c] );
                    }

                    int      indices[16];
                    uint64_t error = 0;
                    for ( int t = 0; t < 16; ++t )
                    {
                        uint64_t best   = ~0ull;
                        int      bestAt = 0;
                        for ( int i = 0; i < 16; ++i )
                        {
                            uint64_t candidate = 0;
                            for ( int c = 0; c < 3; ++c )
                            {
                                const int64_t d =
                                     texels[t][c] - Interpolate( lowValue[c], highValue[c], kWeight4[i] );
                                candidate += static_cast<uint64_t>( d * d );
                            }
                            if ( candidate < best )
                            {
                                best   = candidate;
                                bestAt = i;
                            }
                        }
                        indices[t] = bestAt;
                        error += best;
                    }

                    if ( error < bestError )
                    {
                        bestError = error;
                        std::memcpy( bestLow, lowCode, sizeof( lowCode ) );
                        std::memcpy( bestHigh, highCode, sizeof( highCode ) );
                        std::memcpy( bestIndices, indices, sizeof( indices ) );
                    }
                }

                if ( pass == 0 )
                {
                    float refitLow[3], refitHigh[3];
                    if ( !RefitEndpoints<3>( points, bestIndices, refitLow, refitHigh ) )
                        break;
                    for ( int c = 0; c < 3; ++c )
                    {
                        low[c]  = std::clamp( refitLow[c], 0.0f, 65535.0f );
                        high[c] = std::clamp( refitHigh[c], 0.0f, 65535.0f );
                    }
                }
            }

            if ( bestIndices[0] >= 8 )
            {
                for ( int c = 0; c < 3; ++c )
                    std::swap( bestLow[c], bestHigh[c] );
                for ( int t = 0; t < 16; ++t )
                    bestIndices[t] = 15 - bestIndices[t];
            }

            BlockBits bits;
            // ── 0x03, AND THE FIVE HOURS THAT SAY SO ────────────────────────────────────────────
            //
            // BC6H's mode field is NOT the mode's number. The fourteen modes are spelled 0b00, 0b01,
            // then twelve five-bit codes that do not run in order, and MODE 11 — one subset, 10.10.10.10
            // endpoints, the mode this encoder writes — is the five-bit code 0x03. 0x0F is MODE 14,
            // whose sixty endpoint bits are a 16-bit base and three 4-bit DELTAS.
            //
            // This encoder wrote 0x0F. Its own decoder read 0x0F and unpacked it as mode 11 again, so
            // every round-trip test in the suite passed — the instrument agreed with itself about a
            // block no GPU in the world would have decoded the same way. What caught it was the only
            // witness that could: a frame. The baked environment came back as SIXTY of the wrong bits
            // and the IBL scene rendered PURE WHITE at all three elevations, 100 % of pixels differing
            // against the same scene computed without the cache.
            //
            // It is the reason the shot is part of this task and not a formality.
            bits.Put( 0x03u, 5 ); // mode 11 ("10 10 10 10"), whose five-bit code is 3
            for ( int c = 0; c < 3; ++c )
                bits.Put( static_cast<uint32_t>( bestLow[c] ), 10 );
            for ( int c = 0; c < 3; ++c )
                bits.Put( static_cast<uint32_t>( bestHigh[c] ), 10 );
            bits.Put( static_cast<uint32_t>( bestIndices[0] ), 3 );
            for ( int t = 1; t < 16; ++t )
                bits.Put( static_cast<uint32_t>( bestIndices[t] ), 4 );
            bits.CopyOut( out );
        }

        /// Decodes ONLY mode 11, the one mode the encoder above writes; any other mode is a visible refusal.
        void DecodeBc6hBlock( const unsigned char* in, uint16_t texels[16][3] )
        {
            BlockBits bits;
            bits.CopyIn( in );

            if ( bits.Get( 5 ) != 0x03u ) // see EncodeBc6hBlock: mode 11's code is 3, not 11 and not 0x0F
            {
                for ( int t = 0; t < 16; ++t )
                {
                    texels[t][0] = FloatToHalfBits( 1.0f );
                    texels[t][1] = 0;
                    texels[t][2] = FloatToHalfBits( 1.0f );
                }
                return;
            }

            int low[3], high[3];
            for ( int c = 0; c < 3; ++c )
                low[c] = Unquantize10( static_cast<int>( bits.Get( 10 ) ) );
            for ( int c = 0; c < 3; ++c )
                high[c] = Unquantize10( static_cast<int>( bits.Get( 10 ) ) );

            int indices[16];
            indices[0] = static_cast<int>( bits.Get( 3 ) );
            for ( int t = 1; t < 16; ++t )
                indices[t] = static_cast<int>( bits.Get( 4 ) );

            for ( int t = 0; t < 16; ++t )
                for ( int c = 0; c < 3; ++c )
                    texels[t][c] = FinishUnquantize( Interpolate( low[c], high[c], kWeight4[indices[t]] ) );
        }

        // ── BC4, AND THEREFORE BC5 ───────────────────────────────────────────────────────────────
        //
        // The simplest block in the family and the only one in this file that is written WHOLE rather
        // than in one mode out of many: BC4 has no modes, only two endpoint bytes and sixteen 3-bit
        // indices, and both of its palette shapes are implemented below. BC5 is not a second format
        // here — it is two BC4 blocks, red then green, which is exactly what the specification says it
        // is and why the encoder for it is four lines.
        //
        // THE DIVISIONS TRUNCATE, and that is the specification's arithmetic rather than a shortcut.
        // `(6*r0 + 1*r1) / 7` is what a GPU computes; rounding to nearest here would produce a decoder
        // that disagrees with hardware by one on most texels and a measurement that flattered the
        // encoder by comparing it with the wrong reference.

        /// The eight values a BC4 block's indices select. Which SHAPE depends on the order of the two
        /// endpoints — this is the format's one branch, and getting it backwards would turn indices 6
        /// and 7 from interpolants into the constants 0 and 255.
        void Bc4Palette( const int red0, const int red1, int palette[8] )
        {
            palette[0] = red0;
            palette[1] = red1;
            if ( red0 > red1 )
            {
                // Eight interpolated values. The encoder always produces this shape, because it puts
                // the block's maximum in red0 and its minimum in red1.
                for ( int i = 1; i <= 6; ++i )
                    palette[1 + i] = ( ( 7 - i ) * red0 + i * red1 ) / 7;
            }
            else
            {
                // Six interpolated values plus the two constants. A block whose two endpoints are
                // EQUAL lands here, and it still decodes exactly: every interpolant of r0 with itself
                // is r0, so index 0 is the right answer for every texel of a flat block.
                for ( int i = 1; i <= 4; ++i )
                    palette[1 + i] = ( ( 5 - i ) * red0 + i * red1 ) / 5;
                palette[6] = 0;
                palette[7] = 255;
            }
        }

        /// One channel of sixteen texels into eight bytes.
        void EncodeBc4Block( const int values[16], unsigned char* out )
        {
            int red0 = values[0];
            int red1 = values[0];
            for ( int t = 1; t < 16; ++t )
            {
                red0 = std::max( red0, values[t] );
                red1 = std::min( red1, values[t] );
            }

            int palette[8];
            Bc4Palette( red0, red1, palette );

            // 48 bits of indices, three per texel, texel 0 at the least significant end. Assembled in
            // a `uint64_t` and then written a byte at a time rather than through a cast, because the
            // byte order of the FILE is fixed and the byte order of this host is not the place to take
            // it from -- the same rule the container's header note states.
            uint64_t indices = 0;
            for ( int t = 0; t < 16; ++t )
            {
                int best     = 0;
                int bestCost = std::abs( values[t] - palette[0] );
                for ( int p = 1; p < 8; ++p )
                {
                    const int cost = std::abs( values[t] - palette[p] );
                    if ( cost < bestCost )
                    {
                        bestCost = cost;
                        best     = p;
                    }
                }
                indices |= static_cast<uint64_t>( best ) << ( 3 * t );
            }

            out[0] = static_cast<unsigned char>( red0 );
            out[1] = static_cast<unsigned char>( red1 );
            for ( int b = 0; b < 6; ++b )
                out[2 + b] = static_cast<unsigned char>( ( indices >> ( 8 * b ) ) & 0xFFu );
        }

        void DecodeBc4Block( const unsigned char* in, int values[16] )
        {
            int palette[8];
            Bc4Palette( in[0], in[1], palette );

            uint64_t indices = 0;
            for ( int b = 0; b < 6; ++b )
                indices |= static_cast<uint64_t>( in[2 + b] ) << ( 8 * b );

            for ( int t = 0; t < 16; ++t )
                values[t] = palette[( indices >> ( 3 * t ) ) & 0x7u];
        }

        /// WHICH SOURCE TEXEL A BLOCK TEXEL COMES FROM, with the edge CLAMPED. A level whose extent is
        /// not a multiple of four has partial blocks along its right and bottom edge, and the texels
        /// outside the image still take part in the endpoint fit. Left at zero they would be black, and
        /// black is not a neutral member of a least-squares fit — it drags the real texels' endpoints
        /// towards it. Clamping makes the padding a repeat of the edge, which the fit is indifferent to.
        uint32_t ClampedIndex( const uint32_t at, const uint32_t extent )
        {
            return at < extent ? at : extent - 1;
        }
    } // namespace

    BC6HCeilingCensus CensusBC6HCeiling( const float* rgba, const std::size_t texelCount )
    {
        BC6HCeilingCensus census;
        for ( std::size_t t = 0; t < texelCount; ++t )
        {
            bool clamped = false;
            for ( std::size_t c = 0; c < 3; ++c )
            {
                const float value = rgba[t * 4u + c];
                if ( !std::isfinite( value ) )
                {
                    ++census.NonFiniteChannels;
                    continue;
                }
                census.Peak = std::max( census.Peak, value );
                census.Sum += std::max( value, 0.0f );
                if ( value > kBC6HLargestValue )
                {
                    census.LostAboveCeiling += value - kBC6HLargestValue;
                    clamped = true;
                }
            }
            census.ClampedTexels += clamped ? 1u : 0u;
        }
        return census;
    }

    ImageFormat BlockFormatFor( const ImageFormat sourceFormat )
    {
        switch ( sourceFormat )
        {
            case ImageFormat::RGBA8F:
                return ImageFormat::BC7_UNORM;
            case ImageFormat::RGBA32F:
                return ImageFormat::BC6H_UFLOAT;
            default:
                // Everything else: depth, BGRA (a swapchain format, never cooked), the block formats
                // themselves, and the sentinel. `Count` is this function's "no" — it is not a format,
                // so it cannot be mistaken for one.
                return ImageFormat::Count;
        }
    }

    ImageFormat SourceFormatFor( const ImageFormat blockFormat )
    {
        switch ( blockFormat )
        {
            // THREE BLOCK FORMATS ARE ENCODED FROM ONE SOURCE, which is why this relation had to stop
            // being the inverse of `BlockFormatFor`. Until BC4 and BC5 existed there was exactly one
            // block format per source format, and both entry points below asked
            // `BlockFormatFor(source) == blockFormat` as their argument check. That question now has
            // three right answers for RGBA8 and would have refused two of them; this is the direction
            // of the relation that stays single-valued, so it is the one the checks ask.
            case ImageFormat::BC7_UNORM:
            case ImageFormat::BC5_UNORM:
            case ImageFormat::BC4_UNORM:
                return ImageFormat::RGBA8F;
            case ImageFormat::BC6H_UFLOAT:
                return ImageFormat::RGBA32F;
            default:
                return ImageFormat::Count; // not a block format at all
        }
    }

    Common::ResultStr<std::vector<unsigned char>>
    BlockCompressImage( const uint32_t width, const uint32_t height, const ImageFormat sourceFormat,
                        const ImageFormat blockFormat, const unsigned char* source, const std::size_t sourceBytes,
                        const BlockEncodeSettings& settings )
    {
        if ( width == 0 || height == 0 )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "a {}x{} image has no blocks to encode.", width, height );
        }
        if ( SourceFormatFor( blockFormat ) != sourceFormat )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "format {} is not encoded into format {} by this engine; format {} is encoded from "
                 "format {}.",
                 static_cast<uint32_t>( sourceFormat ), static_cast<uint32_t>( blockFormat ),
                 static_cast<uint32_t>( blockFormat ), static_cast<uint32_t>( SourceFormatFor( blockFormat ) ) );
        }

        const uint64_t expected = CalculateImageSize( width, height, sourceFormat );
        if ( source == nullptr || sourceBytes != expected )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "a {}x{} image of format {} is {} bytes and {} were handed in.", width, height,
                 static_cast<uint32_t>( sourceFormat ), expected, sourceBytes );
        }

        const uint32_t blocksX = BlocksAcross( width, blockFormat );
        const uint32_t blocksY = BlocksDown( height, blockFormat );
        // THE STRIDE COMES FROM THE FORMAT TABLE, not from the 16 that `BlockBits` happens to be. Those
        // are two different sixteens — one is how many bytes this format spends on a block, the other is
        // how wide a BPTC bit stream is — and writing the same literal for both is how they stop being
        // able to disagree the day a format spends eight.
        const std::size_t blockBytes = GetTexelBlock( blockFormat ).Bytes;

        std::vector<unsigned char> out(
             static_cast<std::size_t>( CalculateImageSize( width, height, blockFormat ) ) );

        // ROWS OF BLOCKS ARE ENCODED IN PARALLEL, and the bytes are still exactly the serial loop's:
        // every block is a pure function of its own 4x4 texels (edge texels are clamped reads of the
        // SOURCE, never of another block's output) and lands at an offset fixed by (bx, by) alone, so
        // which thread encodes it, and in what order, cannot reach the output. A 1024 radiance cube's
        // BC6H chain took ~17 s on one worker and the cache entry has to be on disk seconds after the
        // bake, not when the editor closes. ParallelRanges is safe from a worker (the environment
        // cache writer calls this from a JobSystem job) and degrades to the serial loop when the pool
        // is busy, so no caller can be made to wait on a job that never runs.
        // Four rows of a 1024 face's 256 is ~0.1 % of the face per claim: far above the pool's per-range
        // cost, and small enough that the slow rows (the sun) are absorbed by whoever finishes first.
        constexpr std::size_t kBlockRowsPerRange = 4;
        const void* const     bc7Options = blockFormat == ImageFormat::BC7_UNORM ? Bc7Options( settings.Bc7Effort ) : nullptr;
        const auto            encodeRow          = [&]( const uint32_t by )
        {
            for ( uint32_t bx = 0; bx < blocksX; ++bx )
            {
                unsigned char* block = out.data() + ( static_cast<std::size_t>( by ) * blocksX + bx ) * blockBytes;

                if ( blockFormat == ImageFormat::BC7_UNORM )
                {
                    unsigned char texels[16 * 4];
                    for ( uint32_t y = 0; y < 4; ++y )
                    {
                        for ( uint32_t x = 0; x < 4; ++x )
                        {
                            const uint32_t sx   = ClampedIndex( bx * 4 + x, width );
                            const uint32_t sy   = ClampedIndex( by * 4 + y, height );
                            const auto*    rgba = source + ( static_cast<std::size_t>( sy ) * width + sx ) * 4u;
                            std::memcpy( texels + ( y * 4 + x ) * 4u, rgba, 4u );
                        }
                    }
                    EncodeBc7Block( texels, block, bc7Options );
                }
                else if ( blockFormat == ImageFormat::BC4_UNORM || blockFormat == ImageFormat::BC5_UNORM )
                {
                    // ONE LOOP FOR BOTH, over the channels the format promises to keep. BC5's two
                    // 8-byte halves are the red plane and then the green plane, so the channel index
                    // and the half index are THE SAME NUMBER -- writing them as two hand-unrolled
                    // calls is how the green plane ends up encoding red on the day somebody edits one
                    // of them.
                    const uint32_t channels = PreservedChannelCount( blockFormat );
                    for ( uint32_t c = 0; c < channels; ++c )
                    {
                        int values[16];
                        for ( uint32_t y = 0; y < 4; ++y )
                        {
                            for ( uint32_t x = 0; x < 4; ++x )
                            {
                                const uint32_t sx = ClampedIndex( bx * 4 + x, width );
                                const uint32_t sy = ClampedIndex( by * 4 + y, height );
                                const auto* rgba  = source + ( static_cast<std::size_t>( sy ) * width + sx ) * 4u;
                                values[y * 4 + x] = rgba[c];
                            }
                        }
                        EncodeBc4Block( values, block + c * 8u );
                    }
                }
                else
                {
                    int texels[16][3];
                    for ( uint32_t y = 0; y < 4; ++y )
                    {
                        for ( uint32_t x = 0; x < 4; ++x )
                        {
                            const uint32_t sx   = ClampedIndex( bx * 4 + x, width );
                            const uint32_t sy   = ClampedIndex( by * 4 + y, height );
                            const float*   rgba = reinterpret_cast<const float*>( source ) +
                                                ( static_cast<std::size_t>( sy ) * width + sx ) * 4u;
                            for ( int c = 0; c < 3; ++c )
                                texels[y * 4 + x][c] = StartQuantize( FloatToHalfBits( rgba[c] ) );
                        }
                    }
                    EncodeBc6hBlock( texels, block );
                }
            }
        };

        Common::JobSystem::Get().ParallelRanges( blocksY, kBlockRowsPerRange,
                                                 [&]( const std::size_t firstRow, const std::size_t endRow )
                                                 {
                                                     for ( std::size_t by = firstRow; by < endRow; ++by )
                                                         encodeRow( static_cast<uint32_t>( by ) );
                                                 } );

        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<std::vector<unsigned char>>
    BlockDecompressImage( const uint32_t width, const uint32_t height, const ImageFormat blockFormat,
                          const ImageFormat destFormat, const unsigned char* source,
                          const std::size_t sourceBytes )
    {
        if ( width == 0 || height == 0 )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "a {}x{} image has no blocks to decode.", width, height );
        }
        if ( SourceFormatFor( blockFormat ) != destFormat )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "format {} does not decode into format {}; it decodes into format {}.",
                 static_cast<uint32_t>( blockFormat ), static_cast<uint32_t>( destFormat ),
                 static_cast<uint32_t>( SourceFormatFor( blockFormat ) ) );
        }

        const uint64_t expected = CalculateImageSize( width, height, blockFormat );
        if ( source == nullptr || sourceBytes != expected )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "a {}x{} image of block format {} is {} bytes and {} were handed in.", width, height,
                 static_cast<uint32_t>( blockFormat ), expected, sourceBytes );
        }

        const uint32_t    blocksX    = BlocksAcross( width, blockFormat );
        const uint32_t    blocksY    = BlocksDown( height, blockFormat );
        const std::size_t blockBytes = GetTexelBlock( blockFormat ).Bytes;

        std::vector<unsigned char> out(
             static_cast<std::size_t>( CalculateImageSize( width, height, destFormat ) ) );

        for ( uint32_t by = 0; by < blocksY; ++by )
        {
            for ( uint32_t bx = 0; bx < blocksX; ++bx )
            {
                const unsigned char* block =
                     source + ( static_cast<std::size_t>( by ) * blocksX + bx ) * blockBytes;

                if ( blockFormat == ImageFormat::BC7_UNORM )
                {
                    unsigned char texels[16 * 4];
                    DecodeBc7Block( block, texels );
                    for ( uint32_t y = 0; y < 4; ++y )
                    {
                        for ( uint32_t x = 0; x < 4; ++x )
                        {
                            const uint32_t dx = bx * 4 + x;
                            const uint32_t dy = by * 4 + y;
                            if ( dx >= width || dy >= height )
                                continue;
                            auto* rgba = out.data() + ( static_cast<std::size_t>( dy ) * width + dx ) * 4u;
                            std::memcpy( rgba, texels + ( y * 4 + x ) * 4u, 4u );
                        }
                    }
                }
                else if ( blockFormat == ImageFormat::BC4_UNORM || blockFormat == ImageFormat::BC5_UNORM )
                {
                    // WHAT A SAMPLER RETURNS, AND NOT WHAT WAS PUT IN. The channels this format does
                    // not carry come back as 0, 0, 1 from the hardware — that is the whole reason a
                    // shader reading `.rgb` off a BC5 normal map gets a Z of minus one — so the
                    // decoder writes zeros and an opaque alpha rather than leaving the destination's
                    // previous contents or copying the source's. The measurement then compares only
                    // `PreservedChannelCount` channels, which is the promise these zeros are the other
                    // half of.
                    const uint32_t channels = PreservedChannelCount( blockFormat );
                    int            planes[2][16];
                    for ( uint32_t c = 0; c < channels; ++c )
                        DecodeBc4Block( block + c * 8u, planes[c] );

                    for ( uint32_t y = 0; y < 4; ++y )
                    {
                        for ( uint32_t x = 0; x < 4; ++x )
                        {
                            const uint32_t dx = bx * 4 + x;
                            const uint32_t dy = by * 4 + y;
                            if ( dx >= width || dy >= height )
                                continue;
                            auto* rgba = out.data() + ( static_cast<std::size_t>( dy ) * width + dx ) * 4u;
                            for ( uint32_t c = 0; c < 3; ++c )
                            {
                                rgba[c] = c < channels ? static_cast<unsigned char>( planes[c][y * 4 + x] )
                                                       : static_cast<unsigned char>( 0 );
                            }
                            rgba[3] = 255;
                        }
                    }
                }
                else
                {
                    uint16_t texels[16][3];
                    DecodeBc6hBlock( block, texels );
                    for ( uint32_t y = 0; y < 4; ++y )
                    {
                        for ( uint32_t x = 0; x < 4; ++x )
                        {
                            const uint32_t dx = bx * 4 + x;
                            const uint32_t dy = by * 4 + y;
                            if ( dx >= width || dy >= height )
                                continue;
                            float* rgba = reinterpret_cast<float*>( out.data() ) +
                                          ( static_cast<std::size_t>( dy ) * width + dx ) * 4u;
                            for ( int c = 0; c < 3; ++c )
                                rgba[c] = HalfBitsToFloat( texels[y * 4 + x][c] );
                            rgba[3] = 1.0f; // the format has no alpha; see EncodeBc6hBlock
                        }
                    }
                }
            }
        }

        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Core::Formats
