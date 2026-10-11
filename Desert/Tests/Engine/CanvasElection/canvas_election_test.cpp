// "NOBODY ELECTS A CANVAS." A source-text gate, and it is a gate rather than a behavioural test because
// the defect is invisible to behaviour: a scene with ONE canvas gives the electing form and the asking form
// exactly the same answer, so every existing suite, every frame and every hand test passes either way.
//
// WHAT WAS MEASURED, on the commit this was written against. The form
//
//     auto view = reg.view<ECS::UICanvasComponent>();
//     return view.begin() == view.end() ? entt::null : *view.begin();
//
// stood in THREE files — UICanvasRenderer2D.cpp (the walk), UICanvasLayout.cpp (the editor's pick, marquee
// and scale) and UIElementFactory.hpp (the viewport's create menu) — and every one of them had a comment
// stating the coincidence as a rule ("the first one, which is the one the renderer draws"). entt's
// iteration order is a property of the pool, so "the first canvas" means "whichever the scene file happened
// to create first". Consequences, all silent: a second canvas was never drawn, never picked and never
// measured; HUD and menu could not be separated; an overlay and a world-space canvas could not coexist; a
// prefab could not carry its own canvas; and the UI Editor had to refuse to preview any canvas but one.
//
// THE RULE. `*view.begin()` on a view of UICanvasComponent is banned outright. Iterating such a view is
// not — enumerating every canvas is exactly what an honest host does (the viewport outlines all of them and
// picks through all of them) — so the gate is about the DEREFERENCE OF begin(), which is the act of turning
// a set of canvases into one and pretending that was a choice.
//
// Ю1's replacements are UI::CanvasOf (derive it from an element that is already inside it), UI::CanvasCount
// (count, which cannot pick a winner) and UI::SoleCanvas (refuse, by name and with the count, when there is
// not exactly one).

#include "TestSupport/source_roots.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/UI/Source/UI/UICanvasLayout.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Comments and string literals blanked, so the gate never counts its own prose or a message that quotes
    // the banned form. Kept deliberately small: it handles //, /* */ and "..." with escapes, which is all
    // this repository's C++ contains at the granularity that matters here.
    std::string StripCommentsAndStrings( const std::string& src )
    {
        std::string out = src;
        enum class In
        {
            Code,
            Line,
            Block,
            Str,
            Chr
        } state = In::Code;

        for ( std::size_t i = 0; i < out.size(); ++i )
        {
            const char c = out[i];
            const char n = i + 1 < out.size() ? out[i + 1] : '\0';
            switch ( state )
            {
                case In::Code:
                    if ( c == '/' && n == '/' )
                    {
                        state    = In::Line;
                        out[i]   = ' ';
                        out[++i] = ' ';
                    }
                    else if ( c == '/' && n == '*' )
                    {
                        state    = In::Block;
                        out[i]   = ' ';
                        out[++i] = ' ';
                    }
                    else if ( c == '"' )
                        state = In::Str;
                    else if ( c == '\'' )
                        state = In::Chr;
                    break;
                case In::Line:
                    if ( c == '\n' )
                        state = In::Code;
                    else
                        out[i] = ' ';
                    break;
                case In::Block:
                    if ( c == '*' && n == '/' )
                    {
                        state    = In::Code;
                        out[i]   = ' ';
                        out[++i] = ' ';
                    }
                    else if ( c != '\n' )
                        out[i] = ' ';
                    break;
                case In::Str:
                case In::Chr:
                {
                    const char quote = state == In::Str ? '"' : '\'';
                    if ( c == '\\' && i + 1 < out.size() )
                    {
                        out[i]   = ' ';
                        out[++i] = ' ';
                    }
                    else if ( c == quote )
                        state = In::Code;
                    else
                        out[i] = ' ';
                    break;
                }
            }
        }
        return out;
    }

    int LineOf( const std::string& src, std::size_t at )
    {
        return 1 + static_cast<int>( std::count( src.begin(), src.begin() + at, '\n' ) );
    }

    // Every .cpp/.hpp of the engine, the editor and the runtime. The test tree is excluded: a suite is
    // allowed to build any arrangement it likes in order to assert about it.
    std::vector<fs::path> SourceFiles( const std::string& root )
    {
        std::vector<fs::path> files;
        for ( const std::string& dir :
              Desert::TestSupport::LibraryRootsAnd( { "Editor/Source", "Runtime/Source", "Tools" } ) )
        {
            const fs::path base = fs::path( root ) / dir;
            if ( !fs::exists( base ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( base ) )
            {
                if ( !entry.is_regular_file() )
                    continue;
                const std::string ext = entry.path().extension().string();
                if ( ext == ".cpp" || ext == ".hpp" || ext == ".h" )
                    files.push_back( entry.path() );
            }
        }
        return files;
    }

    struct Finding
    {
        std::string File;
        int         Line = 0;
    };

    // Does @p src elect a single canvas out of a view of them? The signal is a `begin()` on something built
    // from `view<...UICanvasComponent>` that is DEREFERENCED — either `*view.begin()` directly, or `*` on a
    // named variable that was assigned such a view earlier in the same file.
    std::vector<Finding> ElectionsIn( const std::string& path, const std::string& code )
    {
        std::vector<Finding> out;

        // (a) the inline form: `*reg.view<ECS::UICanvasComponent>().begin()`.
        for ( std::size_t at = code.find( "UICanvasComponent" ); at != std::string::npos;
              at             = code.find( "UICanvasComponent", at + 1 ) )
        {
            const std::size_t viewAt = code.rfind( "view<", at );
            if ( viewAt == std::string::npos || at - viewAt > 32 )
                continue; // the token is not inside a `view<...>` argument list
            const std::size_t beginAt = code.find( ".begin()", at );
            if ( beginAt == std::string::npos || beginAt - at > 64 )
                continue;
            // Dereferenced? Walk back from `.begin()` to the start of the expression and look for `*`.
            std::size_t j = viewAt;
            while ( j > 0 && ( std::isalnum( static_cast<unsigned char>( code[j - 1] ) ) || code[j - 1] == '_' ||
                               code[j - 1] == '.' || code[j - 1] == ':' || code[j - 1] == '>' ) )
                --j;
            if ( j > 0 && code[j - 1] == '*' )
                out.push_back( { path, LineOf( code, viewAt ) } );
        }

        // (b) the two-step form: `auto v = reg.view<...UICanvasComponent>();` then `*v.begin()`.
        for ( std::size_t at = code.find( "UICanvasComponent" ); at != std::string::npos;
              at             = code.find( "UICanvasComponent", at + 1 ) )
        {
            const std::size_t viewAt = code.rfind( "view<", at );
            if ( viewAt == std::string::npos || at - viewAt > 32 )
                continue;
            const std::size_t eq = code.rfind( '=', viewAt );
            if ( eq == std::string::npos || viewAt - eq > 16 )
                continue;
            std::size_t nameEnd = eq;
            while ( nameEnd > 0 && std::isspace( static_cast<unsigned char>( code[nameEnd - 1] ) ) )
                --nameEnd;
            std::size_t nameBegin = nameEnd;
            while ( nameBegin > 0 && ( std::isalnum( static_cast<unsigned char>( code[nameBegin - 1] ) ) ||
                                       code[nameBegin - 1] == '_' ) )
                --nameBegin;
            if ( nameBegin == nameEnd )
                continue;
            const std::string deref = "*" + code.substr( nameBegin, nameEnd - nameBegin ) + ".begin()";
            if ( code.find( deref, eq ) != std::string::npos )
                out.push_back( { path, LineOf( code, code.find( deref, eq ) ) } );
        }

        return out;
    }
} // namespace

// --- The gate -------------------------------------------------------------------------------------------
TEST( CanvasElection, NoFileTurnsAViewOfCanvasesIntoOneCanvas )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    std::vector<Finding> findings;
    for ( const fs::path& file : SourceFiles( root ) )
    {
        const std::string code = StripCommentsAndStrings( ReadAll( file ) );
        if ( code.find( "UICanvasComponent" ) == std::string::npos )
            continue;
        for ( const Finding& f : ElectionsIn( file.string(), code ) )
            findings.push_back( f );
    }

    std::string report;
    for ( const Finding& f : findings )
        report += "\n    " + f.File + ":" + std::to_string( f.Line );

    EXPECT_TRUE( findings.empty() )
         << "a UICanvasComponent view was dereferenced to produce ONE canvas — that is entt's iteration "
            "order standing in for a decision, and the canvas it picks is silently never the second one."
         << report
         << "\n  Ask instead: UI::CanvasOf (from an element already inside the canvas), UI::CanvasCount "
            "(counting cannot pick a winner) or UI::SoleCanvas (refuses, by name, when there is not "
            "exactly one). See UI/UICanvasLayout.hpp.";
}

// --- The gate can see the thing it bans ------------------------------------------------------------------
//
// A source-text gate that matches nothing is indistinguishable from a source-text gate that is broken, and
// this project has shipped both. So the matcher is run against the two spellings that were actually in the
// tree — an inline dereference and the named-variable two-step — and against the shapes that must stay
// legal, because a gate that also bans honest enumeration would be deleted by the first person it stopped.
TEST( CanvasElection, TheMatcherFindsTheFormItBansAndLeavesEnumerationAlone )
{
    const auto banned = []( const char* src ) { return !ElectionsIn( "x.cpp", src ).empty(); };

    EXPECT_TRUE( banned( "auto c = *reg.view<ECS::UICanvasComponent>().begin();" ) )
         << "the inline form — the one in UICanvasRenderer2D.cpp — was not detected";
    EXPECT_TRUE( banned( "auto view = reg.view<ECS::UICanvasComponent>();\n"
                         "return view.begin() == view.end() ? entt::null : *view.begin();" ) )
         << "the two-step form — the one in UIElementFactory.hpp and UICanvasLayout.cpp — was not detected";

    EXPECT_FALSE( banned( "for ( auto e : reg.view<ECS::UICanvasComponent>() ) roots.Mark( e );" ) )
         << "enumerating every canvas is what an honest host does and must stay legal";
    EXPECT_FALSE( banned( "auto v = reg.view<ECS::UICanvasComponent>();\n"
                          "if ( v.begin() == v.end() ) return 0;" ) )
         << "comparing begin() against end() is counting, not electing";
    EXPECT_FALSE( banned( "auto c = *reg.view<ECS::MeshComponent>().begin();" ) )
         << "this gate is about canvases; other singleton questions have their own answers";
}
