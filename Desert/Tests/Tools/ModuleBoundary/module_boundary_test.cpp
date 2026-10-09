// THE ENGINE'S MODULE BOUNDARIES (ENG-MODULES P0 MODULE-INFRA), REPORT-ONLY UNTIL THE CUTS LAND.
//
// BuildScripts/DesertModules.lua is the one module table: premake validates it, DesertHeaderTool groups the
// reflected types by it, and this suite reads it through the same reader (Tools/DesertHeaderTool/Source/
// ModuleTable.cpp, the Lua VM running the file). The suite walks every #include of the engine and Common,
// resolves it the way the engine's include paths do, and REPORTS each line whose target module is outside the
// includer's declared dependency closure - the crossings the plan's cuts C1..C12 remove. It does not fail on
// their number: the moves (P1..P14) delete them, and P14 turns this report into the gate.
//
// What it DOES hold, so the report cannot go blind: the table loads and is layered; every engine file has a
// module and every placement row places at least one file; named file -> module rows; named crossings the
// report must still contain (delete a pin in the commit whose cut removes its line); and an allowed edge that
// must be resolved yet NOT reported.

#include <gtest/gtest.h>

#include "../../TestSupport/scratch_dir.hpp"

#include <ModuleTable.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Desert::HeaderTool::ModuleTable;

    fs::path RepoRoot()
    {
        return Desert::TestSupport::RepositoryRoot();
    }

    const ModuleTable& Table()
    {
        static const ModuleTable table = []
        {
            auto loaded = ModuleTable::Load( RepoRoot() / "BuildScripts" / "DesertModules.lua" );
            if ( !loaded.IsSuccess() )
            {
                ADD_FAILURE() << loaded.GetError();
                return ModuleTable{};
            }
            return loaded.ExtractValue();
        }();
        return table;
    }

    struct Include
    {
        std::string From; // repo-relative
        std::string To;   // repo-relative, resolved
        std::string FromModule;
        std::string ToModule;
    };

    bool IsSource( const fs::path& p )
    {
        static const std::set<std::string> kExtensions = { ".cpp", ".hpp", ".h", ".mm", ".inl" };
        return kExtensions.contains( p.extension().string() );
    }

    // Every source file of the old library and of every module folder that exists, repo-relative.
    std::set<std::string> EngineFiles()
    {
        std::vector<std::string> roots{ Table().LegacyRoot() };
        for ( const auto& module : Table().Modules() )
            roots.push_back( module.Folder );
        std::set<std::string> files;
        for ( const std::string& root : roots )
        {
            const fs::path dir = RepoRoot() / root;
            if ( !fs::is_directory( dir ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
                if ( entry.is_regular_file() && IsSource( entry.path() ) )
                    files.insert( fs::relative( entry.path(), RepoRoot() ).generic_string() );
        }
        return files;
    }

    // The include roots the engine compiles with: the old library's source root and each module's Source/.
    std::vector<fs::path> IncludeRoots()
    {
        std::vector<fs::path> roots{ fs::path( Table().LegacyRoot() ) };
        for ( const auto& module : Table().Modules() )
            roots.push_back( fs::path( module.Folder ).parent_path() );
        return roots;
    }

    std::vector<Include> AllIncludes( const std::set<std::string>& files )
    {
        static const std::regex     kInclude( R"(^\s*#\s*include\s*([<"])([^>"]+)[>"])" );
        const std::vector<fs::path> roots = IncludeRoots();
        std::vector<Include>        out;
        for ( const std::string& file : files )
        {
            std::ifstream     in( RepoRoot() / file );
            const std::string fromModule = Table().ModuleOf( file );
            for ( std::string line; std::getline( in, line ); )
            {
                std::smatch m;
                if ( !std::regex_search( line, m, kInclude ) )
                    continue;
                std::vector<fs::path> candidates;
                if ( m[1].str() == "\"" )
                    candidates.push_back( ( fs::path( file ).parent_path() / m[2].str() ).lexically_normal() );
                for ( const fs::path& root : roots )
                    candidates.push_back( ( root / m[2].str() ).lexically_normal() );
                const auto target = std::find_if( candidates.begin(), candidates.end(), [&]( const fs::path& c )
                                                  { return files.contains( c.generic_string() ); } );
                if ( target == candidates.end() )
                    continue; // std, third-party, or a shader root include
                const std::string to = target->generic_string();
                out.push_back( { file, to, fromModule, Table().ModuleOf( to ) } );
            }
        }
        return out;
    }

    bool Crosses( const Include& include )
    {
        return include.FromModule != include.ToModule &&
               !Table().Closure( include.FromModule ).contains( include.ToModule );
    }

    std::string Row( const Include& include )
    {
        return std::format( "{} -> {}", include.From, include.To );
    }
} // namespace

TEST( ModuleBoundary, TableLoadsAndIsLayered )
{
    const ModuleTable& table = Table();
    ASSERT_EQ( table.Modules().size(), 26u ) << "the plan's v3 map has 26 modules (Common included)";
    // Named rows of the graph, not a count of edges.
    EXPECT_TRUE( table.Closure( "Engine" ).contains( "RenderCore" ) );
    EXPECT_TRUE( table.Closure( "Engine" ).contains( "Common" ) );
    EXPECT_TRUE( table.Closure( "Foliage" ).contains( "Landscape" ) );
    EXPECT_FALSE( table.Closure( "RenderCore" ).contains( "Engine" ) ) << "the RHI module sits below Engine";
    EXPECT_FALSE( table.Closure( "Engine" ).contains( "Renderer" ) )
         << "the renderer sits above Engine (plan 3.2)";
    for ( std::size_t i = 0; i < table.Modules().size(); ++i )
        for ( const std::string& dep : table.Modules()[i].Deps )
        {
            const Desert::HeaderTool::ModuleInfo* found = table.Find( dep );
            ASSERT_NE( found, nullptr ) << table.Modules()[i].Name << " -> " << dep;
            EXPECT_LT( found - table.Modules().data(), static_cast<std::ptrdiff_t>( i ) )
                 << table.Modules()[i].Name << " depends on " << dep << ", listed after it";
        }
}

TEST( ModuleBoundary, EveryFileHasAModuleAndEveryRowPlacesOne )
{
    const std::set<std::string> files = EngineFiles();
    ASSERT_GT( files.size(), 1000u ) << "the walk found no engine: " << RepoRoot();
    std::vector<int>  placed( Table().Placement().size(), 0 );
    const std::string legacyPrefix = Table().LegacyRoot() + "/";
    for ( const std::string& file : files )
    {
        EXPECT_FALSE( Table().ModuleOf( file ).empty() ) << file << " belongs to no module";
        if ( !file.starts_with( legacyPrefix ) )
            continue;
        const std::string relative = file.substr( legacyPrefix.size() );
        for ( std::size_t i = 0; i < placed.size(); ++i )
            if ( std::regex_search( relative, Table().Placement()[i].Regex ) )
            {
                ++placed[i];
                break;
            }
    }
    for ( std::size_t i = 0; i < placed.size(); ++i )
        EXPECT_GT( placed[i], 0 ) << "placement row " << i + 1 << " (" << Table().Placement()[i].Module << ", "
                                  << Table().Placement()[i].Pattern << ") places no file: delete it";
}

TEST( ModuleBoundary, NamedFilesSitInTheirModules )
{
    struct Pin
    {
        const char* path;
        const char* module;
    };
    constexpr Pin kPins[] = {
         { "Desert/Common/Source/Common/Core/ResultStr.hpp", "Common" },
         { "Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.hpp", "CoreReflection" },
         { "Desert/Desert/Source/Engine/Graphic/RDG/RDGBuilder.hpp", "RenderCore" },
         { "Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.hpp", "Render2DCore" },
         { "Desert/Desert/Source/Engine/Graphic/SceneRenderer.hpp", "Renderer" },
         { "Desert/Desert/Source/Engine/Core/Scene.hpp", "Engine" },
         { "Desert/Desert/Source/Engine/World/Foliage/FoliageCells.hpp", "Foliage" },
         { "Desert/Desert/Source/Platform/MacOS/MacOSWindow.cpp", "ApplicationCore" },
         // plan C11: a module's generated registration belongs to the module, not to the folder it is written in
         { "Desert/Desert/Source/Engine/Generated/Reflection_Foliage.gen.cpp", "Foliage" },
         { "Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp", "Engine" },
    };
    for ( const Pin& pin : kPins )
    {
        EXPECT_TRUE( fs::exists( RepoRoot() / pin.path ) )
             << pin.path << " is gone: re-pin a file of " << pin.module;
        EXPECT_EQ( Table().ModuleOf( pin.path ), pin.module ) << pin.path;
    }
}

// REPORT-ONLY: prints every crossing, grouped by edge; fails only when the report lost a pinned line or
// started reporting an allowed edge (the instrument went blind or wrong), never on how many crossings remain.
TEST( ModuleBoundary, CrossingsReport )
{
    const std::vector<Include>                      includes = AllIncludes( EngineFiles() );
    std::map<std::string, std::vector<std::string>> byEdge;
    std::set<std::string>                           reported;
    std::size_t                                     allowedEngineToRenderCore = 0;
    for ( const Include& include : includes )
    {
        if ( Crosses( include ) )
        {
            byEdge[std::format( "{} -> {}", include.FromModule, include.ToModule )].push_back( Row( include ) );
            reported.insert( Row( include ) );
        }
        else if ( include.FromModule == "Engine" && include.ToModule == "RenderCore" )
            ++allowedEngineToRenderCore;
    }

    std::cout << std::format( "[ModuleBoundary] {} include lines resolved, {} cross the declared graph\n",
                              includes.size(), reported.size() );
    for ( const auto& [edge, rows] : byEdge )
    {
        std::cout << std::format( "  {}: {}\n", edge, rows.size() );
        for ( const std::string& row : rows )
            std::cout << std::format( "      {}\n", row );
    }

    // Named crossings the plan cuts; a pin goes in the commit whose cut deletes its line.
    constexpr const char* kPinnedCrossings[] = {
         // C1: EngineContext is GDynamicRHI
         "Desert/Desert/Source/Engine/Graphic/MemoryReadoutSource.cpp -> "
         "Desert/Desert/Source/Engine/Core/EngineContext.hpp",
         // C4: ISceneRenderer = FSceneInterface
         "Desert/Desert/Source/Engine/Desert.hpp -> Desert/Desert/Source/Engine/Graphic/SceneRenderer.hpp",
         // C8 via C3c: Skeleton reaches the GPU mesh
         "Desert/Desert/Source/Engine/Animation/Skeleton.hpp -> Desert/Desert/Source/Engine/Geometry/Mesh.hpp",
    };
    for ( const char* pin : kPinnedCrossings )
        EXPECT_TRUE( reported.contains( pin ) )
             << "pinned crossing no longer reported: " << pin
             << " (its cut landed: delete the pin; otherwise the report went blind)";

    EXPECT_GT( allowedEngineToRenderCore, 0u )
         << "no Engine -> RenderCore include resolved: the walk lost the RHI";
    EXPECT_FALSE( byEdge.contains( "Engine -> RenderCore" ) ) << "an allowed edge is reported as a crossing";
}
