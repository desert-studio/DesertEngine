// The write primitive's contract: WriteContentToFileAtomic either lands the WHOLE content or leaves
// the destination BYTE-IDENTICAL, and says which happened — and, since Д31-A, WHY — in its return
// value. It is now the ONLY write primitive in the tree: the plain WriteContentToFile that used to sit
// beside it returned `const void`, opened the destination with trunc so the old contents were gone
// before the first new byte landed, and checked neither the insertion nor the close. Tools/SceneMigrator
// destroyed scenes exactly that way, and the editor's whole Ctrl+S chain ran through it, clearing the
// "unsaved changes" mark for writes that had not happened.
//
// The discriminating tests below are the ones that FAIL against an in-place implementation: they
// build situations where writing the destination directly would succeed (and destroy it) while the
// temp-then-rename path is refused — so reverting the primitive to trunc-in-place turns them red,
// which is the mutation check the fix shipped with.

#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;

using Common::Utils::FileSystem;

namespace
{
    fs::path MakeTempDir( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / name;
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

    void WriteRaw( const fs::path& p, const std::string& content )
    {
        std::ofstream out( p, std::ios::binary );
        out << content;
    }

    std::string ReadRaw( const fs::path& p )
    {
        std::ifstream      in( p, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::span<const std::byte> Bytes( const std::string& s )
    {
        return std::as_bytes( std::span( s.data(), s.size() ) );
    }

#ifndef _WIN32
    // A WRITE THAT REALLY FAILS, AND FAILS AT THE FLUSH — which is the only kind this whole census is
    // about. RLIMIT_FSIZE caps how large a file this process may create; a write past the cap returns
    // EFBIG. A payload SMALLER than one filebuf (libc++ uses 4096 bytes) never reaches the kernel during
    // `write()`/`<<` at all, so the stream still looks good afterwards and the EFBIG arrives at the
    // flush. That is exactly the condition under which the checked-but-unflushed idiom reports green,
    // and it is why the negative control below is part of the evidence rather than decoration.
    //
    // SIGXFSZ is ignored for the duration: POSIX raises it alongside EFBIG and its default action would
    // kill the test binary instead of failing the write. Both the limit and the handler are restored.
    class FileSizeCap
    {
    public:
        explicit FileSizeCap( rlim_t bytes )
        {
            m_PreviousHandler = std::signal( SIGXFSZ, SIG_IGN );
            getrlimit( RLIMIT_FSIZE, &m_Previous );
            rlimit capped   = m_Previous;
            capped.rlim_cur = bytes;
            m_Applied       = setrlimit( RLIMIT_FSIZE, &capped ) == 0;
        }

        ~FileSizeCap()
        {
            setrlimit( RLIMIT_FSIZE, &m_Previous );
            std::signal( SIGXFSZ, m_PreviousHandler );
        }

        bool Applied() const
        {
            return m_Applied;
        }

    private:
        rlimit m_Previous{};
        void ( *m_PreviousHandler )( int ) = SIG_DFL;
        bool m_Applied                     = false;
    };
#endif
} // namespace

// The happy path: the content lands whole, and the working file the primitive wrote through is gone.
// A stray .tmp beside every saved file would read as litter, and worse — a crash-then-restart could
// mistake a stale one for in-flight work.
TEST( FileSystemWrite, ASuccessfulWriteLandsWholeAndLeavesNoTemporaryBehind )
{
    const fs::path dir  = MakeTempDir( "desert_fs_write_success" );
    const fs::path file = dir / "out.json";

    EXPECT_TRUE( FileSystem::WriteContentToFileAtomic( file, "{\"a\":1}" ).IsSuccess() );

    EXPECT_EQ( ReadRaw( file ), "{\"a\":1}" );
    fs::path temp = file;
    temp += ".tmp";
    EXPECT_FALSE( fs::exists( temp ) ) << "the working file survived the rename";

    fs::remove_all( dir );
}

// Replacing an existing file is the primitive's whole reason to exist — every caller (the scene
// migrator, both registry writers) overwrites. The new content must REPLACE, not append to or merge
// with, the old.
TEST( FileSystemWrite, AnExistingFileIsReplacedWithTheNewContentExactly )
{
    const fs::path dir  = MakeTempDir( "desert_fs_write_replace" );
    const fs::path file = dir / "out.json";
    WriteRaw( file, "the old contents, deliberately longer than the new ones" );

    EXPECT_TRUE( FileSystem::WriteContentToFileAtomic( file, "short" ).IsSuccess() );
    EXPECT_EQ( ReadRaw( file ), "short" );

    fs::remove_all( dir );
}

// DISCRIMINATING TEST 1 (all platforms): the temp path is blocked by a directory, so the primitive
// cannot even begin — and the destination itself is perfectly writable, so an in-place trunc
// implementation would sail through and replace it. Green only when the failure is refused BEFORE
// the original is touched.
TEST( FileSystemWrite, ABlockedTemporaryCostsTheWriteAndNotTheOriginal )
{
    const fs::path dir  = MakeTempDir( "desert_fs_write_blocked" );
    const fs::path file = dir / "out.json";
    WriteRaw( file, "{\"precious\":true}" );

    fs::path temp = file;
    temp += ".tmp";
    fs::create_directories( temp ); // a directory where the primitive needs its working file

    const auto written = FileSystem::WriteContentToFileAtomic( file, "{\"replacement\":true}" );
    EXPECT_FALSE( written.IsSuccess() );
    // The reason travels with the refusal, not only into the log: the editor puts it in front of the
    // user, who does not have a log open.
    EXPECT_NE( written.GetError().find( temp.string() ), std::string::npos ) << written.GetError();
    EXPECT_EQ( ReadRaw( file ), "{\"precious\":true}" ) << "a failed write cost the original its contents";

    fs::remove_all( dir );
}

#ifndef _WIN32
// DISCRIMINATING TEST 2 (POSIX): the directory is read-only but the file inside it stays writable —
// the exact permissions shape under which the in-place implementation SUCCEEDS (opening an existing
// file for write needs no directory write permission) while creating the temp beside it is refused.
// Windows is excluded because its read-only directory attribute does not deny file creation, so the
// situation cannot be built there with std::filesystem; the blocked-temp test above covers Windows.
TEST( FileSystemWrite, AReadOnlyDirectoryCostsTheWriteAndNotTheOriginal )
{
    const fs::path dir  = MakeTempDir( "desert_fs_write_readonly" );
    const fs::path file = dir / "out.json";
    WriteRaw( file, "{\"precious\":true}" );

    fs::permissions( dir, fs::perms::owner_read | fs::perms::owner_exec );

    EXPECT_FALSE( FileSystem::WriteContentToFileAtomic( file, "{\"replacement\":true}" ).IsSuccess() );
    EXPECT_EQ( ReadRaw( file ), "{\"precious\":true}" ) << "a failed write cost the original its contents";

    // Restore before cleanup, or remove_all leaves the read-only directory behind for the next run.
    fs::permissions( dir, fs::perms::owner_all );
    fs::remove_all( dir );
}
#endif

// ---------------------------------------------------------------------------------------------------
// THE BYTE SPELLING (Д35)
// ---------------------------------------------------------------------------------------------------
//
// WriteBytesToFileAtomic is the primitive's body; WriteContentToFileAtomic is the same call with the
// bytes taken from a string. These assert that the two really are one thing, because the moment they are
// two implementations one of them starts drifting — which is the whole reason the fifteen Д31-D sites
// existed in the first place.

TEST( FileSystemWrite, BytesAndTextAreTheSameWrite )
{
    const fs::path dir = MakeTempDir( "desert_fs_write_bytes" );

    // A payload with an embedded NUL: a byte span keeps it, and this is what the four cloud containers,
    // both bakers and PakTool actually hold. The string overload must carry it too — std::string is not
    // NUL-terminated-by-contract and never was.
    const std::string payload( "head\0\xff\x01tail", 12 );

    ASSERT_TRUE( FileSystem::WriteBytesToFileAtomic( dir / "a.bin", Bytes( payload ) ).IsSuccess() );
    ASSERT_TRUE( FileSystem::WriteContentToFileAtomic( dir / "b.bin", payload ).IsSuccess() );

    EXPECT_EQ( ReadRaw( dir / "a.bin" ), payload );
    EXPECT_EQ( ReadRaw( dir / "a.bin" ), ReadRaw( dir / "b.bin" ) );

    fs::remove_all( dir );
}

TEST( FileSystemWrite, AnEmptySpanWritesAnEmptyFileAndSucceeds )
{
    // A zero-byte result is a SUCCESS holding nothing, not a failure — the same distinction the read
    // primitive makes. An empty .dclayout mask is a legal thing to write.
    const fs::path dir  = MakeTempDir( "desert_fs_write_empty" );
    const fs::path file = dir / "empty.bin";

    EXPECT_TRUE( FileSystem::WriteBytesToFileAtomic( file, {} ).IsSuccess() );
    EXPECT_TRUE( fs::exists( file ) );
    EXPECT_EQ( ReadRaw( file ), "" );

    fs::remove_all( dir );
}

TEST( FileSystemWrite, ABlockedTemporaryCostsTheBYTEWriteAndNotTheOriginal )
{
    // The same discriminating shape as the text test above, against the byte spelling — this is the
    // injection every one of the migrated Д31-D sites is driven with in its own suite, so it has to be
    // proven here to mean what those suites read it as.
    const fs::path dir  = MakeTempDir( "desert_fs_write_bytes_blocked" );
    const fs::path file = dir / "out.bin";
    WriteRaw( file, "{\"precious\":true}" );

    fs::path temp = file;
    temp += ".tmp";
    fs::create_directories( temp );

    const std::string payload( "replacement", 11 );
    const auto        written = FileSystem::WriteBytesToFileAtomic( file, Bytes( payload ) );
    EXPECT_FALSE( written.IsSuccess() );
    EXPECT_NE( written.GetError().find( temp.string() ), std::string::npos ) << written.GetError();
    EXPECT_EQ( ReadRaw( file ), "{\"precious\":true}" ) << "a failed write cost the original its contents";

    fs::remove_all( dir );
}

#ifndef _WIN32
// ---------------------------------------------------------------------------------------------------
// THE DEFECT ITSELF, MADE TO HAPPEN (Д35)
// ---------------------------------------------------------------------------------------------------
//
// Everything above drives a failure the OPEN can see. This drives the one that only the FLUSH can see —
// the failure the fifteen Д31-D sites returned green for — and it is the reason the census exists.
TEST( FileSystemWrite, AFailureVisibleOnlyAtTheFlushIsStillARefusal )
{
    const fs::path dir  = MakeTempDir( "desert_fs_write_flush" );
    const fs::path file = dir / "out.bin";

    // Under one filebuf (4096 on libc++), so nothing reaches the kernel before the close.
    const std::string payload( 1024, 'x' );

    FileSizeCap cap( 64 );
    ASSERT_TRUE( cap.Applied() ) << "RLIMIT_FSIZE could not be lowered; this test cannot mean anything";

    const auto written = FileSystem::WriteContentToFileAtomic( file, payload );
    EXPECT_FALSE( written.IsSuccess() )
         << "the write reported success for 1024 bytes the filesystem refused at the flush";
    EXPECT_NE( written.GetError().find( "1024" ), std::string::npos )
         << "the refusal must carry the size it failed on: " << written.GetError();
}

// THE NEGATIVE CONTROL, and it is not optional. The test above only proves the primitive refuses; it
// does not prove there was anything to refuse. This one runs the idiom the census bans under the SAME
// cap and shows it answering green — so the cap is a real failure, the refusal above is a real refusal,
// and "we now check" is a measurement rather than a belief.
TEST( FileSystemWrite, TheBannedIdiomReportsSuccessUnderTheVerySameFailure )
{
    const fs::path dir  = MakeTempDir( "desert_fs_write_flush_control" );
    const fs::path file = dir / "out.bin";

    const std::string payload( 1024, 'x' );

    bool idiomSaidItWroteIt = false;
    {
        FileSizeCap cap( 64 );
        ASSERT_TRUE( cap.Applied() );

        {
            // Verbatim the four-line shape from this file's header comment: open, check, write, check.
            std::ofstream out( file, std::ios::binary | std::ios::trunc );
            ASSERT_TRUE( static_cast<bool>( out ) ) << "the open must SUCCEED or the control proves nothing";
            out.write( payload.data(), static_cast<std::streamsize>( payload.size() ) );
            idiomSaidItWroteIt = static_cast<bool>( out ); // <-- the verdict the banned sites returned
        } // <-- ~ofstream flushes here, and swallows it
    }

    EXPECT_TRUE( idiomSaidItWroteIt )
         << "the banned idiom did NOT report success here, so this cap does not reproduce the defect and "
            "the refusal test above is not evidence of anything";
    EXPECT_NE( ReadRaw( file ), payload )
         << "the bytes actually landed, so nothing failed and neither test above means what it says";
}
#endif

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
