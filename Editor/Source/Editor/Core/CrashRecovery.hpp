#pragma once

#include "AutosavePaths.hpp"

#include <filesystem>

namespace Desert::Editor
{
    // Crash recovery for the scene editor. A lock file lives in the autosave folder while the editor
    // runs; a clean shutdown removes it. If the lock is still there at the next start, the previous
    // session did not exit cleanly — and if an autosave exists, the editor offers to reopen it.
    //
    // This pairs with the autosave timer (EditorLayer + EditorPreferences::AutosaveMinutes), which writes
    // <Project>/Saved/Autosaves/<scene path mirror>_autosave.desce (see AutosavePaths.hpp for why it is
    // not under the assets root); recovery reopens the newest one through the normal scene-load path,
    // bound to the scene it stands for (Autosave::SceneFor).
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

        // What recovery offers: the newest copy at THIS build's scene schema, and every copy it does not
        // offer (another generation) by path and stated version.
        //
        // AUTOSAVES ARE NOT MIGRATED (lead decision, AUTO1). Saved/Autosaves is machine-local scratch, not
        // content: no process raises it, and the schema migrator never sees it as an assets root (it once
        // created a material tree beside a copy it was pointed at). A copy from another generation is
        // reported and left alone; the scene it stands for is in the repository at the current schema.
        static Autosave::RecoveryChoice ChooseAutosave();
    };
} // namespace Desert::Editor
