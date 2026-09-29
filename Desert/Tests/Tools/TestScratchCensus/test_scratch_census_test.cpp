// NO TEST WRITES FROM THE WORKING DIRECTORY (TST1).
//
// Owner, 2026-09-27: Assets/, DerivedDataCache/ and RegistryProbe/ appeared in the root of the Windows checkout.
// Suites built probe projects as `current_path() / "RegistryProbe"`, and the Windows runner started every
// binary in the repository root. The runners now start each suite in build/TestScratch/<config>/<suite>, but
// handoff_check, an IDE and a shell still start them in the root — so the source itself must not treat the
// working directory as a place to put things. A test that needs a directory holds a
// Desert::TestSupport::ScratchDir / ScratchWorkingDirectory (Desert/Tests/TestSupport/scratch_dir.hpp).
//
// WHAT IS READ AS "WRITES FROM current_path()": the working directory READ (`current_path()` or
// `current_path( ec )`) and then
//   - joined onto (`current_path() / "X"`) — the shape that built RegistryProbe/;
//   - handed to SetProjectRoot — every cache, cook and registry write of the code under test then lands in it;
//   - stored under a name that says it is a place to write (project/output/scratch/dest/cache).
// Reading the working directory to walk UP to the repository root, to name it in a failure message, or to
// save it for a restore is not writing and is not flagged. The census is lexical, so an exception is a row in
// kExceptions with its reason, never a relaxed rule.

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    struct Exception
    {
        const char* File; // repository-relative, generic separators
        const char* Why;
    };

    // One row per file that may keep a flagged line. Empty: every writer found on 2026-09-28 was moved onto
    // the scratch helpers instead.
    // std::array, not a C array: a zero-length C array is ill-formed (and refused by MSVC).
    constexpr std::array<Exception, 0> kExceptions{};

    fs::path RepoRoot()
    {
        for ( fs::path p = fs::current_path(); !p.empty(); p = p.parent_path() )
        {
            if ( fs::exists( p / "Desert" / "Tests" / "TestSupport" ) && fs::exists( p / "Editor" ) )
                return p;
            if ( p == p.parent_path() )
                break;
        }
        return {};
    }

    // The rule, on one line of source. Pure, so the self-tests below pin it without touching the tree.
    bool WritesFromWorkingDirectory( const std::string& line )
    {
        static const std::regex kRead( R"(current_path\(\s*(ec)?\s*\))" );
        static const std::regex kJoin( R"(current_path\(\s*(ec)?\s*\)\s*/)" );
        static const std::regex kProjectRoot( R"(SetProjectRoot\s*\(.*current_path\()" );
        static const std::regex kWriteName(
             R"(\b([A-Za-z0-9_]*(Project|project|Out|out|Scratch|scratch|Dest|dest|Cache|cache)[A-Za-z0-9_]*)\s*=\s*[A-Za-z:]*current_path\()" );
        // A comment names the shape without having it (this census's own header, scratch_dir.hpp's).
        const size_t code = line.find_first_not_of( " \t" );
        if ( code != std::string::npos && line.compare( code, 2, "//" ) == 0 )
            return false;
        if ( !std::regex_search( line, kRead ) )
            return false;
        return std::regex_search( line, kJoin ) || std::regex_search( line, kProjectRoot ) ||
               std::regex_search( line, kWriteName );
    }

    // THE SECOND HALF: A TEST READS THE CHECKOUT THROUGH ITS ROOT, NEVER THROUGH THE WORKING DIRECTORY.
    // Under the runners the working directory is build/TestScratch/<config>/<suite>, so a corpus file or a
    // source named as "Editor/..." from the working directory is not there (the first runner sweep, 09-28:
    // AssetHandleStability, CookedAssetRegistry, FontBaker, WorldCells). Flagged: a repository-relative literal
    // (Editor/, Desert/, Tools/, Runtime/, scripts/) that becomes a path, a stream or a filesystem call's
    // argument ON THE SAME LINE. Joined onto a root (`root / "Editor/..."`, `root + "Editor/..."`) or handed
    // to a helper that joins it is fine — a helper that uses its argument raw is beyond a lexical census,
    // which is why the helper to reach for is Desert::TestSupport::RepositoryRoot().
    bool ReadsRepositoryRelative( const std::string& line )
    {
        const size_t code = line.find_first_not_of( " \t" );
        if ( code != std::string::npos && line.compare( code, 2, "//" ) == 0 )
            return false;
        static const std::string kRel = R"("(Editor|Desert|Tools|Runtime|scripts)/)";
        static const std::regex  kPathVar( R"(\bpath\s+\w+\s*(=\s*|\(\s*|\{\s*))" + kRel );
        static const std::regex  kPathTemp( R"(\bpath\s*[({]\s*)" + kRel );
        static const std::regex  kStream( R"(\b[io]?fstream(\s+\w+)?\s*[({]\s*)" + kRel );
        static const std::regex  kFsCall(
             R"(\b(exists|copy_file|copy|is_regular_file|is_directory|file_size|recursive_directory_iterator|directory_iterator|last_write_time|remove|remove_all|canonical|absolute)\s*\(\s*)" +
             kRel );
        // The helper shape the sweep also found (AssetHandleStability's CopyCorpus): a parameter named
        // `relative` handed straight to a filesystem call or a stream.
        static const std::regex kRawParam(
             R"(\b(exists|copy_file|copy|is_regular_file|recursive_directory_iterator|directory_iterator|file_size)\s*\(\s*relative\b|\b[io]?fstream(\s+\w+)?\s*[({]\s*relative\b)" );
        return std::regex_search( line, kPathVar ) || std::regex_search( line, kPathTemp ) ||
               std::regex_search( line, kStream ) || std::regex_search( line, kFsCall ) ||
               std::regex_search( line, kRawParam );
    }

    TEST( TestScratchCensus, TheReadRuleFlagsRepositoryRelativePaths )
    {
        EXPECT_TRUE( ReadsRepositoryRelative(
             R"(    const fs::path corpus = "Editor/Resources/Assets/Prefabs/UI_Card.deprefab";)" ) );
        EXPECT_TRUE(
             ReadsRepositoryRelative( R"(std::ifstream in( "Desert/Desert/Source/Engine/Core/Scene.cpp" );)" ) );
        EXPECT_TRUE( ReadsRepositoryRelative( R"(ASSERT_TRUE( fs::exists( "Engine/Content/Fonts/R.ttf" ) );)" ) );
        EXPECT_TRUE(
             ReadsRepositoryRelative( R"(for ( auto& e : fs::recursive_directory_iterator( "Tools/X" ) ))" ) );
        EXPECT_TRUE( ReadsRepositoryRelative( R"(const auto p = std::filesystem::path( "scripts/CI/x.sh" );)" ) );
        EXPECT_TRUE(
             ReadsRepositoryRelative( R"(std::filesystem::copy_file( relative, file, options, copied );)" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(std::filesystem::copy_file( root / relative, file );)" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(const fs::path f = root / "Engine/Content/Fonts/R.ttf";)" ) );
        EXPECT_FALSE(
             ReadsRepositoryRelative( R"(std::ifstream in( RepoRoot() + "Editor/Source/EditorLayer.cpp" );)" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(if ( rel.starts_with( "Desert/Tests/" ) ))" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(    // std::ifstream in( "Editor/x" ) was the old shape)" ) );
    }

    TEST( TestScratchCensus, TheRuleFlagsTheWritingShapes )
    {
        EXPECT_TRUE( WritesFromWorkingDirectory(
             R"(    const std::filesystem::path projectDir = std::filesystem::current_path() / "RegistryProbe";)" ) );
        EXPECT_TRUE( WritesFromWorkingDirectory( R"(auto dir = fs::current_path( ec ) / "Assets";)" ) );
        EXPECT_TRUE( WritesFromWorkingDirectory(
             R"(Common::Constants::Path::SetProjectRoot( std::filesystem::current_path(), "Content" );)" ) );
        EXPECT_TRUE( WritesFromWorkingDirectory( R"(            m_ProjectDir = fs::current_path();)" ) );
        EXPECT_TRUE( WritesFromWorkingDirectory( R"(const fs::path outDir = fs::current_path();)" ) );
    }

    TEST( TestScratchCensus, TheRuleLeavesReadsAlone )
    {
        EXPECT_FALSE( WritesFromWorkingDirectory(
             R"(for ( fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path() ))" ) );
        EXPECT_FALSE(
             WritesFromWorkingDirectory( R"(std::filesystem::path here = std::filesystem::current_path();)" ) );
        EXPECT_FALSE(
             WritesFromWorkingDirectory( R"(<< "walked up from " << std::filesystem::current_path();)" ) );
        EXPECT_FALSE( WritesFromWorkingDirectory( R"(fs::path Old = fs::current_path();)" ) );
        EXPECT_FALSE( WritesFromWorkingDirectory(
             R"(    // a probe built as `current_path() / "RegistryProbe"` landed)" ) );
        EXPECT_FALSE( WritesFromWorkingDirectory( R"(std::filesystem::current_path( m_Previous, ec );)" ) );
    }

    TEST( TestScratchCensus, NoTestLeansOnTheWorkingDirectory )
    {
        const fs::path root = RepoRoot();
        ASSERT_FALSE( root.empty() ) << "repository root not found from " << fs::current_path();

        std::set<std::string> excepted;
        for ( const Exception& row : kExceptions )
        {
            excepted.insert( row.File );
            EXPECT_TRUE( fs::exists( root / row.File ) ) << "stale exception row: " << row.File;
        }

        const std::string        self = "Desert/Tests/Tools/TestScratchCensus/test_scratch_census_test.cpp";
        std::vector<std::string> hits;
        size_t                   scanned = 0;
        std::set<std::string>    exceptedUsed;
        for ( const auto& entry : fs::recursive_directory_iterator( root / "Desert" / "Tests" ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const std::string ext = entry.path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" )
                continue;
            const std::string rel = entry.path().lexically_relative( root ).generic_string();
            if ( rel == self )
                continue;
            ++scanned;
            std::ifstream in( entry.path() );
            std::string   line;
            int           number = 0;
            while ( std::getline( in, line ) )
            {
                ++number;
                if ( !WritesFromWorkingDirectory( line ) && !ReadsRepositoryRelative( line ) )
                    continue;
                if ( excepted.contains( rel ) )
                {
                    exceptedUsed.insert( rel );
                    continue;
                }
                hits.push_back( rel + ":" + std::to_string( number ) + ": " + line );
            }
        }

        // A census that scanned nothing passes vacuously; the tree has hundreds of test sources.
        EXPECT_GT( scanned, 200u ) << "the census walked " << ( root / "Desert" / "Tests" ).string();
        for ( const std::string& hit : hits )
            ADD_FAILURE() << "leans on the working directory — write under Desert::TestSupport::ScratchDir / "
                             "ScratchWorkingDirectory, read the checkout through RepositoryRoot() "
                             "(Desert/Tests/TestSupport/scratch_dir.hpp):\n  "
                          << hit;
        for ( const Exception& row : kExceptions )
            EXPECT_TRUE( exceptedUsed.contains( row.File ) )
                 << "exception row matches nothing any more, remove it: " << row.File;
    }
} // namespace

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
