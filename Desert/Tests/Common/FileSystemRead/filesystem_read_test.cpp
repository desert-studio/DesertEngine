// The read primitives' contract: a path that resolves neither on disk nor in a mounted .dpak is a
// NAMED failure — the path is logged and a Result carrying the error comes back — never process
// death. Until 2026-09-05 both primitives hit DESERT_VERIFY (LogError + abort in EVERY
// configuration) on that miss, which made every "file is empty or missing" branch in the asset
// loaders dead code and crashed the editor over a scene naming a deleted prefab. The tests in this
// file are alive only because the primitives return: reverting the miss path to DESERT_VERIFY
// kills the test process, which is exactly the failure mode the suite pins.
//
// Since Ф3 the softness is guarded by the TYPE, not by every caller's diligence: the primitives
// return ResultStr<...>, so content cannot be used without unwrapping, and the old ambiguity is
// gone — a MISSING file is an error naming the path while a genuinely EMPTY file is a success
// holding an empty value. The relation tests below pin that the two are DIFFERENT values; before
// this change both produced one indistinguishable emptiness.
//
// The suite also covers FileSystem::ListFilesRecursive — the ONE enumeration every content scanner
// shares. Its relation: the union of the loose files on disk and the mounted pak's entries under a
// root, deduplicated so a loose file overrides its pak twin. The font and icon services used to
// walk only the disk half, so a packaged game (no loose directories at all) scanned nothing.

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "../../TestSupport/result_assert.hpp"
#include "../../TestSupport/engine_dir.hpp"

namespace fs = std::filesystem;

namespace
{
    fs::path MakeTempDir( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / name;
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

    void WriteFile( const fs::path& p, const std::string& content )
    {
        fs::create_directories( p.parent_path() );
        std::ofstream out( p, std::ios::binary );
        out << content;
    }

} // namespace

TEST( FileSystemRead, MissingFileIsANamedErrorInsteadOfDying )
{
    const fs::path missing = MakeTempDir( "desert_fsread_test" ) / "does_not_exist.desce";
    ASSERT_FALSE( fs::exists( missing ) );

    // The assertion is not just the failure — it is that this line RETURNS at all.
    const auto result = Common::Utils::FileSystem::ReadFileContent( missing );
    EXPECT_FALSE( result.IsSuccess() );
    // NAMED: the error carries the path, so a caller that just forwards GetError() still tells the
    // user which file was missing.
    EXPECT_NE( result.GetError().find( "does_not_exist.desce" ), std::string::npos ) << result.GetError();
}

TEST( FileSystemRead, MissingFileIsANamedByteErrorInsteadOfDying )
{
    const fs::path missing = MakeTempDir( "desert_fsread_test" ) / "does_not_exist.ttf";
    ASSERT_FALSE( fs::exists( missing ) );

    const auto result = Common::Utils::FileSystem::ReadByteFileContent( missing );
    EXPECT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "does_not_exist.ttf" ), std::string::npos ) << result.GetError();
}

// THE RELATION THIS TASK EXISTS FOR: an absent file and a zero-byte file are DIFFERENT values.
// Before the typed return both came back as one indistinguishable emptiness, and a caller that
// cared had to ask Exists() up front — forgetting to was a silent substitution (§1.4).
TEST( FileSystemRead, MissingAndEmptyAreDistinguishable )
{
    const fs::path dir   = MakeTempDir( "desert_fsread_relation" );
    const fs::path empty = dir / "zero_bytes.desce";
    WriteFile( empty, "" );
    ASSERT_TRUE( fs::exists( empty ) );

    const auto emptyRead   = Common::Utils::FileSystem::ReadFileContent( empty );
    const auto missingRead = Common::Utils::FileSystem::ReadFileContent( dir / "never_written.desce" );

    // A genuinely empty file is a SUCCESS holding "" — not an error.
    ASSERT_TRUE( emptyRead.IsSuccess() );
    EXPECT_EQ( emptyRead.GetValue(), "" );

    // A missing file is an ERROR — not a success holding "".
    EXPECT_FALSE( missingRead.IsSuccess() );

    // The relation itself, stated once: the two outcomes disagree.
    EXPECT_NE( emptyRead.IsSuccess(), missingRead.IsSuccess() );
}

TEST( FileSystemRead, MissingAndEmptyAreDistinguishableForBytes )
{
    const fs::path dir   = MakeTempDir( "desert_fsread_relation_bytes" );
    const fs::path empty = dir / "zero_bytes.bin";
    WriteFile( empty, "" );
    ASSERT_TRUE( fs::exists( empty ) );

    const auto emptyRead   = Common::Utils::FileSystem::ReadByteFileContent( empty );
    const auto missingRead = Common::Utils::FileSystem::ReadByteFileContent( dir / "never_written.bin" );

    ASSERT_TRUE( emptyRead.IsSuccess() );
    EXPECT_TRUE( emptyRead.GetValue().empty() );
    EXPECT_FALSE( missingRead.IsSuccess() );
}

// A READ THAT FAILS AFTER THE OPEN SUCCEEDED IS AN ERROR TOO — the third outcome, and the one that
// had no test. A directory is the portable way to reach it: ifstream opens it and the first read
// fails, which is the same shape as a racing delete, a truncated pak or an I/O error on a network
// volume. ReadFileContent used to DISCARD its read result and hand back a SUCCESS holding
// resize()'s zero fill (measured: 64 NUL bytes on macOS, no error, no log) while
// ReadByteFileContent got it right — so the two primitives disagreed about their own contract, and
// the string half was the silent substitution the typed return exists to remove.
TEST( FileSystemRead, AReadThatFailsAfterOpeningIsANamedErrorNotZeroedContent )
{
    const fs::path dir = MakeTempDir( "desert_fsread_unreadable" );
    ASSERT_TRUE( fs::is_directory( dir ) );

    const auto text = Common::Utils::FileSystem::ReadFileContent( dir );
    EXPECT_FALSE( text.IsSuccess() ) << "got a success holding " << text.GetValue().size() << " byte(s)";
    EXPECT_NE( text.GetError().find( "desert_fsread_unreadable" ), std::string::npos ) << text.GetError();

    // The byte primitive answers the same question the same way — the two halves agree.
    const auto bytes = Common::Utils::FileSystem::ReadByteFileContent( dir );
    EXPECT_FALSE( bytes.IsSuccess() );
    EXPECT_EQ( text.IsSuccess(), bytes.IsSuccess() );
}

TEST( FileSystemRead, MissingFileUnderAMountedPakIsStillSoft )
{
    // A pak IS mounted and owns the path's directory — the file just is not in it. Both halves of
    // the lookup miss, which is the exact spot the old abort lived in.
    const fs::path dir = MakeTempDir( "desert_fsread_pak" );
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        const std::string other = "present";
        ASSERT_TRUE( writer.AddData( "Assets/present.txt", other.data(), other.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }
    const auto mounted = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();

    EXPECT_FALSE( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/absent.txt" ).IsSuccess() );
    EXPECT_FALSE( Common::Utils::FileSystem::ReadByteFileContent( dir / "Assets/absent.txt" ).IsSuccess() );

    Common::Utils::VFS::Unmount();
}

TEST( FileSystemRead, ListFilesRecursiveMergesDiskAndPakAndTheLooseFileWins )
{
    const fs::path dir = MakeTempDir( "desert_fsread_list" );

    // Loose half: a.ttf on disk. Pak half: the SAME a.ttf plus b.ttf that exists nowhere loose.
    WriteFile( dir / "Fonts" / "a.ttf", "loose-a" );
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        const std::string a = "pak-a", b = "pak-b";
        ASSERT_TRUE( writer.AddData( "Fonts/a.ttf", a.data(), a.size() ) );
        ASSERT_TRUE( writer.AddData( "Fonts/b.ttf", b.data(), b.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }
    const auto mounted = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();

    const auto listed = Common::Utils::FileSystem::ListFilesRecursive( dir / "Fonts" );
    ASSERT_EQ( listed.size(), 2u ); // a.ttf deduplicated across the two halves, b.ttf from the pak

    const auto a =
         std::find_if( listed.begin(), listed.end(), []( const fs::path& p ) { return p.filename() == "a.ttf"; } );
    ASSERT_NE( a, listed.end() );
    // The loose spelling survived the dedup, so a later read of the listed path gets the LOOSE bytes
    // (disk-first VFS override) — the relation the debugging workflow relies on.
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( *a ), "loose-a" );

    Common::Utils::VFS::Unmount();
}

TEST( FileSystemRead, ListFilesRecursiveResolvesARelativeRootThroughThePak )
{
    // The packaged-game shape: the launcher cds next to Content.dpak and every resource root is a
    // RELATIVE path ("Resources/Fonts/") with no loose directory behind it. The enumeration must
    // resolve that root against the cwd and find the pak's files.
    const fs::path dir = MakeTempDir( "desert_fsread_rel" );
    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        const std::string ttf = "packed-font";
        ASSERT_TRUE( writer.AddData( "Resources/Fonts/fake.ttf", ttf.data(), ttf.size() ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    const Desert::TestSupport::EngineDirScope engineDir( dir );
    const auto mounted = Common::Utils::VFS::MountPak( dir / "Content.dpak" );
    ASSERT_TRUE( mounted.IsSuccess() ) << mounted.GetError();
    ASSERT_FALSE( fs::exists( dir / "Resources/Fonts" ) ); // nothing loose — the pak is the only source

    const auto listed = Common::Utils::FileSystem::ListFilesRecursive( "Resources/Fonts/" );
    ASSERT_EQ( listed.size(), 1u );
    EXPECT_EQ( listed[0].filename(), "fake.ttf" );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( listed[0] ), "packed-font" );

    Common::Utils::VFS::Unmount();
}

TEST( FileSystemRead, ListFilesRecursiveMissingRootIsEmptyNotAnError )
{
    const fs::path dir = MakeTempDir( "desert_fsread_norvoot" );
    EXPECT_TRUE( Common::Utils::FileSystem::ListFilesRecursive( dir / "never_created" ).empty() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
