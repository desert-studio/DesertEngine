#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>

#include <variant>
#include <optional>
#include <vector>
#include <string>
#include <cstdint>

namespace Desert::Core::Formats
{
    enum class Image2DUsage
    {
        Image2D,
        Attachment
    };

    enum class ImageFormat
    {
        RGBA8F,
        // Half-float colour. Carries pre-tonemap radiance and transmittance for the volumetric targets at
        // half the memory of RGBA32F — three 1920x1080 targets cost 47.5 MiB per live SceneRenderer
        // instead of 95 MiB. Half's ~0.05% relative precision is finer than the 8-bit swapchain and the
        // tonemap downstream of it.
        RGBA16F,
        RGBA32F,
        BGRA8F,
        DEPTH24STENCIL8,

        DEPTH32F,

        // Not a format. Every real format goes ABOVE this line, and the count below is derived from it,
        // so there is no number for anyone to remember to bump — which is the whole reason it exists.
        // A hand-maintained constant was tried first, pinned to the last enumerator with
        // `static_assert( DEPTH32F + 1 == kImageFormatCount )`. That catches a format INSERTED mid-enum
        // (every later value shifts) but NOT one APPENDED after DEPTH32F, because DEPTH32F's own value
        // does not move — verified by mutation, the appended-format build succeeded. A sentinel moves in
        // both cases.
        //
        // The lookups below deliberately have a `case` for it that falls through to their error path:
        // Count is not a format, so asking for its size is the same programmer error as passing a
        // cast-in integer, and the switches stay exhaustive over the enum either way.
        Count
    };

    // Derived, never written down. The exhaustiveness guard further down walks 0..Count and
    // constant-evaluates every format lookup for each value, so a format added without a case in one of
    // them fails the BUILD. See the note on "Format facts" below for why that guard is a constant
    // expression rather than a compiler warning.
    constexpr uint32_t kImageFormatCount = static_cast<uint32_t>( ImageFormat::Count );

    enum ImageProperties : uint32_t
    {
        Storage = 0x1,
        Sample  = 0x2,
    };

    // Which planes of an image a barrier or a view must name. The bit values mirror VK_IMAGE_ASPECT_*,
    // but this is the engine's own vocabulary on purpose: Core knows nothing about Vulkan, and the
    // backend translates explicitly rather than punning on the numbers.
    enum ImageAspect : uint32_t
    {
        ImageAspect_Colour  = 0x1,
        ImageAspect_Depth   = 0x2,
        ImageAspect_Stencil = 0x4,
    };

    constexpr ImageProperties operator|( ImageProperties a, ImageProperties b )
    {
        return static_cast<ImageProperties>( static_cast<uint32_t>( a ) | static_cast<uint32_t>( b ) );
    }

    // ── Format facts ───────────────────────────────────────────────────────────────────────────────
    //
    // The lookups below are TOTAL over ImageFormat: every enumerator has an explicit case, there is no
    // `default:` label, and NOTHING is returned after the switch. That combination is deliberate. The
    // previous version of GetBytesPerPixel handled two formats and ended in `return 0U;`, so a format it
    // did not know about produced a bytes-per-pixel of 0 — which CalculateImageSize then multiplied into
    // a zero-byte staging buffer for a real image. The corruption surfaced far away from the format that
    // caused it, which is the expensive part.
    //
    // Adding a format without extending them has to break the BUILD, not a test somebody may not run.
    // The obvious mechanism — -Wswitch on a switch with no `default:` — does NOT work here and is worth
    // knowing about: the workspace compiles with `warnings "Off"` (BuildScripts/Workspace.lua:22), which
    // becomes `-w`, and `-w` overrides -Wswitch AND every `#pragma diagnostic error` that tries to
    // promote it back (verified, clang 17, 2026-08). So the guard is the language instead of a flag:
    // these are `constexpr`, and LookupsAreTotal() below constant-evaluates them for every enumerator.
    // Falling off the end of a constexpr function during constant evaluation is ill-formed, so a missing
    // case is a compile error in any configuration and under any warning settings.

    constexpr uint32_t GetBytesPerPixel( ImageFormat format )
    {
        switch ( format )
        {
            case ImageFormat::RGBA8F:
                return 4; // 4 channels, 8 bits each
            case ImageFormat::RGBA16F:
                return 8; // 4 channels, 16 bits each
            case ImageFormat::RGBA32F:
                return 16; // 4 channels, 32 bits each
            case ImageFormat::BGRA8F:
                return 4;
            case ImageFormat::DEPTH24STENCIL8:
                return 4; // 24-bit depth + 8-bit stencil, packed into one 32-bit texel
            case ImageFormat::DEPTH32F:
                return 4;
            case ImageFormat::Count:
                break; // the sentinel is not a format — fall through to the error path below
        }

        // Reached only by the sentinel or by a corrupted / cast-in integer, so this is a programmer
        // error, not a data error. Name the value and stop; never hand back a size that a caller will
        // turn into an allocation.
        LOG_ERROR( "GetBytesPerPixel: ImageFormat value {} is outside the enumeration",
                   static_cast<uint32_t>( format ) );
        DESERT_VERIFY( false, "ImageFormat outside the enumeration" );
    }

    // Which planes a barrier or an image view must name for this format.
    constexpr ImageAspect GetImageAspect( ImageFormat format )
    {
        switch ( format )
        {
            case ImageFormat::RGBA8F:
            case ImageFormat::RGBA16F:
            case ImageFormat::RGBA32F:
            case ImageFormat::BGRA8F:
                return ImageAspect_Colour;
            // A packed depth+stencil image has BOTH planes, and a barrier that names only DEPTH is a
            // VUID-VkImageMemoryBarrier-image-03319 violation. This is exactly what stopped the scene
            // depth image from being transitioned for a compute read.
            case ImageFormat::DEPTH24STENCIL8:
                return static_cast<ImageAspect>( ImageAspect_Depth | ImageAspect_Stencil );
            case ImageFormat::DEPTH32F:
                return ImageAspect_Depth;
            case ImageFormat::Count:
                break; // the sentinel is not a format — fall through to the error path below
        }

        LOG_ERROR( "GetImageAspect: ImageFormat value {} is outside the enumeration",
                   static_cast<uint32_t>( format ) );
        DESERT_VERIFY( false, "ImageFormat outside the enumeration" );
    }

    namespace Detail
    {
        // The exhaustiveness guard. Every lookup above is called once per enumerator inside a constant
        // expression, so a format without a case makes this call fall off the end of a constexpr
        // function — ill-formed, hence a compile error. The `== 0` comparisons exist only to give the
        // loop something to do; the totality is enforced by the evaluation itself, not by the answers.
        constexpr bool LookupsAreTotal()
        {
            for ( uint32_t i = 0; i < kImageFormatCount; ++i )
            {
                const ImageFormat format = static_cast<ImageFormat>( i );
                if ( GetBytesPerPixel( format ) == 0 )
                    return false;
                if ( GetImageAspect( format ) == 0 )
                    return false;
            }
            return true;
        }
    } // namespace Detail

    static_assert( Detail::LookupsAreTotal(),
                   "Every ImageFormat enumerator needs a case in GetBytesPerPixel and GetImageAspect, and "
                   "kImageFormatCount must count them all." );

    // Byte size of a tightly-packed image. 64-bit because a volume is easy to size past 4 GiB, and a
    // silently truncated allocation size belongs to the same family of bugs as a zero bytes-per-pixel.
    constexpr uint64_t CalculateImageSize( uint32_t width, uint32_t height, ImageFormat format )
    {
        return static_cast<uint64_t>( width ) * height * GetBytesPerPixel( format );
    }

    constexpr uint64_t CalculateImageSize( uint32_t width, uint32_t height, uint32_t depth, ImageFormat format )
    {
        return static_cast<uint64_t>( width ) * height * depth * GetBytesPerPixel( format );
    }

    using ImagePixelData =
         std::variant<std::monostate, std::vector<float>, std::vector<unsigned char>, std::byte*>;
    using EmptyPixelData = std::monostate;

    inline bool HasData( const ImagePixelData& data )
    {
        return !std::holds_alternative<std::monostate>( data );
    }

    inline std::optional<std::vector<unsigned char>> GetUCharData( const ImagePixelData& data )
    {
        if ( const auto* vec = std::get_if<std::vector<unsigned char>>( &data ) )
        {
            return *vec;
        }
        return std::nullopt;
    }

    inline std::optional<std::vector<float>> GetFloatData( const ImagePixelData& data )
    {
        if ( const auto* vec = std::get_if<std::vector<float>>( &data ) )
        {
            return *vec;
        }
        return std::nullopt;
    }

    /// How many bytes the variant is holding, or 0 when it holds nothing or a bare pointer (whose
    /// length it does not carry — that is the whole difference between the two).
    ///
    /// ADDED WITH THE MIP TABLE, AND FOR A DEFECT IT PREVENTS. The staging buffer for a supplied chain
    /// was first sized as `MipLevels.back().ByteOffset + .ByteSize`, which silently encoded "the last
    /// level is the last bytes". The cooked container stores its levels SMALLEST FIRST, so `back()` is
    /// the 1x1 level at offset 0 and the buffer came out four bytes long: every copy region then lay
    /// outside it (VUID-vkCmdCopyBufferToImage-pRegions-00171, eleven of them on one frame) and the
    /// whole scene lost its textures. The blob's own length cannot encode an assumption about order.
    inline std::size_t GetPixelDataSize( const ImagePixelData& data )
    {
        if ( const auto* u8 = std::get_if<std::vector<unsigned char>>( &data ) )
            return u8->size();
        if ( const auto* f32 = std::get_if<std::vector<float>>( &data ) )
            return f32->size() * sizeof( float );
        return 0;
    }

    inline const std::byte* GetRawData( const ImagePixelData& data )
    {
        if ( const auto* ptr = std::get_if<std::byte*>( &data ) )
        {
            return *ptr;
        }
        return nullptr;
    }

    // One level of a mip chain that is ALREADY BUILT and sitting in `Image2DSpecification::Data`.
    // Offsets are measured from the start of that blob, so the whole chain is one allocation and one
    // upload rather than one per level.
    struct MipLevelSpan
    {
        uint64_t ByteOffset = 0;
        uint64_t ByteSize   = 0;
    };

    struct Image2DSpecification
    {
        const std::string     Tag;
        uint32_t              Width;
        uint32_t              Height;
        const ImageFormat     Format;
        uint32_t              Mips = 1;
        // MSAA sample count (1/2/4/8) — attachments only; a multisampled image must have Mips == 1
        // and is consumed by the render pass RESOLVE, not by ordinary samplers.
        uint32_t              Samples = 1;
        ImagePixelData        Data;
        const Image2DUsage    Usage;
        const ImageProperties Properties;

        // THE CHAIN THE CALLER ALREADY HAS, level 0 first. When this is non-empty it IS the chain: the
        // backend takes its level count from here, uploads exactly these spans out of `Data`, and
        // generates nothing. When it is empty the image has `Mips` levels and whatever is in `Data`
        // goes to level 0, which is what every render target and every procedural texture wants.
        //
        // ONE AUTHORITY, NOT TWO FIELDS THAT MUST AGREE. `Mips` is not consulted while this is
        // non-empty, and the backend REFUSES a specification that sets both rather than silently
        // preferring one — a count and a table that disagree is the "middle link drops a property"
        // shape, and the only safe answer to it is to make the ambiguous call impossible to ship.
        //
        // IT REPLACED A `bool GenerateMips` THAT WAS NEVER TRUE. That flag asked the backend to blit
        // mip 0 down the chain, and every one of its five call sites set it to false — the mips that
        // reached the screen came from `MipMap2DGenerator`, a second mechanism passed to
        // `Image2D::Create`. A setting whose only branch is never taken is the dead knob §3 forbids,
        // and it would have become a lie the day a block-compressed format arrived: `blitDst=0` for
        // BC1/BC4/BC5/BC7 on this device, so there is no blit chain to ask for.
        std::vector<MipLevelSpan> MipLevels;
    };

    // Length of the full mip chain for a texture whose largest dimension is @p dim
    // (floor(log2(dim)) + 1, and 1 for a zero/one-texel extent). This is THE definition — the mip count a
    // Vulkan image legally accepts, the count the cost report charges for, and the count a dispatch loop
    // walks all derive from it, so they cannot disagree by each rounding log2 their own way.
    constexpr uint32_t MipChainLength( uint32_t dim )
    {
        uint32_t levels = 1u;
        for ( ; dim > 1u; dim >>= 1u )
            ++levels;
        return levels;
    }

    struct ImageCubeSpecification
    {
        const std::string Tag;
        // The edge of one FACE, in texels. A cube image is six square layers of exactly this size, and
        // every derived quantity — the legal mip chain, a compute dispatch's extent, the byte cost —
        // follows from the face. This field used to be a Width/Height pair carrying the 4x3 "cross"
        // unwrap of the source image, with every consumer dividing back to the face; three separate
        // defects were one side of that division going missing (a mip count computed from the cross ->
        // invalid vkCreateImage and VK_ERROR_DEVICE_LOST; dispatches over the cross -> 12x-16x surplus
        // threads; a prefiltered cube created at a QUARTER of the face its caller asked for). The cross
        // is a layout of source pixels, not a property of the cube.
        const uint32_t        FaceSize;
        const ImageFormat     Format;
        const uint32_t        Mips = 1;
        ImagePixelData        Data;
        const ImageProperties Properties;
    };

    // A volume texture — the shape/detail noise the volumetric passes sample, and any other
    // compute-generated 3D field.
    //
    // There is deliberately NO `Mips` field. The engine has no 3D mip generator (MipMap2DGenerator and
    // MipMapCubeGenerator have no 3D counterpart, and blitting a volume chain is a separate piece of
    // work), so a mip count above 1 could be requested but never filled — a setting that does nothing.
    // Volumes are therefore single-level, and shaders sample them without an explicit LOD.
    struct Image3DSpecification
    {
        const std::string     Tag;
        const uint32_t        Width;
        const uint32_t        Height;
        const uint32_t        Depth;
        const ImageFormat     Format;
        ImagePixelData        Data;
        const ImageProperties Properties;
    };
} // namespace Desert::Core::Formats