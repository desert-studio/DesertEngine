#pragma once

// SESSION RECOVERY (UE: FPackageAutoSaver, UnrealEd/Private/PackageAutoSaver.cpp). Everything the editor
// does so that a crash or a lost device does not cost the user their work:
//   - at start, the lock the previous session left behind (an unclean exit) arms the recovery pop-up with
//     the newest autosave at this build's scene schema, and THIS session's lock is written
//     (UE: OfferToRestorePackages + UpdateRestoreFile(true));
//   - every frame, the timed autosave of the active scene to a SEPARATE file under
//     <Project>/Saved/Autosaves (UE: UpdateAutoSaveCount + AttemptAutoSave);
//   - at a device loss, the one emergency save, of the authored scene;
//   - at a clean shutdown, the lock is removed (UE: UpdateRestoreFile(false)).
//
// A member of EditorLayer BY VALUE, declared after the modules it reads. The files themselves (lock, the
// autosave naming and the choice of what to offer) live in Editor/Core/CrashRecovery and AutosavePaths;
// this is the one thing that acts on them during a session.

#include <cstdint>
#include <memory>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class DockLayout;
    class PlaySession;
    class SceneFiles;
    class SceneWorkspace;

    class SessionRecovery
    {
    public:
        SessionRecovery( SceneWorkspace& workspace, SceneFiles& sceneFiles, PlaySession& play,
                         const std::shared_ptr<Assets::AssetManager>& assetManager );

        // After an unclean exit, hands the newest autosave this build can open to the dock's recovery
        // pop-up (copies at another scene schema are named in the log and left alone); then writes this
        // session's lock. A lock that could not be written is said in a toast: recovery is off.
        void OfferAndArm( DockLayout& dock ) const;

        // The timed autosave: Edit mode only, every EditorPreferences::AutosaveMinutes of wall-clock time,
        // and only when the command revision moved since the last autosave that LANDED.
        void Tick( float seconds );

        // Once, at shutdown after a device loss: the authored scene (Play's snapshot in Play mode) to its own
        // recovery file, so that a good periodic autosave is never clobbered. No-op when the device is fine.
        void SaveOnDeviceLost() const;

        // Clean shutdown: the lock goes, so the next start does not think this one crashed.
        static void Disarm();

    private:
        SceneWorkspace&                              m_Workspace;
        SceneFiles&                                  m_SceneFiles;
        PlaySession&                                 m_Play;
        const std::shared_ptr<Assets::AssetManager>& m_AssetManager;

        float    m_AutosaveAccum        = 0.0f;
        uint64_t m_LastAutosaveRevision = 0;
    };
} // namespace Desert::Editor
