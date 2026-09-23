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

        // ── BLOCK-COMPRESSED FORMATS ───────────────────────────────────────────────────────────
        //
        // A block format has NO BYTES PER PIXEL. It has 4x4 texels in sixteen bytes, and a level one
        // texel wide still occupies a whole block. Everything below this line is therefore written in
        // terms of `GetTexelBlock`, and `GetBytesPerPixel` REFUSES these two rather than answering
        // four or some other number that would multiply plausibly and be wrong.
        //
        // THERE ARE FOUR NOW, AND THE FOURTH ARRIVED WITH ITS SELECTOR. This list carried exactly two
        // for as long as nothing could say what a texture was FOR: an enumerator with no producer is a
        // dead knob, and BC5 and BC4 are answers to a question — "is this a normal map, is this one
        // channel" — that no measurement over pixels can ask. `Core/Formats/TextureIntent.hpp` is that
        // question's authored answer, and `BlockPolicyForIntent` is the one place that turns it into
        // one of the two enumerators below. Neither was added before it existed.

        /// `VK_FORMAT_BC7_UNORM_BLOCK`. LDR colour, 4x4 texels in 16 bytes — a quarter of RGBA8.
        /// NEVER FOR NORMAL MAPS: measured on this project's own normal map, BC7 gives 46.48 dB against
        /// BC5's 52.51 and costs 106x the encode time (T1). That is a rule about what may be encoded
        /// into it, not about the format, and it is enforced where the choice is made.
        BC7_UNORM,

        /// `VK_FORMAT_BC6H_UFLOAT_BLOCK`. HDR colour, 4x4 texels in 16 bytes — a SIXTEENTH of RGBA32F
        /// (sixteen bytes a texel becomes one), which is what the baked environment cubes are stored
        /// and resident as. The fourth channel does not survive — BC6H has no alpha, and a radiance
        /// cube's alpha was 1 everywhere. Unsigned on purpose:
        /// radiance is non-negative, and the signed variant is a different VkFormat with a different
        /// endpoint transform, so the choice is in the enumerator's name rather than in a flag.
        BC6H_UFLOAT,

        /// `VK_FORMAT_BC4_UNORM_BLOCK`. ONE channel, 4x4 texels in EIGHT bytes — half of every other
        /// block in this table, and the reason `TexelBlock` carries a byte count per format rather than
        /// a single "a block is sixteen bytes" that three of the four would have agreed with. An eighth
        /// of RGBA8. A sampler returns it as (R, 0, 0, 1), so the channels beyond the first are not
        /// merely degraded, they are GONE: this is `TextureIntent::Mask` and nothing else.
        BC4_UNORM,

        /// `VK_FORMAT_BC5_UNORM_BLOCK`. TWO channels — two BC4 blocks side by side, 4x4 texels in 16
        /// bytes, a half of RGBA8. It is the tangent-space normal format: X and Y are stored and Z is
        /// reconstructed by the shader as `sqrt(1 - x*x - y*y)`, which a unit-length tangent normal
        /// satisfies by construction. A sampler returns (R, G, 0, 1), so a shader that read `.rgb` and
        /// did `2*n - 1` would get a Z of -1 everywhere; the reconstruction is not an optimisation,
        /// it is what makes the format mean anything.
        BC5_UNORM,

        /// `VK_FORMAT_R16_UNORM`. One 16-bit channel — a landscape tile's height samples, uploaded as the
        /// CPU holds them (Engine/World/Landscape/LandscapeData.hpp). UNORM rather than UINT so the tile is
        /// an ordinary sampled image through the material's combined-sampler bindings; the shader reads it
        /// with texelFetch and recovers the exact integer sample, so no filtering ever touches it.
        /// APPENDED, not grouped with the other uncompressed formats: a cooked texture stores its format
        /// as this enumerator's number (TextureBinary.cpp), so an insertion would renumber every BC file.
        R16_UNORM,

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

    /// THE ONE TABLE EVERY BYTE COUNT IN THIS ENGINE IS DERIVED FROM, and the only one that is total
    /// over the enum. A format's storage is a BLOCK of @ref Width x @ref Height texels occupying
    /// @ref Bytes bytes; an uncompressed format is the degenerate case of a 1x1 block, which is what
    /// makes the same arithmetic serve both and what stops there being two arithmetics to keep in step.
    struct TexelBlock
    {
        uint32_t Width;  /// texels across one block
        uint32_t Height; /// texels down one block
        uint32_t Bytes;  /// bytes one whole block occupies
    };

    /// Total over ImageFormat, for the same reason and by the same mechanism as the lookups below: no
    /// `default:` label, nothing returned after the switch, and `LookupsAreTotal` constant-evaluates it
    /// for every enumerator, so a format added without a block breaks the BUILD.
    constexpr TexelBlock GetTexelBlock( ImageFormat format )
    {
        switch ( format )
        {
            case ImageFormat::RGBA8F:
                return { 1, 1, 4 }; // 4 channels, 8 bits each
            case ImageFormat::RGBA16F:
                return { 1, 1, 8 }; // 4 channels, 16 bits each
            case ImageFormat::RGBA32F:
                return { 1, 1, 16 }; // 4 channels, 32 bits each
            case ImageFormat::BGRA8F:
                return { 1, 1, 4 };
            case ImageFormat::DEPTH24STENCIL8:
                return { 1, 1, 4 }; // 24-bit depth + 8-bit stencil, packed into one 32-bit texel
            case ImageFormat::DEPTH32F:
                return { 1, 1, 4 };
            case ImageFormat::R16_UNORM:
                return { 1, 1, 2 }; // one channel, 16 bits
            // THREE OF THE FOUR BLOCK FORMATS ARE SIXTEEN BYTES AND ONE IS EIGHT, which is why the
            // number is a column of this table and not a constant beside it. The comment here used to
            // say "both BC formats in this engine are the same shape"; BC4 made that sentence false,
            // and a `kBytesPerBlock = 16` written anywhere would have stayed true-looking and been
            // wrong for a quarter of the table. The census has a gate for exactly that shape.
            case ImageFormat::BC7_UNORM:
                return { 4, 4, 16 };
            case ImageFormat::BC6H_UFLOAT:
                return { 4, 4, 16 };
            case ImageFormat::BC4_UNORM:
                return { 4, 4, 8 }; // one channel: two 8-bit endpoints and sixteen 3-bit indices
            case ImageFormat::BC5_UNORM:
                return { 4, 4, 16 }; // two channels: two BC4 blocks, red then green
            case ImageFormat::Count:
                break; // the sentinel is not a format — fall through to the error path below
        }

        LOG_ERROR( "GetTexelBlock: ImageFormat value {} is outside the enumeration",
                   static_cast<uint32_t>( format ) );
        DESERT_VERIFY( false, "ImageFormat outside the enumeration" );
    }

    /// Does one texel of this format have a size of its own? False exactly for the block formats, and
    /// DERIVED from the block table rather than listed again, so a format cannot be block-compressed in
    /// one answer and not in the other.
    constexpr bool IsBlockCompressed( ImageFormat format )
    {
        const TexelBlock block = GetTexelBlock( format );
        return block.Width > 1 || block.Height > 1;
    }

    /// HOW MANY CHANNELS SURVIVE THIS FORMAT, counted from red. Four for every uncompressed format in
    /// this table and for BC7; three for BC6H, which has no alpha; two for BC5 and one for BC4.
    ///
    /// IT EXISTS BECAUSE THE MEASUREMENT NEEDED IT AND WOULD OTHERWISE HAVE LIED. The cook grades a
    /// block encode by decoding it and comparing with the image it came from. Compared over all four
    /// channels, a correct BC4 encode of an opacity mask scores near zero decibels — the green, blue
    /// and alpha it was never asked to keep are all wrong — so the gate would refuse every format
    /// narrower than the source and the authored intent could never take effect. The comparison has to
    /// know what the format PROMISED, and this is that promise in one place rather than at each site
    /// that grades or decodes.
    constexpr uint32_t PreservedChannelCount( ImageFormat format )
    {
        switch ( format )
        {
            case ImageFormat::RGBA8F:
            case ImageFormat::RGBA16F:
            case ImageFormat::RGBA32F:
            case ImageFormat::BGRA8F:
            case ImageFormat::BC7_UNORM:
                return 4;
            // A depth or depth+stencil image is not sampled as colour channels at all; one is the
            // honest answer and the alternative is a zero that would multiply into an empty buffer.
            case ImageFormat::DEPTH24STENCIL8:
            case ImageFormat::DEPTH32F:
                return 1;
            case ImageFormat::R16_UNORM:
                return 1;
            case ImageFormat::BC6H_UFLOAT:
                return 3; // radiance; the format has no alpha at all
            case ImageFormat::BC5_UNORM:
                return 2; // X and Y of a tangent normal; Z is reconstructed by the shader
            case ImageFormat::BC4_UNORM:
                return 1;
            case ImageFormat::Count:
                break; // the sentinel is not a format — fall through to the error path below
        }

        LOG_ERROR( "PreservedChannelCount: ImageFormat value {} is outside the enumeration",
                   static_cast<uint32_t>( format ) );
        DESERT_VERIFY( false, "ImageFormat outside the enumeration" );
    }

    /// How many whole blocks a row of @p width texels occupies. ROUNDED UP: a level three texels wide
    /// is one block wide, not three quarters of one, and this is where a block format's arithmetic
    /// differs from an uncompressed one's at all. The smallest mips are where it matters — a 1x1 level
    /// of BC7 is 16 bytes, the same as a 4x4 one.
    constexpr uint32_t BlocksAcross( uint32_t width, ImageFormat format )
    {
        const uint32_t blockWidth = GetTexelBlock( format ).Width;
        return ( width + blockWidth - 1 ) / blockWidth;
    }

    /// How many whole blocks a column of @p height texels occupies. Rounded up, see `BlocksAcross`.
    constexpr uint32_t BlocksDown( uint32_t height, ImageFormat format )
    {
        const uint32_t blockHeight = GetTexelBlock( format ).Height;
        return ( height + blockHeight - 1 ) / blockHeight;
    }

    /// Bytes from the start of one ROW OF BLOCKS to the next — `VkBufferImageCopy`'s `bufferRowLength`
    /// question, and the `RowPitch` column the cooked container has carried since v1 for this exact day.
    /// For an uncompressed format it is `width * bytes-per-pixel`; for a block format it is neither a
    /// function of the width alone nor of the bytes-per-pixel, because there is no bytes-per-pixel.
    constexpr uint32_t CalculateRowPitch( uint32_t width, ImageFormat format )
    {
        return BlocksAcross( width, format ) * GetTexelBlock( format ).Bytes;
    }

    /// Bytes per PIXEL — and it is an error to ask it about a block format, rather than a number.
    ///
    /// WHY IT REFUSES INSTEAD OF ANSWERING. Sixteen bytes per 4x4 block is one byte per texel, and
    /// returning `1` here would make every `width * height * bpp` in the tree produce a number that is
    /// right for a 2048-wide level and WRONG for every level under four texels — which is the resident
    /// tail of every texture, i.e. the part that is always loaded. A quiet factor-of-sixteen error in
    /// the four smallest levels is the "middle link drops a property" shape, and the only defence that
    /// does not rely on everyone remembering is for the question to be unanswerable.
    ///
    /// The refusal is a COMPILE error wherever it is reached in a constant expression (falling off the
    /// end of a constexpr function is ill-formed) and a `DESERT_VERIFY` at run time. It is still here,
    /// rather than deleted, because two things legitimately want it: the box filter, which works in
    /// whole texels and refuses block formats by name, and the refusal messages that quote it.
    constexpr uint32_t GetBytesPerPixel( ImageFormat format )
    {
        const TexelBlock block = GetTexelBlock( format );
        if ( block.Width == 1 && block.Height == 1 )
            return block.Bytes;

        LOG_ERROR( "GetBytesPerPixel: ImageFormat value {} is block-compressed and has no bytes per pixel; "
                   "ask GetTexelBlock, CalculateRowPitch or CalculateImageSize instead",
                   static_cast<uint32_t>( format ) );
        DESERT_VERIFY( false, "a block-compressed format has no bytes per pixel" );
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
            case ImageFormat::R16_UNORM:
            case ImageFormat::BC7_UNORM:
            case ImageFormat::BC6H_UFLOAT:
            case ImageFormat::BC4_UNORM:
            case ImageFormat::BC5_UNORM:
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

                // The block table is the one asked for EVERY format, because it is the one every byte
                // count is derived from. A format without a case here falls off the end of a constexpr
                // function during this evaluation, which is ill-formed — a compile error, in any
                // configuration and under any warning settings.
                const TexelBlock block = GetTexelBlock( format );
                if ( block.Width == 0 || block.Height == 0 || block.Bytes == 0 )
                    return false;
                if ( GetImageAspect( format ) == 0 )
                    return false;
                // A format that preserved no channel would size a comparison over nothing and grade
                // every encode as perfect. Asked for every enumerator, so a format added without a
                // case falls off the end of a constexpr function here.
                if ( PreservedChannelCount( format ) == 0 )
                    return false;

                // GetBytesPerPixel IS ASKED ONLY WHERE A PIXEL HAS A SIZE, and where it does, the two
                // tables are made to AGREE here rather than merely both existing. A block format is
                // deliberately not asked: asking would be the ill-formed reach described on that
                // function, which would turn its refusal into a build failure for the whole engine.
                if ( !IsBlockCompressed( format ) && GetBytesPerPixel( format ) != block.Bytes )
                    return false;
            }
            return true;
        }
    } // namespace Detail

    static_assert( Detail::LookupsAreTotal(),
                   "Every ImageFormat enumerator needs a case in GetTexelBlock, GetImageAspect and "
                   "PreservedChannelCount, an uncompressed one needs a GetBytesPerPixel that AGREES "
                   "with its block, and kImageFormatCount must count them all." );

    // Byte size of a tightly-packed image. 64-bit because a volume is easy to size past 4 GiB, and a
    // silently truncated allocation size belongs to the same family of bugs as a zero bytes-per-pixel.
    ///
    /// IT IS WRITTEN IN BLOCKS, AND THAT IS WHAT MAKES ALL 28 CENSUSED CALL SITES BLOCK-CORRECT AT ONCE.
    /// For an uncompressed format `BlocksAcross(w) * blockBytes` IS `w * bytes-per-pixel` and
    /// `BlocksDown(h)` IS `h`, so not one existing number moves; for a block format the rounding up to
    /// whole blocks happens here, once, instead of at every site that would have had to remember it.
    constexpr uint64_t CalculateImageSize( uint32_t width, uint32_t height, ImageFormat format )
    {
        return static_cast<uint64_t>( CalculateRowPitch( width, format ) ) * BlocksDown( height, format );
    }

    /// A VOLUME. Block compression is 2D — a BC block spans one slice — so the depth multiplies whole
    /// slices and is not rounded. No block format is legal on a 3D image in this engine anyway (there
    /// is no BC volume in the tree and `Image3DSpecification` carries no mips either), but the
    /// arithmetic says which of the three extents a block covers rather than leaving it to be inferred.
    constexpr uint64_t CalculateImageSize( uint32_t width, uint32_t height, uint32_t depth, ImageFormat format )
    {
        return CalculateImageSize( width, height, format ) * depth;
    }

    // ── THE SECOND MULTIPLIER, AND WHY IT IS A SEPARATE NAME ───────────────────────────────────────
    //
    // `CalculateImageSize` answers for ONE image. Everything above assumed that was the only kind there
    // is, because until the cooked container grew a layer count it was: a cube existed on the device
    // (six `arrayLayers`, `VulkanImage.cpp`) and nowhere else, and the ONE place that charged for its
    // bytes open-coded the six and its own bytes-per-pixel — `SkyRules.hpp` carried
    // `kSkyEnvBytesPerPixel = 16` beside `6ull * side * side`, a hand-written duplicate of
    // `GetBytesPerPixel( RGBA32F )` that nothing made agree with it.
    //
    // A LAYER IS NOT A DEPTH SLICE, so this is not the 3D overload wearing a different name. A volume
    // is one image a shader samples with three coordinates and it has ONE mip chain over all three
    // extents; layers are separate images that share a chain over two. Giving them one function would
    // make `CalculateImageSize(w, h, 6, fmt)` mean both, and the day a 3D format pads its slices
    // differently from its layers the two answers separate with nothing to separate them by.
    constexpr uint64_t CalculateLayeredImageSize( uint32_t width, uint32_t height, uint32_t layers,
                                                  ImageFormat format )
    {
        return CalculateImageSize( width, height, format ) * layers;
    }

    // Bytes of a whole cube: six square faces, @p mips levels, each level half the last (min 1).
    // DERIVED FROM THE FACE, like everything else about a cube in this engine — see
    // `ImageCubeSpecification::FaceSize` for the three defects the cross-unwrap arithmetic produced.
    // How many array layers a cube has. Six, and the number is written down ONCE so that the places
    // which loop over faces, size a cube and build its copy regions cannot each carry their own.
    inline constexpr uint32_t kImageCubeLayerCount = 6;

    constexpr uint64_t CalculateCubeImageSize( uint32_t faceSize, uint32_t mips, ImageFormat format )
    {
        uint64_t bytes = 0;
        for ( uint32_t mip = 0; mip < mips; ++mip )
        {
            const uint32_t side = faceSize >> mip > 1u ? faceSize >> mip : 1u;
            bytes += CalculateLayeredImageSize( side, side, kImageCubeLayerCount, format );
        }
        return bytes;
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
        // `Image2D::Create` (itself deleted by T3.3, when its last user — the source-file `Texture2D`
        // path — went: every 2D chain now comes from this table). A setting whose only branch is never taken is
        // the dead knob §3 forbids, and it would have become a lie the day a block-compressed format arrived:
        // `blitDst=0` for BC1/BC4/BC5/BC7 on this device, so there is no blit chain to ask for.
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

        // THE CHAIN THE CALLER ALREADY HAS, indexed by `level * 6 + face`. Same contract as
        // `Image2DSpecification::MipLevels` and the same reason for existing, with one difference that
        // is the whole point of it: a cube's level is SIX images, so a table of one span per level could
        // not address them and a `Data` blob without a table could not be split into them.
        //
        // IT IS WHAT MADE `Data` MEAN ANYTHING ON A CUBE AT ALL. Before this the backend's `UploadData`
        // for cubes was an empty function body: every caller that put pixels in `Data` — `TextureCube`,
        // the fallback cube — had them silently dropped, and the fallback had to be filled by a separate
        // `RT_ClearToColor` to say anything at all. Both ends looked right; the link between them threw
        // the pixels away.
        //
        // Empty means "no pixels, the image is for a compute pass to fill", which is what every cube in
        // this engine was until a baked environment could be read off disk.
        std::vector<MipLevelSpan> Levels;
    };

    // A volume texture — the shape/detail noise the volumetric passes sample, and any other
    // compute-generated 3D field.
    //
    // There is deliberately NO `Mips` field. The engine has no 3D mip generator (MipMapCubeGenerator has no
    // 3D counterpart, and blitting a volume chain is a separate piece of
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