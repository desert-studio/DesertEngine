// EVERY PLACE THIS ENGINE TURNS AN EXTENT INTO A BYTE COUNT, AND WHAT EACH ONE ASSUMES.
//
// WHY A CENSUS AND NOT A UNIT TEST. `Core::Formats::CalculateImageSize` is `width * height *
// bytes-per-pixel`, and for ten years of this codebase that was simply what an image cost, because every
// image was one layer of one uncompressed 2D surface. Two things break the identity, in the same way and
// at the same places:
//
//   * LAYERS. A cube is six images sharing a level, and the cooked container can now say so (v3). The
//     product above is then the size of ONE FACE, and a call site that meant "the whole image" is short
//     by 6x. It is not a compile error, it is not a validation error, and at a `vkCmdCopyBufferToImage`
//     it is five faces of undefined memory.
//   * BLOCKS. A BC7 level has no bytes-per-PIXEL at all: it has 16 bytes per 4x4 block, and a level
//     narrower than four texels still occupies a whole block. That is the next step of
//     `Docs/World/PROGRAMME.md` §5 and it lands on exactly this set of lines.
//
// WHY THE BRIEF'S TWO NUMBERS WERE NOT THE CENSUS, measured at a5911014 over the source text with
// comments and string literals removed: `GetBytesPerPixel` occurs 6 times and `CalculateImageSize` 15 --
// and the union of the two MISSES the one site in the tree that already multiplied by a face count.
// `SkyRules.hpp` wrote `bytes += 6ull * side * side * kSkyEnvBytesPerPixel`, with a hand-typed 16 beside
// it that nothing made agree with `GetBytesPerPixel( RGBA32F )`. A census by identifier could not find
// the site that spelled neither identifier, which is why that arithmetic was moved into
// `CalculateCubeImageSize` and why this file exists to keep it there.
//
// WHY A SOURCE-TEXT CENSUS AND NOT A RUNTIME ONE. A site this machine never executes is still a site,
// and the shape being looked for is invisible until the day something has six layers. The same reason
// `ArgumentOrder` reads source text: the defect is a property of what is WRITTEN.
//
// HOW IT IS HELD. One row per CALL EXPRESSION -- not a count, and not a per-file tally. A count can be
// satisfied by editing the count; a row has to be read, and every row carries what that site sizes. The
// totals below are DERIVED from the rows and then confirmed against the scan, in both directions: a new
// call is a row nobody wrote, and a deleted call is a row that no longer matches anything.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /// Comments AND string literals become spaces. Both matter here: the prose in these files is mostly
    /// about this very subject, and `LOG_ERROR( "GetBytesPerPixel: ..." )` is a message, not a call.
    std::string StripCommentsAndStrings( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/' )
            {
                while ( i < src.size() && src[i] != '\n' )
                {
                    out.push_back( ' ' );
                    ++i;
                }
            }
            else if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*' )
            {
                out.append( "  " );
                i += 2;
                while ( i + 1 < src.size() && !( src[i] == '*' && src[i + 1] == '/' ) )
                {
                    out.push_back( src[i] == '\n' ? '\n' : ' ' );
                    ++i;
                }
                out.append( "  " );
                i += 2;
            }
            else if ( src[i] == '"' )
            {
                out.push_back( ' ' );
                ++i;
                while ( i < src.size() && src[i] != '"' )
                {
                    if ( src[i] == '\\' )
                        ++i;
                    if ( i < src.size() )
                        out.push_back( src[i] == '\n' ? '\n' : ' ' );
                    ++i;
                }
                if ( i < src.size() )
                {
                    out.push_back( ' ' );
                    ++i;
                }
            }
            else
            {
                out.push_back( src[i] );
                ++i;
            }
        }
        return out;
    }

    /// The whole call, parentheses balanced, whitespace normalised to single spaces. That is the key a
    /// row is written against: it survives a line break moving and it changes when the ARGUMENTS change,
    /// which is precisely when somebody has to look at the row again.
    std::string CallExpression( const std::string& src, const std::size_t at, const std::size_t nameLength )
    {
        std::size_t j = at + nameLength;
        while ( j < src.size() && ( src[j] == ' ' || src[j] == '\t' || src[j] == '\n' ) )
            ++j;
        if ( j >= src.size() || src[j] != '(' )
            return {};

        int         depth = 0;
        std::size_t k     = j;
        for ( ; k < src.size(); ++k )
        {
            if ( src[k] == '(' )
                ++depth;
            else if ( src[k] == ')' && --depth == 0 )
                break;
        }
        if ( k >= src.size() )
            return {};

        std::string out;
        bool        space = false;
        for ( std::size_t p = at; p <= k; ++p )
        {
            const char c = src[p];
            if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' )
            {
                space = true;
                continue;
            }
            if ( space && !out.empty() )
                out.push_back( ' ' );
            space = false;
            out.push_back( c );
        }
        return out;
    }

    /// THE WAYS A BYTE COUNT CAN BE REACHED. Every one of them ends in `GetBytesPerPixel`, which is what
    /// makes this list complete rather than merely long: there is no other source of a bytes-per-pixel in
    /// the tree, and the format table's own totality guard stops a format from appearing without one.
    ///
    /// THE BLOCK ENTRY POINTS JOINED IT WHEN BLOCKS ARRIVED, and they had to: after that change
    /// `CalculateImageSize` no longer calls `GetBytesPerPixel` at all — it is `CalculateRowPitch` times
    /// `BlocksDown` — so a census that watched only the old five would have watched the arithmetic's
    /// former self. The claim "every one of them ends in GetBytesPerPixel" is now false and the true
    /// one is that every one of them ends in `GetTexelBlock`, which is the table the format model's
    /// own totality guard makes mandatory for every enumerator.
    constexpr const char* kEntryPoints[] = { "GetBytesPerPixel",
                                             "CalculateImageSize",
                                             "CalculateLayeredImageSize",
                                             "CalculateCubeImageSize",
                                             "TightlyPackedChainBytes",
                                             "GetTexelBlock",
                                             "CalculateRowPitch",
                                             "BlocksAcross",
                                             "BlocksDown" };

    constexpr const char* kSearchedRoots[] = { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source",
                                               "Runtime/Source", "Tools" };

    struct Site
    {
        std::string File;
        std::string Expression;

        bool operator<( const Site& other ) const
        {
            return File != other.File ? File < other.File : Expression < other.Expression;
        }
    };

    std::set<Site> Scan()
    {
        const std::string root = RepoRoot();
        EXPECT_FALSE( root.empty() ) << "the census cannot find the repository from the working directory";

        std::set<Site> found;
        for ( const char* searched : kSearchedRoots )
        {
            std::error_code             ec;
            const std::filesystem::path base = root + searched;
            if ( !std::filesystem::exists( base, ec ) )
                continue;

            for ( const auto& entry : std::filesystem::recursive_directory_iterator( base, ec ) )
            {
                if ( !entry.is_regular_file() )
                    continue;
                const std::string ext = entry.path().extension().string();
                if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" )
                    continue;
                if ( entry.path().string().find( "ThirdParty" ) != std::string::npos )
                    continue;

                const std::string text     = StripCommentsAndStrings( ReadAll( entry.path().string() ) );
                std::string       relative = entry.path().string().substr( root.size() );

                for ( const char* name : kEntryPoints )
                {
                    const std::string_view needle( name );
                    for ( std::size_t at = text.find( needle ); at != std::string::npos;
                          at             = text.find( needle, at + 1 ) )
                    {
                        const std::string call = CallExpression( text, at, needle.size() );
                        if ( !call.empty() )
                            found.insert( Site{ relative, call } );
                    }
                }
            }
        }
        return found;
    }

    /// What the site's product MEANS. The verdict is the thing a reader needs when layers or blocks
    /// arrive: it says whether this line already accounts for them, or is one image on purpose.
    enum class Sizes
    {
        /// The block table itself, or a query of it that is not a byte count on its own.
        TheBlockTable,
        /// The definition of the arithmetic itself. It lives in the format table and a change to layers
        /// or blocks starts here.
        TheArithmeticItself,
        /// One image, and one image is the right answer: a 2D render target, a LUT, a single level.
        OneImage,
        /// A whole layered image -- the layer count is an argument, so faces are already in the product.
        EveryLayer,
    };

    /// WHAT HAPPENS TO THIS SITE WHEN THE FORMAT IS A BC7 OR A BC6H. The question the register's
    /// failure message has always asked is now a COLUMN, because a block format is no longer
    /// hypothetical: two of them are in the table, one of them is what every cooked LDR texture that
    /// clears the cook's gates is stored as, and one is what every baked environment cube is stored as.
    enum class Blocks
    {
        /// The site is written in blocks and is right for both kinds. Every `CalculateImageSize`,
        /// `CalculateRowPitch` and `CalculateLayeredImageSize` is here by construction: for a 1x1 block
        /// they reduce to the products they always were.
        Correct,
        /// The site CANNOT take a block format and says so BY NAME before it reaches this line. There
        /// is exactly one such place and it is the box filter, which works in whole texels.
        RefusedByName,
    };

    struct Row
    {
        const char* File;
        const char* Expression;
        Sizes       What;
        Blocks      WithBlocks;
        const char* Why;
    };

    // ----------------------------------------------------------------------------------------------
    // THE REGISTER. One row per call expression, sorted by file. The counts are derived from it below.
    // Kept out of the formatter's hands for the same reason the pointer register is: one row is four
    // lines and reflowing packs several onto one, which makes the list unreadable as a list.
    // clang-format off
    const std::vector<Row>& Register()
    {
        static const std::vector<Row> rows = {
        { "Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
          "CalculateImageSize( width, height, sourceFormat )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the encoder checking the buffer it was handed against the image it was told about; the "
          "source is uncompressed here by construction, and the check is what stops a short buffer "
          "being read past" },
        { "Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
          "BlocksAcross( width, blockFormat )", Sizes::TheBlockTable, Blocks::Correct,
          "how many blocks a row of the OUTPUT has -- the loop bound of the encoder itself, so a "
          "level of six texels encodes two blocks and not one and a half" },
        { "Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
          "BlocksDown( height, blockFormat )", Sizes::TheBlockTable, Blocks::Correct,
          "the same in the other axis; together they are the only place the encoder decides how much "
          "work a level is" },
        { "Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
          "GetTexelBlock( blockFormat )", Sizes::TheBlockTable, Blocks::Correct,
          "the stride from one encoded block to the next, taken from the format table rather than "
          "from the 16 that a BPTC bit stream happens to be -- two different sixteens that must be "
          "able to disagree" },
        { "Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
          "CalculateImageSize( width, height, blockFormat )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the size of the encoder's OUTPUT, and of the decoder's input; it is the same function the "
          "container sizes its rows with, so an encode that produced a different length is caught "
          "where it happens" },
        { "Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp",
          "CalculateImageSize( width, height, destFormat )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the decoder's output, in the uncompressed format the blocks decode back to" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "GetTexelBlock( ImageFormat format )", Sizes::TheBlockTable, Blocks::Correct,
          "THE TABLE. Total over the enum with no default label and nothing after the switch, so a "
          "format added without a block breaks the BUILD; every other row in this register is derived "
          "from it" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "GetTexelBlock( format )", Sizes::TheBlockTable, Blocks::Correct,
          "the derived predicates and pitches inside the table's own file, plus the totality guard" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "BlocksAcross( uint32_t width, ImageFormat format )", Sizes::TheBlockTable, Blocks::Correct,
          "the definition of rounding UP to whole blocks, which is the one arithmetic difference "
          "between a block format and an uncompressed one and the reason a 1x1 mip is sixteen bytes" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "BlocksDown( uint32_t height, ImageFormat format )", Sizes::TheBlockTable, Blocks::Correct,
          "the same in the vertical axis; separate because a block need not be square, and a future "
          "format whose block is not would otherwise have one of its two extents silently wrong" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateRowPitch( uint32_t width, ImageFormat format )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the definition of a row's stride -- the column the container has carried since v1 for this "
          "day, and the number `VkBufferImageCopy` means by bufferRowLength" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "BlocksAcross( width, format )", Sizes::TheBlockTable, Blocks::Correct,
          "the row pitch's own body -- a pitch is a count of whole blocks times the bytes one block "
          "occupies, and it is the only place that product is written" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateRowPitch( width, format )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the 2D size's own body: a pitch times the number of block rows. THIS is the line that made "
          "all twenty-odd call sites below block-correct at once, because none of them had to change" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "BlocksDown( height, format )", Sizes::TheBlockTable, Blocks::Correct,
          "the other half of CalculateImageSize's body: how many rows of blocks a level has, which for "
          "an uncompressed format is its height and for a BC format is a quarter of it rounded up" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateImageSize( width, height, format )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the 3D and layered overloads, both of which multiply a 2D size by a count of whole slices "
          "or layers: a block spans one slice, so neither of those counts is ever rounded" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "GetBytesPerPixel( ImageFormat format )", Sizes::TheArithmeticItself, Blocks::RefusedByName,
          "bytes per PIXEL, which a block format does not have. It now REFUSES one -- a compile error "
          "in a constant expression and a DESERT_VERIFY at run time -- rather than answering 1, which "
          "would have been right for a 2048-wide level and wrong for every level under four texels" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "GetBytesPerPixel( format )", Sizes::TheArithmeticItself, Blocks::RefusedByName,
          "the totality guard, which asks it only where a pixel HAS a size and then requires the "
          "answer to AGREE with the block table -- so the two tables cannot drift apart" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateImageSize( uint32_t width, uint32_t height, ImageFormat format )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the 2D definition: ONE image of one layer, which is what it has always meant and what the "
          "layered entry point exists so it can go on meaning" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateImageSize( uint32_t width, uint32_t height, uint32_t depth, ImageFormat format )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the 3D definition. A depth slice is not a layer: a volume is one image sampled with three "
          "coordinates, so it is deliberately NOT the layered overload wearing another name" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateLayeredImageSize( uint32_t width, uint32_t height, uint32_t layers, ImageFormat " "format )", Sizes::TheArithmeticItself, Blocks::Correct,
          "the layered definition, added with the container's face count -- the second multiplier, "
          "named rather than open-coded at each site that needs it" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateCubeImageSize( uint32_t faceSize, uint32_t mips, ImageFormat format )", Sizes::TheArithmeticItself, Blocks::Correct,
          "a whole cube derived from its FACE, which is how every other cube quantity in this engine "
          "is derived; it replaced the only site in the tree that spelled its own six and its own "
          "sixteen" },
        { "Desert/Desert/Source/Engine/Core/Formats/ImageFormat.hpp",
          "CalculateLayeredImageSize( side, side, kImageCubeLayerCount, format )", Sizes::EveryLayer, Blocks::Correct,
          "the cube sum's own body, one mip level at a time; the six is the named constant and not a "
          "literal, so the places that loop over faces and the place that charges for them agree" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "GetBytesPerPixel( format )", Sizes::OneImage, Blocks::RefusedByName,
          "the BOX FILTER, and the one legitimate caller left. It refuses every format but RGBA8F and "
          "RGBA32F BY NAME, three lines above this call -- it averages whole texels and a block is "
          "not one, which is why a block chain is built by encoding an uncompressed chain rather than "
          "by filtering blocks" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "GetTexelBlock( format )", Sizes::TheBlockTable, Blocks::Correct,
          "the refusal messages in BOTH chain builders and in the decoder, which quote the block "
          "instead of a bytes-per-pixel -- a message that explained a size disagreement by asking a "
          "question that ABORTS on block formats would crash on exactly the files the disagreement is "
          "likeliest for" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "CalculateRowPitch( extents[level].first, format )", Sizes::OneImage, Blocks::Correct,
          "the RowPitch column, written by both chain builders and checked by the decoder. For an "
          "uncompressed format it is width times bytes-per-pixel exactly as before" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "BlocksAcross( extents[level].first, format )", Sizes::TheBlockTable, Blocks::Correct,
          "the decoder's row-pitch refusal, which says how many BLOCKS a row is rather than how many "
          "bytes a texel is" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "CalculateImageSize( extents[level].first, extents[level].second, format )", Sizes::OneImage, Blocks::Correct,
          "one (level, layer) image -- the size BuildLevelTable places and the size the decoder "
          "demands of a row. The layer count multiplies them in TightlyPackedChainBytes rather than "
          "here" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "CalculateImageSize( extents[level].first, extents[level].second, sourceFormat )", Sizes::OneImage, Blocks::Correct,
          "BlockCompressChain checking each row of the chain it was handed before encoding it; the "
          "source is the uncompressed format, and this is what catches a table that does not describe "
          "its own pixels" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "CalculateLayeredImageSize( w, h, layerCount, format )", Sizes::EveryLayer, Blocks::Correct,
          "TightlyPackedChainBytes' own body, one level at a time. It used to be `w * h * bpp * "
          "layerCount` and is now a call, which is what makes the container's totals block-correct" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "TightlyPackedChainBytes( const uint32_t width, const uint32_t height, const uint32_t " "levelCount, const uint32_t layerCount, const Core::Formats::ImageFormat format )", Sizes::EveryLayer, Blocks::Correct,
          "the definition: every level of every layer, tightly packed. This is the size a GPU "
          "readback is allocated with and the size BuildLevelTable demands, so the two cannot drift" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "TightlyPackedChainBytes( width, height, levelCount, layerCount, format )", Sizes::EveryLayer, Blocks::Correct,
          "BuildLevelTable checking the input it was handed against the shape it was told about; a "
          "face short here is the exact mistake a readback that forgot a layer makes" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp",
          "TightlyPackedChainBytes( width, height, levelCount, layerCount, blockFormat )", Sizes::EveryLayer, Blocks::Correct,
          "BlockCompressChain checking its OUTPUT against the same function BuildLevelTable will "
          "check it against, here, where both numbers are still in hand -- a block chain that came "
          "out a different length is the one mistake that step can make quietly" },
        { "Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.hpp",
          "TightlyPackedChainBytes( uint32_t width, uint32_t height, uint32_t levelCount, uint32_t " "layerCount, Core::Formats::ImageFormat format )", Sizes::EveryLayer, Blocks::Correct,
          "the declaration of the same; it is public because a caller reading a cube off the device "
          "has to size the readback before it has anything to hand in" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanImage.cpp",
          "CalculateImageSize( m_Specification.Width, m_Specification.Height, m_Specification.Format " ")", Sizes::OneImage, Blocks::Correct,
          "VulkanImage2D's staging buffer for a single-level 2D upload (CreateResource and SetData); "
          "a 2D image has one layer by construction in this backend. A cooked BC7 texture never takes "
          "this branch -- it always supplies a chain -- but the size would be right if it did" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanImage.cpp",
          "CalculateImageSize( w, h, fmt )", Sizes::OneImage, Blocks::Correct,
          "ReadPixelsRGBA8's readback buffer -- one 2D level of one image, converted to RGBA8 "
          "afterwards. The pack it feeds refuses every format it has no swizzle for, block formats "
          "included" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanImage.cpp",
          "CalculateImageSize( side, side, m_Specification.Format )", Sizes::OneImage, Blocks::Correct,
          "ONE FACE of one mip in VulkanImageCube::RT_ReadAllLevels, which is the right unit there: "
          "the function advances the buffer offset per face and per level, so the product it wants is "
          "the single image a copy region covers" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanImage.cpp",
          "CalculateImageSize( m_Specification.Width, m_Specification.Height, m_Specification.Depth, " "m_Specification.Format )", Sizes::OneImage, Blocks::Correct,
          "VulkanImage3D's staging buffer; a volume is one image and has no layers "
          "(Image3DSpecification deliberately has no Mips field either), and no block format is legal "
          "on one" },
        { "Desert/Desert/Source/Engine/Graphic/Environment/EnvironmentBake.cpp",
          "TightlyPackedChainBytes( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount, " "kCubeFormat )", Sizes::EveryLayer, Blocks::Correct,
          "the bake asserting that what came back off the device is the shape it is about to place: a "
          "disagreement here is the middle link dropping a property, so it is a refusal with both "
          "numbers rather than a silent truncation. kCubeFormat is the COMPUTE format -- a storage "
          "image cannot be a block format -- and the encode to BC6H happens after this" },
        { "Desert/Desert/Source/Engine/Graphic/Environment/EnvironmentBake.cpp",
          "CalculateCubeImageSize( faceSize, mips, data.Format )", Sizes::EveryLayer, Blocks::Correct,
          "what a cube read off the cache costs RESIDENT, logged where it is paid. The format is the "
          "FILE's, not a constant, because that is the whole point: the same cube is 128 MiB as "
          "RGBA32F and 8 MiB as BC6H, and a figure quoted from a constant would have gone on saying "
          "128" },
        { "Desert/Desert/Source/Engine/Graphic/SkyRules.hpp",
          "CalculateCubeImageSize( faceSize, mips, kSkyEnvCubeFormat )", Sizes::EveryLayer, Blocks::Correct,
          "what one IBL cube costs to COMPUTE, for the bake's cost report. THIS is the line that used "
          "to open-code 6ull * side * side * kSkyEnvBytesPerPixel with a hand-typed 16. It still "
          "names the compute format, which is still RGBA32F -- the cached cube's resident cost is "
          "EnvironmentBake's row above, and the two are deliberately different numbers" },
        { "Desert/Desert/Source/Engine/Graphic/SkyRules.hpp",
          "CalculateImageSize( size.Width, size.Height, kSkyEnvCubeFormat )", Sizes::OneImage, Blocks::Correct,
          "the equirect panorama the cubes are convolved from -- a 2D image, and the format is named "
          "rather than its size, for the same reason the line above it stopped naming 16" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
          "CalculateImageSize( resolution, resolution, Core::Formats::ImageFormat::RGBA32F )", Sizes::OneImage, Blocks::Correct,
          "a cloud noise target's cost line; one 2D image, and a render target can never be a block "
          "format" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
          "CalculateImageSize( traceWidth, traceHeight, Core::Formats::ImageFormat::RGBA16F )", Sizes::OneImage, Blocks::Correct,
          "the full-resolution cloud trace target's cost line; one 2D image" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
          "CalculateImageSize( halfWidth, halfHeight, Core::Formats::ImageFormat::RGBA16F )", Sizes::OneImage, Blocks::Correct,
          "the half-resolution cloud trace target's cost line; one 2D image" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Fog/HeightFogRenderer.cpp",
          "CalculateImageSize( width, height, Core::Formats::ImageFormat::RGBA16F )", Sizes::OneImage, Blocks::Correct,
          "the height fog targets' cost lines; 2D images" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",
          "CalculateImageSize( kTransmittanceLutWidth, kTransmittanceLutHeight, " "Core::Formats::ImageFormat::RGBA16F )", Sizes::OneImage, Blocks::Correct,
          "the transmittance LUT's cost line; a 2D lookup table" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",
          "CalculateImageSize( kMultiScatterLutSize, kMultiScatterLutSize, " "Core::Formats::ImageFormat::RGBA16F )", Sizes::OneImage, Blocks::Correct,
          "the multiple-scattering LUT's cost line; a 2D lookup table" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",
          "CalculateImageSize( kSkyViewLutWidth, kSkyViewLutHeight, " "Core::Formats::ImageFormat::RGBA16F )", Sizes::OneImage, Blocks::Correct,
          "the sky-view LUT's cost line; a 2D lookup table" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",
          "CalculateImageSize( kAerialPerspectiveWidth, kAerialPerspectiveHeight, " "kAerialPerspectiveDepth, Core::Formats::ImageFormat::RGBA16F )", Sizes::OneImage, Blocks::Correct,
          "the aerial perspective VOLUME's cost line -- three extents, not three layers" },
        };
        return rows;
    }
    // clang-format on

    const Row* FindRow( const Site& site )
    {
        for ( const Row& row : Register() )
        {
            if ( site.File == row.File && site.Expression == row.Expression )
                return &row;
        }
        return nullptr;
    }
} // namespace

TEST( ImageByteSizeCensus, EverySiteThatTurnsAnExtentIntoBytesHasARow )
{
    const std::set<Site> scanned = Scan();
    ASSERT_FALSE( scanned.empty() ) << "the scan found nothing, so it is measuring the wrong tree";

    for ( const Site& site : scanned )
    {
        EXPECT_NE( FindRow( site ), nullptr )
             << "\n  " << site.File << "\n  " << site.Expression
             << "\nturns an extent into a byte count and no row in "
                "Desert/Tests/Engine/ImageByteSizeCensus says what it sizes."
                "\nAdd one. The question it has to answer is: when this image has SIX LAYERS, or when its "
                "format is a BLOCK format, is this product still the number this site wanted?";
    }
}

TEST( ImageByteSizeCensus, TheRegisterDescribesSitesThatStillExist )
{
    const std::set<Site> scanned = Scan();
    for ( const Row& row : Register() )
    {
        const bool alive = scanned.count( Site{ row.File, row.Expression } ) != 0;
        EXPECT_TRUE( alive ) << "\n  " << row.File << "\n  " << row.Expression
                             << "\nis a row for a call that is no longer in the tree (or whose arguments "
                                "changed). A register that keeps rows for code that left stops being a "
                                "description of the code.";
    }
}

TEST( ImageByteSizeCensus, EveryRowCarriesAnArgumentAndTheTotalsAreDerived )
{
    std::map<Sizes, int> tally;
    for ( const Row& row : Register() )
    {
        EXPECT_GT( std::string( row.Why ).size(), 30u )
             << row.File << " / " << row.Expression << " has a row but not an argument";
        ++tally[row.What];
    }

    // DERIVED, NOT PINNED. The totals come out of the rows above and are compared with the scan, so the
    // only way to move a number here is to add or remove a row -- which means writing or deleting a
    // sentence about what that site sizes.
    const int total = tally[Sizes::TheBlockTable] + tally[Sizes::TheArithmeticItself] + tally[Sizes::OneImage] +
                      tally[Sizes::EveryLayer];
    EXPECT_EQ( static_cast<std::size_t>( total ), Register().size() );
    EXPECT_EQ( static_cast<std::size_t>( total ), Scan().size() );

    // Every verdict has to occur, or the distinction the register is built on is not being exercised.
    EXPECT_GT( tally[Sizes::TheBlockTable], 0 );
    EXPECT_GT( tally[Sizes::OneImage], 0 );
    EXPECT_GT( tally[Sizes::EveryLayer], 0 );

    std::cout << "[  CENSUS  ] " << total << " site(s): " << tally[Sizes::TheBlockTable]
              << " querying the block table, " << tally[Sizes::TheArithmeticItself] << " defining the arithmetic, "
              << tally[Sizes::OneImage] << " sizing one image, " << tally[Sizes::EveryLayer]
              << " sizing every layer.\n";
}

TEST( ImageByteSizeCensus, EveryRowSaysWhatHappensToItWhenTheFormatIsABlock )
{
    // THE COLUMN THE BLOCK FORMATS ADDED. Before them the question "and when this is BC7?" was a
    // sentence in the failure message below; two block formats exist now, one of them is what a cooked
    // LDR texture is stored as and the other is what every baked environment cube is stored as, so the
    // answer is a field every row has to fill in.
    //
    // A ROW MAY SAY `RefusedByName`, and the whole value of the column is that it may. The box filter
    // cannot average blocks and must not pretend to; what it may NOT do is take a block format and
    // produce a plausible number. There is exactly one function like that in the tree and its refusal
    // is three lines above the call.
    int correct = 0, refused = 0;
    for ( const Row& row : Register() )
        ( row.WithBlocks == Blocks::Correct ? correct : refused )++;

    EXPECT_EQ( correct + refused, static_cast<int>( Register().size() ) );
    EXPECT_GT( correct, 0 );
    EXPECT_GT( refused, 0 ) << "if nothing refuses a block format any more, the refusal has been routed "
                               "around rather than removed -- check GetBytesPerPixel and the box filter";

    std::cout << "[  CENSUS  ] " << correct << " site(s) block-correct, " << refused
              << " refusing a block format by name.\n";
}

TEST( ImageByteSizeCensus, NobodyOutsideTheFormatTableSpellsItsOwnBytesPerPixel )
{
    // THE SHAPE THE IDENTIFIER CENSUS ABOVE CANNOT SEE, and the one that was actually in the tree.
    // `kSkyEnvBytesPerPixel = 16` named no entry point, so no search for `GetBytesPerPixel` or
    // `CalculateImageSize` would ever have reached it -- and it was the ONLY site that already
    // multiplied by a face count. A constant whose NAME says bytes-per-pixel and whose value is a
    // literal is a second copy of the format table with nothing holding the two together.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> offenders;
    for ( const char* searched : kSearchedRoots )
    {
        std::error_code             ec;
        const std::filesystem::path base = root + searched;
        if ( !std::filesystem::exists( base, ec ) )
            continue;

        for ( const auto& entry : std::filesystem::recursive_directory_iterator( base, ec ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const std::string ext = entry.path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" )
                continue;
            if ( entry.path().string().find( "ThirdParty" ) != std::string::npos )
                continue;
            // The format table is where the answer is allowed to be a literal; that is what it is for.
            if ( entry.path().string().find( "Core/Formats/ImageFormat.hpp" ) != std::string::npos )
                continue;

            const std::string text = StripCommentsAndStrings( ReadAll( entry.path().string() ) );
            // THE BLOCK SPELLINGS JOINED THE LIST, because a block format's constants are the same
            // shape of second copy: `kBytesPerBlock = 16` beside a 4x4 assumption is
            // `kSkyEnvBytesPerPixel = 16` with a different noun, and nothing would make it agree with
            // `GetTexelBlock` either. The names are the ones a reader would reach for.
            for ( const char* spelling : { "BytesPerPixel", "BytesPerBlock", "BlockBytes", "BlockWidth",
                                           "BlockHeight", "TexelsPerBlock" } )
            {
                const std::string needle( spelling );
                for ( std::size_t at = text.find( needle ); at != std::string::npos;
                      at             = text.find( needle, at + 1 ) )
                {
                    // Only a DEFINITION counts: `...<spelling> = <digit>`. A call is the census above.
                    std::size_t after = at + needle.size();
                    while ( after < text.size() && ( text[after] == ' ' || text[after] == '\t' ) )
                        ++after;
                    if ( after >= text.size() || text[after] != '=' )
                        continue;
                    ++after;
                    while ( after < text.size() && ( text[after] == ' ' || text[after] == '\t' ) )
                        ++after;
                    if ( after < text.size() && std::isdigit( static_cast<unsigned char>( text[after] ) ) )
                        offenders.push_back( entry.path().string().substr( root.size() ) + " (" + needle + ")" );
                }
            }
        }
    }

    EXPECT_TRUE( offenders.empty() ) << "these files define a texel or block size as a literal instead of "
                                        "asking Core::Formats::GetTexelBlock / GetBytesPerPixel: "
                                     << [&]
    {
        std::string all;
        for ( const std::string& f : offenders )
            all += "\n  " + f;
        return all;
    }();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
