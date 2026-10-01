#include "Editor/LevelEditor/ProfilerWindow.hpp"

#include "Editor/Core/ShotOptions.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/ShotDirector.hpp"

#include <Common/Core/Profiler.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <ImGui/imgui.h>

namespace Desert::Editor
{
    // The profiler table as text. Used by the panel's button AND by --gpu-profile, because a headless shot
    // draws no ImGui and the panel is the only other way these numbers are readable.
    //
    // The GPU column comes from the backend's timestamp queries, so it is device time, not the CPU's wait
    // for it; the two columns disagreeing is the interesting case rather than a fault.
    void ProfilerWindow::RecordFlightFrame( bool counted )
    {
        const ShotOptions&           shot     = ShotOptions::Get();
        Common::Profiling::Profiler& profiler = Common::Profiling::Profiler::Get();

        double gpuMs    = Flight::kNotMeasured;
        double streamMs = 0.0;
        for ( const Common::Profiling::ScopeResult& scope : profiler.LastFrame() )
        {
            if ( scope.Name == Common::Profiling::kGpuFrameTotalScope && profiler.GpuEnabled() )
                gpuMs = scope.GpuMs;
            else if ( scope.Name == "WorldStreamer::Tick" )
                streamMs = scope.TotalMs;
        }
        m_FlightLog.TimeLast( profiler.LastFrameMs(), gpuMs, streamMs );

        Flight::FrameRow row;
        row.Frame    = m_Shots.Frame();
        row.Kind     = m_Shots.Frame() < Flight::kWarmupFrames ? Flight::Phase::Warmup
                       : counted                               ? Flight::Phase::Flight
                                                               : Flight::Phase::Settling;
        row.Distance = Flight::DistanceAt( m_Shots.Frame(), shot.FlightSpeed, ShotOptions::PlayStepSeconds );
        row.Position = Flight::PoseAt( *shot.FlightRoute, row.Distance ).Position;
        row.Entities = m_Workspace.ActiveScene()->GetAllEntities().size();
        if ( m_Play.Streamer() && m_Play.Streamer()->Streams( *m_Workspace.ActiveScene() ) )
        {
            const auto& report   = m_Play.Streamer()->LastTick();
            row.ResidentRecords  = report.LiveRecords;
            row.UnitsActivated   = report.Tick.UnitsActivated;
            row.UnitsDeactivated = report.Tick.UnitsDeactivated;
            row.RecordsActivated = report.Tick.RecordsActivated;
            row.RecordsDestroyed = report.Tick.RecordsDestroyed;
            row.ActivationMs     = report.ActivationMs;
            row.ActivatedUnits   = report.ActivatedUnits;
        }
        row.AssetGpuBytes = Graphic::ResourceLedger::Take().BytesForOwner( Graphic::ResourceOwner::AssetService );
        m_FlightLog.Append( std::move( row ) );
    }

    bool ProfilerWindow::FinishFlight()
    {
        const ShotOptions& shot = ShotOptions::Get();
        const auto         rows = m_FlightLog.Rows();
        const auto         written =
             Common::Utils::FileSystem::WriteContentToFileAtomic( shot.FlightCsv, Flight::Csv( rows ) );
        if ( !written.IsSuccess() )
        {
            LOG_ERROR( "[Flight] the CSV was not written to '{}': {}", shot.FlightCsv, written.GetError() );
            return false;
        }
        const auto summary = Flight::Summarise( rows );
        if ( !summary.IsSuccess() )
        {
            LOG_ERROR( "[Flight] '{}': {}", shot.FlightRoute->Spec, summary.GetError() );
            return false;
        }
        LOG_INFO( "[Flight] '{}' at {:.0f} cm/s, {} row(s) in '{}': {}", shot.FlightRoute->Spec, shot.FlightSpeed,
                  rows.size(), shot.FlightCsv, Flight::Describe( summary.GetValue(), rows ) );
        return true;
    }

    void ProfilerWindow::DumpProfilerToLog()
    {
        auto& prof = ::Common::Profiling::Profiler::Get();

        const double frameMs = prof.LastFrameMs();
        const double fps     = frameMs > 0.0001 ? 1000.0 / frameMs : 0.0;

        const std::string frameTotalScope = ::Common::Profiling::kGpuFrameTotalScope;

        double gpuFrameMs = 0.0;
        double gpuSumMs   = 0.0;
        for ( const auto& s : prof.LastFrame() )
        {
            if ( s.Name == frameTotalScope )
                gpuFrameMs = s.GpuMs;
        }

        LOG_INFO( "[Profiler] ---- per-pass breakdown (averaged over {:.1f} s of frames) ----",
                  prof.AvgWindowSeconds() );
        LOG_INFO( "[Profiler] Frame (wall) {:.3f} ms ({:.0f} FPS), GPU frame {:.3f} ms", frameMs, fps,
                  gpuFrameMs );
        LOG_INFO( "[Profiler] {:<34} {:>10} {:>6} {:>10} {:>10} {:>6}", "scope", "cpu ms", "x", "gpu ms",
                  "gpu self", "x" );

        for ( const auto& s : prof.LastFrame() )
        {
            LOG_INFO( "[Profiler] {:<34} {:>10.3f} {:>6} {:>10.3f} {:>10.3f} {:>6}", s.Name, s.TotalMs, s.Calls,
                      s.GpuMs, s.GpuSelfMs, s.GpuCalls );
            // SELF time is the only summable column — the inclusive one counts a parent's microseconds
            // again in each child. The frame bracket is the denominator, not a pass, so it stays out.
            if ( s.GpuCalls > 0 && s.Name != frameTotalScope )
                gpuSumMs += s.GpuSelfMs;
        }

        LOG_INFO( "[Profiler] GPU self times sum to {:.3f} ms of a {:.3f} ms GPU frame ({:.1f} %); the "
                  "remainder is device work no pass is marked around.",
                  gpuSumMs, gpuFrameMs, gpuFrameMs > 0.0001 ? gpuSumMs / gpuFrameMs * 100.0 : 0.0 );

        // THE DRAW-CALL DETECTOR, READ WITHOUT A WINDOW. The counter itself landed with step 2 of
        // `Docs/World/PROGRAMME.md`, and its only reader was the viewport's perf HUD — which is ImGui,
        // which is drawn into the swapchain, which `--shot` does not read. So the one number step 3 is
        // judged on was unreadable in exactly the mode a measurement is taken in. It joins the profiler
        // dump rather than getting a flag of its own because it answers the same question the dump does
        // — what did this frame cost — and because a second flag would be a second thing to remember.
        const Graphic::DrawCounters drawCounters = Graphic::DrawCounter::LastFrame();
        LOG_INFO( "[Profiler] draws {} / instances {} (an instanced batch is ONE draw and many instances; "
                  "the two are printed apart because culling moves them in different proportions)",
                  drawCounters.Draws, drawCounters.Instances );
        LOG_INFO( "[Profiler] ---- end ----" );
    }

    void ProfilerWindow::DrawProfilerWindow()
    {
        namespace ImGui = ::ImGui;
        auto& prof      = ::Common::Profiling::Profiler::Get();

        if ( !m_ShowProfiler )
            return;

        const double frameMs = prof.LastFrameMs();
        const double fps     = frameMs > 0.0001 ? 1000.0 / frameMs : 0.0;

        ImGui::SetNextWindowSize( ImVec2( 420, 460 ), ImGuiCond_FirstUseEver );
        ImGui::SetNextWindowPos( ImVec2( 700, 120 ), ImGuiCond_FirstUseEver );
        if ( !ImGui::Begin( "Profiler", &m_ShowProfiler ) ) // X button clears m_ShowProfiler
        {
            ImGui::End();
            return;
        }

        ImGui::Checkbox( "Enabled", &prof.Enabled() );
        ImGui::SameLine();
        ImGui::Checkbox( "Sort by time", &prof.SortByTime() );
        ImGui::SameLine();
        // GPU timestamps are OFF by default: they cost ~8 % of a debug frame on MoltenVK, and an
        // always-on instrument means every later measurement carries the tax. Turning this on is a
        // deliberate act. See Docs/GPU_TIMESTAMPS.md for the measured price.
        ImGui::BeginDisabled( prof.GetGpuSink() == nullptr );
        ImGui::Checkbox( "GPU", &prof.GpuEnabled() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Device timestamps around every pass.\n"
                               "Costs about 8%% of the frame it measures, so it is off by default." );
        ImGui::SameLine();
        ImGui::BeginDisabled( !prof.GpuEnabled() );
        ImGui::Checkbox( "per-pass", &prof.GpuPassScopes() );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "Off: time the whole frame only (two timestamps, near-free).\n"
                               "On: also time every pass, which is what costs." );
        ImGui::EndDisabled();
        if ( prof.GetGpuSink() == nullptr && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "This device reports no usable timestamp queries — CPU columns only." );

        ImGui::SetNextItemWidth( 160.0f );
        ImGui::SliderFloat( "Avg window (s)", &prof.AvgWindowSeconds(), 0.1f, 2.0f, "%.1f" );

        // The whole-frame GPU bracket the backend records around the command buffer. It is the denominator
        // the per-pass GPU column is checked against: the passes should tile it, not exceed it.
        double gpuFrameMs = 0.0;
        for ( const auto& s : prof.LastFrame() )
            if ( s.Name == ::Common::Profiling::kGpuFrameTotalScope )
                gpuFrameMs = s.GpuMs;

        ImGui::Text( "Frame: %.3f ms  (%.0f FPS)   [avg]", frameMs, fps );
        if ( gpuFrameMs > 0.0 )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "GPU: %.3f ms", gpuFrameMs );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Dump to Log" ) )
            DumpProfilerToLog();

        ImGui::Separator();

        if ( ImGui::BeginTable( "##prof", 6,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_SizingStretchProp ) )
        {
            // The numeric columns are FIXED width and the name stretches. With six columns sharing the
            // width proportionally, the panel docked at its usual size truncated every header to
            // "cp... gp... gpu..." — unreadable, and the two GPU columns are the ones a reader has to
            // tell apart. A millisecond figure needs a known number of characters, not a share of the
            // panel, so it gets one.
            const float kNumWidth = ImGui::CalcTextSize( "0000.000" ).x;
            ImGui::TableSetupColumn( "scope", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn( "cpu", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "gpu", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "self", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "%", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableSetupColumn( "x", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableHeadersRow();

            for ( const auto& s : prof.LastFrame() )
            {
                const double pct = frameMs > 0.0001 ? ( s.TotalMs / frameMs ) * 100.0 : 0.0;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( s.Name.c_str() );
                // Docked at its usual width the name column clips, and "Clouds: Sha" / "Clouds: Exe" are
                // two different passes. The full name on hover costs nothing and settles it.
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", s.Name.c_str() );
                ImGui::TableNextColumn();
                ImGui::Text( "%.3f", s.TotalMs );
                ImGui::TableNextColumn();
                // A dash, not 0.000: a scope that records no GPU work and a scope the GPU timer could not
                // reach are different states, and printing zero for both invents a measurement.
                if ( s.GpuCalls > 0 )
                    ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "%.3f", s.GpuMs );
                else
                    ImGui::TextDisabled( "-" );
                ImGui::TableNextColumn();
                // Nested passes subtracted — the column that can be added up.
                if ( s.GpuCalls > 0 )
                    ImGui::TextColored( ImVec4( 0.45f, 0.70f, 0.95f, 1.0f ), "%.3f", s.GpuSelfMs );
                else
                    ImGui::TextDisabled( "-" );
                ImGui::TableNextColumn();
                // Tint hot scopes (>25% of the frame) red.
                if ( pct > 25.0 )
                    ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%.1f", pct );
                else
                    ImGui::Text( "%.1f", pct );
                ImGui::TableNextColumn();
                ImGui::Text( "%u", s.Calls );
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    void ProfilerWindow::DrawEngineStats( float rightMargin )
    {
        namespace ImGui = ::ImGui;

        const auto text = m_Application->GetEngineStats().GetFormattedStats();
        auto       size = ImGui::CalcTextSize( text.c_str() );

        ImGui::SameLine( ImGui::GetWindowContentRegionMax().x - rightMargin - size.x -
                         ImGui::GetStyle().ItemSpacing.x * 2.0f );

        // TextUnformatted, not Text: ImGui::Text takes a printf FORMAT, so this passed runtime-built
        // engine stats as the format string. Today GetFormattedStats() can only produce
        // "FPS: 60 | Frame: 16.67ms" and contains no '%', so nothing has gone wrong — but the day any
        // percentage is added to that line (a GPU utilisation, a budget fraction — the obvious next
        // additions) ImGui's vsnprintf reads a vararg that was never passed.
        //
        // The example above said "16.6ms" while the function was printing SIX decimals — the argument
        // was right and the sample output was a different program's. It is two decimals now because
        // GetFormattedStats was fixed, not because the comment was made to agree with it.
        ImGui::TextUnformatted( text.c_str() );
    }
} // namespace Desert::Editor
