#pragma once

// THE COOKED TEXTURE CONTAINER — why a `.tex` carries pixels now, and what it has to be able to refuse.
//
// WHAT IT REPLACED, MEASURED IN THIS TREE. `Editor/Cooked/Textures/T_Checker.tex` was 133 bytes of JSON
// naming a PNG and carrying not one pixel. Every load re-decoded that PNG and then built the mip chain
// on the GPU with `vkCmdBlitImage`. Neither half survives contact with the next step of
// `Docs/World/PROGRAMME.md` §5: a block-compressed format has `blitDst=0` on this device (re-measured
// by `Docs/Textures/bcprobe.c`), so the GPU cannot generate its chain, and PNG is a single entropy-coded
// stream, so "read only mip 4" is not slow, it is INEXPRESSIBLE. Mips have to be in the file before
// either can happen, which is why this container exists before the encoder does.
//
// ── THE FORMAT ───────────────────────────────────────────────────────────────────────────────────
//
//   [Header, 128 B][LevelRow, LevelCount * 16 B][source key bytes][pad][levels, SMALLEST FIRST]
//
// The layout is `Docs/Textures/T3_FORMAT_PLAN.md` §5c, which is binding, and two of its decisions are
// not cosmetic:
//
//  * THE LEVELS ARE STORED SMALLEST FIRST. The resident tail — the levels at 16x16 and below that a
//    streaming build keeps in memory for every texture, so that "an object on screen with no texture"
//    stops being a possible state — is then a CONTIGUOUS PREFIX of the file, together with the header
//    and the table. One read of a few hundred bytes gets all of it. Stored in image order the tail
//    would sit at the END of every file and cost a seek per texture, for ever. Mip 0 loses nothing:
//    it is read by offset, not sequentially.
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

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets::Serialization
{
    /// The container's own version sequence, and nothing else's. It is not `kSceneVersion` (21), not
    /// `kMeshBinaryVersion` (1) and not `kAnimationVersion` (3): a `.desce` names a texture by path and
    /// carries none of its bytes, so a scene written yesterday opens a texture cooked today without
    /// either file knowing about the other's number.
    ///
    /// There is no version 0. The JSON manifest this container replaced is recognised by the ABSENCE of
    /// the magic and is REFUSED, not migrated — cooked content is derived data, and the remedy for a
    /// stale cook is to cook again. That is the same call `MeshBinary` made by owner decision
    /// 2026-09-22 ("if the cook is deprecated the user just deletes it").
    inline constexpr uint32_t kTextureBinaryVersion = 1;

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
    /// any texture this engine can create (a 16384-texel chain is 15 rows of 24 bytes) and a source key
    /// of ordinary length — so the common case is ONE read, not two. It is not a limit: a file whose
    /// metadata does not fit is read again at the exact size `TextureBinaryMetadataBytes` reports, and
    /// the suite drives that branch with a deliberately long key rather than leaving it to a user.
    inline constexpr std::size_t kTextureBinaryPrefixBytes = 4096;

    /// How many bytes from the start of the file a header decode needs, answered from the first
    /// `kTextureBinaryHeaderSize` bytes alone. 0 when @p headerBytes is shorter than that, or does not
    /// carry the magic — in both cases the caller has nothing to size a second read with and should let
    /// the decoder produce the refusal.
    [[nodiscard]] uint64_t TextureBinaryMetadataBytes( std::string_view headerBytes );

    /// One level of the chain, INDEXED BY MIP LEVEL — `Levels[0]` is always the full-size image, whatever
    /// order the bytes sit in on disk. Only the physical order is reversed; the table is not.
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

        uint32_t                   Width  = 0;
        uint32_t                   Height = 0;
        Core::Formats::ImageFormat Format = Core::Formats::ImageFormat::RGBA8F;

        std::vector<TextureLevel> Levels;

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
        uint32_t                   Width             = 0;
        uint32_t                   Height            = 0;
        Core::Formats::ImageFormat Format            = Core::Formats::ImageFormat::RGBA8F;
        uint32_t                   LevelCount        = 0;
        uint64_t                   PayloadBytes      = 0;
        uint64_t                   FileSize          = 0;
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

    /// The complete mip chain for one image, level 0 first, built by a 2x2 box filter — the same
    /// operation the `vkCmdBlitImage` chain it replaces performed with `VK_FILTER_LINEAR` at exactly
    /// half scale, so the pixels this produces are the pixels the GPU was producing.
    ///
    /// IT FILTERS IN THE STORED VALUES, NOT IN LINEAR LIGHT, and that is a decision rather than an
    /// oversight: the blit chain filtered in the image's own UNORM values too, and a gamma-correct
    /// downsample would change every minified texel in every frame on the day mips moved into the file.
    /// Correct mip generation for sRGB content belongs with the authored-format field (`PROGRAMME.md`
    /// §5, step 4), where the file can finally say whether its contents are sRGB at all.
    ///
    /// An odd extent halves DOWN (`max(1, n/2)`, the Vulkan chain rule) and the filter averages the
    /// 2x2 block clamped to the source, so the last row or column of an odd level is not dropped.
    /// @p base must hold exactly `width * height * GetBytesPerPixel(format)` bytes.
    [[nodiscard]] Common::ResultStr<std::vector<TextureLevel>>
    BuildMipChain( uint32_t width, uint32_t height, Core::Formats::ImageFormat format,
                   const std::vector<unsigned char>& base, std::vector<unsigned char>& chainOut );

    /// `TextureAssetData` -> container bytes. Total: every field of the struct is written, so a round
    /// trip is an identity and the suite asserts it as one.
    [[nodiscard]] std::string EncodeTextureBinary( const TextureAssetData& data );

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
