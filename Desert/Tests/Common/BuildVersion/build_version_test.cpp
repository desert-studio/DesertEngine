// What this suite is for: the build number the engine reports must be the repository's, and it must go
// RED the moment the compiled-in header stops agreeing with git.
//
// The defect it exists to prevent, in the shape it actually had: `Version.gen.hpp` is generated and
// gitignored, and for a month its only generator ran from one wrapper script. The header on disk in the
// main tree said 492 commits with hash 768c3f1 while the repository stood at 1233 — and nothing
// anywhere compared the two, so the editor's status bar, the Runtime banner and (once the launcher
// registry lands) `engines.json` all reported a version from August. "Builds and tests pass" was true
// the whole time.
//
// This is a RELATION test in the sense the project means it: neither side is wrong on its own. The
// header is a valid header, git is a valid repository; what was broken was that nobody asserted they
// were talking about the same commit. Assert the agreement, not either side.

#include "../../TestSupport/scratch_dir.hpp"
#include <Common/Core/Version.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace
{
#ifdef _WIN32
#define DESERT_POPEN _popen
#define DESERT_PCLOSE _pclose
    constexpr const char* kNullDevice = "NUL";
#else
#define DESERT_POPEN popen
#define DESERT_PCLOSE pclose
    constexpr const char* kNullDevice = "/dev/null";
#endif

    // The repository root, found by walking up from wherever the binary was started — the same approach
    // TeardownOrder and SceneTonemapMigration use, so the suite does not have to be run from one exact
    // directory.
    const std::filesystem::path& RepoRoot()
    {
        static const std::filesystem::path root = []() -> std::filesystem::path
        {
            const std::filesystem::path here = Desert::TestSupport::RepositoryRoot();
            return here;
        }();
        return root;
    }

    std::string Trim( std::string text )
    {
        while ( !text.empty() && ( text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ) )
            text.pop_back();
        return text;
    }

    // Runs `git -C <repo root> <args>` and returns its first line, or nothing when git is absent, the
    // directory is not a repository, or the command failed. stderr is discarded on purpose: a missing
    // git is a legitimate answer here (§ the archive build), not a message for the test log.
    std::optional<std::string> Git( const std::string& args )
    {
        const std::string command =
             "git -C \"" + RepoRoot().string() + "\" " + args + " 2>" + std::string( kNullDevice );

        std::FILE* pipe = DESERT_POPEN( command.c_str(), "r" );
        if ( pipe == nullptr )
            return std::nullopt;

        std::array<char, 256> buffer{};
        std::string           out;
        if ( std::fgets( buffer.data(), static_cast<int>( buffer.size() ), pipe ) != nullptr )
            out = buffer.data();

        // Drain, so a chatty command cannot leave the child on a full pipe.
        while ( std::fgets( buffer.data(), static_cast<int>( buffer.size() ), pipe ) != nullptr )
        {
        }

        const int status = DESERT_PCLOSE( pipe );
        if ( status != 0 )
            return std::nullopt;

        out = Trim( std::move( out ) );
        if ( out.empty() )
            return std::nullopt;
        return out;
    }

    // True when git can answer questions about THIS tree at all.
    bool HasGit()
    {
        return Git( "rev-parse --verify HEAD" ).has_value();
    }

    // A shallow clone answers `rev-list --count HEAD` with 1, which is not a build number. The
    // generator refuses to write one in that case, so the test has to expect the same refusal — this is
    // the one piece of logic deliberately mirrored from scripts/GenVersion.sh, and it is mirrored
    // because it is exactly the relation under test: what git can honestly say vs what the header says.
    bool IsShallow()
    {
        const std::optional<std::string> answer = Git( "rev-parse --is-shallow-repository" );
        return !answer.has_value() || *answer == "true";
    }

    // What the build number OUGHT to be right now, or nothing when git cannot say.
    std::optional<std::uint32_t> GitCommitCount()
    {
        if ( !HasGit() || IsShallow() )
            return std::nullopt;

        const std::optional<std::string> answer = Git( "rev-list --count HEAD" );
        if ( !answer.has_value() )
            return std::nullopt;
        return static_cast<std::uint32_t>( std::stoul( *answer ) );
    }

    constexpr const char* kStaleHint =
         "\nThe compiled-in version header disagrees with git. Either the build did not regenerate "
         "Version.gen.hpp (the Common project generates it — re-run premake5 if these makefiles or "
         "project files predate that hook), or this binary was built before the commit you are now "
         "sitting on and simply needs rebuilding.";
} // namespace

// The suite cannot mean anything from outside a checkout, and a skip would look like a pass.
TEST( BuildVersion, TheSuiteIsRunFromInsideTheRepository )
{
    ASSERT_TRUE( std::filesystem::exists( RepoRoot() / "Desert" / "Common" / "Source" / "Common" / "Core" /
                                          "Version.cpp" ) )
         << "Could not find the repository root by walking up from " << Desert::TestSupport::RepositoryRoot()
         << ". Run this suite from inside the checkout (RunTests.sh does).";
}

// THE RELATION. This is the assertion the whole task exists to make.
TEST( BuildVersion, BuildNumberEqualsGitCommitCount )
{
    const std::optional<std::uint32_t> expected = GitCommitCount();
    const std::optional<std::uint32_t> actual   = Common::Version::CommitCount();

    if ( !expected.has_value() )
    {
        EXPECT_FALSE( actual.has_value() )
             << "git cannot supply a trustworthy commit count here (no repository, or a shallow clone), "
                "yet the header carries one: "
             << *actual << ". A number nobody can verify is worse than no number.";
        return;
    }

    ASSERT_TRUE( actual.has_value() ) << "git says this tree is at commit " << *expected
                                      << ", but the build reports no build number at all." << kStaleHint;
    EXPECT_EQ( *actual, *expected ) << "The build reports commit " << *actual << "; git says " << *expected << "."
                                    << kStaleHint;
}

// The identity beside the number, from the same generator run: a hash or branch that disagrees is the
// same staleness, caught one commit earlier than the count is when history moves sideways (a rebase, a
// branch switch) without changing its length.
TEST( BuildVersion, HashAndBranchEqualGitsAnswer )
{
    if ( !HasGit() )
    {
        EXPECT_STREQ( Common::Version::Hash(), "unknown" );
        EXPECT_STREQ( Common::Version::Branch(), "unknown" );
        return;
    }

    const std::optional<std::string> hash   = Git( "rev-parse --short HEAD" );
    const std::optional<std::string> branch = Git( "rev-parse --abbrev-ref HEAD" );
    ASSERT_TRUE( hash.has_value() );
    ASSERT_TRUE( branch.has_value() );

    EXPECT_EQ( std::string( Common::Version::Hash() ), *hash ) << kStaleHint;
    EXPECT_EQ( std::string( Common::Version::Branch() ), *branch ) << kStaleHint;
}

// The generated header is not optional any more, so the "unknown" identity must be unreachable in a
// checkout. Version.cpp used to fall back to it silently behind `__has_include`, and ccache's direct
// mode — which keys on the headers a TU actually included — then held that fallback in place even after
// the header appeared. This is the assertion that would have caught that state.
TEST( BuildVersion, TheUnknownFallbackIsUnreachableInACheckout )
{
    if ( !HasGit() )
        GTEST_SKIP() << "No git here; 'unknown' is the correct answer and is covered above.";

    EXPECT_STRNE( Common::Version::Hash(), "unknown" )
         << "The binary carries the no-git fallback identity while sitting in a git checkout." << kStaleHint;
    EXPECT_STRNE( Common::Version::Branch(), "unknown" )
         << "The binary carries the no-git fallback identity while sitting in a git checkout." << kStaleHint;
    EXPECT_EQ( std::string( Common::Version::Full() ).find( "unknown" ), std::string::npos )
         << "Full() shows the no-git fallback in a checkout: " << Common::Version::Full() << kStaleHint;
}

// The base version is authored in one file and must arrive unchanged — a middle link that drops a
// property is this project's most repeated defect shape, and there are three links between the VERSION
// file and the status bar.
TEST( BuildVersion, BaseEqualsTheVersionFile )
{
    std::ifstream in( RepoRoot() / "VERSION" );
    ASSERT_TRUE( in.good() ) << "VERSION is missing at " << ( RepoRoot() / "VERSION" );

    std::string base;
    std::getline( in, base );
    base = Trim( std::move( base ) );

    EXPECT_EQ( std::string( Common::Version::Base() ), base );
}

// Full() is the string every consumer actually shows, so assert it carries the parts rather than
// trusting that it does. Both branches matter: with a build number it must be there, and WITHOUT one
// there must be no zero standing in for it.
TEST( BuildVersion, FullCarriesTheParts )
{
    const std::string full = Common::Version::Full();

    EXPECT_EQ( full.rfind( Common::Version::Base(), 0 ), 0u ) << "Full() must start with Base(): " << full;
    EXPECT_NE( full.find( Common::Version::Hash() ), std::string::npos ) << "Full() must carry the hash: " << full;

    const std::optional<std::uint32_t> commits = Common::Version::CommitCount();
    if ( commits.has_value() )
    {
        EXPECT_NE( full.find( "." + std::to_string( *commits ) + "+" ), std::string::npos )
             << "Full() must carry the build number: " << full;
    }
    else
    {
        // The whole point of the optional: an unknown build number is ABSENT from the string, so it
        // cannot be read back as a zero and lose a threshold comparison it was never able to enter.
        EXPECT_EQ( full, std::string( Common::Version::Base() ) + "+" + Common::Version::Hash() +
                              ( Common::Version::Dirty() ? ".dirty" : "" ) )
             << "Without a build number Full() must have no build-number field at all: " << full;
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
