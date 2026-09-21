#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// The unknown-key carrier below. rfl is in this header and not only in the .cpp because the carrier is a
// FIELD of the struct — the same reason Assets::EntityData declares its own rfl::ExtraFields inline.
#include <rflcpp/rfl/ExtraFields.hpp>
#include <rflcpp/rfl/Generic.hpp>

// The viewport's debug/show state. An ENGINE type, because the engine's renderer is what consumes it —
// this header only says where the editor's persisted copy lives.
#include <Engine/Graphic/DebugViewState.hpp>

namespace Desert::Editor
{
    // User-level editor settings, persisted to ~/.desertengine/editor.json (per-user, not per-project).
    // Loaded once at editor startup. EVERY EDITOR OF THESE FIELDS PERSISTS ON THE SPOT — the Preferences
    // window commits each control when the user lets go of it, and the panels that own one field each
    // (the viewport's Show flags and snap steps, MSAA in Scene Settings, the stars in Details) commit on
    // the click. There is no "apply" step anywhere and there deliberately is not one; see Save().
    //
    // THIS STRUCT IS THE LIVE STATE, not a copy of it that something else has to be given. Everything
    // that consumes a preference reads it from here every time it needs it — the gizmo snap through
    // Core::GizmoState, the Details stars through the two helpers below, the Show flags straight off
    // DebugView. NOTHING AT ALL IS PUSHED ANYWHERE from here any more: the one value that was
    // (RenderConfig::MSAASamples) belonged to the MACHINE rather than to the editor, and К3 moved it and
    // its four siblings into Common::Settings::MachineSettings, which both hosts read.
    //
    // WHAT BELONGS IN THIS FILE, in one sentence (К1): ONE PERSON'S COPY OF THE EDITOR — what a user's own
    // installation must remember between sessions and across every project, and whose value two people on
    // the same project may legitimately hold differently at the same moment. What does NOT belong: anything
    // a second person opening the project must see (that is the .deproj), anything that varies from level
    // to level (that is the .desce), and anything the SHIPPED RUNTIME needs — the packaged game never opens
    // this file, so a value put here is a value taken away from the player.
    //
    // The three-question procedure that decides where a NEW field goes, the argument for the order of the
    // questions, and the census that goes red when a field lands in the wrong file are all in
    // Desert/Tests/Engine/ConfigOwnership. That suite enumerates this struct through rfl::fields<> — the
    // same call Save() writes it with — so a field added here without a decision fails it immediately.
    // (It skips the ONE member that is not a key of the file, UnknownKeys; PreferenceOwnership asserts
    // that the skip and what rfl::json::write actually emits still agree, so the exemption cannot widen.)
    //
    // AND THIS IS THE ONLY STORE OF EDITOR PREFERENCES. `~/.desertengine` holds four neighbours, and the
    // sentence that used to stand here counted three of them and called this file the only per-user store
    // full stop — which stopped being true when К3 created `machine.json` next door. Each neighbour, and
    // why none of them is somewhere a preference may go instead:
    //
    //   * `projects.json` and `engines.json` — cross-process REGISTRIES shared with the launcher, which
    //     links no engine code; they are lists of what exists, not answers about how anything behaves.
    //   * `Layouts/*.ini` (with the working-directory `imgui.ini`) — opaque dock state ImGui itself
    //     writes and parses. Nothing here can read it and nothing there is a named value.
    //   * `machine.json` — Common::Settings::MachineSettings, and the ONE deliberate second file. Its
    //     fields are the same KIND as these (per host, per person) and it exists because the audience
    //     differs: SceneRenderer reads every one of them and the packaged game runs SceneRenderer, while
    //     the Editor target is the only thing that ever opens editor.json. One kind, two hosts, two files
    //     — the argument and both ends of the wiring are asserted in ConfigOwnership §8.
    //
    // `asset_favorites.txt` was a FIFTH and was the one that had no argument. К5 closed it: the pinned
    // folders are `FavouriteFolders` below, and the file is deleted by the migration that reads it. A new
    // per-user setting goes in this struct — or in machine.json if the shipped game needs it too; a new
    // per-user FILE is a conversation with the owner.
    struct EditorPreferences
    {
        float CameraSpeed = 1.0f;
        // THE GIZMO SNAP, AND THESE FOUR FIELDS ARE ITS ONLY STORAGE. Core::GizmoState reads and writes
        // them; it keeps no copy, and neither does anything else. It used to keep one, with these values
        // pushed into it by Save() — so an unrelated save reverted a step the user had just chosen, and
        // a step chosen from a toolbar never reached this file at all. К6 deleted the second copy rather
        // than adding a fourth Save() call; Desert/Tests/Editor/PreferenceOwnership holds the line.
        //
        // World units, and 1 world unit is 1 CENTIMETRE project-wide. TranslateSnap was 0.5f with the
        // comment "world units" from the metre era, which is how the shipped grid snap ended up at half
        // a centimetre; Load() migrates any stored value below 1 cm once.
        float TranslateSnap   = 50.0f; // cm — half a metre
        float RotateSnapDeg   = 15.0f; // degrees
        float ScaleSnap       = 0.1f;
        bool  PersistentSnap  = false;
        int   AutosaveMinutes = 5;     // 0 = autosave off
        bool  ShowPerfHud     = false; // in-viewport FPS / frame-graph / top-scopes overlay
        // Bumped when the default dock layout's window IDs change (e.g. panel-title icons add a ### suffix,
        // which changes every window's ImGui ID). A stored value below the current forces ONE automatic
        // "reset to default layout" so panels re-dock cleanly instead of scattering against a stale imgui.ini.
        int DockLayoutVersion = 0;

        // `MSAASamples` USED TO BE HERE and К3 moved it, not because the KIND was wrong — a fidelity
        // ladder is per machine — but because the FILE was. It reached the renderer through
        // Graphic::RenderConfig, whose only writer was this struct, and the packaged Runtime never opens
        // this file: a shipped game therefore ran MSAA nailed to 1 with no reader and no dial. It is in
        // Common::Settings::MachineSettings now, beside the post-AA mode it is half a question with. Its
        // key is dropped from existing files by MigrateLoaded()'s retired list — without that, UnknownKeys
        // below would preserve it in every user's editor.json for ever.

        // Selection outline (Jump Flood) — an editor-only viewport visualization, not a scene property.
        // Pushed to the renderer each frame via SceneRenderer::SetOutlineSettings. Width/smoothness in px.
        glm::vec3 OutlineColor      = glm::vec3( 1.0f, 0.5f, 0.0f ); // orange
        float     OutlineWidth      = 4.0f;
        float     OutlineSmoothness = 2.0f;
        bool      EnableOutline     = true;

        // Viewport Show flags + View Mode — grid, colliders, bounding boxes, wireframe, buffer views.
        // Edited by the viewport toolbar's "Show" popup and its View Mode dropdown, pushed to every scene's
        // renderer each frame via SceneRenderer::SetDebugView, and persisted here because it is the USER's
        // answer to "what am I looking at", not the level's. It used to live in the level file: 55 of 80
        // scenes shipped `ShowColliders: true` through git, and 72 of 77 overrode whatever grid setting the
        // person opening them had chosen. See Graphic/DebugViewState.hpp.
        //
        // EVERY FLAG DEFAULTS OFF, including the grid, and that is a deliberate departure from the old
        // SceneSettings default of `ShowGrid = true`. Two reasons: the repository's own scenes are 72:5
        // against the grid, so all-off is what "the editor looks the same after the migration" actually
        // means here; and it makes one rule — an overlay appears because YOU turned it on — instead of one
        // default per flag. Turning the grid on is one click in the Show popup and it then persists across
        // scenes and sessions, which is strictly more than the old behaviour offered.
        //
        // AND IT IS THE USER'S ANSWER AT EVERY INSTANT — never a viewport's idea of what should be visible
        // right now. К10: the viewport's 2D UI mode used to write `ShowGrid = false` straight into this
        // struct and park the real answer in a member of the panel, so any of the twenty-six
        // EditorPreferences::Save() call sites, fired while 2D mode was on, wrote "this user does not want
        // a grid" to disk — permanently, in every scene, because they once edited a canvas. К2 fenced the
        // two save sites that existed then; К8 and П6 raised the count of unfenced ones to twenty-four.
        //
        // The rule that replaced the fences: A VIEWPORT MODE MAY NOT HAVE A FIELD IN THIS STRUCT. What a
        // mode hides is applied to a COPY on the way to the renderer — Editor/Core/ViewportModes.hpp and
        // ViewportPanel::EffectiveDebugView — so a twenty-seventh save site has nothing to remember.
        // Desert/Tests/Editor/PreferenceOwnership §7 holds both halves of that line.
        Graphic::DebugViewState DebugView;

        // Photogrammetry (Model-from-Photos panel): TOOL-AGNOSTIC external command. Reconstruct: {input} = the
        // photos folder, {output} = the produced mesh file, {outdir} = its directory (plug in Meshroom/COLMAP).
        //
        // TWO FIELDS THAT USED TO SIT HERE WERE DELETED BY К1, not moved: `PhotogrammetryCaptureCommand` and
        // `PhotogrammetryMode`. Both were serialized into every editor.json and neither was mentioned by a
        // single line of code outside this declaration — the `{photos}` capture substitution the first one
        // documented was never implemented (the panel writes frames itself), and the Object/Face preset
        // switch the second one documented does not exist. They were §1.3 dead settings, invisible because
        // this file had no readership census at all; Desert/Tests/Engine/ConfigOwnership is now that census.
        //
        // THEY ARE ALSO WHY THE RETIRED-KEY LIST EXISTS. Until К9 they needed no migration, because a key
        // the struct had lost was dropped by the next save on its own. UnknownKeys below stops that
        // happening to anybody's keys, so it would have preserved these two for ever as well — a deletion
        // that never finishes. They are named in MigrateLoaded()'s retired list instead, which drops them
        // once, says so, and writes the file back without them.
        std::string PhotogrammetryCommand    = "meshroom_batch --input {input} --output {outdir}";
        std::string PhotogrammetryPhotosDir  = "";
        std::string PhotogrammetryOutputMesh = "Cooked/Photogrammetry/model.obj";
        // Path to the dlib 68-point model (shape_predictor_68_face_landmarks.dat) for real face tracking on
        // the camera overlay. Empty / dlib-not-built => a placeholder overlay is drawn instead.
        std::string PhotogrammetryFaceModel = "";

        // --- Packaging (Build Settings) -------------------------------------------------------------
        // The three answers the Build Settings panel asks for. They are HERE and not in the panel, and
        // not in the .deproj, by the К1 procedure's first question: two people working on this project
        // at the same moment legitimately want different values for all three — an output path is a
        // place on one person's disk, which Runtime to bundle depends on whether that person is
        // debugging or cutting a playtest build, and a .app is one developer's convenience. None of
        // them is a fact about the product, and the shipped Runtime never reads them: they are consumed
        // by the editor BEFORE the game exists.
        //
        // The panel edits these in place and keeps no copy of its own. That is the rule this header
        // states above and the one К6 had to restore for the gizmo snap: a second copy means one of the
        // two stops being written, and which one is not visible from either side.
        //
        // The target PLATFORM is deliberately not among them. It is not a choice: this editor packages
        // for its own host and nothing else (Editor/Packaging/PackageTarget.hpp), so storing an answer
        // would be storing the only value it can have. П6 deleted the chooser that pretended otherwise.
        std::string PackageOutputDir = "Build/Output"; // relative to the editor cwd, or absolute
        // Which Runtime binary to bundle; one of Editor/Packaging/GamePackager.hpp's kPackageConfigs,
        // and the same default PackageOptions carries. Not that header's constant directly: this struct
        // is serialised by reflect-cpp and must stay a plain data type with no editor includes in it.
        std::string PackageConfig    = "Shipping";
        bool        PackageAppBundle = true;           // macOS: <Name>.app with MoltenVK inside

        // --- Details panel ------------------------------------------------------------------------
        // Fields the user pinned to the top of Details, as "TypeName.FieldName" (e.g. "PointLightData.
        // Intensity"). Only reflected fields can be pinned — a hand-written component widget has no
        // field identity to key on.
        std::vector<std::string> FavouriteFields;
        // Component sections the user collapsed, by their registered name. Everything not listed is
        // expanded, so a fresh install behaves exactly like before this was persisted.
        std::vector<std::string> CollapsedComponents;

        // --- Content browser ------------------------------------------------------------------------
        // The folders the user pinned in the Assets browser. KEYED BY PROJECT — the key is the `Name` the
        // project's `.deproj` states — and every entry is a path RELATIVE to that project's assets root
        // ("." is the root itself).
        //
        // BOTH HALVES OF THAT REPLACE A DEFECT, and the defect was a whole separate file (К5). This list
        // was `~/.desertengine/asset_favorites.txt`: one flat line per pin, no schema, no project, and the
        // pin written as the ABSOLUTE path it happened to have on the machine that made it. Three things
        // followed, and all three are gone with the shape:
        //
        //   * PATH AS IDENTITY. Moving a project — or cloning it into a worktree, which is how this
        //     engine is developed — invalidated every pin silently, because the string named a place on a
        //     disk rather than a folder in a project. The same defect had already been paid for twice, in
        //     `.tex` (Ф2) and in scene files (Д2); a path relative to the assets root cannot carry it.
        //   * NO PROJECT ANYWHERE. There was ONE list for every project this user had ever opened, and the
        //     browser drew all of it: opening a second project showed the first one's folders in its
        //     sidebar. The key is what makes a pin belong to something.
        //   * NOBODY EVER REMOVED ANYTHING. The file only grew. Pins of a folder that no longer exists are
        //     dropped by ToggleFavouriteFolder for the project being edited, and a project whose list
        //     empties loses its key outright.
        //
        // WHAT THAT STILL DOES NOT CLEAN, stated rather than implied: the key of a project this user has
        // DELETED survives, because a key is a name and this build cannot tell a deleted project from one
        // on a drive that is not mounted — and guessing wrong destroys pins nobody can restore. The
        // residue is a name and a few short strings in this user's own file. Two projects that share a
        // `Name` share a list for the same reason: `Name` is the only identity a `.deproj` carries, and
        // the alternative — the project's path — is the defect this field exists to remove.
        std::map<std::string, std::vector<std::string>> FavouriteFolders;

        // --- EVERY OTHER KEY THE FILE HAPPENS TO CONTAIN ------------------------------------------
        // NOT A SETTING, AND NOT A KEY OF ITS OWN. rfl::ExtraFields is spread flat at this struct's own
        // level on write and captures every top-level key the fields above did not claim on read, so
        // `editor.json` gains nothing called "UnknownKeys" — this member is the file's leftovers, held
        // between a read and the next write.
        //
        // WHY IT HAS TO EXIST (К9). Every save is `rfl::json::write( Get() )`, which rewrites the whole
        // file from the struct THIS binary was compiled with. A key the binary has never heard of was
        // therefore deleted by the act of saving anything at all — honestly, silently, and with no way
        // for either side to notice. That is not a hypothetical: two agents' builds ran against the one
        // owner's `~/.desertengine/editor.json` within an hour, and the build that predated the
        // packaging fields erased `PackageAppBundle`, `PackageConfig` and `PackageOutputDir` the first
        // time somebody toggled anything. The file had to be restored by hand from a backup.
        //
        // The shape is general and this is the second time the project has paid for it: a container
        // rewritten in full by a writer that knows only PART of what it contains. (The first was П3, and
        // Desert/Tests/Editor/PreferenceOwnership §7 states the relation this member enforces:
        // "saving does not delete a key the writer does not know".)
        //
        // IT IS NOT A COMPATIBILITY SHIM AND DOES NOT KEEP LEGACY ALIVE (contract §4). A key this
        // project DELETED on purpose is not unknown, it is retired: MigrateLoaded() drops it by name,
        // says so in the log and writes the file back without it. Preservation is for keys another
        // BUILD owns, and it is the deletion path that decides a key is dead — never the accident of
        // which binary saved last.
        rfl::ExtraFields<rfl::Generic> UnknownKeys;

        static EditorPreferences& Get();

        // Membership helpers for the two lists above. Toggling SAVES immediately: these are single
        // clicks scattered through the panel, and losing them to a crash before the next explicit save
        // would be worse than the write.
        static bool IsFavouriteField( const std::string& key );
        static void ToggleFavouriteField( const std::string& key );
        static bool IsComponentCollapsed( const std::string& name );
        static void SetComponentCollapsed( const std::string& name, bool collapsed );

        // The pinned folders OF THE PROJECT THAT IS OPEN, as ABSOLUTE paths — the form the content
        // browser navigates and compares with. Empty when no project is open, which is a real answer and
        // not a failure: a pin belongs to a project, so without one there is nothing to pin it to.
        //
        // THE PANEL NEVER SEES THE STORED FORM. Which project a pin belongs to and what it is relative to
        // are decided here, in the file that owns the field, and nowhere else — the browser hands over the
        // absolute path it already has and gets absolute paths back. That is the same rule the gizmo snap
        // had to be given back after К6: one value, one place that knows its shape.
        static std::vector<std::string> CurrentFavouriteFolders();
        static bool                     IsFavouriteFolder( const std::string& absoluteFolder );
        // Pins or unpins, and SAVES on the spot, exactly as the two list helpers above do and for the same
        // reason — this is a single click in a context menu. A write that fails is named by Save() itself,
        // with the file and the reason, and this session then holds the pin in memory only; that is why
        // there is no third return convention here for one store.
        static void ToggleFavouriteFolder( const std::string& absoluteFolder );

        // ~/.desertengine (created on demand); shared with the Project Hub's projects.json.
        static std::string ConfigDirectory();

        // Reads editor.json into Get() (keeps defaults when the file is missing/corrupt). The camera
        // speed is applied by EditorLayer once a camera exists.
        //
        // IT WRITES THE FILE IN EXACTLY ONE CASE, and the case is named: when MigrateLoaded() below
        // raises a stored value, the new form is written back so the migration fires once instead of
        // every launch (contract §4.3). That write goes through SaveMigrated(), NOT through Save(), and
        // the difference is the point — a load that calls "save" is a reader that can write, and its log
        // line then claims a save the user never made. Nothing else in Load() touches the file.
        static void Load();

        // THE MIGRATIONS THIS FILE CARRIES, AS ONE PURE FUNCTION. In: a preference set as it was read
        // from disk. Out: the same set in this build's form, plus one line per thing it changed (empty =
        // there was nothing to do, which is what Load() tests before it writes anything). No file, no
        // globals, no logging — contract §4.4 asks a migration to be pure and tested, and
        // Desert/Tests/Editor/PreferenceOwnership calls this directly rather than through the file.
        //
        // TWO KINDS, and they are opposite halves of one rule about who may delete a key. It RAISES a
        // stored value this build's units no longer match, and it DROPS the keys this project retired by
        // name. Everything else the file happens to hold is somebody else's and survives untouched — see
        // UnknownKeys above. A deletion is a decision, so it is spelled out here; it is never the
        // side effect of one binary saving before another.
        static std::vector<std::string> MigrateLoaded( EditorPreferences& p );

        // THE OTHER MIGRATION, AND IT IS A DIFFERENT SHAPE BECAUSE IT COMES FROM A DIFFERENT FILE (К5).
        // MigrateLoaded() raises editor.json's own stored values; this folds the retired
        // `~/.desertengine/asset_favorites.txt` into FavouriteFolders. In: the lines that file held, the
        // project they are being read for, and that project's assets root. Out: one line per thing it did,
        // empty when there was nothing to do. Pure on the same terms as MigrateLoaded — no file, no
        // globals, no logging — so the rules below are testable without a home directory:
        //
        //   * a line INSIDE `assetsRoot` becomes a path relative to it, appended in file order and never
        //     duplicated (the legacy file could hold the same folder twice; nothing deduplicated it);
        //   * a line OUTSIDE it is DROPPED AND NAMED. It is a folder of some other project, and this
        //     build cannot say which: the file recorded no project, which is the defect being migrated
        //     away from. Carrying it into the open project's list would be inventing an answer;
        //   * `assetsRoot` itself becomes ".".
        //
        // §4 AND NOT A COMPATIBILITY PATH: it runs once, the caller then deletes the file, and no reader
        // of the pinned folders ever looks at the old location again. Two stores for one value is what
        // К5 was.
        static std::vector<std::string> MigrateFavouritesFile( EditorPreferences& p, const std::string& project,
                                                               const std::filesystem::path&    assetsRoot,
                                                               const std::vector<std::string>& lines );

        // THE USER JUST CHANGED SOMETHING. Called by every control that edits this struct, at the moment
        // the edit finishes (ImGui::IsItemDeactivatedAfterEdit for a slider or a drag, the click for a
        // checkbox or a menu item) — never on every frame of a drag, which would be sixty writes a second.
        //
        // IT CHANGES NO SETTING, and since К3 it touches nothing outside the file except UnknownKeys,
        // which is re-read from disk immediately before the write. The one derived push that used to
        // share this function (RenderConfig::MSAASamples) went with the field. Anything else here would
        // be a save that edits state the user did not touch in the action that triggered it — what К6
        // removed.
        //
        // WHY THE RE-READ IS PART OF SAVING AND NOT OF LOADING (К9). Several editors share one user's
        // file. An editor that started before a key existed holds a carrier that has never heard of it,
        // and would delete it on the next save — the defect again, one road further along. So the keys
        // this build cannot name are taken from the file at the moment of writing rather than remembered
        // from the moment of reading. KEYS ONLY: every field this struct declares is written from what the
        // user has in front of them, so two editors still resolve a real disagreement last-writer-wins.
        //
        // A SAVE THAT WOULD CHANGE NOTHING DOES NOT HAPPEN. The bytes are compared against WHAT THE FILE
        // SAYS — the re-read above supplies it, and this process's memo of its own last write is only the
        // fallback for a file that cannot be read (К8 had the memo alone; К9 made the disk the authority
        // it was always meant to stand in for). The file is confirmed still to be there, so the answer can
        // never be true of a file that is gone. An identical write is skipped — no file write,
        // and no log line about one. That is what makes commit-on-edit affordable: letting go of a control you
        // only hovered, re-picking the MSAA level you are already on, or dragging a slider back to where
        // it started all cost nothing. True means "editor.json holds these values", which is as true of a
        // skipped write as of a performed one; false means it could not be written (reason logged) and the
        // values are live in this session only.
        static bool Save();

        // THE EDITOR, NOT THE USER, RAISED A STORED VALUE TO THIS BUILD'S FORM — a migration write-back,
        // and the only legitimate reason to write this file without a user action behind it. `what` is
        // the sentence that reaches the log in place of the changed-field list Save() derives, because a
        // migration is not a field the user moved and reporting it as one is how "[Prefs] Saved ..." came
        // to mean nothing. Same deduplication and same return meaning as Save().
        static bool SaveMigrated( const std::string& what );
    };
} // namespace Desert::Editor
