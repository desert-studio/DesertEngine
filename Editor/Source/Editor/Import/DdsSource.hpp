#pragma once

// A `.dds` FILE AS A TEXTURE SOURCE — decoded to the uncompressed image it stands for, like a PNG.
//
// This is NOT the closed question of "DDS as our cooked container": the cook still writes the `.tex`
// container with its own BCn blocks (`Docs/Textures/T2_CONTAINER_DECISION.md`), and nothing at runtime
// reads a `.dds`. What changes is the INPUT side, the way UE's TextureFactory takes a `.dds` source
// (Runtime/ImageCore DDSFile.cpp parses it; the importer then holds the uncompressed source image).
// Asset packs ship as `.dds` (Bistro: 622 of them), and until this file existed every one of them
// reached stb and came back "unknown image type".
//
// WHAT IS READ. The DX9 header and the DX10 extension header; of the image, only the TOP MIP of the
// FIRST array slice / cube face — the source is one image and the cook builds its own mip chain from
// it. Pixel formats: BC1-BC7 (BC4/BC5 unsigned and signed, BC6H unsigned and signed), RGBA8/BGRA8/
// BGRX8 (DX10 codes and 32-bit DX9 masks), RGBA16 UNORM, RGBA16F and RGBA32F. The block decoders are
// bcdec (ThirdParty/bcdec, MIT) — every BC7 and BC6H mode, not only the two this project's encoder
// writes (`Engine/Core/Formats/BlockCompression.hpp`).
//
// WHAT COMES OUT. LDR formats become RGBA8; BC6H, RGBA16F and RGBA32F become RGBA32F (the cook keeps
// float sources float, same as `.exr`/`.hdr`). BC4 is a grey image (R replicated into G and B, alpha
// opaque) — what stb gives for a grey PNG. BC5 is the two-channel tangent-space normal format
// (`ImageFormat.hpp`, BC5_UNORM), so its Z is RECONSTRUCTED into blue from X and Y: the decoded source
// is a whole normal map whatever intent it is later cooked with, not a red-green picture that is only
// right on the BC5 path. sRGB vs UNORM variants decode to the same bytes — the colour space is the
// asset's intent, not the container's flag.
//
// WHAT IS REFUSED, AND HOW. Anything else — a volume texture, a palettised or 24-bit legacy format, a
// DXGI code outside the list, a file shorter than its top mip — is an error whose text names the
// format as the file spelled it. The importer adds the path. There is no "best effort" decode.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    struct DdsSourceImage
    {
        uint32_t                   Width   = 0;
        uint32_t                   Height  = 0;
        bool                       IsFloat = false; ///< true: `RgbaF` holds the texels; false: `Rgba8` does
        std::vector<unsigned char> Rgba8;           ///< Width*Height*4 bytes when !IsFloat
        std::vector<float>         RgbaF;           ///< Width*Height*4 floats when IsFloat
        std::string Format; ///< the source's format as the file named it, e.g. "BC7_UNORM_SRGB (DX10)"
    };

    /// The `DDS ` magic, or the `.dds` spelling of @p sourceKey. Either means DDS, for the same reason as
    /// `IsExrSource`: the spelling alone would send a renamed PNG here, the magic alone would send a
    /// truncated `.dds` to stb, whose "unknown image type" names nothing that is actually wrong.
    [[nodiscard]] bool IsDdsSource( std::string_view bytes, std::string_view sourceKey );

    /// Decode the top mip of the first slice. The error text names the format; the caller names the path.
    [[nodiscard]] Common::ResultStr<DdsSourceImage> DecodeDdsSource( const unsigned char* bytes,
                                                                     std::size_t          size );

    /// A DDS SOURCE'S BLOCKS AS STORED — the first slice's whole mip chain, level 0 first, in its own block
    /// format. The cook stores them in the `.tex` byte for byte when they are already the format the texture's
    /// intent asks for (UE takes a DDS's mips as the source's; re-encoding blocks that were decoded from blocks
    /// would grade a second loss against the first). `Twin` says whether the engine has the same block format
    /// at all, `Nearest` what an unspecified intent re-encodes the source to.
    struct DdsBlockChain
    {
        uint32_t Width      = 0;
        uint32_t Height     = 0;
        uint32_t LevelCount = 1; ///< the file's dwMipMapCount (0 read as 1)
        /// The engine format with the same block bytes (BC4/BC5/BC7 UNORM incl. sRGB, BC6H UF16); Count when
        /// the source's blocks have no twin (BC1-BC3, signed BC4/BC5, BC6H SF16) or it is not a block format.
        ::Desert::Core::Formats::ImageFormat Twin = ::Desert::Core::Formats::ImageFormat::Count;
        /// The LDR block format an unspecified intent cooks this source to; Count for a float or an
        /// uncompressed source, which takes the cook's ordinary path.
        ::Desert::Core::Formats::ImageFormat Nearest = ::Desert::Core::Formats::ImageFormat::Count;
        /// Every level of the first slice, tightly packed, level 0 first; filled only when `Twin` is.
        std::vector<unsigned char> Images;
        std::string                SourceName; ///< as `DdsSourceImage::Format`
    };

    /// Read the header and, for a source with a twin, the chain. A chain shorter than its declared levels is an
    /// error naming both sizes; the caller names the path.
    [[nodiscard]] Common::ResultStr<DdsBlockChain> ReadDdsBlockChain( const unsigned char* bytes,
                                                                      std::size_t          size );
} // namespace Desert::Editor
