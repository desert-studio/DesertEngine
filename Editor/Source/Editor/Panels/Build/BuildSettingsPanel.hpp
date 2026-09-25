#pragma once

#include <cstddef>

#include "../IPanel.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // The "package the game" dialog. Build BAKES the open project into a self-contained game folder
    // (Runtime binary + Assets + Cooked + engine shaders + launcher) via GamePackager, on a JobSystem
    // worker so the UI never stalls. Hidden by default; enable via View -> Build Settings.
    //
    // THE PANEL HOLDS NO SETTINGS OF ITS OWN. The three packaging answers live in EditorPreferences
    // (editor.json) and are edited there in place; the target platform is not an answer at all, because
    // this editor packages for its own host and for nothing else — see Editor/Packaging/PackageTarget.hpp
    // for why, and the panel says so on screen rather than offering a choice it cannot honour.
    //
    // Everything declared below is bookkeeping for the async job and the startup-scene combo, and
    // Desert/Tests/Editor/BuildSettingsConsumers is the census that holds that line: a member this panel
    // puts inside an editing widget has to name the code that reads it.
    class BuildSettingsPanel final : public IPanel
    {
    public:
        BuildSettingsPanel() : IPanel( "Build Settings", /*showPanel=*/false )
        {
        }

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 560.0f, 640.0f };
        }

        void OnUIRender() override;

    private:
        void RescanScenes(); // fills m_Scenes with project-relative .desce paths

        // Async packaging state (worker writes, UI reads).
        std::atomic<bool> m_Building{ false };
        std::atomic<bool> m_HasResult{ false };
        bool              m_LastSuccess = false;
        // COMPLETE IS NOT SUCCESS, and the panel has to paint the difference: a package exists in both
        // cases, but an incomplete one ships assets the player's machine will cook at every start (or
        // that are broken outright). Before I12 both were the same green, so the last step before a
        // build reaches a player was the one step that could not report a partial result.
        bool        m_LastComplete      = false;
        size_t      m_LastCookFailures  = 0;
        size_t      m_LastCookUnwritten = 0;
        std::string m_LastMessage; // guarded by the m_Building/m_HasResult handshake
        std::string m_LastPackageDir;
        std::string m_SchemeMessage; // UI thread only: what the last "Create default" click did
        // Where the release's patch baseline was recorded, or empty when this build produced none
        // (every Rebuild Content.dpak — a dev archive is not a release). Shown because it is the one
        // artifact of a package that has to be KEPT: the next update is built by comparing against it,
        // and it cannot be recreated once this version is gone.
        std::string m_LastManifestPath;

        // Startup-scene picker: the .desce scenes found under the project (relative to the project
        // dir), scanned lazily on first render and via the Rescan button.
        std::vector<std::string> m_Scenes;
        bool                     m_ScenesScanned = false;
    };
} // namespace Desert::Editor
