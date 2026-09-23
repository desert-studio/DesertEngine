#include "TextureBinary.hpp"

#include <Engine/Core/Formats/BlockCompression.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/Lz4Block.hpp>

// FOR THE THRESHOLD AND NOTHING ELSE. The question "is this compression worth keeping" has exactly one
// answer in this project, and it is derived (see PakFile.hpp: the break-even device for a ratio of 1.6
// at the decoder's measured speed). A second copy of the number here would be a second thing to keep in
// step, and the two would answer differently about the same bytes the first time one moved.
#include <Common/Utilities/PakFile.hpp>

// spdlog's BUNDLED fmt, which is what every other translation unit in the engine uses.
// `<fmt/format.h>` names a standalone fmt that exists in NO checkout of this repository: it
// compiled here only because Homebrew happened to have one at /opt/homebrew/include, and it
// broke two test suites on CI the first time a machine without it tried.
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>
#include <type_traits>

namespace Desert::Assets::Serialization
{
    namespace
    {
        // ── THE WIRE RECORDS ─────────────────────────────────────────────────────────────────────
        //
        // Fixed-width members and nothing else. These sizes ARE the file format: a record that grows
        // by four bytes on one compiler reads every following record shifted, and a texture made of
        // shifted bytes is a texture that draws — wrongly, silently, and only on the platform that did
        // not write the file.

        struct FileHeader
        {
            char     Magic[8];
            uint32_t ByteOrder;
            uint32_t Version;
            uint32_t HeaderSize; // so a reader can step over a header a later version grew
            uint32_t Width;
            uint32_t Height;
            uint32_t Format; // an ImageFormat enumerator, travelling at a width the file fixes
            uint32_t LevelCount;
            uint32_t Flags; // no version defines one; a non-zero value is REFUSED, see below
            uint32_t SourceKeyLength;
            uint32_t SourceKeyOffset; // from file start
            uint64_t SourceContentHash;
            uint64_t EncoderHash;  // v3: the settings that produced these pixels; 0 = none recorded
            uint64_t PayloadBytes; // declared sum of the level sizes, padding excluded
            uint64_t FileSize;     // declared; compared against the bytes actually in hand
            uint64_t Handle;
            uint64_t StoredPayloadBytes; // v2: declared sum of the STORED level sizes, padding excluded
            uint32_t LayerCount;         // v3: images per level -- 1, or 6 for a cube
            uint32_t Kind;               // v3: a TextureKind, travelling at a width the file fixes
            uint32_t Intent;             // the authored TextureIntent this was cooked FOR; 0 = nobody said
            uint32_t Reserved[5];
        };
        static_assert( sizeof( FileHeader ) == kTextureBinaryHeaderSize );
        static_assert( alignof( FileHeader ) == 8 );
        static_assert( offsetof( FileHeader, SourceContentHash ) == 48 );
        static_assert( offsetof( FileHeader, FileSize ) == 72 );
        static_assert( offsetof( FileHeader, Handle ) == 80 );
        // v2 SPENT ONE OF THE RESERVED SLOTS RATHER THAN GROWING THE HEADER. 128 bytes is what
        // `kTextureBinaryHeaderSize` promises and what every reader's first read is sized by; the
        // reserve existed for exactly this.
        static_assert( offsetof( FileHeader, StoredPayloadBytes ) == 88 );
        // v3 SPENT TWO MORE OF THEM, FOR THE SAME REASON. The header is still 128 bytes and still at a
        // fixed offset, so every reader's first read is sized by the same constant it always was.
        static_assert( offsetof( FileHeader, LayerCount ) == 96 );
        static_assert( offsetof( FileHeader, Kind ) == 100 );
        static_assert( sizeof( TextureKind ) == 4, "the texture kind is a u32 in the file" );
        // THE AUTHORED INTENT SPENT A RESERVED SLOT AND DID NOT MOVE THE VERSION, and that is a
        // decision with an argument rather than a saving. The version gate exists because a reader
        // that MISREADS a file is worse than one that refuses it, and the two earlier bumps were
        // forced by exactly that: v2 changed the level row's SIZE, so a v1 file read with a v2 stride
        // decodes every level after the first from the wrong place, and v3 changed the number of ROWS,
        // where a v3 reader handed a v2 file would take `LayerCount` out of a reserved zero and build
        // a texture with NO layers -- a plausible-looking wrong answer.
        //
        // This word cannot do either. Zero is the value every `.tex` ever written already carries
        // there, it is a DEFINED enumerator (`Unspecified`), and it is the same value this cook writes
        // for a texture nobody authored an intent for. So an old file and a new unauthored one are
        // byte-for-byte identical and decode identically -- which is not a migration that was skipped,
        // it is a migration with nothing in it. An UNKNOWN value is still refused by name below, which
        // is the case a future enumerator would produce and the one a reader genuinely cannot honour.
        static_assert( offsetof( FileHeader, Intent ) == 104 );
        static_assert( sizeof( Core::Formats::TextureIntent ) == 4, "the authored intent is a u32 in the file" );

        struct LevelRow
        {
            uint64_t ByteOffset; // from FILE start -- so a streaming read can seek straight to it
            uint32_t ByteSize;   // after decoding
            uint32_t RowPitch;
            uint32_t StoredSize; // v2: bytes actually in the file; == ByteSize for a Store level
            uint32_t Codec;      // v2: a TextureLevelCodec, travelling at a width the file fixes
        };
        static_assert( sizeof( LevelRow ) == 24 );
        static_assert( alignof( LevelRow ) == 8 );
        static_assert( sizeof( TextureLevelCodec ) == 4, "the level codec is a u32 in the file" );
        // THE ROW GREW AT ITS END, and the three v1 fields kept their offsets. That is not what makes a
        // v1 file readable — nothing does, the version gate refuses it — it is so that the ONE thing a
        // reader does with a row (a memcpy at a computed stride) has one fewer way to be wrong.
        static_assert( offsetof( LevelRow, ByteOffset ) == 0 );
        static_assert( offsetof( LevelRow, ByteSize ) == 8 );
        static_assert( offsetof( LevelRow, RowPitch ) == 12 );

        /// Reads back as 0x01020304 on a big-endian host, which is the whole point of writing it.
        constexpr uint32_t kByteOrderTag = 0x04030201u;

        // THE WRITER REFUSES TO EXIST ON A HOST IT COULD NOT READ ITS OWN FILE BACK ON. The tag above
        // lets the READER name a foreign byte order; these two make the BUILD name it, which is the
        // only place it can be answered, because this file writes raw object representations.
        static_assert( std::endian::native == std::endian::little,
                       "the cooked texture container is little-endian; see kByteOrderTag" );
        static_assert( CHAR_BIT == 8, "the container's sizes are in 8-bit bytes" );

        void Append( std::string& out, const void* bytes, const size_t count )
        {
            if ( count == 0 )
                return;
            out.append( static_cast<const char*>( bytes ), count );
        }

        constexpr uint64_t AlignUp( const uint64_t at )
        {
            return ( at + ( kTextureLevelAlignment - 1 ) ) & ~( kTextureLevelAlignment - 1 );
        }

        /// Pad @p out up to the next level boundary. Every level starts there, which is what makes a
        /// level `memcpy`-able into mapped memory and its `VkBufferImageCopy::bufferOffset` a legal
        /// multiple of any texel block size this format will carry.
        void PadToLevelBoundary( std::string& out )
        {
            while ( ( out.size() % kTextureLevelAlignment ) != 0 )
                out.push_back( '\0' );
        }

        /// The next extent down the chain. `max(1, n/2)` is the rule Vulkan's mip chain is defined by,
        /// and `MipChainLength` in ImageFormat.hpp counts the levels this produces.
        constexpr uint32_t HalfExtent( const uint32_t n )
        {
            return n > 1u ? n / 2u : 1u;
        }

        /// Every level's extent, level 0 first, by the same halving rule. ONE SPELLING: the encoder,
        /// the layout helper and the decoder all ask this, so a change to the chain rule cannot reach
        /// one of them and miss another.
        std::vector<std::pair<uint32_t, uint32_t>> LevelExtents( const uint32_t width, const uint32_t height,
                                                                 const uint32_t levelCount )
        {
            std::vector<std::pair<uint32_t, uint32_t>> extents( levelCount );
            uint32_t                                   w = width, h = height;
            for ( uint32_t level = 0; level < levelCount; ++level )
            {
                extents[level] = { w, h };
                w              = HalfExtent( w );
                h              = HalfExtent( h );
            }
            return extents;
        }

        /// The shape checks every entry point owes before it believes a (level, layer) table: what the
        /// two columns mean TOGETHER. Returns an empty string when the shape is legal.
        std::string LayoutRefusal( const uint32_t width, const uint32_t height, const uint32_t layerCount,
                                   const TextureKind kind )
        {
            if ( layerCount == 0 )
                return "a texture with zero array layers holds no image at all";
            if ( kind != TextureKind::Texture2D && kind != TextureKind::Cube )
            {
                return fmt::format( "texture kind {} is not one this version knows ({} = 2D, {} = cube)",
                                    static_cast<uint32_t>( kind ), static_cast<uint32_t>( TextureKind::Texture2D ),
                                    static_cast<uint32_t>( TextureKind::Cube ) );
            }
            if ( kind == TextureKind::Texture2D && layerCount != 1 )
            {
                return fmt::format(
                     "kind {} (2D) names {} array layers; this version stores one layer per 2D texture, "
                     "and a layered image has to say what its layers mean",
                     static_cast<uint32_t>( kind ), layerCount );
            }
            if ( kind == TextureKind::Cube )
            {
                if ( layerCount != kTextureCubeLayerCount )
                {
                    return fmt::format( "a cube is {} layers and this one names {}", kTextureCubeLayerCount,
                                        layerCount );
                }
                if ( width != height )
                {
                    return fmt::format( "a cube face is square and this one is {}x{}", width, height );
                }
            }
            return {};
        }

        /// One 2x2 box step, in the component type the format stores. The source block is CLAMPED
        /// rather than assumed even: at an odd extent the pair degenerates to a single texel, so the
        /// last row or column contributes instead of being dropped.
        template <typename Component, typename Accumulator>
        void BoxDownsample( const Component* src, const uint32_t srcW, const uint32_t srcH, Component* dst,
                            const uint32_t dstW, const uint32_t dstH, const uint32_t components )
        {
            for ( uint32_t y = 0; y < dstH; ++y )
            {
                const uint32_t y0 = std::min( y * 2u, srcH - 1u );
                const uint32_t y1 = std::min( y * 2u + 1u, srcH - 1u );
                for ( uint32_t x = 0; x < dstW; ++x )
                {
                    const uint32_t x0 = std::min( x * 2u, srcW - 1u );
                    const uint32_t x1 = std::min( x * 2u + 1u, srcW - 1u );
                    for ( uint32_t c = 0; c < components; ++c )
                    {
                        const Accumulator sum =
                             static_cast<Accumulator>(
                                  src[( static_cast<size_t>( y0 ) * srcW + x0 ) * components + c] ) +
                             static_cast<Accumulator>(
                                  src[( static_cast<size_t>( y0 ) * srcW + x1 ) * components + c] ) +
                             static_cast<Accumulator>(
                                  src[( static_cast<size_t>( y1 ) * srcW + x0 ) * components + c] ) +
                             static_cast<Accumulator>(
                                  src[( static_cast<size_t>( y1 ) * srcW + x1 ) * components + c] );
                        if constexpr ( std::is_integral_v<Component> )
                        {
                            // ROUND, DO NOT TRUNCATE. `sum / 4` alone biases every level darker by up
                            // to 0.75/255, and the bias compounds down a ten-level chain into a
                            // visibly darker distant texture. +2 is the half-ulp the blit chain's
                            // fixed-point average already applied.
                            dst[( static_cast<size_t>( y ) * dstW + x ) * components + c] =
                                 static_cast<Component>( ( sum + 2 ) / 4 );
                        }
                        else
                        {
                            dst[( static_cast<size_t>( y ) * dstW + x ) * components + c] =
                                 static_cast<Component>( sum / static_cast<Accumulator>( 4 ) );
                        }
                    }
                }
            }
        }
    } // namespace

    bool LooksLikeTextureBinary( const std::string_view bytes )
    {
        return bytes.size() >= sizeof( kTextureBinaryMagic ) &&
               std::memcmp( bytes.data(), kTextureBinaryMagic, sizeof( kTextureBinaryMagic ) ) == 0;
    }

    uint64_t TextureBinaryMetadataBytes( const std::string_view headerBytes )
    {
        if ( !LooksLikeTextureBinary( headerBytes ) || headerBytes.size() < sizeof( FileHeader ) )
            return 0;

        FileHeader header{};
        std::memcpy( &header, headerBytes.data(), sizeof( header ) );
        return static_cast<uint64_t>( header.SourceKeyOffset ) + header.SourceKeyLength;
    }

    uint64_t SourceSignature( const void* bytes, const std::size_t size )
    {
        return ( static_cast<uint64_t>( static_cast<uint32_t>( size ) ) << 32 ) |
               Common::Utils::Crc32c( bytes, size );
    }

    std::string StaleCookRefusal( const std::string_view whatFor )
    {
        return fmt::format(
             "'{}' does not carry the cooked-texture magic, so it predates the pixel container (it is the "
             "retired JSON manifest, which held no pixels at all). Cooked content is derived and is not "
             "migrated: delete it and cook again (Assets > Rebuild Cooked Assets, or re-import the source "
             "image).",
             std::string( whatFor ) );
    }

    Common::ResultStr<std::vector<TextureLevel>> BuildMipChain( const uint32_t width, const uint32_t height,
                                                                const Core::Formats::ImageFormat  format,
                                                                const std::vector<unsigned char>& base,
                                                                std::vector<unsigned char>&       chainOut )
    {
        using Fmt = Core::Formats::ImageFormat;

        if ( width == 0 || height == 0 )
        {
            return Common::MakeFormattedError<std::vector<TextureLevel>>(
                 "a mip chain was asked for a {}x{} image; a texture with a zero extent has no levels.", width,
                 height );
        }
        if ( format != Fmt::RGBA8F && format != Fmt::RGBA32F )
        {
            // NAMED, NOT ASSUMED. The box filter below knows two component types, and a format it does
            // not know would otherwise be downsampled as the wrong one — bytes read as floats is the
            // kind of wrong answer that draws.
            return Common::MakeFormattedError<std::vector<TextureLevel>>(
                 "the cook builds mip chains for RGBA8F and RGBA32F only; format {} was asked for.",
                 static_cast<uint32_t>( format ) );
        }

        const uint32_t bpp      = Core::Formats::GetBytesPerPixel( format );
        const size_t   baseSize = static_cast<size_t>( width ) * height * bpp;
        if ( base.size() != baseSize )
        {
            return Common::MakeFormattedError<std::vector<TextureLevel>>(
                 "the base level of a {}x{} image at {} bytes per pixel is {} bytes and {} were handed in.", width,
                 height, bpp, baseSize, base.size() );
        }

        const uint32_t levelCount = Core::Formats::MipChainLength( std::max( width, height ) );

        // BUILT LARGEST FIRST, STORED SMALLEST FIRST. The filter can only run downwards — level i+1 is
        // made out of level i — so the chain is computed in image order into a scratch buffer and then
        // emitted in reverse. The reversal is the file's layout decision (T3 §5c, the resident tail as
        // a contiguous prefix), not the filter's, and keeping the two apart is what stops a future
        // change to one from silently changing the other.
        std::vector<std::vector<unsigned char>> scratch;
        scratch.reserve( levelCount );
        scratch.push_back( base );

        uint32_t srcW = width, srcH = height;
        for ( uint32_t level = 1; level < levelCount; ++level )
        {
            const uint32_t dstW = HalfExtent( srcW );
            const uint32_t dstH = HalfExtent( srcH );

            std::vector<unsigned char> next( static_cast<size_t>( dstW ) * dstH * bpp );
            if ( format == Fmt::RGBA8F )
            {
                BoxDownsample<unsigned char, uint32_t>( scratch.back().data(), srcW, srcH, next.data(), dstW, dstH,
                                                        4u );
            }
            else
            {
                BoxDownsample<float, float>( reinterpret_cast<const float*>( scratch.back().data() ), srcW, srcH,
                                             reinterpret_cast<float*>( next.data() ), dstW, dstH, 4u );
            }
            scratch.push_back( std::move( next ) );
            srcW = dstW;
            srcH = dstH;
        }

        std::vector<TextureLevel> levels( levelCount );
        chainOut.clear();

        const std::vector<std::pair<uint32_t, uint32_t>> extents = LevelExtents( width, height, levelCount );

        for ( uint32_t i = 0; i < levelCount; ++i )
        {
            const uint32_t level = levelCount - 1 - i; // smallest first
            while ( ( chainOut.size() % kTextureLevelAlignment ) != 0 )
                chainOut.push_back( 0 );

            levels[level].Width      = extents[level].first;
            levels[level].Height     = extents[level].second;
            levels[level].ByteOffset = chainOut.size();
            levels[level].ByteSize   = scratch[level].size();
            levels[level].RowPitch   = Core::Formats::CalculateRowPitch( extents[level].first, format );

            chainOut.insert( chainOut.end(), scratch[level].begin(), scratch[level].end() );
        }

        return Common::MakeSuccess( std::move( levels ) );
    }

    Common::ResultStr<std::vector<unsigned char>> BlockCompressChain(
         const uint32_t width, const uint32_t height, const uint32_t levelCount, const uint32_t layerCount,
         const Core::Formats::ImageFormat sourceFormat, const Core::Formats::ImageFormat blockFormat,
         const std::vector<TextureLevel>& sourceLevels, const std::vector<unsigned char>& sourcePixels )
    {
        if ( !Core::Formats::IsBlockCompressed( blockFormat ) )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "format {} is not block-compressed; there is nothing for BlockCompressChain to do and a "
                 "caller that asked for it meant something else.",
                 static_cast<uint32_t>( blockFormat ) );
        }
        if ( sourceLevels.size() != static_cast<std::size_t>( levelCount ) * layerCount )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "a table of {} rows was handed in for {} levels x {} layers, which is {} rows.",
                 sourceLevels.size(), levelCount, layerCount,
                 static_cast<std::size_t>( levelCount ) * layerCount );
        }

        const auto extents = LevelExtents( width, height, levelCount );

        std::vector<unsigned char> out;
        out.reserve( static_cast<std::size_t>(
             TightlyPackedChainBytes( width, height, levelCount, layerCount, blockFormat ) ) );

        // TABLE ORDER: level 0 first, layers together inside a level. That is the order
        // `BuildLevelTable` reads its input in, and the order it reverses on the way to the file. This
        // loop must NOT reverse anything — the one place the physical order is decided is there.
        for ( uint32_t level = 0; level < levelCount; ++level )
        {
            for ( uint32_t layer = 0; layer < layerCount; ++layer )
            {
                const TextureLevel& source = sourceLevels[TextureLevelIndex( level, layer, layerCount )];
                const uint64_t      expect = Core::Formats::CalculateImageSize( extents[level].first,
                                                                                extents[level].second, sourceFormat );
                if ( source.Width != extents[level].first || source.Height != extents[level].second ||
                     source.ByteSize != expect )
                {
                    return Common::MakeFormattedError<std::vector<unsigned char>>(
                         "level {} layer {} says it is {}x{} of {} bytes and the chain of a {}x{} image makes "
                         "it {}x{} of {}.",
                         level, layer, source.Width, source.Height, source.ByteSize, width, height,
                         extents[level].first, extents[level].second, expect );
                }
                if ( source.ByteOffset + source.ByteSize > sourcePixels.size() )
                {
                    return Common::MakeFormattedError<std::vector<unsigned char>>(
                         "level {} layer {} runs to byte {} of a {}-byte chain.", level, layer,
                         source.ByteOffset + source.ByteSize, sourcePixels.size() );
                }

                auto encoded = Core::Formats::BlockCompressImage(
                     source.Width, source.Height, sourceFormat, blockFormat,
                     sourcePixels.data() + source.ByteOffset, static_cast<std::size_t>( source.ByteSize ) );
                if ( !encoded.IsSuccess() )
                    return Common::MakeError<std::vector<unsigned char>>( encoded.GetError() );

                out.insert( out.end(), encoded.GetValue().begin(), encoded.GetValue().end() );
            }
        }

        // The sum is checked against the SAME function `BuildLevelTable` will check it against, here,
        // where both numbers are still in hand. A block chain that came out a different length is the
        // one mistake this whole step can make quietly.
        const uint64_t expected = TightlyPackedChainBytes( width, height, levelCount, layerCount, blockFormat );
        if ( out.size() != expected )
        {
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "encoding {} levels x {} layers of a {}x{} image produced {} bytes and the block chain for "
                 "that shape is {}.",
                 levelCount, layerCount, width, height, out.size(), expected );
        }

        return Common::MakeSuccess( std::move( out ) );
    }

    uint64_t TightlyPackedChainBytes( const uint32_t width, const uint32_t height, const uint32_t levelCount,
                                      const uint32_t layerCount, const Core::Formats::ImageFormat format )
    {
        // BLOCKS, NOT PIXELS. `CalculateImageSize` rounds a level up to whole 4x4 blocks for a BC
        // format and is `w * h * bytes-per-pixel` for every other, so this sum is the same number it
        // has always been for an uncompressed chain and the RIGHT number for a compressed one. The
        // levels under four texels are where the two differ — and those are every texture's resident
        // tail, so getting them wrong would be wrong in the part that is always loaded.
        uint64_t bytes = 0;
        for ( const auto& [w, h] : LevelExtents( width, height, levelCount ) )
            bytes += Core::Formats::CalculateLayeredImageSize( w, h, layerCount, format );
        return bytes;
    }

    Common::ResultStr<std::vector<TextureLevel>>
    BuildLevelTable( const uint32_t width, const uint32_t height, const uint32_t levelCount,
                     const uint32_t layerCount, const Core::Formats::ImageFormat format,
                     const std::vector<unsigned char>& images, std::vector<unsigned char>& chainOut )
    {
        if ( width == 0 || height == 0 || levelCount == 0 || layerCount == 0 )
        {
            return Common::MakeFormattedError<std::vector<TextureLevel>>(
                 "a level table was asked for a {}x{} image with {} levels and {} layers; every one of "
                 "those has to be at least one.",
                 width, height, levelCount, layerCount );
        }

        const uint32_t maxLevels = Core::Formats::MipChainLength( std::max( width, height ) );
        if ( levelCount > maxLevels )
        {
            return Common::MakeFormattedError<std::vector<TextureLevel>>(
                 "a {}x{} image has a chain of at most {} levels and {} were asked for.", width, height, maxLevels,
                 levelCount );
        }

        const uint64_t expected = TightlyPackedChainBytes( width, height, levelCount, layerCount, format );
        if ( images.size() != expected )
        {
            // THE REFUSAL QUOTES THE BLOCK, NOT A BYTES-PER-PIXEL. Asking `GetBytesPerPixel` here
            // would abort on a BC format — it refuses by design — so the message that explains a size
            // disagreement must not be the thing that crashes on the formats the disagreement is
            // likeliest for.
            const Core::Formats::TexelBlock block = Core::Formats::GetTexelBlock( format );
            return Common::MakeFormattedError<std::vector<TextureLevel>>(
                 "{} levels x {} layers of a {}x{} image at {} bytes per {}x{} block is {} bytes tightly "
                 "packed and {} were handed in.",
                 levelCount, layerCount, width, height, block.Bytes, block.Width, block.Height, expected,
                 images.size() );
        }

        const auto extents = LevelExtents( width, height, levelCount );

        // Where each (level, layer) starts in the TIGHTLY PACKED input, which is table order.
        std::vector<uint64_t> sourceOffset( static_cast<size_t>( levelCount ) * layerCount );
        {
            uint64_t at = 0;
            for ( uint32_t level = 0; level < levelCount; ++level )
            {
                const uint64_t one =
                     Core::Formats::CalculateImageSize( extents[level].first, extents[level].second, format );
                for ( uint32_t layer = 0; layer < layerCount; ++layer )
                {
                    sourceOffset[TextureLevelIndex( level, layer, layerCount )] = at;
                    at += one;
                }
            }
        }

        std::vector<TextureLevel> levels( static_cast<size_t>( levelCount ) * layerCount );
        chainOut.clear();

        // SMALLEST LEVEL FIRST, LAYERS TOGETHER INSIDE IT — the file's physical order, and the same
        // order `EncodeTextureBinary` writes the payload in. The two are separate loops over the same
        // rule, so the suite asserts the RELATION between them rather than trusting either.
        for ( uint32_t i = 0; i < levelCount; ++i )
        {
            const uint32_t level = levelCount - 1 - i;
            for ( uint32_t layer = 0; layer < layerCount; ++layer )
            {
                while ( ( chainOut.size() % kTextureLevelAlignment ) != 0 )
                    chainOut.push_back( 0 );

                const size_t   row = TextureLevelIndex( level, layer, layerCount );
                const uint64_t one =
                     Core::Formats::CalculateImageSize( extents[level].first, extents[level].second, format );

                levels[row].Width      = extents[level].first;
                levels[row].Height     = extents[level].second;
                levels[row].ByteOffset = chainOut.size();
                levels[row].ByteSize   = one;
                levels[row].RowPitch   = Core::Formats::CalculateRowPitch( extents[level].first, format );

                const unsigned char* src = images.data() + sourceOffset[row];
                chainOut.insert( chainOut.end(), src, src + one );
            }
        }

        return Common::MakeSuccess( std::move( levels ) );
    }

    std::string EncodeTextureBinary( const TextureAssetData& data, const TextureEncodeOptions& options )
    {
        // THE WRITER REFUSES A SHAPE IT COULD NOT READ BACK. It has no Result to fail into — a cook that
        // produced bytes is what every caller wants — so the refusal is the ONLY other thing a reader
        // treats as "not a container": nothing. An empty string carries no magic and the loader's first
        // sentence names it as such, instead of a file whose header says six layers and whose table has
        // one row.
        if ( !LayoutRefusal( data.Width, data.Height, data.LayerCount, data.Kind ).empty() ||
             data.Levels.empty() || ( data.Levels.size() % data.LayerCount ) != 0 )
        {
            LOG_ERROR( "EncodeTextureBinary refused a {}x{} texture of kind {} with {} layers and {} level "
                       "rows: the shape does not describe a container this version can write.",
                       data.Width, data.Height, static_cast<uint32_t>( data.Kind ), data.LayerCount,
                       data.Levels.size() );
            return {};
        }

        const uint32_t layerCount = data.LayerCount;
        const uint32_t rowCount   = static_cast<uint32_t>( data.Levels.size() );
        const uint32_t levelCount = data.LevelCount();

        const uint64_t keyOffset = sizeof( FileHeader ) + static_cast<uint64_t>( rowCount ) * sizeof( LevelRow );
        const uint64_t payloadOffset = AlignUp( keyOffset + data.SourcePath.size() );

        uint64_t payloadBytes = 0;
        for ( const TextureLevel& level : data.Levels )
            payloadBytes += level.ByteSize;

        // ── ONE LEVEL AT A TIME, AND THE LEVEL'S OWN BYTES DECIDE ────────────────────────────────
        //
        // Each level is offered to the codec on its own and the result is kept only where it clears the
        // archive's threshold. That is the same rule `PakWriter::WriteBlob` applies to an entry, taken
        // from the same two constants, and it has to be the same rule: a level that does not repay its
        // decompression is a level that should be `memcpy`ed, and the break-even that decides it is a
        // property of the device, not of what the bytes happen to be a picture of.
        //
        // In practice the tail of the chain always stores — a 1x1 level is four bytes and LZ4 cannot
        // shrink them — which is precisely why the codec is a per-LEVEL column. A file-wide flag would
        // have to claim something false about one end of the chain.
        std::vector<std::string> encoded( rowCount ); // physical bytes per row, empty = use the pixels
        std::vector<LevelRow>    table( rowCount );

        // PHYSICAL ORDER IS SMALLEST FIRST, and the offsets have to be laid out in the order the bytes
        // are written, not in level order. With compression the two stopped being derivable from each
        // other: a level's place now depends on how well every SMALLER level compressed.
        uint64_t cursor = payloadOffset;
        // The file ends where the LAST-STORED level ends, which is never the aligned cursor: padding
        // exists to start the NEXT level, and there is no next level after level 0. A file that ended
        // on the pad would carry bytes nobody reads, and the decoder refuses exactly that.
        uint64_t fileEnd     = payloadOffset;
        uint64_t storedBytes = 0;
        for ( uint32_t i = 0; i < levelCount; ++i )
        {
            const uint32_t level = levelCount - 1 - i;
            // LAYERS TOGETHER, INSIDE THE LEVEL. That is what keeps the first bullet of the header note
            // true for a cube: the resident tail is the smallest levels OF EVERY FACE, and it is still
            // one contiguous read because the six faces of a level sit next to each other.
            for ( uint32_t layer = 0; layer < layerCount; ++layer )
            {
                const size_t         row    = TextureLevelIndex( level, layer, layerCount );
                const TextureLevel&  source = data.Levels[row];
                const unsigned char* bytes  = data.Pixels.data() + source.ByteOffset;

                uint64_t          storedSize = source.ByteSize;
                TextureLevelCodec codec      = TextureLevelCodec::Store;
                if ( options.CompressLevels && source.ByteSize > 0 )
                {
                    std::string  packed( Common::Utils::Lz4BlockBound( static_cast<size_t>( source.ByteSize ) ),
                                         '\0' );
                    const size_t produced = Common::Utils::Lz4BlockCompress(
                         bytes, static_cast<size_t>( source.ByteSize ), packed.data(), packed.size() );
                    if ( produced > 0 &&
                         static_cast<uint64_t>( produced ) * Common::Utils::kCompressionDenominator <=
                              source.ByteSize * Common::Utils::kCompressionNumerator )
                    {
                        packed.resize( produced );
                        encoded[row] = std::move( packed );
                        storedSize   = produced;
                        codec        = TextureLevelCodec::LZ4;
                    }
                }

                table[row] = LevelRow{ cursor, static_cast<uint32_t>( source.ByteSize ), source.RowPitch,
                                       static_cast<uint32_t>( storedSize ), static_cast<uint32_t>( codec ) };
                storedBytes += storedSize;
                fileEnd = cursor + storedSize;
                cursor  = AlignUp( fileEnd );
            }
        }

        FileHeader header{};
        std::memcpy( header.Magic, kTextureBinaryMagic, sizeof( header.Magic ) );
        header.ByteOrder         = kByteOrderTag;
        header.Version           = kTextureBinaryVersion;
        header.HeaderSize        = static_cast<uint32_t>( sizeof( FileHeader ) );
        header.Width             = data.Width;
        header.Height            = data.Height;
        header.Format            = static_cast<uint32_t>( data.Format );
        header.LevelCount        = levelCount;
        header.LayerCount         = layerCount;
        header.Kind               = static_cast<uint32_t>( data.Kind );
        header.Intent             = static_cast<uint32_t>( data.Intent );
        header.Flags             = 0;
        header.SourceKeyLength   = static_cast<uint32_t>( data.SourcePath.size() );
        header.SourceKeyOffset   = static_cast<uint32_t>( keyOffset );
        header.SourceContentHash = data.SourceContentHash;
        header.EncoderHash        = data.EncoderHash;
        header.PayloadBytes       = payloadBytes;
        header.StoredPayloadBytes = storedBytes;
        header.FileSize           = fileEnd;
        header.Handle             = static_cast<uint64_t>( data.Handle );

        std::string out;
        out.reserve( static_cast<size_t>( header.FileSize ) );
        Append( out, &header, sizeof( header ) );
        Append( out, table.data(), table.size() * sizeof( LevelRow ) );
        Append( out, data.SourcePath.data(), data.SourcePath.size() );
        PadToLevelBoundary( out );

        // SMALLEST FIRST, the same order the offsets were laid out in — and written from the SAME
        // `table` the header carries, so the bytes and the rows cannot describe different files.
        for ( uint32_t i = 0; i < levelCount; ++i )
        {
            const uint32_t level = levelCount - 1 - i;
            for ( uint32_t layer = 0; layer < layerCount; ++layer )
            {
                const size_t row = TextureLevelIndex( level, layer, layerCount );
                PadToLevelBoundary( out );
                if ( encoded[row].empty() )
                    Append( out, data.Pixels.data() + data.Levels[row].ByteOffset, data.Levels[row].ByteSize );
                else
                    Append( out, encoded[row].data(), encoded[row].size() );
            }
        }
        return out;
    }

    namespace
    {
        /// The checks both decoders share: everything answerable from the header and the table without
        /// the payload being present. @p bytesInHand is what the caller actually holds, which is the
        /// whole file for one decoder and a prefix for the other — so the truncation check lives with
        /// the caller that can make it, not here.
        Common::ResultStr<TextureBinaryHeaderInfo> ReadHeaderAndTable( const std::string_view bytes,
                                                                       const std::string_view whatFor,
                                                                       std::vector<LevelRow>& tableOut,
                                                                       uint64_t&              payloadOffsetOut )
        {
            const std::string who( whatFor );

            if ( bytes.size() < sizeof( FileHeader ) )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' is {} bytes, which is shorter than the {}-byte cooked-texture header — the file "
                     "is truncated or is not a cooked texture at all.",
                     who, bytes.size(), sizeof( FileHeader ) );
            }

            FileHeader header{};
            std::memcpy( &header, bytes.data(), sizeof( header ) );

            if ( std::memcmp( header.Magic, kTextureBinaryMagic, sizeof( header.Magic ) ) != 0 )
                return Common::MakeError<TextureBinaryHeaderInfo>( StaleCookRefusal( whatFor ) );

            if ( header.ByteOrder != kByteOrderTag )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' was written by a host of the opposite byte order (tag {:#010x}, this host reads "
                     "{:#010x}). Cooked textures are little-endian; re-cook it on the target host.",
                     who, header.ByteOrder, kByteOrderTag );
            }

            if ( header.Version != kTextureBinaryVersion )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' is cooked-texture format version {}, this build reads version {}. Re-cook it "
                     "(Assets > Rebuild Cooked Assets).",
                     who, header.Version, kTextureBinaryVersion );
            }

            if ( header.HeaderSize < sizeof( FileHeader ) )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares a {}-byte header and version {} has {}. A header cannot be smaller "
                     "than the fields this version reads out of it.",
                     who, header.HeaderSize, kTextureBinaryVersion, sizeof( FileHeader ) );
            }

            // FORWARD-COMPATIBILITY GUARDS, NOT DEAD FIELDS. Version 1 defines no flag and has no
            // encoder, so both of these are written as zero. A file that sets either was produced by a
            // build that knows something this one does not — an sRGB marking it would have to honour,
            // an encoder whose output it cannot interpret — and the only safe answer is to say so.
            // Ignoring them would be the silent wrong answer: the texture would decode and draw, in the
            // wrong colour space or from the wrong bytes.
            if ( header.Flags != 0 )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' sets content flags {:#010x}; version {} defines none and cannot honour them. "
                     "Re-cook it with a build that does.",
                     who, header.Flags, kTextureBinaryVersion );
            }
            // `EncoderHash` IS NO LONGER REFUSED, AND THAT IS A v3 DECISION WITH AN ARGUMENT. v1 and v2
            // refused a non-zero value because they could not say what it meant; the sentence they
            // printed said "no encoder to compare against". v3 gives it its meaning: it is the
            // SETTINGS THAT PRODUCED THESE PIXELS — not how they are packed, which is the per-level
            // `Codec` column and is still checked by name.
            //
            // WHY IGNORING IT IS SAFE HERE AND CHECKING IT IS SOMEBODY ELSE'S JOB. The pixels of a
            // cooked texture are the pixels: a reader with no expectation about how they were made can
            // draw them and be right. A reader that DOES have one — the environment loader, which will
            // not accept a cube baked from a different sky look — is handed the number and refuses the
            // mismatch itself, because only it knows what it asked for. A container that tried to make
            // that comparison would need to carry the question as well as the answer.
            //
            // `TextureImporter` writes 0, so an ordinary `.tex` still says "no settings" and the
            // suite pins that it does.

            if ( header.Format >= Core::Formats::kImageFormatCount )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' names pixel format {}, and this build knows {} formats. The file was written by "
                     "a newer cook; re-cook it.",
                     who, header.Format, Core::Formats::kImageFormatCount );
            }

            // ZERO LEVELS IS ITS OWN FAILURE, NOT A SMALL TEXTURE. A container with an empty chain
            // decodes into an image with no pixels, and every consumer downstream would then be asked
            // to sample it. It is refused here so the sentence names the cook rather than the sampler.
            if ( header.LevelCount == 0 )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares zero mip levels: the cook wrote a container with no image in it.", who );
            }

            // THE SHAPE BEFORE THE SIZES. `LayerCount` multiplies the table's length and every byte
            // total below it, so a nonsense pair has to be named here rather than surviving into an
            // arithmetic that would merely produce a wrong — and plausible — number of rows.
            // AN INTENT THIS BUILD DOES NOT KNOW IS REFUSED BY NAME, for the reason `Format` is: the
            // word is a small integer, so a file written by a later cook carries a value that reads
            // back as a perfectly good enumerator of the wrong meaning. Zero is `Unspecified` and is
            // what every file written before this field existed already holds, so nothing old is
            // caught here -- only something NEWER than this build, which is precisely what a reader
            // cannot honour.
            if ( !Core::Formats::IsKnownTextureIntent( header.Intent ) )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' was cooked for texture intent {}, and this build knows {}. The file was "
                     "written by a newer cook; re-cook it.",
                     who, header.Intent, Core::Formats::kTextureIntentCount );
            }

            const TextureKind kind = static_cast<TextureKind>( header.Kind );
            if ( const std::string bad = LayoutRefusal( header.Width, header.Height, header.LayerCount, kind );
                 !bad.empty() )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' does not describe a shape this version can store: {}. Re-cook it.", who, bad );
            }

            const uint64_t rowCount =
                 static_cast<uint64_t>( header.LevelCount ) * static_cast<uint64_t>( header.LayerCount );
            const uint64_t tableEnd = static_cast<uint64_t>( header.HeaderSize ) + rowCount * sizeof( LevelRow );
            if ( tableEnd > bytes.size() )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares {} mip levels of {} layers, whose table needs {} bytes, and only {} are "
                     "present.",
                     who, header.LevelCount, header.LayerCount, tableEnd, bytes.size() );
            }

            if ( header.SourceKeyOffset != tableEnd ||
                 static_cast<uint64_t>( header.SourceKeyOffset ) + header.SourceKeyLength > bytes.size() )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' puts its source key at {} for {} bytes; version {} puts it at {} and the file "
                     "holds {}. The header does not describe this file.",
                     who, header.SourceKeyOffset, header.SourceKeyLength, kTextureBinaryVersion, tableEnd,
                     bytes.size() );
            }

            const uint64_t payloadOffset =
                 AlignUp( static_cast<uint64_t>( header.SourceKeyOffset ) + header.SourceKeyLength );

            tableOut.resize( static_cast<size_t>( rowCount ) );
            std::memcpy( tableOut.data(), bytes.data() + header.HeaderSize, tableOut.size() * sizeof( LevelRow ) );

            // THE LAYOUT IS DERIVED, NOT TRUSTED — the lesson `MeshBinary.cpp` paid for. Checking only
            // that a level lies inside the file is not enough: a corrupted offset that still fits reads
            // back a complete, plausible, WRONG image. Every level's extent follows from the header by
            // halving, its size from the extent and the format, and its offset from the level PHYSICALLY
            // before it — which, since the levels are stored smallest first, is the level one NUMBER
            // higher. A row that disagrees is refused rather than followed.
            const Core::Formats::ImageFormat format = static_cast<Core::Formats::ImageFormat>( header.Format );
            // THE BLOCK, NOT A BYTES-PER-PIXEL. `GetBytesPerPixel` refuses a BC format by design, so a
            // reader that asked it here would ABORT on every block-compressed file instead of decoding
            // it — and this line is reached before anything about the file has been believed.
            const Core::Formats::TexelBlock block = Core::Formats::GetTexelBlock( format );

            // ── HOW MANY LEVELS A FILE OWES, AND WHY THE ANSWER DEPENDS ON THE KIND ──────────────
            //
            // A 2D TEXTURE OWES ITS WHOLE CHAIN. That is what this container was built for: the resident
            // tail is the levels at 16x16 and below, and a file that stops at level 3 has no tail to
            // read — "an object on screen with no texture" becomes possible again, quietly, for one
            // texture. A short chain there is a cook that stopped early and is named as such.
            //
            // A CUBE OWES THE LEVELS IT WAS BAKED WITH, and demanding a full chain of one is simply
            // wrong. `kSkyEnvRadianceMips` is 1 on a 1024 face and that single level is a MEASURED
            // refusal of the other ten (`SkyRules.hpp`: 96 -> 128 MiB per live SceneRenderer for a
            // change of at most 1/255); the prefiltered cube's nine levels are a GGX roughness ramp,
            // not a minification chain, and its count is `MipChainLength(256)` by arithmetic rather
            // than by obligation. The bound that survives for both is the one the DEVICE imposes:
            // more levels than the extent supports is an invalid `vkCreateImage`
            // (VUID-VkImageCreateInfo-mipLevels-00958).
            //
            // FOUND BY RUNNING IT. The first baked environment this container ever wrote was refused on
            // its own second load, with "the cook is partial" — against a cube whose level count the
            // engine had deliberately chosen. Both ends were right and the rule in the middle was a 2D
            // rule applied to everything.
            const uint32_t maxLevels = Core::Formats::MipChainLength( std::max( header.Width, header.Height ) );
            if ( header.LevelCount > maxLevels )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' carries {} mip levels and a {}x{} image supports at most {}.", who, header.LevelCount,
                     header.Width, header.Height, maxLevels );
            }
            if ( kind == TextureKind::Texture2D && header.LevelCount != maxLevels )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' carries {} mip levels and a {}x{} texture has a chain of {}. The cook is partial.", who,
                     header.LevelCount, header.Width, header.Height, maxLevels );
            }

            const std::vector<std::pair<uint32_t, uint32_t>> extents =
                 LevelExtents( header.Width, header.Height, header.LevelCount );

            uint64_t expectedOffset = payloadOffset;
            uint64_t declaredBytes  = 0;
            uint64_t storedBytes    = 0;
            for ( uint32_t i = 0; i < header.LevelCount; ++i )
            {
                const uint32_t level = header.LevelCount - 1 - i; // physical order: smallest first
                for ( uint32_t layer = 0; layer < header.LayerCount; ++layer )
                {
                    const LevelRow& row = tableOut[TextureLevelIndex( level, layer, header.LayerCount )];

                    // THE CODEC IS CHECKED BEFORE ANYTHING IS BELIEVED ABOUT THE SIZES, because what the
                    // two size columns mean to each other depends on it. An enumerator this build does not
                    // know is a file from a newer cook, and it is named rather than treated as Store —
                    // which would hand the sampler a buffer of compressed bytes that draws.
                    if ( row.Codec > static_cast<uint32_t>( TextureLevelCodec::LZ4 ) )
                    {
                        return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                             "'{}' stores level {} layer {} with codec {}, and version {} knows {}. The file "
                             "was written by a newer cook; re-cook it.",
                             who, level, layer, row.Codec, kTextureBinaryVersion,
                             static_cast<uint32_t>( TextureLevelCodec::LZ4 ) + 1 );
                    }
                    if ( row.StoredSize == 0 )
                    {
                        return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                             "'{}' level {} layer {} occupies zero bytes of the file; a level that was "
                             "cooked has bytes.",
                             who, level, layer );
                    }
                    // A STORED LEVEL'S TWO SIZES ARE THE SAME NUMBER, and saying so is what stops the pair
                    // from drifting into "stored, but shorter than its pixels" — which reads back as a
                    // complete image made partly of whatever followed it.
                    if ( row.Codec == static_cast<uint32_t>( TextureLevelCodec::Store ) &&
                         row.StoredSize != row.ByteSize )
                    {
                        return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                             "'{}' level {} layer {} is stored uncompressed and claims {} bytes in the file "
                             "against {} bytes of pixels. An uncompressed level is its pixels.",
                             who, level, layer, row.StoredSize, row.ByteSize );
                    }

                    // DERIVED IN BLOCKS. For an uncompressed format these two are `w * h * bpp` and
                    // `w * bpp` exactly as they were; for a BC format they round the level up to whole
                    // 4x4 blocks, which is the ONLY place in this decoder where the two differ and the
                    // place a file written by a block-unaware cook stops verifying.
                    const uint64_t expectSize =
                         Core::Formats::CalculateImageSize( extents[level].first, extents[level].second, format );
                    if ( row.ByteSize != expectSize )
                    {
                        return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                             "'{}' level {} layer {} ({}x{}) claims {} bytes; {} bytes per {}x{} block makes "
                             "it {}.",
                             who, level, layer, extents[level].first, extents[level].second, row.ByteSize,
                             block.Bytes, block.Width, block.Height, expectSize );
                    }
                    const uint32_t expectPitch = Core::Formats::CalculateRowPitch( extents[level].first, format );
                    if ( row.RowPitch != expectPitch )
                    {
                        return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                             "'{}' level {} layer {} declares a row pitch of {}; a {}-texel row is {} block(s) "
                             "of {} bytes, so {}.",
                             who, level, layer, row.RowPitch, extents[level].first,
                             Core::Formats::BlocksAcross( extents[level].first, format ), block.Bytes,
                             expectPitch );
                    }
                    if ( row.ByteOffset != expectedOffset )
                    {
                        return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                             "'{}' level {} layer {} starts at {}, and the levels stored before it put it at "
                             "{}. The level table does not describe this file.",
                             who, level, layer, row.ByteOffset, expectedOffset );
                    }

                    declaredBytes += row.ByteSize;
                    storedBytes += row.StoredSize;
                    // THE CHAIN IS OVER THE STORED SIZES, and that is the one line where per-level
                    // compression could have been dropped in the middle. Where the next level begins is a
                    // fact about the FILE, so it follows from how many bytes the previous level occupies
                    // there — not from how many it decodes to. Chaining on `ByteSize` would validate a
                    // compressed file by accident only when nothing compressed.
                    expectedOffset = AlignUp( row.ByteOffset + row.StoredSize );
                }
            }

            // The declared payload total is a second, independent statement of the same quantity, and
            // §5c asks for it precisely so that the sum and the table have to agree with each other.
            // v2 makes that two quantities, because the file now has two: what it holds and what it
            // decodes to. Both are declared and both are checked.
            if ( header.PayloadBytes != declaredBytes )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares {} bytes of pixels and its level table adds up to {}.", who,
                     header.PayloadBytes, declaredBytes );
            }
            if ( header.StoredPayloadBytes != storedBytes )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares {} stored bytes and its level table adds up to {}.", who,
                     header.StoredPayloadBytes, storedBytes );
            }

            TextureBinaryHeaderInfo info;
            info.Handle = Common::UUID( header.Handle );
            info.SourcePath.assign( bytes.data() + header.SourceKeyOffset, header.SourceKeyLength );
            info.SourceContentHash = header.SourceContentHash;
            info.EncoderHash        = header.EncoderHash;
            info.Width             = header.Width;
            info.Height            = header.Height;
            info.Format            = format;
            info.LevelCount         = header.LevelCount;
            info.LayerCount         = header.LayerCount;
            info.Kind               = kind;
            info.Intent             = static_cast<Core::Formats::TextureIntent>( header.Intent );
            info.PayloadBytes       = header.PayloadBytes;
            info.StoredPayloadBytes = header.StoredPayloadBytes;
            info.FileSize           = header.FileSize;

            // THE TABLE TRAVELS WITH THE HEADER, which is what makes a streaming read possible from the
            // prefix alone. It is filled from the rows that were just validated, not re-read, so no
            // caller can be handed a row the checks above did not pass.
            info.Levels.reserve( header.LevelCount );
            for ( const LevelRow& row : tableOut )
            {
                info.Levels.push_back( TextureLevelLocation{ row.ByteOffset, row.StoredSize, row.ByteSize,
                                                             static_cast<TextureLevelCodec>( row.Codec ) } );
            }

            payloadOffsetOut = payloadOffset;
            return Common::MakeSuccess( std::move( info ) );
        }
    } // namespace

    Common::ResultStr<TextureBinaryHeaderInfo> DecodeTextureHeader( const std::string_view bytes,
                                                                    const std::string_view whatFor )
    {
        std::vector<LevelRow> table;
        uint64_t              payloadOffset = 0;
        return ReadHeaderAndTable( bytes, whatFor, table, payloadOffset );
    }

    Common::ResultStr<TextureAssetData> DecodeTextureBinary( const std::string_view bytes,
                                                             const std::string_view whatFor )
    {
        const std::string     who( whatFor );
        std::vector<LevelRow> table;
        uint64_t              payloadOffset = 0;

        auto info = ReadHeaderAndTable( bytes, whatFor, table, payloadOffset );
        if ( !info.IsSuccess() )
            return Common::MakeError<TextureAssetData>( info.GetError() );

        // THE TRUNCATION CHECK, AND THE REASON `FileSize` IS IN THE HEADER AT ALL. It is made here and
        // not in the shared helper because the header decoder is handed a PREFIX on purpose, and a
        // prefix is legally short. A file that stops early otherwise reads back as a valid texture
        // whose last levels are whatever the allocator left behind.
        if ( info.GetValue().FileSize != bytes.size() )
        {
            return Common::MakeFormattedError<TextureAssetData>(
                 "'{}' declares {} bytes and {} are present — the file is truncated or has been appended "
                 "to. Nothing was loaded.",
                 who, info.GetValue().FileSize, bytes.size() );
        }

        // The payload run ends where the LAST-STORED level ends — which, with the levels reversed, is
        // level 0. Padding between levels is inside the run, so the run's length is not the sum of the
        // sizes; the sum is checked separately, against `PayloadBytes`, in the shared reader.
        //
        // IT IS THE STORED SIZE THAT ENDS THE FILE. Level 0 of a compressed texture occupies fewer
        // bytes than it decodes to, and a file measured by the decoded size would be declared short by
        // exactly the amount the codec saved — a truncation refusal fired at every healthy file.
        // THE LAST ROW WRITTEN, WHICH IS NOT ROW 0 ANY MORE. Physical order is smallest level first with
        // the layers together inside a level, so the bytes that end the file belong to level 0's LAST
        // layer. For a 2D texture that is still row 0 and this line reads exactly as it did; for a cube
        // it is row 5, and taking `front()` here would declare every healthy cube short by five faces.
        const LevelRow& lastWritten =
             table[TextureLevelIndex( 0, info.GetValue().LayerCount - 1, info.GetValue().LayerCount )];
        const uint64_t payloadEnd = lastWritten.ByteOffset + lastWritten.StoredSize;
        if ( payloadEnd != info.GetValue().FileSize )
        {
            return Common::MakeFormattedError<TextureAssetData>(
                 "'{}' ends its last stored level at {} and declares a file of {} bytes. Trailing or "
                 "missing bytes nobody reads are a file this cook could not have produced.",
                 who, payloadEnd, info.GetValue().FileSize );
        }

        TextureAssetData data;
        data.Handle            = info.GetValue().Handle;
        data.SourcePath        = info.GetValue().SourcePath;
        data.SourceContentHash = info.GetValue().SourceContentHash;
        data.EncoderHash       = info.GetValue().EncoderHash;
        data.Width             = info.GetValue().Width;
        data.Height            = info.GetValue().Height;
        data.Format            = info.GetValue().Format;

        // ── THE TWO LAYOUTS, AND THE CONVERSION BETWEEN THEM ─────────────────────────────────────
        //
        // THE ONE CONVERSION IN THIS FORMAT, made exactly once. The file's rows are FILE-relative; the
        // GPU needs offsets inside the staging buffer, which holds the DECODED payload and nothing
        // else. Before v2 that conversion was a subtraction, because the two layouts were the same run
        // of bytes at two addresses. They are not the same run any more: the file's spacing follows the
        // STORED sizes and the buffer's follows the DECODED ones, so the decoded layout is BUILT here
        // by the same rule the encoder builds the chain with (pad to the boundary, then the level).
        //
        // Both layouts are laid out in PHYSICAL order — smallest level first — because that is the
        // order the padding accumulates in. Walking the table in level order would place level 0 at
        // offset 0 and every other level after it, which is neither layout.
        const uint32_t levelCount = info.GetValue().LevelCount;
        const uint32_t layerCount = info.GetValue().LayerCount;
        data.LayerCount           = layerCount;
        data.Kind                 = info.GetValue().Kind;
        data.Intent               = info.GetValue().Intent;

        const auto extents = LevelExtents( info.GetValue().Width, info.GetValue().Height, levelCount );
        data.Levels.resize( table.size() );
        for ( uint32_t level = 0; level < levelCount; ++level )
        {
            for ( uint32_t layer = 0; layer < layerCount; ++layer )
            {
                const size_t row          = TextureLevelIndex( level, layer, layerCount );
                data.Levels[row].Width    = extents[level].first;
                data.Levels[row].Height   = extents[level].second;
                data.Levels[row].ByteSize = table[row].ByteSize;
                data.Levels[row].RowPitch = table[row].RowPitch;
            }
        }

        uint64_t decodedRun = 0;
        for ( uint32_t i = 0; i < levelCount; ++i )
        {
            const uint32_t level = levelCount - 1 - i;
            for ( uint32_t layer = 0; layer < layerCount; ++layer )
            {
                const size_t row            = TextureLevelIndex( level, layer, layerCount );
                decodedRun                  = AlignUp( decodedRun );
                data.Levels[row].ByteOffset = decodedRun;
                decodedRun += table[row].ByteSize;
            }
        }

        data.Pixels.resize( static_cast<size_t>( decodedRun ) );
        for ( size_t index = 0; index < table.size(); ++index )
        {
            const LevelRow& row = table[index];
            const char*     src = bytes.data() + row.ByteOffset;
            unsigned char*  dst = data.Pixels.data() + data.Levels[index].ByteOffset;

            if ( static_cast<TextureLevelCodec>( row.Codec ) == TextureLevelCodec::Store )
            {
                std::memcpy( dst, src, row.ByteSize );
                continue;
            }

            // A BLOCK THAT DOES NOT PRODUCE EXACTLY THE DECLARED BYTES IS A FAILED READ, never a
            // shorter level. `Lz4BlockDecompress` bounds-checks every read and every write and refuses
            // a short or long result, so what reaches here is either the level or a refusal naming it.
            if ( !Common::Utils::Lz4BlockDecompress( src, row.StoredSize, dst, row.ByteSize ) )
            {
                return Common::MakeFormattedError<TextureAssetData>(
                     "'{}' level {} layer {} is {} compressed bytes at {} and did not decode to the {} "
                     "bytes it declares. Nothing was loaded.",
                     who, static_cast<uint32_t>( index / layerCount ), static_cast<uint32_t>( index % layerCount ),
                     row.StoredSize, row.ByteOffset, row.ByteSize );
            }
        }

        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets::Serialization
