/**
 * A27 — A FIXTURE AN IGNORE RULE CAN SWALLOW, AND THE SUITE THAT NEVER SEES IT GO.
 *
 * WHAT THIS CENSUS FORBIDS, stated here so the next reader knows what it stands guard over: a test may
 * not name a file that lives under a WHITELIST ignore rule and is not named by that rule's `!` exception.
 *
 * The incident. `.gitignore` turns `Editor/Cooked/Meshes/` into a whitelist — a blanket
 * `Editor/Cooked/Meshes/` + a star, followed by one `!Editor/Cooked/Meshes/<file>` line per fixture that is
 * allowed through. A25 added two fixtures, `ForeignArm.skeleton` and `ForeignArm_Swing.anim`, and did not
 * add the two `!` lines. `git add` skipped both IN SILENCE. The `.retarget` that names them, living in a
 * directory with no such rule, was committed normally. `Desert/Tests/Engine/RetargetAsset` was 257/257
 * green in the tree where the two files happened to sit on disk, said nothing whatever about `dev`, and
 * when that worktree was removed the bytes were gone — there was no copy anywhere, in the tree or in the
 * object store, because nothing had ever hashed them.
 *
 * Every part of that is silent. The ignore rule is silent, `git add` is silent, the author's own sweep is
 * green, and the first report comes from someone else's clone. THAT is what this file exists to convert
 * into a red test in the author's own tree, on the author's own run, before the commit.
 *
 * It reads the repository, not git: a census that shelled out to `git check-ignore` would be untestable
 * on a machine without the repository and would answer about the index rather than about the RULE. The
 * rule is text in a file, the claim is text in a file, and this compares them.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::filesystem::exists( here / ".gitignore" ) && std::filesystem::exists( here / "Desert" ) )
                return here;
            here = here.parent_path();
        }
        return {};
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    struct Whitelist
    {
        /// Directories under a blanket `<dir>/*` rule, with the trailing slash: `Editor/Cooked/Meshes/`.
        std::set<std::string> Directories;
        /// Every path a `!` line lets back through, verbatim.
        std::set<std::string> Exceptions;
    };

    /**
     * @brief Parse `.gitignore` into the two halves of a whitelist.
     *
     * COMMENTS ARE DROPPED BEFORE ANYTHING IS PARSED, and that is not tidiness. Two censuses in one week
     * went red on PROSE that quoted the very thing they forbid, and the second run had a real finding in
     * it that would have drowned with the false one. This file's own `.gitignore` block explains the
     * incident above and names both fixtures; read as data it would be nonsense.
     */
    Whitelist ParseWhitelist( const std::string& text )
    {
        Whitelist          out;
        std::istringstream in( text );
        std::string        line;
        while ( std::getline( in, line ) )
        {
            while ( !line.empty() && ( line.back() == '\r' || line.back() == ' ' ) )
                line.pop_back();
            if ( line.empty() || line[0] == '#' )
                continue;

            if ( line[0] == '!' )
            {
                out.Exceptions.insert( line.substr( 1 ) );
                continue;
            }
            if ( line.size() > 2 && line.compare( line.size() - 2, 2, "/*" ) == 0 )
                out.Directories.insert( line.substr( 0, line.size() - 1 ) );
        }
        return out;
    }

    /// Every double-quoted literal in `text`. ONLY LITERALS, for the reason ParseWhitelist gives: a path
    /// written in a comment is documentation, and a census that reads documentation as code is one that
    /// gets switched off.
    std::vector<std::string> QuotedLiterals( const std::string& text )
    {
        std::vector<std::string> out;
        size_t                   at = 0;
        while ( ( at = text.find( '"', at ) ) != std::string::npos )
        {
            const size_t end = text.find( '"', at + 1 );
            if ( end == std::string::npos )
                break;
            out.push_back( text.substr( at + 1, end - at - 1 ) );
            at = end + 1;
        }
        return out;
    }

    std::vector<std::filesystem::path> TestSources( const std::filesystem::path& root )
    {
        std::vector<std::filesystem::path> out;
        std::error_code                    ec;
        for ( auto it = std::filesystem::recursive_directory_iterator( root / "Desert" / "Tests", ec );
              it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( ec )
                break;
            const std::filesystem::path& p = it->path();
            if ( p.extension() == ".cpp" || p.extension() == ".hpp" )
                out.push_back( p );
        }
        return out;
    }
} // namespace

TEST( CookedFixtureWhitelistTest, NoTestNamesAFixtureThatTheIgnoreRuleWouldSwallow )
{
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not find the repository root from the working directory";

    const Whitelist rules = ParseWhitelist( ReadFile( root / ".gitignore" ) );
    ASSERT_FALSE( rules.Directories.empty() ) << ".gitignore has no `<dir>/*` rule at all; either the "
                                                 "file moved or this census is reading the wrong one";

    const std::vector<std::filesystem::path> sources = TestSources( root );
    ASSERT_FALSE( sources.empty() ) << "found no test sources under Desert/Tests";

    // ONE ROW PER OFFENDER, and the count is DERIVED from the rows rather than pinned as a number: a gate
    // that pins a count can be satisfied by editing the number, and this one has to be satisfied by
    // adding the `!` line.
    std::vector<std::string> swallowed;
    for ( const std::filesystem::path& source : sources )
    {
        for ( const std::string& literal : QuotedLiterals( ReadFile( source ) ) )
        {
            for ( const std::string& dir : rules.Directories )
            {
                if ( literal.size() <= dir.size() || literal.compare( 0, dir.size(), dir ) != 0 )
                    continue;
                if ( rules.Exceptions.contains( literal ) )
                    continue;
                swallowed.push_back( source.filename().string() + " names " + literal );
            }
        }
    }

    std::sort( swallowed.begin(), swallowed.end() );
    swallowed.erase( std::unique( swallowed.begin(), swallowed.end() ), swallowed.end() );

    for ( const std::string& row : swallowed )
        ADD_FAILURE() << row << " — that path is under a blanket ignore rule and has no `!` line, so "
                      << "`git add` will skip it without a word and the suite will be green only in the "
                      << "tree where the file happens to sit on disk. Add the `!` line to .gitignore.";
    EXPECT_EQ( swallowed.size(), 0U );
}

TEST( CookedFixtureWhitelistTest, EveryFileTheWhitelistLetsThroughStillExists )
{
    // THE OTHER DIRECTION, AND IT IS THE CHEAPER HALF OF THE SAME PROPERTY. A `!` line whose file has
    // been deleted is an instruction about nothing: it reads as coverage, it survives review, and it
    // leaves the next author believing a fixture is protected when the rule protects an absence.
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const Whitelist rules = ParseWhitelist( ReadFile( root / ".gitignore" ) );
    for ( const std::string& allowed : rules.Exceptions )
    {
        if ( allowed.empty() || allowed.back() == '/' )
            continue; // a directory re-admission, not a file
        EXPECT_TRUE( std::filesystem::exists( root / allowed ) )
             << ".gitignore lets " << allowed << " through and there is no such file";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
