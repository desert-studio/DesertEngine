// THE RUNTIME READS COOKED TEXTURES; THE EDITOR AND THE TOOLS DECODE SOURCES — T3, as a property of the tree.
//
// WHAT WAS WRONG. `Docs/Textures/T2_CONTAINER_DECISION.md` made a source image an IMPORT format and the
// `.tex` container the storage format, and `TextureImporter` became the one place that decodes. The
// engine nevertheless kept three decoders reachable from the draw layer:
//
//   * `Texture2D::Create( spec, path )` -> `ImageReader::Read / ReadHDR` -> `stbi_load*`. Its last
//     caller was the sky panorama, which decoded its `.hdr` BEFORE asking the environment cache — so a
//     cache hit paid a decode for nothing and a miss kept a decoder in every shipped player.
//   * `TextureCube::Create( spec, path )`, which had no caller at all and refused everything it was
//     given. Deleted, with the two orphan trait headers that named it.
//   * `ImageReader::ReadGif` in AnimatedImageService — THE ONE NAMED EXCEPTION, below.
//
// WHAT IS FORBIDDEN, as identifiers in code (comments and string literals blanked first):
//
//   * any identifier beginning `stbi_load`, `stbi_is_hdr` or `stbi_info` — every stb_image decode or
//     probe entry point, derived from the prefix so tomorrow's `stbi_load_16_from_callbacks` is seen;
//   * `ImageReader` — the engine's source-image reader class, whatever method is called on it;
//   * an `#include` of `stb_image.h` (the WRITER, `stb_image_write.h`, is an encoder and is allowed:
//     `--shot` writes PNGs from the runtime, and writing is not what T3 is about).
//
// THE REGISTER PINS ROWS, NOT A COUNT. Each exception is (file, identifier, reason), every offender must
// match a row, and every row must still match an offender — a row whose code has gone is red too, so
// the register cannot outlive the thing it excuses. The rows today are one exception seen from three
// files: the GIF path. Its condition for leaving is the lead's decision of 2026-09-23: a GIF becomes a
// SOURCE format that the importer cooks to video (the runtime already plays MPEG-1 through pl_mpeg), so
// the day that card lands these rows are deleted and `ImageReader` with them.
//
// COMMENTS AND LITERALS ARE STRIPPED FIRST, for the reason `ImGuiBoundary` records: a census that reddens
// on prose gets switched off, and takes a real finding down with it. Two engine files that DISCUSS
// `stbi_load` in comments are named below as the negative control, and must stay both raw-positive and
// stripped-negative so the control cannot quietly go vacuous.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace CT = Desert::Tests::ConsumerText;

namespace
{
    // The trees a packaged player is built from. Common is here because the engine and the runtime link
    // it, so a decoder added there reaches the player exactly as one added to the engine would.
    constexpr const char* kRuntimeTrees[] = {
         "Desert/Desert/Source",
         "Desert/Common/Source",
         "Runtime/Source",
    };

    struct Exception
    {
        const char* File;
        const char* Identifier;
        const char* Reason;
    };

    // THE REGISTER. One exception, three rows — the definition, its decode call and its one consumer.
    constexpr Exception kExceptions[] = {
         { "Desert/Desert/Source/Engine/Core/IO/ImageReader.hpp", "ImageReader",
           "GIF -> video at import is its own card (lead, 2026-09-23); ReadGif is all that is left here" },
         { "Desert/Desert/Source/Engine/Core/IO/ImageReader.cpp", "ImageReader",
           "GIF -> video at import is its own card (lead, 2026-09-23)" },
         { "Desert/Desert/Source/Engine/Core/IO/ImageReader.cpp", "stbi_load_gif_from_memory",
           "GIF -> video at import is its own card (lead, 2026-09-23)" },
         { "Desert/Desert/Source/Engine/Core/IO/ImageReader.cpp", "#include stb_image.h",
           "GIF -> video at import is its own card (lead, 2026-09-23)" },
         { "Desert/Desert/Source/Engine/Runtime/Services/AnimatedImage/AnimatedImageService.cpp", "ImageReader",
           "GIF -> video at import is its own card (lead, 2026-09-23); the UI plays a GIF by decoding it" },
    };

    // THE NEGATIVE CONTROL — files whose PROSE names a forbidden decoder and whose code does not.
    constexpr const char* kProseRows[] = {
         "Desert/Desert/Source/Engine/Assets/CloudLayout.hpp",
         "Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeSheet.hpp",
    };

    // THE POSITIVE CONTROL — the importer, which is where the decode lives now. If the census stopped
    // finding a decoder HERE, its token rule would have stopped matching real code.
    constexpr const char* kDecoderHome = "Editor/Source/Editor/Import/TextureImporter.cpp";

    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( ( prefix / "Desert/Desert/premake5.lua" ).string() ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    std::string ReadAll( const fs::path& file )
    {
        const std::ifstream in( file.string() );
        if ( !in )
            return {};
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    bool IsDecoderIdentifier( const std::string& id )
    {
        static const std::vector<std::string> kPrefixes = { "stbi_load", "stbi_is_hdr", "stbi_info" };
        if ( id == "ImageReader" )
            return true;
        return std::ranges::any_of( kPrefixes, [&id]( const std::string& prefix )
                                    { return id.compare( 0, prefix.size(), prefix ) == 0; } );
    }

    // Distinct forbidden identifiers in @p code (comments AND literals already blanked).
    std::set<std::string> DecoderIdentifiersIn( const std::string& code )
    {
        std::set<std::string> found;
        for ( std::size_t i = 0; i < code.size(); )
        {
            if ( !CT::IsIdentChar( code[i] ) || ( i > 0 && CT::IsIdentChar( code[i - 1] ) ) )
            {
                ++i;
                continue;
            }
            const std::size_t start = i;
            while ( i < code.size() && CT::IsIdentChar( code[i] ) )
                ++i;
            const std::string id = code.substr( start, i - start );
            if ( IsDecoderIdentifier( id ) )
                found.insert( id );
        }
        return found;
    }

    // "#include stb_image.h" when @p text (comments blanked, literals KEPT — an include target is a
    // literal) includes the stb_image DECODER header, in either spelling. The writer does not match: the
    // file name is compared whole, after the last separator.
    std::set<std::string> DecoderIncludesIn( const std::string& text )
    {
        std::set<std::string> found;
        std::istringstream    in( text );
        std::string           line;
        while ( std::getline( in, line ) )
        {
            const std::size_t hash = line.find_first_not_of( " \t" );
            if ( hash == std::string::npos || line[hash] != '#' )
                continue;
            const std::size_t word = line.find( "include", hash + 1 );
            if ( word == std::string::npos || line.find_first_not_of( " \t", hash + 1 ) != word )
                continue;
            const std::size_t open = line.find_first_of( "<\"", word + 7 );
            if ( open == std::string::npos )
                continue;
            const std::size_t close = line.find_first_of( ">\"", open + 1 );
            if ( close == std::string::npos )
                continue;
            const std::string target = line.substr( open + 1, close - open - 1 );
            const std::size_t slash  = target.find_last_of( "/\\" );
            const std::string name   = slash == std::string::npos ? target : target.substr( slash + 1 );
            if ( name == "stb_image.h" )
                found.insert( "#include stb_image.h" );
        }
        return found;
    }

    std::set<std::string> OffendersIn( const std::string& raw )
    {
        std::set<std::string> all = DecoderIdentifiersIn( CT::StripCommentsAndLiterals( raw ) );
        for ( const std::string& include : DecoderIncludesIn( CT::StripComments( raw ) ) )
            all.insert( include );
        return all;
    }

    struct Finding
    {
        std::string File;
        std::string Identifier;
    };

    std::vector<Finding> Census( const fs::path& root )
    {
        std::vector<Finding> findings;
        for ( const char* tree : kRuntimeTrees )
        {
            const fs::path base = root / tree;
            if ( !fs::exists( base ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( base ) )
            {
                if ( !entry.is_regular_file() )
                    continue;
                const std::string ext = entry.path().extension().string();
                if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".mm" && ext != ".inl" )
                    continue;
                const std::string relative = fs::relative( entry.path(), root ).generic_string();
                for ( const std::string& id : OffendersIn( ReadAll( entry.path() ) ) )
                    findings.push_back( { relative, id } );
            }
        }
        return findings;
    }

    bool Registered( const Finding& finding )
    {
        return std::ranges::any_of( kExceptions, [&finding]( const Exception& row )
                                    { return finding.File == row.File && finding.Identifier == row.Identifier; } );
    }
} // namespace

TEST( RuntimeSourceDecoders, NoRuntimeSourceReachesAnImageDecoderOutsideTheRegister )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";

    // THE TREES MUST EXIST AND BE NON-EMPTY, or "no offender" is the answer of a census that read nothing.
    for ( const char* tree : kRuntimeTrees )
        ASSERT_TRUE( fs::is_directory( root / tree ) ) << tree << " is not where this census expects it";

    std::string unregistered;
    for ( const Finding& finding : Census( root ) )
    {
        if ( !Registered( finding ) )
            unregistered += "\n    " + finding.File + ": " + finding.Identifier;
    }
    EXPECT_TRUE( unregistered.empty() )
         << "runtime code reaches a SOURCE image decoder. The runtime reads the `.tex` container the editor's "
            "TextureImporter writes (Texture2D::CreateFromCooked); a decode belongs in Editor/ or Tools/. If "
            "this is a deliberate exception, it is a named row in kExceptions with its reason and its "
            "condition for leaving:"
         << unregistered;
}

TEST( RuntimeSourceDecoders, EveryRegisteredExceptionStillExists )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<Finding> findings = Census( root );
    for ( const Exception& row : kExceptions )
    {
        const bool live = std::ranges::any_of( findings, [&row]( const Finding& f )
                                               { return f.File == row.File && f.Identifier == row.Identifier; } );
        EXPECT_TRUE( live ) << row.File << ": " << row.Identifier
                            << " is registered as an exception and the code no longer has it. Delete the row: "
                               "an exception that outlives its code is the next one's cover ("
                            << row.Reason << ").";
    }
}

TEST( RuntimeSourceDecoders, TheEditorStillDecodesSoTheRuleMatchesRealCode )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string raw = ReadAll( root / kDecoderHome );
    ASSERT_FALSE( raw.empty() ) << kDecoderHome << " could not be read";
    const std::set<std::string> found = OffendersIn( raw );
    EXPECT_TRUE( found.contains( "stbi_load_from_memory" ) ) << "the LDR decode is no longer seen in the importer";
    EXPECT_TRUE( found.contains( "stbi_loadf_from_memory" ) )
         << "the HDR decode is no longer seen in the importer";
    EXPECT_TRUE( found.contains( "#include stb_image.h" ) ) << "the decoder include is no longer seen";
}

TEST( RuntimeSourceDecoders, ProseAboutADecoderIsNotADecoder )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* file : kProseRows )
    {
        const std::string raw = ReadAll( root / file );
        ASSERT_FALSE( raw.empty() ) << file << " could not be read";
        EXPECT_NE( raw.find( "stbi_load" ), std::string::npos )
             << file << " no longer mentions stbi_load at all, so it controls nothing; name another prose row";
        EXPECT_TRUE( OffendersIn( raw ).empty() ) << file << " is prose-only and the census reads it as code";
    }

    // The writer is not the decoder, in both include spellings, and a literal naming a decoder is prose.
    EXPECT_TRUE( DecoderIncludesIn( "#include <stb_image/stb_image_write.h>\n" ).empty() );
    EXPECT_FALSE( DecoderIncludesIn( "#include \"stb_image/stb_image.h\"\n" ).empty() );
    EXPECT_TRUE( OffendersIn( "const char* s = \"stbi_load_from_memory\";\n" ).empty() );
    EXPECT_FALSE( OffendersIn( "auto* p = stbi_load_16_from_callbacks( c, u, &w, &h, &n, 4 );\n" ).empty() );
    // `ImageReader` is matched whole, not as a prefix: an unrelated `ImageReaderSettings` is not the class.
    EXPECT_TRUE( OffendersIn( "struct ImageReaderSettings {};\n" ).empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
