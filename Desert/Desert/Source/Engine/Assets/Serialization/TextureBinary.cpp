#include "TextureBinary.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/Crc32c.hpp>

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
            uint32_t HeaderSize; // so a reader can step over a header a later version grew
            uint32_t Width;
            uint32_t Height;
            uint32_t Format; // an ImageFormat enumerator, travelling at a width the file fixes
            uint32_t LevelCount;
            uint32_t Flags; // v1 defines none; a non-zero value is REFUSED, see below
            uint32_t SourceKeyLength;
            uint32_t SourceKeyOffset; // from file start
            uint64_t SourceContentHash;
            uint64_t EncoderHash;  // v1 has no encoder; a non-zero value is REFUSED, see below
            uint64_t PayloadBytes; // declared sum of the level sizes, padding excluded
            uint64_t FileSize;     // declared; compared against the bytes actually in hand
            uint64_t Handle;
            uint32_t Reserved[10];
        };
        static_assert( sizeof( FileHeader ) == kTextureBinaryHeaderSize );
        static_assert( alignof( FileHeader ) == 8 );
        static_assert( offsetof( FileHeader, SourceContentHash ) == 48 );
        static_assert( offsetof( FileHeader, FileSize ) == 72 );
        static_assert( offsetof( FileHeader, Handle ) == 80 );

        struct LevelRow
        {
            uint64_t ByteOffset; // from FILE start -- so a streaming read can seek straight to it
            uint32_t ByteSize;
            uint32_t RowPitch;
        };
        static_assert( sizeof( LevelRow ) == 16 );
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

        uint32_t                                   levelW = width, levelH = height;
        std::vector<std::pair<uint32_t, uint32_t>> extents( levelCount );
        for ( uint32_t level = 0; level < levelCount; ++level )
        {
            extents[level] = { levelW, levelH };
            levelW         = HalfExtent( levelW );
            levelH         = HalfExtent( levelH );
        }

        for ( uint32_t i = 0; i < levelCount; ++i )
        {
            const uint32_t level = levelCount - 1 - i; // smallest first
            while ( ( chainOut.size() % kTextureLevelAlignment ) != 0 )
                chainOut.push_back( 0 );

            levels[level].Width      = extents[level].first;
            levels[level].Height     = extents[level].second;
            levels[level].ByteOffset = chainOut.size();
            levels[level].ByteSize   = scratch[level].size();
            levels[level].RowPitch   = extents[level].first * bpp;

            chainOut.insert( chainOut.end(), scratch[level].begin(), scratch[level].end() );
        }

        return Common::MakeSuccess( std::move( levels ) );
    }

    std::string EncodeTextureBinary( const TextureAssetData& data )
    {
        const uint32_t levelCount = static_cast<uint32_t>( data.Levels.size() );

        const uint64_t keyOffset = sizeof( FileHeader ) + static_cast<uint64_t>( levelCount ) * sizeof( LevelRow );
        const uint64_t payloadOffset = AlignUp( keyOffset + data.SourcePath.size() );

        uint64_t payloadBytes = 0;
        for ( const TextureLevel& level : data.Levels )
            payloadBytes += level.ByteSize;

        // The table's offsets are FILE-relative; `TextureLevel::ByteOffset` is payload-relative. This is
        // the one conversion in the format and it lives here and in the decoder, nowhere else.
        std::vector<LevelRow> table;
        table.reserve( levelCount );
        for ( const TextureLevel& level : data.Levels )
            table.push_back( LevelRow{ payloadOffset + level.ByteOffset, static_cast<uint32_t>( level.ByteSize ),
                                       level.RowPitch } );

        FileHeader header{};
        std::memcpy( header.Magic, kTextureBinaryMagic, sizeof( header.Magic ) );
        header.ByteOrder         = kByteOrderTag;
        header.Version           = kTextureBinaryVersion;
        header.HeaderSize        = static_cast<uint32_t>( sizeof( FileHeader ) );
        header.Width             = data.Width;
        header.Height            = data.Height;
        header.Format            = static_cast<uint32_t>( data.Format );
        header.LevelCount        = levelCount;
        header.Flags             = 0;
        header.SourceKeyLength   = static_cast<uint32_t>( data.SourcePath.size() );
        header.SourceKeyOffset   = static_cast<uint32_t>( keyOffset );
        header.SourceContentHash = data.SourceContentHash;
        header.EncoderHash       = 0;
        header.PayloadBytes      = payloadBytes;
        header.FileSize          = payloadOffset + data.Pixels.size();
        header.Handle            = static_cast<uint64_t>( data.Handle );

        std::string out;
        out.reserve( static_cast<size_t>( header.FileSize ) );
        Append( out, &header, sizeof( header ) );
        Append( out, table.data(), table.size() * sizeof( LevelRow ) );
        Append( out, data.SourcePath.data(), data.SourcePath.size() );
        PadToLevelBoundary( out );
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
            if ( header.EncoderHash != 0 )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' names encoder settings {:#018x}; version {} cooks uncompressed levels only "
                     "and has no encoder to compare against. Re-cook it.",
                     who, header.EncoderHash, kTextureBinaryVersion );
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

            const uint64_t tableEnd = static_cast<uint64_t>( header.HeaderSize ) +
                                      static_cast<uint64_t>( header.LevelCount ) * sizeof( LevelRow );
            if ( tableEnd > bytes.size() )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares {} mip levels, whose table needs {} bytes, and only {} are present.", who,
                     header.LevelCount, tableEnd, bytes.size() );
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

            tableOut.resize( header.LevelCount );
            std::memcpy( tableOut.data(), bytes.data() + header.HeaderSize, tableOut.size() * sizeof( LevelRow ) );

            // THE LAYOUT IS DERIVED, NOT TRUSTED — the lesson `MeshBinary.cpp` paid for. Checking only
            // that a level lies inside the file is not enough: a corrupted offset that still fits reads
            // back a complete, plausible, WRONG image. Every level's extent follows from the header by
            // halving, its size from the extent and the format, and its offset from the level PHYSICALLY
            // before it — which, since the levels are stored smallest first, is the level one NUMBER
            // higher. A row that disagrees is refused rather than followed.
            const Core::Formats::ImageFormat format = static_cast<Core::Formats::ImageFormat>( header.Format );
            const uint32_t                   bpp    = Core::Formats::GetBytesPerPixel( format );

            const uint32_t expectLevels = Core::Formats::MipChainLength( std::max( header.Width, header.Height ) );
            if ( header.LevelCount != expectLevels )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' carries {} mip levels and a {}x{} image has a chain of {}. The cook is partial.", who,
                     header.LevelCount, header.Width, header.Height, expectLevels );
            }

            std::vector<std::pair<uint32_t, uint32_t>> extents( header.LevelCount );
            {
                uint32_t w = header.Width, h = header.Height;
                for ( uint32_t level = 0; level < header.LevelCount; ++level )
                {
                    extents[level] = { w, h };
                    w              = HalfExtent( w );
                    h              = HalfExtent( h );
                }
            }

            uint64_t expectedOffset = payloadOffset;
            uint64_t declaredBytes  = 0;
            for ( uint32_t i = 0; i < header.LevelCount; ++i )
            {
                const uint32_t  level = header.LevelCount - 1 - i; // physical order: smallest first
                const LevelRow& row   = tableOut[level];

                const uint64_t expectSize =
                     static_cast<uint64_t>( extents[level].first ) * extents[level].second * bpp;
                if ( row.ByteSize != expectSize )
                {
                    return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                         "'{}' level {} ({}x{}) claims {} bytes; {} bytes per pixel makes it {}.", who, level,
                         extents[level].first, extents[level].second, row.ByteSize, bpp, expectSize );
                }
                if ( row.RowPitch != extents[level].first * bpp )
                {
                    return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                         "'{}' level {} declares a row pitch of {}; a {}-texel row at {} bytes per pixel "
                         "is {}.",
                         who, level, row.RowPitch, extents[level].first, bpp, extents[level].first * bpp );
                }
                if ( row.ByteOffset != expectedOffset )
                {
                    return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                         "'{}' level {} starts at {}, and the levels stored before it put it at {}. The "
                         "level table does not describe this file.",
                         who, level, row.ByteOffset, expectedOffset );
                }

                declaredBytes += row.ByteSize;
                expectedOffset = AlignUp( row.ByteOffset + row.ByteSize );
            }

            // The declared payload total is a second, independent statement of the same quantity, and
            // §5c asks for it precisely so that the sum and the table have to agree with each other.
            if ( header.PayloadBytes != declaredBytes )
            {
                return Common::MakeFormattedError<TextureBinaryHeaderInfo>(
                     "'{}' declares {} bytes of pixels and its level table adds up to {}.", who,
                     header.PayloadBytes, declaredBytes );
            }

            TextureBinaryHeaderInfo info;
            info.Handle = Common::UUID( header.Handle );
            info.SourcePath.assign( bytes.data() + header.SourceKeyOffset, header.SourceKeyLength );
            info.SourceContentHash = header.SourceContentHash;
            info.Width             = header.Width;
            info.Height            = header.Height;
            info.Format            = format;
            info.LevelCount        = header.LevelCount;
            info.PayloadBytes      = header.PayloadBytes;
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

        // The payload run ends where the LAST-STORED level ends — which, with the levels reversed, is
        // level 0. Padding between levels is inside the run, so the run's length is not the sum of the
        // sizes; the sum is checked separately, against `PayloadBytes`, in the shared reader.
        const uint64_t payloadEnd = table.front().ByteOffset + table.front().ByteSize;
        if ( payloadEnd != info.GetValue().FileSize )
        {
            return Common::MakeFormattedError<TextureAssetData>(
                 "'{}' ends its last stored level at {} and declares a file of {} bytes. Trailing or "
                 "missing bytes nobody reads are a file this cook could not have produced.",
                 who, payloadEnd, info.GetValue().FileSize );
        }
        const uint64_t payloadRun = payloadEnd - payloadOffset;

        TextureAssetData data;
        data.Handle            = info.GetValue().Handle;
        data.SourcePath        = info.GetValue().SourcePath;
        data.SourceContentHash = info.GetValue().SourceContentHash;
        data.Width             = info.GetValue().Width;
        data.Height            = info.GetValue().Height;
        data.Format            = info.GetValue().Format;

        // THE ONE CONVERSION IN THIS FORMAT, made exactly once. The file's rows are FILE-relative; the
        // GPU needs offsets inside the staging buffer, which holds the payload run and nothing else.
        uint32_t levelW = info.GetValue().Width, levelH = info.GetValue().Height;
        data.Levels.reserve( table.size() );
        for ( const LevelRow& row : table )
        {
            data.Levels.push_back(
                 TextureLevel{ levelW, levelH, row.ByteOffset - payloadOffset, row.ByteSize, row.RowPitch } );
            levelW = levelW > 1u ? levelW / 2u : 1u;
            levelH = levelH > 1u ? levelH / 2u : 1u;
        }

        data.Pixels.resize( static_cast<size_t>( payloadRun ) );
        if ( payloadRun > 0 )
            std::memcpy( data.Pixels.data(), bytes.data() + payloadOffset, static_cast<size_t>( payloadRun ) );

        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets::Serialization
