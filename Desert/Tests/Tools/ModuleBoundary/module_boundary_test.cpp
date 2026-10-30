// THE ENGINE'S MODULE BOUNDARIES (ENG-MODULES P0 MODULE-INFRA), REPORT-ONLY UNTIL THE CUTS LAND.
//
// cmake/DesertModuleTable.cmake is the one module table: configure validates it and builds one library per
// module from it, DesertHeaderTool groups the reflected types by it, and this suite reads it through the same
// reader (Tools/DesertHeaderTool/Source/ModuleTable.cpp, the <build>/DesertModules.json configure writes). The
// suite walks every #include of the engine and Common, resolves it the way the engine's include paths do, and
// REPORTS each line whose target module is outside the includer's declared dependency closure - the crossings the
// plan's cuts C1..C12 remove. It does not fail on their number: the moves (P1..P14) delete them, and P14 turns
// this report into the gate.
//
// What it DOES hold, so the report cannot go blind: the table loads and is layered; every engine file has a
// module and every placement row places at least one file; named file -> module rows; named crossings the
// report must still contain (delete a pin in the commit whose cut removes its line); and an allowed edge that
// must be resolved yet NOT reported.

#include <gtest/gtest.h>

#include "../../TestSupport/scratch_dir.hpp"

#include <ModuleTable.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
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
            auto loaded = ModuleTable::Load( DESERT_MODULE_TABLE_JSON );
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
    ASSERT_EQ( table.Modules().size(), 27u )
         << "the plan's v3 map has 26 modules (Common included) + ScriptLuau (SCR-PORT)";
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
    const std::string legacyPrefix = std::format( "{}/", Table().LegacyRoot() );
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
         { "Desert/Render2DCore/Source/Render2DCore/DrawList2D.hpp", "Render2DCore" },
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
    // C1: EngineContext is GDynamicRHI — ONE pin, split only by the column limit, so it lives outside the array.
    constexpr const char* kMemoryReadoutPin  = "Desert/Desert/Source/Engine/Graphic/MemoryReadoutSource.cpp -> "
                                               "Desert/Desert/Source/Engine/Core/EngineContext.hpp";
    constexpr const char* kPinnedCrossings[] = {
         kMemoryReadoutPin,
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

// ONE LIBRARY PER MODULE (BUILD-MOD2): configure builds each module's legacy files as their own static library,
// placing every file with CMake's regex (cmake/DesertModules.cmake, `\w` / `\d` rewritten to bracket classes);
// the header tool and this suite place with std::regex on the rows as written. Red when the two disagree on any
// file (a row uses a construct the two engines read differently), or when the build compiles a different set of
// legacy files than this suite walks (a file missed by the build's glob, or one only the build sees).
namespace
{
    // Every file of the legacy root but its shared precompiled header (every library compiles pch.cpp).
    std::vector<std::string> LegacyFiles()
    {
        const std::string        prefix = std::format( "{}/", Table().LegacyRoot() );
        std::vector<std::string> legacy;
        for ( const std::string& file : EngineFiles() )
            if ( file.starts_with( prefix ) )
            {
                const std::string relative = file.substr( prefix.size() );
                if ( relative != "pch.cpp" && relative != "pch.hpp" )
                    legacy.push_back( relative );
            }
        return legacy;
    }
} // namespace

TEST( ModuleBoundary, BuildAndHeaderToolPlaceEveryLegacyFileTheSame )
{
    const std::vector<std::string> paths = LegacyFiles();
    ASSERT_GT( paths.size(), 1000u ) << "the walk found no legacy library: " << RepoRoot();
    const auto& built = Table().BuildPlacement();
    for ( const std::string& path : paths )
    {
        const auto it = built.find( path );
        if ( it == built.end() )
        {
            ADD_FAILURE() << path << ": the suite walks it, the build compiles it into no module library";
            continue;
        }
        EXPECT_EQ( it->second, Table().ModuleOfLegacy( path ) )
             << path << ": the build (CMake regex) and the header tool (std::regex) place it differently";
    }
    const std::set<std::string> walked( paths.begin(), paths.end() );
    for ( const auto& [path, module] : built )
        EXPECT_TRUE( walked.contains( path ) )
             << path << ": the build places it into " << module << ", the suite's walk does not see it";
}

// Red when a module with legacy files has no table library, or one that is not named Desert<Module>.
TEST( ModuleBoundary, EveryModuleWithLegacyFilesHasItsLibrary )
{
    std::set<std::string> withFiles;
    for ( const std::string& file : LegacyFiles() )
        withFiles.insert( Table().ModuleOfLegacy( file ) );
    ASSERT_FALSE( withFiles.empty() );
    const auto& libraries = Table().LegacyLibraries();
    EXPECT_EQ( libraries.size(), withFiles.size() );
    for ( const std::string& module : withFiles )
    {
        const auto it = libraries.find( module );
        ASSERT_NE( it, libraries.end() ) << module << " has legacy files but no table library";
        EXPECT_EQ( it->second, std::format( "Desert{}", module ) );
    }
}

// THE ARCHIVE LIMIT the split exists for: a static library over 4 GB is LNK1248, so every module library's
// Debug archive stays under 3.5 GB. Reads build/Bin/Debug; skipped when no module library was built there.
TEST( ModuleBoundary, NoModuleLibraryNearsTheArchiveLimit )
{
    constexpr std::uintmax_t kMargin  = 3584ull * 1024 * 1024;
    const fs::path           bin      = RepoRoot() / "build" / "Bin" / "Debug";
    int                      measured = 0;
    for ( const auto& [module, library] : Table().LegacyLibraries() )
        for ( const fs::path archive : { bin / ( library + ".lib" ), bin / ( "lib" + library + ".a" ) } )
            if ( fs::exists( archive ) )
            {
                ++measured;
                const std::uintmax_t size = fs::file_size( archive );
                std::cout << std::format( "{:>8} MB  {}\n", size / ( 1024 * 1024 ), archive.filename().string() );
                EXPECT_LT( size, kMargin )
                     << archive << " is within half a gigabyte of the 4 GB archive limit: split " << module
                     << " (cmake/DesertModuleTable.cmake)";
            }
    if ( measured == 0 )
        GTEST_SKIP() << "no Debug module library under " << bin;
}
