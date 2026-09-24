#pragma once

// THE EDITOR-SIDE ASSET ENVELOPE — one binary container that every binary content kind is wrapped in.
//
// Why one envelope and not a header per format: the kind of an asset has to be answerable from the
// file ALONE, without the extension and without decoding the body — the role UE gives
// FPackageFileSummary, whose class and custom versions the AssetRegistry reads without touching the
// exports. A per-format header would make "what is this file" a question with one answer per format.
//
// What this is NOT: the runtime archive. `.dpak` stays the cooked, shipping layer; the envelope is what
// the editor reads and writes, and the cook strips its editor-only sections (ImportInfo, Source) on the
// way into a pak. Text kinds (.desce, .demat) do not get the envelope; they will implement the same
// header CONTRACT (`IAssetHeaderFormat` below), which is why reading a header is an interface and not
// a function that knows about binary layout.
//
// Byte layout, container version 1, all integers little-endian regardless of host:
//
//   off  size  field
//     0     4  magic "DAST"
//     4     4  container version (1)
//     8     4  header size H: bytes from offset 0 up to the first section
//    12     4  CRC-32C over [0, H) computed with THIS field zeroed
//    16    16  asset GUID (hi u64, lo u64) — frozen at creation, never derived from a path
//    32   2+n  kind name (u16 length + bytes): `ContentKindSpec::Name`, the registry's one spelling,
//              not the enumerator index, so reordering `ContentKind` cannot re-kind a file
//         4+8k subsystem versions: u32 count, then { u32 tag, u32 version } — UE custom versions
//        4+16d dependencies: u32 count, then { u64 hi, u64 lo } GUIDs
//        4+32s section TOC: u32 count, then { u32 tag, u32 codec, u64 offset, u64 size, u64 hash }
//     H   ...  section bodies, contiguous and in TOC order; the first starts exactly at H
//
// The TOC hash is `Utils::PakContentHash` of the section's bytes: it is both the integrity check of
// the body and the section's content id (the future DDC key), so there is one hash, not two.

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Common::Content
{
    // Four ASCII characters packed so the FIRST character is the lowest byte, i.e. the tag reads
    // correctly in a hex dump of the little-endian file.
    constexpr uint32_t FourCC( const char ( &text )[5] )
    {
        return static_cast<uint32_t>( static_cast<unsigned char>( text[0] ) ) |
               ( static_cast<uint32_t>( static_cast<unsigned char>( text[1] ) ) << 8 ) |
               ( static_cast<uint32_t>( static_cast<unsigned char>( text[2] ) ) << 16 ) |
               ( static_cast<uint32_t>( static_cast<unsigned char>( text[3] ) ) << 24 );
    }

    // Printable form of a FourCC for error messages; non-printable bytes come out as '?'.
    std::string FourCCToString( uint32_t tag );

    constexpr uint32_t ASSET_ENVELOPE_MAGIC             = FourCC( "DAST" );
    constexpr uint32_t ASSET_ENVELOPE_CONTAINER_VERSION = 1;

    // 128 bits, as UE's FGuid: an asset keeps this identity across renames and moves, so it is minted
    // once (Generate) and afterwards only ever copied. There is deliberately no constructor from a path.
    struct AssetGuid
    {
        uint64_t Hi = 0;
        uint64_t Lo = 0;

        static AssetGuid Generate();

        bool IsNull() const
        {
            return Hi == 0 && Lo == 0;
        }

        bool operator==( const AssetGuid& ) const = default;
    };

    // One subsystem's serialization version inside an asset: a mesh envelope carries the mesh codec's
    // version AND the material-slot layout's AND the bounds', each owned by a different subsystem.
    struct SubsystemVersion
    {
        uint32_t Tag     = 0;
        uint32_t Version = 0;

        bool operator==( const SubsystemVersion& ) const = default;
    };

    // THE HEADER CONTRACT: what any asset file — binary envelope today, text header in AF6 — must be
    // able to state about itself without its body being read.
    struct AssetHeader
    {
        ContentKind                   Kind = ContentKind::COUNT;
        AssetGuid                     Guid;
        std::vector<SubsystemVersion> Subsystems;
        std::vector<AssetGuid>        Dependencies;

        bool operator==( const AssetHeader& ) const = default;
    };

    // What the reading build knows. A file stamped with a subsystem this build has never heard of, or
    // with a NEWER version than it knows, is refused: decoding it with older code would read a layout
    // that code was not written for (UE refuses such a package for the same reason).
    //
    // RECORDING IS NOT READING. The asset registry is cooked by a tool that links `Common` only and so
    // cannot know the engine's subsystem versions; it RECORDS what a header states (kind, GUID, versions)
    // and the build that loads the body judges them. `RecordOnly` is that one caller's question, and it
    // never lets a header through that is malformed: kind, GUID and tag shape are checked all the same.
    struct AssetHeaderReadContext
    {
        std::span<const SubsystemVersion> KnownSubsystems;
        bool                              RecordOnly = false;
    };

    enum class EnvelopeSection : uint32_t
    {
        Meta       = FourCC( "META" ), // name, tags, bounds — EnvelopeMeta below
        ImportInfo = FourCC( "IMPT" ), // editor only: how the asset was imported
        Source     = FourCC( "SRCE" ), // editor only: the source data the asset is rebuilt from
        Payload    = FourCC( "PAYL" ), // the kind's own bytes, in the codec the kind already uses
    };

    // Whether the ENVELOPE transforms the section bytes. Payload keeps its kind's own encoding inside
    // the bytes, so today every section is Stored; an unknown codec is refused rather than passed on.
    enum class EnvelopeCodec : uint32_t
    {
        Stored = 0,
    };

    struct EnvelopeTocEntry
    {
        EnvelopeSection Tag    = EnvelopeSection::Payload;
        EnvelopeCodec   Codec  = EnvelopeCodec::Stored;
        uint64_t        Offset = 0;
        uint64_t        Size   = 0;
        uint64_t        Hash   = 0; // Utils::PakContentHash of the section bytes

        bool operator==( const EnvelopeTocEntry& ) const = default;
    };

    struct EnvelopeHeader
    {
        AssetHeader                   Asset;
        uint32_t                      HeaderSize = 0;
        std::vector<EnvelopeTocEntry> Toc;

        std::optional<EnvelopeTocEntry> Find( EnvelopeSection tag ) const;
        // Offset one past the last section: the size a complete file must have.
        uint64_t EndOfSections() const;
    };

    struct EnvelopeSectionData
    {
        EnvelopeSection        Tag   = EnvelopeSection::Payload;
        EnvelopeCodec          Codec = EnvelopeCodec::Stored;
        std::vector<std::byte> Bytes;

        bool operator==( const EnvelopeSectionData& ) const = default;
    };

    struct AssetEnvelope
    {
        AssetHeader                      Asset;
        std::vector<EnvelopeSectionData> Sections;
    };

    // Serialises in the order given (so a read-then-write reproduces the file byte for byte). Refuses a
    // null GUID, a COUNT kind, a null dependency, and duplicate subsystem tags or section tags.
    ResultStr<std::vector<std::byte>> WriteAssetEnvelope( const AssetEnvelope& envelope );
    BoolResultStr WriteAssetEnvelopeFile( const std::filesystem::path& file, const AssetEnvelope& envelope );

    // Parses the header from a buffer that needs to hold ONLY the header prefix [0, H): the body is not
    // an input, so no state of the body can change the answer. Checks magic, container version, CRC,
    // kind, subsystem versions and TOC shape (contiguous from H, known tags and codecs, no duplicates);
    // does NOT check that the sections fit a file — it is not given one.
    ResultStr<EnvelopeHeader> ReadEnvelopeHeader( std::span<const std::byte>    prefix,
                                                  const AssetHeaderReadContext& context );
    // Reads exactly the header prefix from a stream positioned at the start of an envelope.
    ResultStr<EnvelopeHeader> ReadEnvelopeHeader( std::istream& in, const AssetHeaderReadContext& context );

    // Whole-file read: header, then every section bounds-checked against the file size and verified
    // against its TOC hash. Trailing bytes after the last section are refused too.
    ResultStr<AssetEnvelope> ReadAssetEnvelope( std::span<const std::byte>    file,
                                                const AssetHeaderReadContext& context );
    ResultStr<AssetEnvelope> ReadAssetEnvelopeFile( const std::filesystem::path&  file,
                                                    const AssetHeaderReadContext& context );

    // One way of stating an asset header. A format claims a file by its leading BYTES — never by the
    // file's name — so the decoder is chosen by content, and a renamed file keeps its kind.
    class IAssetHeaderFormat
    {
    public:
        virtual ~IAssetHeaderFormat() = default;

        virtual std::string_view       Name() const                                              = 0;
        virtual bool                   Recognises( std::span<const std::byte> leading ) const    = 0;
        virtual ResultStr<AssetHeader> ReadHeader( std::istream&                 in,
                                                   const AssetHeaderReadContext& context ) const = 0;
    };

    // How many leading bytes `Recognises` is shown.
    constexpr std::size_t ASSET_HEADER_SNIFF_BYTES = 16;

    const IAssetHeaderFormat& BinaryEnvelopeHeaderFormat();

    // Every header format this build reads, in the order they are tried.
    std::span<const IAssetHeaderFormat* const> AssetHeaderFormats();

    // The one entry point for "what is this file": sniffs the leading bytes, lets the format that
    // recognises them read the header, and refuses a file no format claims.
    ResultStr<AssetHeader> ReadAssetHeader( const std::filesystem::path&  file,
                                            const AssetHeaderReadContext& context );

    // The same, for a caller walking content of every kind: std::nullopt when NO format claims the file
    // (a shader, a font - content that states no header), an error when one claims it and the header is
    // bad. The two are different answers and the registry cook must not fold the second into the first.
    ResultStr<std::optional<AssetHeader>> ReadAssetHeaderIfStated( const std::filesystem::path&  file,
                                                                   const AssetHeaderReadContext& context );

    // The Meta section's contents. Bounds are in world units (centimetres); absent for kinds that have
    // no spatial extent (a string table, an anim graph).
    struct EnvelopeBounds
    {
        std::array<float, 3> Lo = { 0.0f, 0.0f, 0.0f };
        std::array<float, 3> Hi = { 0.0f, 0.0f, 0.0f };

        bool operator==( const EnvelopeBounds& ) const = default;
    };

    struct EnvelopeMeta
    {
        std::string                   Name;
        std::vector<std::string>      Tags;
        std::optional<EnvelopeBounds> Bounds;

        bool operator==( const EnvelopeMeta& ) const = default;
    };

    std::vector<std::byte>  EncodeEnvelopeMeta( const EnvelopeMeta& meta );
    ResultStr<EnvelopeMeta> DecodeEnvelopeMeta( std::span<const std::byte> bytes );
} // namespace Common::Content
