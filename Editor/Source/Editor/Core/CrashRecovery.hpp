#pragma once

#include <filesystem>

namespace Desert::Editor
{
    // Crash recovery for the scene editor. A lock file lives in the autosave folder while the editor
    // runs; a clean shutdown removes it. If the lock is still there at the next start, the previous
    // session did not exit cleanly — and if an autosave exists, the editor offers to reopen it.
    //
    // This pairs with the existing autosave timer (EditorLayer + EditorPreferences::AutosaveMinutes),
    // which periodically writes Scene/Autosave/<name>_autosave.desce; recovery just reopens the newest
    // one through the normal scene-load path. Project-scoped via Constants::Path::SCENE_PATH.
    class CrashRecovery
    {
    public:
        // Autosave directory and the session lock inside it.
        static std::filesystem::path AutosaveDir();
        static std::filesystem::path LockPath();

        // Call ONCE at startup, before ArmSession(): true if the last session left the lock behind.
        static bool WasUncleanExit();

        // Create/refresh the session lock ("editor running"). FALSE when the lock could not be
        // written, which means WasUncleanExit() will report "clean" after a crash of THIS session —
        // the caller is the only place that can say so while the fact is still knowable.
        static bool ArmSession();

        // Remove the session lock (clean shutdown).
        static void DisarmSession();

        // Newest *_autosave.desce in the autosave dir, or empty when there is none.
        static std::filesystem::path LatestAutosave();

        // RAISE EVERY AUTOSAVE IN THIS DIRECTORY TO THE CURRENT SCHEMA, IN PLACE, AND SAY WHAT WAS DONE.
        //
        // WHY THE EDITOR DOES THIS AND NOT THE MIGRATOR TOOL. Every other `.desce` in this project is
        // converted by a task: the schema is raised, `Tools/SceneMigrator` is run over the repository,
        // and the converted files are committed. `Scenes/Autosave/` cannot be converted that way and the
        // reason is structural rather than an oversight — it is in `.gitignore`, so it does not exist in
        // the worktree where a schema step is written, and it is not in the commit where the corpus is
        // converted. The schema then reaches the owner's machine and five of his autosaves stop opening
        // (measured on the v17 -> v18 raise; he migrated them by hand). Pointing the tool at the
        // directory does not fix that: somebody would have to remember to run it, on a machine the task
        // never touched, which is exactly the step that failed.
        //
        // So the process that WRITES these files is the one that raises them. That is not the "engine
        // migrates on load" arrangement the contract retired — the engine's loader still refuses a scene
        // that is not at the head, and the Runtime never opens an autosave at all. It is one directory,
        // owned by one program, converted once, on disk, through the SAME function the tool runs
        // (`Migration::RunSceneMigrator`), so there is no second statement of the migration to drift.
        //
        // LOUD IN BOTH DIRECTIONS (§1.4): every file raised is logged with its name and the generations
        // it moved between, and every file that could NOT be raised is logged as an error naming the
        // file — losing somebody's recovery copy quietly is the one outcome worth more than the rest.
        // Returns false when at least one autosave was refused, so the caller can tell the user.
        static bool MigrateAutosaves();
    };
} // namespace Desert::Editor
