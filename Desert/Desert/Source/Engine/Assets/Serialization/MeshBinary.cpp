#include "MeshBinary.hpp"

#include <Common/Content/MeshBinaryHeader.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <bit>
#include <climits>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Desert::Assets::Serialization
{
    namespace
    {
        // ── THE WIRE RECORDS ─────────────────────────────────────────────────────────────────────
        //
        // Plain fixed-width members and nothing else. Every one is pinned by a static_assert below,
        // because these sizes ARE the file format: a record that quietly grows by four bytes on one
        // compiler reads every following record shifted, and a mesh made of shifted floats is a mesh
        // that draws — wrongly, silently, and only on the platform that did not write the file.

        // The header's layout is Common's (MeshBinaryHeader.hpp): the content scan reads the same 64 bytes
        // without linking the engine, so there is one description for both readers.
        using FileHeader = Common::Content::MeshBinaryFileHeader;

        struct SectionRow
        {
            uint32_t Id;
            uint32_t ElementSize; // what the WRITER used; the reader checks it against its own
            uint64_t Offset;      // from file start, 8-byte aligned
            uint64_t Count;       // elements, not bytes
        };
        static_assert( sizeof( SectionRow ) == 24 );
        static_assert( sizeof( SectionRow ) == Common::Content::kMeshBinarySectionRowSize );

        struct BinSubmesh
        {
            uint32_t NameOffset; // into the Strings section
            uint32_t NameLength;
            uint32_t VertexOffset;
            uint32_t VertexCount;
            uint32_t IndexOffset;
            uint32_t IndexCount;
            uint32_t LODFirst; // into the LODRanges section
            uint32_t LODCount;
            float    Transform[16];
            float    BoundsMin[3];
            float    BoundsMax[3];
            // The material's header GUID (v3). A GUID and not HandleForGuid of it: a derived hash stored in
            // the file would tie the format to the hash function.
            uint64_t MaterialGuidHi;
            uint64_t MaterialGuidLo;
        };
        static_assert( sizeof( BinSubmesh ) == 136 );
        static_assert( offsetof( BinSubmesh, Transform ) == 32 );
        static_assert( offsetof( BinSubmesh, MaterialGuidHi ) == 120 );
        // The content scan reads the material GUIDs through Common's description of this record
        // (MeshBinaryHeader.hpp) without linking the engine; these pin the two to one layout.
        static_assert( sizeof( BinSubmesh ) == Common::Content::kMeshBinarySubmeshSizeV3 );
        static_assert( offsetof( BinSubmesh, MaterialGuidHi ) ==
                       Common::Content::kMeshBinarySubmeshMaterialGuidOffset );
        static_assert( offsetof( BinSubmesh, MaterialGuidLo ) ==
                       Common::Content::kMeshBinarySubmeshMaterialGuidOffset + 8 );

        // Versions 1 and 2 end the row in an 8-byte material NUMBER from before material GUIDs (AF7c) where
        // v3 has the GUID; the first 120 bytes are the same. Only the number 0 (no material) is read: a
        // non-zero one names its material by an identity this build cannot resolve, and SceneMigrator's mesh
        // pass is what maps it through the legacy material register.
        constexpr uint32_t kBinSubmeshSizeV2 = 128;
        constexpr uint32_t kBinSubmeshShared = 120;

        struct BinLODRange
        {
            uint64_t First; // into the LODIndices section, in TRIANGLES
            uint64_t Count;
        };
        static_assert( sizeof( BinLODRange ) == 16 );

        struct BinMorphTarget
        {
            uint32_t NameOffset;
            uint32_t NameLength;
            uint64_t DeltaPositionsFirst; // into the MorphDeltas section, in vec3s
            uint64_t DeltaPositionsCount;
            uint64_t DeltaNormalsFirst;
            uint64_t DeltaNormalsCount;
        };
        static_assert( sizeof( BinMorphTarget ) == 40 );

        struct BinVec3
        {
            float X, Y, Z;
        };
        static_assert( sizeof( BinVec3 ) == 12 );

        // THE VERTEX AND INDEX ARRAYS ARE COPIED IN BULK, which is the single reason this format is
        // fast rather than merely small: 105 317 vertices become one `memcpy`, not 105 317 parses.
        // That is only legal while the in-memory struct has exactly the layout the file does, so the
        // agreement is asserted here rather than assumed. If any of these fires, the fix is a new
        // container version with its own wire record — never a relaxed assert.
        static_assert( sizeof( glm::vec3 ) == 12 && sizeof( glm::vec2 ) == 8 );
        static_assert( sizeof( StaticVertexData ) == 56 && alignof( StaticVertexData ) == 4 );
        static_assert( sizeof( SkinnedVertexData ) == 88 && alignof( SkinnedVertexData ) == 4 );
        static_assert( sizeof( IndexData ) == 12 );
        static_assert( std::is_trivially_copyable_v<StaticVertexData> );
        static_assert( std::is_trivially_copyable_v<SkinnedVertexData> );
        static_assert( std::is_trivially_copyable_v<IndexData> );
        static_assert( std::is_trivially_copyable_v<glm::vec3> );

        constexpr uint32_t kByteOrderTag = Common::Content::kMeshBinaryByteOrderTag;

        // THE WRITER REFUSES TO EXIST ON A HOST IT COULD NOT READ ITS OWN FILE BACK ON. The tag above
        // lets the READER name a foreign byte order; these two make the BUILD name it, which is the
        // only place it can be answered, because this file writes raw object representations and a
        // big-endian build would emit a file every shipping target then rejects. Both shipping targets
        // are little-endian with 8-bit bytes, so neither assert can fire today — and a port that makes
        // one fire is told at compile time instead of at a customer's load.
        static_assert( std::endian::native == std::endian::little,
                       "the cooked mesh container is little-endian; see kByteOrderTag" );
        static_assert( CHAR_BIT == 8, "the container's sizes are in 8-bit bytes" );

        enum SectionId : uint32_t
        {
            SecStaticVertices = 1,
            SecSkinnedVertices,
            SecIndices,
            SecSubmeshes,
            SecLODRanges,
            SecLODIndices,
            SecMorphTargets,
            SecMorphDeltas,
            SecStrings,
            SecPolyGroups, // version 2
            SecCount_      // one past the last id; also the number of rows in the table
        };
        static_assert( SecSubmeshes == Common::Content::kMeshBinarySubmeshSectionId );
        constexpr uint32_t kSectionCount = SecCount_ - 1;
        // Version 1 is version 2 without its last section (MeshBinary.hpp); the table is otherwise identical.
        constexpr uint32_t kSectionCountV1 = SecStrings;

        uint32_t SectionCountOf( const uint32_t version )
        {
            return version == 1 ? kSectionCountV1 : kSectionCount;
        }

        constexpr uint32_t kFlagIsSkinned            = Common::Content::kMeshFlagIsSkinned;
        constexpr uint32_t kFlagHasSkeletonSignature = Common::Content::kMeshFlagHasSkeletonSignature;

        /// The element size version 1 declares for each section, indexed by id. A reader that finds a
        /// different number in the file stops there: see the header's note on type width.
        uint32_t ExpectedElementSize( const uint32_t id, const uint32_t version )
        {
            switch ( id )
            {
                case SecStaticVertices:
                    return sizeof( StaticVertexData );
                case SecSkinnedVertices:
                    return sizeof( SkinnedVertexData );
                case SecIndices:
                    return sizeof( IndexData );
                case SecSubmeshes:
                    return version >= 3 ? static_cast<uint32_t>( sizeof( BinSubmesh ) ) : kBinSubmeshSizeV2;
                case SecLODRanges:
                    return sizeof( BinLODRange );
                case SecLODIndices:
                    return sizeof( IndexData );
                case SecMorphTargets:
                    return sizeof( BinMorphTarget );
                case SecMorphDeltas:
                    return sizeof( BinVec3 );
                case SecStrings:
                    return 1;
                case SecPolyGroups:
                    return sizeof( int32_t );
                default:
                    return 0;
            }
        }

        const char* SectionName( const uint32_t id )
        {
            switch ( id )
            {
                case SecStaticVertices:
                    return "StaticVertices";
                case SecSkinnedVertices:
                    return "SkinnedVertices";
                case SecIndices:
                    return "Indices";
                case SecSubmeshes:
                    return "Submeshes";
                case SecLODRanges:
                    return "LODRanges";
                case SecLODIndices:
                    return "LODIndices";
                case SecMorphTargets:
                    return "MorphTargets";
                case SecMorphDeltas:
                    return "MorphDeltas";
                case SecStrings:
                    return "Strings";
                case SecPolyGroups:
                    return "PolyGroups";
                default:
                    return "<unknown>";
            }
        }

        void Append( std::string& out, const void* bytes, const size_t count )
        {
            if ( count == 0 )
                return;
            out.append( static_cast<const char*>( bytes ), count );
        }

        /// Pad @p out to the next 8-byte boundary. Sections start aligned so that a future reader is
        /// free to map the file instead of copying it; nothing here depends on it, and every read in
        /// this file goes through `memcpy`, which does not.
        void PadToEight( std::string& out )
        {
            while ( ( out.size() % 8 ) != 0 )
                out.push_back( '\0' );
        }
    } // namespace

    bool LooksLikeMeshBinary( const std::string_view bytes )
    {
        return bytes.size() >= sizeof( kMeshBinaryMagic ) &&
               std::memcmp( bytes.data(), kMeshBinaryMagic, sizeof( kMeshBinaryMagic ) ) == 0;
    }

    std::string EncodeMeshBinary( const MeshAssetData& data )
    {
        // The jagged parts are flattened first, because the fixed records reference them by index and
        // cannot be written until those indices exist.
        std::string              strings;
        std::vector<BinLODRange> lodRanges;
        std::vector<IndexData>   lodIndices;
        std::vector<BinVec3>     morphDeltas;

        const auto InternString = []( std::string& blob, const std::string& text ) -> std::pair<uint32_t, uint32_t>
        {
            const auto offset = static_cast<uint32_t>( blob.size() );
            blob += text;
            return { offset, static_cast<uint32_t>( text.size() ) };
        };

        std::vector<BinSubmesh> submeshes;
        submeshes.reserve( data.Submeshes.size() );
        for ( const SubmeshData& s : data.Submeshes )
        {
            BinSubmesh rec{};
            const auto name  = InternString( strings, s.Name );
            rec.NameOffset   = name.first;
            rec.NameLength   = name.second;
            rec.VertexOffset = s.VertexOffset;
            rec.VertexCount  = s.VertexCount;
            rec.IndexOffset  = s.IndexOffset;
            rec.IndexCount   = s.IndexCount;
            rec.LODFirst     = static_cast<uint32_t>( lodRanges.size() );
            rec.LODCount     = static_cast<uint32_t>( s.LODs.size() );
            std::memcpy( rec.Transform, glm::value_ptr( s.Transform ), sizeof( rec.Transform ) );
            std::memcpy( rec.BoundsMin, glm::value_ptr( s.BoundingBox.Min ), sizeof( rec.BoundsMin ) );
            std::memcpy( rec.BoundsMax, glm::value_ptr( s.BoundingBox.Max ), sizeof( rec.BoundsMax ) );
            rec.MaterialGuidHi = s.MaterialGuid.Hi;
            rec.MaterialGuidLo = s.MaterialGuid.Lo;

            for ( const std::vector<IndexData>& level : s.LODs )
            {
                lodRanges.push_back( BinLODRange{ static_cast<uint64_t>( lodIndices.size() ),
                                                  static_cast<uint64_t>( level.size() ) } );
                lodIndices.insert( lodIndices.end(), level.begin(), level.end() );
            }
            submeshes.push_back( rec );
        }

        std::vector<BinMorphTarget> morphTargets;
        morphTargets.reserve( data.MorphTargets.size() );
        for ( const MorphTargetData& mt : data.MorphTargets )
        {
            BinMorphTarget rec{};
            const auto     name = InternString( strings, mt.Name );
            rec.NameOffset      = name.first;
            rec.NameLength      = name.second;

            rec.DeltaPositionsFirst = morphDeltas.size();
            rec.DeltaPositionsCount = mt.DeltaPositions.size();
            for ( const glm::vec3& d : mt.DeltaPositions )
                morphDeltas.push_back( BinVec3{ d.x, d.y, d.z } );

            rec.DeltaNormalsFirst = morphDeltas.size();
            rec.DeltaNormalsCount = mt.DeltaNormals.size();
            for ( const glm::vec3& d : mt.DeltaNormals )
                morphDeltas.push_back( BinVec3{ d.x, d.y, d.z } );

            morphTargets.push_back( rec );
        }

        // A VIEW AND NOT A POINTER, and the census that made it one is right: `Desert/Tests/Engine/
        // PointerOwnership` refuses a raw pointer member that cannot say who destroys the pointee and
        // what keeps it alive. Here both answers are "the vectors above, for the length of this
        // function", and a `string_view` says so in the type instead of in a comment — it carries the
        // length with the address, so nothing downstream can read past the run it describes.
        struct Payload
        {
            uint32_t                   Id;
            std::span<const std::byte> Bytes;
            uint64_t                   Count;
        };
        const auto AsBytes = []( const auto* first, const size_t count ) -> std::span<const std::byte>
        {
            if ( count == 0 )
                return {}; // an empty vector's data() may be null, and a span over null is not one
            // `std::as_bytes` and not a cast: it is the one spelling of "view this object as its own
            // bytes" that is neither a reinterpret_cast nor a trip through `void*`, both of which the
            // analyser rejects — and it requires the element type to be trivially copyable, which is
            // the very property the static_asserts above pin.
            return std::as_bytes( std::span( first, count ) );
        };
        const Payload payloads[kSectionCount] = {
             { SecStaticVertices, AsBytes( data.StaticVertices.data(), data.StaticVertices.size() ),
               data.StaticVertices.size() },
             { SecSkinnedVertices, AsBytes( data.SkinnedVertices.data(), data.SkinnedVertices.size() ),
               data.SkinnedVertices.size() },
             { SecIndices, AsBytes( data.Indices.data(), data.Indices.size() ), data.Indices.size() },
             { SecSubmeshes, AsBytes( submeshes.data(), submeshes.size() ), submeshes.size() },
             { SecLODRanges, AsBytes( lodRanges.data(), lodRanges.size() ), lodRanges.size() },
             { SecLODIndices, AsBytes( lodIndices.data(), lodIndices.size() ), lodIndices.size() },
             { SecMorphTargets, AsBytes( morphTargets.data(), morphTargets.size() ), morphTargets.size() },
             { SecMorphDeltas, AsBytes( morphDeltas.data(), morphDeltas.size() ), morphDeltas.size() },
             { SecStrings, AsBytes( strings.data(), strings.size() ), strings.size() },
             { SecPolyGroups, AsBytes( data.PolyGroups.data(), data.PolyGroups.size() ), data.PolyGroups.size() },
        };

        // Offsets are computed before anything is written, because the table sits in front of the
        // payloads it describes.
        SectionRow     table[kSectionCount] = {};
        const uint64_t tableEnd             = Common::Content::kMeshBinaryPrefixV3 + sizeof( table );
        uint64_t       at                   = tableEnd;
        static_assert( ( Common::Content::kMeshBinaryPrefixV3 + sizeof( SectionRow ) * kSectionCount ) % 8 == 0,
                       "the first section must start 8-byte aligned without padding after the table" );
        for ( uint32_t i = 0; i < kSectionCount; ++i )
        {
            const uint32_t elementSize =
                 ExpectedElementSize( payloads[i].Id, Common::Content::kMeshBinaryVersion );
            table[i].Id                = payloads[i].Id;
            table[i].ElementSize       = elementSize;
            table[i].Offset            = at;
            table[i].Count             = payloads[i].Count;
            at += elementSize * payloads[i].Count;
            at = ( at + 7u ) & ~static_cast<uint64_t>( 7u );
        }

        FileHeader header{};
        std::memcpy( header.Magic, kMeshBinaryMagic, sizeof( header.Magic ) );
        header.ByteOrder    = kByteOrderTag;
        header.Version      = kMeshBinaryVersion;
        header.FileSize     = at;
        header.SectionCount = kSectionCount;
        header.Flags        = ( data.IsSkinned ? kFlagIsSkinned : 0u ) |
                       ( data.SkeletonSignature.has_value() ? kFlagHasSkeletonSignature : 0u );
        header.SkeletonSignature = data.SkeletonSignature.value_or( 0 );
        // The box goes into the header so the content scan learns it from 64 bytes, without the body.
        Common::Content::StateMeshBounds( header, MeshDataBounds( data ) );

        std::string out;
        out.reserve( static_cast<size_t>( at ) );
        Append( out, &header, sizeof( header ) );
        Append( out, &data.Guid.Hi, sizeof( data.Guid.Hi ) );
        Append( out, &data.Guid.Lo, sizeof( data.Guid.Lo ) );
        Append( out, table, sizeof( table ) );
        for ( const Payload& payload : payloads )
        {
            PadToEight( out );
            // The view's own length rather than a recomputed product: the two must agree, and the one
            // that cannot drift is the one the view was built with.
            Append( out, payload.Bytes.data(), payload.Bytes.size() );
        }
        PadToEight( out );
        return out;
    }

    Common::ResultStr<MeshAssetData> DecodeMeshBinary( const std::string_view bytes,
                                                       const std::string_view whatFor )
    {
        const std::string who( whatFor );

        if ( bytes.size() < sizeof( FileHeader ) )
        {
            return Common::MakeFormattedError<MeshAssetData>(
                 "'{}' is {} bytes, which is shorter than the {}-byte cooked-mesh header — the file is "
                 "truncated or is not a cooked mesh at all.",
                 who, bytes.size(), sizeof( FileHeader ) );
        }

        FileHeader header{};
        std::memcpy( &header, bytes.data(), sizeof( header ) );

        if ( std::memcmp( header.Magic, kMeshBinaryMagic, sizeof( header.Magic ) ) != 0 )
            return Common::MakeFormattedError<MeshAssetData>( "'{}' does not carry the cooked-mesh magic.", who );

        if ( header.ByteOrder != kByteOrderTag )
        {
            return Common::MakeFormattedError<MeshAssetData>(
                 "'{}' was written by a host of the opposite byte order (tag {:#010x}, this host reads "
                 "{:#010x}). Cooked meshes are little-endian; re-cook the mesh on the target host.",
                 who, header.ByteOrder, kByteOrderTag );
        }

        if ( header.Version < 1 || header.Version > kMeshBinaryVersion )
        {
            return Common::MakeFormattedError<MeshAssetData>(
                 "'{}' is cooked-mesh format version {}, this build reads version {}. Re-cook it "
                 "(Assets > Rebuild Cooked Assets).",
                 who, header.Version, kMeshBinaryVersion );
        }

        // THE TRUNCATION CHECK, AND THE REASON `FileSize` IS IN THE HEADER AT ALL. A file that stops
        // early otherwise reads as a perfectly valid mesh with whatever happened to fit.
        if ( header.FileSize != bytes.size() )
        {
            return Common::MakeFormattedError<MeshAssetData>(
                 "'{}' declares {} bytes and {} are present — the file is truncated or has been "
                 "appended to. Nothing was loaded.",
                 who, header.FileSize, bytes.size() );
        }

        const uint32_t sectionCount = SectionCountOf( header.Version );
        if ( header.SectionCount != sectionCount )
        {
            return Common::MakeFormattedError<MeshAssetData>(
                 "'{}' declares {} sections, version {} has exactly {}.", who, header.SectionCount, header.Version,
                 sectionCount );
        }
        const std::size_t prefixSize = Common::Content::MeshHeaderSize( header.Version );
        if ( bytes.size() < prefixSize + sizeof( SectionRow ) * sectionCount )
        {
            return Common::MakeFormattedError<MeshAssetData>(
                 "'{}' is {} bytes, shorter than its own header and {}-row section table.", who, bytes.size(),
                 sectionCount );
        }

        // Rows past the version's count stay zero, and read as empty sections below.
        SectionRow table[kSectionCount] = {};
        std::memcpy( table, bytes.data() + prefixSize, sizeof( SectionRow ) * sectionCount );

        // THE LAYOUT IS DERIVED, NOT TRUSTED. Version 1 packs the sections in table order, each starting
        // at the next 8-byte boundary after the last, so the offset a row SHOULD carry follows from the
        // counts alone — and a row that carries a different one is refused rather than followed.
        //
        // Checking only that an offset lies inside the file is not enough, and this is not theoretical:
        // flipping one byte of the first row's offset moved it 96 bytes forward, which was still inside
        // the file, still 8-aligned and still left room for the declared count. The decode succeeded and
        // handed back a mesh of shifted floats. That is the silent wrong answer §1.4 forbids, produced
        // by a single corrupt byte.
        uint64_t expectedOffset = prefixSize + sizeof( SectionRow ) * sectionCount;
        for ( uint32_t i = 0; i < sectionCount; ++i )
        {
            const SectionRow& row      = table[i];
            const uint32_t    expectId = i + 1; // ids are 1..sectionCount in table order, in every version
            if ( row.Id != expectId )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' section table row {} names id {} where version {} puts {} ({}).", who, i, row.Id,
                     header.Version, expectId, SectionName( expectId ) );
            }

            const uint32_t expectSize = ExpectedElementSize( row.Id, header.Version );
            if ( row.ElementSize != expectSize )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' section '{}' has {}-byte elements, this build reads {}-byte ones. The file was "
                     "written by a build whose record layout differs; re-cook it.",
                     who, SectionName( row.Id ), row.ElementSize, expectSize );
            }

            if ( row.Offset != expectedOffset )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' section '{}' starts at {}, and the counts before it put it at {}. The section "
                     "table does not describe this file.",
                     who, SectionName( row.Id ), row.Offset, expectedOffset );
            }
            if ( row.Offset > header.FileSize )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' section '{}' starts at {}, past the end of a {}-byte file.", who, SectionName( row.Id ),
                     row.Offset, header.FileSize );
            }

            // Division rather than multiplication, so a count chosen to overflow the product cannot
            // pass the bound it was meant to defeat.
            if ( row.Count > ( header.FileSize - row.Offset ) / expectSize )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' section '{}' claims {} elements of {} bytes from offset {}, which runs past the "
                     "end of a {}-byte file.",
                     who, SectionName( row.Id ), row.Count, expectSize, row.Offset, header.FileSize );
            }

            expectedOffset += expectSize * row.Count;
            expectedOffset = ( expectedOffset + 7u ) & ~static_cast<uint64_t>( 7u );
        }

        // And the declared size must be exactly where the last section ends: trailing bytes nobody reads
        // would be a file the writer could not have produced.
        if ( expectedOffset != header.FileSize )
        {
            return Common::MakeFormattedError<MeshAssetData>(
                 "'{}' declares {} bytes and its own section table accounts for {}.", who, header.FileSize,
                 expectedOffset );
        }

        const auto At = [&]( const uint32_t id ) { return bytes.data() + table[id - 1].Offset; };
        const auto N  = [&]( const uint32_t id ) { return static_cast<size_t>( table[id - 1].Count ); };

        MeshAssetData data;
        data.IsSkinned = ( header.Flags & kFlagIsSkinned ) != 0;
        if ( const auto guid = Common::Content::ReadMeshHeaderGuid( bytes ) )
            data.Guid = *guid;
        if ( ( header.Flags & kFlagHasSkeletonSignature ) != 0 )
            data.SkeletonSignature = header.SkeletonSignature;

        data.StaticVertices.resize( N( SecStaticVertices ) );
        std::memcpy( data.StaticVertices.data(), At( SecStaticVertices ),
                     N( SecStaticVertices ) * sizeof( StaticVertexData ) );

        data.SkinnedVertices.resize( N( SecSkinnedVertices ) );
        std::memcpy( data.SkinnedVertices.data(), At( SecSkinnedVertices ),
                     N( SecSkinnedVertices ) * sizeof( SkinnedVertexData ) );

        data.Indices.resize( N( SecIndices ) );
        std::memcpy( data.Indices.data(), At( SecIndices ), N( SecIndices ) * sizeof( IndexData ) );

        const std::string_view strings( At( SecStrings ), N( SecStrings ) );

        // EVERY CROSS-SECTION REFERENCE IS CHECKED BEFORE IT IS FOLLOWED. The bounds above prove each
        // section lies inside the file; they say nothing about a submesh pointing at LOD range 900 of
        // 3, and following that index is a read of whatever is next in memory.
        const auto Substring = [&]( const uint32_t offset, const uint32_t length, std::string& out ) -> bool
        {
            if ( static_cast<uint64_t>( offset ) + length > strings.size() )
                return false;
            out.assign( strings.data() + offset, length );
            return true;
        };

        data.Submeshes.resize( N( SecSubmeshes ) );
        for ( size_t i = 0; i < data.Submeshes.size(); ++i )
        {
            BinSubmesh  rec{};
            const char* row = At( SecSubmeshes ) + i * ExpectedElementSize( SecSubmeshes, header.Version );
            if ( header.Version >= 3 )
            {
                std::memcpy( &rec, row, sizeof( rec ) );
            }
            else
            {
                std::memcpy( &rec, row, kBinSubmeshShared );
                uint64_t materialNumber = 0;
                std::memcpy( &materialNumber, row + kBinSubmeshShared, sizeof( materialNumber ) );
                if ( materialNumber != 0 )
                {
                    return Common::MakeFormattedError<MeshAssetData>(
                         "'{}' is cooked-mesh format version {}, and submesh {} names its material by the "
                         "pre-GUID number {}. Run SceneMigrator: its mesh pass maps the number to the material's "
                         "GUID through the legacy material register.",
                         who, header.Version, i, materialNumber );
                }
            }

            SubmeshData& out = data.Submeshes[i];
            if ( !Substring( rec.NameOffset, rec.NameLength, out.Name ) )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' submesh {} names bytes [{}, {}) of a {}-byte string section.", who, i, rec.NameOffset,
                     static_cast<uint64_t>( rec.NameOffset ) + rec.NameLength, strings.size() );
            }
            // THE SUBMESH'S OWN RANGES, against the arrays they index. B11 left this out and called it
            // asset semantics on the grounds that the JSON form did not check it either — but the JSON
            // form could not produce this failure. There, a corrupted file stopped being parseable
            // text; here it stays perfectly valid bytes and the mesh loads with a range that walks off
            // the end of the vertex or index array at DRAW time, in the renderer, with the file long
            // since closed. The decoder is the last place that still knows both numbers.
            //
            // Written as `count > size - offset` rather than `offset + count > size`: both operands are
            // uint32_t widened to uint64_t above for the same reason, and the subtraction form cannot
            // overflow at all once `offset <= size` is established.
            // THE TWO NUMBERS ARE IN DIFFERENT UNITS, and the first draft of this check got it wrong in
            // exactly the way the check exists to catch. `SecIndices` counts `IndexData` records, and an
            // `IndexData` is a FACE — three `uint32_t`. `SubmeshData::IndexCount` counts INDICES. So the
            // shipped 24-face probe was refused for "names indices [0, 36) of 24" while being perfectly
            // valid: 24 faces are 72 indices and its two submeshes hold 36 each. Convert, once, here.
            const uint64_t indexCount = N( SecIndices ) * 3;

            // Static and skinned vertices are summed because a file carries one array or the other and
            // the empty one contributes zero; summing keeps this from having to know which kind it is.
            const uint64_t vertexCount = N( SecStaticVertices ) + N( SecSkinnedVertices );
            if ( rec.VertexOffset > vertexCount || rec.VertexCount > vertexCount - rec.VertexOffset )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' submesh {} names vertices [{}, {}) of {}.", who, i, rec.VertexOffset,
                     static_cast<uint64_t>( rec.VertexOffset ) + rec.VertexCount, vertexCount );
            }
            if ( rec.IndexOffset > indexCount || rec.IndexCount > indexCount - rec.IndexOffset )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' submesh {} names indices [{}, {}) of {} ({} faces).", who, i, rec.IndexOffset,
                     static_cast<uint64_t>( rec.IndexOffset ) + rec.IndexCount, indexCount, N( SecIndices ) );
            }

            out.VertexOffset = rec.VertexOffset;
            out.VertexCount  = rec.VertexCount;
            out.IndexOffset  = rec.IndexOffset;
            out.IndexCount   = rec.IndexCount;
            std::memcpy( glm::value_ptr( out.Transform ), rec.Transform, sizeof( rec.Transform ) );
            std::memcpy( glm::value_ptr( out.BoundingBox.Min ), rec.BoundsMin, sizeof( rec.BoundsMin ) );
            std::memcpy( glm::value_ptr( out.BoundingBox.Max ), rec.BoundsMax, sizeof( rec.BoundsMax ) );
            out.MaterialGuid = Common::Content::AssetGuid{ rec.MaterialGuidHi, rec.MaterialGuidLo };

            if ( static_cast<uint64_t>( rec.LODFirst ) + rec.LODCount > N( SecLODRanges ) )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' submesh {} names LOD ranges [{}, {}) of {}.", who, i, rec.LODFirst,
                     static_cast<uint64_t>( rec.LODFirst ) + rec.LODCount, N( SecLODRanges ) );
            }

            out.LODs.resize( rec.LODCount );
            for ( uint32_t l = 0; l < rec.LODCount; ++l )
            {
                BinLODRange range{};
                std::memcpy( &range, At( SecLODRanges ) + ( rec.LODFirst + l ) * sizeof( BinLODRange ),
                             sizeof( range ) );
                if ( range.First > N( SecLODIndices ) || range.Count > N( SecLODIndices ) - range.First )
                {
                    return Common::MakeFormattedError<MeshAssetData>(
                         "'{}' submesh {} LOD {} names triangles [{}, {}) of {}.", who, i, l, range.First,
                         range.First + range.Count, N( SecLODIndices ) );
                }
                out.LODs[l].resize( static_cast<size_t>( range.Count ) );
                std::memcpy( out.LODs[l].data(), At( SecLODIndices ) + range.First * sizeof( IndexData ),
                             static_cast<size_t>( range.Count ) * sizeof( IndexData ) );
            }
        }

        // One per face or none: a partial list would pair groups with the wrong triangles past its end.
        if ( N( SecPolyGroups ) != 0 && N( SecPolyGroups ) != N( SecIndices ) )
        {
            return Common::MakeFormattedError<MeshAssetData>( "'{}' carries {} polygroups for {} faces.", who,
                                                              N( SecPolyGroups ), N( SecIndices ) );
        }
        data.PolyGroups.resize( N( SecPolyGroups ) );
        if ( !data.PolyGroups.empty() )
            std::memcpy( data.PolyGroups.data(), At( SecPolyGroups ), N( SecPolyGroups ) * sizeof( int32_t ) );

        data.MorphTargets.resize( N( SecMorphTargets ) );
        for ( size_t i = 0; i < data.MorphTargets.size(); ++i )
        {
            BinMorphTarget rec{};
            std::memcpy( &rec, At( SecMorphTargets ) + i * sizeof( BinMorphTarget ), sizeof( rec ) );

            MorphTargetData& out = data.MorphTargets[i];
            if ( !Substring( rec.NameOffset, rec.NameLength, out.Name ) )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' blendshape {} names bytes [{}, {}) of a {}-byte string section.", who, i,
                     rec.NameOffset, static_cast<uint64_t>( rec.NameOffset ) + rec.NameLength, strings.size() );
            }

            const auto ReadDeltas = [&]( const uint64_t first, const uint64_t count,
                                         std::vector<glm::vec3>& into ) -> bool
            {
                if ( first > N( SecMorphDeltas ) || count > N( SecMorphDeltas ) - first )
                    return false;
                into.resize( static_cast<size_t>( count ) );
                std::memcpy( into.data(), At( SecMorphDeltas ) + first * sizeof( BinVec3 ),
                             static_cast<size_t>( count ) * sizeof( BinVec3 ) );
                return true;
            };

            if ( !ReadDeltas( rec.DeltaPositionsFirst, rec.DeltaPositionsCount, out.DeltaPositions ) ||
                 !ReadDeltas( rec.DeltaNormalsFirst, rec.DeltaNormalsCount, out.DeltaNormals ) )
            {
                return Common::MakeFormattedError<MeshAssetData>(
                     "'{}' blendshape {} names deltas outside the {}-element delta section.", who, i,
                     N( SecMorphDeltas ) );
            }
        }

        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<MeshAssetData> ReadMeshAssetData( const std::string_view bytes,
                                                        const std::string_view whatFor )
    {
        if ( LooksLikeMeshBinary( bytes ) )
            return DecodeMeshBinary( bytes, whatFor );

        // THE JSON ARM IS GONE, BY OWNER DECISION 2026-09-22: "if the cook is deprecated the user just
        // deletes it — we are still in development, there is nothing to migrate".
        //
        // The argument is stronger than the timing. `Cooked/` is DERIVED data: gitignored,
        // machine-local, and reproducible for free by re-cooking. Keeping a second reader alive to
        // migrate something that can be regenerated is a second source of truth bought with nothing —
        // and a second reader for one structure is exactly what this container replaced. Every cooked
        // mesh this repository TRACKS carries the magic, so no clone loses a file it cannot rebuild.
        //
        // The refusal therefore names the remedy rather than the parse error: a stale cook is not a
        // corrupt file and must not read like one.
        return Common::MakeFormattedError<MeshAssetData>(
             "'{}' does not carry the cooked-mesh magic, so it predates the binary container. Cooked "
             "content is derived and is not migrated: delete it and cook again "
             "(`cd Editor && ../build/Bin/Debug/AssetRegistryTool cook Desert.deproj`, or re-import the "
             "source mesh).",
             std::string( whatFor ) );
    }
} // namespace Desert::Assets::Serialization
