// "NIKAKIH IMGUI V SAMOM DVIZHKE I RANTAJME" -- the owner's instruction, made a property of the tree.
//
// WHAT WAS ACTUALLY WRONG. Dear ImGui is the EDITOR's interface toolkit. It was nevertheless compiled
// into `libDesert.a` -- Engine/imgui/ and Engine/Graphic/API/Vulkan/imgui/ held the integration layer,
// Desert/Desert/premake5.lua linked the library, and TWO edges made the engine depend on it rather than
// merely contain it: Application.hpp (the header every layer host opens) included ImGuiLayer.hpp, and
// Graphic::UICacheTexture::Create() constructed the ImGui implementation. Measured on the Debug archive
// before the move: 14 724 ImGui symbol lines in `nm libDesert.a`, and a packaged player that links that
// archive pays for every one of them without drawing a single ImGui pixel -- the Runtime draws its UI
// with the engine's own Render2D batcher.
//
// WHY A SOURCE CENSUS WHEN THE BUILD ALREADY PROVES IT. The build proves TODAY's binary. It cannot
// prove that the next `#include <ImGui/imgui.h>` in an engine header will be noticed: on macOS the
// toolkit's headers are reachable from the engine's own `-isystem ThirdParty` and the engine still
// compiles; what breaks is the LINK of some other consumer, on some other platform, weeks later. The
// grep-shaped relation is the one that fires on the day the include is written.
//
// The build scripts are pinned here too, because "the source names no ImGui symbol" and "the engine
// project does not link ImGui" are different statements and only the second one keeps the archive
// clean. A static library that links a project it never calls still carries it into every consumer.
//
// COMMENTS AND STRING LITERALS ARE STRIPPED BEFORE ANYTHING IS SEARCHED FOR, AND THAT IS NOT TIDINESS.
// A census that reddens on prose gets switched off -- twice in one week in this repository, once taking
// a real finding down with it. This file's own paragraphs name every token it forbids; so do
// Engine/Assets/AssetEviction.hpp (which explains why an image the editor has displayed cannot be
// evicted) and Engine/Graphic/Render2D/DrawList2D.hpp (which says it is our own answer to ImDrawList).
// Those two files are NAMED ROWS in the negative control below: they must stay green, and their raw
// text must keep containing a forbidden token, so the control cannot quietly become vacuous.
//
// The include scan reads a text with comments removed and LITERALS KEPT, on purpose: `#include "..."`
// is a string literal, and a census that blanked it would be blind to exactly half the spellings.

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
    // The trees that must be free of the toolkit. `Desert/Common/Source` is here and not only the two the
    // instruction named: the macro `EBABLE_IMGUI` lived in Common/Core/Core.hpp, so leaving Common out
    // would leave the hole this task closed unguarded.
    constexpr const char* kForbiddenTrees[] = {
         "Desert/Desert/Source",
         "Desert/Common/Source",
         "Runtime/Source",
    };

    // THE PROSE ROWS -- the negative control, as named files rather than as a count. Each must contain a
    // forbidden token in its RAW text (or the control proves nothing) and none after stripping.
    constexpr const char* kProseRows[] = {
         "Desert/Desert/Source/Engine/Assets/AssetEviction.hpp",
         "Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.hpp",
    };

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

    bool IsIdentChar( char c )
    {
        return ( std::isalnum( static_cast<unsigned char>( c ) ) != 0 ) || c == '_';
    }

    // THE FORBIDDEN IDENTIFIERS, AS PREFIXES AND NOT AS A LIST OF NAMES. Every public Dear ImGui spelling
    // begins with one of these: `ImGui::`, `ImGuiIO`, `ImGui_ImplVulkan_*`, `ImVec2`, `ImVec4`,
    // `ImDrawList`, `ImDrawData`, `ImFont`, `ImTextureID`, `IM_ARRAYSIZE`, `IMGUI_VERSION`. A list of
    // names would stop seeing the type somebody uses tomorrow; a prefix set derives it.
    //
    // `Im` must be followed by an UPPER-CASE letter, which is what keeps `Image2D`, `ImageFormat` and
    // `ImportSettings` out of it, and `IM` alone is not a prefix, which is what keeps the words
    // IMMEDIATE, IMPLEMENTATION and IMPLIED out of the SHOUTED headings this project writes.
    bool IsToolkitIdentifier( const std::string& id )
    {
        static const std::vector<std::string> kPrefixes = { "ImGui",     "ImVec",   "ImDraw", "ImFont",
                                                            "ImTexture", "ImColor", "ImRect", "ImU",
                                                            "ImWchar",   "IMGUI_",  "IM_" };
        for ( const std::string& prefix : kPrefixes )
            if ( id.compare( 0, prefix.size(), prefix ) == 0 )
                return true;
        return false;
    }

    // Every identifier in @p code that IsToolkitIdentifier accepts, with the line it sits on.
    std::vector<std::string> ToolkitIdentifiersIn( const std::string& code )
    {
        std::vector<std::string> found;
        int                      line = 1;
        for ( std::size_t i = 0; i < code.size(); )
        {
            if ( code[i] == '\n' )
            {
                ++line;
                ++i;
                continue;
            }
            if ( !IsIdentChar( code[i] ) || ( i > 0 && IsIdentChar( code[i - 1] ) ) )
            {
                ++i;
                continue;
            }
            const std::size_t start = i;
            while ( i < code.size() && IsIdentChar( code[i] ) )
                ++i;
            const std::string id = code.substr( start, i - start );
            if ( IsToolkitIdentifier( id ) )
                found.push_back( id + " (line " + std::to_string( line ) + ")" );
        }
        return found;
    }

    // The `#include` directives of @p text (comments already removed, literals INTACT) whose target names
    // the toolkit. Both spellings, `<...>` and `"..."`.
    std::vector<std::string> ToolkitIncludesIn( const std::string& text )
    {
        std::vector<std::string> found;
        std::istringstream       in( text );
        std::string              line;
        int                      number = 0;
        while ( std::getline( in, line ) )
        {
            ++number;
            const std::size_t hash = line.find_first_not_of( " \t" );
            if ( hash == std::string::npos || line[hash] != '#' )
                continue;
            const std::size_t word = line.find( "include", hash + 1 );
            if ( word == std::string::npos )
                continue;
            if ( line.find_first_not_of( " \t", hash + 1 ) != word )
                continue;

            std::string lowered = line.substr( word + 7 );
            std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            if ( lowered.find( "imgui" ) != std::string::npos )
                found.push_back( line.substr( hash ) + " (line " + std::to_string( number ) + ")" );
        }
        return found;
    }

    struct Source
    {
        std::string Relative;
        std::string Raw;
    };

    std::vector<Source> SourcesOf( const fs::path& root, const char* subtree )
    {
        std::vector<Source> out;
        const fs::path      base = root / subtree;
        if ( !fs::exists( base ) )
            return out;
        for ( const auto& entry : fs::recursive_directory_iterator( base ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const std::string ext = entry.path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".mm" )
                continue;
            out.push_back( { fs::relative( entry.path(), root ).generic_string(), ReadAll( entry.path() ) } );
        }
        return out;
    }

    std::string Join( const std::vector<std::string>& lines )
    {
        std::string out;
        for ( const std::string& line : lines )
            out += "\n    " + line;
        return out;
    }

    // Lua `--` line comments and `--[[ ]]` blocks removed, quoted strings kept: the premake rows below
    // ARE quoted strings, and this file's own premake script talks about the token it forbids.
    std::string StripLuaComments( const std::string& src )
    {
        std::string out;
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src[i] == '"' || src[i] == '\'' )
            {
                const char quote = src[i];
                out += src[i++];
                while ( i < src.size() && src[i] != quote )
                {
                    if ( src[i] == '\\' && i + 1 < src.size() )
                        out += src[i++];
                    out += src[i++];
                }
                if ( i < src.size() )
                    out += src[i++];
                continue;
            }
            if ( src.compare( i, 4, "--[[" ) == 0 )
            {
                const std::size_t end = src.find( "]]", i + 4 );
                i                     = end == std::string::npos ? src.size() : end + 2;
                continue;
            }
            if ( src.compare( i, 2, "--" ) == 0 )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
                continue;
            }
            out += src[i++];
        }
        return out;
    }
} // namespace

// ----------------------------------------------------------------------------------------------------
// 1. THE SOURCE TEXT
// ----------------------------------------------------------------------------------------------------

TEST( ImGuiBoundary, NoEngineOrRuntimeSourceIncludesTheToolkit )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";

    std::vector<std::string> offenders;
    for ( const char* subtree : kForbiddenTrees )
    {
        for ( const Source& source : SourcesOf( root, subtree ) )
        {
            for ( const std::string& line : ToolkitIncludesIn( CT::StripComments( source.Raw ) ) )
                offenders.push_back( source.Relative + ": " + line );
        }
    }

    EXPECT_TRUE( offenders.empty() )
         << "Dear ImGui is the EDITOR's interface toolkit and these trees ship without it. The "
            "integration layer lives in Editor/Source/Editor/ImGuiIntegration/; if the engine needs "
            "something from it, the editor must hand it over rather than the engine reach for it."
         << Join( offenders );
}

TEST( ImGuiBoundary, NoEngineOrRuntimeSourceNamesAToolkitIdentifier )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> offenders;
    for ( const char* subtree : kForbiddenTrees )
    {
        for ( const Source& source : SourcesOf( root, subtree ) )
        {
            for ( const std::string& id : ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( source.Raw ) ) )
                offenders.push_back( source.Relative + ": " + id );
        }
    }

    EXPECT_TRUE( offenders.empty() )
         << "a toolkit identifier in engine or runtime CODE (comments and string literals are stripped "
            "first, so this is not prose). Naming one of these types is how the dependency comes back."
         << Join( offenders );
}

TEST( ImGuiBoundary, TheEngineNeverIncludesAnEditorHeader )
{
    // THE DIRECTION, NOT JUST THE TOOLKIT. Moving the files is only half the repair: an engine header
    // that reaches into Editor/ puts the dependency back the way it was, with a different name on it.
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> offenders;
    for ( const char* subtree : kForbiddenTrees )
    {
        for ( const Source& source : SourcesOf( root, subtree ) )
        {
            const std::string  text = CT::StripComments( source.Raw );
            std::istringstream in( text );
            std::string        line;
            int                number = 0;
            while ( std::getline( in, line ) )
            {
                ++number;
                const std::size_t hash = line.find_first_not_of( " \t" );
                if ( hash == std::string::npos || line[hash] != '#' )
                    continue;
                if ( line.find( "include", hash + 1 ) == std::string::npos )
                    continue;
                if ( line.find( "<Editor/" ) != std::string::npos ||
                     line.find( "\"Editor/" ) != std::string::npos )
                    offenders.push_back( source.Relative + ": " + line.substr( hash ) + " (line " +
                                         std::to_string( number ) + ")" );
            }
        }
    }

    EXPECT_TRUE( offenders.empty() ) << "the engine and the runtime are BELOW the editor and cannot name it."
                                     << Join( offenders );
}

// ----------------------------------------------------------------------------------------------------
// 2. THE BUILD GRAPH -- the half a grep cannot see
// ----------------------------------------------------------------------------------------------------

TEST( ImGuiBoundary, TheEngineAndRuntimeProjectsDoNotLinkTheToolkitAndTheEditorDoes )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // The exact quoted token premake reads. `"ImGuiNodeEditor"` is a different library and a different
    // string, so matching the quotes is what keeps these rows from confusing the two.
    const std::string token = "\"ImGui\"";

    for ( const char* script : { "Desert/Desert/premake5.lua", "Runtime/premake5.lua" } )
    {
        const std::string text = StripLuaComments( ReadAll( root / script ) );
        ASSERT_FALSE( text.empty() ) << script << " could not be read";
        EXPECT_EQ( text.find( token ), std::string::npos )
             << script
             << " links ImGui. A project that links the toolkit carries it into every consumer of its "
                "output, however little of it the sources name -- which is exactly how libDesert.a came "
                "to hold 14 724 ImGui symbols. The Editor is the only project that may link it.";
    }

    const std::string editor = StripLuaComments( ReadAll( root / "Editor/premake5.lua" ) );
    ASSERT_FALSE( editor.empty() );
    EXPECT_NE( editor.find( token ), std::string::npos )
         << "Editor/premake5.lua must name ImGui itself. It used to arrive on Windows through "
            "Desert.vcxproj's transitive project reference; the engine does not link it any more, so an "
            "Editor that does not name it is a Windows-only link failure no macOS sweep can see.";
}

// ----------------------------------------------------------------------------------------------------
// 3. THE NEGATIVE CONTROL -- prose and literals must NOT redden this gate
// ----------------------------------------------------------------------------------------------------

TEST( ImGuiBoundary, ProseAndStringLiteralsAreNotCode )
{
    // Synthetic first, so the mechanism is pinned independently of any file in the tree.
    const std::string comments = "// ImGui::Begin and ImVec2 named in a line comment\n"
                                 "/* ImFont, ImTextureID */\n"
                                 "int after = 0;\n";
    EXPECT_TRUE( ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( comments ) ).empty() );
    EXPECT_NE( CT::StripCommentsAndLiterals( comments ).find( "int after" ), std::string::npos )
         << "the stripper ate the code as well as the prose, which would make every row below vacuous";

    const std::string literal = "const char* why = \"ImGui::Begin is what this used to call\";\n";
    EXPECT_TRUE( ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( literal ) ).empty() );

    const std::string commentedInclude = "// #include <ImGui/imgui.h> -- this is what we removed\n";
    EXPECT_TRUE( ToolkitIncludesIn( CT::StripComments( commentedInclude ) ).empty() );

    // ...and the POSITIVE half, in the same test, because a stripper that blanked everything would pass
    // all three expectations above.
    EXPECT_FALSE( ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( "ImGui::Begin( \"x\" );\n" ) ).empty() );
    EXPECT_FALSE( ToolkitIncludesIn( CT::StripComments( "#include <ImGui/imgui.h>\n" ) ).empty() );
    EXPECT_FALSE( ToolkitIncludesIn( CT::StripComments( "#include \"ImGui/imgui.h\"\n" ) ).empty() )
         << "the quoted spelling of an include is a STRING LITERAL: an include scan run over text whose "
            "literals were blanked cannot see it, which is why this scan keeps them";
}

TEST( ImGuiBoundary, TheProseFilesStayGreenAndAreStillProse )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* file : kProseRows )
    {
        const std::string raw = ReadAll( root / file );
        ASSERT_FALSE( raw.empty() ) << file
                                    << " is gone; a negative control naming a missing file is a "
                                       "row that passes without checking anything";

        // The row is only a control while the file really does talk about the toolkit.
        EXPECT_FALSE( ToolkitIdentifiersIn( raw ).empty() )
             << file
             << " no longer names a forbidden token anywhere, so it has stopped being a negative control "
                "for this gate. Point the row at a file that does, or delete it and say why.";

        EXPECT_TRUE( ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( raw ) ).empty() )
             << file << " has a toolkit identifier in CODE now, not only in its prose.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
