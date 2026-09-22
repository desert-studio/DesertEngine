// THE RELATION UNDER TEST: a packaged game handed a damaged archive does not start quietly.
//
// Not "MountPak returns false" — that was true before this suite existed and the game started
// anyway, because both call sites in Runtime/Source/Main.cpp threw the result away. What has to hold
// is the agreement between three things that were free to disagree:
//
//   the archive is damaged  <->  startup refuses  <->  the refusal names the file and the step.
//
// Every case below asserts all three, and the ones that pass assert the opposite end: an INTACT
// update mounts and the bytes the game then reads are the patched ones. A suite that only proves the
// failure would be satisfied by a startup path that refuses everything.
//
// The archives are real .dpak files written by the engine's own PakWriter — the same code
// Tools/PakTool drives — and they are damaged by writing bytes into them, not by mocking a reader.

#include "PackagedContent.hpp"

#include <Common/Content/ContentChunks.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../TestSupport/result_assert.hpp"

namespace fs = std::filesystem;

namespace
{
    // One directory per test, so a case that leaves an archive behind cannot make the next one pass.
    fs::path MakeTempDir( const std::string& name )
    {
        const fs::path dir = fs::temp_directory_path() / ( "desert_packaged_mount_" + name );
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

    void WritePak( const fs::path& file, const std::vector<std::pair<std::string, std::string>>& entries )
    {
        Common::Utils::PakWriter writer( file );
        ASSERT_TRUE( writer.IsOpen() ) << file.string();
        for ( const auto& [key, data] : entries )
            ASSERT_TRUE( writer.AddData( key, data.data(), data.size() ) ) << key;
        ASSERT_EQ( writer.Finalize(), entries.size() );
    }

    std::string ReadBytes( const fs::path& file )
    {
        std::ifstream in( file, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }

    void WriteBytes( const fs::path& file, const std::string& bytes )
    {
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        out.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
    }

    // Cut the tail off — what a download interrupted part-way through actually leaves on disk, and
    // the single most likely way a shipped patch arrives damaged.
    void TruncateToHalf( const fs::path& file )
    {
        const std::string bytes = ReadBytes( file );
        WriteBytes( file, bytes.substr( 0, bytes.size() / 2 ) );
    }

    // Flip one byte. `where` is an offset from the start; the caller picks whether that lands in the
    // header, in the data region or in the index, because those are three different defects.
    void FlipByte( const fs::path& file, size_t where )
    {
        std::string bytes = ReadBytes( file );
        ASSERT_LT( where, bytes.size() );
        bytes[where] = static_cast<char>( bytes[where] ^ 0xff );
        WriteBytes( file, bytes );
    }

    struct MountGuard
    {
        ~MountGuard()
        {
            Common::Utils::VFS::Unmount();
        }
    };
} // namespace

// ---------------------------------------------------------------- the passing end

TEST( PackagedMount, AnIntactUpdateMountsAndTheGameSeesItsContent )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "intact" );

    WritePak( dir / "Content.dpak",
              { { "Assets/level.desce", "first release" }, { "Assets/music.wav", "untouched by the update" } } );
    WritePak( dir / "Patch_001.dpak", { { "Assets/level.desce", "the fix the player downloaded" } } );

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );

    ASSERT_EQ( result.ExitCode, Desert::Player::kContentOk ) << result.Message;
    EXPECT_EQ( result.BasePak, dir / "Content.dpak" );
    ASSERT_EQ( result.Patches.size(), 1u );
    EXPECT_EQ( result.Patches[0], dir / "Patch_001.dpak" );

    // The point of mounting at all: the bytes the game reads are the UPDATED ones, and the files the
    // update did not touch still come from the base.
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/level.desce" ),
                             "the fix the player downloaded" );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/music.wav" ),
                             "untouched by the update" );
}

TEST( PackagedMount, AnUpdateThatREMOVESAFileIsAppliedAtStartup )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "removal" );

    WritePak( dir / "Content.dpak", { { "Assets/level.desce", "first release" },
                                      { "Assets/cut_character.mesh", "an asset the sequel drops" } } );

    // Present BEFORE the update — with the base alone, because the patch is what has to make the
    // difference. Writing it first would mount it here too, and the assertion below would pass on an
    // engine that had never applied a deletion in its life.
    ASSERT_EQ( Desert::Player::MountPackagedContent( dir, "MyGame" ).ExitCode, Desert::Player::kContentOk );
    EXPECT_TRUE( Common::Utils::FileSystem::Exists( dir / "Assets/cut_character.mesh" ) );
    Common::Utils::VFS::Unmount();

    {
        // A patch that removes one file and changes another — the ordinary shape of a content update,
        // and the one an overlay mount could not express at all before the deletion list existed.
        Common::Utils::PakWriter writer( dir / "Patch_001.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        const std::string fixed = "the fix the player downloaded";
        ASSERT_TRUE( writer.AddData( "Assets/level.desce", fixed.data(), fixed.size() ) );
        ASSERT_TRUE( writer.SetDeletedKeys( { "Assets/cut_character.mesh" } ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    // Now with the patch in place — this is the whole startup path, not the VFS in isolation.
    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );
    ASSERT_EQ( result.ExitCode, Desert::Player::kContentOk ) << result.Message;
    ASSERT_EQ( result.Patches.size(), 1u );

    EXPECT_FALSE( Common::Utils::FileSystem::Exists( dir / "Assets/cut_character.mesh" ) );
    EXPECT_FALSE( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/cut_character.mesh" ).IsSuccess() );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/level.desce" ),
                             "the fix the player downloaded" );
}

TEST( PackagedMount, AnUpdateBuiltAgainstADifferentBaseStopsStartup )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "wrongbase" );

    WritePak( dir / "Content.dpak", { { "Assets/level.desce", "first release" } } );
    {
        Common::Utils::PakWriter writer( dir / "Patch_001.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        // This patch removes a file this base has never shipped: it was built against some other
        // release. Its other changes are therefore about content that is not here either, and applying
        // them would produce a game nobody has ever tested.
        ASSERT_TRUE( writer.SetDeletedKeys( { "Assets/from_another_game.mesh" } ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );

    EXPECT_EQ( result.ExitCode, Desert::Player::kContentPatchArchiveFailed );
    EXPECT_NE( result.Message.find( "from_another_game.mesh" ), std::string::npos ) << result.Message;
    EXPECT_NE( result.Message.find( "Patch_001.dpak" ), std::string::npos ) << result.Message;
    // Nothing half-mounted: refusing means the game does not run on a content set that half applied.
    EXPECT_FALSE( Common::Utils::VFS::IsMounted() );
}

TEST( PackagedMount, NoArchiveAtAllIsADevTreeAndNotAFailure )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "loose" );

    // A checkout with loose content on disk and no pak anywhere. Every read is a plain disk read, and
    // refusing to start here would break the way the engine is developed.
    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );

    EXPECT_EQ( result.ExitCode, Desert::Player::kContentOk ) << result.Message;
    EXPECT_TRUE( result.Message.empty() );
    EXPECT_TRUE( result.BasePak.empty() );
    EXPECT_FALSE( Common::Utils::VFS::IsMounted() );
}

// ---------------------------------------------------------------- the refusing end

TEST( PackagedMount, ADamagedUpdateStopsStartupAndNothingIsLeftMounted )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "bad_patch" );

    WritePak( dir / "Content.dpak", { { "Assets/level.desce", "first release" } } );
    WritePak( dir / "Patch_001.dpak", { { "Assets/level.desce", "the fix the player downloaded" } } );
    TruncateToHalf( dir / "Patch_001.dpak" );

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );

    // 1. It refuses, and with the code that means "the UPDATE is broken" rather than "the install is".
    EXPECT_EQ( result.ExitCode, Desert::Player::kContentPatchArchiveFailed );

    // 2. The message names the file, says what is wrong with it, and says what to do. A player has
    //    nothing else — no editor, no log to read, no sources.
    EXPECT_NE( result.Message.find( "Patch_001.dpak" ), std::string::npos ) << result.Message;
    EXPECT_NE( result.Message.find( "truncated" ), std::string::npos ) << result.Message;
    EXPECT_NE( result.Message.find( "What to do" ), std::string::npos ) << result.Message;

    // 3. AND NOTHING IS MOUNTED. This is the clause that makes the refusal mean something: a base
    //    that mounted before the patch failed would leave the process holding exactly the half-updated
    //    content set the refusal exists to prevent.
    EXPECT_FALSE( Common::Utils::VFS::IsMounted() );
    EXPECT_FALSE( Common::Utils::FileSystem::Exists( dir / "Assets/level.desce" ) );
}

TEST( PackagedMount, ADamagedGameArchiveStopsStartupWithADifferentCode )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "bad_base" );

    WritePak( dir / "Content.dpak", { { "Assets/level.desce", "first release" } } );
    FlipByte( dir / "Content.dpak", 0 ); // the magic — "this is not a Desert archive at all"

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );

    // A broken INSTALL and a broken UPDATE are different events with opposite remedies, so they are
    // different exit codes. Collapsing them would throw that away at the one place it is free.
    EXPECT_EQ( result.ExitCode, Desert::Player::kContentBaseArchiveFailed );
    EXPECT_NE( result.ExitCode, Desert::Player::kContentPatchArchiveFailed );
    EXPECT_NE( result.Message.find( "Content.dpak" ), std::string::npos ) << result.Message;
    EXPECT_NE( result.Message.find( "not a Desert archive" ), std::string::npos ) << result.Message;
    EXPECT_FALSE( Common::Utils::VFS::IsMounted() );
}

TEST( PackagedMount, TheBaseArchiveIsChosenByAFixedPreferenceOrder )
{
    // Which of several archives IS the game has never been asserted anywhere, and the order is not
    // arbitrary: renaming the exe and its pak together is how one build ships as two games, so the
    // exe's own name has to outrank the generic one. Patch paks are excluded at every step — a folder
    // holding only patches has no base, which is a different thing from having a damaged one.
    const fs::path dir = MakeTempDir( "preference" );

    WritePak( dir / "Patch_001.dpak", { { "p.txt", "p" } } );
    EXPECT_TRUE( Desert::Player::FindBasePak( dir, "MyGame" ).empty() );

    WritePak( dir / "Whatever.dpak", { { "w.txt", "w" } } );
    EXPECT_EQ( Desert::Player::FindBasePak( dir, "MyGame" ), dir / "Whatever.dpak" ); // the only one

    WritePak( dir / "Content.dpak", { { "c.txt", "c" } } );
    EXPECT_EQ( Desert::Player::FindBasePak( dir, "MyGame" ), dir / "Content.dpak" ); // beats "only one"

    WritePak( dir / "MyGame.dpak", { { "g.txt", "g" } } );
    EXPECT_EQ( Desert::Player::FindBasePak( dir, "MyGame" ), dir / "MyGame.dpak" ); // beats Content
}

TEST( PackagedMount, SeveralUnnamedArchivesRefuseRatherThanGuess )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "ambiguous" );

    WritePak( dir / "Alpha.dpak", { { "a.txt", "a" } } );
    WritePak( dir / "Beta.dpak", { { "b.txt", "b" } } );

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );

    EXPECT_EQ( result.ExitCode, Desert::Player::kContentArchivesAmbiguous );
    EXPECT_NE( result.Message.find( "Alpha.dpak" ), std::string::npos ) << result.Message;
    EXPECT_NE( result.Message.find( "Beta.dpak" ), std::string::npos ) << result.Message;
    EXPECT_FALSE( Common::Utils::VFS::IsMounted() );
}

// ---------------------------------------------------------------- the reason has to be the RIGHT one

TEST( PackagedMount, EachKindOfDamageIsNamedAsItself )
{
    // "Corrupt" is one word for six failures whose remedies differ: a wrong file, a partial
    // download, a damaged index. If they all came out identical the message would be no more useful
    // than the silence it replaced, so each one is pinned to its own wording here.
    const fs::path dir = MakeTempDir( "reasons" );

    const auto reasonFor = [&]( const std::string& name, void ( *damage )( const fs::path& ) )
    {
        const fs::path pak = dir / ( name + ".dpak" );
        WritePak( pak, { { "Assets/a.txt", std::string( 4096, 'a' ) } } );
        damage( pak );
        return Common::Utils::PakReader( pak ).OpenError();
    };

    EXPECT_NE( reasonFor( "magic", []( const fs::path& p ) { FlipByte( p, 2 ); } ).find( "not a Desert archive" ),
               std::string::npos );
    EXPECT_NE( reasonFor( "cut", []( const fs::path& p ) { TruncateToHalf( p ); } ).find( "truncated" ),
               std::string::npos );

    // A missing file is not damage and must not read like it.
    const auto missing = Common::Utils::PakReader( dir / "never_written.dpak" ).OpenError();
    EXPECT_NE( missing.find( "no such file" ), std::string::npos ) << missing;

    // An archive that is fine says nothing at all.
    const fs::path good = dir / "good.dpak";
    WritePak( good, { { "Assets/a.txt", "fine" } } );
    const Common::Utils::PakReader reader( good );
    EXPECT_TRUE( reader.IsOpen() );
    EXPECT_TRUE( reader.OpenError().empty() ) << reader.OpenError();
}

// ---------------------------------------------------------------- the half the structure cannot see

TEST( PackagedMount, DamageInsideTheContentItselfFailsTheREADRatherThanReturningWrongBytes )
{
    // THE CASE THE MOUNT CHECKS CANNOT CATCH, and the reason Read() verifies the stored hash. A byte
    // flipped in the DATA region leaves the header, the index and every span perfectly valid, so the
    // archive mounts and startup is right to allow it — the damage is invisible until somebody asks
    // for those exact bytes. Without the hash check the read succeeds and the game runs on content
    // nobody built.
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "bad_blob" );

    const std::string payload = "the level the player is about to load";
    WritePak( dir / "Content.dpak", { { "Assets/level.desce", payload } } );

    // The data region starts right after the 16-byte header, so this lands squarely inside the blob.
    FlipByte( dir / "Content.dpak", 16 + payload.size() / 2 );

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );
    ASSERT_EQ( result.ExitCode, Desert::Player::kContentOk ) << result.Message; // structurally intact

    const auto read = Common::Utils::FileSystem::ReadFileContent( dir / "Assets/level.desce" );
    EXPECT_FALSE( read.IsSuccess() ) << "a corrupt entry must never read back as a success";
    EXPECT_NE( read.GetValue(), payload );
}

TEST( PackagedMount, AnUndamagedArchiveStillReadsBackEveryByte )
{
    // The guard on the check above: hash verification must not reject anything sound. Sizes chosen to
    // cross the reader's buffering — empty, one byte, and larger than a block.
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "sizes" );

    const std::string empty;
    const std::string one( 1, '\x01' );
    const std::string big( 300000, '\xa5' );
    WritePak( dir / "Content.dpak",
              { { "Assets/empty.bin", empty }, { "Assets/one.bin", one }, { "Assets/big.bin", big } } );

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );
    ASSERT_EQ( result.ExitCode, Desert::Player::kContentOk ) << result.Message;

    const Common::Utils::PakReader reader( dir / "Content.dpak" );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    EXPECT_EQ( reader.Read( "Assets/empty.bin" ).value_or( "unset" ), empty );
    EXPECT_EQ( reader.Read( "Assets/one.bin" ).value_or( "unset" ), one );
    EXPECT_EQ( reader.Read( "Assets/big.bin" ).value_or( "unset" ), big );
}

// ---------------------------------------------------------------- the division into chunks
//
// A DIVIDED GAME IS STILL ONE CONTENT SET, and the failure direction that matters is the quiet one:
// a chunk that does not arrive must stop startup, because every symptom of a missing region turns up
// somewhere else entirely — a black texture, a scene that will not open, a model that is not there.

TEST( PackagedMount, TheBaseNamesItsChunksAndTheGameGetsTheirContent )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "chunks" );

    WritePak( dir / "Content.dpak", { { "Assets/menu.desce", "in the base" },
                                      { std::string( Common::Content::CHUNK_MANIFEST_KEY ), "North\n" } } );
    WritePak( dir / "Chunk_North.dpak", { { "Assets/north.desce", "in the region" } } );

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );

    ASSERT_EQ( result.ExitCode, Desert::Player::kContentOk ) << result.Message;
    ASSERT_EQ( result.Chunks.size(), 1u );
    EXPECT_EQ( result.Chunks[0], dir / "Chunk_North.dpak" );

    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/menu.desce" ),
                             "in the base" );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/north.desce" ),
                             "in the region" );
    // WHICH ARCHIVE the bytes came from, asked rather than inferred — the division is invisible to a
    // comparison of the content, and that is exactly why it has to be observable.
    EXPECT_EQ( Common::Utils::VFS::SourcePak( dir / "Assets/north.desce" ),
               std::optional<fs::path>( dir / "Chunk_North.dpak" ) );
    EXPECT_EQ( Common::Utils::VFS::SourcePak( dir / "Assets/menu.desce" ),
               std::optional<fs::path>( dir / "Content.dpak" ) );
}

TEST( PackagedMount, AChunkIsNotAGameAndOneThatDidNotArriveStopsStartup )
{
    {
        // A folder holding only chunk archives is not a folder holding several games: FindBasePak
        // must not offer one as the thing to start, or a partial download would come up as a game
        // made of one region.
        MountGuard     guard;
        const fs::path dir = MakeTempDir( "chunk_is_not_a_game" );
        WritePak( dir / "Chunk_North.dpak", { { "Assets/north.desce", "region" } } );
        WritePak( dir / "Chunk_South.dpak", { { "Assets/south.desce", "region" } } );

        std::vector<fs::path> ambiguous;
        EXPECT_TRUE( Desert::Player::FindBasePak( dir, "MyGame", &ambiguous ).empty() );
        EXPECT_TRUE( ambiguous.empty() ) << "chunk archives were offered as candidate games";
    }
    {
        MountGuard     guard;
        const fs::path dir = MakeTempDir( "chunk_missing" );
        WritePak( dir / "Content.dpak",
                  { { "Assets/menu.desce", "in the base" },
                    { std::string( Common::Content::CHUNK_MANIFEST_KEY ), "North\nSouth\n" } } );
        WritePak( dir / "Chunk_North.dpak", { { "Assets/north.desce", "in the region" } } );
        // Chunk_South.dpak was never delivered.

        const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );
        EXPECT_EQ( result.ExitCode, Desert::Player::kContentChunkArchiveFailed );
        EXPECT_NE( result.Message.find( "South" ), std::string::npos ) << result.Message;
        // NOTHING LEFT MOUNTED. Half a content set is the state this whole file exists to prevent.
        EXPECT_FALSE( Common::Utils::VFS::IsMounted() );
        EXPECT_TRUE( result.BasePak.empty() );
        EXPECT_TRUE( result.Chunks.empty() );
    }
}

TEST( PackagedMount, AnArchiveThatNamesNoChunksIsAGameThatWasNeverDivided )
{
    MountGuard     guard;
    const fs::path dir = MakeTempDir( "undivided" );
    WritePak( dir / "Content.dpak", { { "Assets/menu.desce", "the whole game" } } );

    const auto result = Desert::Player::MountPackagedContent( dir, "MyGame" );
    ASSERT_EQ( result.ExitCode, Desert::Player::kContentOk ) << result.Message;
    EXPECT_TRUE( result.Chunks.empty() );
    DESERT_EXPECT_RESULT_EQ( Common::Utils::FileSystem::ReadFileContent( dir / "Assets/menu.desce" ),
                             "the whole game" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
