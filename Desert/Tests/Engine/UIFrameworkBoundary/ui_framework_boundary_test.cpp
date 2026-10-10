// UIFrameworkBoundary -- the UI framework is its own library (UI-FW-4), and this suite is what keeps it one.
//
// DesertUI (Desert/UI/Source/UI) reads its element tree through UI::IUITree + NodeId and its resources through
// interfaces; the ECS-backed implementations are the ENGINE's (Engine/UI/Ecs/). Render2DCore
// (Desert/Render2DCore/Source/Render2DCore) is the recorded half of 2D drawing the framework fills. The layering
// is Common <- Render2DCore <- DesertUI <- Desert, the same order BuildScripts/DesertModules.lua declares.
//
// WHY A SOURCE CENSUS WHEN THE BUILD ALREADY PROVES IT. The build proves today's binary. It does not notice the
// day an `#include <Engine/...>` is written into the framework: the include directories every consumer passes
// still resolve it, and what breaks is the next library that wants the framework without the engine. The
// census fires on the day the include is written.
//
// TWO STATEMENTS, BOTH PINNED. "No framework source names an engine header" and "the framework project does not
// link the engine" are different claims; a static library that links something it never calls still drags it
// into every consumer, so the premake scripts are read too.
//
// Comments are stripped before an include is looked for (a commented-out include is prose); string literals are
// kept, because the target of `#include "..."` IS a literal.

#include "../../TestSupport/scratch_dir.hpp"
#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

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
    struct Library
    {
        const char*              Name;    // the premake project
        const char*              Tree;    // its sources, repository-relative
        const char*              Script;  // its premake script
        std::vector<std::string> Allowed; // include prefixes it may name, besides the standard library
        std::set<std::string>    Links;   // the projects it may link
    };

    // The include prefixes every row may use besides its own: the shared base and the maths library.
    const std::vector<std::string> kShared = { "Common/", "glm/" };

    const std::vector<Library>& Libraries()
    {
        static const std::vector<Library> libraries = {
             { "DesertUI",
               "Desert/UI/Source/UI",
               "Desert/UI/premake5.lua",
               { "UI/", "Render2DCore/", "CoreReflection/" },
               { "Render2DCore", "Common" } },
             { "Render2DCore",
               "Desert/Render2DCore/Source/Render2DCore",
               "Desert/Render2DCore/premake5.lua",
               { "Render2DCore/" },
               { "Common" } },
        };
        return libraries;
    }

    std::string ReadAll( const fs::path& file )
    {
        std::ifstream      in( file, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // The target of every `#include` line of @p text (comments already removed), with its delimiters.
    std::vector<std::string> IncludeTargets( const std::string& text )
    {
        std::vector<std::string> targets;
        std::istringstream       in( text );
        std::string              line;
        while ( std::getline( in, line ) )
        {
            const std::size_t hash = line.find_first_not_of( " \t" );
            if ( hash == std::string::npos || line[hash] != '#' )
                continue;
            const std::size_t word = line.find_first_not_of( " \t", hash + 1 );
            if ( word == std::string::npos || line.compare( word, 7, "include" ) != 0 )
                continue;
            const std::size_t open = line.find_first_of( "<\"", word + 7 );
            if ( open == std::string::npos )
                continue;
            const std::size_t close = line.find( line[open] == '<' ? '>' : '"', open + 1 );
            if ( close == std::string::npos )
                continue;
            targets.push_back( line.substr( open, close - open + 1 ) );
        }
        return targets;
    }

    // Why @p target may not appear in @p library; empty when it may. A quoted include is a sibling file (the
    // tree's own); an angle include must be the standard library (no directory, no extension) or one of the
    // library's prefixes.
    std::string Refusal( const Library& library, const std::string& target )
    {
        const std::string path = target.substr( 1, target.size() - 2 );
        if ( target.front() == '"' )
            return path.find( ".." ) == std::string::npos ? std::string{} : "reaches out of its own tree";
        if ( path.find( '/' ) == std::string::npos && path.find( '.' ) == std::string::npos )
            return {};
        for ( const std::string& prefix : library.Allowed )
            if ( path.starts_with( prefix ) )
                return {};
        for ( const std::string& prefix : kShared )
            if ( path.starts_with( prefix ) )
                return {};
        if ( path.starts_with( "Engine/" ) )
            return "an engine header: the engine is built ON this library, never under it";
        if ( path.find( "entt" ) != std::string::npos )
            return "entt: the element tree is UI::IUITree, the registry is Engine/UI/Ecs/'s";
        if ( path.find( "vulkan" ) != std::string::npos || path.find( "vk_" ) != std::string::npos ||
             path.find( "VkBootstrap" ) != std::string::npos )
            return "Vulkan: the GPU playback of a draw list is Render2D's, in the engine";
        if ( path.find( "GLFW" ) != std::string::npos || path.find( "glfw" ) != std::string::npos )
            return "GLFW: the window is the host's (Engine/UI/Ecs/UIWindowClipboard.hpp)";
        return "not a dependency of this library (BuildScripts/DesertModules.lua)";
    }

    std::vector<std::string> Offenders( const fs::path& root, const Library& library )
    {
        std::vector<std::string> offenders;
        for ( const auto& entry : fs::recursive_directory_iterator( root / library.Tree ) )
        {
            const std::string ext = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".mm" ) )
                continue;
            for ( const std::string& target : IncludeTargets( CT::StripComments( ReadAll( entry.path() ) ) ) )
                if ( const std::string why = Refusal( library, target ); !why.empty() )
                    offenders.push_back( fs::relative( entry.path(), root ).generic_string() + ": #include " +
                                         target + " -- " + why );
        }
        return offenders;
    }

    // Lua with its `--` comments removed (no string in these scripts holds a "--").
    std::string StripLuaComments( const std::string& text )
    {
        std::istringstream in( text );
        std::string        line;
        std::string        out;
        while ( std::getline( in, line ) )
            out += line.substr( 0, line.find( "--" ) ) + "\n";
        return out;
    }

    // Every quoted name inside the `links { ... }` blocks of @p script.
    std::set<std::string> LinkedProjects( const std::string& script )
    {
        std::set<std::string> names;
        for ( std::size_t at = script.find( "links" ); at != std::string::npos;
              at             = script.find( "links", at + 1 ) )
        {
            const std::size_t open  = script.find( '{', at );
            const std::size_t close = script.find( '}', open );
            if ( open == std::string::npos || close == std::string::npos )
                break;
            for ( std::size_t q = script.find( '"', open ); q != std::string::npos && q < close;
                  q             = script.find( '"', q + 1 ) )
            {
                const std::size_t end = script.find( '"', q + 1 );
                names.insert( script.substr( q + 1, end - q - 1 ) );
                q = end;
            }
        }
        return names;
    }

    std::string Join( const std::vector<std::string>& lines )
    {
        std::string out;
        for ( const std::string& line : lines )
            out += "\n    " + line;
        return out;
    }
} // namespace

TEST( UIFrameworkBoundary, TheLibrariesIncludeNothingAboveThem )
{
    const fs::path root = Desert::TestSupport::RepositoryRoot();
    for ( const Library& library : Libraries() )
    {
        ASSERT_TRUE( fs::is_directory( root / library.Tree ) ) << library.Tree << " is gone";
        const std::vector<std::string> offenders = Offenders( root, library );
        EXPECT_TRUE( offenders.empty() ) << library.Name << " names what it is not built on:" << Join( offenders );
    }
}

TEST( UIFrameworkBoundary, TheLibrariesLinkOnlyWhatTheyAreBuiltOn )
{
    const fs::path root = Desert::TestSupport::RepositoryRoot();
    for ( const Library& library : Libraries() )
    {
        const std::string script = StripLuaComments( ReadAll( root / library.Script ) );
        ASSERT_NE( script.find( std::string( "project \"" ) + library.Name + "\"" ), std::string::npos )
             << library.Script << " no longer defines " << library.Name;
        for ( const std::string& linked : LinkedProjects( script ) )
            EXPECT_TRUE( library.Links.contains( linked ) )
                 << library.Name << " links " << linked << ", which it is not built on";
        // The engine's include root on the library's include path is the same edge as an engine include, one
        // step earlier: it is what would let the next `#include <Engine/...>` compile.
        EXPECT_EQ( script.find( "Desert/Desert/Source" ), std::string::npos )
             << library.Name << " puts the engine's sources on its include path";
    }
}

TEST( UIFrameworkBoundary, TheEngineIsBuiltOnTheFrameworkNotUnderIt )
{
    const std::set<std::string> engine = LinkedProjects(
         StripLuaComments( ReadAll( Desert::TestSupport::RepositoryRoot() / "Desert/Desert/premake5.lua" ) ) );
    EXPECT_TRUE( engine.contains( "DesertUI" ) );
    EXPECT_TRUE( engine.contains( "Render2DCore" ) );
}

// The census's own negative control, so it cannot quietly become vacuous: the relation it reads fires on a real
// include and stays silent on prose.
TEST( UIFrameworkBoundary, ARealIncludeIsCaughtAndProseIsNot )
{
    const Library& ui = Libraries().front();

    const std::vector<std::string> real =
         IncludeTargets( CT::StripComments( "#include <Engine/ECS/Components.hpp>\n" ) );
    ASSERT_EQ( real.size(), 1u );
    EXPECT_FALSE( Refusal( ui, real.front() ).empty() );
    EXPECT_FALSE( Refusal( ui, "<entt/entt.hpp>" ).empty() );
    EXPECT_FALSE( Refusal( ui, "<vulkan/vulkan.h>" ).empty() );
    EXPECT_FALSE( Refusal( ui, "<GLFW/glfw3.h>" ).empty() );
    EXPECT_FALSE( Refusal( ui, "\"../../Desert/Source/Engine/Core/Window.hpp\"" ).empty() );

    EXPECT_TRUE( IncludeTargets( CT::StripComments( "// #include <Engine/ECS/Components.hpp>\n" ) ).empty() );
    EXPECT_TRUE( Refusal( ui, "<UI/UITree.hpp>" ).empty() );
    EXPECT_TRUE( Refusal( ui, "<Render2DCore/DrawList2D.hpp>" ).empty() );
    EXPECT_TRUE( Refusal( ui, "<vector>" ).empty() );

    const std::set<std::string> links =
         LinkedProjects( "links { \"Render2DCore\", \"Common\" }\nlinks { \"X\" }\n" );
    EXPECT_EQ( links, ( std::set<std::string>{ "Common", "Render2DCore", "X" } ) );
}
