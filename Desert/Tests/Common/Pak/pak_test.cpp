#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <Common/Utilities/Crc32c.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

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

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// v3: integrity, compression, migration, appending and mount provenance.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
    // The repository root, walked up from wherever the binary was started — the same approach the
    // migration suites use, so this test does not have to be run from one exact directory.
    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            std::error_code ec;
            if ( fs::exists( prefix / "Desert/Common/Source/Common/Utilities/PakFile.hpp", ec ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    std::string Slurp( const fs::path& p )
    {
        std::ifstream in( p, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }

    void Spit( const fs::path& p, const std::string& bytes )
    {
        std::ofstream out( p, std::ios::binary );
        out.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
    }

    template <typename T>
    void PushPod( std::string& out, T value )
    {
        out.append( reinterpret_cast<const char*>( &value ), sizeof( T ) );
    }

    // A v1 or v2 archive, built by hand. There is no writer for either any more — the tree keeps ONE
    // way to write an archive and it writes v3 — so the only honest way to prove the reader still
    // reads them is to lay out their bytes here, which is also the only place the OLD layouts are
    // now written down.
    std::string LegacyArchive( int version, const std::vector<std::pair<std::string, std::string>>& entries )
    {
        std::string out;
        out += version == 1 ? "DPK1" : "DPK2";
        PushPod<uint32_t>( out, static_cast<uint32_t>( entries.size() ) );
        const size_t indexOffsetField = out.size();
        PushPod<uint64_t>( out, 0 );

        std::vector<uint64_t> offsets;
        for ( const auto& [key, data] : entries )
        {
            (void)key;
            offsets.push_back( out.size() );
            out += data;
        }
        const uint64_t indexOffset = out.size();
        for ( size_t i = 0; i < entries.size(); ++i )
        {
            PushPod<uint32_t>( out, static_cast<uint32_t>( entries[i].first.size() ) );
            out += entries[i].first;
            PushPod<uint64_t>( out, offsets[i] );
            PushPod<uint64_t>( out, entries[i].second.size() );
            if ( version == 2 )
                PushPod<uint64_t>(
                     out, Common::Utils::PakContentHash( entries[i].second.data(), entries[i].second.size() ) );
        }
        std::memcpy( out.data() + indexOffsetField, &indexOffset, sizeof( indexOffset ) );
        return out;
    }
} // namespace

// THE FORMAT IS A BYTE LAYOUT, so it is pinned as one. The build already refuses a host whose byte
// order or type widths would break it (PakFile.cpp's static_asserts), and this is the other half:
// what the writer ACTUALLY emits, field by field, at known offsets. Windows is the primary shipping
// target and this machine is not Windows — a green macOS sweep is not a green build — so the check
// that matters is one that names the bytes rather than round-tripping them through the same code on
// one platform.
TEST( Pak, TheV3RecordLayoutIsPinnedByteForByte )
{
    const fs::path dir = MakeTempDir();
    const fs::path pak = dir / "layout.dpak";
    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "a", "xyz", 3 ) ); // 3 bytes never compress: stored verbatim
        ASSERT_EQ( writer.Finalize(), 1u );
    }

    const std::string bytes = Slurp( pak );
    // header(16) + blob(3) + record(4 + 1 + 8 + 8 + 8 + 8 + 4 + 4)
    ASSERT_EQ( bytes.size(), 16u + 3u + 45u );
    EXPECT_EQ( bytes.substr( 0, 4 ), "DPK3" );

    const auto u32At = [&]( size_t at )
    {
        uint32_t v = 0;
        std::memcpy( &v, bytes.data() + at, 4 );
        return v;
    };
    const auto u64At = [&]( size_t at )
    {
        uint64_t v = 0;
        std::memcpy( &v, bytes.data() + at, 8 );
        return v;
    };

    EXPECT_EQ( u32At( 4 ), 1u );  // entry count
    EXPECT_EQ( u64At( 8 ), 19u ); // index offset = header + the one blob
    EXPECT_EQ( bytes.substr( 16, 3 ), "xyz" );
    EXPECT_EQ( u32At( 19 ), 1u ); // path length
    EXPECT_EQ( bytes[23], 'a' );
    EXPECT_EQ( u64At( 24 ), 16u ); // offset
    EXPECT_EQ( u64At( 32 ), 3u );  // stored size
    EXPECT_EQ( u64At( 40 ), 3u );  // content size
    EXPECT_EQ( u64At( 48 ), Common::Utils::PakContentHash( "xyz", 3 ) );
    EXPECT_EQ( u32At( 56 ), Common::Utils::Crc32c( "xyz", 3 ) );
    EXPECT_EQ( u32At( 60 ), static_cast<uint32_t>( Common::Utils::PakCodec::Store ) );

    // The low byte first, spelled out: on a big-endian host every field above would still compare
    // equal to itself and this is the one assertion that would not.
    EXPECT_EQ( static_cast<unsigned char>( bytes[4] ), 1u );
    EXPECT_EQ( static_cast<unsigned char>( bytes[7] ), 0u );

    // AND AGAIN WITH AN ENTRY WHOSE TWO SIZES DIFFER, because the check above cannot see the ORDER of
    // the size columns: a stored entry has one value in both, so swapping them in the writer leaves
    // every assertion so far passing. Measured — that exact mutation was green here and only turned
    // the round-trip tests red, which is a defect found three tests away from the file layout it is
    // about.
    const fs::path    packed = fs::path( pak ).parent_path() / "layout_lz4.dpak";
    const std::string runs( 40000, 'C' );
    {
        Common::Utils::PakWriter writer( packed );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "c", runs.data(), runs.size() ) );
        ASSERT_EQ( writer.Finalize(), 1u );
    }
    const std::string packedBytes = Slurp( packed );
    const auto        pu64At      = [&]( size_t at )
    {
        uint64_t v = 0;
        std::memcpy( &v, packedBytes.data() + at, 8 );
        return v;
    };
    uint64_t packedIndex = 0;
    std::memcpy( &packedIndex, packedBytes.data() + 8, 8 );
    const size_t   record      = static_cast<size_t>( packedIndex ) + 4 + 1; // u32 pathLen + "c"
    const uint64_t storedSize  = pu64At( record + 8 );
    const uint64_t contentSize = pu64At( record + 16 );
    EXPECT_EQ( contentSize, runs.size() );
    EXPECT_LT( storedSize, contentSize ) << "the stored size column is not where the format says";
    EXPECT_EQ( storedSize, packedIndex - 16u ); // the one blob fills the whole content region
}

// ── THE EXCEPTION REGISTER, AND THE PROPERTY IT PROTECTS ─────────────────────────────────────────
//
// "The data decides" is the packer's policy and it is right for content whose only question is how
// many bytes it costs to ship. `kStoredVerbatimRules` is the list of kinds for which it is WRONG,
// because compressing the whole entry destroys a property the file format exists to provide — and it
// destroys it silently: the archive round-trips, every other test stays green, and the only thing
// that changed is that reading a part of the file now costs decoding all of it.
//
// TWO TESTS, BECAUSE THE REGISTER AND THE BEHAVIOUR CAN DRIFT APART. The first pins the rows; the
// second proves the packer obeys them, and carries its own negative control.
TEST( Pak, EveryStoredVerbatimExceptionIsNamedHereAndTheCountIsDerivedFromTheNames )
{
    // THE REGISTER. One row per exception, named, and the count comes from this list rather than being
    // written down beside it: a gate that pinned a NUMBER could be satisfied by editing the number,
    // which is how an exception nobody argued for gets in. Adding a rule without naming it here fails,
    // and so does removing one that is named.
    constexpr std::string_view kNamed[] = {
         ".tex", // the cooked texture container — one mip must be readable without the file
    };

    EXPECT_EQ( Common::Utils::kStoredVerbatimRules.size(), std::size( kNamed ) )
         << "the packer's exception register changed and this census did not. Name the row and say why, "
            "or delete it — the count is derived from the names on purpose.";

    // Both directions, because one alone is half a census: a named row that is gone would otherwise
    // pass, and a row nobody named would pass the moment the sizes happened to match.
    for ( const std::string_view named : kNamed )
    {
        const auto found = std::find_if(
             Common::Utils::kStoredVerbatimRules.begin(), Common::Utils::kStoredVerbatimRules.end(),
             [named]( const Common::Utils::StoredVerbatimRule& rule ) { return rule.Extension == named; } );
        EXPECT_TRUE( found != Common::Utils::kStoredVerbatimRules.end() )
             << named << " is named here and is not in the register";
    }
    for ( const Common::Utils::StoredVerbatimRule& rule : Common::Utils::kStoredVerbatimRules )
    {
        EXPECT_TRUE( std::find( std::begin( kNamed ), std::end( kNamed ), rule.Extension ) != std::end( kNamed ) )
             << rule.Extension << " is exempted from compression and nothing here says why";
        // A ROW WITHOUT A REASON IS A ROW THAT CANNOT BE REVIEWED. The register's second column is the
        // whole difference between an argued exception and a hard-coded extension list.
        EXPECT_FALSE( rule.Why.empty() ) << rule.Extension << " has no reason recorded";
        EXPECT_EQ( rule.Extension.front(), '.' ) << rule.Extension << " must be spelled with its dot";
    }
}

// THE BEHAVIOUR, WITH ITS OWN NEGATIVE CONTROL IN THE SAME ARCHIVE. The same bytes are added twice
// under two keys that differ only in their extension: the `.tex` must be stored verbatim and the other
// must be compressed. So the test cannot pass by accident in either direction — if someone deletes the
// exception the first half goes red, and if the payload ever stopped clearing the packer's threshold
// the second half goes red instead of quietly proving nothing.
TEST( Pak, CookedTexturesAreStoredWholeSoOneLevelStaysReadableOnItsOwn )
{
    const fs::path dir = MakeTempDir();
    const fs::path pak = dir / "textures.dpak";

    // A PAYLOAD THAT COMPRESSES, AND THE REGISTER'S DECISION IS BY NAME. This used to read the committed
    // `Editor/Cooked/Textures/T_Checker.tex`, on the argument that a synthetic payload tests a decision
    // about shipped content against bytes nothing ships. That file is no longer committed (PK1: the
    // packager cooks what it ships), and the argument is now honoured where the shipped bytes are made:
    // Desert/Tests/Editor/PackagedContent reads the codec of the REAL cooked checker texture inside the
    // REAL package. What is left here is the rule itself, and the rule reads the key, not the bytes --
    // so all it needs from the payload is that it clears the compression threshold, which the control
    // below asserts rather than assumes.
    std::string payload( 64 * 1024, '\0' );
    for ( std::size_t i = 0; i < payload.size(); ++i )
        payload[i] = static_cast<char>( ( i / 64 ) % 7 ); // long runs: LZ4 takes it far below the threshold

    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Cooked/Textures/T_Checker.tex", payload.data(), payload.size() ) );
        // The negative control: the same bytes, a name the register says nothing about.
        ASSERT_TRUE( writer.AddData( "Cooked/Textures/T_Checker.bin", payload.data(), payload.size() ) );
        // And the case spelling, which a case-preserving filesystem can hand the packer.
        ASSERT_TRUE( writer.AddData( "Cooked/Textures/T_Checker.TEX", payload.data(), payload.size() ) );
        ASSERT_EQ( writer.Finalize(), 3u );
    }

    Common::Utils::PakReader reader( pak );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();

    EXPECT_EQ( reader.EntryCodec( "Cooked/Textures/T_Checker.tex" ), Common::Utils::PakCodec::Store )
         << "a .tex was compressed as one entry: reading one mip level now means decoding the whole "
            "texture, which is the disease the container was written to cure";
    EXPECT_EQ( reader.EntryStoredSize( "Cooked/Textures/T_Checker.tex" ).value_or( 0 ), payload.size() );
    EXPECT_EQ( reader.EntryCodec( "Cooked/Textures/T_Checker.TEX" ), Common::Utils::PakCodec::Store )
         << "the verdict depended on the case the filesystem handed back";

    // THE CONTROL. These exact bytes DO clear the threshold, so the first half above is a decision the
    // packer took and not a measurement it never had to make.
    EXPECT_EQ( reader.EntryCodec( "Cooked/Textures/T_Checker.bin" ), Common::Utils::PakCodec::LZ4 )
         << "the payload no longer compresses, so this test can no longer tell an exception from a "
            "non-event — find a payload that does, do not delete the check";
    EXPECT_LE( reader.EntryStoredSize( "Cooked/Textures/T_Checker.bin" ).value_or( 0 ) *
                    Common::Utils::kCompressionDenominator,
               payload.size() * Common::Utils::kCompressionNumerator );

    // Whatever the codec column says, the bytes must come back.
    EXPECT_EQ( reader.Read( "Cooked/Textures/T_Checker.tex" ), payload );
    EXPECT_EQ( reader.Read( "Cooked/Textures/T_Checker.bin" ), payload );
}

// A DIRECTORY IS NOT A FILE, and the extension is taken from the last component for that reason. The
// key below ends in ".desce" and merely LIVES under a directory called "Cooked.tex".
TEST( Pak, TheExceptionIsReadOffTheFileNameAndNotOffTheDirectory )
{
    const fs::path    pak = MakeTempDir() / "dirs.dpak";
    const std::string repetitive( 200000, 'Z' );
    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Cooked.tex/Scenes/Main.desce", repetitive.data(), repetitive.size() ) );
        ASSERT_TRUE( writer.AddData( "Cooked/Scenes/NoExtension", repetitive.data(), repetitive.size() ) );
        ASSERT_EQ( writer.Finalize(), 2u );
    }

    Common::Utils::PakReader reader( pak );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    EXPECT_EQ( reader.EntryCodec( "Cooked.tex/Scenes/Main.desce" ), Common::Utils::PakCodec::LZ4 )
         << "a directory named Cooked.tex answered for a file inside it";
    EXPECT_EQ( reader.EntryCodec( "Cooked/Scenes/NoExtension" ), Common::Utils::PakCodec::LZ4 )
         << "a key with no extension took an exception that names one";
}

// EVERY KIND OF CONTENT, not one convenient file. A codec that mangles one class of bytes is the
// defect this whole step could introduce, and a round trip on a text asset would never see it.
TEST( Pak, EveryKindOfContentInTheTreeSurvivesPackAndReadByteForByte )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";
    const fs::path content = root / "Editor/Resources";

    // Up to two files per extension, biggest first so the sample is the one most likely to compress.
    std::map<std::string, std::vector<fs::path>> byKind;
    std::error_code                              ec;
    for ( auto it = fs::recursive_directory_iterator( content, ec ); it != fs::recursive_directory_iterator();
          it.increment( ec ) )
    {
        if ( ec || !it->is_regular_file() )
            continue;
        const std::string ext = it->path().extension().string();
        if ( ext.empty() || ext == ".DS_Store" )
            continue;
        if ( fs::file_size( it->path(), ec ) > 8u * 1024 * 1024 )
            continue; // the suite must stay quick; the big ones are measured elsewhere
        auto& list = byKind[ext];
        if ( list.size() < 2 )
            list.push_back( it->path() );
    }

    // A disk walk that found nothing would otherwise pass this test in silence.
    ASSERT_GE( byKind.size(), 12u ) << "only " << byKind.size() << " kinds of content found under "
                                    << content.string();

    const fs::path                     pak = MakeTempDir() / "kinds.dpak";
    std::map<std::string, std::string> expected;
    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        for ( const auto& [ext, files] : byKind )
        {
            for ( const auto& file : files )
            {
                // KEYED ON THE PATH, NOT THE FILENAME. Two files in different directories share a
                // name — `model.demat` appears under three mesh folders — so a filename key collided,
                // the `expected` map silently merged the pair, and the writer (which used to allow a
                // duplicate) wrote one more entry than the map held. The count mismatch was the only
                // symptom and it surfaced on Windows, where the walk happened to reach both.
                const std::string key = std::filesystem::relative( file, root ).generic_string() + ext;
                expected[key]         = Slurp( file );
                ASSERT_TRUE( writer.AddFile( key, file ) ) << file.string();
            }
        }
        ASSERT_EQ( writer.Finalize(), expected.size() );
    }

    Common::Utils::PakReader reader( pak );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    EXPECT_EQ( reader.Version(), Common::Utils::PakVersion::V3 );

    size_t compressed = 0;
    size_t stored     = 0;
    for ( const auto& [key, bytes] : expected )
    {
        const auto read = reader.Read( key );
        ASSERT_TRUE( read.has_value() ) << key;
        EXPECT_EQ( *read, bytes ) << key << " did not survive the round trip";
        EXPECT_EQ( reader.EntrySize( key ).value_or( 0 ), bytes.size() ) << key;
        if ( reader.EntryCodec( key ) == Common::Utils::PakCodec::LZ4 )
        {
            ++compressed;
            EXPECT_LT( reader.EntryStoredSize( key ).value_or( 0 ), bytes.size() ) << key;
        }
        else
        {
            ++stored;
            EXPECT_EQ( reader.EntryStoredSize( key ).value_or( 0 ), bytes.size() ) << key;
        }
    }

    // BOTH ARMS MUST HAVE BEEN TAKEN. A corpus that happened to compress entirely would leave the
    // stored path untested while the test still went green, and the reverse would mean the codec was
    // never exercised at all — the shape of a test that measures its own absence.
    EXPECT_GT( compressed, 0u ) << "nothing in the tree compressed; the codec path was never taken";
    EXPECT_GT( stored, 0u ) << "everything compressed; the stored path was never taken";
}

// The knob has to be the DATA's, not a list someone maintains. Two entries, one of each kind, and the
// packer must reach opposite conclusions about them without being told which is which.
TEST( Pak, CompressionIsDecidedPerEntryByWhatTheBytesDo )
{
    const fs::path    pak = MakeTempDir() / "policy.dpak";
    const std::string repetitive( 200000, 'R' );
    std::string       noise( 200000, '\0' );
    std::mt19937      rng( 4242 );
    for ( auto& c : noise )
        c = static_cast<char>( rng() & 0xFF );

    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "compressible", repetitive.data(), repetitive.size() ) );
        ASSERT_TRUE( writer.AddData( "incompressible", noise.data(), noise.size() ) );
        ASSERT_EQ( writer.Finalize(), 2u );
    }

    Common::Utils::PakReader reader( pak );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    EXPECT_EQ( reader.EntryCodec( "compressible" ), Common::Utils::PakCodec::LZ4 );
    EXPECT_EQ( reader.EntryCodec( "incompressible" ), Common::Utils::PakCodec::Store );

    // The threshold is a RELATION, so it is asserted as one rather than against a remembered number:
    // a kept compression must have saved at least what the policy demands, and a stored entry must
    // occupy exactly its content.
    EXPECT_LE( reader.EntryStoredSize( "compressible" ).value_or( 0 ) * Common::Utils::kCompressionDenominator,
               repetitive.size() * Common::Utils::kCompressionNumerator );
    EXPECT_EQ( reader.EntryStoredSize( "incompressible" ).value_or( 0 ), noise.size() );

    EXPECT_EQ( reader.Read( "compressible" ), repetitive );
    EXPECT_EQ( reader.Read( "incompressible" ), noise );
}

// THE GATE THAT MUST NOT GET WEAKER. A damaged archive stopped the game before v3 and must stop it
// after: a flipped bit in the data region leaves the header, the index and every span perfectly
// valid, so nothing but the per-entry check can see it.
TEST( Pak, AFlippedBitIsRefusedAndTheFAILURENamesTheEntry )
{
    const fs::path    dir = MakeTempDir();
    const fs::path    pak = dir / "bitrot.dpak";
    const std::string good( 40000, 'G' ); // compresses: the damage must be caught before decoding
    const std::string other = "untouched";
    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/good.bin", good.data(), good.size() ) );
        ASSERT_TRUE( writer.AddData( "Assets/other.txt", other.data(), other.size() ) );
        ASSERT_EQ( writer.Finalize(), 2u );
    }

    // THE OFFSET IS CHECKED BEFORE IT IS USED, not asserted and dereferenced. `ASSERT_TRUE` expands
    // to a `return`, which `bugprone-unchecked-optional-access` does not follow — so the assertion
    // reads as a guard to a person and as nothing at all to the analyser. A plain `if` is the
    // spelling both of them understand, and NOLINT here would silence the one check that noticed.
    const auto offset = Common::Utils::PakReader( pak ).EntryOffset( "Assets/good.bin" );
    if ( !offset.has_value() )
    {
        FAIL() << "the entry this test damages is not in the archive it just wrote — the damage would "
                  "have landed at an arbitrary place and the refusal below would mean nothing";
    }
    const uint64_t at                = *offset + 7;
    std::string    bytes             = Slurp( pak );
    bytes[static_cast<size_t>( at )] = static_cast<char>( bytes[static_cast<size_t>( at )] ^ 0x01 );
    Spit( pak, bytes );

    Common::Utils::PakReader reader( pak );
    // It still OPENS — that is the point. Every structural check passes, so the only thing between
    // the game and corrupt content is the read's own verification.
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    EXPECT_FALSE( reader.Read( "Assets/good.bin" ).has_value() );
    // And the damage is confined to the entry that has it: refusing the whole archive would turn one
    // bad sector into a game that will not start at all.
    EXPECT_EQ( reader.Read( "Assets/other.txt" ), other );
}

TEST( Pak, AnArchiveFromAFutureVersionIsRefusedByItsName )
{
    const fs::path dir   = MakeTempDir();
    const fs::path pak   = dir / "future.dpak";
    std::string    bytes = LegacyArchive( 2, { { "a", "b" } } );
    bytes[3]             = '7'; // DPK7: a version this build cannot know about
    Spit( pak, bytes );

    Common::Utils::PakReader reader( pak );
    EXPECT_FALSE( reader.IsOpen() );
    // "Corrupt" and "newer than this build" have opposite remedies — re-download against upgrade —
    // and the magic is the only place the difference is legible.
    EXPECT_NE( reader.OpenError().find( "DPK7" ), std::string::npos ) << reader.OpenError();
    EXPECT_NE( reader.OpenError().find( "newer" ), std::string::npos ) << reader.OpenError();

    // A file that is not a Desert archive at all still says so, rather than being reported as a
    // version problem: the prefix is what separates the two.
    const fs::path zip = dir / "notours.dpak";
    Spit( zip, std::string( "PK\x03\x04padding-that-is-long-enough", 30 ) );
    Common::Utils::PakReader other( zip );
    EXPECT_FALSE( other.IsOpen() );
    EXPECT_NE( other.OpenError().find( "not a Desert archive" ), std::string::npos ) << other.OpenError();
}

// MIGRATION IS "IT STILL READS", stated as a test rather than as a promise. There is no writer for
// either old version any more, so these bytes are the only remaining definition of their layouts.
TEST( Pak, ArchivesFromEveryEarlierVersionStillRead )
{
    const fs::path    dir   = MakeTempDir();
    const fs::path    v1    = dir / "old1.dpak";
    const fs::path    v2    = dir / "old2.dpak";
    const std::string alpha = "content of alpha";
    const std::string beta  = "content of beta";
    Spit( v1, LegacyArchive( 1, { { "a.txt", alpha }, { "b.txt", beta } } ) );
    Spit( v2, LegacyArchive( 2, { { "a.txt", alpha }, { "b.txt", beta } } ) );

    Common::Utils::PakReader r1( v1 );
    ASSERT_TRUE( r1.IsOpen() ) << r1.OpenError();
    EXPECT_EQ( r1.Version(), Common::Utils::PakVersion::V1 );
    EXPECT_EQ( r1.Read( "a.txt" ), alpha );
    EXPECT_EQ( r1.EntryHash( "a.txt" ).value_or( 1 ), 0u ); // v1 carries no identity column
    EXPECT_EQ( r1.EntryCodec( "a.txt" ), Common::Utils::PakCodec::Store );
    EXPECT_EQ( r1.EntryStoredSize( "a.txt" ).value_or( 0 ), alpha.size() );

    Common::Utils::PakReader r2( v2 );
    ASSERT_TRUE( r2.IsOpen() ) << r2.OpenError();
    EXPECT_EQ( r2.Version(), Common::Utils::PakVersion::V2 );
    EXPECT_EQ( r2.Read( "b.txt" ), beta );
    EXPECT_EQ( r2.EntryHash( "b.txt" ).value_or( 0 ), Common::Utils::PakContentHash( beta.data(), beta.size() ) );

    // AND A v2 ARCHIVE IS STILL VERIFIED, at v2's price. An archive already sitting on a disk must
    // not become LESS checked because a newer format arrived.
    std::string damaged = LegacyArchive( 2, { { "a.txt", alpha } } );
    damaged[18]         = static_cast<char>( damaged[18] ^ 0x01 );
    const fs::path bad  = dir / "old2bad.dpak";
    Spit( bad, damaged );
    Common::Utils::PakReader r2bad( bad );
    ASSERT_TRUE( r2bad.IsOpen() ) << r2bad.OpenError();
    EXPECT_FALSE( r2bad.Read( "a.txt" ).has_value() );
}

TEST( Pak, AppendAddsWithoutMovingOrRewritingWhatWasAlreadyThere )
{
    const fs::path    dir = MakeTempDir();
    const fs::path    pak = dir / "grow.dpak";
    const std::string first( 30000, 'F' );
    const std::string second = "a second entry";
    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "one.bin", first.data(), first.size() ) );
        ASSERT_TRUE( writer.AddData( "two.txt", second.data(), second.size() ) );
        ASSERT_EQ( writer.Finalize(), 2u );
    }

    uint64_t offsetBefore = 0;
    uint64_t hashBefore   = 0;
    {
        Common::Utils::PakReader before( pak );
        ASSERT_TRUE( before.IsOpen() );
        offsetBefore = *before.EntryOffset( "one.bin" );
        hashBefore   = *before.EntryHash( "one.bin" );
    }

    const std::string third( 12000, 'T' );
    {
        Common::Utils::PakWriter appender( pak, Common::Utils::PakWriter::Mode::Append );
        ASSERT_TRUE( appender.IsOpen() );
        ASSERT_TRUE( appender.AddData( "three.bin", third.data(), third.size() ) );
        // The count is the archive's, not the batch's — two old records plus one new.
        EXPECT_EQ( appender.Finalize(), 3u );
    }

    Common::Utils::PakReader after( pak );
    ASSERT_TRUE( after.IsOpen() ) << after.OpenError();
    EXPECT_EQ( after.EntryCount(), 3u );
    EXPECT_EQ( after.Read( "one.bin" ), first );
    EXPECT_EQ( after.Read( "two.txt" ), second );
    EXPECT_EQ( after.Read( "three.bin" ), third );
    // THE PROMISE OF THE MODE, asserted rather than described: the bytes already in the archive were
    // not moved and not re-identified.
    EXPECT_EQ( after.EntryOffset( "one.bin" ).value_or( 0 ), offsetBefore );
    EXPECT_EQ( after.EntryHash( "one.bin" ).value_or( 0 ), hashBefore );
    // And the new payload went past the old index rather than over it.
    EXPECT_GT( after.EntryOffset( "three.bin" ).value_or( 0 ), offsetBefore );
}

// AN INTERRUPTED APPEND COSTS NOTHING, and this is the state a crash actually leaves behind: every
// byte a real append wrote, and a header that was never updated to name them. Reconstructed from the
// two versions of the file rather than simulated with rubbish, because the thing under test is WHERE
// the append put its bytes.
//
// AN EARLIER VERSION OF THIS TEST APPENDED GARBAGE BY HAND AND WAS GREEN AGAINST THE NAIVE
// IMPLEMENTATION. Writing new blobs OVER the old index — which sits exactly where the blobs end —
// still produces a perfectly valid archive when the append finishes, so nothing that only looks at
// the finished file can tell the two apart. The difference is only visible in what the file looks
// like BEFORE the last sixteen bytes land, and that is what these two assertions are.
TEST( Pak, AnAppendInterruptedBeforeItsHeaderLeavesTheArchiveExactlyAsItWas )
{
    const fs::path    dir = MakeTempDir();
    const fs::path    pak = dir / "torn.dpak";
    const std::string first( 5000, 'P' );
    const std::string second = "the second entry, which sits right before the index";
    {
        Common::Utils::PakWriter writer( pak );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "kept.bin", first.data(), first.size() ) );
        ASSERT_TRUE( writer.AddData( "kept.txt", second.data(), second.size() ) );
        ASSERT_EQ( writer.Finalize(), 2u );
    }
    const std::string before = Slurp( pak );

    const std::string added( 9000, 'N' );
    {
        Common::Utils::PakWriter appender( pak, Common::Utils::PakWriter::Mode::Append );
        ASSERT_TRUE( appender.IsOpen() );
        ASSERT_TRUE( appender.AddData( "added.bin", added.data(), added.size() ) );
        ASSERT_EQ( appender.Finalize(), 3u );
    }
    const std::string after = Slurp( pak );

    // THE RULE ITSELF: past the header, not one byte of the archive that was already there moved.
    // The old index is inside this range, and an append that wrote its blobs over it — the naive
    // implementation, and the one the format invites — fails here and nowhere else.
    //
    // The sixteen header bytes are excluded because the header is the ONE thing an append is supposed
    // to change, and it is changed last and alone; that is the whole design. (This assertion was
    // written without the exclusion first and went red on the correct code, which is the clearest
    // possible statement of what the rule does and does not cover.)
    ASSERT_GT( after.size(), before.size() );
    EXPECT_EQ( after.compare( 16, before.size() - 16, before, 16, before.size() - 16 ), 0 )
         << "the append rewrote bytes that were already in the archive";

    // The crash state: everything the append wrote, with the OLD header still in front of it.
    const fs::path crashed = dir / "crashed.dpak";
    Spit( crashed, before.substr( 0, 16 ) + after.substr( 16 ) );

    Common::Utils::PakReader reader( crashed );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    EXPECT_EQ( reader.EntryCount(), 2u );
    EXPECT_EQ( reader.Read( "kept.bin" ), first );
    EXPECT_EQ( reader.Read( "kept.txt" ), second );
    EXPECT_FALSE( reader.Contains( "added.bin" ) ); // nothing points at it, so nothing sees it
}

TEST( Pak, AppendRefusesTheArchivesItWouldHaveToRewrite )
{
    const fs::path dir = MakeTempDir();

    // A v2 archive: appending would write a v3 index into a file whose own header says v2.
    const fs::path legacy = dir / "legacy.dpak";
    Spit( legacy, LegacyArchive( 2, { { "a.txt", "alpha" } } ) );
    Common::Utils::PakWriter onLegacy( legacy, Common::Utils::PakWriter::Mode::Append );
    EXPECT_FALSE( onLegacy.IsOpen() );

    // A patch archive: its deletion list was authored against a set of entries this would change.
    const fs::path patch = dir / "patch.dpak";
    {
        Common::Utils::PakWriter writer( patch );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "kept.txt", "k", 1 ) );
        ASSERT_TRUE( writer.SetDeletedKeys( { "gone.txt" } ) );
        ASSERT_EQ( writer.Finalize(), 2u );
    }
    Common::Utils::PakWriter onPatch( patch, Common::Utils::PakWriter::Mode::Append );
    EXPECT_FALSE( onPatch.IsOpen() );

    // A file that is not there at all: an append is not a create in disguise.
    Common::Utils::PakWriter onNothing( dir / "absent.dpak", Common::Utils::PakWriter::Mode::Append );
    EXPECT_FALSE( onNothing.IsOpen() );
}

// WHICH ARCHIVE ANSWERED, asked rather than inferred. Two paks shipping the SAME key with the SAME
// bytes is a legal and common case — a patch that re-ships a file unchanged — and it makes the
// precedence invisible to any comparison of the content.
TEST( Pak, TheMountStackCanBeAskedWhichArchiveServesAKey )
{
    const fs::path    dir  = MakeTempDir();
    const fs::path    base = dir / "base.dpak";
    const fs::path    over = dir / "over.dpak";
    const std::string same = "identical in both archives";
    {
        Common::Utils::PakWriter writer( base );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/same.txt", same.data(), same.size() ) );
        ASSERT_TRUE( writer.AddData( "Assets/onlybase.txt", "b", 1 ) );
        ASSERT_EQ( writer.Finalize(), 2u );
    }
    {
        Common::Utils::PakWriter writer( over );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/same.txt", same.data(), same.size() ) );
        ASSERT_EQ( writer.Finalize(), 1u );
    }

    const auto mountedBase = Common::Utils::VFS::MountPak( base );
    ASSERT_TRUE( mountedBase.IsSuccess() ) << mountedBase.GetError();
    const auto mountedOver = Common::Utils::VFS::MountPak( over );
    ASSERT_TRUE( mountedOver.IsSuccess() ) << mountedOver.GetError();

    EXPECT_EQ( Common::Utils::VFS::ReadFile( dir / "Assets/same.txt" ), same );
    // The later mount wins, and the answer says so by name — which the bytes cannot.
    ASSERT_TRUE( Common::Utils::VFS::SourcePak( dir / "Assets/same.txt" ).has_value() );
    EXPECT_EQ( *Common::Utils::VFS::SourcePak( dir / "Assets/same.txt" ), over );
    EXPECT_EQ( *Common::Utils::VFS::SourcePak( dir / "Assets/onlybase.txt" ), base );
    // And it agrees with the read: a key nothing serves has no source either.
    EXPECT_FALSE( Common::Utils::VFS::SourcePak( dir / "Assets/absent.txt" ).has_value() );
    EXPECT_FALSE( Common::Utils::VFS::ReadFile( dir / "Assets/absent.txt" ).has_value() );

    Common::Utils::VFS::Unmount();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
