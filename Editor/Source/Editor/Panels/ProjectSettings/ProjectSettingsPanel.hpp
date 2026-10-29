#pragma once

#include <Editor/Core/IPanel.hpp>

#include <Projects/GameSettings.hpp>
#include <Projects/RendererSettings.hpp>

#include <string>

namespace Desert::Editor
{
    // UE's Project Settings ▸ Project ▸ Description (Company) and ▸ Movies, over the one file that holds them:
    // `<project>/Config/Game.json` (Engine/Project/GameSettings.hpp); and ▸ Engine ▸ Rendering over
    // `<project>/Config/Engine.json` (Engine/Project/RendererSettings.hpp, UE DefaultEngine.ini). The panel keeps no settings of its own —
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
        void DrawRenderingSection();
        void Commit();
        void CommitRendering();

        // The project directory m_Edit was copied for: a different project (or none) re-copies it.
        std::string                     m_LoadedFor;
        ::Desert::Project::GameSettings m_Edit;
        ::Desert::Project::RendererSettings m_EditRendering;
        // The last refusal (a save that failed, a drop that is not a movie of this project), shown until the
        // next successful action. Empty = nothing to report.
        std::string m_Problem;
    };
} // namespace Desert::Editor
