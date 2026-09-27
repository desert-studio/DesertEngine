#include <Common/Utilities/WriteWatch.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

// FIX2's class, reproduced on one machine: a second same-size write inside one tick of the file system's clock
// leaves (write time, size) exactly as they were. On NTFS that is ~15.6 ms and it happens; on APFS the clock is
// nanoseconds and it does not, so each test puts the previous write time back after the rewrite — the file is
// then indistinguishable by stat from the one observed before, which is what a Windows runner sees.
namespace
{
    namespace fs = std::filesystem;
    using Seen   = Common::Utils::WriteWatch::Seen;

    struct Scratch
    {
        fs::path Dir;
        explicit Scratch( const char* name ) : Dir( fs::temp_directory_path() / name )
        {
            fs::remove_all( Dir );
            fs::create_directories( Dir );
        }
        ~Scratch()
        {
            std::error_code ec;
            fs::remove_all( Dir, ec );
        }
    };

    void Write( const fs::path& file, const std::string& text )
    {
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        out << text;
    }

    // Rewrites @p file with @p text of the same length and restores the write time it had: one tick, two writes.
    void RewriteInsideOneTick( const fs::path& file, const std::string& text )
    {
        const auto before = fs::last_write_time( file );
        Write( file, text );
        fs::last_write_time( file, before );
    }
} // namespace

TEST( WriteWatch, FirstSightingIsABaseline )
{
    const Scratch  s( "desert_write_watch_first" );
    const fs::path file = s.Dir / "a.lua";
    Write( file, "print('a')" );

    Common::Utils::WriteWatch watch;
    EXPECT_EQ( watch.Observe( "a", file ), Seen::First );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Unchanged );
}

TEST( WriteWatch, AMissingFileKeepsTheLastObservation )
{
    const Scratch  s( "desert_write_watch_missing" );
    const fs::path file = s.Dir / "a.lua";

    Common::Utils::WriteWatch watch;
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Missing );
    Write( file, "print('a')" );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::First );
}

TEST( WriteWatch, AMovedStampIsAChange )
{
    const Scratch  s( "desert_write_watch_moved" );
    const fs::path file = s.Dir / "a.lua";
    Write( file, "print('a')" );

    Common::Utils::WriteWatch watch;
    ASSERT_EQ( watch.Observe( "a", file ), Seen::First );
    Write( file, "print('a')" );
    fs::last_write_time( file, fs::last_write_time( file ) + std::chrono::seconds( 1 ) );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Changed );
}

// THE DEFECT. The first observation happens right after the write, so its stamp is racy and the watch hashed
// the content; the same-size rewrite inside the same tick must be reported although stat cannot tell.
TEST( WriteWatch, ASameSizeRewriteInsideOneTickIsAChange )
{
    const Scratch  s( "desert_write_watch_racy" );
    const fs::path file = s.Dir / "a.lua";
    Write( file, "print('a')" );

    Common::Utils::WriteWatch watch;
    ASSERT_EQ( watch.Observe( "a", file ), Seen::First );
    RewriteInsideOneTick( file, "print('b')" );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Changed );

    // And the same once the stamp has already been seen to move: the edit that arrives in the tick right
    // after a reload is the case a hot reloader meets when an editor saves twice.
    RewriteInsideOneTick( file, "print('c')" );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Changed );
}

// The price of the rule is a content hash while the stamp is fresh, not a spurious reload: equal content under
// an equal racy stamp is unchanged.
TEST( WriteWatch, ARacyStampWithEqualContentIsUnchanged )
{
    const Scratch  s( "desert_write_watch_equal" );
    const fs::path file = s.Dir / "a.lua";
    Write( file, "print('a')" );

    Common::Utils::WriteWatch watch;
    ASSERT_EQ( watch.Observe( "a", file ), Seen::First );
    RewriteInsideOneTick( file, "print('a')" );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Unchanged );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Unchanged );
}

// A stamp observed after the window has settled: nothing can be written under it any more without moving it,
// so stat is trusted and the content is not read. Proven by rewriting under the settled stamp — a thing no real
// writer can do — and seeing the watch believe stat.
TEST( WriteWatch, ASettledStampIsTrustedWithoutReadingTheFile )
{
    const Scratch  s( "desert_write_watch_settled" );
    const fs::path file = s.Dir / "a.lua";
    Write( file, "print('a')" );
    fs::last_write_time( file, fs::file_time_type::clock::now() - std::chrono::minutes( 1 ) );

    Common::Utils::WriteWatch watch;
    ASSERT_EQ( watch.Observe( "a", file ), Seen::First );
    RewriteInsideOneTick( file, "print('b')" );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::Unchanged );
}

TEST( WriteWatch, ForgetMakesTheNextSightingABaseline )
{
    const Scratch  s( "desert_write_watch_forget" );
    const fs::path file = s.Dir / "a.lua";
    Write( file, "print('a')" );

    Common::Utils::WriteWatch watch;
    ASSERT_EQ( watch.Observe( "a", file ), Seen::First );
    watch.Forget( "a" );
    EXPECT_EQ( watch.Observe( "a", file ), Seen::First );
    watch.Clear();
    EXPECT_EQ( watch.Observe( "a", file ), Seen::First );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
