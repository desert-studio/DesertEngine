#pragma once

// THE COOKED TEXTURE CONTAINER — why a `.tex` carries pixels now, and what it has to be able to refuse.
//
// WHAT IT REPLACED, MEASURED IN THIS TREE. The cooked `T_Checker.tex` was 133 bytes of JSON
// naming a PNG and carrying not one pixel. Every load re-decoded that PNG and then built the mip chain
// on the GPU with `vkCmdBlitImage`. Neither half survives contact with the next step of
// `Docs/World/PROGRAMME.md` §5: a block-compressed format has `blitDst=0` on this device (re-measured
// by `Docs/Textures/bcprobe.c`), so the GPU cannot generate its chain, and PNG is a single entropy-coded
// stream, so "read only mip 4" is not slow, it is INEXPRESSIBLE. Mips have to be in the file before
// either can happen, which is why this container exists before the encoder does.
//
// ── THE FORMAT ───────────────────────────────────────────────────────────────────────────────────
//
//   [Header, 128 B][LevelRow, LevelCount * LayerCount * 24 B][source key bytes][pad][levels, SMALLEST FIRST]
//
// The layout is `Docs/Textures/T3_FORMAT_PLAN.md` §5c, which is binding, and two of its decisions are
// not cosmetic:
//
//  * A LEVEL IS A LEVEL OF EVERY LAYER (v3). The table has one row per (mip level, array layer), and a
//    cube is six layers of a square face. The row index is `level * LayerCount + layer` and nothing in
//    the file is addressed any other way. Faces are stored TOGETHER, inside their level, so the two
//    bullets below keep meaning what they said: the resident tail of a cube is still one short read,
//    and "read only mip 4" still means one seek for each of the six faces that make it up, next to
//    each other. See `TextureKind` for why the layer count alone is not allowed to say "cube".
//  * THE LEVELS ARE STORED SMALLEST FIRST. The resident tail — the levels at 16x16 and below that a
//    streaming build keeps in memory for every texture, so that "an object on screen with no texture"
//    stops being a possible state — is then a CONTIGUOUS PREFIX of the file, together with the header
//    and the table. One read of a few hundred bytes gets all of it. Stored in image order the tail
//    would sit at the END of every file and cost a seek per texture, for ever. Mip 0 loses nothing:
//    it is read by offset, not sequentially.
//  * EVERY LEVEL CARRIES ITS OWN CODEC (v2). A level is compressed on its own or not at all, so the
//    property the first bullet buys survives compression: the resident tail is still a short read of
//    a contiguous prefix, and mip 4 is still one seek and one decode of mip 4's bytes. Compressing
//    the FILE — which is what an archive does to an entry it likes the look of — gives the same
//    number of bytes on the wire and takes that away, because there is then no such thing as a level
//    to read. This is the shape KTX2 calls supercompression and the shape UE's Oodle chunks have;
//    the argument for it here is measured rather than borrowed, and it is in `EncodeTextureBinary`.
//  * EVERY LEVEL STARTS 16-BYTE ALIGNED. That is what lets a level be `memcpy`ed into mapped memory
//    without an unaligned access, and it keeps every level's `VkBufferImageCopy::bufferOffset` a legal
//    multiple of the texel block size for both an RGBA8 and an RGBA32F payload. The price is under
//    fifteen bytes per level — below 200 bytes for a whole texture.
//
// The whole payload run is one `memcpy` into one staging buffer and one copy region per level, which
// is what makes the upload one allocation rather than one per level.
//
// ── WINDOWS IS THE TARGET AND THIS MACHINE IS NOT WINDOWS ────────────────────────────────────────
//
// The same three hazards `MeshBinary.hpp` names, answered the same way, because they are properties of
// binary files and not of meshes:
//
//  * TYPE WIDTH IS IN THE FILE, in the sense that every wire struct is `static_assert`ed to an exact
//    size and every field is a fixed-width integer. A record that pads differently under MSVC stops
//    the BUILD, not a customer's load.
//  * BYTE ORDER IS IN THE FILE. `ByteOrder` is the integer 0x04030201, which reads back as 0x01020304
//    on a big-endian host and is refused by name. It is not byte-swapped: both shipping targets are
//    little-endian, and a swapper is a branch neither this machine nor CI could ever execute.
//  * NOTHING WITH A COMPILER-DEPENDENT LAYOUT CROSSES THE BOUNDARY — no `glm`, no `std::array`, no
//    enum written at its own width. `Format` travels as a `uint32_t`.
//
// ── THE FORMAT MUST BE ABLE TO EXPRESS ITS OWN FAILURE ───────────────────────────────────────────
//
// `FileSize` is in the header for one reason: a file whose bytes stop early would otherwise decode as a
// perfectly good texture with whatever happened to fit — the last level silently short, or missing, and
// a sampler reading whatever the allocator left there. The declared size is compared against the bytes
// in hand and the refusal names both numbers. A texture with zero levels is NOT the same thing and is
// refused separately, by its own sentence: an empty container is a cook that produced nothing.
//
// THERE IS NO PAYLOAD CHECKSUM, DELIBERATELY, and the argument is `MeshBinary.hpp`'s: hashing every
// byte on every load is the second full pass `PROGRAMME.md` §6 books as 80 % of the archive read path.
// `SourceContentHash` below is a different question — see the next paragraph — and is never computed
// over the payload at load time.
//
// ── "IS THIS COOK STALE?" IS A COMPUTABLE FACT HERE, NOT A GUESS ABOUT TIMESTAMPS ────────────────
//
// `SourceContentHash` is CRC-32C of the SOURCE IMAGE FILE's bytes, recorded by the cook that read them.
// The cooker re-hashes the source and re-cooks when the number disagrees. That replaces an mtime
// comparison, and the file this container lives in recorded why that mattered before the field existed:
// git sets mtimes to checkout time, so a committed stale cook was "up to date" for ever by
// construction. An artist who edits a PNG now gets a re-cook because the BYTES changed, which is the
// question everybody was actually asking.
//
// It is NOT the asset registry's Identity column (`ContentRegistry.hpp`): that column carries an asset
// HANDLE — the id a `.demat` resolves against — and answers "which asset is this", not "which bytes was
// it made from". Two questions, two numbers; merging them would make a re-export of the same image look
// like a different asset.

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets::Serialization
{
    /// The container's own version sequence, and nothing else's. It is not `kSceneVersion` (22), not
    /// `kMeshBinaryVersion` (1) and not `kAnimationVersion` (3): a `.desce` names a texture by path and
    /// carries none of its bytes, so a scene written yesterday opens a texture cooked today without
    /// either file knowing about the other's number.
    ///
    /// There is no version 0. The JSON manifest this container replaced is recognised by the ABSENCE of
    /// the magic and is REFUSED, not migrated — cooked content is derived data, and the remedy for a
    /// stale cook is to cook again. That is the same call `MeshBinary` made by owner decision
    /// 2026-09-22 ("if the cook is deprecated the user just deletes it").
    ///
    ///   v1  uncompressed levels; a 16-byte level row of offset/size/pitch.
    ///   v2  + per-level `StoredSize` and `Codec` (a 24-byte row), and a header column for the stored
    ///       payload total. A v1 file is REFUSED BY VERSION, with the remedy, exactly as the paragraph
    ///       above says: there is no migration path and there must not be one.
    ///   v3  + `LayerCount` and `Kind`, so the container can say how many images a level is made of and
    ///       what they mean together. Before this the header read `Width; Height; Format; LevelCount`
    ///       and that was ALL: a cube was not merely unsupported, it was INEXPRESSIBLE, and every size
    ///       in the reader was `width * height * bytes-per-pixel` for exactly one image. It also gives
    ///       `EncoderHash` a meaning (the settings a derived asset was made with) instead of refusing
    ///       it, and makes the full-chain requirement a rule about 2D textures rather than about every
    ///       file — a baked cube's level count is a measured choice, not an obligation.
    ///
    /// WHY THE BUMP COULD NOT BE AVOIDED, since a version that can be dodged should be. The level row
    /// is a fixed-width record read by `memcpy` at a computed stride, so a v1 file read with a v2 row
    /// does not fail — it reads every level after the first from the wrong place, which is the failure
    /// mode this whole container was written to make impossible. A `Flags` bit would only move the
    /// question, because the row's SIZE is what changes.
    ///
    /// WHY v3 COULD NOT BE A FLAG EITHER, and v2's own note is why: the level row is read by `memcpy`
    /// at a computed stride, and the number of ROWS is now `LevelCount * LayerCount`. A v2 reader
    /// handed a cube would read six times too few rows and then chain every offset from the wrong
    /// place; a v3 reader handed a v2 file would read `LayerCount` out of a reserved word. The reserve
    /// is zero in every v2 file ever written, so that second direction would decode as a texture with
    /// ZERO layers rather than fail — which is precisely the silent shape the version gate exists for.
    ///
    /// v2 IS REFUSED BY VERSION, WITH ITS REMEDY, exactly like v1. Fourteen sources and one committed
    /// `.tex` is the whole cost of re-cooking, and it was paid when this landed.
    inline constexpr uint32_t kTextureBinaryVersion = 3;

    /// "DESTTEXT". Eight ASCII bytes, so the sequence on disk is the same whatever the host's word
    /// order — a magic written as an integer would itself need a byte-order rule in order to be read.
    inline constexpr char kTextureBinaryMagic[8] = { 'D', 'E', 'S', 'T', 'T', 'E', 'X', '\0' };

    /// Every level begins on this boundary inside the file. See the header note.
    inline constexpr uint64_t kTextureLevelAlignment = 16;

    /// The header is a FIXED SIZE AT A FIXED OFFSET and answers every question the registry, the content
    /// browser and reference resolution ask, without a forward reference to anything: extent, format,
    /// level count, source hash, identity. `TextureAsset::LoadFromFile` reads this prefix and no more,
    /// which is what keeps `TextureService`'s "cheap: reads the metadata, not pixels" true now that the
    /// file is megabytes rather than bytes. `HeaderSize` is in the header itself so a reader can step
    /// over a header that a later version grew.
    inline constexpr std::size_t kTextureBinaryHeaderSize = 128;

    /// The window a loader reads first. Big enough that one read covers the header, the level table of
    /// any texture this engine can create and a source key of ordinary length — so the common case is
    /// ONE read, not two. The biggest table the engine can produce is a 16384-texel CUBE: 15 levels of
    /// 6 layers, 90 rows of 24 bytes = 2160, which with the 128-byte header leaves 1808 for the key.
    /// It is not a limit: a file whose metadata does not fit is read again at the exact size
    /// `TextureBinaryMetadataBytes` reports, and the suite drives that branch with a deliberately long
    /// key rather than leaving it to a user.
    inline constexpr std::size_t kTextureBinaryPrefixBytes = 4096;

    /// WHAT THE LAYERS OF ONE LEVEL MEAN TOGETHER. `LayerCount` alone cannot say this and must not be
    /// asked to: six layers are a cube to a sampler and six unrelated images to an array, and the two
    /// are different `VkImageViewType`s, different descriptors and different shader declarations. A
    /// reader that inferred "6 == cube" would be one six-slice array away from binding a `samplerCube`
    /// to something that is not one — a middle link dropping a property, in the file itself.
    enum class TextureKind : uint32_t
    {
        Texture2D = 0, /// one layer, one image
        Cube      = 1, /// exactly six layers, square, in the Vulkan face order (+X -X +Y -Y +Z -Z)
    };

    /// The face order every cube in this container is written and read in. It is `VkImageCreateInfo`'s
    /// array-layer order, which is what `vkCmdCopyBufferToImage`'s `baseArrayLayer` indexes and what a
    /// `samplerCube` addresses — so the container stores what the GPU consumes and nobody permutes.
    inline constexpr uint32_t kTextureCubeLayerCount = 6;

    /// Where (level, layer) sits in a level table whose layer count is @p layerCount. THE ONE INDEXING
    /// RULE, so no caller writes `level * 6 + face` and no caller writes `layer * levels + level`.
    [[nodiscard]] constexpr std::size_t TextureLevelIndex( const uint32_t level, const uint32_t layer,
                                                           const uint32_t layerCount )
    {
        return static_cast<std::size_t>( level ) * layerCount + layer;
    }

    /// How many bytes from the start of the file a header decode needs, answered from the first
    /// `kTextureBinaryHeaderSize` bytes alone. 0 when @p headerBytes is shorter than that, or does not
    /// carry the magic — in both cases the caller has nothing to size a second read with and should let
    /// the decoder produce the refusal.
    [[nodiscard]] uint64_t TextureBinaryMetadataBytes( std::string_view headerBytes );

    /// How one level's bytes lie in the file. The column exists so that the LEVEL decides, for the same
    /// reason the archive's codec column is per entry and not per archive: a mip chain's smallest levels
    /// are a few bytes each and never compress, and a decision taken for the file as a whole would have
    /// to be wrong for one end of it.
    enum class TextureLevelCodec : uint32_t
    {
        Store = 0, /// the level's bytes are its pixels, byte for byte
        LZ4   = 1, /// `Common/Utilities/Lz4Block.hpp`, one block, no frame — the archive's own codec
    };

    /// WHERE ONE LEVEL LIES IN THE FILE, which is the entire input a streaming read needs: seek here,
    /// read this many bytes, decode them this way, and you hold that level and nothing else.
    ///
    /// ITS `FileOffset` IS FILE-RELATIVE AND IT IS A DIFFERENT NUMBER FROM `TextureLevel::ByteOffset`,
    /// which is an offset into the DECODED payload. They are separate types precisely so the two cannot
    /// be handed to each other: with compression the pair stopped being a constant apart, and a single
    /// struct meaning both things is how a middle link drops a property.
    struct TextureLevelLocation
    {
        uint64_t          FileOffset = 0; /// from the start of the FILE
        uint32_t          StoredSize = 0; /// bytes to read
        uint32_t          ByteSize   = 0; /// bytes after decoding
        TextureLevelCodec Codec      = TextureLevelCodec::Store;
    };

    /// One image of the chain — one (mip level, array layer) pair. The table is INDEXED BY
    /// `TextureLevelIndex(level, layer, LayerCount)`, so row 0 is always layer 0's full-size image,
    /// whatever order the bytes sit in on disk. Only the physical order is reversed; the table is not.
    ///
    /// `ByteOffset` here is measured from the START OF THE PAYLOAD RUN, while the file's own row measures
    /// it from the start of the FILE. That is deliberate and it is the one place this format converts a
    /// number, so it is also the one place a middle link could drop a property: the whole run is copied
    /// into one staging buffer, and the GPU needs the offset INSIDE that buffer. The decoder subtracts
    /// the payload's start exactly once and the suite pins both spellings.
    struct TextureLevel
    {
        uint32_t Width      = 0;
        uint32_t Height     = 0;
        uint64_t ByteOffset = 0;
        uint64_t ByteSize   = 0;
        /// Bytes from the start of one row of this level to the next. For an uncompressed format it is
        /// `Width * bytes-per-pixel`; it is in the file because for a BLOCK format a row's stride is not
        /// a function of the width alone, and the day that arrives the reader must not have to guess.
        uint32_t RowPitch = 0;
    };

    /// Everything a cooked texture is. `Pixels` holds every level of the chain back to back; `Levels`
    /// says where each one starts. A decode that succeeded always has at least one level, and
    /// `Levels[0]` is always the full-size image.
    struct TextureAssetData
    {
        Common::UUID Handle;

        /// The source image's place inside the project, in `AssetHandle::StableKeyForPath` form
        /// (`assets:Textures/T.png`) — never an absolute path, which is what this field used to carry
        /// and what made every committed `.tex` loadable on exactly one machine.
        ///
        /// IT IS PROVENANCE, NOT A POINTER THE RENDERER FOLLOWS. Nothing in the texture path opens it
        /// any more: the pixels are in this file. It is here because the cooker has to find the source
        /// again in order to ask whether the cook is stale, because the editor labels a texture by the
        /// name an artist gave it, and because the animated-image and video services address their own
        /// sources by it.
        std::string SourcePath;

        /// The source image file's 64-bit signature at cook time: CRC-32C of its bytes in the low half,
        /// its byte length in the high half. See the header comment for why it is the FILE's bytes and
        /// not the decoded pixels, and `SourceSignature` below for why both halves.
        uint64_t SourceContentHash = 0;

        /// The extent of LEVEL 0 OF ONE LAYER. For a cube both are the face edge and they are equal;
        /// the file refuses a cube whose two extents differ rather than picking one of them.
        uint32_t                   Width  = 0;
        uint32_t                   Height = 0;
        Core::Formats::ImageFormat Format = Core::Formats::ImageFormat::RGBA8F;

        /// How many images ONE LEVEL is made of. 1 for an ordinary texture, `kTextureCubeLayerCount`
        /// for a cube. It is never inferred from `Kind` and `Kind` is never inferred from it: both are
        /// written, and the decoder refuses a pair that disagrees.
        uint32_t    LayerCount = 1;
        TextureKind Kind       = TextureKind::Texture2D;

        /// WHAT THE AUTHOR SAID THIS TEXTURE IS FOR, as the cook read it at cook time. See
        /// `Core/Formats/TextureIntent.hpp`; `Unspecified` means nobody said and is what every cooked
        /// texture in this repository carried before the field existed.
        ///
        /// IT IS A RECORD, NOT THE AUTHORITY, and the distinction is the whole reason it is safe to
        /// have here. The authority is a `.detex` beside the SOURCE, because `Cooked/` is derived data
        /// whose documented remedy is deletion. This copy exists so that a refusal is reproducible
        /// from the file alone -- "this was cooked as a normal map and stored uncompressed" is a
        /// sentence the container can support and a sentence the log alone cannot, once the log has
        /// scrolled. The cook re-reads the authored file every time and the two cannot drift, because
        /// a disagreement between them IS the staleness check: `EncoderHash` folds the intent in, so a
        /// changed `.detex` re-cooks.
        Core::Formats::TextureIntent Intent = Core::Formats::TextureIntent::Unspecified;

        /// THE SETTINGS THAT PRODUCED THESE PIXELS, or 0 for "none were recorded". It is not how the
        /// bytes are packed — that is the per-level `Codec` — and it is not which file they came from
        /// — that is `SourceContentHash`. It answers the third question a derived asset has: WAS THIS
        /// MADE THE WAY I AM ASKING FOR IT NOW.
        ///
        /// An image importer records nothing here: a PNG's pixels are its pixels. A BAKE records
        /// everything its output depends on that is not the source file, and the reader that has an
        /// expectation compares it and re-bakes on a mismatch. v1 and v2 refused a non-zero value
        /// outright; see the long note at the check in `TextureBinary.cpp` for why v3 does not.
        uint64_t EncoderHash = 0;

        /// Indexed by `TextureLevelIndex(level, layer, LayerCount)`, so `size() == levels * LayerCount`.
        /// `Levels[TextureLevelIndex(0, layer, LayerCount)]` is always layer @p layer's full-size image.
        std::vector<TextureLevel> Levels;

        /// How many mip levels the table describes. DERIVED, never stored twice: a count beside a table
        /// is a second thing to keep in step, and the pair would disagree the first time one moved.
        [[nodiscard]] uint32_t LevelCount() const
        {
            return LayerCount == 0 ? 0u : static_cast<uint32_t>( Levels.size() / LayerCount );
        }

        /// `unsigned char` AND NOT `std::byte`, because this vector is handed to the GPU upload as-is.
        /// `Core::Formats::ImagePixelData` carries a `std::vector<unsigned char>` alternative, so this
        /// type MOVES into it; a `std::vector<std::byte>` had to be copied element by element first,
        /// and on a 2048x2048 texture that copy is 21.3 MB nobody asked for. The bytes are opaque
        /// either way — `Levels` says what is in them.
        std::vector<unsigned char> Pixels;
    };

    /// What the header alone says. Enough to identify the texture, to decide whether its cook is stale
    /// and to size its upload; not enough to draw it.
    struct TextureBinaryHeaderInfo
    {
        Common::UUID               Handle;
        std::string                SourcePath;
        uint64_t                   SourceContentHash = 0;
        uint64_t                   EncoderHash       = 0;
        uint32_t                   Width             = 0;
        uint32_t                   Height            = 0;
        Core::Formats::ImageFormat Format            = Core::Formats::ImageFormat::RGBA8F;
        uint32_t                   LevelCount        = 0;
        uint32_t                   LayerCount        = 1;
        TextureKind                Kind              = TextureKind::Texture2D;
        /// The authored intent this file was cooked for. See `TextureAssetData::Intent`.
        Core::Formats::TextureIntent Intent = Core::Formats::TextureIntent::Unspecified;
        /// Sum of the DECODED level sizes — how big the staging buffer has to be. Padding excluded.
        uint64_t PayloadBytes = 0;
        /// Sum of the STORED level sizes — how many bytes of this file are pixels. Equal to
        /// `PayloadBytes` when nothing was compressed, and never larger.
        uint64_t StoredPayloadBytes = 0;
        uint64_t FileSize           = 0;

        /// Where every level lies, indexed by `TextureLevelIndex(level, layer, LayerCount)`, read out of
        /// the table the header decode already had to parse and validate. This is what makes "read only mip 4" a
        /// thing a caller can DO rather than a property the format merely claims: one `kTextureBinaryPrefixBytes`
        /// read gets the header and this table, and each row then names a seek, a length and a codec.
        std::vector<TextureLevelLocation> Levels;
    };

    /// The source signature the cook records and the freshness check compares: CRC-32C of @p bytes in
    /// the low 32 bits, `size` in the high 32.
    ///
    /// WHY BOTH HALVES. T3 §5c asks for eight bytes and for the hash to be taken over the source FILE's
    /// bytes rather than its decoded pixels, because a check that has to decode every source is a check
    /// people stop running. CRC-32C is what this project already has as its byte-integrity primitive
    /// (8.17 GB/s on the instruction here against FNV-1a's 0.77 — `Common/Utilities/Crc32c.hpp`), and it
    /// is 32 bits. The upper half is filled with the length rather than left empty: it costs nothing,
    /// and it removes the whole class of collisions where two differently-sized files share a CRC.
    [[nodiscard]] uint64_t SourceSignature( const void* bytes, std::size_t size );

    /// Does @p bytes begin with the container magic? The one question either reader will answer about a
    /// payload it has not parsed, and the one that separates a cooked texture from the retired manifest.
    [[nodiscard]] bool LooksLikeTextureBinary( std::string_view bytes );

    /// LAY OUT A CHAIN SOMETHING ELSE ALREADY BUILT, which is the only shape a cube can arrive in: the
    /// mip levels of a prefiltered environment are a GGX convolution at a per-mip roughness, not a box
    /// filter of the level above, so there is nothing here to compute and everything here to place.
    ///
    /// @p images is TIGHTLY PACKED IN TABLE ORDER — level 0's layers first, then level 1's — which is
    /// the order a `vkCmdCopyImageToBuffer` over an image's subresources produces. What comes back is
    /// the file's own order (smallest level first, layers together inside a level) with the padding in
    /// place, and a table whose every row has been checked against the extent the level must have.
    ///
    /// It is a SEPARATE function from `BuildMipChain` rather than a flag on it, because the two answer
    /// different questions: that one GENERATES levels and may refuse a format it cannot filter, this
    /// one PLACES levels and does not care what made them.
    [[nodiscard]] Common::ResultStr<std::vector<TextureLevel>>
    BuildLevelTable( uint32_t width, uint32_t height, uint32_t levelCount, uint32_t layerCount,
                     Core::Formats::ImageFormat format, const std::vector<unsigned char>& images,
                     std::vector<unsigned char>& chainOut );

    /// RE-ENCODE A CHAIN THAT IS ALREADY LAID OUT INTO A BLOCK FORMAT, level by level and layer by
    /// layer, and hand back the result TIGHTLY PACKED IN TABLE ORDER — which is exactly what
    /// `BuildLevelTable` takes, so the two compose and there is no third spelling of the layout.
    ///
    /// WHY IT TAKES A LAID-OUT CHAIN AND NOT A RAW IMAGE. Both producers of a chain in this engine —
    /// `BuildMipChain` for a 2D cook and `BuildLevelTable` for a baked cube — already hand back this
    /// pair, and compression is a step BETWEEN building a chain and writing it. A function that took
    /// raw pixels would have to re-derive the level extents, which is the arithmetic that has to agree
    /// with the file and therefore must not be written twice.
    ///
    /// WHY IT IS NOT A FLAG ON `EncodeTextureBinary`. That function's `CompressLevels` is LZ4 over the
    /// stored bytes and is TRANSPARENT: the file decodes to the identical texture either way. This is
    /// not — it changes the format, the sizes and the pixels, and the container records a different
    /// `Format` afterwards. Putting the two behind one option would make "compressed" mean two things,
    /// one of them lossy.
    [[nodiscard]] Common::ResultStr<std::vector<unsigned char>>
    BlockCompressChain( uint32_t width, uint32_t height, uint32_t levelCount, uint32_t layerCount,
                        Core::Formats::ImageFormat sourceFormat, Core::Formats::ImageFormat blockFormat,
                        const std::vector<TextureLevel>&  sourceLevels,
                        const std::vector<unsigned char>& sourcePixels );

    /// How many bytes `BuildLevelTable` demands in @p images for this shape: every level of every layer,
    /// tightly packed. Exposed because a caller that reads a GPU image back has to SIZE the readback
    /// before it has anything to hand in, and a second spelling of this sum is how the two would drift.
    [[nodiscard]] uint64_t TightlyPackedChainBytes( uint32_t width, uint32_t height, uint32_t levelCount,
                                                    uint32_t layerCount, Core::Formats::ImageFormat format );

    /// The complete mip chain for one image, level 0 first, built by a 2x2 box filter — the same
    /// operation the `vkCmdBlitImage` chain it replaces performed with `VK_FILTER_LINEAR` at exactly
    /// half scale, so the pixels this produces are the pixels the GPU was producing.
    ///
    /// IT FILTERS IN THE STORED VALUES, NOT IN LINEAR LIGHT, and that is a decision rather than an
    /// oversight: the blit chain filtered in the image's own UNORM values too, and a gamma-correct
    /// downsample would change every minified texel in every frame on the day mips moved into the file.
    /// Correct mip generation for sRGB content belongs with an authored colour-space marking, and the
    /// citation here used to be wrong twice over: it named `PROGRAMME.md` §5 STEP 4, which is the
    /// ENCODER (§5's order is mips-in-file, block model, authored field, encoder — the field is step
    /// 3), and step 3 has since landed as `Core/Formats/TextureIntent.hpp` WITHOUT an sRGB bit. That
    /// was deliberate: colour space has its own consumer (the sampler's view format) and its own
    /// migration, and folding it into an intent would make one field answer two questions. So this
    /// filter still works in stored values, and what it waits for is a marking that does not exist
    /// yet rather than a step that has already happened.
    ///
    /// An odd extent halves DOWN (`max(1, n/2)`, the Vulkan chain rule) and the filter averages the
    /// 2x2 block clamped to the source, so the last row or column of an odd level is not dropped.
    /// @p base must hold exactly `width * height * GetBytesPerPixel(format)` bytes.
    [[nodiscard]] Common::ResultStr<std::vector<TextureLevel>>
    BuildMipChain( uint32_t width, uint32_t height, Core::Formats::ImageFormat format,
                   const std::vector<unsigned char>& base, std::vector<unsigned char>& chainOut );

    /// What the cook may do to the levels on the way out. The default is what the cooker uses.
    struct TextureEncodeOptions
    {
        /// Try the codec on each level and keep the result only where it clears the archive's own
        /// threshold (`Common/Utilities/PakFile.hpp`, kCompressionNumerator/kCompressionDenominator).
        /// Off produces a byte-for-byte v2 file with every level `Store`d, which is what the suite
        /// uses to prove the two paths decode to the same texture.
        bool CompressLevels = true;
    };

    /// `TextureAssetData` -> container bytes. Total: every field of the struct is written, so a round
    /// trip is an identity and the suite asserts it as one — WITH OR WITHOUT COMPRESSION, which is the
    /// relation that says the codec is transparent rather than merely present.
    ///
    /// WHY COMPRESSION IS ON BY DEFAULT, measured over every texture this repository can cook
    /// (2026-09-23, this tree, the archive's Lz4Block at its own threshold). The fourteen sources make
    /// 152 415 040 bytes of mip chain. Compressed as WHOLE FILES — which is what an archive would do to
    /// them — they are 57 894 177 bytes, 2.63x. Compressed LEVEL BY LEVEL they are 58 562 013, 2.60x.
    /// The per-level form therefore costs 1.15 % more bytes and keeps the property the container exists
    /// for; the whole-file form saves that 1.15 % and makes reading one mip mean decoding all of them.
    /// That is the trade, and it is not close.
    ///
    /// It is also not always a cost: on `O4_MaskAddRemove.png` the whole payload compresses 1.56x and
    /// does not clear the threshold at all, so the archive would store 1 398 112 bytes, while the same
    /// pixels level by level come to 11 833 — 118x. One long stream can hide matches that each level
    /// finds on its own.
    [[nodiscard]] std::string EncodeTextureBinary( const TextureAssetData&     data,
                                                   const TextureEncodeOptions& options = {} );

    /// The header and the level table only — no pixel bytes are copied, and @p bytes may be just the
    /// prefix of the file (anything from `kTextureBinaryHeaderSize` upwards). The truncation check is
    /// therefore NOT applied here: a prefix is legally short. Use `DecodeTextureBinary` when the whole
    /// file is in hand.
    [[nodiscard]] Common::ResultStr<TextureBinaryHeaderInfo> DecodeTextureHeader( std::string_view bytes,
                                                                                  std::string_view whatFor );

    /// Container bytes -> `TextureAssetData`, or a refusal naming the file and the two numbers that
    /// disagreed. @p whatFor is the path the refusal quotes; it is never read out of the payload,
    /// because a corrupt payload cannot be trusted to name itself.
    [[nodiscard]] Common::ResultStr<TextureAssetData> DecodeTextureBinary( std::string_view bytes,
                                                                           std::string_view whatFor );

    /// The refusal a reader owes a file that does not carry the magic. One sentence, in one place, so
    /// the loader and the cooker cannot describe the same situation differently.
    [[nodiscard]] std::string StaleCookRefusal( std::string_view whatFor );
} // namespace Desert::Assets::Serialization
