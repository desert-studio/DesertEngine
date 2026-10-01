#pragma once

// THE PROFILER WINDOW (UE: the Stats / SProfilerWindow pair). The CPU+GPU scope table as a window and as log
// lines (--gpu-profile), the engine stats at the right of the menu bar, and the --flight capture's per-frame rows.
//
// A member of EditorLayer BY VALUE; what it reads arrives by reference. It knows nothing of EditorLayer: the
// layer decides WHEN (a frame of Play under --flight, the capture's last frame, the menu bar) and this decides
// WHAT.

#include "Editor/Core/FlightRules.hpp"

namespace Desert::Engine
{
    class Application;
}

namespace Desert::Editor
{
    class PlaySession;
    class SceneWorkspace;
    class ShotDirector;

    class ProfilerWindow
    {
    public:
        ProfilerWindow( Engine::Application* application, SceneWorkspace& workspace, PlaySession& play,
                        const ShotDirector& shots )
             : m_Application( application ), m_Workspace( workspace ), m_Play( play ), m_Shots( shots )
        {
        }

        // View ▸ Profiler and the toolbar toggle this.
        [[nodiscard]] bool& Shown()
        {
            return m_ShowProfiler;
        }

        /// @p rightMargin is how much of the bar's right-hand end is already spoken for — the window
        /// buttons — so the stats right-align against them instead of underneath them.
        void DrawEngineStats( float rightMargin );
        void DrawProfilerWindow();
        /// The profiler's CPU+GPU table as log lines — the panel's button and --gpu-profile share it.
        void DumpProfilerToLog();
        /// --flight: times the previous frame's row and appends this frame's (Editor/Core/FlightRules.hpp).
        /// @p counted is whether the capture counts this frame; an uncounted one is a Settling row.
        void RecordFlightFrame( bool counted );
        /// --flight, on the last frame: writes the CSV and logs the summary. False when either failed.
        [[nodiscard]] bool FinishFlight();

    private:
        Engine::Application* m_Application;
        SceneWorkspace&      m_Workspace;
        PlaySession&         m_Play;
        const ShotDirector&  m_Shots;

        bool m_ShowProfiler = true; // View ▸ Profiler toggles the profiler window
        // --flight: one row per frame of Play, written as the CSV when the capture ends.
        Flight::FlightLog m_FlightLog;
    };
} // namespace Desert::Editor
