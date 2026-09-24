#include "AssetEnvelope.hpp"
#include "TextAssetHeader.hpp"
#include "MeshBinaryHeader.hpp"

#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <algorithm>
#include <bit>
#include <fstream>
#include <istream>
#include <limits>
#include <random>

namespace Common::Content
{
    namespace
    {
        // A header larger than this is a corrupt size field, not an asset: refusing it before the
        // allocation keeps a flipped bit from turning into a multi-gigabyte read.
        constexpr uint32_t MAX_HEADER_SIZE = 16u * 1024u * 1024u;

        constexpr std::size_t FIXED_PREFIX_SIZE = 16; // magic, version, header size, CRC
        constexpr std::size_t CRC_OFFSET        = 12;
        constexpr std::size_t TOC_ENTRY_SIZE    = 32;

        class ByteWriter
        {
        public:
            void U8( uint8_t v )
            {
                m_Bytes.push_back( static_cast<std::byte>( v ) );
            }

            void U16( uint16_t v )
            {
                for ( int i = 0; i < 2; ++i )
                    U8( static_cast<uint8_t>( v >> ( 8 * i ) ) );
            }

            void U32( uint32_t v )
            {
                for ( int i = 0; i < 4; ++i )
                    U8( static_cast<uint8_t>( v >> ( 8 * i ) ) );
            }

            void U64( uint64_t v )
            {
                for ( int i = 0; i < 8; ++i )
                    U8( static_cast<uint8_t>( v >> ( 8 * i ) ) );
            }

            void Bytes( std::span<const std::byte> bytes )
            {
                m_Bytes.insert( m_Bytes.end(), bytes.begin(), bytes.end() );
            }

            void String16( std::string_view text )
            {
                U16( static_cast<uint16_t>( text.size() ) );
                Bytes( std::as_bytes( std::span( text.data(), text.size() ) ) );
            }

            void PatchU32( std::size_t at, uint32_t v )
            {
                for ( int i = 0; i < 4; ++i )
                    m_Bytes[at + i] = static_cast<std::byte>( v >> ( 8 * i ) );
            }

            std::size_t Size() const
            {
                return m_Bytes.size();
            }

            std::vector<std::byte>& Data()
            {
                return m_Bytes;
            }

        private:
            std::vector<std::byte> m_Bytes;
        };

        // Every read is bounds-checked; the first failure sticks, so a parse can run to the end of a
        // block and ask once. `What` names the field that ran off the end.
        class ByteReader
        {
        public:
            explicit ByteReader( std::span<const std::byte> bytes ) : m_Bytes( bytes )
            {
            }

            bool Need( std::size_t count, std::string_view what )
            {
                if ( !m_Failed.empty() )
                    return false;
                if ( count > m_Bytes.size() - m_Pos )
                {
                    m_Failed = std::string( what );
                    return false;
                }
                return true;
            }

            uint64_t Uint( std::size_t width, std::string_view what )
            {
                if ( !Need( width, what ) )
                    return 0;
                uint64_t v = 0;
                for ( std::size_t i = 0; i < width; ++i )
                    v |= static_cast<uint64_t>( std::to_integer<uint8_t>( m_Bytes[m_Pos + i] ) ) << ( 8 * i );
                m_Pos += width;
                return v;
            }

            std::string String16( std::string_view what )
            {
                const auto length = static_cast<std::size_t>( Uint( 2, what ) );
                if ( !Need( length, what ) )
                    return {};
                std::string text( reinterpret_cast<const char*>( m_Bytes.data() + m_Pos ), length );
                m_Pos += length;
                return text;
            }

            std::size_t Remaining() const
            {
                return m_Bytes.size() - m_Pos;
            }

            const std::string& Failed() const
            {
                return m_Failed;
            }

        private:
            std::span<const std::byte> m_Bytes;
            std::size_t                m_Pos = 0;
            std::string                m_Failed;
        };

        std::optional<ContentKind> KindForName( std::string_view name )
        {
            const auto kinds = ContentKinds();
            for ( std::size_t i = 0; i < kinds.size(); ++i )
            {
                if ( kinds[i].Name == name )
                    return static_cast<ContentKind>( i );
            }
            return std::nullopt;
        }

        bool IsKnownSection( uint32_t tag )
        {
            switch ( static_cast<EnvelopeSection>( tag ) )
            {
                case EnvelopeSection::Meta:
                case EnvelopeSection::ImportInfo:
                case EnvelopeSection::Source:
                case EnvelopeSection::Payload:
                    return true;
            }
            return false;
        }

        bool IsKnownCodec( uint32_t codec )
        {
            switch ( static_cast<EnvelopeCodec>( codec ) )
            {
                case EnvelopeCodec::Stored:
                    return true;
            }
            return false;
        }

        uint32_t HeaderCrc( std::span<const std::byte> header )
        {
            std::vector<std::byte> copy( header.begin(), header.end() );
            std::fill_n( copy.begin() + CRC_OFFSET, 4, std::byte{ 0 } );
            return Utils::Crc32c( copy.data(), copy.size() );
        }

        uint64_t SectionHash( std::span<const std::byte> bytes )
        {
            return Utils::PakContentHash( bytes.data(), bytes.size() );
        }

        uint32_t LoadU32( std::span<const std::byte> bytes, std::size_t at )
        {
            uint32_t v = 0;
            for ( std::size_t i = 0; i < 4; ++i )
                v |= static_cast<uint32_t>( std::to_integer<uint8_t>( bytes[at + i] ) ) << ( 8 * i );
            return v;
        }

        // 256 bits of OS entropy per thread: a GUID collision between two assets would merge their
        // identities silently, so the seed is not a single 32-bit random_device draw.
        std::mt19937_64 SeededEngine()
        {
            std::random_device device;
            std::seed_seq seed{ device(), device(), device(), device(), device(), device(), device(), device() };
            return std::mt19937_64( seed );
        }
    } // namespace

    std::string FourCCToString( uint32_t tag )
    {
        std::string text( 4, '?' );
        for ( int i = 0; i < 4; ++i )
        {
            const auto c = static_cast<unsigned char>( tag >> ( 8 * i ) );
            if ( c >= 0x20 && c < 0x7f )
                text[i] = static_cast<char>( c );
        }
        return text;
    }

    AssetGuid AssetGuid::Generate()
    {
        static thread_local std::mt19937_64 engine = SeededEngine();
        AssetGuid                           guid;
        do
        {
            guid.Hi = engine();
            guid.Lo = engine();
        } while ( guid.IsNull() );
        return guid;
    }

    AssetHandle HandleForGuid( const AssetGuid& guid ) noexcept
    {
        if ( guid.IsNull() )
            return AssetHandle();
        uint64_t hash = 1469598103934665603ull;
        for ( const uint64_t half : { guid.Hi, guid.Lo } )
        {
            for ( int shift = 56; shift >= 0; shift -= 8 )
            {
                hash ^= ( half >> shift ) & 0xFFu;
                hash *= 1099511628211ull;
            }
        }
        return AssetHandle( hash != 0 ? hash : 1ull );
    }

    std::optional<EnvelopeTocEntry> EnvelopeHeader::Find( EnvelopeSection tag ) const
    {
        for ( const EnvelopeTocEntry& entry : Toc )
        {
            if ( entry.Tag == tag )
                return entry;
        }
        return std::nullopt;
    }

    uint64_t EnvelopeHeader::EndOfSections() const
    {
        return Toc.empty() ? HeaderSize : Toc.back().Offset + Toc.back().Size;
    }

    ResultStr<std::vector<std::byte>> WriteAssetEnvelope( const AssetEnvelope& envelope )
    {
        using Out               = std::vector<std::byte>;
        const AssetHeader& head = envelope.Asset;
        if ( head.Kind == ContentKind::COUNT )
            return MakeError<Out>( "asset envelope: kind is COUNT, not a content kind" );
        if ( head.Guid.IsNull() )
            return MakeError<Out>( "asset envelope: null GUID — mint one with AssetGuid::Generate() once, "
                                   "then keep it" );
        for ( std::size_t i = 0; i < head.Subsystems.size(); ++i )
        {
            for ( std::size_t j = i + 1; j < head.Subsystems.size(); ++j )
            {
                if ( head.Subsystems[i].Tag == head.Subsystems[j].Tag )
                    return MakeFormattedError<Out>( "asset envelope: subsystem '{}' listed twice",
                                                    FourCCToString( head.Subsystems[i].Tag ) );
            }
        }
        for ( const AssetGuid& dependency : head.Dependencies )
        {
            if ( dependency.IsNull() )
                return MakeError<Out>( "asset envelope: null dependency GUID" );
        }
        for ( std::size_t i = 0; i < envelope.Sections.size(); ++i )
        {
            for ( std::size_t j = i + 1; j < envelope.Sections.size(); ++j )
            {
                if ( envelope.Sections[i].Tag == envelope.Sections[j].Tag )
                    return MakeFormattedError<Out>(
                         "asset envelope: section '{}' listed twice",
                         FourCCToString( static_cast<uint32_t>( envelope.Sections[i].Tag ) ) );
            }
        }

        ByteWriter w;
        w.U32( ASSET_ENVELOPE_MAGIC );
        w.U32( ASSET_ENVELOPE_CONTAINER_VERSION );
        w.U32( 0 ); // header size, patched below
        w.U32( 0 ); // CRC, patched below
        w.U64( head.Guid.Hi );
        w.U64( head.Guid.Lo );
        w.String16( KindName( head.Kind ) );
        w.U32( static_cast<uint32_t>( head.Subsystems.size() ) );
        for ( const SubsystemVersion& subsystem : head.Subsystems )
        {
            w.U32( subsystem.Tag );
            w.U32( subsystem.Version );
        }
        w.U32( static_cast<uint32_t>( head.Dependencies.size() ) );
        for ( const AssetGuid& dependency : head.Dependencies )
        {
            w.U64( dependency.Hi );
            w.U64( dependency.Lo );
        }
        w.U32( static_cast<uint32_t>( envelope.Sections.size() ) );
        const std::size_t headerSize = w.Size() + envelope.Sections.size() * TOC_ENTRY_SIZE;
        if ( headerSize > MAX_HEADER_SIZE )
            return MakeFormattedError<Out>( "asset envelope: header of {} bytes exceeds the {} byte limit",
                                            headerSize, MAX_HEADER_SIZE );
        uint64_t offset = headerSize;
        for ( const EnvelopeSectionData& section : envelope.Sections )
        {
            w.U32( static_cast<uint32_t>( section.Tag ) );
            w.U32( static_cast<uint32_t>( section.Codec ) );
            w.U64( offset );
            w.U64( section.Bytes.size() );
            w.U64( SectionHash( section.Bytes ) );
            offset += section.Bytes.size();
        }
        w.PatchU32( 8, static_cast<uint32_t>( headerSize ) );
        w.PatchU32( CRC_OFFSET, HeaderCrc( std::span( w.Data() ).first( headerSize ) ) );
        for ( const EnvelopeSectionData& section : envelope.Sections )
            w.Bytes( section.Bytes );
        return MakeSuccess( std::move( w.Data() ) );
    }

    BoolResultStr WriteAssetEnvelopeFile( const std::filesystem::path& file, const AssetEnvelope& envelope )
    {
        auto bytes = WriteAssetEnvelope( envelope );
        if ( !bytes )
            return MakeError<bool>( bytes.GetError() );
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        if ( !out )
            return MakeFormattedError<bool>( "asset envelope: cannot open '{}' for writing", file.string() );
        const auto& data = bytes.GetValue();
        out.write( reinterpret_cast<const char*>( data.data() ), static_cast<std::streamsize>( data.size() ) );
        out.close();
        if ( !out )
            return MakeFormattedError<bool>( "asset envelope: writing {} bytes to '{}' failed", data.size(),
                                             file.string() );
        return MakeSuccess( true );
    }

    ResultStr<EnvelopeHeader> ReadEnvelopeHeader( std::span<const std::byte>    prefix,
                                                  const AssetHeaderReadContext& context )
    {
        using Out = EnvelopeHeader;
        if ( prefix.size() < FIXED_PREFIX_SIZE )
            return MakeFormattedError<Out>( "asset envelope: {} bytes is shorter than the {} byte fixed prefix",
                                            prefix.size(), FIXED_PREFIX_SIZE );
        const uint32_t magic      = LoadU32( prefix, 0 );
        const uint32_t version    = LoadU32( prefix, 4 );
        const uint32_t headerSize = LoadU32( prefix, 8 );
        const uint32_t storedCrc  = LoadU32( prefix, CRC_OFFSET );
        if ( magic != ASSET_ENVELOPE_MAGIC )
            return MakeFormattedError<Out>( "asset envelope: magic '{}' is not '{}'", FourCCToString( magic ),
                                            FourCCToString( ASSET_ENVELOPE_MAGIC ) );
        if ( version != ASSET_ENVELOPE_CONTAINER_VERSION )
            return MakeFormattedError<Out>( "asset envelope: container version {} — this build reads {}", version,
                                            ASSET_ENVELOPE_CONTAINER_VERSION );
        if ( headerSize < FIXED_PREFIX_SIZE || headerSize > MAX_HEADER_SIZE )
            return MakeFormattedError<Out>( "asset envelope: header size {} is outside [{}, {}]", headerSize,
                                            FIXED_PREFIX_SIZE, MAX_HEADER_SIZE );
        if ( prefix.size() < headerSize )
            return MakeFormattedError<Out>( "asset envelope: header declares {} bytes, only {} available",
                                            headerSize, prefix.size() );
        const auto header = prefix.first( headerSize );
        if ( const uint32_t crc = HeaderCrc( header ); crc != storedCrc )
            return MakeFormattedError<Out>( "asset envelope: header CRC-32C {:08x} does not match stored {:08x}",
                                            crc, storedCrc );

        EnvelopeHeader out;
        out.HeaderSize = headerSize;
        ByteReader r( header.subspan( FIXED_PREFIX_SIZE ) );
        out.Asset.Guid.Hi      = r.Uint( 8, "GUID" );
        out.Asset.Guid.Lo      = r.Uint( 8, "GUID" );
        const std::string kind = r.String16( "kind name" );
        if ( !r.Failed().empty() )
            return MakeFormattedError<Out>( "asset envelope: header ends inside the {}", r.Failed() );
        if ( out.Asset.Guid.IsNull() )
            return MakeError<Out>( "asset envelope: null GUID" );
        const auto resolved = KindForName( kind );
        if ( !resolved )
            return MakeFormattedError<Out>( "asset envelope: unknown kind '{}'", kind );
        out.Asset.Kind = *resolved;

        const auto subsystemCount = r.Uint( 4, "subsystem count" );
        if ( !r.Need( subsystemCount * 8, "subsystem table" ) )
            return MakeFormattedError<Out>( "asset envelope: header ends inside the {}", r.Failed() );
        for ( uint64_t i = 0; i < subsystemCount; ++i )
        {
            SubsystemVersion stamped;
            stamped.Tag     = static_cast<uint32_t>( r.Uint( 4, "subsystem table" ) );
            stamped.Version = static_cast<uint32_t>( r.Uint( 4, "subsystem table" ) );
            for ( const SubsystemVersion& earlier : out.Asset.Subsystems )
            {
                if ( earlier.Tag == stamped.Tag )
                    return MakeFormattedError<Out>( "asset envelope: subsystem '{}' listed twice",
                                                    FourCCToString( stamped.Tag ) );
            }
            const auto known = std::find_if( context.KnownSubsystems.begin(), context.KnownSubsystems.end(),
                                             [&]( const SubsystemVersion& k ) { return k.Tag == stamped.Tag; } );
            if ( context.RecordOnly )
            {
                out.Asset.Subsystems.push_back( stamped );
                continue;
            }
            if ( known == context.KnownSubsystems.end() )
                return MakeFormattedError<Out>(
                     "asset envelope: subsystem '{}' (version {}) is unknown to this build",
                     FourCCToString( stamped.Tag ), stamped.Version );
            if ( stamped.Version > known->Version )
                return MakeFormattedError<Out>(
                     "asset envelope: subsystem '{}' version {} is newer than this build's {}",
                     FourCCToString( stamped.Tag ), stamped.Version, known->Version );
            out.Asset.Subsystems.push_back( stamped );
        }

        const auto dependencyCount = r.Uint( 4, "dependency count" );
        if ( !r.Need( dependencyCount * 16, "dependency table" ) )
            return MakeFormattedError<Out>( "asset envelope: header ends inside the {}", r.Failed() );
        for ( uint64_t i = 0; i < dependencyCount; ++i )
        {
            AssetGuid dependency;
            dependency.Hi = r.Uint( 8, "dependency table" );
            dependency.Lo = r.Uint( 8, "dependency table" );
            if ( dependency.IsNull() )
                return MakeFormattedError<Out>( "asset envelope: dependency {} is a null GUID", i );
            out.Asset.Dependencies.push_back( dependency );
        }

        const auto sectionCount = r.Uint( 4, "section count" );
        if ( !r.Need( sectionCount * TOC_ENTRY_SIZE, "section TOC" ) )
            return MakeFormattedError<Out>( "asset envelope: header ends inside the {}", r.Failed() );
        uint64_t expectedOffset = headerSize;
        for ( uint64_t i = 0; i < sectionCount; ++i )
        {
            const auto       tag   = static_cast<uint32_t>( r.Uint( 4, "section TOC" ) );
            const auto       codec = static_cast<uint32_t>( r.Uint( 4, "section TOC" ) );
            EnvelopeTocEntry entry;
            entry.Offset = r.Uint( 8, "section TOC" );
            entry.Size   = r.Uint( 8, "section TOC" );
            entry.Hash   = r.Uint( 8, "section TOC" );
            if ( !IsKnownSection( tag ) )
                return MakeFormattedError<Out>( "asset envelope: TOC entry {} has unknown section tag '{}'", i,
                                                FourCCToString( tag ) );
            if ( !IsKnownCodec( codec ) )
                return MakeFormattedError<Out>( "asset envelope: section '{}' has unknown codec {}",
                                                FourCCToString( tag ), codec );
            entry.Tag   = static_cast<EnvelopeSection>( tag );
            entry.Codec = static_cast<EnvelopeCodec>( codec );
            if ( out.Find( entry.Tag ) )
                return MakeFormattedError<Out>( "asset envelope: section '{}' listed twice",
                                                FourCCToString( tag ) );
            // Contiguous from the end of the header: no gaps to hide bytes in, no overlaps to alias.
            if ( entry.Offset != expectedOffset )
                return MakeFormattedError<Out>( "asset envelope: section '{}' starts at {}, expected {}",
                                                FourCCToString( tag ), entry.Offset, expectedOffset );
            if ( entry.Size > std::numeric_limits<uint64_t>::max() - entry.Offset )
                return MakeFormattedError<Out>( "asset envelope: section '{}' size {} overflows its offset {}",
                                                FourCCToString( tag ), entry.Size, entry.Offset );
            expectedOffset = entry.Offset + entry.Size;
            out.Toc.push_back( entry );
        }
        if ( r.Remaining() != 0 )
            return MakeFormattedError<Out>( "asset envelope: {} unparsed bytes at the end of the header",
                                            r.Remaining() );
        return MakeSuccess( std::move( out ) );
    }

    ResultStr<EnvelopeHeader> ReadEnvelopeHeader( std::istream& in, const AssetHeaderReadContext& context )
    {
        using Out = EnvelopeHeader;
        std::vector<std::byte> prefix( FIXED_PREFIX_SIZE );
        if ( !in.read( reinterpret_cast<char*>( prefix.data() ), static_cast<std::streamsize>( prefix.size() ) ) )
            return MakeFormattedError<Out>( "asset envelope: stream ends before the {} byte fixed prefix",
                                            FIXED_PREFIX_SIZE );
        const uint32_t headerSize = LoadU32( prefix, 8 );
        if ( LoadU32( prefix, 0 ) == ASSET_ENVELOPE_MAGIC && headerSize > FIXED_PREFIX_SIZE &&
             headerSize <= MAX_HEADER_SIZE )
        {
            prefix.resize( headerSize );
            const auto rest = static_cast<std::streamsize>( headerSize - FIXED_PREFIX_SIZE );
            if ( !in.read( reinterpret_cast<char*>( prefix.data() + FIXED_PREFIX_SIZE ), rest ) )
                return MakeFormattedError<Out>( "asset envelope: stream ends inside the {} byte header",
                                                headerSize );
        }
        // A bad magic or size falls through with the fixed prefix only, so the span reader names it.
        return ReadEnvelopeHeader( std::span<const std::byte>( prefix ), context );
    }

    ResultStr<AssetEnvelope> ReadAssetEnvelope( std::span<const std::byte>    file,
                                                const AssetHeaderReadContext& context )
    {
        using Out   = AssetEnvelope;
        auto header = ReadEnvelopeHeader( file, context );
        if ( !header )
            return MakeError<Out>( header.GetError() );
        const EnvelopeHeader& head = header.GetValue();
        AssetEnvelope         out;
        out.Asset = head.Asset;
        for ( const EnvelopeTocEntry& entry : head.Toc )
        {
            const std::string tag = FourCCToString( static_cast<uint32_t>( entry.Tag ) );
            if ( entry.Offset + entry.Size > file.size() )
                return MakeFormattedError<Out>( "asset envelope: section '{}' [{}, {}) ends past the file size {}",
                                                tag, entry.Offset, entry.Offset + entry.Size, file.size() );
            const auto bytes = file.subspan( entry.Offset, entry.Size );
            if ( const uint64_t hash = SectionHash( bytes ); hash != entry.Hash )
                return MakeFormattedError<Out>(
                     "asset envelope: section '{}' hash {:016x} does not match TOC {:016x}", tag, hash,
                     entry.Hash );
            out.Sections.push_back(
                 { entry.Tag, entry.Codec, std::vector<std::byte>( bytes.begin(), bytes.end() ) } );
        }
        if ( head.EndOfSections() != file.size() )
            return MakeFormattedError<Out>( "asset envelope: {} trailing bytes after the last section",
                                            file.size() - head.EndOfSections() );
        return MakeSuccess( std::move( out ) );
    }

    ResultStr<AssetEnvelope> ReadAssetEnvelopeFile( const std::filesystem::path&  file,
                                                    const AssetHeaderReadContext& context )
    {
        std::ifstream in( file, std::ios::binary );
        if ( !in )
            return MakeFormattedError<AssetEnvelope>( "asset envelope: cannot open '{}'", file.string() );
        std::vector<std::byte> bytes;
        in.seekg( 0, std::ios::end );
        bytes.resize( static_cast<std::size_t>( in.tellg() ) );
        in.seekg( 0, std::ios::beg );
        if ( !in.read( reinterpret_cast<char*>( bytes.data() ), static_cast<std::streamsize>( bytes.size() ) ) )
            return MakeFormattedError<AssetEnvelope>( "asset envelope: reading '{}' failed", file.string() );
        auto envelope = ReadAssetEnvelope( bytes, context );
        if ( !envelope )
            return MakeFormattedError<AssetEnvelope>( "{} ('{}')", envelope.GetError(), file.string() );
        return envelope;
    }

    namespace
    {
        class BinaryEnvelopeFormat final : public IAssetHeaderFormat
        {
        public:
            std::string_view Name() const override
            {
                return "binary envelope";
            }

            bool Recognises( std::span<const std::byte> leading ) const override
            {
                return leading.size() >= 4 && LoadU32( leading, 0 ) == ASSET_ENVELOPE_MAGIC;
            }

            ResultStr<AssetHeader> ReadHeader( std::istream&                 in,
                                               const AssetHeaderReadContext& context ) const override
            {
                auto header = ReadEnvelopeHeader( in, context );
                if ( !header )
                    return MakeError<AssetHeader>( header.GetError() );
                return MakeSuccess( header.GetValue().Asset );
            }
        };
    } // namespace

    namespace
    {
        class MeshBinaryFormat final : public IAssetHeaderFormat
        {
        public:
            std::string_view Name() const override
            {
                return "mesh binary";
            }

            // Only a version that states a GUID is claimed: a v1/v2 mesh states no header, as before.
            bool Recognises( std::span<const std::byte> leading ) const override
            {
                return leading.size() >= 16 && std::memcmp( leading.data(), kMeshBinaryMagic, 8 ) == 0 &&
                       LoadU32( leading, 8 ) == kMeshBinaryByteOrderTag && LoadU32( leading, 12 ) >= 3;
            }

            ResultStr<AssetHeader> ReadHeader( std::istream& in, const AssetHeaderReadContext& ) const override
            {
                std::string prefix( kMeshBinaryPrefixV3, '\0' );
                in.read( prefix.data(), static_cast<std::streamsize>( prefix.size() ) );
                if ( static_cast<std::size_t>( in.gcount() ) != prefix.size() )
                    return MakeFormattedError<AssetHeader>( "mesh header: {} bytes where the prefix is {}",
                                                            in.gcount(), kMeshBinaryPrefixV3 );
                MeshBinaryFileHeader header{};
                std::memcpy( &header, prefix.data(), sizeof( header ) );
                // Recognises() claims every version from 3 up, so that a later mesh is REFUSED here by name
                // rather than passed over as "states no header": nothing says a later layout keeps the GUID
                // at byte 64, and reading it from there would state an identity the file never wrote.
                if ( header.Version > kMeshBinaryVersion )
                    return MakeFormattedError<AssetHeader>( "mesh header: mesh version {}, this build reads 1..{}",
                                                            header.Version, kMeshBinaryVersion );
                const std::optional<AssetGuid> guid = ReadMeshHeaderGuid( prefix );
                if ( !guid || guid->IsNull() )
                    return MakeError<AssetHeader>( "mesh header: a version 3 mesh states a null GUID" );
                AssetHeader stated;
                stated.Kind = ( header.Flags & kMeshFlagIsSkinned ) != 0 ? ContentKind::SkinnedMesh
                                                                         : ContentKind::StaticMesh;
                stated.Guid = *guid;
                return MakeSuccess( std::move( stated ) );
            }
        };
    } // namespace

    const IAssetHeaderFormat& MeshBinaryHeaderFormat()
    {
        static const MeshBinaryFormat format;
        return format;
    }

    const IAssetHeaderFormat& BinaryEnvelopeHeaderFormat()
    {
        static const BinaryEnvelopeFormat format;
        return format;
    }

    std::span<const IAssetHeaderFormat* const> AssetHeaderFormats()
    {
        static const std::array<const IAssetHeaderFormat*, 3> formats = {
             &BinaryEnvelopeHeaderFormat(), &TextHeaderFormat(), &MeshBinaryHeaderFormat() };
        return formats;
    }

    ResultStr<std::optional<AssetHeader>> ReadAssetHeaderIfStated( const std::filesystem::path&  file,
                                                                   const AssetHeaderReadContext& context )
    {
        using Out = std::optional<AssetHeader>;
        std::ifstream in( file, std::ios::binary );
        if ( !in )
            return MakeFormattedError<Out>( "asset header: cannot open '{}'", file.string() );
        std::array<std::byte, ASSET_HEADER_SNIFF_BYTES> leading{};
        in.read( reinterpret_cast<char*>( leading.data() ), static_cast<std::streamsize>( leading.size() ) );
        const auto sniffed =
             std::span<const std::byte>( leading ).first( static_cast<std::size_t>( in.gcount() ) );
        in.clear();
        in.seekg( 0, std::ios::beg );
        for ( const IAssetHeaderFormat* format : AssetHeaderFormats() )
        {
            if ( !format->Recognises( sniffed ) )
                continue;
            auto header = format->ReadHeader( in, context );
            if ( !header )
                return MakeFormattedError<Out>( "{} ('{}', {})", header.GetError(), file.string(),
                                                format->Name() );
            return MakeSuccess( Out( std::move( header.GetValue() ) ) );
        }
        return MakeSuccess( Out() );
    }

    ResultStr<AssetHeader> ReadAssetHeader( const std::filesystem::path&  file,
                                            const AssetHeaderReadContext& context )
    {
        auto stated = ReadAssetHeaderIfStated( file, context );
        if ( !stated )
            return MakeError<AssetHeader>( stated.GetError() );
        if ( !stated.GetValue() )
            return MakeFormattedError<AssetHeader>( "asset header: no header format recognises '{}'",
                                                    file.string() );
        return MakeSuccess( *stated.GetValue() );
    }

    std::vector<std::byte> EncodeEnvelopeMeta( const EnvelopeMeta& meta )
    {
        ByteWriter w;
        w.String16( meta.Name );
        w.U32( static_cast<uint32_t>( meta.Tags.size() ) );
        for ( const std::string& tag : meta.Tags )
            w.String16( tag );
        w.U8( meta.Bounds ? 1 : 0 );
        if ( meta.Bounds )
        {
            for ( float v : meta.Bounds->Lo )
                w.U32( std::bit_cast<uint32_t>( v ) );
            for ( float v : meta.Bounds->Hi )
                w.U32( std::bit_cast<uint32_t>( v ) );
        }
        return std::move( w.Data() );
    }

    ResultStr<EnvelopeMeta> DecodeEnvelopeMeta( std::span<const std::byte> bytes )
    {
        using Out = EnvelopeMeta;
        ByteReader   r( bytes );
        EnvelopeMeta meta;
        meta.Name           = r.String16( "meta name" );
        const auto tagCount = r.Uint( 4, "meta tag count" );
        // Each tag costs at least its 2 length bytes, which bounds the count before any allocation.
        if ( r.Need( tagCount * 2, "meta tags" ) )
        {
            for ( uint64_t i = 0; i < tagCount; ++i )
                meta.Tags.push_back( r.String16( "meta tags" ) );
        }
        const auto hasBounds = r.Uint( 1, "meta bounds flag" );
        if ( hasBounds > 1 )
            return MakeFormattedError<Out>( "asset meta: bounds flag {} is neither 0 nor 1", hasBounds );
        if ( hasBounds == 1 )
        {
            EnvelopeBounds bounds;
            for ( float& v : bounds.Lo )
                v = std::bit_cast<float>( static_cast<uint32_t>( r.Uint( 4, "meta bounds" ) ) );
            for ( float& v : bounds.Hi )
                v = std::bit_cast<float>( static_cast<uint32_t>( r.Uint( 4, "meta bounds" ) ) );
            meta.Bounds = bounds;
        }
        if ( !r.Failed().empty() )
            return MakeFormattedError<Out>( "asset meta: section ends inside the {}", r.Failed() );
        if ( r.Remaining() != 0 )
            return MakeFormattedError<Out>( "asset meta: {} unparsed trailing bytes", r.Remaining() );
        return MakeSuccess( std::move( meta ) );
    }
} // namespace Common::Content
