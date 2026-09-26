// SAVING SETTINGS MUST NOT CHANGE A FIELD THE USER DID NOT TOUCH IN THAT ACTION.
//
// The statement is deliberately wider than the defect that produced it (К6), because the defect was a
// SHAPE and not a typo: one value, two stores, and writers that disagreed about which one was the
// authority. In the exact form it shipped:
//
//   * the four gizmo snap values existed twice — as fields of EditorPreferences (which is what
//     ~/.desertengine/editor.json holds) and as private statics of Core::GizmoState;
//   * the toolbar magnet and the viewport's snap popup wrote the GizmoState copy and never called
//     Save(), so a chosen step did not survive a restart;
//   * EditorPreferences::Save() pushed the OTHER copy back over it as its first statement, so any
//     unrelated save — the View menu's Perf HUD toggle, an MSAA pick in Scene Settings, a star on a
//     field in Details — silently reverted the step in the middle of a session, with no message.
//
// The fix is not a fourth Save() call. The second store is gone: EditorPreferences owns the four
// fields, GizmoState reads and writes them and keeps nothing, and Save() has no state to apply. These
// tests hold that, and the first three of them are red on the tree that shipped the defect.
//
// WHY THE ASSERTIONS ARE ABOUT A RELATION rather than about either side (desert-engine-verify §4): both
// sides were individually correct. GizmoState returned exactly what had last been written to it, and
// Save() wrote exactly what the struct held. A unit test of either passes. What was wrong was that they
// had to agree and nothing made them.
//
// THE SUITE WRITES A REAL editor.json, in a REAL home directory — see main(), which points HOME at a
// temporary one first. That is the point: half of what is asserted here is about what reaches the file
// and comes back, and a mock of the store cannot answer that.
//
// ---------------------------------------------------------------------------------------------------
// К8 EXTENDED IT WITH THE OTHER HALF OF THE SAME SHAPE, and the two are worth stating side by side:
//
//   К6: one VALUE had two owners  — the snap step lived in EditorPreferences and in GizmoState, and the
//       two writers disagreed about which was the authority.
//   К8: one DECISION had two takers — the Preferences window's "Save" button claimed to decide whether
//       an edit was kept, but every control in that window edits the LIVE store, so the edit was already
//       in force before the button was reached. Closing the window with the x left the change working
//       and unwritten ("I cancelled" — it did not); and any unrelated save from another panel (the Perf
//       HUD toggle, an MSAA pick, a star in Details) silently committed those abandoned edits to disk.
//       The button promised a decision it did not take, and a stranger in another panel took it.
//
// The button is gone. Every control commits itself on ImGui::IsItemDeactivatedAfterEdit() — one file
// write when the mouse is released — and Save() skips a write whose bytes match what is already on disk,
// which is what makes committing on every control affordable.
//
// TWO KINDS OF ASSERTION FOLLOW, and neither covers the other. Sections 1-4 link the real store and ask
// it questions. Section 5 READS EditorLayer::DrawPreferencesWindow's source text, because the half of
// the statement that says "exactly when the user let go" lives in a function this suite cannot link (it
// needs ImGui, the renderer and the whole editor) and is not observable from the store at all — a store
// cannot tell whether the call that reached it came from a control's release or from a button. Reading
// the source for a census has precedent here: Desert/Tests/Engine/DeviceLostCensus does the same thing
// for the same reason, and shares the reader this file includes.

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Core/GizmoState.hpp>
#include <Editor/Core/ViewportModes.hpp>

// §8 opens a real project: which project a pin belongs to, and what it is relative to, are both answers
// this gives — so a test of the pinning helpers that mocked it would be testing nothing they do.
#include <Engine/Project/ProjectContext.hpp>

// glm::vec3 <-> JSON reflector (OutlineColor). Must be visible before rfl::json, exactly as it must be
// in EditorPreferences.cpp — without it the whole struct is "Unsupported type" at the first vec3.
#include <Common/Core/Serialization/GlmReflection.hpp>

// The shared source reader (Д33): comments and literals blanked, length and line breaks preserved. §5
// needs it for the same reason DeviceLostCensus does — a census that counted the calls named in comments
// would certify prose, and DrawPreferencesWindow is now mostly prose.
#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <rflcpp/rfl/Generic.hpp>
#include <rflcpp/rfl/fields.hpp>
#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using Desert::Editor::EditorPreferences;
using Desert::Editor::Core::ApplyViewportModes;
using Desert::Editor::Core::ViewportModes;
using Gizmo = Desert::Editor::Core::GizmoState;

namespace
{
    std::string PrefsPath()
    {
        return EditorPreferences::ConfigDirectory() + "/editor.json";
    }

    std::string ReadWholeFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    void WriteWholeFile( const std::filesystem::path& path, const std::string& text )
    {
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << text;
    }

    // A freshly installed editor: nothing on disk, defaults in memory. Every test starts here so none
    // of them can pass on a value another one happened to leave behind.
    //
    // THE Load() CALL IS LOAD-BEARING and was added by К8. The store remembers what it believes is on
    // disk so that it can skip a write that would change nothing, and that memo is process-wide — it
    // outlives a test the way the file does not. Load() with no file present is how a real first run
    // tells the store the disk is empty, so it is also how a test does: without it, a test whose values
    // happened to match the previous test's would find its first save skipped.
    void FreshInstall()
    {
        std::error_code ec;
        std::filesystem::remove( PrefsPath(), ec );
        EditorPreferences::Get() = EditorPreferences{};
        EditorPreferences::Load();
    }

    // Every top-level key of one editor.json snapshot, in the order the file states them.
    std::vector<std::string> KeysOf( const std::string& json )
    {
        const auto parsed = Common::Json::Parse( json );
        EXPECT_TRUE( parsed.IsSuccess() ) << "not readable JSON: " << json;
        if ( !parsed.IsSuccess() )
            return { "<unreadable>" };

        const Common::Json::Node root = Common::Json::Root( parsed.GetValue() );
        EXPECT_EQ( root.GetKind(), Common::Json::Kind::Object ) << "not a JSON object: " << json;
        if ( root.GetKind() != Common::Json::Kind::Object )
            return { "<not-an-object>" };

        std::vector<std::string> keys;
        root.ForEachMember( [&]( std::string_view name, const Common::Json::Node& )
                            { keys.emplace_back( name ); } );
        return keys;
    }

    // The value of one key, as text, or "<missing>". Compared as text on purpose: it is the only form in
    // which "unchanged" is checkable for a key whose TYPE this build has no idea about.
    std::string ValueOf( const std::string& json, const std::string& key )
    {
        const auto parsed = Common::Json::Parse( json );
        if ( !parsed.IsSuccess() )
            return "<unreadable>";
        const Common::Json::Node root = Common::Json::Root( parsed.GetValue() );
        if ( root.GetKind() != Common::Json::Kind::Object )
            return "<not-an-object>";
        const auto value = root.Find( key );
        if ( !value.has_value() )
            return "<missing>";
        return Common::Json::Write( value->Raw() );
    }

    // Which keys of editor.json differ between two snapshots of it.
    //
    // THE KEY LIST IS THE UNION OF WHAT THE TWO TEXTS CONTAIN, and it used to be
    // rfl::fields<EditorPreferences>(). Both are derived rather than typed, so both are field-count-proof;
    // what changed with К9 is that the struct's fields stopped being the whole of the file's keys. A key
    // another build owns is a key of this file, and a helper that asked rfl::fields<> about it would be
    // structurally unable to see the very thing these tests are about — the same "the container is derived
    // from the same source as the question" trap the contract's §1.4 names. EditorPreferences::ChangedFields
    // was moved to the union for the same reason, and its log line depends on it.
    std::vector<std::string> FieldsThatDiffer( const std::string& before, const std::string& after )
    {
        std::vector<std::string> keys = KeysOf( before );
        for ( const std::string& key : KeysOf( after ) )
            if ( std::find( keys.begin(), keys.end(), key ) == keys.end() )
                keys.push_back( key );

        std::vector<std::string> differing;
        for ( const std::string& key : keys )
        {
            const std::string a = ValueOf( before, key );
            const std::string b = ValueOf( after, key );
            if ( a == "<missing>" || b == "<missing>" )
            {
                differing.push_back( key + " <missing>" );
                continue;
            }
            if ( a != b )
                differing.push_back( key );
        }
        return differing;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. ONE VALUE, ONE STORAGE
// ---------------------------------------------------------------------------------------------------

// The relation itself, asserted in BOTH directions, because a one-way push is exactly what the broken
// version had: Save() copied preferences -> GizmoState and nothing ever came back.
TEST( PreferenceOwnership, TheGizmoAndThePreferencesCannotHoldDifferentSnapValues )
{
    FreshInstall();

    // Written the way a toolbar writes it.
    Gizmo::SetTranslateSnap( 25.0f );
    Gizmo::SetRotateSnapDegrees( 45.0f );
    Gizmo::SetScaleSnap( 0.25f );
    Gizmo::SetPersistentSnap( true );

    EXPECT_FLOAT_EQ( EditorPreferences::Get().TranslateSnap, 25.0f );
    EXPECT_FLOAT_EQ( EditorPreferences::Get().RotateSnapDeg, 45.0f );
    EXPECT_FLOAT_EQ( EditorPreferences::Get().ScaleSnap, 0.25f );
    EXPECT_TRUE( EditorPreferences::Get().PersistentSnap );

    // Written the way the Preferences window writes it — straight into the owning fields.
    EditorPreferences::Get().TranslateSnap  = 10.0f;
    EditorPreferences::Get().RotateSnapDeg  = 90.0f;
    EditorPreferences::Get().ScaleSnap      = 0.5f;
    EditorPreferences::Get().PersistentSnap = false;

    EXPECT_FLOAT_EQ( Gizmo::TranslateSnap(), 10.0f );
    EXPECT_FLOAT_EQ( Gizmo::RotateSnapDegrees(), 90.0f );
    EXPECT_FLOAT_EQ( Gizmo::ScaleSnap(), 0.5f );
    EXPECT_FALSE( Gizmo::PersistentSnap() );
}

// SnapActive is the one piece of policy GizmoState still owns, and it reads the toggle it no longer
// stores. Ctrl INVERTS the toggle rather than forcing snapping on, which is the part that a rewrite of
// the accessor would be most likely to get backwards.
TEST( PreferenceOwnership, CtrlInvertsThePersistentToggleWhicheverWayItIsSet )
{
    FreshInstall();

    Gizmo::SetPersistentSnap( false );
    EXPECT_FALSE( Gizmo::SnapActive( false ) );
    EXPECT_TRUE( Gizmo::SnapActive( true ) );

    Gizmo::SetPersistentSnap( true );
    EXPECT_TRUE( Gizmo::SnapActive( false ) );
    EXPECT_FALSE( Gizmo::SnapActive( true ) );
}

// ---------------------------------------------------------------------------------------------------
// 2. THE HEADLINE RELATION
// ---------------------------------------------------------------------------------------------------

// The user's own sentence: set a snap step, then do something unrelated in another panel, and the step
// is still what you set. The unrelated action is the View menu's Perf HUD item, copied verbatim from
// EditorLayer::DrawViewMenu — one bool and a Save() — because that is the cheapest real trigger and
// nothing about it mentions the gizmo.
TEST( PreferenceOwnership, AnUnrelatedSaveChangesNoFieldTheUserDidNotTouch )
{
    FreshInstall();

    Gizmo::SetTranslateSnap( 25.0f );
    Gizmo::SetRotateSnapDegrees( 45.0f );
    Gizmo::SetScaleSnap( 0.25f );
    Gizmo::SetPersistentSnap( true );

    const std::string before = Common::Json::Write( EditorPreferences::Get() );

    EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
    EditorPreferences::Save();

    const std::vector<std::string> expected = { "ShowPerfHud" };
    EXPECT_EQ( FieldsThatDiffer( before, Common::Json::Write( EditorPreferences::Get() ) ), expected )
         << "a save moved a field the action that triggered it never mentioned";

    // And the same statement asked of the thing the user can actually see — the step the gizmo snaps
    // by. This is the assertion the broken tree fails: the struct above was never touched by the old
    // Save() either; what it reverted was the SECOND copy, which is what the gizmo read.
    EXPECT_FLOAT_EQ( Gizmo::TranslateSnap(), 25.0f );
    EXPECT_FLOAT_EQ( Gizmo::RotateSnapDegrees(), 45.0f );
    EXPECT_FLOAT_EQ( Gizmo::ScaleSnap(), 0.25f );
    EXPECT_TRUE( Gizmo::PersistentSnap() );
}

// The same sentence with another real trigger, and one that is a preference in its own right: the Perf
// HUD item in the View menu writes prefs.ShowPerfHud and saves.
//
// IT USED TO BE THE MSAA COMBO, which was worth its own case because MSAA was the one value Save() still
// pushed anywhere (into Graphic::RenderConfig) — so if a push were ever going to leak back into a
// neighbour, that was the action that would do it. К3 moved the field out of this file entirely
// (Common::Settings::MachineSettings), and with it the last push: Save() now writes the file and touches
// nothing else at all, which is a strictly stronger version of what this case was defending. The trigger
// is replaced rather than the case deleted, because what is being asserted is about SAVING, not about
// MSAA.
TEST( PreferenceOwnership, TogglingThePerfHudDoesNotDisturbTheSnapStep )
{
    FreshInstall();

    Gizmo::SetTranslateSnap( 500.0f );
    const std::string before = Common::Json::Write( EditorPreferences::Get() );

    EditorPreferences::Get().ShowPerfHud = true;
    EditorPreferences::Save();

    const std::vector<std::string> expected = { "ShowPerfHud" };
    EXPECT_EQ( FieldsThatDiffer( before, Common::Json::Write( EditorPreferences::Get() ) ), expected );
    EXPECT_FLOAT_EQ( Gizmo::TranslateSnap(), 500.0f );
}

// The general form, stated over the whole struct so that a preference added tomorrow is covered without
// anyone remembering this file. Save() is allowed to write the file and nothing else; if it ever grows
// a second "apply" statement, this is what refuses it.
//
// The values are moved off their defaults on purpose: a struct left at its defaults cannot tell a
// harmless no-op from a reset, which is precisely how the original push went unnoticed.
TEST( PreferenceOwnership, SaveRewritesNothingInTheStructItWrites )
{
    FreshInstall();

    EditorPreferences::Get().TranslateSnap  = 7.0f;
    EditorPreferences::Get().RotateSnapDeg  = 3.0f;
    EditorPreferences::Get().ScaleSnap      = 0.75f;
    EditorPreferences::Get().PersistentSnap = true;
    EditorPreferences::Get().CameraSpeed    = 4.25f;
    EditorPreferences::Get().ShowPerfHud    = true;

    const std::string before = Common::Json::Write( EditorPreferences::Get() );
    EditorPreferences::Save();

    EXPECT_EQ( FieldsThatDiffer( before, Common::Json::Write( EditorPreferences::Get() ) ),
               std::vector<std::string>{} );
}

// ---------------------------------------------------------------------------------------------------
// 3. THE CHOICE REACHES THE FILE
// ---------------------------------------------------------------------------------------------------

// The small half of the defect: a step picked from the toolbar was live for the session and gone on the
// next launch, because the writer never reached the store. A restart is simulated exactly the way one
// happens — the process's copy is discarded and rebuilt by Load() from what is on disk.
TEST( PreferenceOwnership, AStepChosenFromAToolbarSurvivesARestart )
{
    FreshInstall();

    Gizmo::SetTranslateSnap( 25.0f );
    Gizmo::SetRotateSnapDegrees( 30.0f );
    Gizmo::SetScaleSnap( 0.5f );
    Gizmo::SetPersistentSnap( true );

    ASSERT_TRUE( std::filesystem::exists( PrefsPath() ) )
         << "no toolbar write reached " << PrefsPath() << " at all";

    EditorPreferences::Get() = EditorPreferences{}; // the next launch starts from the struct's defaults
    EditorPreferences::Load();

    EXPECT_FLOAT_EQ( Gizmo::TranslateSnap(), 25.0f );
    EXPECT_FLOAT_EQ( Gizmo::RotateSnapDegrees(), 30.0f );
    EXPECT_FLOAT_EQ( Gizmo::ScaleSnap(), 0.5f );
    EXPECT_TRUE( Gizmo::PersistentSnap() );
}

// Persisting on the click is only affordable because re-picking the step you are already on writes
// nothing. ImGui::Selectable reports a click whether or not it changed anything, and the magnet popup
// is a list of seven steps with the current one highlighted — so the commonest interaction with it is
// picking the value that is already set.
//
// THE PROBE IS A MARKER, NOT A DELETION, and К8 changed it for a reason worth keeping: the old version
// removed editor.json and asserted it did not come back, which stopped being the right question once the
// store learned to confirm the file is still there before skipping a write. Deleting the file behind a
// running editor SHOULD produce a write — a "true" that meant "my memo says so" while nothing was on
// disk would be §1.4's silent wrong answer. Overwriting the file with a marker asks the question the
// test is actually about: does a redundant save touch the bytes?
//
// К9 MOVED THE MARKER ON FOR THE SAME KIND OF REASON, one step further along. It used to be a whole
// foreign document — `{ "this file was not rewritten": true }` — which was fine while the store ignored
// the disk entirely. It no longer does: a save now re-reads the file so that a key another build wrote
// after this one started is preserved rather than deleted, and a save that finds a document full of keys
// it has never seen SHOULD write, because merging them is the thing К9 built. So the marker is now
// whitespace: byte-detectable, semantically nothing, and therefore a probe for a rewrite rather than for
// the store's opinion of a stranger's file. (`AFinishedEditReachesTheFileOnceAndARedundantSaveNotAtAll`
// below already used this probe, which is how it survived the change untouched.)
TEST( PreferenceOwnership, ReChoosingTheStepAlreadySetDoesNotRewriteTheFile )
{
    FreshInstall();

    Gizmo::SetTranslateSnap( 25.0f );
    ASSERT_TRUE( std::filesystem::exists( PrefsPath() ) );

    const std::string marker = ReadWholeFile( PrefsPath() ) + "\n";
    WriteWholeFile( PrefsPath(), marker );

    Gizmo::SetTranslateSnap( 25.0f );  // the same step, picked again
    Gizmo::SetPersistentSnap( false ); // the toggle, set to what it already is

    EXPECT_EQ( ReadWholeFile( PrefsPath() ), marker )
         << "a choice that changed nothing rewrote editor.json (and logged a save that did nothing)";

    // ...and a real change still writes.
    Gizmo::SetTranslateSnap( 50.0f );
    EXPECT_NE( ReadWholeFile( PrefsPath() ), marker );
}

// ---------------------------------------------------------------------------------------------------
// 4. THE ONE MIGRATION THIS FILE CARRIES
// ---------------------------------------------------------------------------------------------------

// Load() raises a metre-era TranslateSnap to centimetres once and writes it back, and until now nothing
// tested it — which matters more than it looks, because it is the only code in the editor that can
// change a preference the user did not touch in that session, and it is therefore the one legitimate
// exception to this suite's headline. It earns the exception by being a MIGRATION: it fires on a value
// this build cannot produce, it says so in the log, and it persists the new form so it never fires
// again. Contract §4.4 asks a migration to be tested; this is that test.
TEST( PreferenceOwnership, AMetreEraSnapIsRaisedToCentimetresOnceAndWrittenBack )
{
    FreshInstall();

    // 0.5 "world units" from when a unit was a metre — half a metre, meant as 50 cm.
    EditorPreferences::Get().TranslateSnap = 0.5f;
    ASSERT_TRUE( EditorPreferences::Save() );

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EXPECT_FLOAT_EQ( Gizmo::TranslateSnap(), 50.0f );

    // Written back in the new form, so the SECOND launch reads 50 and the migration does not fire on it
    // again — a migration that ran every launch would multiply by a hundred every time.
    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EXPECT_FLOAT_EQ( Gizmo::TranslateSnap(), 50.0f );
}

// The other half, and the one that says the rule is not "anything small is metres": 1 cm is the
// smallest step the editor offers, so the migration's window is strictly below it and a legitimate
// centimetre value must pass through untouched.
TEST( PreferenceOwnership, ACentimetreEraSnapIsLeftAlone )
{
    FreshInstall();

    EditorPreferences::Get().TranslateSnap = 1.0f;
    ASSERT_TRUE( EditorPreferences::Save() );

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EXPECT_FLOAT_EQ( Gizmo::TranslateSnap(), 1.0f );
}

// ---------------------------------------------------------------------------------------------------
// 5. К8 — ONE WRITE PER FINISHED EDIT, AND NOT ONE MORE
// ---------------------------------------------------------------------------------------------------

// The user's sentence, in the half a linked test can answer: a finished edit reaches the file once, and
// a save with nothing new in it does not touch the file at all.
//
// This is red on the tree that shipped the defect in the second direction — Save() wrote and logged
// unconditionally, so every redundant call was a file write and a "[Prefs] Saved ..." line. That was
// affordable while one button was the only writer in this window; it is not affordable now that nine
// controls each commit on release and three other panels commit on a click.
//
// The 60 frames are the drag itself: a DragFloat accumulates into the value it is handed and reports a
// change on every frame the mouse moves, which is why the commit waits for the release instead of riding
// the change. That the release is where the window calls Save() is §6's business, not this test's — a
// store cannot see which call site reached it.
TEST( PreferenceOwnership, AFinishedEditReachesTheFileOnceAndARedundantSaveNotAtAll )
{
    FreshInstall();

    EditorPreferences::Get().OutlineWidth = 4.0f;
    ASSERT_TRUE( EditorPreferences::Save() );
    const std::string beforeDrag = ReadWholeFile( PrefsPath() );

    for ( int frame = 0; frame < 60; ++frame )
        EditorPreferences::Get().OutlineWidth = 4.0f + 0.1f * static_cast<float>( frame );

    ASSERT_TRUE( EditorPreferences::Save() ); // the release
    const std::string afterDrag = ReadWholeFile( PrefsPath() );

    const std::vector<std::string> expected = { "OutlineWidth" };
    EXPECT_EQ( FieldsThatDiffer( beforeDrag, afterDrag ), expected )
         << "the release wrote more than the control the user was holding";

    // A second release with nothing new — letting go of a control you only hovered, or putting a handle
    // back where you found it. A marker appended to the file survives iff no write happened.
    const std::string marked = afterDrag + "\n";
    WriteWholeFile( PrefsPath(), marked );
    EXPECT_TRUE( EditorPreferences::Save() );
    EXPECT_EQ( ReadWholeFile( PrefsPath() ), marked ) << "a save with nothing new in it rewrote editor.json";
}

// The same statement asked of the FILE rather than of the struct, which is where К8's second symptom
// lived: a stranger's save is what carried the abandoned edits to disk, so what the file gains when an
// unrelated panel saves is the thing to pin. `AnUnrelatedSaveChangesNoFieldTheUserDidNotTouch` above
// asks this of the in-memory struct and cannot see a write at all.
TEST( PreferenceOwnership, TheFileGainsOnlyTheFieldTheSavingActionTouched )
{
    FreshInstall();

    // A Preferences control, committed by its own release the way every control in that window now is.
    EditorPreferences::Get().CameraSpeed = 6.0f;
    ASSERT_TRUE( EditorPreferences::Save() );
    const std::string before = ReadWholeFile( PrefsPath() );

    // Somebody toggles the Perf HUD from the View menu — a different panel, one bool, one save.
    EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
    ASSERT_TRUE( EditorPreferences::Save() );

    const std::vector<std::string> expected = { "ShowPerfHud" };
    EXPECT_EQ( FieldsThatDiffer( before, ReadWholeFile( PrefsPath() ) ), expected );
}

// A save that could not be written must not leave the store believing the disk agrees with it — the memo
// that lets a redundant save be skipped is only safe while it is updated on success alone. Provoked the
// one way that needs no privileges: a DIRECTORY sitting where editor.json's temporary file must go, so
// the atomic write's rename cannot succeed.
TEST( PreferenceOwnership, AFailedWriteIsNotRememberedAsASuccessfulOne )
{
    FreshInstall();

    EditorPreferences::Get().CameraSpeed = 2.5f;
    ASSERT_TRUE( EditorPreferences::Save() );

    std::error_code             ec;
    const std::filesystem::path blocker = PrefsPath() + ".tmp";
    std::filesystem::create_directory( blocker, ec );
    ASSERT_TRUE( std::filesystem::is_directory( blocker ) ) << "could not stage a blocked write";

    EditorPreferences::Get().CameraSpeed = 9.0f;
    EXPECT_FALSE( EditorPreferences::Save() ) << "a write that could not happen reported success";

    std::filesystem::remove_all( blocker, ec );

    // The retry must actually write. If the failed attempt had updated the memo, this save would be
    // skipped as redundant and the setting would be lost with nothing in the log to say so.
    ASSERT_TRUE( EditorPreferences::Save() );
    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EXPECT_FLOAT_EQ( EditorPreferences::Get().CameraSpeed, 9.0f );
}

// Load() writes in exactly one case — a migration — and the migration itself is a pure function of the
// struct (contract §4.4). Asserted directly, with no file anywhere near it, which is the whole point of
// extracting it: the old code did the raise inline in Load() and called Save() from it, so the only way
// to test the arithmetic was to write and re-read a file.
TEST( PreferenceOwnership, TheMigrationIsAPureFunctionAndNamesWhatItRaised )
{
    EditorPreferences metreEra;
    metreEra.TranslateSnap = 0.5f;

    const auto raised = EditorPreferences::MigrateLoaded( metreEra );
    ASSERT_EQ( raised.size(), 1u );
    EXPECT_NE( raised.front().find( "TranslateSnap" ), std::string::npos )
         << "a migration line that does not name the field it moved: " << raised.front();
    EXPECT_FLOAT_EQ( metreEra.TranslateSnap, 50.0f );

    // Idempotent: run on its own output it has nothing left to do. A migration that fired twice would
    // multiply by a hundred every launch.
    EXPECT_TRUE( EditorPreferences::MigrateLoaded( metreEra ).empty() );

    EditorPreferences centimetreEra;
    centimetreEra.TranslateSnap = 1.0f;
    EXPECT_TRUE( EditorPreferences::MigrateLoaded( centimetreEra ).empty() );
    EXPECT_FLOAT_EQ( centimetreEra.TranslateSnap, 1.0f );
}

// ---------------------------------------------------------------------------------------------------
// 6. К8 — THE CENSUS OF THE PREFERENCES WINDOW ITSELF
// ---------------------------------------------------------------------------------------------------
//
// "Exactly when the user let go of the mouse" is a statement about CALL SITES, and no amount of asking
// the store can answer it: the store sees a Save() and cannot tell a control's release from a button.
// So this section reads EditorLayer::DrawPreferencesWindow and asserts three things about its text —
// that every control in it commits, that nothing in it saves outside a commit (which is how the "Save"
// button is kept dead), and that every control really is bound to a field of the preference struct.
//
// It is red on the tree that shipped the defect in the first direction: nine controls, zero commits, one
// button. It is deliberately a census rather than a spot check, because the failure mode of nine
// copy-pasted guards is that the tenth control arrives without one — a middle link that silently drops a
// property, which is the recurring defect shape in this project and the reason the rule is enforced over
// the whole function instead of trusted per line.

namespace
{
    namespace Text = Desert::Tests::ConsumerText;

    // Walks up from the working directory looking for a file only the repository has. Same shape as
    // DeviceLostCensus's and AssetReferenceCensus's, and copied for the same reason they are copies of
    // each other: each census probes a DIFFERENT sentinel, so sharing it would mean passing the sentinel
    // in and would say less than the three lines it replaced.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/EditorLayer.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // The body of `Class::Method`, by brace matching from the first '{' after the name. Empty when the
    // name is not there at all, which the callers report as a distinct failure: a census naming a
    // function that no longer exists is worse than a missing guard, because it passes.
    std::string BodyOf( const std::string& src, const std::string& qualifiedName, std::size_t& bodyStart )
    {
        std::size_t at = 0;
        while ( ( at = src.find( qualifiedName, at ) ) != std::string::npos )
        {
            const std::size_t open = src.find( '{', at );
            if ( open == std::string::npos )
                return {};
            const std::size_t semi = src.find( ';', at );
            if ( semi != std::string::npos && semi < open )
            {
                at += qualifiedName.size();
                continue;
            }
            int         depth = 0;
            std::size_t i     = open;
            for ( ; i < src.size(); ++i )
            {
                if ( src[i] == '{' )
                    ++depth;
                else if ( src[i] == '}' && --depth == 0 )
                    break;
            }
            bodyStart = open;
            return src.substr( open, i - open );
        }
        return {};
    }

    // The window's source, with the line it starts on, so that a failure can say `EditorLayer.cpp:4592`
    // instead of a byte offset into a substring nobody can navigate to. The reader preserves every
    // newline and the file's length, which is what makes the arithmetic exact.
    struct WindowSource
    {
        std::string Body;
        std::size_t FirstLine = 0;

        std::size_t LineOf( std::size_t at ) const
        {
            return FirstLine + static_cast<std::size_t>( std::count( Body.begin(), Body.begin() + at, '\n' ) );
        }
    };

    WindowSource PreferencesWindow()
    {
        const std::string root = RepoRoot();
        EXPECT_FALSE( root.empty() ) << "could not find the repository root from the working directory";

        const std::string src =
             Text::StripCommentsAndLiterals( ReadWholeFile( root + "Editor/Source/EditorLayer.cpp" ) );

        std::size_t  at = 0;
        WindowSource out;
        out.Body = BodyOf( src, "EditorLayer::DrawPreferencesWindow", at );
        if ( !out.Body.empty() )
            out.FirstLine = 1 + static_cast<std::size_t>( std::count( src.begin(), src.begin() + at, '\n' ) );
        return out;
    }

    // EVERY ImGui CALL THE WINDOW MAKES, split by whether it can edit a preference. The lists are the
    // census: a call in neither of them fails the suite with "classify this", which is what stops a new
    // control from being added without the question being asked. `Button` is deliberately in NEITHER —
    // the button that took this task's decision away is not a call this window is allowed to make again
    // without somebody arguing for it here first.
    const std::vector<std::string>& ControlCalls()
    {
        static const std::vector<std::string> calls = {
             "SliderFloat", "SliderFloat2", "SliderFloat3", "SliderInt",   "SliderAngle", "DragFloat",
             "DragFloat2",  "DragFloat3",   "DragInt",      "Checkbox",    "ColorEdit3",  "ColorEdit4",
             "InputText",   "InputInt",     "InputFloat",   "InputFloat3", "Combo",       "RadioButton" };
        return calls;
    }

    const std::vector<std::string>& InertCalls()
    {
        static const std::vector<std::string> calls = { "Begin",
                                                        "End",
                                                        "SetNextWindowSize",
                                                        "SetNextItemWidth",
                                                        "Spacing",
                                                        "Separator",
                                                        "SameLine",
                                                        "NewLine",
                                                        "Dummy",
                                                        "Text",
                                                        "TextUnformatted",
                                                        "TextDisabled",
                                                        "PushID",
                                                        "PopID",
                                                        "Indent",
                                                        "Unindent",
                                                        "BeginDisabled",
                                                        "EndDisabled",
                                                        "IsItemHovered",
                                                        "SetTooltip",
                                                        "IsItemDeactivatedAfterEdit",
                                                        "PushItemWidth",
                                                        "PopItemWidth",
                                                        "TextWrapped",
                                                        "PushTextWrapPos",
                                                        "PopTextWrapPos",
                                                        "PushStyleColor",
                                                        "PopStyleColor",
                                                        "GetStyleColorVec4" };
        return calls;
    }

    bool Contains( const std::vector<std::string>& haystack, const std::string& needle )
    {
        return std::find( haystack.begin(), haystack.end(), needle ) != haystack.end();
    }

    // Positions of `ImGui::<name>` for every name in `names`, in source order.
    std::vector<std::size_t> CallPositions( const std::string& body, const std::vector<std::string>& names )
    {
        std::vector<std::size_t> out;
        for ( const std::string& name : names )
            for ( std::size_t at : Text::WordPositions( body, name ) )
            {
                if ( at < 7 || body.compare( at - 7, 7, "ImGui::" ) != 0 )
                    continue;
                out.push_back( at );
            }
        std::sort( out.begin(), out.end() );
        return out;
    }

    // Every `ImGui::<name>` the body contains, whatever the name.
    std::vector<std::string> EveryImGuiCall( const std::string& body )
    {
        std::vector<std::string> out;
        for ( std::size_t at = body.find( "ImGui::" ); at != std::string::npos;
              at             = body.find( "ImGui::", at + 1 ) )
        {
            if ( at > 0 && Text::IsIdentChar( body[at - 1] ) )
                continue;
            const std::string name = Text::IdentAt( body, at + 7 );
            if ( !name.empty() && !Contains( out, name ) )
                out.push_back( name );
        }
        return out;
    }

    // The balanced argument list of the call whose name ends at `afterName`.
    std::string ArgumentsOf( const std::string& body, std::size_t afterName )
    {
        const std::size_t open = body.find( '(', afterName );
        if ( open == std::string::npos )
            return {};
        int         depth = 0;
        std::size_t i     = open;
        for ( ; i < body.size(); ++i )
        {
            if ( body[i] == '(' )
                ++depth;
            else if ( body[i] == ')' && --depth == 0 )
                break;
        }
        return body.substr( open, i - open );
    }
} // namespace

// The gate that keeps the other two honest: a control this file has never heard of is neither covered by
// the commit rule nor visible to the binding rule, so it must stop the suite rather than pass through it.
TEST( PreferenceOwnershipWindow, EveryCallInThePreferencesWindowIsClassified )
{
    const WindowSource window = PreferencesWindow();
    ASSERT_FALSE( window.Body.empty() ) << "EditorLayer::DrawPreferencesWindow was not found in EditorLayer.cpp";
    const std::string& body = window.Body;

    for ( const std::string& name : EveryImGuiCall( body ) )
        EXPECT_TRUE( Contains( ControlCalls(), name ) || Contains( InertCalls(), name ) )
             << "ImGui::" << name
             << " is called in the Preferences window and this census does not know whether it can edit a "
                "preference. Add it to ControlCalls() (and give it a commit) or to InertCalls(). If it is "
                "ImGui::Button, read the comment above DrawPreferencesWindow first: К8 removed the one "
                "this window used to have.";
}

// THE HEADLINE. Every control commits, and it commits on the release rather than on the change.
TEST( PreferenceOwnershipWindow, EveryControlCommitsWhenTheUserLetsGoOfIt )
{
    const WindowSource window = PreferencesWindow();
    ASSERT_FALSE( window.Body.empty() ) << "EditorLayer::DrawPreferencesWindow was not found in EditorLayer.cpp";
    const std::string& body = window.Body;

    const std::vector<std::size_t> controls = CallPositions( body, ControlCalls() );
    ASSERT_FALSE( controls.empty() ) << "the Preferences window draws no control at all";

    const std::vector<std::size_t> commits = Text::WordPositions( body, "IsItemDeactivatedAfterEdit" );

    for ( std::size_t i = 0; i < controls.size(); ++i )
    {
        const std::size_t from = controls[i];
        const std::size_t to   = i + 1 < controls.size() ? controls[i + 1] : body.size();

        bool committed = false;
        for ( std::size_t commit : commits )
            committed = committed || ( commit > from && commit < to );

        EXPECT_TRUE( committed )
             << "EditorLayer.cpp:" << window.LineOf( from ) << " — ImGui::" << Text::IdentAt( body, from )
             << " in the Preferences window is not followed by an ImGui::IsItemDeactivatedAfterEdit() "
                "commit before the next control. An edit made with it is live but never written, which is "
                "exactly the state К8 removed.";
    }
}

// The other half, and the one that keeps the button dead: nothing in this window may write the file
// except a commit. Stated as an interleaving rather than a count, so that a save moved away from its
// guard is caught as readily as a save with no guard at all.
TEST( PreferenceOwnershipWindow, NothingInThePreferencesWindowSavesOutsideACommit )
{
    const WindowSource window = PreferencesWindow();
    ASSERT_FALSE( window.Body.empty() ) << "EditorLayer::DrawPreferencesWindow was not found in EditorLayer.cpp";
    const std::string& body = window.Body;

    const std::vector<std::size_t> commits = Text::WordPositions( body, "IsItemDeactivatedAfterEdit" );
    const std::vector<std::size_t> saves   = Text::WordPositions( body, "Save" );

    ASSERT_EQ( saves.size(), commits.size() )
         << "the Preferences window calls Save() " << saves.size() << " time(s) behind " << commits.size()
         << " commit(s). A save that is not a control's release is a second decider for the same "
            "decision — the shape К8 exists to remove.";

    for ( std::size_t i = 0; i < saves.size(); ++i )
        EXPECT_LT( commits[i], saves[i] ) << "EditorLayer.cpp:" << window.LineOf( saves[i] )
                                          << " — this Save() does not sit behind a commit of its own.";
}

// And every control really edits a PREFERENCE, not a local copy of one. The field names are checked
// against rfl::fields<EditorPreferences>() — the same list Save() serializes — so a field renamed in the
// struct and left behind in the window is caught here rather than by a setting that quietly stops
// persisting.
TEST( PreferenceOwnershipWindow, EveryControlIsBoundToARealPreferenceField )
{
    const WindowSource window = PreferencesWindow();
    ASSERT_FALSE( window.Body.empty() ) << "EditorLayer::DrawPreferencesWindow was not found in EditorLayer.cpp";
    const std::string& body = window.Body;

    // The name the window binds the store to, derived rather than spelled: renaming the local is an
    // honest edit that changes nothing about whether the controls reach the store.
    const std::vector<std::size_t> gets = Text::WordPositions( body, "Get" );
    ASSERT_FALSE( gets.empty() ) << "the Preferences window never reaches EditorPreferences::Get()";
    const std::string store = Text::DeclaredNameBeforeAssignment( Text::StatementBefore( body, gets.front() ) );
    ASSERT_FALSE( store.empty() ) << "could not see what EditorPreferences::Get() is bound to";

    std::vector<std::string> fields;
    for ( const auto& meta : rfl::fields<EditorPreferences>() )
        fields.push_back( std::string( meta.name() ) );

    for ( std::size_t at : CallPositions( body, ControlCalls() ) )
    {
        const std::string args  = ArgumentsOf( body, at );
        int               bound = 0;
        for ( std::size_t use : Text::WordPositions( args, store ) )
        {
            const std::size_t dot = Text::SkipSpace( args, use + store.size() );
            if ( dot >= args.size() || args[dot] != '.' )
                continue;
            const std::string field = Text::IdentAt( args, Text::SkipSpace( args, dot + 1 ) );
            if ( field.empty() )
                continue;
            ++bound;
            EXPECT_TRUE( Contains( fields, field ) )
                 << "EditorLayer.cpp:" << window.LineOf( at ) << " — the Preferences window edits " << store << "."
                 << field << ", which is not a field of EditorPreferences";
        }
        EXPECT_GT( bound, 0 ) << "EditorLayer.cpp:" << window.LineOf( at )
                              << " — ImGui::" << Text::IdentAt( body, at )
                              << " edits nothing in the preference store";
    }
}

// ---------------------------------------------------------------------------------------------------
// 7. К9 — SAVING DOES NOT DELETE A KEY THE WRITER DOES NOT KNOW
// ---------------------------------------------------------------------------------------------------
//
// THE DEFECT, MEASURED ON THE OWNER'S OWN FILE. Every save is `Common::Json::Write( Get() )`: the whole of
// editor.json, rewritten from the struct the running binary was compiled with. Several agents run
// several builds against the one `~/.desertengine/editor.json`, so the build that had not yet grown the
// packaging fields erased `PackageAppBundle`, `PackageConfig` and `PackageOutputDir` — written minutes
// earlier by another build — the first time anybody toggled anything at all. Nothing was wrong with
// either binary: each wrote itself out honestly. The file had to be restored by hand.
//
// IT IS A SHAPE, NOT AN INCIDENT: a container rewritten IN FULL by a writer that knows only PART of what
// it contains. The project has now paid for it twice (П3 was the other, in the opposite direction — a
// rewrite built from what the source still offered, so the file a deletion was about could never be in
// it). The relation these tests assert is the general one, because the specific keys will be different
// next time:
//
//     LOADING AND SAVING editor.json PRESERVES EVERY KEY, INCLUDING THE ONES THIS BUILD CANNOT NAME.
//
// They cannot be written with the historical keys — `PackageAppBundle` is a field of this build, so this
// build is exactly the wrong witness for it. The keys below are invented for that reason, and the shapes
// are chosen to be the ones a hand-rolled "copy the leftovers" implementation gets wrong: a nested
// object, an array, a null, a fraction.
//
// RED ON THE TREE THIS TASK STARTED FROM, and by construction: delete `EditorPreferences::UnknownKeys`
// and reflect-cpp goes back to discarding what it cannot match, which is what the tree did.

namespace
{
    // The canonical editor.json this build writes, with `extra` — a fragment of `"key":value` pairs —
    // spliced in at the top level.
    //
    // Built from the real writer instead of typed out, so it stays a valid preference file as fields come
    // and go, and so a test cannot accidentally assert against a file shape nothing produces.
    std::string PrefsFileWith( const std::string& extra )
    {
        const std::string canonical = Common::Json::Write( EditorPreferences{} );
        const std::size_t close     = canonical.rfind( '}' );
        EXPECT_NE( close, std::string::npos ) << "the preference writer did not produce a JSON object";
        if ( close == std::string::npos )
            return canonical;
        return canonical.substr( 0, close ) + "," + extra + "}";
    }

    // A JSON literal in the form rfl writes it, so a test compares values and not spelling.
    std::string Canonical( const std::string& jsonLiteral )
    {
        const auto parsed = Common::Json::Parse( jsonLiteral );
        EXPECT_TRUE( parsed.IsSuccess() ) << "test data is not valid JSON: " << jsonLiteral;
        if ( !parsed.IsSuccess() )
            return "<unreadable>";
        return Common::Json::Write( parsed.GetValue() );
    }
} // namespace

// THE HEADLINE. A file arrives holding a key this build has never heard of; the user does something
// entirely unrelated; the key is still there afterwards and its value has not been touched.
TEST( PreferenceOwnershipUnknownKeys, ASaveDoesNotDeleteAKeyThisBuildDoesNotKnow )
{
    struct Case
    {
        const char* Key;
        const char* Value;
        const char* Shape;
    };

    const Case cases[] = {
         { "ANewerBuildsBoolSetting", "true", "a bool — the shape PackageAppBundle had" },
         { "ANewerBuildsStringSetting", "\"Release\"", "a string — the shape PackageConfig had" },
         { "ANewerBuildsPathSetting", "\"Build/Output\"", "a string with a separator in it" },
         { "ANewerBuildsIntSetting", "17", "a whole number" },
         { "ANewerBuildsFloatSetting", "0.25", "a fraction, which a naive int round-trip flattens" },
         { "ANewerBuildsListSetting", "[\"a\",\"b\",\"c\"]", "an array" },
         { "ANewerBuildsBlockSetting", R"({"Nested":{"Deep":[1,2,3]},"Flag":false})", "a nested object" },
         { "ANewerBuildsAbsentSetting", "null", "a null — distinct from the key being gone" },
    };

    for ( const Case& probe : cases )
    {
        SCOPED_TRACE( std::string( probe.Key ) + " = " + probe.Value + "  (" + probe.Shape + ")" );

        FreshInstall();
        WriteWholeFile( PrefsPath(), PrefsFileWith( std::string( "\"" ) + probe.Key + "\":" + probe.Value ) );

        // The next launch of an editor that has never heard of this key.
        EditorPreferences::Get() = EditorPreferences{};
        EditorPreferences::Load();

        // ...in which somebody toggles the Perf HUD from the View menu. One bool, one save, and nothing
        // about it mentions anybody else's settings.
        EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
        ASSERT_TRUE( EditorPreferences::Save() );

        const std::string after = ReadWholeFile( PrefsPath() );
        EXPECT_EQ( ValueOf( after, probe.Key ), Canonical( probe.Value ) )
             << "saving editor.json deleted or altered a key this build does not know. This is the defect "
                "К9 exists for: several builds share one owner's file, and each of them rewrites the whole "
                "of it from its own struct.";

        // And the save still did what it was for.
        EXPECT_EQ( ValueOf( after, "ShowPerfHud" ),
                   Canonical( EditorPreferences::Get().ShowPerfHud ? "true" : "false" ) );
    }
}

// The same statement over a RESTART, which is where the loss was actually noticed: the value has to come
// back out of the file, not merely be re-written from a copy this process is still holding.
TEST( PreferenceOwnershipUnknownKeys, AnUnknownKeySurvivesAnyNumberOfLaunchesAndSaves )
{
    FreshInstall();
    WriteWholeFile( PrefsPath(), PrefsFileWith( R"("ANewerBuildsSetting":{"Mode":"Face","Passes":3})" ) );

    const std::string expected = Canonical( R"({"Mode":"Face","Passes":3})" );

    for ( int launch = 0; launch < 3; ++launch )
    {
        SCOPED_TRACE( "launch " + std::to_string( launch ) );

        EditorPreferences::Get() = EditorPreferences{};
        EditorPreferences::Load();

        EditorPreferences::Get().CameraSpeed = 1.0f + static_cast<float>( launch );
        ASSERT_TRUE( EditorPreferences::Save() );

        EXPECT_EQ( ValueOf( ReadWholeFile( PrefsPath() ), "ANewerBuildsSetting" ), expected );
    }
}

// The other direction, and the one that makes future migrations non-destructive rather than merely
// survivable: a key that IS a field of this build belongs to the field, not to the carrier. That is what
// happens when the build which owns a key finally lands — yesterday's unknown key is read into the field
// it was always meant for, and the file states it exactly once afterwards.
TEST( PreferenceOwnershipUnknownKeys, AKeyThisBuildDoesKnowGoesToItsFieldAndNotToTheCarrier )
{
    FreshInstall();
    WriteWholeFile( PrefsPath(), PrefsFileWith( R"("ANewerBuildsSetting":1)" ) );

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();

    // CameraSpeed is written by the canonical half of the file above, so it exercises the matched path.
    EXPECT_EQ( EditorPreferences::Get().UnknownKeys.size(), 1u )
         << "a key the struct declares was captured as an unknown one, which would then be written twice";
    EXPECT_TRUE( EditorPreferences::Get().UnknownKeys.get( "ANewerBuildsSetting" ).has_value() );

    EditorPreferences::Get().CameraSpeed = 9.5f;
    ASSERT_TRUE( EditorPreferences::Save() );

    const std::vector<std::string> keys = KeysOf( ReadWholeFile( PrefsPath() ) );
    EXPECT_EQ( std::count( keys.begin(), keys.end(), std::string( "CameraSpeed" ) ), 1 )
         << "editor.json states CameraSpeed more than once";

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EXPECT_FLOAT_EQ( EditorPreferences::Get().CameraSpeed, 9.5f );
}

// A file this build wrote itself carries nothing extra. Without this, "every key survives" could be
// satisfied by a carrier that quietly accumulates duplicates of the struct's own fields.
TEST( PreferenceOwnershipUnknownKeys, AFileThisBuildWroteHasNoUnknownKeysInIt )
{
    FreshInstall();

    EditorPreferences::Get().CameraSpeed = 3.5f;
    ASSERT_TRUE( EditorPreferences::Save() );

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();

    EXPECT_TRUE( EditorPreferences::Get().UnknownKeys.empty() )
         << "the store captured keys from a file it wrote itself";
}

// THE EXEMPTION, PINNED. Desert/Tests/Engine/ConfigOwnership censuses editor.json from
// rfl::fields<EditorPreferences>() minus the ExtraFields member — a field list, read without writing
// anything. This asserts the same claim against the BYTES: the file's keys are exactly the struct's
// fields, less the carrier, plus whatever the carrier is holding. If the two ever disagree, the census
// is certifying a file shape that does not exist.
TEST( PreferenceOwnershipUnknownKeys, TheKeysWrittenAreExactlyTheStructsFieldsPlusThePreservedOnes )
{
    FreshInstall();
    WriteWholeFile( PrefsPath(), PrefsFileWith( R"("ANewerBuildsSetting":1,"AndAnother":"two")" ) );

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
    ASSERT_TRUE( EditorPreferences::Save() );

    std::vector<std::string> expected;
    int                      carriers = 0;
    for ( const auto& meta : rfl::fields<EditorPreferences>() )
    {
        // The one member that is not a key of the file. Named here and nowhere else in this suite,
        // because this test IS the statement that it is the only one.
        if ( std::string( meta.name() ) == "UnknownKeys" )
        {
            ++carriers;
            continue;
        }
        expected.push_back( std::string( meta.name() ) );
    }
    EXPECT_EQ( carriers, 1 ) << "EditorPreferences::UnknownKeys was renamed or removed";

    expected.push_back( "ANewerBuildsSetting" );
    expected.push_back( "AndAnother" );

    std::vector<std::string> written = KeysOf( ReadWholeFile( PrefsPath() ) );

    std::sort( expected.begin(), expected.end() );
    std::sort( written.begin(), written.end() );

    EXPECT_EQ( written, expected )
         << "what editor.json actually contains and what ConfigOwnership censuses have come apart";
}

// ---------------------------------------------------------------------------------------------------
// 7a. THE KEY THAT APPEARED AFTER THIS EDITOR STARTED
// ---------------------------------------------------------------------------------------------------
//
// THE HALF A LOAD-TIME CARRIER CANNOT REACH, and the one that matches how this project is actually
// worked: several editors open all day against one `~/.desertengine/editor.json`. Editor A starts when
// the file holds nothing A does not know, so A's carrier is EMPTY and stays empty however faithfully it
// is written back. B's newer build then writes a key of its own. A's next save — a Perf HUD toggle, a
// slider release, anything — deletes it, and every assertion in §7 above still passes while it happens.
//
// So the keys another build owns are re-read at the moment of WRITING, not remembered from the moment of
// reading. These are the tests for that, and they are red against a carrier that is only filled by
// Load().

TEST( PreferenceOwnershipUnknownKeys, ASaveKeepsAKeyThatAppearedAfterThisEditorLoadedTheFile )
{
    FreshInstall();

    // 10:00 — this editor starts. The file holds nothing it does not know.
    EditorPreferences::Get().CameraSpeed = 2.0f;
    ASSERT_TRUE( EditorPreferences::Save() );
    ASSERT_TRUE( EditorPreferences::Get().UnknownKeys.empty() );

    // 10:30 — another build, with a field this one does not have, writes the file.
    const std::string theirs = ReadWholeFile( PrefsPath() );
    WriteWholeFile( PrefsPath(), theirs.substr( 0, theirs.rfind( '}' ) ) + R"(,"ANewerBuildsSetting":true})" );

    // 11:00 — somebody toggles the Perf HUD in the editor that has been open since ten.
    EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
    ASSERT_TRUE( EditorPreferences::Save() );

    EXPECT_EQ( ValueOf( ReadWholeFile( PrefsPath() ), "ANewerBuildsSetting" ), "true" )
         << "a save deleted a key that appeared after this editor read the file — the carrier is filled "
            "at load and never refreshed, so it has never heard of the key it is overwriting";
}

// THE BOUNDARY THAT KEEPS THE RE-READ FROM BECOMING A SECOND STORE. It adopts KEYS, never VALUES: a
// field this struct declares is written from what the user has in front of them, so two editors still
// resolve a real disagreement last-writer-wins exactly as before. Adopting a field would be a save that
// changes a setting the user did not touch, which is the whole of what К6 removed.
TEST( PreferenceOwnershipUnknownKeys, TheReReadTakesKeysItCannotNameAndNoValueItCan )
{
    FreshInstall();

    EditorPreferences::Get().CameraSpeed = 2.0f;
    ASSERT_TRUE( EditorPreferences::Save() );

    // Another editor writes BOTH a key this build has never seen and a different value for a field it
    // owns. Only the first may come back.
    EditorPreferences other;
    other.CameraSpeed        = 99.0f;
    const std::string theirs = Common::Json::Write( other );
    WriteWholeFile( PrefsPath(), theirs.substr( 0, theirs.rfind( '}' ) ) + R"(,"ANewerBuildsSetting":true})" );

    EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
    ASSERT_TRUE( EditorPreferences::Save() );

    EXPECT_FLOAT_EQ( EditorPreferences::Get().CameraSpeed, 2.0f )
         << "the re-read pulled a SETTING off disk and changed a value the user did not touch";
    EXPECT_EQ( ValueOf( ReadWholeFile( PrefsPath() ), "CameraSpeed" ), "2.0" );
    EXPECT_EQ( ValueOf( ReadWholeFile( PrefsPath() ), "ANewerBuildsSetting" ), "true" );
}

// ---------------------------------------------------------------------------------------------------
// 7b. THE OTHER HALF OF THE RULE: A DELETION STILL FINISHES
// ---------------------------------------------------------------------------------------------------
//
// Preserving unknown keys is one edit away from "this file never loses anything", which is contract §4's
// legacy-forever failure wearing a safety feature's clothes. The line between them is WHO decided: a key
// another build owns is preserved, a key this project retired is dropped BY NAME, once, with a log line.
// К1 deleted `PhotogrammetryCaptureCommand` and `PhotogrammetryMode` — both dead settings read by nothing
// — and until К9 they needed no migration because the next save dropped them for free. It no longer does.

TEST( PreferenceOwnershipUnknownKeys, ARetiredKeyIsDroppedAndAnotherBuildsKeyIsNot )
{
    EditorPreferences carrying;
    // std::string, not a literal: rfl::Object overloads insert() for both std::string and
    // std::string_view, so a bare const char* is ambiguous.
    carrying.UnknownKeys.insert( std::string( "PhotogrammetryMode" ),
                                 Common::Json::Value( std::string( "Object" ) ) );
    carrying.UnknownKeys.insert( std::string( "ANewerBuildsSetting" ), Common::Json::Value( true ) );
    carrying.UnknownKeys.insert( std::string( "PhotogrammetryCaptureCommand" ),
                                 Common::Json::Value( std::string( "capture {photos}" ) ) );

    const auto raised = EditorPreferences::MigrateLoaded( carrying );

    ASSERT_EQ( raised.size(), 2u ) << "the migration did not report exactly the two keys it retired";
    EXPECT_NE( raised[0].find( "PhotogrammetryMode" ), std::string::npos ) << raised[0];
    EXPECT_NE( raised[1].find( "PhotogrammetryCaptureCommand" ), std::string::npos ) << raised[1];

    ASSERT_EQ( carrying.UnknownKeys.size(), 1u );
    EXPECT_TRUE( carrying.UnknownKeys.get( "ANewerBuildsSetting" ).has_value() )
         << "the retirement took a key that belongs to somebody else with it";

    // Idempotent, like the unit migration beside it: run on its own output there is nothing left to do,
    // so a second launch neither writes nor logs.
    EXPECT_TRUE( EditorPreferences::MigrateLoaded( carrying ).empty() );
}

// The same thing through the file, because "dropped from the struct" and "gone from disk" are two claims
// and only the second is what a retirement means. Load() writes back exactly when the migration raised
// something, which is the mechanism that makes it fire once.
TEST( PreferenceOwnershipUnknownKeys, LoadingWritesTheFileBackWithoutTheRetiredKeys )
{
    FreshInstall();
    WriteWholeFile( PrefsPath(), PrefsFileWith( R"("PhotogrammetryMode":"Object","ANewerBuildsSetting":42)" ) );

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();

    const std::string afterLoad = ReadWholeFile( PrefsPath() );
    EXPECT_EQ( ValueOf( afterLoad, "PhotogrammetryMode" ), "<missing>" )
         << "a key this project retired is still on disk after a load that was supposed to drop it";
    EXPECT_EQ( ValueOf( afterLoad, "ANewerBuildsSetting" ), "42" );

    // And it does not fire again: the second launch has nothing to raise, so nothing is written and the
    // bytes are exactly what the first launch left. A marker appended to the file survives iff no write
    // happened, which is the probe the К8 tests above use for the same question.
    const std::string marked = afterLoad + "\n";
    WriteWholeFile( PrefsPath(), marked );
    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EXPECT_EQ( ReadWholeFile( PrefsPath() ), marked ) << "the retirement fires on every launch";
}

// ---------------------------------------------------------------------------------------------------
// 8. К10 — A VIEWPORT MODE IS NOT A PREFERENCE
// ---------------------------------------------------------------------------------------------------
//
// THE SENTENCE: no save of the settings ever writes a debug-view flag different from the one the user
// chose. It is deliberately stated over the whole of DebugViewState rather than over `ShowGrid`, because
// the defect was a shape and the grid was only the field that happened to have a mode attached to it.
//
// In the exact form it shipped, and it is the third instance of the family К6 and К8 are the other two:
//
//   К6: one VALUE had two owners.   К8: one DECISION had two takers.
//   К10: one FIELD had two MEANINGS — `EditorPreferences::DebugView.ShowGrid` was read as "the user
//        wants a grid" by everything that persists it, and written as "this viewport is in 2D UI mode"
//        by ViewportPanel. Entering 2D mode assigned `false` into the struct that goes to disk and parked
//        the user's real answer in `ViewportPanel::m_SavedShowGrid`.
//
// The two meanings have different owners and different lifetimes, and that is the whole argument: the
// user's answer outlives the session, the mode dies with a toggle. К2 saw the collision and fenced it by
// restoring the true value around the two `EditorPreferences::Save()` calls that existed in that file;
// by the time К8 and П6 had finished there were twenty-six call sites and twenty-four had no fence. Any
// of those, fired while 2D mode was on — the View menu's Perf HUD item, an MSAA pick, a star in Details,
// a Build Settings path — wrote "ShowGrid: false" into ~/.desertengine/editor.json, permanently and for
// every scene, because nothing on the next launch can tell a mode from a choice.
//
// THE FIX IS NOT A TWENTY-FIFTH FENCE. The mode no longer has a field in the saved struct at all: the
// suppression is applied to a COPY on the view's way to the renderer (Editor/Core/ViewportModes.hpp,
// ViewportPanel::EffectiveDebugView), exactly as the corner orientation triad — the OTHER thing 2D mode
// hides — has always worked. This is К6's move again: the second store is deleted rather than
// synchronised, so a call site added tomorrow cannot forget something that no longer exists.
//
// FIVE ASSERTIONS, and they cover different halves. The first three link the real seam and the real store
// and ask them the user's question. The last two read source text, because "nobody stashes a preference
// in a panel" and "no save is fenced" are statements about code that no store can be asked about — the
// same reason §6 reads DrawPreferencesWindow, and the reader is the shared one Д33 rebuilt.

namespace
{
    // The names of DebugViewState's fields, DERIVED from the serialization that writes editor.json rather
    // than from a list or from parsing the header. `rfl::json::write` is the exact call Save() makes, so
    // a flag added to DebugViewState tomorrow is covered by everything below without anybody editing this
    // file — and a flag that somehow stopped being serialized would drop out of the census loudly (every
    // user asserts the list is non-empty) instead of quietly.
    std::vector<std::string> DebugViewFields()
    {
        std::vector<std::string> names;
        const auto               whole = Common::Json::Parse( Common::Json::Write( EditorPreferences{} ) );
        if ( !whole.IsSuccess() )
            return names;
        const auto view = Common::Json::Root( whole.GetValue() ).Find( "DebugView" );
        if ( !view.has_value() )
            return names;
        view->ForEachMember( [&]( std::string_view name, const Common::Json::Node& )
                             { names.emplace_back( name ); } );
        return names;
    }

    // Which DEBUG-VIEW flags differ between two serializations of a DebugViewState. The sibling of
    // FieldsThatDiffer above and derived the same way; it is a separate function only because the two
    // enumerate different structs, and using the wrong one would compare a list of key names against a
    // document that has none of them and report everything as missing.
    std::vector<std::string> ViewFieldsThatDiffer( const std::string& before, const std::string& after )
    {
        const auto lhs = Common::Json::Parse( before );
        const auto rhs = Common::Json::Parse( after );
        EXPECT_TRUE( lhs.IsSuccess() ) << "the 'before' view is not readable JSON";
        EXPECT_TRUE( rhs.IsSuccess() ) << "the 'after' view is not readable JSON";
        if ( !lhs.IsSuccess() || !rhs.IsSuccess() )
            return { "<unreadable>" };

        const Common::Json::Node lhsObject = Common::Json::Root( lhs.GetValue() );
        const Common::Json::Node rhsObject = Common::Json::Root( rhs.GetValue() );
        if ( lhsObject.GetKind() != Common::Json::Kind::Object ||
             rhsObject.GetKind() != Common::Json::Kind::Object )
            return { "<not-an-object>" };

        std::vector<std::string> differing;
        for ( const std::string& key : DebugViewFields() )
        {
            const auto a = lhsObject.Find( key );
            const auto b = rhsObject.Find( key );
            if ( !a.has_value() || !b.has_value() )
            {
                differing.push_back( key + " <missing>" );
                continue;
            }
            if ( Common::Json::Write( a->Raw() ) != Common::Json::Write( b->Raw() ) )
                differing.push_back( key );
        }
        return differing;
    }
} // namespace

// The seam itself: a mode SUBTRACTS from the user's answer and changes nothing else.
//
// Stated over every field rather than over ShowGrid, and in both directions — a user who already has the
// grid off must not be handed it back when the mode ends, and a mode must never turn a flag ON. A mode
// that could add an overlay would be a viewport deciding what the user wanted to see, which is the same
// authority confusion in the opposite direction.
TEST( PreferenceOwnership, AViewportModeOnlyEverSubtractsFromTheUsersAnswer )
{
    ASSERT_FALSE( DebugViewFields().empty() )
         << "DebugViewState serializes no fields; this census would certify nothing";

    Desert::Graphic::DebugViewState user;
    user.ShowGrid          = true;
    user.ShowColliders     = true;
    user.ShowBoundingBoxes = true;
    user.WireframeMode     = true;

    // No mode on: the renderer is handed exactly what the user chose, field for field.
    EXPECT_EQ( Common::Json::Write( ApplyViewportModes( user, ViewportModes{} ) ), Common::Json::Write( user ) )
         << "a viewport with no mode active altered the user's view";

    // 2D UI mode on: the grid is gone and NOTHING ELSE MOVED. Asserted as "exactly one field differs, and
    // it is ShowGrid" rather than as "ShowGrid is false", so a mode that quietly took a second overlay
    // away with it is caught by the same line.
    ViewportModes ui2d;
    ui2d.UI2D = true;

    const Desert::Graphic::DebugViewState hidden = ApplyViewportModes( user, ui2d );
    EXPECT_FALSE( hidden.ShowGrid ) << "2D UI mode did not hide the grid";

    const std::vector<std::string> expected = { "ShowGrid" };
    EXPECT_EQ( ViewFieldsThatDiffer( Common::Json::Write( user ), Common::Json::Write( hidden ) ), expected )
         << "2D UI mode changed a flag other than the grid on the way to the renderer";

    // A user with the grid already off keeps it off, and the mode adds nothing.
    const Desert::Graphic::DebugViewState allOff;
    EXPECT_EQ( Common::Json::Write( ApplyViewportModes( allOff, ui2d ) ), Common::Json::Write( allOff ) );
}

// THE HEADLINE, ASKED OF THE FILE. The user's own sentence, walked end to end through the real store: the
// grid is on, a viewport enters 2D UI mode, somebody does something unrelated that saves, the editor is
// closed without leaving 2D mode and started again — and the grid is still on.
//
// The unrelated action is the View menu's Perf HUD item, copied verbatim from EditorLayer::DrawViewMenu
// for the same reason §2 uses it: it is the cheapest real trigger and nothing about it mentions the
// viewport. On the tree that shipped the defect this fails at the last line, because entering 2D mode had
// already written `false` into the field the save then carried to disk.
TEST( PreferenceOwnership, NoSaveWritesAGridDifferentFromTheOneTheUserChose )
{
    FreshInstall();

    // The user ticks Grid in the viewport's Show popup. One click, one save — that is how that popup works.
    EditorPreferences::Get().DebugView.ShowGrid = true;
    ASSERT_TRUE( EditorPreferences::Save() );

    // They open a UI canvas and press "2D". The suppression exists ONLY in what the renderer is handed.
    ViewportModes ui2d;
    ui2d.UI2D = true;
    EXPECT_FALSE( ApplyViewportModes( EditorPreferences::Get().DebugView, ui2d ).ShowGrid )
         << "2D UI mode did not hide the grid at all";
    EXPECT_TRUE( EditorPreferences::Get().DebugView.ShowGrid )
         << "entering a viewport mode changed the value the settings file holds";

    // ...and now anything at all saves. This is the step that made the defect permanent.
    EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
    ASSERT_TRUE( EditorPreferences::Save() );

    // The editor is closed WITHOUT leaving 2D mode, and started again.
    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();
    EXPECT_TRUE( EditorPreferences::Get().DebugView.ShowGrid )
         << "the grid the user turned on was off after a restart: a viewport mode reached editor.json";
}

// The same statement over the FILE and over every debug flag, so that a mode attached to a different flag
// tomorrow is covered without this file changing: a save carries the fields the saving action touched and
// no others, whatever a viewport happens to be doing at the time.
TEST( PreferenceOwnership, AViewportModeAddsNothingToWhatASaveCarries )
{
    FreshInstall();

    auto& view             = EditorPreferences::Get().DebugView;
    view.ShowGrid          = true;
    view.ShowColliders     = true;
    view.ShowBoundingBoxes = true;
    ASSERT_TRUE( EditorPreferences::Save() );
    const std::string before = ReadWholeFile( PrefsPath() );

    // A viewport is in 2D mode for the whole of the next action. Modes are computed and never stored, so
    // there is nothing here that could interfere with a save — which is exactly what is being asserted.
    ViewportModes ui2d;
    ui2d.UI2D = true;
    EXPECT_FALSE( ApplyViewportModes( view, ui2d ).ShowGrid );

    EditorPreferences::Get().CameraSpeed = 3.5f;
    ASSERT_TRUE( EditorPreferences::Save() );

    const std::vector<std::string> expected = { "CameraSpeed" };
    EXPECT_EQ( FieldsThatDiffer( before, ReadWholeFile( PrefsPath() ) ), expected );
}

// ---------------------------------------------------------------------------------------------------
// 7b. THE TWO SHAPES THAT MADE IT POSSIBLE, REFUSED IN THE SOURCE
// ---------------------------------------------------------------------------------------------------

namespace
{
    // Every .cpp under Editor/Source, comments and literals blanked. The shared reader preserves length
    // and line breaks, so an offset still names a line of the original file.
    struct EditorSource
    {
        std::string Path;
        std::string Text;

        std::size_t LineOf( std::size_t at ) const
        {
            return 1 + static_cast<std::size_t>( std::count( Text.begin(), Text.begin() + at, '\n' ) );
        }
    };

    const std::vector<EditorSource>& EditorSources()
    {
        static const std::vector<EditorSource> sources = []
        {
            std::vector<EditorSource> out;
            const std::string         root = RepoRoot();
            if ( root.empty() )
                return out;

            for ( const auto& entry : std::filesystem::recursive_directory_iterator( root + "Editor/Source" ) )
            {
                if ( !entry.is_regular_file() || entry.path().extension() != ".cpp" )
                    continue;
                EditorSource file;
                file.Path = entry.path().filename().string();
                file.Text = Text::StripCommentsAndLiterals( ReadWholeFile( entry.path() ) );
                out.push_back( std::move( file ) );
            }
            std::sort( out.begin(), out.end(),
                       []( const EditorSource& a, const EditorSource& b ) { return a.Path < b.Path; } );
            return out;
        }();
        return sources;
    }

    // The identifier that owns the member access ending at `at`, i.e. the `x` of `x.Field`. Empty when
    // `Field` is not reached through a dot at all.
    std::string ReceiverBefore( const std::string& s, std::size_t at )
    {
        if ( at == 0 || s[at - 1] != '.' )
            return {};
        std::size_t end = at - 1;
        while ( end > 0 && std::isspace( static_cast<unsigned char>( s[end - 1] ) ) != 0 )
            --end;
        std::size_t start = end;
        while ( start > 0 && Text::IsIdentChar( s[start - 1] ) )
            --start;
        return s.substr( start, end - start );
    }

    // `<receiver>.<member>` immediately followed by a plain `=`, i.e. an assignment TARGET rather than a
    // read. Returns "receiver.member", or an empty string when what is at `at` is anything else.
    std::string AssignmentTargetAt( const std::string& s, std::size_t at )
    {
        const std::string receiver = Text::IdentAt( s, at );
        if ( receiver.empty() )
            return {};
        std::size_t i = Text::SkipSpace( s, at + receiver.size() );
        if ( i >= s.size() || s[i] != '.' )
            return {};
        i                      = Text::SkipSpace( s, i + 1 );
        const std::string name = Text::IdentAt( s, i );
        if ( name.empty() )
            return {};
        i = Text::SkipSpace( s, i + name.size() );
        if ( i >= s.size() || s[i] != '=' )
            return {};
        if ( i + 1 < s.size() && s[i + 1] == '=' )
            return {}; // a comparison
        return receiver + "." + name;
    }

    // WHICH NAMES IN THIS FILE ARE THE PREFERENCE STORE'S DEBUG VIEW, derived rather than spelled. A file
    // binds it as `auto& view = EditorPreferences::Get().DebugView;` (the viewport) or reaches it as
    // `prefs.DebugView` (the layer), and the local's name is an honest thing to rename — so the binding is
    // read out of the same file instead of being hard-coded, exactly as SettingConsumers derives its
    // receivers.
    //
    // THIS IS WHAT KEEPS THE CENSUS OFF INNOCENT CODE. `PreviewViewport.cpp` legitimately writes
    // `debugView.ShowGrid = m_Setup.ShowGrid` — a preview's own authored setup pushed into a
    // DebugViewState it owns outright, with no preference anywhere near it. Without the binding step that
    // line is indistinguishable from the defect, and a census that reddens on correct code gets disabled.
    std::vector<std::string> DebugViewReceivers( const std::string& text )
    {
        std::vector<std::string> out{ "DebugView" }; // the direct form, EditorPreferences::Get().DebugView.X
        for ( const std::size_t at : Text::WordPositions( text, "DebugView" ) )
        {
            const std::size_t after = Text::SkipSpace( text, at + 9 );
            if ( after >= text.size() || text[after] != ';' )
                continue; // not the end of an initializer, so nothing is being bound to it here
            const std::string statement = Text::StatementBefore( text, at );
            if ( statement.find( "EditorPreferences" ) == std::string::npos )
                continue;
            const std::string name = Text::DeclaredNameBeforeAssignment( statement );
            if ( !name.empty() && std::find( out.begin(), out.end(), name ) == out.end() )
                out.push_back( name );
        }
        return out;
    }
} // namespace

// SHAPE ONE: A PREFERENCE FIELD PARKED IN A PANEL'S OWN STATE.
//
// `m_SavedShowGrid = view.ShowGrid` on the way into 2D mode, `view.ShowGrid = m_SavedShowGrid` on the way
// out and inside both save fences. That copy is what made the store stop being the authority — the same
// second-store shape as К6's GizmoState statics, arriving through a different door.
//
// The rule: A DEBUG-VIEW FLAG OF THE PREFERENCE STORE IS NEVER COPIED TO OR FROM A MEMBER VARIABLE. Every
// side of it is derived — the field names from the serialization, the receiver from each file's own
// binding, the member prefix from this codebase's `m_` / `s_` convention — so no list here can go stale.
// A control writing `view.ShowGrid = grid` from a local is untouched, which is right: that local IS the
// user's click, made one statement earlier by an ImGui checkbox.
TEST( PreferenceOwnershipSource, NoPanelKeepsItsOwnCopyOfADebugViewFlag )
{
    const std::vector<std::string> fields = DebugViewFields();
    ASSERT_FALSE( fields.empty() ) << "no debug-view field names were derived; this census would pass on air";
    ASSERT_FALSE( EditorSources().empty() ) << "no Editor sources were read";

    int inspected = 0;
    for ( const EditorSource& file : EditorSources() )
    {
        const std::vector<std::string> receivers = DebugViewReceivers( file.Text );
        for ( const std::string& field : fields )
            for ( const std::size_t at : Text::WordPositions( file.Text, field ) )
            {
                const std::string receiver = ReceiverBefore( file.Text, at );
                if ( receiver.empty() ||
                     std::find( receivers.begin(), receivers.end(), receiver ) == receivers.end() )
                    continue; // some other struct's flag of the same name, not the preference store's
                ++inspected;

                // Restored FROM a member: `view.ShowGrid = m_SavedShowGrid;`
                const std::size_t eq = Text::SkipSpace( file.Text, at + field.size() );
                if ( eq < file.Text.size() && file.Text[eq] == '=' &&
                     ( eq + 1 >= file.Text.size() || file.Text[eq + 1] != '=' ) )
                {
                    const std::string from = Text::IdentAt( file.Text, Text::SkipSpace( file.Text, eq + 1 ) );
                    EXPECT_TRUE( from.rfind( "m_", 0 ) != 0 && from.rfind( "s_", 0 ) != 0 )
                         << file.Path << ":" << file.LineOf( at ) << " — " << receiver << "." << field
                         << " is being restored from `" << from
                         << "`. A panel that holds a copy of the user's answer has made the store stop being "
                            "the authority; that is К10's defect and К6's before it. Suppress in the view "
                            "handed to the renderer instead (Editor/Core/ViewportModes.hpp).";
                }

                // Stashed INTO a member: `m_SavedShowGrid = view.ShowGrid;`
                const std::string stashed =
                     Text::DeclaredNameBeforeAssignment( Text::StatementBefore( file.Text, at ) );
                EXPECT_TRUE( stashed.rfind( "m_", 0 ) != 0 && stashed.rfind( "s_", 0 ) != 0 )
                     << file.Path << ":" << file.LineOf( at ) << " — " << receiver << "." << field
                     << " is being stashed into `" << stashed
                     << "`. Reading it to render is fine; keeping a copy of it is the second store К10 removed.";
            }
    }

    // A census that resolved no receiver at all would pass in silence, which is the failure mode every
    // other census in this repository has had at least once.
    EXPECT_GT( inspected, 0 ) << "no access to a preference debug-view flag was found anywhere in "
                                 "Editor/Source; the receiver derivation has stopped working";
}

// SHAPE TWO: A FENCE AROUND A SAVE.
//
// `const bool live = view.ShowGrid; if (uiMode) view.ShowGrid = saved; Save(); view.ShowGrid = live;` —
// К2's answer, and it worked, at exactly two of the twenty-six call sites that existed then. A fence is
// per-call-site by construction, so the rule it enforces lives in whoever remembers it; this census is
// that rule living in the build instead.
//
// Stated as an INTERLEAVING, which is what makes it a relation and not a keyword ban: what is refused is
// a preference save with an assignment to `x.y` immediately after it that an earlier statement of the
// same block also assigned. That is precisely "put it back the way the user had it, write, take it away
// again", and it says nothing about a save followed by ordinary unrelated work.
TEST( PreferenceOwnershipSource, NoSaveOfThePreferencesIsFencedByARestoredField )
{
    ASSERT_FALSE( EditorSources().empty() ) << "no Editor sources were read";

    int saves = 0;
    for ( const EditorSource& file : EditorSources() )
        for ( const std::size_t at : Text::WordPositions( file.Text, "Save" ) )
        {
            // A CALL, `EditorPreferences::Save();`, and not the definition of one or a `Save` on some
            // other object. The definition is `bool EditorPreferences::Save()` followed by a BRACE, so
            // requiring the terminating semicolon is what tells the two apart.
            //
            // Spaces are skipped at every step rather than assumed away. Relying on clang-format to keep
            // the call spelled `Save();` would make this census depend on a tool that is not run on every
            // edit, and a reader that silently skips the one call site a fence was added to is the exact
            // shape of "a census that certifies nothing" this suite exists to avoid.
            if ( at < 19 || file.Text.compare( at - 19, 19, "EditorPreferences::" ) != 0 )
                continue;
            const std::size_t open = Text::SkipSpace( file.Text, at + 4 );
            if ( open >= file.Text.size() || file.Text[open] != '(' )
                continue;
            const std::size_t close = Text::SkipSpace( file.Text, open + 1 );
            if ( close >= file.Text.size() || file.Text[close] != ')' )
                continue; // Save( something ) — not this function, which takes no arguments
            const std::size_t semi = Text::SkipSpace( file.Text, close + 1 );
            if ( semi >= file.Text.size() || file.Text[semi] != ';' )
                continue;
            ++saves;

            // The statement straight after the save. A fence's tell is that it assigns to a member.
            const std::size_t nextEnd = file.Text.find( ';', semi + 1 );
            if ( nextEnd == std::string::npos )
                continue;
            const std::string next   = file.Text.substr( semi + 1, nextEnd - semi - 1 );
            const std::string target = AssignmentTargetAt( next, Text::SkipSpace( next, 0 ) );
            if ( target.empty() )
                continue;

            // ...and that the same target was assigned before the save, inside the same block.
            const std::size_t brace  = file.Text.rfind( '{', at );
            const std::size_t from   = brace == std::string::npos ? 0 : brace;
            const std::string before = file.Text.substr( from, at - from );
            const std::string owner  = target.substr( 0, target.find( '.' ) );

            bool fenced = false;
            for ( const std::size_t use : Text::WordPositions( before, owner ) )
                fenced = fenced || AssignmentTargetAt( before, use ) == target;

            EXPECT_FALSE( fenced )
                 << file.Path << ":" << file.LineOf( at ) << " — this EditorPreferences save is FENCED: `"
                 << target
                 << "` is set before it and set again straight after, so what is written is not what the "
                    "struct is carrying. A fence protects one call site out of twenty-six and the next one "
                    "arrives without it. Take the transient meaning out of the saved field instead — "
                    "Editor/Core/ViewportModes.hpp.";
        }

    // A census that walked nothing would pass. The editor really does save its preferences from a couple
    // of dozen places, and that number is the whole reason this is a rule about the FIELD rather than a
    // fence at each site.
    EXPECT_GE( saves, 20 ) << "only " << saves
                           << " EditorPreferences::Save() call sites were found in Editor/Source; the reader "
                              "is not seeing the code it is meant to be judging";
}

// ---------------------------------------------------------------------------------------------------
// 8. THE PINNED FOLDERS — A FILE THAT WAS OUTSIDE EVERY RULE THIS SUITE STATES (К5)
// ---------------------------------------------------------------------------------------------------
//
// `~/.desertengine/asset_favorites.txt` was the content browser's list of pinned folders: flat lines, no
// schema, ABSOLUTE paths, an `ofstream ... trunc` whose result nobody read, and ONE list shared by every
// project the user had ever opened. Four defects, and the reason all four survived is that the file was
// not in any census — the rule "a per-user value is a field of EditorPreferences" was stated by the
// header above it and enforced only over the fields that were already there.
//
// What is asserted here is the migration and the two properties the new shape has and the file did not:
// a pin belongs to a PROJECT, and it names a folder RELATIVE to that project rather than a place on this
// machine's disk. Those are relations (desert-engine-verify §4) — the stored string and the assets root
// have to agree, and the whole defect was that the string alone was taken as the answer.

namespace
{
    // The assets root of an imaginary project, as a path. Nothing here touches the disk: the migration is
    // pure and classifies lines lexically, which is what lets it judge a folder that was deleted years ago.
    const std::filesystem::path kRootA = "/home/dev/ProjectA/Assets";
    const std::filesystem::path kRootB = "/elsewhere/B/Content";
} // namespace

TEST( PreferenceOwnershipFavourites, TheLegacyFileFoldsIntoTheOpenProjectAndForeignLinesAreNamedNotGuessed )
{
    EditorPreferences p;

    const std::vector<std::string> legacy = {
         "/home/dev/ProjectA/Assets/Scenes",         // inside — migrates
         "/home/dev/ProjectA/Assets/Textures/UI",    // inside, nested — migrates
         "",                                         // the file's trailing newline
         "/home/dev/SomeOtherProject/Assets/Meshes", // ANOTHER project's folder, and the file said so nowhere
         "/home/dev/ProjectA/Assets/Scenes",         // the legacy file deduplicated nothing
    };

    const auto raised = EditorPreferences::MigrateFavouritesFile( p, "ProjectA", kRootA, legacy );

    const std::vector<std::string> expected = { "Scenes", "Textures/UI" };
    EXPECT_EQ( p.FavouriteFolders["ProjectA"], expected )
         << "the folders inside the project did not arrive relative, in file order and deduplicated";

    // THE FOREIGN LINE IS THE POINT OF THE TEST. Attributing it to the open project would be inventing the
    // answer the retired file never recorded — and it would put a folder of somebody else's project in
    // this one's sidebar, which is the defect being migrated away from, carried across by the migration.
    EXPECT_EQ( std::count( p.FavouriteFolders["ProjectA"].begin(), p.FavouriteFolders["ProjectA"].end(),
                           "../../SomeOtherProject/Assets/Meshes" ),
               0 );

    bool named = false;
    for ( const std::string& line : raised )
        named = named || line.find( "SomeOtherProject" ) != std::string::npos;
    EXPECT_TRUE( named ) << "a dropped pin was not named; a migration that discards silently is §1.4's "
                            "empty successful answer";
}

// THE RELATION THE ABSOLUTE PATH COULD NOT HOLD. One project, two places on disk — a clone, a worktree,
// a machine with a different home — and the stored form must be the same, because it is a fact about the
// project and not about the disk. With absolute paths the second column here was a different string and
// every pin silently stopped matching.
TEST( PreferenceOwnershipFavourites, TheSameFolderInTwoCheckoutsIsTheSameStoredPin )
{
    EditorPreferences here;
    EditorPreferences there;

    EditorPreferences::MigrateFavouritesFile( here, "P", kRootA, { kRootA.generic_string() + "/Scenes/Levels" } );
    EditorPreferences::MigrateFavouritesFile( there, "P", kRootB, { kRootB.generic_string() + "/Scenes/Levels" } );

    EXPECT_EQ( here.FavouriteFolders["P"], there.FavouriteFolders["P"] );
    EXPECT_EQ( here.FavouriteFolders["P"], ( std::vector<std::string>{ "Scenes/Levels" } ) );
}

// The assets root itself is pinnable from the tree's own context menu, and "" is not a path. It round
// trips as ".", which CurrentFavouriteFolders resolves back to the root rather than to `root/`.
TEST( PreferenceOwnershipFavourites, TheAssetsRootItselfIsStorableAndIsNotAnEmptyString )
{
    EditorPreferences p;
    EditorPreferences::MigrateFavouritesFile( p, "P", kRootA, { kRootA.generic_string() } );
    EXPECT_EQ( p.FavouriteFolders["P"], ( std::vector<std::string>{ "." } ) );
}

// A key with nothing behind it is not "a project with no pins", it is residue — and residue accumulating
// for ever was the fourth of the four defects. A file holding only another project's folders leaves none.
TEST( PreferenceOwnershipFavourites, AFileWithNothingToMigrateLeavesNoKeyBehind )
{
    EditorPreferences p;
    EditorPreferences::MigrateFavouritesFile( p, "P", kRootA, { "/somewhere/else/Assets/Scenes" } );
    EXPECT_EQ( p.FavouriteFolders.count( "P" ), 0u );

    EditorPreferences empty;
    EXPECT_TRUE( EditorPreferences::MigrateFavouritesFile( empty, "P", kRootA, {} ).empty() );
    EXPECT_EQ( empty.FavouriteFolders.count( "P" ), 0u );
}

// TWO PROJECTS, TWO LISTS, AND THE FILE IS THE THING THAT HAS TO KEEP THEM APART. The retired store had
// one list for all of them, so opening a second project drew the first one's folders in its sidebar; this
// asserts the property that replaced it, through the real file rather than in memory.
TEST( PreferenceOwnershipFavourites, EachProjectsPinsSurviveARestartAndDoNotReachTheOther )
{
    FreshInstall();

    EditorPreferences::Get().FavouriteFolders["Alpha"] = { "Scenes", "Textures/UI" };
    EditorPreferences::Get().FavouriteFolders["Beta"]  = { "Meshes" };
    ASSERT_TRUE( EditorPreferences::Save() );

    EditorPreferences::Get() = EditorPreferences{};
    EditorPreferences::Load();

    EXPECT_EQ( EditorPreferences::Get().FavouriteFolders["Alpha"],
               ( std::vector<std::string>{ "Scenes", "Textures/UI" } ) );
    EXPECT_EQ( EditorPreferences::Get().FavouriteFolders["Beta"], ( std::vector<std::string>{ "Meshes" } ) );
    EXPECT_EQ( EditorPreferences::Get().FavouriteFolders.size(), 2u )
         << "the two projects' pins did not stay two lists";
}

// THE PATH THE PANEL ACTUALLY TAKES, against a real project on a real disk — the three helpers the content
// browser calls, in the order a user calls them.
//
// IT OPENS A PROJECT, WHICH IS WHY IT IS LAST IN THE FILE. ProjectContext::Open remaps
// Common::Constants::Path globally and there is no Close; gtest runs suites in the order it first meets
// them, so this group runs after every other one here. Nothing above reads a content path — they are all
// about editor.json under HOME — but a test added after this one is inheriting an open project, and that
// is worth knowing before it is a mystery.
//
// `RecordInRecent::No`: a unit test is not a person opening a project, and the alternative is filing a temp
// directory at the top of the developer's own recent list on every run.
TEST( PreferenceOwnershipFavourites, PinningThroughTheBrowsersOwnHelpersRoundTripsAndDropsWhatIsGone )
{
    FreshInstall();

    const std::filesystem::path project = std::filesystem::temp_directory_path() / "DesertFavouritesProject";
    std::error_code             ec;
    std::filesystem::remove_all( project, ec );
    std::filesystem::create_directories( project / "Assets" / "Scenes", ec );
    std::filesystem::create_directories( project / "Assets" / "Doomed", ec );
    WriteWholeFile( project / "Pinning.deproj",
                    R"({"FileVersion":1,"Name":"Pinning","AssetsRoot":"Assets","DefaultScene":"",)"
                    R"("Description":"","EngineVersion":""})" );

    ASSERT_TRUE( Desert::Project::ProjectContext::Open( ( project / "Pinning.deproj" ).string(),
                                                        Desert::Project::ProjectContext::RecordInRecent::No ) );

    const std::string scenes = ( project / "Assets" / "Scenes" ).generic_string();
    const std::string doomed = ( project / "Assets" / "Doomed" ).generic_string();

    EXPECT_FALSE( EditorPreferences::IsFavouriteFolder( scenes ) );
    EditorPreferences::ToggleFavouriteFolder( scenes );
    EditorPreferences::ToggleFavouriteFolder( doomed );

    // Absolute in, absolute out — the panel navigates with these strings and never sees the stored form.
    EXPECT_TRUE( EditorPreferences::IsFavouriteFolder( scenes ) );
    EXPECT_EQ( EditorPreferences::CurrentFavouriteFolders(), ( std::vector<std::string>{ scenes, doomed } ) );

    // ...and RELATIVE on disk, which is the property the absolute path could not hold.
    EXPECT_EQ( EditorPreferences::Get().FavouriteFolders["Pinning"],
               ( std::vector<std::string>{ "Scenes", "Doomed" } ) );

    // A pin whose folder is deleted behind the editor's back is dropped by the next write, and only that
    // one: the check has to be able to tell a folder that is gone from a folder it could not ask about.
    std::filesystem::remove_all( project / "Assets" / "Doomed", ec );
    EditorPreferences::ToggleFavouriteFolder( scenes ); // unpin Scenes; Doomed is pruned in the same write
    EXPECT_EQ( EditorPreferences::Get().FavouriteFolders.count( "Pinning" ), 0u )
         << "the project's key outlived its last pin";

    std::filesystem::remove_all( project, ec );
}

// ONE PLACE COMPOSES `$HOME/.desertengine`, AND THE CONTENT BROWSER WAS THE THIRD.
//
// `FavoritesFile()` read `HOME` (and `USERPROFILE` on Windows) and joined the directory name itself — a
// third statement of where this user's configuration lives, differing from the real one in the respect
// that mattered: it did not create the directory, so the first pin of a fresh install was written into a
// folder that might not exist. EditorPreferences::ConfigDirectory() is not a fourth: it forwards.
//
// The census is over the RAW text on purpose. The shared reader blanks string literals, and the literal
// is the whole subject — a census run over stripped text here would be looking at nothing and would pass
// whatever the tree did.
TEST( PreferenceOwnershipFavourites, OnlyOnePlaceInTheEditorAndTheEngineComposesTheUserConfigDirectory )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> composers;
    for ( const char* tree :
          { "Editor/Source", "Desert/Desert/Source", "Desert/Common/Source", "Runtime/Source" } )
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root + tree ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const std::string ext = entry.path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" )
                continue;
            if ( ReadWholeFile( entry.path() ).find( "\".desertengine\"" ) != std::string::npos )
                composers.push_back( entry.path().filename().string() );
        }

    const std::vector<std::string> expected = { "ProjectContext.cpp" };
    EXPECT_EQ( composers, expected )
         << "the user config directory is composed somewhere other than ProjectContext::ConfigDirectory(). "
            "A second copy is a directory that is not created, or a directory that moves in one place and "
            "not the other; Tools/ProjectHub keeps its own only because it is a separate binary that links "
            "no engine code at all.";
}

int main( int argc, char** argv )
{
    // ~/.desertengine/editor.json is a real file in a real home directory, and this suite writes it. Point
    // HOME at a temporary directory before anything can read it, or a test run would overwrite the
    // developer's own editor preferences. ProjectContext::ConfigDirectory() reads HOME on every call and
    // caches nothing, and it is the variable it consults first on Windows too, so this is enough.
    const std::filesystem::path home = std::filesystem::temp_directory_path() / "DesertPreferenceOwnership";

    std::error_code ec;
    std::filesystem::remove_all( home, ec );
    std::filesystem::create_directories( home, ec );

#ifdef DESERT_PLATFORM_WINDOWS
    _putenv_s( "HOME", home.string().c_str() );
#else
    setenv( "HOME", home.string().c_str(), 1 );
#endif

    ::testing::InitGoogleTest( &argc, argv );
    const int result = RUN_ALL_TESTS();

    std::filesystem::remove_all( home, ec );
    return result;
}
