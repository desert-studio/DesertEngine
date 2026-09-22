#include "TextureBinary.hpp"

#include <Common/Core/Logger.hpp>

#include <fmt/format.h>

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
            uint64_t FileSize; // declared; compared against the bytes actually in hand
            uint64_t Handle;
            uint32_t Width;
            uint32_t Height;
            uint32_t Format;     // an ImageFormat enumerator, travelling at a width the file fixes
            uint32_t LevelCount;
            uint32_t SourceKeyOffset; // from file start
            uint32_t SourceKeyLength;
            uint32_t SourceContentHash;
            uint32_t Reserved;
        };
        static_assert( sizeof( FileHeader ) == kTextureBinaryHeaderSize );
        static_assert( alignof( FileHeader ) == 8 );
        static_assert( offsetof( FileHeader, FileSize ) == 16 );
        static_assert( offsetof( FileHeader, Handle ) == 24 );

        struct LevelRow
        {
            uint32_t Width;
            uint32_t Height;
            uint64_t ByteOffset; // from the START OF THE PAYLOAD RUN
            uint64_t ByteSize;
        };
        static_assert( sizeof( LevelRow ) == 24 );
        static_assert( alignof( LevelRow ) == 8 );

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

        /// Pad @p out to the next 8-byte boundary, so the payload run starts aligned for any texel
        /// size a future version stores. Nothing in this file depends on it — every read goes through
        /// `memcpy` — but a reader that maps the file instead of copying it would.
        void PadToEight( std::string& out )
        {
            while ( ( out.size() % 8 ) != 0 )
                out.push_back( '\0' );
        }

        /// The next extent down the chain. `max(1, n/2)` is the rule Vulkan's mip chain is defined by,
        /// and `MipChainLength` in ImageFormat.hpp counts the levels this produces.
        constexpr uint32_t HalfExtent( const uint32_t n )
        {
            return n > 1u ? n / 2u : 1u;
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
                             static_cast<Accumulator>( src[( static_cast<size_t>( y0 ) * srcW + x0 ) * components + c] ) +
                             static_cast<Accumulator>( src[( static_cast<size_t>( y0 ) * srcW + x1 ) * components + c] ) +
                             static_cast<Accumulator>( src[( static_cast<size_t>( y1 ) * srcW + x0 ) * components + c] ) +
                             static_cast<Accumulator>( src[( static_cast<size_t>( y1 ) * srcW + x1 ) * components + c] );
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
                                                                const Core::Formats::ImageFormat format,
                                                                const std::vector<unsigned char>& base,
                                                                std::vector<unsigned char>&      chainOut )
    {
        using Fmt = Core::Formats::ImageFormat;

        if ( width == 0 || height == 0 )
        {
            return Common::MakeFormattedError<std::vector<TextureLevel>>(
                 "a mip chain was asked for a {}x{} image; a texture with a zero extent has no levels.",
                 width, height );
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
                 "the base level of a {}x{} image at {} bytes per pixel is {} bytes and {} were handed in.",
                 width, height, bpp, baseSize, base.size() );
        }

        const uint32_t levelCount =
             Core::Formats::MipChainLength( std::max( width, height ) );

        std::vector<TextureLevel> levels;
        levels.reserve( levelCount );

        chainOut.clear();
        chainOut.reserve( baseSize + baseSize / 2u );
        chainOut.insert( chainOut.end(), base.begin(), base.end() );
        levels.push_back( TextureLevel{ width, height, 0, baseSize } );

        uint32_t srcW = width, srcH = height;
        for ( uint32_t level = 1; level < levelCount; ++level )
        {
            const uint32_t dstW = HalfExtent( srcW );
            const uint32_t dstH = HalfExtent( srcH );
            const size_t   dstSize = static_cast<size_t>( dstW ) * dstH * bpp;

            const size_t srcOffset = levels.back().ByteOffset;
            const size_t dstOffset = chainOut.size();
            chainOut.resize( dstOffset + dstSize );

            // The source is addressed through the vector AFTER the resize, never through a pointer
            // taken before it: `resize` may reallocate, and a pointer into the old buffer is the
            // classic way a mip chain ends up reading freed memory on exactly one allocator.
            if ( format == Fmt::RGBA8F )
            {
                BoxDownsample<unsigned char, uint32_t>(
                     chainOut.data() + srcOffset, srcW, srcH,
                     chainOut.data() + dstOffset, dstW, dstH, 4u );
            }
            else
            {
                BoxDownsample<float, float>( reinterpret_cast<const float*>( chainOut.data() + srcOffset ),
                                             srcW, srcH,
                                             reinterpret_cast<float*>( chainOut.data() + dstOffset ), dstW,
                                             dstH, 4u );
            }

            levels.push_back( TextureLevel{ dstW, dstH, dstOffset, dstSize } );
            srcW = dstW;
            srcH = dstH;
        }

        return Common::MakeSuccess( std::move( levels ) );
    }

    std::string EncodeTextureBinary( const TextureAssetData& data )
    {
        const uint32_t levelCount = static_cast<uint32_t>( data.Levels.size() );

        const uint64_t tableEnd  = sizeof( FileHeader ) + static_cast<uint64_t>( levelCount ) * sizeof( LevelRow );
        const uint64_t keyOffset = tableEnd;
        uint64_t       payloadOffset = keyOffset + data.SourcePath.size();
        payloadOffset = ( payloadOffset + 7u ) & ~static_cast<uint64_t>( 7u );

        std::vector<LevelRow> table;
        table.reserve( levelCount );
        for ( const TextureLevel& level : data.Levels )
            table.push_back( LevelRow{ level.Width, level.Height, level.ByteOffset, level.ByteSize } );

        FileHeader header{};
        std::memcpy( header.Magic, kTextureBinaryMagic, sizeof( header.Magic ) );
        header.ByteOrder         = kByteOrderTag;
        header.Version           = kTextureBinaryVersion;
        header.FileSize          = payloadOffset + data.Pixels.size();
        header.Handle            = static_cast<uint64_t>( data.Handle );
        header.Width             = data.Width;
        header.Height            = data.Height;
        header.Format            = static_cast<uint32_t>( data.Format );
        header.LevelCount        = levelCount;
        header.SourceKeyOffset   = static_cast<uint32_t>( keyOffset );
        header.SourceKeyLength   = static_cast<uint32_t>( data.SourcePath.size() );
        header.SourceContentHash = data.SourceContentHash;
        header.Reserved          = 0;

        std::string out;
        out.reserve( static_cast<size_t>( header.FileSize ) );
        Append( out, &header, sizeof( header ) );
        Append( out, table.data(), table.size() * sizeof( LevelRow ) );
        Append( out, data.SourcePath.data(), data.SourcePath.size() );
        PadToEight( out );
        Append( out, data.Pixels.data(), data.Pixels.size() );
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

            const uint64_t tableEnd =
                 sizeof( FileHeader ) + static_cast<uint64_t>( header.LevelCount ) * sizeof( LevelRow );
            if ( tableEnd > bytes.size() )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares {} mip levels, whose table needs {} bytes, and only {} are present.",
                     who, header.LevelCount, tableEnd, bytes.size() );
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

            uint64_t payloadOffset = static_cast<uint64_t>( header.SourceKeyOffset ) + header.SourceKeyLength;
            payloadOffset          = ( payloadOffset + 7u ) & ~static_cast<uint64_t>( 7u );

            tableOut.resize( header.LevelCount );
            std::memcpy( tableOut.data(), bytes.data() + sizeof( FileHeader ),
                         tableOut.size() * sizeof( LevelRow ) );

            // THE LAYOUT IS DERIVED, NOT TRUSTED — the lesson `MeshBinary.cpp` paid for. Checking only
            // that a level lies inside the file is not enough: a corrupted offset that still fits reads
            // back a complete, plausible, WRONG image. Version 1 packs the levels tightly in table
            // order, each extent halving down from the base, so every row's contents follow from the
            // header alone, and a row that disagrees is refused rather than followed.
            uint64_t       expectedOffset = 0;
            uint32_t       expectW        = header.Width;
            uint32_t       expectH        = header.Height;
            const uint32_t bpp = Core::Formats::GetBytesPerPixel( static_cast<Core::Formats::ImageFormat>( header.Format ) );
            for ( uint32_t i = 0; i < header.LevelCount; ++i )
            {
                const LevelRow& row = tableOut[i];
                if ( row.Width != expectW || row.Height != expectH )
                {
                    return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                         "'{}' level {} is {}x{}, and halving down from the declared {}x{} puts {}x{} there.",
                         who, i, row.Width, row.Height, header.Width, header.Height, expectW, expectH );
                }
                const uint64_t expectSize = static_cast<uint64_t>( expectW ) * expectH * bpp;
                if ( row.ByteSize != expectSize )
                {
                    return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                         "'{}' level {} ({}x{}) claims {} bytes; {} bytes per pixel makes it {}.", who, i,
                         row.Width, row.Height, row.ByteSize, bpp, expectSize );
                }
                if ( row.ByteOffset != expectedOffset )
                {
                    return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                         "'{}' level {} starts at {} inside the payload, and the levels before it put it at "
                         "{}. The level table does not describe this file.",
                         who, i, row.ByteOffset, expectedOffset );
                }
                expectedOffset += expectSize;
                expectW = HalfExtent( expectW );
                expectH = HalfExtent( expectH );
            }

            // And the chain must be COMPLETE: a container that stops at level 3 of a ten-level chain is
            // a partial cook, and the day mip streaming arrives it must be told apart from a file that
            // legitimately ships a residual tail. Version 1 has no tail, so the count is the chain.
            const uint32_t expectLevels =
                 Core::Formats::MipChainLength( std::max( header.Width, header.Height ) );
            if ( header.LevelCount != expectLevels )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' carries {} mip levels and a {}x{} image has a chain of {}. The cook is partial.",
                     who, header.LevelCount, header.Width, header.Height, expectLevels );
            }

            TextureBinaryHeaderInfo info;
            info.Handle            = Common::UUID( header.Handle );
            info.SourcePath.assign( bytes.data() + header.SourceKeyOffset, header.SourceKeyLength );
            info.SourceContentHash = header.SourceContentHash;
            info.Width             = header.Width;
            info.Height            = header.Height;
            info.Format            = static_cast<Core::Formats::ImageFormat>( header.Format );
            info.LevelCount        = header.LevelCount;
            info.FileSize          = header.FileSize;

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

        uint64_t payloadBytes = 0;
        for ( const LevelRow& row : table )
            payloadBytes += row.ByteSize;

        if ( payloadOffset + payloadBytes != info.GetValue().FileSize )
        {
            return Common::MakeFormattedError<TextureAssetData>(
                 "'{}' puts {} bytes of pixels at offset {}, which ends at {}, and the file declares {}. "
                 "Trailing or missing bytes nobody reads are a file this cook could not have produced.",
                 who, payloadBytes, payloadOffset, payloadOffset + payloadBytes, info.GetValue().FileSize );
        }

        TextureAssetData data;
        data.Handle            = info.GetValue().Handle;
        data.SourcePath        = info.GetValue().SourcePath;
        data.SourceContentHash = info.GetValue().SourceContentHash;
        data.Width             = info.GetValue().Width;
        data.Height            = info.GetValue().Height;
        data.Format            = info.GetValue().Format;

        data.Levels.reserve( table.size() );
        for ( const LevelRow& row : table )
            data.Levels.push_back( TextureLevel{ row.Width, row.Height, row.ByteOffset, row.ByteSize } );

        data.Pixels.resize( static_cast<size_t>( payloadBytes ) );
        if ( payloadBytes > 0 )
            std::memcpy( data.Pixels.data(), bytes.data() + payloadOffset, static_cast<size_t>( payloadBytes ) );

        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets::Serialization
