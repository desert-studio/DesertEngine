#pragma once

// THE PROFILER WINDOW (UE: the Stats / SProfilerWindow pair). The CPU+GPU scope table as a window and as log
// lines (--gpu-profile), the engine stats at the right of the menu bar, and the --flight capture's per-frame rows.
//
// A member of EditorLayer BY VALUE; what it reads arrives by reference. It knows nothing of EditorLayer: the
// layer decides WHEN (a frame of Play under --flight, the capture's last frame, the menu bar) and this decides
// WHAT.

#include "Editor/Core/FlightRules.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace Desert::Engine
{
    class Application;
}

namespace Desert::Graphic
{
    class SceneRenderer;
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
        /// @p renderer: the view whose draw bins and mesh passes the DrawBins / MeshPasses lines read (the
        /// workspace's primary renderer; null = those lines say none). A parameter, not m_Workspace: the
        /// dump is static, called by the shot director, the control channel and the palette.
        static void DumpProfilerToLog( const Graphic::SceneRenderer* renderer );
        /// --flight: times the previous frame's row and appends this frame's (Editor/Core/FlightRules.hpp).
        /// @p counted is whether the capture counts this frame; an uncounted one is a Settling row.
        void RecordFlightFrame( bool counted );
        /// --flight, on the last frame: writes the CSV and logs the summary. False when either failed.
        [[nodiscard]] bool FinishFlight();

    private:
        // THE TABLE'S CELLS, formatted ONCE PER PUBLICATION (UE: the stat HUD formats a group when the stats
        // thread publishes a frame, Insights' tables virtualise their rows). The profiler publishes a window
        // twice a second; the window is painted every frame, so the numbers are printed here when
        // Profiler::PublishCount moves and the paint only submits the rows the clipper says are visible.
        struct TableRow
        {
            std::array<char, 16> Cpu{};
            std::array<char, 16> Wait{}; // "-" when the scope waited for nothing
            std::array<char, 16> Gpu{};  // "-" when no GPU time reached the row
            std::array<char, 16> GpuSelf{};
            std::array<char, 16> Pct{};
            std::array<char, 16> Calls{};
            bool                 Waited = false;
            bool                 Timed  = false; // GPU columns carry a measurement
            bool                 Hot    = false; // over 25 % of the frame
        };
        // Re-formats m_TableRows from the profiler's published window when it is a new one.
        void RefreshTable();

        Engine::Application* m_Application;
        SceneWorkspace&      m_Workspace;
        PlaySession&         m_Play;
        const ShotDirector&  m_Shots;

        std::vector<TableRow> m_TableRows;                    // by row of Profiler::LastFrame
        uint64_t              m_TablePublish    = UINT64_MAX; // the publication m_TableRows was formatted from
        double                m_TableGpuFrameMs = 0.0;        // that publication's GPU frame bracket

        bool m_ShowProfiler = true; // View ▸ Profiler toggles the profiler window
        // --flight: one row per frame of Play, written as the CSV when the capture ends.
        Flight::FlightLog m_FlightLog;
    };
} // namespace Desert::Editor
