#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

#include "../../TestSupport/result_assert.hpp"

namespace fs = std::filesystem;

namespace
{
    // A clean directory AND a clean mount stack. The VFS stack is process-global, so a test that
    // mounted and then failed an ASSERT before its Unmount would otherwise hand the next test a pak
    // rooted in a directory that has just been deleted — a failure that names the wrong test.
    fs::path MakeTempDir()
    {
        Common::Utils::VFS::Unmount();
        const fs::path dir = fs::temp_directory_path() / "desert_pak_test";
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

#ifndef _WIN32
    // See Desert/Tests/Common/FileSystemWrite for the full reasoning. In one line: RLIMIT_FSIZE makes a
    // write past a byte cap fail with EFBIG, and a payload under one filebuf does not reach the kernel
    // until the flush — so this is the failure PakWriter used to answer green about three separate ways.
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

TEST( Pak, WriteReadRoundtrip )
{
    const fs::path dir = MakeTempDir();

    {
        Common::Utils::PakWriter writer( dir / "test.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        const std::string a = "hello pak";
        const std::string b( 100000, 'x' ); // bigger-than-one-block blob
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", a.data(), a.size() ) );
        ASSERT_TRUE( writer.AddData( "Cooked/Meshes/b.stmesh", b.data(), b.size() ) );
        EXPECT_EQ( writer.Finalize(), 2u );
    }

    Common::Utils::PakReader reader( dir / "test.dpak" );
    ASSERT_TRUE( reader.IsOpen() );
    EXPECT_EQ( reader.EntryCount(), 2u );
    EXPECT_TRUE( reader.Contains( "Assets/a.txt" ) );
    EXPECT_FALSE( reader.Contains( "Assets/missing.txt" ) );

    auto a = reader.Read( "Assets/a.txt" );
    ASSERT_TRUE( a.has_value() );
    EXPECT_EQ( *a, "hello pak" );

    auto b = reader.Read( "Cooked/Meshes/b.stmesh" );
    ASSERT_TRUE( b.has_value() );
    EXPECT_EQ( b->size(), 100000u );
    EXPECT_EQ( ( *b )[99999], 'x' );

    const auto cooked = reader.KeysWithPrefix( "Cooked/" );
    ASSERT_EQ( cooked.size(), 1u );
    EXPECT_EQ( cooked[0], "Cooked/Meshes/b.stmesh" );
}

TEST( Pak, VfsMountResolvesAbsolutePathsAndFileSystemFallsBack )
{
    const fs::path dir = MakeTempDir();

    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        const std::string scene = "{\"scene\":true}";
        ASSERT_TRUE( writer.AddData( "Assets/Scenes/Main.desce", scene.data(), scene.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    const auto mounted = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();

    // The file does NOT exist on disk — only in the pak. Absolute path under the mount root resolves.
    const fs::path virtualPath = dir / "Assets" / "Scenes" / "Main.desce";
    ASSERT_FALSE( fs::exists( virtualPath ) );

    EXPECT_TRUE( Common::Utils::VFS::Exists( virtualPath ) );
    EXPECT_TRUE( Common::Utils::FileSystem::Exists( virtualPath ) );                 // VFS-aware
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( virtualPath ), // read via pak
                             "{\"scene\":true}" );
    EXPECT_EQ( Common::Utils::FileSystem::GetFileSize( virtualPath ), 14u );

    // Paths outside the mount root stay unresolved.
    EXPECT_FALSE( Common::Utils::VFS::Exists( "/definitely/not/mounted.txt" ) );

    // Listing reconstructs FULL paths under the mount root — in the VFS's ONE canonical spelling
    // (symlinked prefixes resolved: on macOS this temp dir is /var/... as spelled and /private/var/...
    // resolved). The contract is not a spelling but the round trip: a listed path must name the same
    // file as the spelling the caller asked with, and must READ back through the VFS.
    const auto listed = Common::Utils::VFS::ListFiles( dir / "Assets" );
    ASSERT_EQ( listed.size(), 1u );
    std::error_code cec;
    EXPECT_EQ( fs::weakly_canonical( listed[0], cec ), fs::weakly_canonical( virtualPath, cec ) );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( listed[0] ), "{\"scene\":true}" );

    // LOOSE FILE OVERRIDE: a real file with the same path wins over the pak entry.
    fs::create_directories( virtualPath.parent_path() );
    {
        std::ofstream out( virtualPath );
        out << "loose";
    }
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( virtualPath ), "loose" );

    Common::Utils::VFS::Unmount();
    EXPECT_FALSE( Common::Utils::VFS::Exists( virtualPath ) );
}

TEST( Pak, EntryHashesMatchContent )
{
    const fs::path dir = MakeTempDir();

    const std::string payload = "hash me";
    {
        Common::Utils::PakWriter writer( dir / "hash.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "a.bin", payload.data(), payload.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    Common::Utils::PakReader reader( dir / "hash.dpak" );
    ASSERT_TRUE( reader.IsOpen() );
    const auto h = reader.EntryHash( "a.bin" );
    ASSERT_TRUE( h.has_value() );
    EXPECT_EQ( *h, Common::Utils::PakContentHash( payload.data(), payload.size() ) );
    EXPECT_NE( *h, 0u );
    EXPECT_FALSE( reader.EntryHash( "missing" ).has_value() );
}

TEST( Pak, PatchMountOverridesBase )
{
    const fs::path dir = MakeTempDir();

    const std::string baseData  = "base";
    const std::string patchData = "patched";
    const std::string extraData = "only-in-base";
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", baseData.data(), baseData.size() ) );
        ASSERT_TRUE( writer.AddData( "Assets/b.txt", extraData.data(), extraData.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }
    {
        Common::Utils::PakWriter writer( dir / "Patch_001.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", patchData.data(), patchData.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    const auto mountedBase = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( mountedBase.IsSuccess() ) << mountedBase.GetError();
    const auto mountedPatch = Common::Utils::VFS::MountPak( dir / "Patch_001.dpak" ); // later mount wins
    ASSERT_TRUE( mountedPatch.IsSuccess() ) << mountedPatch.GetError();

    // The patched key reads from the LATER mount; untouched keys still come from the base.
    EXPECT_EQ( Common::Utils::VFS::ReadFile( dir / "Assets/a.txt" ).value_or( "" ), "patched" );
    EXPECT_EQ( Common::Utils::VFS::ReadFile( dir / "Assets/b.txt" ).value_or( "" ), "only-in-base" );

    // Listing dedupes overridden keys (a.txt appears once).
    const auto listed = Common::Utils::VFS::ListFiles( dir / "Assets" );
    EXPECT_EQ( listed.size(), 2u );

    Common::Utils::VFS::Unmount();
    EXPECT_FALSE( Common::Utils::VFS::IsMounted() );
}

// ---------------------------------------------------------------- deletions in a patch
//
// THE RELATION UNDER TEST, HALF ONE: a file the source DELETED disappears from what the game sees.
// Both halves matter and they fail in opposite directions — the other half ("a file a person edited is
// not overwritten") lives in the ContentUpdate suite, because it is the other consumer of the same
// mechanism.
//
// Before this, an overlay patch could only add or override, so the only way to drop a file from a
// shipped game was to re-ship the whole base. The archives here are real .dpak files written by the
// real writer and mounted by the real VFS.

TEST( Pak, DeletedEntryDisappearsFromTheMountedContent )
{
    const fs::path dir = MakeTempDir();

    const std::string keep   = "still here";
    const std::string doomed = "the update removes this";
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/keep.txt", keep.data(), keep.size() ) );
        ASSERT_TRUE( writer.AddData( "Assets/gone.txt", doomed.data(), doomed.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }
    {
        // A patch that ONLY deletes: no content entries at all. Finalize must still report a written
        // archive, or a pure-removal update would read as an empty one and be discarded.
        Common::Utils::PakWriter writer( dir / "Patch_001.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.SetDeletedKeys( { "Assets/gone.txt" } ) );
        EXPECT_EQ( writer.Finalize(), 1u );
    }

    const auto base = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( base.IsSuccess() ) << base.GetError();
    // Both files are there before the patch — otherwise the assertion after it proves nothing.
    ASSERT_TRUE( Common::Utils::VFS::Exists( dir / "Assets/gone.txt" ) );

    const auto patch = Common::Utils::VFS::MountPak( dir / "Patch_001.dpak" );
    ASSERT_TRUE( patch.IsSuccess() ) << patch.GetError();

    // THE ASSERTION. Every way of asking must agree — a file that Exists() but cannot be read, or that
    // is gone from reads but still appears in a listing, is the defect this rule was added to prevent.
    EXPECT_FALSE( Common::Utils::VFS::Exists( dir / "Assets/gone.txt" ) );
    EXPECT_FALSE( Common::Utils::VFS::ReadFile( dir / "Assets/gone.txt" ).has_value() );
    EXPECT_FALSE( Common::Utils::VFS::FileSize( dir / "Assets/gone.txt" ).has_value() );
    EXPECT_FALSE( Common::Utils::FileSystem::Exists( dir / "Assets/gone.txt" ) );

    const auto listed = Common::Utils::VFS::ListFiles( dir / "Assets" );
    EXPECT_EQ( listed.size(), 1u );

    // The neighbour is untouched: a deletion removes one key, not the mount.
    EXPECT_EQ( Common::Utils::VFS::ReadFile( dir / "Assets/keep.txt" ).value_or( "" ), keep );

    Common::Utils::VFS::Unmount();
}

TEST( Pak, DeletionListIsNotContent )
{
    const fs::path dir = MakeTempDir();

    const std::string payload = "real file";
    {
        Common::Utils::PakWriter writer( dir / "p.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "a.txt", payload.data(), payload.size() ) );
        ASSERT_TRUE( writer.SetDeletedKeys( { "old.txt" } ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    Common::Utils::PakReader reader( dir / "p.dpak" );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();

    // The bookkeeping entry occupies an index record but must be invisible to every content accessor:
    // an extractor, a walker or a manifest that picked it up would treat it as a shipped file.
    const std::string reserved( Common::Utils::kDeletedEntriesKey );
    EXPECT_EQ( reader.EntryCount(), 1u );
    EXPECT_FALSE( reader.Contains( reserved ) );
    EXPECT_FALSE( reader.Read( reserved ).has_value() );
    EXPECT_FALSE( reader.EntrySize( reserved ).has_value() );
    EXPECT_FALSE( reader.EntryHash( reserved ).has_value() );
    const auto keys = reader.KeysWithPrefix( "" );
    ASSERT_EQ( keys.size(), 1u );
    EXPECT_EQ( keys[0], "a.txt" );

    ASSERT_EQ( reader.DeletedKeys().size(), 1u );
    EXPECT_EQ( reader.DeletedKeys()[0], "old.txt" );
    EXPECT_TRUE( reader.IsDeleted( "old.txt" ) );

    // The writer refuses to make the reserved key mean two things.
    Common::Utils::PakWriter other( dir / "q.dpak" );
    ASSERT_TRUE( other.IsOpen() );
    EXPECT_FALSE( other.AddData( reserved, payload.data(), payload.size() ) );
    EXPECT_FALSE( other.SetDeletedKeys( { reserved } ) );
    EXPECT_FALSE( other.SetDeletedKeys( { "" } ) );
    EXPECT_FALSE( other.SetDeletedKeys( { "two\nkeys" } ) ); // one record, not two that both parse
}

TEST( Pak, AnArchiveCannotBothShipAndDeleteAKey )
{
    const fs::path dir = MakeTempDir();

    const std::string        payload = "x";
    Common::Utils::PakWriter writer( dir / "contradiction.dpak" );
    ASSERT_TRUE( writer.IsOpen() );
    ASSERT_TRUE( writer.AddData( "a.txt", payload.data(), payload.size() ) );
    ASSERT_TRUE( writer.SetDeletedKeys( { "a.txt" } ) ); // legal in isolation, caught at Finalize
    EXPECT_EQ( writer.Finalize(), 0u );
}

TEST( Pak, APatchThatDeletesWhatIsNotThereIsRefused )
{
    const fs::path dir = MakeTempDir();

    const std::string payload = "base content";
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", payload.data(), payload.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }
    {
        Common::Utils::PakWriter writer( dir / "Patch_001.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.SetDeletedKeys( { "Assets/never_shipped.txt" } ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    const auto base = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( base.IsSuccess() ) << base.GetError();

    // A deletion with nothing to delete means this patch was built against a different base — so the
    // file it was published to remove is some other file, or none. Refused, with the count and an
    // example, because the alternative is a game that starts and looks updated and is not.
    const auto patch = Common::Utils::VFS::MountPak( dir / "Patch_001.dpak" );
    EXPECT_FALSE( patch.IsSuccess() );
    EXPECT_NE( patch.GetError().find( "never_shipped.txt" ), std::string::npos ) << patch.GetError();
    EXPECT_NE( patch.GetError().find( "different base" ), std::string::npos ) << patch.GetError();

    // And it did not half-mount: the base is still the only thing on the stack.
    EXPECT_EQ( Common::Utils::VFS::ReadFile( dir / "Assets/a.txt" ).value_or( "" ), payload );

    Common::Utils::VFS::Unmount();
}

TEST( Pak, ALaterPatchCanBringADeletedFileBack )
{
    const fs::path dir = MakeTempDir();

    const std::string first  = "v1";
    const std::string second = "v3";
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", first.data(), first.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }
    {
        Common::Utils::PakWriter writer( dir / "Patch_001.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.SetDeletedKeys( { "Assets/a.txt" } ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }
    {
        Common::Utils::PakWriter writer( dir / "Patch_002.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", second.data(), second.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    ASSERT_TRUE( Common::Utils::VFS::MountPak( dir / "Content.dpak" ).IsSuccess() );
    ASSERT_TRUE( Common::Utils::VFS::MountPak( dir / "Patch_001.dpak" ).IsSuccess() );
    // Dropped in 1.1 and restored in 1.2 is an ordinary release history, so it must mount and the
    // newest mount must win. Refusing it — which the analysis proposed — would refuse a legal update.
    ASSERT_TRUE( Common::Utils::VFS::MountPak( dir / "Patch_002.dpak" ).IsSuccess() );

    EXPECT_TRUE( Common::Utils::VFS::Exists( dir / "Assets/a.txt" ) );
    EXPECT_EQ( Common::Utils::VFS::ReadFile( dir / "Assets/a.txt" ).value_or( "" ), second );
    EXPECT_EQ( Common::Utils::VFS::ListFiles( dir / "Assets" ).size(), 1u );

    Common::Utils::VFS::Unmount();
}

TEST( Pak, ADamagedDeletionListIsAnOpenFailureWithAReason )
{
    const fs::path dir = MakeTempDir();

    const std::string payload = "content";
    {
        Common::Utils::PakWriter writer( dir / "Patch_001.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", payload.data(), payload.size() ) );
        ASSERT_TRUE( writer.SetDeletedKeys( { "Assets/b.txt" } ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    // Flip one byte of the deletion list's blob. The header, the index and every span stay perfectly
    // valid — only the content hash disagrees, which is exactly the shape a truncated or tampered
    // download leaves behind.
    const auto beforeRead = Common::Utils::FileSystem::ReadFileContent( dir / "Patch_001.dpak" );
    ASSERT_TRUE( beforeRead.IsSuccess() ) << "the patch pak did not read back: " << beforeRead.GetError();
    const std::string before = beforeRead.GetValue();
    const size_t      at     = before.find( "Assets/b.txt" );
    ASSERT_NE( at, std::string::npos );
    std::string after = before;
    after[at + 7]     = 'X'; // "Assets/b.txt" -> "Assets/X.txt" inside the LIST blob
    {
        std::ofstream out( dir / "Patch_001.dpak", std::ios::binary | std::ios::trunc );
        out.write( after.data(), static_cast<std::streamsize>( after.size() ) );
    }

    Common::Utils::PakReader reader( dir / "Patch_001.dpak" );
    EXPECT_FALSE( reader.IsOpen() );
    EXPECT_NE( reader.OpenError().find( "deletion list" ), std::string::npos ) << reader.OpenError();

    const auto mounted = Common::Utils::VFS::MountPak( dir / "Patch_001.dpak" );
    EXPECT_FALSE( mounted.IsSuccess() );
    Common::Utils::VFS::Unmount();
}

// ---------------------------------------------------------------------------------------------------
// THE WRITER'S ONE VERDICT (Д35)
// ---------------------------------------------------------------------------------------------------
//
// PakWriter used to lie three ways about one archive: the constructor set m_Ok from a stream whose
// header had not been flushed, AddData returned true AND recorded the entry for bytes nobody had
// confirmed, and Finalize read `out ? size : 0` on a stream it never closed. All three were the same
// missing sentence — THE INDEX CANNOT NAME WHAT IS NOT ON THE DISK — and they are now one stream held
// for the archive's life, closed in Finalize, with the count read after the close.

TEST( Pak, FinalizeRefusesAnArchiveWhoseBytesNeverReachedTheDisk )
{
#ifdef _WIN32
    GTEST_SKIP() << "RLIMIT_FSIZE is POSIX; the flush-failure injection has no Windows equivalent here";
#else
    const fs::path dir = MakeTempDir();
    const fs::path pak = dir / "capped.dpak";

    size_t finalized = 1;
    bool   opened    = false;
    bool   added     = false;
    {
        // 64 bytes: the 16-byte header fits, everything after it does not. Every byte stays in the one
        // filebuf, so nothing fails until the close inside Finalize — which is precisely the window in
        // which this class used to return three green answers.
        FileSizeCap cap( 64 );
        ASSERT_TRUE( cap.Applied() ) << "RLIMIT_FSIZE could not be lowered; this test cannot mean anything";

        Common::Utils::PakWriter writer( pak );
        opened = writer.IsOpen();

        const std::string blob( 2048, 'z' );
        added     = writer.AddData( "Assets/a.bin", blob.data(), blob.size() );
        finalized = writer.Finalize();
    }

    // IsOpen and AddData are ALLOWED to have said yes — they are about the stream, not the disk, and the
    // header comment says so. What must not happen is the archive coming back finished.
    EXPECT_TRUE( opened ) << "the file itself was creatable, so the open genuinely succeeded";
    EXPECT_TRUE( added ) << "under one filebuf the insertion cannot fail; if it did, this test is no "
                            "longer exercising the buffered case";
    EXPECT_EQ( finalized, 0u ) << "Finalize reported a finished archive whose bytes the filesystem refused";

    // And the verdict agrees with the disk: nothing can read the thing back.
    Common::Utils::PakReader reader( pak );
    EXPECT_FALSE( reader.IsOpen() ) << "an archive Finalize refused must not open";
#endif
}

TEST( Pak, AFinalizedWriterDoesNotAnswerASecondTime )
{
    // Finalize closes the stream, so there is nothing left to confirm. Answering from m_Entries again
    // would be a count about a file the writer no longer holds — the same "success from memory" shape.
    const fs::path dir = MakeTempDir();

    Common::Utils::PakWriter writer( dir / "twice.dpak" );
    ASSERT_TRUE( writer.IsOpen() );
    const std::string a = "hello";
    ASSERT_TRUE( writer.AddData( "Assets/a.txt", a.data(), a.size() ) );

    EXPECT_EQ( writer.Finalize(), 1u );
    EXPECT_EQ( writer.Finalize(), 0u ) << "the second Finalize re-answered from memory";
    EXPECT_FALSE( writer.AddData( "Assets/b.txt", a.data(), a.size() ) )
         << "a finished archive accepted another entry";

    // The first answer was true, and stays true: the archive on disk is the one entry it reported.
    Common::Utils::PakReader reader( dir / "twice.dpak" );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    EXPECT_EQ( reader.EntryCount(), 1u );
}

TEST( Pak, AnUnwritablePathIsRefusedAtOpenRatherThanAtFinalize )
{
    // The negative control for the two above: when the failure IS visible at the open, IsOpen is the one
    // that says so — so a false from IsOpen still means what every existing caller reads it as.
    const fs::path dir     = MakeTempDir();
    const fs::path blocker = dir / "not_a_directory";
    {
        std::ofstream make( blocker, std::ios::trunc );
        make << "occupied";
    }

    Common::Utils::PakWriter writer( blocker / "inside.dpak" );
    EXPECT_FALSE( writer.IsOpen() );
    const std::string a = "hello";
    EXPECT_FALSE( writer.AddData( "Assets/a.txt", a.data(), a.size() ) );
    EXPECT_EQ( writer.Finalize(), 0u );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
