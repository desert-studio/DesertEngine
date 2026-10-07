// The one `main` of every test runner. Contract and rationale: TestSupport/runner.hpp.

#include "runner.hpp"

#include "engine_dir.hpp"
#include "project_scope.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::TestSupport
{
    namespace
    {
        constexpr std::string_view kSuiteFlag = "--desert-suite=";
        constexpr std::string_view kChildFlag = "--desert-child=";

        // Exit code for a selection the runner refuses (unknown suite, empty selection, unknown child):
        // distinct from gtest's 1 so a script can tell "tests failed" from "nothing was run".
        constexpr int kSelectionError = 2;

        // Function-local so it exists before the first ChildEntry constructor runs, whatever the
        // static-initialisation order of the translation units.
        std::map<std::string_view, std::pair<ChildMain, SuiteHostSteps>>& ChildTable()
        {
            static std::map<std::string_view, std::pair<ChildMain, SuiteHostSteps>> table;
            return table;
        }

        // Path suffix -> adopting suite. Function-local for the same reason as ChildTable.
        std::map<std::string_view, std::string>& AdoptedTable()
        {
            static std::map<std::string_view, std::string> table;
            return table;
        }

        // Suite -> its environments, in registration order. Function-local for the same reason as ChildTable.
        std::map<std::string, SuiteHostSteps>& HostTable()
        {
            static std::map<std::string, SuiteHostSteps> table;
            return table;
        }

        std::vector<std::pair<std::string, EnvironmentFactory>>& EnvironmentTable()
        {
            static std::vector<std::pair<std::string, EnvironmentFactory>> table;
            return table;
        }

        void PrintError( const std::string& text )
        {
            std::fputs( std::format( "[desert-runner] {}\n", text ).c_str(), stderr );
        }

        // The suite of a test = the directory right under `Desert/Tests/<Layer>/` in the path of the
        // source file that defined it. nullopt when the path is not under that shape (a test defined in
        // a TestSupport header, say), which the selection reports instead of guessing.
        std::optional<std::string> SuiteOfFile( std::string_view file )
        {
            std::string normalised( file );
            for ( char& c : normalised )
            {
                if ( c == '\\' )
                {
                    c = '/';
                }
            }
            constexpr std::string_view kRoot = "Desert/Tests/";
            const size_t               root  = normalised.rfind( kRoot );
            if ( root == std::string::npos )
            {
                return std::nullopt;
            }
            const size_t layerBegin = root + kRoot.size();
            const size_t layerEnd   = normalised.find( '/', layerBegin );
            if ( layerEnd == std::string::npos )
            {
                return std::nullopt;
            }
            const size_t suiteBegin = layerEnd + 1;
            const size_t suiteEnd   = normalised.find( '/', suiteBegin );
            if ( suiteEnd == std::string::npos || suiteEnd == suiteBegin )
            {
                return std::nullopt;
            }
            return normalised.substr( suiteBegin, suiteEnd - suiteBegin );
        }

        // SuiteOfFile, or the suite that adopted the file (AdoptedTestSource).
        std::optional<std::string> SuiteOfTest( std::string_view file )
        {
            if ( auto suite = SuiteOfFile( file ) )
            {
                return suite;
            }
            std::string normalised( file );
            std::replace( normalised.begin(), normalised.end(), '\\', '/' );
            for ( const auto& [suffix, suite] : AdoptedTable() )
            {
                if ( normalised.ends_with( suffix ) )
                {
                    return suite;
                }
            }
            return std::nullopt;
        }

        // gtest's own filter grammar ("pos1:pos2-neg1:neg2", '*' and '?' wildcards), so `--gtest_filter`
        // given together with `--desert-suite` narrows the suite instead of being overwritten by it.
        bool MatchesPattern( std::string_view pattern, std::string_view name )
        {
            size_t p = 0, n = 0, starP = std::string_view::npos, starN = 0;
            while ( n < name.size() )
            {
                if ( p < pattern.size() && ( pattern[p] == '?' || pattern[p] == name[n] ) )
                {
                    ++p;
                    ++n;
                }
                else if ( p < pattern.size() && pattern[p] == '*' )
                {
                    starP = p++;
                    starN = n;
                }
                else if ( starP != std::string_view::npos )
                {
                    p = starP + 1;
                    n = ++starN;
                }
                else
                {
                    return false;
                }
            }
            while ( p < pattern.size() && pattern[p] == '*' )
            {
                ++p;
            }
            return p == pattern.size();
        }

        bool MatchesAnyPattern( std::string_view patterns, std::string_view name )
        {
            size_t begin = 0;
            while ( begin <= patterns.size() )
            {
                const size_t end = std::min( patterns.find( ':', begin ), patterns.size() );
                if ( MatchesPattern( patterns.substr( begin, end - begin ), name ) )
                {
                    return true;
                }
                begin = end + 1;
            }
            return false;
        }

        bool MatchesGtestFilter( std::string_view filter, std::string_view name )
        {
            const size_t     dash     = filter.find( '-' );
            std::string_view positive = filter.substr( 0, dash );
            if ( positive.empty() )
            {
                positive = "*";
            }
            if ( !MatchesAnyPattern( positive, name ) )
            {
                return false;
            }
            return dash == std::string_view::npos || !MatchesAnyPattern( filter.substr( dash + 1 ), name );
        }

        std::set<std::string> SplitSuiteList( std::string_view list )
        {
            std::set<std::string> suites;
            size_t                begin = 0;
            while ( begin <= list.size() )
            {
                const size_t end = std::min( list.find( ',', begin ), list.size() );
                if ( end > begin )
                {
                    suites.emplace( list.substr( begin, end - begin ) );
                }
                begin = end + 1;
            }
            return suites;
        }

        // The suites named by --desert-suite, read before InitGoogleTest (which leaves the flag in argv) so
        // the host steps run first; nullopt = no selection. An unknown or empty selection is refused after gtest starts.
        std::optional<std::set<std::string>> PeekSuiteSelection( int argc, char** argv )
        {
            std::optional<std::set<std::string>> suites;
            for ( int i = 1; i < argc; ++i )
            {
                const std::string_view arg( argv[i] );
                if ( arg.starts_with( kSuiteFlag ) )
                {
                    suites = SplitSuiteList( arg.substr( kSuiteFlag.size() ) );
                }
            }
            return suites;
        }

        // Rewrites gtest's filter to exactly the tests of `suites` that the user's filter (if any) keeps.
        // Returns false, having printed why, when a suite is unknown or the selection is empty.
        bool SelectSuites( const std::set<std::string>& suites )
        {
            if ( suites.empty() )
            {
                PrintError( std::format( "{} names no suite", kSuiteFlag ) );
                return false;
            }
            const std::string          userFilter = GTEST_FLAG_GET( filter );
            const testing::UnitTest&   unitTest   = *testing::UnitTest::GetInstance();
            std::set<std::string>      known;
            std::vector<std::string>   unmapped;
            std::map<std::string, int> selectedPerSuite;
            std::string                filter;
            for ( int s = 0; s < unitTest.total_test_suite_count(); ++s )
            {
                const testing::TestSuite& testSuite = *unitTest.GetTestSuite( s );
                for ( int t = 0; t < testSuite.total_test_count(); ++t )
                {
                    const testing::TestInfo& info = *testSuite.GetTestInfo( t );
                    const std::string fullName    = std::format( "{}.{}", info.test_suite_name(), info.name() );
                    const auto               suite       = SuiteOfTest( info.file() );
                    if ( !suite )
                    {
                        unmapped.push_back( std::format( "{} ({})", fullName, info.file() ) );
                        continue;
                    }
                    known.insert( *suite );
                    if ( !suites.contains( *suite ) || !MatchesGtestFilter( userFilter, fullName ) )
                    {
                        continue;
                    }
                    ++selectedPerSuite[*suite];
                    std::format_to( std::back_inserter( filter ), "{}{}", filter.empty() ? "" : ":", fullName );
                }
            }
            bool ok = true;
            for ( const std::string& name : unmapped )
            {
                PrintError( std::format( "test outside Desert/Tests/<Layer>/<Suite>/: {}", name ) );
                ok = false;
            }
            for ( const std::string& suite : suites )
            {
                if ( !known.contains( suite ) )
                {
                    PrintError( std::format( "unknown suite '{}': no test of this runner is defined under a "
                                             "directory of that name",
                                             suite ) );
                    ok = false;
                }
                else if ( !selectedPerSuite.contains( suite ) )
                {
                    PrintError(
                         std::format( "suite '{}' selects no test under --gtest_filter={}", suite, userFilter ) );
                    ok = false;
                }
            }
            if ( ok )
            {
                GTEST_FLAG_SET( filter, filter );
            }
            return ok;
        }
    } // namespace

    ChildEntry::ChildEntry( std::string_view name, ChildMain main, SuiteHostSteps steps )
    {
        auto [it, inserted] = ChildTable().emplace( name, std::pair{ main, steps } );
        if ( !inserted )
        {
            PrintError( std::format( "two --desert-child entry points are registered as '{}'", name ) );
            std::abort();
        }
    }

    AdoptedTestSource::AdoptedTestSource( std::string_view pathSuffix, const char* where )
    {
        const auto suite = SuiteOfFile( where );
        if ( !suite )
        {
            PrintError(
                 std::format( "AdoptedTestSource '{}' is constructed outside Desert/Tests/<Layer>/<Suite>/ ({})",
                              pathSuffix, where ) );
            std::abort();
        }
        auto [it, inserted] = AdoptedTable().emplace( pathSuffix, *suite );
        if ( !inserted )
        {
            PrintError(
                 std::format( "'{}' is adopted by two suites, '{}' and '{}'", pathSuffix, it->second, *suite ) );
            std::abort();
        }
    }
    SuiteEnvironment::SuiteEnvironment( EnvironmentFactory make, const char* where )
    {
        const auto suite = SuiteOfFile( where );
        if ( !suite || make == nullptr )
        {
            PrintError( std::format( "SuiteEnvironment is constructed outside Desert/Tests/<Layer>/<Suite>/ or "
                                     "without a factory ({})",
                                     where ) );
            std::abort();
        }
        EnvironmentTable().emplace_back( *suite, make );
    }

    SuiteHost::SuiteHost( SuiteHostSteps steps, const char* where )
    {
        const auto suite = SuiteOfFile( where );
        if ( !suite || ( !steps.EngineDir && !steps.Project ) )
        {
            PrintError( std::format( "SuiteHost is constructed outside Desert/Tests/<Layer>/<Suite>/ or "
                                     "declares no step ({})",
                                     where ) );
            std::abort();
        }
        auto [it, inserted] = HostTable().emplace( *suite, steps );
        if ( !inserted )
        {
            PrintError( std::format( "suite '{}' declares its SuiteHost twice ({})", *suite, where ) );
            std::abort();
        }
    }
} // namespace Desert::TestSupport

int main( int argc, char** argv )
{
    using namespace Desert::TestSupport;

    // A child runs instead of gtest, with the arguments that followed the flag.
    if ( argc >= 2 && std::string_view( argv[1] ).starts_with( kChildFlag ) )
    {
        const std::string_view name = std::string_view( argv[1] ).substr( kChildFlag.size() );
        const auto             it   = ChildTable().find( name );
        if ( it == ChildTable().end() )
        {
            PrintError( std::format( "no --desert-child entry point named '{}' in this runner", name ) );
            return kSelectionError;
        }
        argv[1] = argv[0];
        const auto& [childMain, steps] = it->second;
        if ( steps.EngineDir )
        {
            SetSuiteEngineDir();
        }
        if ( steps.Project )
        {
            OpenSuiteProject();
        }
        return childMain( argc - 1, argv + 1 );
    }

    // The host steps the selected suites declared, before gtest parses its flags (SuiteHost, runner.hpp).
    const auto hostSuites = PeekSuiteSelection( argc, argv );
    for ( const auto& [suite, steps] : HostTable() )
    {
        if ( steps.EngineDir && ( !hostSuites || hostSuites->contains( suite ) ) )
        {
            SetSuiteEngineDir();
        }
    }
    for ( const auto& [suite, steps] : HostTable() )
    {
        if ( steps.Project && ( !hostSuites || hostSuites->contains( suite ) ) )
        {
            OpenSuiteProject();
        }
    }

    testing::InitGoogleTest( &argc, argv );

    // InitGoogleTest removed its own flags; what is left is ours or a mistake.
    std::optional<std::set<std::string>> suites;
    for ( int i = 1; i < argc; ++i )
    {
        const std::string_view arg( argv[i] );
        if ( arg.starts_with( kSuiteFlag ) )
        {
            suites = SplitSuiteList( arg.substr( kSuiteFlag.size() ) );
        }
        else
        {
            PrintError( std::format( "unknown argument '{}'", arg ) );
            return kSelectionError;
        }
    }
    if ( suites && !SelectSuites( *suites ) )
    {
        return kSelectionError;
    }
    for ( const auto& [suite, make] : EnvironmentTable() )
    {
        if ( !suites || suites->contains( suite ) )
        {
            ::testing::AddGlobalTestEnvironment( make() );
        }
    }
    return RUN_ALL_TESTS();
}
