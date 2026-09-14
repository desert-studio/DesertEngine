// THE REGISTER IS DATA, SO IT CAN GO WRONG QUIETLY — this is what stops it.
//
// scripts/CI/TidyRegister.txt names the files that were cleaned of a defect class and the checks they may
// never show again. Nothing about a text file resists a row being deleted in a rush, a path being renamed
// out from under it, or a check name being misspelled — and every one of those failures makes the gate
// GREENER, which is the direction nobody notices.
//
// So the register is asserted here rather than only being run by the shell:
//
//   * every file row exists, and is a .cpp the build compiles (a header cannot be a row: clang-tidy is
//     given a translation unit, and a header row would be silently skipped);
//   * every check row is actually enabled by .clang-tidy — a check the config disables would make its
//     row a claim nobody is testing, which is worse than no row;
//   * the row count has a floor, because a register emptied by an over-eager edit passes the shell gate
//     in four seconds and says "clean".
//
// The floor is NOT the current count. A gate that pins a number can be satisfied by editing the number;
// what is pinned here is a set of NAMED rows that must be present, plus a floor well below the real size.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "scripts/CI/TidyRegister.txt" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream     in( path, std::ios::binary );
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    struct Register
    {
        std::vector<std::string> Checks;
        std::vector<std::string> Files;
    };

    Register Parse( const std::string& text )
    {
        Register    out;
        int         section = 0; // 0 none, 1 checks, 2 files
        std::string line;
        for ( std::istringstream in( text ); std::getline( in, line ); )
        {
            if ( !line.empty() && line.back() == '\r' )
                line.pop_back();
            if ( line == "CHECKS:" )
            {
                section = 1;
                continue;
            }
            if ( line == "FILES:" )
            {
                section = 2;
                continue;
            }
            if ( line.empty() || line[0] == '#' )
                continue;
            if ( section == 1 )
                out.Checks.push_back( line );
            else if ( section == 2 )
                out.Files.push_back( line );
        }
        return out;
    }
} // namespace

TEST( TidyRegister, EveryFileRowIsATranslationUnitThatExists )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not find scripts/CI/TidyRegister.txt from the working directory";

    const Register reg = Parse( ReadAll( fs::path( root ) / "scripts/CI/TidyRegister.txt" ) );

    for ( const std::string& row : reg.Files )
    {
        EXPECT_TRUE( fs::exists( fs::path( root ) / row ) )
             << "the register names '" << row
             << "', which does not exist. A row is a claim about a file: a rename moves the row, it does"
                " not delete it.";
        EXPECT_TRUE( row.size() > 4 && row.compare( row.size() - 4, 4, ".cpp" ) == 0 )
             << "'" << row
             << "' is not a .cpp. clang-tidy is handed translation units, so a header row would be"
                " skipped without a word.";
    }
}

TEST( TidyRegister, EveryCheckRowIsEnabledByTheConfig )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const Register    reg    = Parse( ReadAll( fs::path( root ) / "scripts/CI/TidyRegister.txt" ) );
    const std::string config = ReadAll( fs::path( root ) / ".clang-tidy" );
    ASSERT_FALSE( config.empty() );

    for ( const std::string& check : reg.Checks )
    {
        // The config enables families with globs (`bugprone-*`) and disables individuals with `-name`.
        // A register row for a check the config switched OFF is a row nothing tests.
        EXPECT_EQ( config.find( "-" + check ), std::string::npos )
             << ".clang-tidy disables '" << check
             << "', so its row in the register is a claim the gate can never make.";

        const std::string family = check.substr( 0, check.find( '-' ) ) + "-*";
        EXPECT_NE( config.find( family ), std::string::npos )
             << ".clang-tidy does not enable the '" << family << "' family that '" << check << "' belongs to.";
    }
}

TEST( TidyRegister, TheRegisterIsNotEmptyAndStillNamesTheRowsItWasBuiltFor )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const Register reg = Parse( ReadAll( fs::path( root ) / "scripts/CI/TidyRegister.txt" ) );

    EXPECT_GE( reg.Checks.size(), 7u ) << "the register lists only " << reg.Checks.size()
                                       << " check(s); Д36 closed seven classes.";
    EXPECT_GE( reg.Files.size(), 40u ) << "the register lists only " << reg.Files.size()
                                       << " file(s). A register emptied by an over-eager edit passes the"
                                          " shell gate in seconds and reports itself clean.";

    // NAMED ROWS, NOT A COUNT. One file per class, each the site the class was FOUND at, so deleting the
    // row that matters cannot be hidden by adding rows that do not.
    const char* const required[] = {
         "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp",                       // the unsequenced key
         "Desert/Desert/Source/Engine/Graphic/API/Vulkan/CommandBufferAllocator.cpp",   // .value() in a ctor
         "Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",                // memcmp over floats
         "Desert/Desert/Source/Engine/Core/ShaderCompiler/Includer/ShaderIncluder.cpp", // throwing dtor
         "Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.cpp",                 // 32-bit pointer offset
         "Desert/Common/Source/Common/Core/Profiler.cpp",                               // (int)( x + 0.5 )
         "Desert/Desert/Source/Engine/UI/UICanvasRenderer2D.cpp",                       // assignment in an if
         "Tools/DesertHeaderTool/main.cpp",                                             // the empty catch
    };
    for ( const char* row : required )
        EXPECT_NE( std::find( reg.Files.begin(), reg.Files.end(), std::string( row ) ), reg.Files.end() )
             << row
             << " has been removed from the register. That file is the SITE one of the seven"
                " classes was found at; dropping its row retires the class silently.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
