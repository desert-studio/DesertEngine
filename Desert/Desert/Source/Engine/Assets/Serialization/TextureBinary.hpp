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
//   [Header, 64 B][LevelRow, LevelCount * 24 B][source key bytes, padded to 8][level payloads, tight]
//
// The payloads are tightly packed, deliberately: their sizes are whole multiples of the texel size
// (4 bytes for RGBA8, 16 for RGBA32F), so every level's offset inside the payload run is a legal
// `VkBufferImageCopy::bufferOffset` without padding — and one `memcpy` of the whole run into one
// staging buffer serves every level, which is what makes the upload one copy command per level rather
// than one allocation per level.
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
    inline constexpr char kTextureBinaryMagic[8] = { 'D', 'E', 'S', 'T', 'T', 'E', 'X', 'T' };

    /// The bytes of the header plus one level row — the most a reader ever needs in order to answer
    /// "what is this texture" without touching a pixel. `TextureAsset::LoadFromFile` reads exactly this
    /// prefix, which is what keeps `TextureService`'s "cheap: reads the metadata, not pixels" true now
    /// that the file is megabytes rather than bytes.
    inline constexpr std::size_t kTextureBinaryHeaderSize = 64;

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

    /// One level of the chain. `ByteOffset` is measured from the START OF THE PAYLOAD RUN, not from the
    /// start of the file, because that is the number the GPU upload needs: the whole run is copied into
    /// one staging buffer and each level is copied out of it at this offset.
    struct TextureLevel
    {
        uint32_t Width      = 0;
        uint32_t Height     = 0;
        uint64_t ByteOffset = 0;
        uint64_t ByteSize   = 0;
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

        /// CRC-32C of the source image file's bytes at cook time. See the header comment.
        uint32_t SourceContentHash = 0;

        uint32_t                   Width  = 0;
        uint32_t                   Height = 0;
        Core::Formats::ImageFormat Format = Core::Formats::ImageFormat::RGBA8F;

        std::vector<TextureLevel> Levels;
        std::vector<std::byte>    Pixels;
    };

    /// What the header alone says. Enough to identify the texture, to decide whether its cook is stale
    /// and to size its upload; not enough to draw it.
    struct TextureBinaryHeaderInfo
    {
        Common::UUID               Handle;
        std::string                SourcePath;
        uint32_t                   SourceContentHash = 0;
        uint32_t                   Width             = 0;
        uint32_t                   Height            = 0;
        Core::Formats::ImageFormat Format            = Core::Formats::ImageFormat::RGBA8F;
        uint32_t                   LevelCount        = 0;
        uint64_t                   FileSize          = 0;
    };

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
                   const std::vector<std::byte>& base, std::vector<std::byte>& chainOut );

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
