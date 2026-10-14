// THE ENGINE'S MODULE TABLE COVERS EVERY SUBSYSTEM DIRECTORY EXACTLY ONCE (BUILD-MOD1).
//
// The engine is one static library per module (BuildScripts/EngineModules.lua), because the single Debug
// Desert.lib passed the 4 GB a COFF archive may hold (LNK1248). Each module compiles the source
// directories its table entry lists, so the table is now the thing that decides whether a .cpp is built:
//   * a directory under Desert/Desert/Source/Engine that no module lists compiles into NOTHING, and the
//     first sign is an unresolved symbol in whichever binary calls it first;
//   * a directory two modules list compiles TWICE, and every symbol in it is defined in two libraries.
// Both are silent at generation time; this census reads the table as text and names the directory.
// It also holds the other two rules of that file: exactly one module compiles Engine/Generated (and so
// runs the DesertHeaderTool prebuild), exactly one hosts Source/Platform and the vendored sources, and no
// consumer script names an engine library by hand instead of calling DesertEngineModules.Link().

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    struct ModuleRow
    {
        std::string              name;
        bool                     platform = false;
        std::vector<std::string> dirs;
    };

    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( ( prefix / "BuildScripts/EngineModules.lua" ).string() ) )
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

    // One table entry per line: `{ name = "X", [platform = true,] dirs = { "A", "B" } },`
    // (the shape BuildScripts/EngineModules.lua documents above the table).
    std::vector<ModuleRow> ParseModules( const std::string& lua )
    {
        static const std::regex entry(
             R"re(\{\s*name\s*=\s*"(\w+)"\s*,\s*(platform\s*=\s*true\s*,\s*)?dirs\s*=\s*\{([^}]*)\})re" );
        static const std::regex quoted( R"re("([^"]+)")re" );
        std::vector<ModuleRow>  rows;
        for ( auto it = std::sregex_iterator( lua.begin(), lua.end(), entry ); it != std::sregex_iterator(); ++it )
        {
            ModuleRow row;
            row.name            = ( *it )[1].str();
            row.platform        = ( *it )[2].matched;
            const std::string d = ( *it )[3].str();
            for ( auto q = std::sregex_iterator( d.begin(), d.end(), quoted ); q != std::sregex_iterator(); ++q )
                row.dirs.push_back( ( *q )[1].str() );
            rows.push_back( row );
        }
        return rows;
    }

    // Every fault of the table against the directories that exist, one line each; empty = the table is right.
    std::vector<std::string> Faults( const std::vector<ModuleRow>&   modules,
                                     const std::vector<std::string>& engineDirs )
    {
        std::vector<std::string>                        faults;
        std::map<std::string, std::vector<std::string>> owners;
        int                                             generatedOwners = 0;
        int                                             platformOwners  = 0;
        for ( const ModuleRow& module : modules )
        {
            platformOwners += module.platform ? 1 : 0;
            for ( const std::string& dir : module.dirs )
            {
                owners[dir].push_back( module.name );
                generatedOwners += dir == "Generated" ? 1 : 0;
            }
        }
        for ( const std::string& dir : engineDirs )
        {
            const auto found = owners.find( dir );
            if ( found == owners.end() )
                faults.push_back( "Engine/" + dir + " belongs to no module: it compiles into nothing" );
            else if ( found->second.size() > 1 )
                faults.push_back( "Engine/" + dir + " belongs to " + found->second[0] + " and " +
                                  found->second[1] + ": it compiles twice" );
        }
        for ( const auto& [dir, names] : owners )
        {
            if ( std::find( engineDirs.begin(), engineDirs.end(), dir ) == engineDirs.end() )
                faults.push_back( names[0] + " lists Engine/" + dir + ", which does not exist" );
        }
        if ( generatedOwners != 1 )
            faults.push_back( std::to_string( generatedOwners ) +
                              " modules compile Engine/Generated; the codegen prebuild must run exactly once" );
        if ( platformOwners != 1 )
            faults.push_back( std::to_string( platformOwners ) +
                              " modules carry platform = true; exactly one compiles Source/Platform" );
        return faults;
    }

    std::vector<std::string> EngineDirs( const fs::path& root )
    {
        std::vector<std::string> dirs;
        for ( const fs::directory_entry& entry : fs::directory_iterator( root / "Desert/Desert/Source/Engine" ) )
        {
            if ( entry.is_directory() )
                dirs.push_back( entry.path().filename().string() );
        }
        return dirs;
    }
} // namespace

TEST( EngineModules, EveryEngineDirectoryBelongsToExactlyOneModule )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "BuildScripts/EngineModules.lua not found above the working directory";
    const std::vector<ModuleRow> modules = ParseModules( ReadAll( root / "BuildScripts/EngineModules.lua" ) );
    ASSERT_GE( modules.size(), 2u )
         << "the module table did not parse: keep each entry on one line in the documented shape";

    const std::vector<std::string> dirs = EngineDirs( root );
    ASSERT_GT( dirs.size(), 10u ) << "Desert/Desert/Source/Engine was not found or not read";

    for ( const std::string& fault : Faults( modules, dirs ) )
        ADD_FAILURE() << fault << " (BuildScripts/EngineModules.lua)";
}

// The negative control: the same check over a table with each fault in it must name each one, or the
// test above could pass by reading nothing.
TEST( EngineModules, TheCensusNamesEveryKindOfFault )
{
    const std::string            table   = R"lua(
        { name = "A", platform = true, dirs = { "Core", "Generated" } },
        { name = "B",                   dirs = { "Core", "Ghost" } },
        { name = "C", platform = true, dirs = { "Generated" } },
    )lua";
    const std::vector<ModuleRow> modules = ParseModules( table );
    ASSERT_EQ( modules.size(), 3u );
    EXPECT_TRUE( modules[0].platform );
    EXPECT_FALSE( modules[1].platform );

    const std::vector<std::string> faults = Faults( modules, { "Core", "Generated", "Graphic" } );
    const auto                     names  = [&]( const std::string& part )
    {
        return std::any_of( faults.begin(), faults.end(),
                            [&]( const std::string& f ) { return f.find( part ) != std::string::npos; } );
    };
    EXPECT_TRUE( names( "Engine/Graphic belongs to no module" ) );
    EXPECT_TRUE( names( "Engine/Core belongs to A and B" ) );
    EXPECT_TRUE( names( "Engine/Generated belongs to A and C" ) );
    EXPECT_TRUE( names( "B lists Engine/Ghost" ) );
    EXPECT_TRUE( names( "2 modules compile Engine/Generated" ) );
    EXPECT_TRUE( names( "2 modules carry platform = true" ) );
}

// Every consumer links the engine through the helper. A script that names "Desert" (the one library that
// no longer exists) or one module by hand links a subset that resolves today and breaks on the next
// symbol that crosses into a module it forgot.
TEST( EngineModules, NoConsumerNamesAnEngineLibraryByHand )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::vector<ModuleRow> modules = ParseModules( ReadAll( root / "BuildScripts/EngineModules.lua" ) );
    ASSERT_GE( modules.size(), 2u );

    std::vector<std::string> forbidden = { "\"Desert\"" };
    for ( const ModuleRow& module : modules )
        forbidden.push_back( "\"" + module.name + "\"" );

    for ( const char* script :
          { "Desert/Tests/premake5.lua", "Editor/premake5.lua", "Editor/Dependencies.lua", "Runtime/premake5.lua",
            "Tools/SceneMigrator/premake5.lua", "Tools/WorldGen/premake5.lua", "Desert/Desert/premake5.lua" } )
    {
        const std::string text = ReadAll( root / script );
        ASSERT_FALSE( text.empty() ) << script << " could not be read";
        for ( const std::string& token : forbidden )
            EXPECT_EQ( text.find( token ), std::string::npos )
                 << script << " names " << token
                 << "; link the engine with DesertEngineModules.Link() (or DependOnCodegen())";
    }

    const std::string engine = ReadAll( root / "Desert/Desert/premake5.lua" );
    EXPECT_NE( engine.find( "ipairs(DesertEngineModules.Modules)" ), std::string::npos )
         << "Desert/Desert/premake5.lua must define one project per table entry with the shared function";
}
