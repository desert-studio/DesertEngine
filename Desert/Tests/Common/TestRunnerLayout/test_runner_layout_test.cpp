// THE TEST TREE IS LAID OUT FOR ONE RUNNER PER LAYER (BUILD1 P1, BuildScripts/BUILD1-CONTRACT.md §4).
//
// Every suite of a layer is compiled into ONE executable (`<Layer>Tests`, Desert/Tests/premake5.lua) and
// chosen at run time with `--desert-suite=<Suite>`. That only stays correct while the tree keeps four
// properties nothing else checks:
//
//   1. A suite is either a runner suite (no premake5.lua, no `main`) or, until the second half of P1
//      converts it, its own project (a premake5.lua AND its own `main`). One without the other is a suite
//      whose `main` collides with RunnerMain's, or a project that links no `main` at all.
//   2. No `main` under Desert/Tests other than TestSupport/RunnerMain.cpp and those unconverted suites.
//   3. No gtest test-suite name in two suite directories of one runner: gtest aborts the whole runner
//      when one name maps to two fixture classes, and TEST() names of two suites would merge in reports.
//   4. No class or struct DEFINED at namespace scope outside an anonymous namespace in a runner suite's
//      .cpp: two suites' `struct Carrier` in one binary is an ODR violation the linker resolves silently
//      (one suite runs against the other's inline members). JsonDocument and JsonFacade both had one.
//
// HOW TO CONVERT A SUITE (what the messages below point at): delete its premake5.lua and its `int main`;
// move what the script added (include dir, library, tool source) into the runner's entry in kRunners,
// Desert/Tests/premake5.lua; put file-local types into `namespace { }`; give its tests a test-suite name
// no other suite of the layer uses. BuildScripts/BUILD1-CONTRACT.md, "Adding a suite".

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kHowToConvert =
         "convert it: delete its premake5.lua and its `int main`, move what the script "
         "added into kRunners in Desert/Tests/premake5.lua (BuildScripts/"
         "BUILD1-CONTRACT.md, \"Adding a suite\")";

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

    std::string ReadFile( const fs::path& file )
    {
        std::ifstream      in( file, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    bool IsIdentifierChar( char c )
    {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
    }

    // The source with comments removed and every string or character literal (raw ones included) replaced
    // by `""`, so braces, `main` and `struct` inside them are not read as code. Line breaks are kept, so a
    // line number in the result is a line number in the file.
    std::string StripCode( const std::string& text )
    {
        std::string out;
        out.reserve( text.size() );
        const size_t n = text.size();
        size_t       i = 0;
        while ( i < n )
        {
            if ( text.compare( i, 2, "//" ) == 0 )
            {
                const size_t end = text.find( '\n', i );
                i                = end == std::string::npos ? n : end;
                continue;
            }
            if ( text.compare( i, 2, "/*" ) == 0 )
            {
                const size_t end = text.find( "*/", i + 2 );
                for ( size_t k = i; k < std::min( n, end == std::string::npos ? n : end ); ++k )
                {
                    if ( text[k] == '\n' )
                        out += '\n';
                }
                i = end == std::string::npos ? n : end + 2;
                continue;
            }
            const bool atTokenStart = i == 0 || !IsIdentifierChar( text[i - 1] );
            if ( text[i] == 'R' && i + 1 < n && text[i + 1] == '"' &&
                 ( atTokenStart || ( i > 0 && ( text[i - 1] == '8' || text[i - 1] == 'u' || text[i - 1] == 'U' ||
                                                text[i - 1] == 'L' ) ) ) )
            {
                const size_t open = text.find( '(', i + 2 );
                if ( open != std::string::npos )
                {
                    const std::string close = std::format( "){}\"", text.substr( i + 2, open - ( i + 2 ) ) );
                    const size_t      end   = text.find( close, open + 1 );
                    const size_t      stop  = end == std::string::npos ? n : end + close.size();
                    out += "\"\"";
                    for ( size_t k = i; k < stop; ++k )
                    {
                        if ( text[k] == '\n' )
                            out += '\n';
                    }
                    i = stop;
                    continue;
                }
            }
            // A quote after a digit is a digit separator (1'000), not a character literal.
            const bool quote =
                 text[i] == '"' || ( text[i] == '\'' && ( i == 0 || !IsIdentifierChar( text[i - 1] ) ) );
            if ( quote )
            {
                const char delimiter = text[i];
                size_t     k         = i + 1;
                while ( k < n && text[k] != delimiter && text[k] != '\n' )
                    k += text[k] == '\\' ? 2 : 1;
                out += "\"\"";
                i = k + 1;
                continue;
            }
            out += text[i];
            ++i;
        }
        return out;
    }

    bool DefinesMain( const std::string& code )
    {
        static const std::regex kMain( R"((^|[^\w:])int\s+main\s*\()" );
        return std::regex_search( StripCode( code ), kMain );
    }

    // "<line>: struct Name" for each class/struct DEFINED where every enclosing scope is a NAMED namespace
    // (or the global one). Forward declarations, `enum class` and types inside functions, classes or an
    // anonymous namespace are not reported.
    std::vector<std::string> NamespaceScopeTypes( const std::string& code )
    {
        static const std::regex kToken(
             R"(\bnamespace\b(\s+[\w:]+)?\s*\{|\b(class|struct)\s+(\w+)\s*(final\s*)?([:{])|[{}])" );
        const std::string        stripped = StripCode( code );
        std::vector<std::string> found;
        enum class Scope
        {
            Named,
            Anonymous,
            Block
        };
        std::vector<Scope> stack;
        for ( auto it = std::sregex_iterator( stripped.begin(), stripped.end(), kToken );
              it != std::sregex_iterator(); ++it )
        {
            const std::smatch& m = *it;
            const std::string  token( m[0] );
            if ( token.starts_with( "namespace" ) )
            {
                stack.push_back( m[1].matched ? Scope::Named : Scope::Anonymous );
            }
            else if ( m[2].matched )
            {
                const size_t            start      = static_cast<size_t>( m.position( 0 ) );
                const size_t            lookBehind = std::min<size_t>( start, 12 );
                static const std::regex kEnum( R"(\benum\s*$)" );
                const bool isEnum = std::regex_search( stripped.substr( start - lookBehind, lookBehind ), kEnum );
                const bool exposed =
                     std::all_of( stack.begin(), stack.end(), []( Scope s ) { return s == Scope::Named; } );
                if ( exposed && !isEnum )
                {
                    const auto line = std::count( stripped.begin(), stripped.begin() + start, '\n' ) + 1;
                    found.push_back( std::format( "{}: {} {}", line, m[2].str(), m[3].str() ) );
                }
                if ( m[5].str() == "{" )
                    stack.push_back( Scope::Block );
            }
            else if ( token == "{" )
            {
                stack.push_back( Scope::Block );
            }
            else if ( token == "}" && !stack.empty() )
            {
                stack.pop_back();
            }
        }
        return found;
    }

    // The gtest test-suite names a file declares (first argument of TEST, TEST_F, TEST_P, TYPED_TEST...).
    std::set<std::string> TestSuiteNames( const std::string& code )
    {
        static const std::regex kTest(
             R"(\b(TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P|TYPED_TEST_SUITE|INSTANTIATE_TEST_SUITE_P)\s*\(\s*(\w+)\s*,\s*(\w+))" );
        const std::string     stripped = StripCode( code );
        std::set<std::string> names;
        for ( auto it = std::sregex_iterator( stripped.begin(), stripped.end(), kTest );
              it != std::sregex_iterator(); ++it )
        {
            // INSTANTIATE_TEST_SUITE_P( Prefix, SuiteName, ... ): the suite is the second argument.
            names.insert( ( *it )[1].str() == "INSTANTIATE_TEST_SUITE_P" ? ( *it )[3].str() : ( *it )[2].str() );
        }
        return names;
    }

    struct Suite
    {
        std::string           layer;
        std::string           name;
        fs::path              dir;
        bool                  ownProject = false; // still has its own premake5.lua (not converted yet)
        std::vector<fs::path> sources;            // *.cpp directly in the suite directory
    };

    std::vector<Suite> Suites( const fs::path& root )
    {
        std::vector<Suite> suites;
        for ( const char* layer : { "Common", "Engine", "Editor", "Runtime", "Tools" } )
        {
            const fs::path layerDir = root / "Desert" / "Tests" / layer;
            if ( !fs::is_directory( layerDir ) )
                continue;
            for ( const auto& entry : fs::directory_iterator( layerDir ) )
            {
                if ( !entry.is_directory() )
                    continue;
                Suite suite{ layer, entry.path().filename().string(), entry.path() };
                suite.ownProject = fs::exists( entry.path() / "premake5.lua" );
                for ( const auto& file : fs::directory_iterator( entry.path() ) )
                {
                    if ( file.is_regular_file() && file.path().extension() == ".cpp" )
                        suite.sources.push_back( file.path() );
                }
                std::sort( suite.sources.begin(), suite.sources.end() );
                suites.push_back( std::move( suite ) );
            }
        }
        std::sort( suites.begin(), suites.end(),
                   []( const Suite& a, const Suite& b ) { return a.layer + a.name < b.layer + b.name; } );
        return suites;
    }

    std::string Relative( const fs::path& file, const fs::path& root )
    {
        return fs::relative( file, root ).generic_string();
    }

    // ── The rules on text, pinned without the tree ──────────────────────────────────────────────────

    TEST( TestRunnerLayout, StripCodeRemovesCommentsAndLiteralsButKeepsLines )
    {
        const std::string code     = "int a; // struct X {\n/* int main( */ auto s = R\"x(struct Y { )x\";\n"
                                     "char c = '{'; int n = 1'000;\nstruct Z {};";
        const std::string stripped = StripCode( code );
        EXPECT_EQ( std::count( stripped.begin(), stripped.end(), '\n' ), 3 );
        EXPECT_EQ( stripped.find( "struct X" ), std::string::npos );
        EXPECT_EQ( stripped.find( "struct Y" ), std::string::npos );
        EXPECT_EQ( stripped.find( "main" ), std::string::npos );
        EXPECT_NE( stripped.find( "struct Z" ), std::string::npos );
        EXPECT_NE( stripped.find( "1'000" ), std::string::npos );
    }

    TEST( TestRunnerLayout, MainIsFoundInCodeOnly )
    {
        EXPECT_TRUE( DefinesMain( "int main( int argc, char** argv ) { return 0; }" ) );
        EXPECT_TRUE( DefinesMain( "x;\nint main()\n{}" ) );
        EXPECT_FALSE( DefinesMain( "// int main( int, char** )\nconst char* s = \"int main(\";" ) );
        EXPECT_FALSE( DefinesMain( "int domain( int );" ) );
        EXPECT_FALSE( DefinesMain( "int Child::main( int );" ) );
    }

    TEST( TestRunnerLayout, OnlyTypesOutsideAnAnonymousNamespaceAreExposed )
    {
        const std::string code =
             "struct Global { int a; };\n"
             "struct Forward;\n"
             "enum class Mode { A, B };\n"
             "namespace Named\n{\n    class Inside : public Base\n    {\n        struct Nested {};\n"
             "    };\n}\n"
             "namespace\n{\n    struct Hidden {};\n    namespace Deeper { struct AlsoHidden {}; }\n}\n"
             "void F() { struct Local {}; }\n";
        const std::vector<std::string> expected{ "1: struct Global", "6: class Inside" };
        EXPECT_EQ( NamespaceScopeTypes( code ), expected );
    }

    TEST( TestRunnerLayout, TestSuiteNamesAreTheFirstMacroArgument )
    {
        const std::string code = "TEST( Alpha, One ) {}\nTEST_F( BetaFixture, Two ) {}\n// TEST( Gamma, Three )\n"
                                 "INSTANTIATE_TEST_SUITE_P( Every, DeltaParam, ::testing::Values( 1 ) );";
        const std::set<std::string> expected{ "Alpha", "BetaFixture", "DeltaParam" };
        EXPECT_EQ( TestSuiteNames( code ), expected );
    }

    // ── The census over the tree ────────────────────────────────────────────────────────────────────

    TEST( TestRunnerLayout, EverySuiteIsARunnerSuiteOrItsOwnProjectNeverHalfOfEach )
    {
        const fs::path root = RepoRoot();
        ASSERT_FALSE( root.empty() ) << "walked up from " << fs::current_path()
                                     << " without finding the tree root";
        const std::vector<Suite> suites = Suites( root );
        ASSERT_GT( suites.size(), 300u ) << "the suite listing found almost nothing: the census would pass blind";
        size_t runnerSuites = 0;
        for ( const Suite& suite : suites )
        {
            EXPECT_FALSE( suite.sources.empty() )
                 << Relative( suite.dir, root ) << " has no .cpp: a suite directory "
                 << "the runner would select no test from";
            const bool hasMain =
                 std::any_of( suite.sources.begin(), suite.sources.end(),
                              []( const fs::path& file ) { return DefinesMain( ReadFile( file ) ); } );
            if ( suite.ownProject )
            {
                EXPECT_TRUE( hasMain ) << Relative( suite.dir, root )
                                       << " has a premake5.lua but no `main`: " << kHowToConvert;
            }
            else
            {
                ++runnerSuites;
                EXPECT_FALSE( hasMain )
                     << Relative( suite.dir, root ) << " is built into its layer's runner, whose "
                     << "only main is TestSupport/RunnerMain.cpp: delete the suite's `int main`";
            }
        }
        EXPECT_GT( runnerSuites, 0u );
    }

    TEST( TestRunnerLayout, NoMainUnderDesertTestsButTheRunnersAndUnconvertedSuites )
    {
        const fs::path root  = RepoRoot();
        const fs::path tests = root / "Desert" / "Tests";
        ASSERT_TRUE( fs::is_directory( tests ) );
        std::set<fs::path> ownProjects;
        for ( const Suite& suite : Suites( root ) )
        {
            if ( suite.ownProject )
                ownProjects.insert( suite.dir );
        }
        size_t runnerMains = 0;
        for ( const auto& entry : fs::recursive_directory_iterator( tests ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".cpp" ||
                 !DefinesMain( ReadFile( entry.path() ) ) )
                continue;
            if ( entry.path() == tests / "TestSupport" / "RunnerMain.cpp" )
            {
                ++runnerMains;
                continue;
            }
            EXPECT_TRUE( ownProjects.contains( entry.path().parent_path() ) )
                 << Relative( entry.path(), root )
                 << " defines `main`, but it is not the top level of a suite that is "
                 << "still its own project: " << kHowToConvert;
        }
        EXPECT_EQ( runnerMains, 1u ) << "TestSupport/RunnerMain.cpp must define the runners' one main";
    }

    TEST( TestRunnerLayout, NoTestSuiteNameInTwoSuitesOfOneRunner )
    {
        const fs::path                                                      root = RepoRoot();
        std::map<std::string, std::map<std::string, std::set<std::string>>> owners; // layer -> name -> suites
        for ( const Suite& suite : Suites( root ) )
        {
            if ( suite.ownProject )
                continue;
            for ( const fs::path& file : suite.sources )
            {
                for ( const std::string& name : TestSuiteNames( ReadFile( file ) ) )
                    owners[suite.layer][name].insert( suite.name );
            }
        }
        for ( const auto& [layer, names] : owners )
        {
            for ( const auto& [name, suites] : names )
            {
                std::string list;
                for ( const std::string& s : suites )
                    std::format_to( std::back_inserter( list ), "{}{}", list.empty() ? "" : ", ", s );
                EXPECT_EQ( suites.size(), 1u ) << "gtest test-suite '" << name << "' is declared by " << list
                                               << " of the " << layer << "Tests runner: one name may map to one "
                                               << "suite (and one fixture class); rename it in all but one";
            }
        }
    }

    TEST( TestRunnerLayout, NoNamespaceScopeTypeOutsideAnAnonymousNamespace )
    {
        const fs::path root = RepoRoot();
        for ( const Suite& suite : Suites( root ) )
        {
            if ( suite.ownProject )
                continue;
            for ( const fs::path& file : suite.sources )
            {
                for ( const std::string& type : NamespaceScopeTypes( ReadFile( file ) ) )
                {
                    ADD_FAILURE()
                         << Relative( file, root ) << ":" << type << " is defined at namespace scope outside an "
                         << "anonymous namespace; every suite of the layer links into one runner, so a second "
                         << "suite's type of the same name is a silent ODR violation. Wrap it in `namespace { }`";
                }
            }
        }
    }
} // namespace
