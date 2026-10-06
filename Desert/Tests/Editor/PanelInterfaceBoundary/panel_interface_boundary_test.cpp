// "THE PANEL INTERFACE MUST NOT SPEAK THE TOOLKIT'S VOCABULARY." The owner's instruction was shorter --
// "ubrat' ImVec" -- and this is what it means as a property of the tree.
//
// WHAT WAS ACTUALLY WRONG. Editor/Source/Editor/Panels/IPanel.hpp is the base class of every panel and
// of every document, and two of its virtuals returned `ImVec2`:
//
//     virtual ImVec2 GetWindowPadding() const;
//     virtual ImVec2 GetDefaultSize() const;
//
// A type in a signature is not an implementation detail: naming it obliged the header to open
// <ImGui/imgui.h>, and therefore obliged EVERY consumer of the header to find the toolkit on its
// include path -- including test suites that draw nothing at all. Eleven test premakes carried
// `externalincludedirs { "%{wks.location}/ThirdParty" }` for that reason and two of them said so in
// their own comments: "the ImGui and glm include paths are here because IPanel.hpp declares ImVec2
// members, not because any ImGui function is called." The toolkit was a cost paid by everything that
// merely NAMED a panel.
//
// THIS IS A DIFFERENT BOUNDARY FROM ImGuiBoundary, AND DELIBERATELY NOT AN EXTENSION OF IT.
// Desert/Tests/Engine/ImGuiBoundary says "the engine and the runtime contain no toolkit at all". That
// statement is false of this tree: the editor DRAWS with Dear ImGui and must go on naming it in the
// files that draw. What is asserted here is narrower and is about one header -- the interface panels
// are declared through -- plus every repository header it opens, transitively -- the half a scan of one file
// cannot see. A census that said both things would be a census whose red means two different repairs.
//
// THE WALK REPLACES THE BUILD ROWS. Four suites used to compile IPanel.hpp from premakes that carried no
// ThirdParty root, so a toolkit include arriving through any header the interface opens stopped them
// building. The suites now build inside the EditorTests runner, whose include path holds the toolkit for
// the editor sources it compiles, so no build can say this any more; the walk over the headers the
// interface opens says it instead, for the whole chain and not only the first file.
//
// COMMENTS AND STRING LITERALS ARE STRIPPED BEFORE ANYTHING IS SEARCHED FOR. This repository has twice
// switched off a census that reddened on its own prose, once losing a real finding with it -- and
// IPanel.hpp's own paragraphs necessarily name the type they explain the removal of, which is exactly
// the shape that bites. That property is turned into the negative control here rather than worked
// around: the header's RAW text must keep naming the toolkit, and its stripped text must not.

#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <Editor/Panels/IPanel.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace CT = Desert::Tests::ConsumerText;

namespace
{
    // The one header this census is about.
    constexpr const char* kInterfaceHeader = "Editor/Source/Editor/Panels/IPanel.hpp";

    // THE CONVERSION SITE -- where a panel's glm::vec2 becomes the toolkit's ImVec2, which is the other
    // end of this change and the only place that may still name it. It doubles as the negative control:
    // without it the census could quietly become "nothing in the editor names ImGui", which is not the
    // rule and would be satisfied by an editor that had stopped drawing.
    constexpr const char* kConversionSite = "Editor/Source/EditorLayer.cpp";

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

    // The toolkit's public spellings, as PREFIXES. Same derivation as ImGuiBoundary's, and for the same
    // reason: a list of names stops seeing the type somebody uses tomorrow. `Im` must be followed by an
    // upper-case letter so that Image2D and ImportSettings are not toolkit names, and `IM` alone is not
    // a prefix so that the words IMMEDIATE and IMPLEMENTATION in a shouted heading are not either.
    bool IsToolkitIdentifier( const std::string& id )
    {
        static const std::vector<std::string> kPrefixes = { "ImGui",     "ImVec",   "ImDraw", "ImFont",
                                                            "ImTexture", "ImColor", "ImRect", "ImU",
                                                            "ImWchar",   "IMGUI_",  "IM_" };
        return std::ranges::any_of( kPrefixes, [&id]( const std::string& prefix )
                                    { return id.compare( 0, prefix.size(), prefix ) == 0; } );
    }

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

    // `#include` lines naming the toolkit, in text whose comments are gone and whose LITERALS ARE KEPT:
    // the quoted spelling of an include is a string literal, and a scan over blanked literals would be
    // blind to half of them.
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

    // Every header IPanel.hpp opens that lives in this repository, transitively, as repository-relative
    // paths, the interface itself first. An include is resolved the way the editor's own include path
    // resolves it -- next to the including file first, then under the three source roots -- and one that
    // resolves nowhere here (the standard library, glm) is not the repository's to police.
    std::vector<fs::path> RepoHeadersOpenedBy( const fs::path& root, const fs::path& header )
    {
        static const std::regex kInclude( R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])" );
        const fs::path          kRoots[] = { "Editor/Source", "Desert/Desert/Source", "Desert/Common/Source" };

        std::vector<fs::path> order{ header };
        for ( std::size_t next = 0; next < order.size(); ++next )
        {
            const fs::path     current = order[next];
            std::istringstream in( CT::StripComments( ReadAll( root / current ) ) );
            std::string        line;
            std::smatch        match;
            while ( std::getline( in, line ) )
            {
                if ( !std::regex_search( line, match, kInclude ) )
                    continue;
                const fs::path        named = match[1].str();
                std::vector<fs::path> candidates{ current.parent_path() / named };
                for ( const fs::path& base : kRoots )
                    candidates.push_back( base / named );
                for ( const fs::path& candidate : candidates )
                {
                    const fs::path normal = candidate.lexically_normal();
                    if ( !fs::is_regular_file( root / normal ) )
                        continue;
                    if ( std::ranges::find( order, normal ) == order.end() )
                        order.push_back( normal );
                    break;
                }
            }
        }
        return order;
    }

    std::string Join( const std::vector<std::string>& lines )
    {
        std::string out;
        for ( const std::string& line : lines )
            out += "\n    " + line;
        return out;
    }

    // A panel is an abstract class; this is the smallest thing that IS one. It exists so the defaults
    // below can be READ rather than described, and so that this translation unit really does compile the
    // interface rather than merely mention its file name.
    class BarePanel final : public Desert::Editor::IPanel
    {
    public:
        BarePanel() : IPanel( "Bare" )
        {
        }
        void OnUIRender() override
        {
        }
    };
} // namespace

// ----------------------------------------------------------------------------------------------------
// 1. THE HEADER
// ----------------------------------------------------------------------------------------------------

TEST( PanelInterfaceBoundary, TheInterfaceHeaderIncludesNoToolkitHeader )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";

    const std::string raw = ReadAll( root / kInterfaceHeader );
    ASSERT_FALSE( raw.empty() ) << kInterfaceHeader << " could not be read";

    const std::vector<std::string> offenders = ToolkitIncludesIn( CT::StripComments( raw ) );
    EXPECT_TRUE( offenders.empty() )
         << kInterfaceHeader
         << " opens the toolkit again. Every panel, every document and every suite that names one then "
            "has to find Dear ImGui on its include path -- which is the cost this interface was cleared "
            "of. A value handed to ImGui becomes an ImVec2 where it is DRAWN (EditorLayer's panel loop), "
            "not where it is declared."
         << Join( offenders );
}

TEST( PanelInterfaceBoundary, TheInterfaceHeaderNamesNoToolkitTypeInCode )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string raw = ReadAll( root / kInterfaceHeader );
    ASSERT_FALSE( raw.empty() );

    const std::vector<std::string> offenders = ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( raw ) );
    EXPECT_TRUE( offenders.empty() )
         << kInterfaceHeader
         << " names a toolkit type in CODE (comments and string literals are stripped first, so this is "
            "not prose). Naming one in a signature is how the include comes back."
         << Join( offenders );
}

// ----------------------------------------------------------------------------------------------------
// 2. THE HEADERS IT OPENS -- the half a scan of one file cannot see
// ----------------------------------------------------------------------------------------------------

TEST( PanelInterfaceBoundary, NoHeaderTheInterfaceOpensIncludesTheToolkit )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<fs::path> opened = RepoHeadersOpenedBy( root, kInterfaceHeader );
    // The walk itself is checked: an interface that opened nothing of the repository's would make this
    // row vacuous, and IPanel.hpp does open engine headers today.
    ASSERT_GT( opened.size(), 1u ) << kInterfaceHeader << " resolved to no repository header at all";

    for ( const fs::path& header : opened )
    {
        const std::vector<std::string> offenders =
             ToolkitIncludesIn( CT::StripComments( ReadAll( root / header ) ) );
        EXPECT_TRUE( offenders.empty() )
             << header.generic_string() << " is opened by " << kInterfaceHeader
             << " and includes the toolkit, so every suite and panel that names the interface needs Dear "
                "ImGui on its include path again -- the cost the interface was cleared of, arriving one "
                "header further down."
             << Join( offenders );
    }
}

// ----------------------------------------------------------------------------------------------------
// 3. THE DEFAULTS THE PANELS ARE DRAWN WITH -- the reason the field exists at all
// ----------------------------------------------------------------------------------------------------

TEST( PanelInterfaceBoundary, TheDefaultPaddingAndSizeAreUnchangedByTheTypeChange )
{
    // 8 px on both axes is what every panel in the editor breathes with, and it must survive a change of
    // the TYPE that carries it. A conversion written the wrong way round (x for y, or a truncation to
    // int) would be invisible in a header scan and visible only as a slightly different editor.
    const BarePanel panel;
    EXPECT_FLOAT_EQ( panel.GetWindowPadding().x, 8.0f );
    EXPECT_FLOAT_EQ( panel.GetWindowPadding().y, 8.0f );

    // (0,0) is not a size: it is the sentinel EditorLayer tests for before calling SetNextWindowSize at
    // all, so a panel that states no preference lets ImGui pick. A non-zero default here would give every
    // such panel a size it never asked for.
    EXPECT_FLOAT_EQ( panel.GetDefaultSize().x, 0.0f );
    EXPECT_FLOAT_EQ( panel.GetDefaultSize().y, 0.0f );
}

// ----------------------------------------------------------------------------------------------------
// 4. THE NEGATIVE CONTROLS -- prose is not code, and the editor still draws
// ----------------------------------------------------------------------------------------------------

TEST( PanelInterfaceBoundary, ProseAndStringLiteralsAreNotCode )
{
    const std::string comments = "// ImVec2 GetWindowPadding() is what this used to be\n"
                                 "/* ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ... ) */\n"
                                 "int after = 0;\n";
    EXPECT_TRUE( ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( comments ) ).empty() );
    EXPECT_NE( CT::StripCommentsAndLiterals( comments ).find( "int after" ), std::string::npos )
         << "the stripper ate the code as well as the prose, which would make every row above vacuous";

    EXPECT_TRUE( ToolkitIncludesIn( CT::StripComments( "// #include <ImGui/imgui.h> -- removed\n" ) ).empty() );

    // ...and the POSITIVE half in the same test, because a stripper that blanked everything would pass
    // all three expectations above.
    EXPECT_FALSE( ToolkitIdentifiersIn( CT::StripCommentsAndLiterals( "ImVec2 p( 8.0f, 8.0f );\n" ) ).empty() );
    EXPECT_FALSE( ToolkitIncludesIn( CT::StripComments( "#include <ImGui/imgui.h>\n" ) ).empty() );
    EXPECT_FALSE( ToolkitIncludesIn( CT::StripComments( "#include \"ImGui/imgui.h\"\n" ) ).empty() )
         << "the quoted spelling of an include is a STRING LITERAL: a scan run over text whose literals "
            "were blanked cannot see it, which is why this scan keeps them";
}

TEST( PanelInterfaceBoundary, TheInterfaceHeaderStillExplainsTheRuleAndTheEditorStillDraws )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // The header's own prose is this census's negative control: it must go on naming the type it no
    // longer declares, or the strip step above has stopped doing any work and every row is vacuous.
    const std::string header = ReadAll( root / kInterfaceHeader );
    ASSERT_FALSE( header.empty() );
    EXPECT_FALSE( ToolkitIdentifiersIn( header ).empty() )
         << kInterfaceHeader
         << " no longer names the toolkit even in its comments, so it has stopped being a negative "
            "control for the stripper. Either it stopped explaining why ImVec2 left -- which the next "
            "reader needs -- or the scan is looking at the wrong file.";

    // And the rule is NARROW. The panel loop is supposed to name the toolkit -- it is what converts the
    // panel's glm::vec2 into an ImVec2 and hands it to PushStyleVar/SetNextWindowSize. If this row ever
    // goes green-by-emptiness, either the conversion is gone (the padding and the first-open size would
    // then be silently unapplied) or the census has widened into ImGuiBoundary's statement.
    const std::string drawing = ReadAll( root / kConversionSite );
    ASSERT_FALSE( drawing.empty() ) << kConversionSite << " is gone; point this row at the new panel loop";
    const std::string drawingCode = CT::StripCommentsAndLiterals( drawing );
    EXPECT_NE( drawingCode.find( "ImVec2" ), std::string::npos )
         << kConversionSite
         << " names no ImVec2 in code any more. The panel interface states padding and default size as "
            "glm::vec2 and THIS is the file that turns them into what ImGui takes; a panel loop with no "
            "conversion in it is a loop that stopped applying them.";
    EXPECT_NE( drawingCode.find( "GetWindowPadding" ), std::string::npos )
         << kConversionSite << " no longer asks a panel for its padding at all.";
}
