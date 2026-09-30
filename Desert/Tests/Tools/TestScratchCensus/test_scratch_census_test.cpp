// NO TEST TOUCHES THE WORKING DIRECTORY (TST1, ENG-ROOT).
//
// Owner, 2026-09-27: Assets/, DerivedDataCache/ and RegistryProbe/ appeared in the root of the Windows checkout —
// suites built probe projects as `current_path() / "RegistryProbe"`. Owner, 2026-09-30 (ENG-ROOT): no chdir, no
// cwd, anywhere. A suite is a host like the editor (UE: FPaths::EngineDir / ProjectDir): it points the engine at
// its engine directory (Desert::TestSupport::EngineDirScope, Desert/Tests/TestSupport/engine_dir.hpp), opens a
// project with SetProjectRoot, writes under a Desert::TestSupport::ScratchDir and reads the checkout through
// RepositoryRoot() (scratch_dir.hpp) — every path explicit, so it runs from any folder.
//
// THE RULE: `current_path(` on a line of code in Desert/Tests — reading the working directory or moving it —
// is flagged. The only lawful ones are tests whose SUBJECT is the working directory ("X does not follow the
// working directory" has to stand the process somewhere else to prove it); each is a row in kExceptions naming
// the file, the tests and the reason. The census is lexical, so an exception is a row, never a relaxed rule.

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../../TestSupport/scratch_dir.hpp"

namespace
{
    namespace fs = std::filesystem;

    struct Exception
    {
        const char* File; // repository-relative, generic separators
        const char* Why;
    };

    // One row per file that may keep a flagged line, with the tests that are about the working directory.
    constexpr std::array<Exception, 2> kExceptions{ {
         { "Desert/Tests/Engine/AssetHandleStability/asset_handle_stability_test.cpp",
           "ASyntheticKeyDoesNotFollowTheWorkingDirectory moves the process to prove a procedural:// key does not "
           "follow it." },
         { "Desert/Tests/Tools/TestScratchCensus/test_scratch_census_test.cpp", "the rule's own self-tests." },
    } };

    fs::path RepoRoot()
    {
        return Desert::TestSupport::RepositoryRoot();
    }

    // The rule, on one line of source. Pure, so the self-tests below pin it without touching the tree.
    bool TouchesTheWorkingDirectory( const std::string& line )
    {
        static const std::regex kCall( R"(\bcurrent_path\s*\()" );
        // A comment names the call without making it (this census's own header, scratch_dir.hpp's).
        const size_t code = line.find_first_not_of( " \t" );
        if ( code != std::string::npos && line.compare( code, 2, "//" ) == 0 )
            return false;
        return std::regex_search( line, kCall );
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
        EXPECT_TRUE(
             ReadsRepositoryRelative( R"(ASSERT_TRUE( fs::exists( "Editor/Resources/Fonts/R.ttf" ) );)" ) );
        EXPECT_TRUE(
             ReadsRepositoryRelative( R"(for ( auto& e : fs::recursive_directory_iterator( "Tools/X" ) ))" ) );
        EXPECT_TRUE( ReadsRepositoryRelative( R"(const auto p = std::filesystem::path( "scripts/CI/x.sh" );)" ) );
        EXPECT_TRUE(
             ReadsRepositoryRelative( R"(std::filesystem::copy_file( relative, file, options, copied );)" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(std::filesystem::copy_file( root / relative, file );)" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(const fs::path f = root / "Editor/Resources/Fonts/R.ttf";)" ) );
        EXPECT_FALSE(
             ReadsRepositoryRelative( R"(std::ifstream in( RepoRoot() + "Editor/Source/EditorLayer.cpp" );)" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(if ( rel.starts_with( "Desert/Tests/" ) ))" ) );
        EXPECT_FALSE( ReadsRepositoryRelative( R"(    // std::ifstream in( "Editor/x" ) was the old shape)" ) );
    }

    TEST( TestScratchCensus, TheRuleFlagsEveryWorkingDirectoryCall )
    {
        EXPECT_TRUE( TouchesTheWorkingDirectory(
             R"(    const std::filesystem::path projectDir = std::filesystem::current_path() / "RegistryProbe";)" ) );
        EXPECT_TRUE( TouchesTheWorkingDirectory( R"(auto dir = fs::current_path( ec ) / "Assets";)" ) );
        EXPECT_TRUE( TouchesTheWorkingDirectory( R"(std::filesystem::current_path( here / "Editor" );)" ) );
        EXPECT_TRUE(
             TouchesTheWorkingDirectory( R"(std::filesystem::path here = std::filesystem::current_path();)" ) );
        EXPECT_TRUE( TouchesTheWorkingDirectory( R"(<< "walked up from " << std::filesystem::current_path();)" ) );
        EXPECT_TRUE( TouchesTheWorkingDirectory( R"(std::filesystem::current_path( m_Previous, ec );)" ) );
    }

    TEST( TestScratchCensus, TheRuleLeavesCommentsAndTheHostStepAlone )
    {
        EXPECT_FALSE( TouchesTheWorkingDirectory(
             R"(    // a probe built as `current_path() / "RegistryProbe"` landed)" ) );
        EXPECT_FALSE(
             TouchesTheWorkingDirectory( R"(const Desert::TestSupport::EngineDirScope engineDir( dir );)" ) );
        EXPECT_FALSE( TouchesTheWorkingDirectory( R"(Common::Constants::Path::SetEngineDir( pkg );)" ) );
    }

    TEST( TestScratchCensus, NoTestLeansOnTheWorkingDirectory )
    {
        const fs::path root = RepoRoot();
        ASSERT_FALSE( root.empty() ) << "the baked repository root is empty";

        std::set<std::string> excepted;
        for ( const Exception& row : kExceptions )
        {
            excepted.insert( row.File );
            EXPECT_TRUE( fs::exists( root / row.File ) ) << "stale exception row: " << row.File;
        }

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
            ++scanned;
            std::ifstream in( entry.path() );
            std::string   line;
            int           number = 0;
            while ( std::getline( in, line ) )
            {
                ++number;
                if ( !TouchesTheWorkingDirectory( line ) && !ReadsRepositoryRelative( line ) )
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
            ADD_FAILURE()
                 << "leans on the working directory — point the engine with EngineDirScope (engine_dir.hpp), "
                    "write under ScratchDir, read the checkout through RepositoryRoot() "
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
