#pragma once

#include "../IPanel.hpp"

#include <Engine/Project/GameSettings.hpp>

#include <string>

namespace Desert::Editor
{
    // UE's Project Settings ▸ Project ▸ Description (Company) and ▸ Movies, over the one file that holds them:
    // `<project>/Config/Game.json` (Engine/Project/GameSettings.hpp). The panel keeps no settings of its own —
    // it edits a copy of CurrentGameSettings() and every committed change is written through
    // Project::SaveGameSettings at once, so the Runtime player reads exactly what this window shows.
    class ProjectSettingsPanel final : public IPanel
    {
    public:
        ProjectSettingsPanel();

        void OnUIRender() override;

    private:
        void DrawGameSection();
        void DrawMoviesSection();
        void Commit();

        // The project directory m_Edit was copied for: a different project (or none) re-copies it.
        std::string                     m_LoadedFor;
        ::Desert::Project::GameSettings m_Edit;
        // The last refusal (a save that failed, a drop that is not a movie of this project), shown until the
        // next successful action. Empty = nothing to report.
        std::string m_Problem;
    };
} // namespace Desert::Editor
