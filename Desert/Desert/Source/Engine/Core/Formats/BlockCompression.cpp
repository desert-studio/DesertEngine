#include "BlockCompression.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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
            if ( value > 65504.0f )
                value = 65504.0f;

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

        // ── BC7, MODE 6 ──────────────────────────────────────────────────────────────────────────
        //
        // One subset, four RGBA endpoint channels at 7 bits plus one shared p-bit each, sixteen 4-bit
        // indices. 7 + 56 + 2 + 63 = 128, and the missing bit of index 0 is the ANCHOR: its high bit is
        // implicit zero, so the encoder must arrange for texel 0 to fall in the lower half of the ramp
        // (by swapping the endpoints and inverting every index if it does not). Getting that wrong
        // produces a block that decodes, looks nearly right, and is wrong in one texel of sixteen.

        struct Bc7Endpoints
        {
            int Low[4];
            int High[4];
            int PBit[2];
        };

        /// A 7-bit code plus a p-bit reconstructs to `(code << 1) | p`. Given the p-bit we must use,
        /// this is the code that lands nearest @p value.
        int QuantizeSevenPlusP( const int value, const int pbit )
        {
            const int code = ( std::clamp( value, 0, 255 ) - pbit + 1 ) >> 1;
            return std::clamp( code, 0, 127 );
        }

        void Bc7Palette( const Bc7Endpoints& endpoints, int palette[16][4] )
        {
            int low[4], high[4];
            for ( int c = 0; c < 4; ++c )
            {
                low[c]  = ( endpoints.Low[c] << 1 ) | endpoints.PBit[0];
                high[c] = ( endpoints.High[c] << 1 ) | endpoints.PBit[1];
            }
            for ( int i = 0; i < 16; ++i )
                for ( int c = 0; c < 4; ++c )
                    palette[i][c] = Interpolate( low[c], high[c], kWeight4[i] );
        }

        uint64_t Bc7ChooseIndices( const int texels[16][4], const int palette[16][4], int indices[16] )
        {
            uint64_t total = 0;
            for ( int t = 0; t < 16; ++t )
            {
                uint64_t best   = ~0ull;
                int      bestAt = 0;
                for ( int i = 0; i < 16; ++i )
                {
                    uint64_t error = 0;
                    for ( int c = 0; c < 4; ++c )
                    {
                        const int64_t d = texels[t][c] - palette[i][c];
                        error += static_cast<uint64_t>( d * d );
                    }
                    if ( error < best )
                    {
                        best   = error;
                        bestAt = i;
                    }
                }
                indices[t] = bestAt;
                total += best;
            }
            return total;
        }

        void EncodeBc7Block( const int texels[16][4], unsigned char* out )
        {
            float points[16][4];
            for ( int t = 0; t < 16; ++t )
                for ( int c = 0; c < 4; ++c )
                    points[t][c] = static_cast<float>( texels[t][c] );

            float low[4], high[4];
            DiagonalEndpoints<4>( points, low, high );

            Bc7Endpoints best{};
            int          bestIndices[16] = {};
            uint64_t     bestError       = ~0ull;

            // ── THE P-BIT IS SHARED BY ALL FOUR CHANNELS, AND THAT IS WHY ALPHA GETS TO CHOOSE ──
            //
            // Mode 6 stores each endpoint as four 7-bit values plus ONE p-bit, and the p-bit is the
            // low bit of all four reconstructed 8-bit channels at once. So an endpoint can express
            // every even value or every odd value, never both — and a block of (200,200,200,255)
            // cannot be exact: 200 needs an even endpoint and 255 an odd one.
            //
            // MEASURED BY SQUARED ERROR ALONE, THE ENCODER PICKS EVEN AND SHIPS ALPHA 254. Three
            // channels off by one is a worse number than one channel off by one, so the search is
            // right and the ANSWER is wrong: almost every LDR texture in a project is fully opaque,
            // and "alpha is 254 everywhere" is a systematic bias in the one channel whose exact value
            // a material may branch on, paid to save an invisible LSB in three channels that are
            // about to be filtered, tonemapped and quantized to eight bits anyway.
            //
            // So when the block's alpha is CONSTANT — which is what opaque means, and what a fully
            // transparent cut-out means — its parity is fixed first and the colour pays the LSB. When
            // alpha varies the trade does not arise and the search runs over all four combinations.
            const bool constantAlpha = [&]( const int( *t )[4] )
            {
                for ( int i = 1; i < 16; ++i )
                    if ( t[i][3] != t[0][3] )
                        return false;
                return true;
            }( texels );
            const int forcedPBit = texels[0][3] & 1;

            for ( int pass = 0; pass < 2; ++pass )
            {
                for ( int p0 = constantAlpha ? forcedPBit : 0; p0 < ( constantAlpha ? forcedPBit + 1 : 2 ); ++p0 )
                {
                    for ( int p1 = constantAlpha ? forcedPBit : 0; p1 < ( constantAlpha ? forcedPBit + 1 : 2 );
                          ++p1 )
                    {
                        Bc7Endpoints candidate{};
                        candidate.PBit[0] = p0;
                        candidate.PBit[1] = p1;
                        for ( int c = 0; c < 4; ++c )
                        {
                            candidate.Low[c] = QuantizeSevenPlusP( static_cast<int>( std::lround( low[c] ) ), p0 );
                            candidate.High[c] =
                                 QuantizeSevenPlusP( static_cast<int>( std::lround( high[c] ) ), p1 );
                        }

                        int palette[16][4];
                        Bc7Palette( candidate, palette );
                        int            indices[16];
                        const uint64_t error = Bc7ChooseIndices( texels, palette, indices );
                        if ( error < bestError )
                        {
                            bestError = error;
                            best      = candidate;
                            std::memcpy( bestIndices, indices, sizeof( indices ) );
                        }
                    }
                }

                // Refit once against the indices the first pass chose, then try the four p-bit
                // combinations again around the better line.
                if ( pass == 0 )
                {
                    float refitLow[4], refitHigh[4];
                    if ( !RefitEndpoints<4>( points, bestIndices, refitLow, refitHigh ) )
                        break;
                    for ( int c = 0; c < 4; ++c )
                    {
                        low[c]  = std::clamp( refitLow[c], 0.0f, 255.0f );
                        high[c] = std::clamp( refitHigh[c], 0.0f, 255.0f );
                    }
                }
            }

            // THE ANCHOR. Index 0 is written in three bits, so its high bit must be zero. When it is
            // not, the SAME block is expressed by exchanging the endpoints and mirroring every index.
            if ( bestIndices[0] >= 8 )
            {
                for ( int c = 0; c < 4; ++c )
                    std::swap( best.Low[c], best.High[c] );
                std::swap( best.PBit[0], best.PBit[1] );
                for ( int t = 0; t < 16; ++t )
                    bestIndices[t] = 15 - bestIndices[t];
            }

            BlockBits bits;
            bits.Put( 1u << 6, 7 ); // mode 6: six zeroes then a one
            for ( int c = 0; c < 4; ++c )
            {
                bits.Put( static_cast<uint32_t>( best.Low[c] ), 7 );
                bits.Put( static_cast<uint32_t>( best.High[c] ), 7 );
            }
            bits.Put( static_cast<uint32_t>( best.PBit[0] ), 1 );
            bits.Put( static_cast<uint32_t>( best.PBit[1] ), 1 );
            bits.Put( static_cast<uint32_t>( bestIndices[0] ), 3 );
            for ( int t = 1; t < 16; ++t )
                bits.Put( static_cast<uint32_t>( bestIndices[t] ), 4 );
            bits.CopyOut( out );
        }

        /// Decodes ONLY what this encoder writes. A mode this file does not produce is not decoded —
        /// it is filled with magenta, which is a visible refusal rather than a plausible colour.
        void DecodeBc7Block( const unsigned char* in, int texels[16][4] )
        {
            BlockBits bits;
            bits.CopyIn( in );

            int mode = 0;
            while ( mode < 8 && bits.Get( 1 ) == 0 )
                ++mode;
            if ( mode != 6 )
            {
                for ( int t = 0; t < 16; ++t )
                {
                    texels[t][0] = 255;
                    texels[t][1] = 0;
                    texels[t][2] = 255;
                    texels[t][3] = 255;
                }
                return;
            }

            Bc7Endpoints endpoints{};
            for ( int c = 0; c < 4; ++c )
            {
                endpoints.Low[c]  = static_cast<int>( bits.Get( 7 ) );
                endpoints.High[c] = static_cast<int>( bits.Get( 7 ) );
            }
            endpoints.PBit[0] = static_cast<int>( bits.Get( 1 ) );
            endpoints.PBit[1] = static_cast<int>( bits.Get( 1 ) );

            int palette[16][4];
            Bc7Palette( endpoints, palette );

            int indices[16];
            indices[0] = static_cast<int>( bits.Get( 3 ) );
            for ( int t = 1; t < 16; ++t )
                indices[t] = static_cast<int>( bits.Get( 4 ) );

            for ( int t = 0; t < 16; ++t )
                for ( int c = 0; c < 4; ++c )
                    texels[t][c] = palette[indices[t]][c];
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

        /// Decodes ONLY mode 11, for the same reason and with the same visible refusal as BC7's.
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

    Common::ResultStr<std::vector<unsigned char>>
    BlockCompressImage( const uint32_t width, const uint32_t height, const ImageFormat sourceFormat,
                        const ImageFormat blockFormat, const unsigned char* source, const std::size_t sourceBytes )
    {
        if ( width == 0 || height == 0 )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "a {}x{} image has no blocks to encode.", width, height );
        }
        if ( BlockFormatFor( sourceFormat ) != blockFormat )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "format {} is not encoded into format {} by this engine; BlockFormatFor says {}.",
                 static_cast<uint32_t>( sourceFormat ), static_cast<uint32_t>( blockFormat ),
                 static_cast<uint32_t>( BlockFormatFor( sourceFormat ) ) );
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

        for ( uint32_t by = 0; by < blocksY; ++by )
        {
            for ( uint32_t bx = 0; bx < blocksX; ++bx )
            {
                unsigned char* block = out.data() + ( static_cast<std::size_t>( by ) * blocksX + bx ) * blockBytes;

                if ( blockFormat == ImageFormat::BC7_UNORM )
                {
                    int texels[16][4];
                    for ( uint32_t y = 0; y < 4; ++y )
                    {
                        for ( uint32_t x = 0; x < 4; ++x )
                        {
                            const uint32_t sx   = ClampedIndex( bx * 4 + x, width );
                            const uint32_t sy   = ClampedIndex( by * 4 + y, height );
                            const auto*    rgba = source + ( static_cast<std::size_t>( sy ) * width + sx ) * 4u;
                            for ( int c = 0; c < 4; ++c )
                                texels[y * 4 + x][c] = rgba[c];
                        }
                    }
                    EncodeBc7Block( texels, block );
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
        }

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
        if ( BlockFormatFor( destFormat ) != blockFormat )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "format {} does not decode into format {}; BlockFormatFor says {} encodes into {}.",
                 static_cast<uint32_t>( blockFormat ), static_cast<uint32_t>( destFormat ),
                 static_cast<uint32_t>( destFormat ), static_cast<uint32_t>( BlockFormatFor( destFormat ) ) );
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
                    int texels[16][4];
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
                            for ( int c = 0; c < 4; ++c )
                                rgba[c] = static_cast<unsigned char>( texels[y * 4 + x][c] );
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
