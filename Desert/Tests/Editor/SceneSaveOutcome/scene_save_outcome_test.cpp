// THE DEFECT THIS SUITE EXISTS FOR, stated as the user meets it:
//
//   The scene has unsaved changes. The user drags another scene onto the viewport. The editor asks
//   "the current scene has unsaved changes" and offers "Save and Open". They click it. The save fails —
//   a read-only file, a full disk, a Scene/ directory that a sync client has locked — and the editor
//   puts the amber "unsaved changes" star out, raises a GREEN "Saved 'X'" toast, and then calls
//   LoadScene, which clears the command history and calls Scene::Clear(). The work they clicked the
//   button to protect is gone from memory, was never on disk, and they were told it was saved.
//
// The whole chain was `void` from end to end — FileSystem::WriteContentToFile, SceneSerializer::
// SaveToFile, Scene::Serialize, and the four call sites in EditorLayer — so no link in it could have
// answered differently. It returns a result now, and the DECISION taken from that result lives in
// Editor/Core/SceneSaveRules.hpp as a pure function so this suite can reach it: nothing here needs a
// window, a device, an ECS registry or ImGui.
//
// The tests below drive the REAL write primitive into a REAL failure — a blocked temporary path, the
// same lever Desert/Tests/Common/FileSystemWrite uses — and feed its genuine result into the rule, so
// what is asserted is the whole path from "the bytes did not land" to "the star stays on and the scene
// is not thrown away", with only ImGui's buttons stubbed out.

#include <Editor/Core/SceneSaveRules.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

using Common::Utils::FileSystem;
using Desert::Editor::Core::Rules::DecideAfterSceneSave;

namespace
{
    fs::path MakeTempDir( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / name;
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

    std::string ReadRaw( const fs::path& p )
    {
        std::ifstream      in( p, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // What SceneSerializer::SaveToFile does, minus the serializer: write the scene text, and answer with
    // the destination named in the error. Scene.hpp reaches the renderer, so no GPU-free suite can link
    // the real one (SceneStitch says the same about the load half) — this is the same two statements.
    Common::BoolResultStr SaveSceneText( const fs::path& path, const std::string& text )
    {
        if ( const auto written = FileSystem::WriteContentToFileAtomic( path, text ); !written )
            return Common::MakeFormattedError( "could not write {}: {}", path.string(), written.GetError() );
        return Common::MakeSuccess( true );
    }

    // The two pieces of editor state the verdict governs, so a test can say "the star is still on" and
    // "the scene is still in memory" instead of asserting two booleans with no names.
    struct EditorState
    {
        uint64_t    SavedRevision = 0;
        uint64_t    Revision      = 7; // != SavedRevision: the scene has unsaved changes
        bool        SceneInMemory = true;
        std::string LastToast;
        bool        LastToastWasError = false;

        bool ShowsUnsavedStar() const
        {
            return Revision != SavedRevision;
        }
    };

    // EditorLayer::SaveOpenScene and the "Save and Open" button, with ImGui removed: the same order of
    // the same three effects, taken from the same verdict.
    bool ApplyVerdictAndOpenAnother( EditorState& state, const Common::BoolResultStr& save )
    {
        const auto verdict = DecideAfterSceneSave( save, "Subject", "Scene/Subject.desce" );
        if ( verdict.MarkSceneSaved )
            state.SavedRevision = state.Revision;
        state.LastToast         = verdict.Message;
        state.LastToastWasError = verdict.IsError;

        if ( verdict.MayDiscardScene )
            state.SceneInMemory = false; // LoadScene() -> CommandHistory::Clear() + Scene::Clear()
        return verdict.MayDiscardScene;
    }
} // namespace

// ── THE ACCEPTANCE TEST ────────────────────────────────────────────────────────────────────────────
//
// A scene whose file cannot be written, saved through the real primitive, and the two things that must
// survive it: the unsaved mark, and the scene itself.
TEST( SceneSaveOutcome, AFailedWriteKeepsTheSceneDirtyAndAliveAndSaysSo )
{
    const fs::path dir   = MakeTempDir( "desert_scene_save_failed" );
    const fs::path scene = dir / "Subject.desce";
    {
        std::ofstream out( scene, std::ios::binary );
        out << R"({"SceneName":"Subject","Version":1})";
    }

    // The failure, built the way FileSystemWrite builds it: a directory sitting where the primitive
    // needs its working file. The destination itself stays perfectly writable, so this is a real
    // "the save did not happen" and not "the path was nonsense".
    fs::path temp = scene;
    temp += ".tmp";
    fs::create_directories( temp );

    EditorState state;
    ASSERT_TRUE( state.ShowsUnsavedStar() ) << "the fixture has to start dirty or it asserts nothing";

    const bool mayOpenAnother = ApplyVerdictAndOpenAnother( state, SaveSceneText( scene, "{\"new\":true}" ) );

    EXPECT_FALSE( mayOpenAnother )
         << "\"Save and Open\" would have gone on to LoadScene over a scene that was never written";
    EXPECT_TRUE( state.SceneInMemory )
         << "the in-memory scene was destroyed after a failed save — there is nowhere left to recover it "
            "from, and this is the single worst outcome in the editor";
    EXPECT_TRUE( state.ShowsUnsavedStar() )
         << "the unsaved-changes star went out for a scene that is still only in memory";
    EXPECT_TRUE( state.LastToastWasError ) << "the user was shown a success for a failed save";

    // The message has to carry the PATH: a GUI user does not have the log open, and "save failed" with
    // no file named is a message they cannot act on.
    EXPECT_NE( state.LastToast.find( scene.string() ), std::string::npos ) << state.LastToast;
    EXPECT_NE( state.LastToast.find( "Subject" ), std::string::npos ) << state.LastToast;

    // And the file that was already there is untouched — the primitive never opens the original.
    EXPECT_EQ( ReadRaw( scene ), R"({"SceneName":"Subject","Version":1})" );

    fs::remove_all( dir );
}

// The control. Without it a rule that answered "failed" to everything would satisfy the test above and
// make saving impossible.
TEST( SceneSaveOutcome, ASuccessfulWriteClearsTheStarAndReleasesTheScene )
{
    const fs::path dir   = MakeTempDir( "desert_scene_save_ok" );
    const fs::path scene = dir / "Subject.desce";

    EditorState state;
    const bool  mayOpenAnother = ApplyVerdictAndOpenAnother( state, SaveSceneText( scene, "{\"new\":true}" ) );

    EXPECT_TRUE( mayOpenAnother );
    EXPECT_FALSE( state.SceneInMemory ) << "the pending open never ran after a save that did land";
    EXPECT_FALSE( state.ShowsUnsavedStar() );
    EXPECT_FALSE( state.LastToastWasError );
    EXPECT_NE( state.LastToast.find( "Subject" ), std::string::npos ) << state.LastToast;
    EXPECT_EQ( ReadRaw( scene ), "{\"new\":true}" );

    fs::remove_all( dir );
}

// Ctrl+S twice over a failure: the second attempt must still be able to save. The mark is the input to
// the next save, so a rule that cleared it on failure would make the FIRST failure permanent — the
// editor would consider the scene clean and never write it again.
TEST( SceneSaveOutcome, ARetryAfterAFailureStillSaves )
{
    const fs::path dir   = MakeTempDir( "desert_scene_save_retry" );
    const fs::path scene = dir / "Subject.desce";

    fs::path temp = scene;
    temp += ".tmp";
    fs::create_directories( temp );

    EditorState state;
    ApplyVerdictAndOpenAnother( state, SaveSceneText( scene, "{\"first\":true}" ) );
    ASSERT_TRUE( state.ShowsUnsavedStar() );

    fs::remove_all( temp ); // whatever was holding the path is gone; the user presses Ctrl+S again
    ApplyVerdictAndOpenAnother( state, SaveSceneText( scene, "{\"second\":true}" ) );

    EXPECT_FALSE( state.ShowsUnsavedStar() );
    EXPECT_FALSE( state.LastToastWasError );
    EXPECT_EQ( ReadRaw( scene ), "{\"second\":true}" );

    fs::remove_all( dir );
}

// The two permissions are read by different call sites (Ctrl+S reads the first, "Save and Open" reads
// the second) and must never disagree about a failure. This is the relation the four hand-written
// copies of the policy could not state.
TEST( SceneSaveOutcome, NeitherPermissionIsEverGrantedByAFailure )
{
    for ( const char* reason : { "could not open the temporary file", "could not write 42 bytes",
                                 "could not rename over the original" } )
    {
        const auto verdict = DecideAfterSceneSave( Common::MakeError( reason ), "Subject", "Scene/Subject.desce" );
        EXPECT_FALSE( verdict.MarkSceneSaved ) << reason;
        EXPECT_FALSE( verdict.MayDiscardScene ) << reason;
        EXPECT_TRUE( verdict.IsError ) << reason;
        EXPECT_NE( verdict.Message.find( reason ), std::string::npos )
             << "the reason the save chain gave did not reach the user: " << verdict.Message;
    }
}

// ── WHERE THE SAVE GOES ────────────────────────────────────────────────────────────────────────────
//
// Reproduced before it was fixed: U52_LockProbe.desce was open, Ctrl+S produced U52_Lock_Probe.desce,
// and the original was byte-identical to what it had been. The destination was derived from the scene's
// NAME, so the two disagreed the moment a name carried a space the file name did not — and every
// symptom was invisible: the write succeeded, the toast was green, the star went out. It surfaced a
// session later as "my level lost a day's work", with the day's work sitting one filename away.
//
// The two sides that must agree are the file a scene was OPENED from and the file a save WRITES. So
// that is what is asserted, on the case where they can differ.
TEST( SceneSaveOutcome, ASaveGoesToTheFileTheSceneWasOpenedFromEvenWhenTheNameDisagrees )
{
    using Desert::Editor::Core::Rules::SceneSaveDestination;

    // The reproduction, exactly: the open file and the scene name differ by a separator.
    EXPECT_EQ( SceneSaveDestination( "Scene/U52_LockProbe.desce", "U52 Lock Probe", "Scene", ".desce" ),
               "Scene/U52_LockProbe.desce" )
         << "the save went somewhere other than the file that is open — this is the defect itself";

    // And it is not merely tolerant of a space: the name may be anything at all, including the name of
    // ANOTHER scene, and it still does not decide where the bytes go.
    EXPECT_EQ( SceneSaveDestination( "Scene/Level_01.desce", "Clouds_Protocol", "Scene", ".desce" ),
               "Scene/Level_01.desce" );

    // A subdirectory survives too. The old derivation always wrote into Scene/ flat, so saving a scene
    // opened from Scene/Chapter2/ moved it up a level as well as renaming it.
    EXPECT_EQ( SceneSaveDestination( "Scene/Chapter2/Boss.desce", "Boss Arena", "Scene", ".desce" ),
               "Scene/Chapter2/Boss.desce" );
}

// The one case where a name may still name a file: there is no file. This is the whole of what remains
// of the derivation, and it has to keep working — File -> New Scene, Ctrl+S is a path a user takes.
TEST( SceneSaveOutcome, ASceneThatHasNeverBeenOnDiskIsNamedAfterItself )
{
    using Desert::Editor::Core::Rules::SceneSaveDestination;

    EXPECT_EQ( SceneSaveDestination( "", "New Scene", "Scene", ".desce" ), "Scene/New_Scene.desce" );
    EXPECT_EQ( SceneSaveDestination( "", "Boss Arena", "Scene/", ".desce" ), "Scene/Boss_Arena.desce" );
}

// SAVE AS IS A NEW ASSET. The GUID in a .desce header is the scene's identity; a copy written under a
// new path that kept it would leave two files claiming one asset.
TEST( SceneSaveIdentity, TheFileItCameFromKeepsItsGuid )
{
    EXPECT_TRUE( Desert::Editor::Core::Rules::SaveKeepsAssetIdentity( "Assets/Scenes/Level.desce",
                                                                      "Assets/Scenes/Level.desce" ) );
    // The same file spelled differently is still the same file.
    EXPECT_TRUE( Desert::Editor::Core::Rules::SaveKeepsAssetIdentity( "Assets/Scenes/Level.desce",
                                                                      "Assets/Scenes/./Sub/../Level.desce" ) );
}

TEST( SceneSaveIdentity, ANewPathIsANewAsset )
{
    EXPECT_FALSE( Desert::Editor::Core::Rules::SaveKeepsAssetIdentity( "Assets/Scenes/Level.desce",
                                                                       "Assets/Scenes/Level_Copy.desce" ) );
    EXPECT_FALSE( Desert::Editor::Core::Rules::SaveKeepsAssetIdentity( "Assets/Scenes/Level.desce",
                                                                       "Assets/Other/Level.desce" ) );
}

TEST( SceneSaveIdentity, ASceneWithNoFileHasNoIdentityToKeep )
{
    EXPECT_FALSE( Desert::Editor::Core::Rules::SaveKeepsAssetIdentity( "", "Assets/Scenes/Starter.desce" ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
