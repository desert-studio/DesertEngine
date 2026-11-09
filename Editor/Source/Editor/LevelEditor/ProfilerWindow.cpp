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
#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>
#include <Engine/Graphic/GpuDriven/CullPhaseCounts.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
        // The caller records only while a flight is armed; without a route there is no position to log.
        if ( !shot.FlightRoute.has_value() )
            return;
        const Flight::Route& route = *shot.FlightRoute;

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
        row.Frame = m_Shots.Frame();
        if ( m_Shots.Frame() < Flight::kWarmupFrames )
            row.Kind = Flight::Phase::Warmup;
        else if ( counted )
            row.Kind = Flight::Phase::Flight;
        else
            row.Kind = Flight::Phase::Settling;
        row.Distance = Flight::DistanceAt( m_Shots.Frame(), shot.FlightSpeed, ShotOptions::PlayStepSeconds );
        row.Position = Flight::PoseAt( route, row.Distance ).Position;
        row.Entities = m_Workspace.ActiveScene()->GetAllEntities().size();
        if ( m_Play.Streamer() != nullptr && m_Play.Streamer()->Streams( *m_Workspace.ActiveScene() ) )
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
        if ( !shot.FlightRoute.has_value() )
        {
            LOG_ERROR( "[Flight] finished with no flight route armed; there is no flight to write" );
            return false;
        }
        const Flight::Route& route = *shot.FlightRoute;
        const auto           rows  = m_FlightLog.Rows();
        const auto           written =
             Common::Utils::FileSystem::WriteContentToFileAtomic( shot.FlightCsv, Flight::Csv( rows ) );
        if ( !written.IsSuccess() )
        {
            LOG_ERROR( "[Flight] the CSV was not written to '{}': {}", shot.FlightCsv, written.GetError() );
            return false;
        }
        const auto summary = Flight::Summarise( rows );
        if ( !summary.IsSuccess() )
        {
            LOG_ERROR( "[Flight] '{}': {}", route.Spec, summary.GetError() );
            return false;
        }
        LOG_INFO( "[Flight] '{}' at {:.0f} cm/s, {} row(s) in '{}': {}", route.Spec, shot.FlightSpeed, rows.size(),
                  shot.FlightCsv, Flight::Describe( summary.GetValue(), rows ) );
        return true;
    }

    void ProfilerWindow::DumpProfilerToLog( const Graphic::SceneRenderer* const renderer )
    {
        auto& prof = ::Common::Profiling::Profiler::Get();

        const double frameMs = prof.LastFrameMs();
        const double fps     = frameMs > 0.0001 ? 1000.0 / frameMs : 0.0;

        const std::string      frameTotalScope = ::Common::Profiling::kGpuFrameTotalScope;
        const std::string_view gapPrefix       = ::Common::Profiling::kGpuGapScopePrefix;
        // The remainder rows (Profiler.hpp): published beside the passes, never one of them.
        const auto isRemainder = [&]( const std::string_view name )
        {
            return name == frameTotalScope || name == ::Common::Profiling::kGpuUnaccountedGraphicsScope ||
                   name == ::Common::Profiling::kGpuUnaccountedAsyncScope ||
                   std::string_view( name ).starts_with( gapPrefix );
        };

        double gpuFrameMs         = 0.0;
        double gpuSumMs           = 0.0;
        double unaccountedMs      = 0.0;
        double asyncUnaccountedMs = -1.0; // no row: no async segment was timed
        for ( const auto& s : prof.LastFrame() )
        {
            if ( s.Name == frameTotalScope )
                gpuFrameMs = s.GpuMs;
            else if ( s.Name == ::Common::Profiling::kGpuUnaccountedGraphicsScope )
                unaccountedMs = s.GpuMs;
            else if ( s.Name == ::Common::Profiling::kGpuUnaccountedAsyncScope )
                asyncUnaccountedMs = s.GpuMs;
        }

        LOG_INFO( "[Profiler] ---- per-pass breakdown (averaged over {:.1f} s of frames) ----",
                  prof.AvgWindowSeconds() );
        // CLOUD-MARCH-COST-4: the rows are the last CLOSED wall-clock window, not the dump's frame. On a
        // turning camera (FPS-FIELD-BARN-TURN) its frame count and age place the measured arc on the turn.
        // A scenario `frames N` wait is a frame-exact capture (ControlService): the rows are exactly its frames.
        const char* const windowKind = prof.LastWindowCaptured() ? "frame-exact capture" : "time window";
        LOG_INFO( "[Profiler] window: {} frame(s), {}, closed {:.0f} ms before this dump",
                  prof.LastWindowFrames(), windowKind, prof.LastWindowAgeMs() );
        prof.ReleaseCapture(); // this dump read the held capture: time windows resume
        LOG_INFO( "[Profiler] Frame (wall) {:.3f} ms ({:.0f} FPS), GPU frame {:.3f} ms", frameMs, fps,
                  gpuFrameMs );
        // GRASS-VRAM-BUDGET-1: the device-local memory the last sampled frame ran in (MemoryWatch). A frame past
        // its budget is paged by the driver, and the GPU times below then say nothing about their passes.
        if ( const uint64_t budget = Graphic::MemoryWatch::LastDeviceLocalBudget(); budget != 0u )
        {
            constexpr double MB    = 1024.0 * 1024.0;
            const uint64_t   usage = Graphic::MemoryWatch::LastDeviceLocalUsage();
            std::string_view state = "within it";
            if ( Graphic::MemoryWatch::OverBudget() )
                state = "OVER it";
            LOG_INFO( "[Profiler] device-local {:.0f} MB of a {:.0f} MB budget ({}), run peak {:.0f} MB",
                      static_cast<double>( usage ) / MB, static_cast<double>( budget ) / MB, state,
                      static_cast<double>( Graphic::MemoryWatch::PeakDeviceLocalUsage() ) / MB );
        }
        // cpu self: the row with its nested CPU scopes subtracted; cpu wait: the part of cpu ms blocked in a
        // Wait scope (fence, acquire, present, pacer) - cpu ms minus cpu wait is the row's real work.
        // cpu max / cpu p99: the row's cpu ms in its costliest frame of the window and the 99th percentile over
        // the window's frames (CPU-PROFILER-ZERO-COST; appended after the old columns, which keep their places).
        LOG_INFO( "[Profiler] {:<34} {:>10} {:>6} {:>10} {:>10} {:>6} {:>10} {:>10} {:>10} {:>10}", "scope",
                  "cpu ms", "x", "gpu ms", "gpu self", "x", "cpu self", "cpu wait", "cpu max", "cpu p99" );

        // GPU-PASS-TIMES-1: THE JOIN. A pass's CPU scope is one row ("Deferred: GBuffer"), its timestamp pair
        // another: the backend names a view's GPU rows "<view> | <pass>" (GpuViewScopeName), so several views keep
        // their passes apart. Read side by side they were two rows, the CPU one with a 0.000 GPU column and the
        // GPU one sorted to the table's bottom by its 0 cpu ms - the table read as "no pass costs GPU time".
        // Here a view's GPU row is joined onto the CPU row of its pass (summed over views, as the CPU row sums
        // them), and the per-view rows are listed apart, largest first, in the GPU table below.
        const std::vector<::Common::Profiling::ScopeResult>& rows = prof.LastFrame();
        struct JoinedGpu
        {
            double   Ms     = 0.0;
            double   SelfMs = 0.0;
            uint32_t Calls  = 0;
        };
        std::vector<JoinedGpu> joined( rows.size() );
        std::vector<uint8_t>   joinedAway( rows.size(), 0 ); // 1: a view's GPU row now read on its pass's row
        {
            std::unordered_map<std::string_view, size_t> rowByName;
            rowByName.reserve( rows.size() );
            for ( size_t i = 0; i < rows.size(); ++i )
                rowByName.emplace( rows[i].Name, i );
            for ( size_t i = 0; i < rows.size(); ++i )
            {
                const std::string_view name = rows[i].Name;
                const size_t           view = name.find( " | " );
                if ( rows[i].GpuCalls == 0 || rows[i].Calls != 0 || view == std::string_view::npos )
                    continue;
                const auto pass = rowByName.find( name.substr( view + 3 ) );
                if ( pass == rowByName.end() )
                    continue;
                // Only onto a CPU row with no GPU time of its own: a row timed under its own name stays as is.
                const ::Common::Profiling::ScopeResult& cpuRow = rows[pass->second];
                if ( cpuRow.Calls == 0 || cpuRow.GpuCalls != 0 )
                    continue;
                JoinedGpu& into = joined[pass->second];
                into.Ms += rows[i].GpuMs;
                into.SelfMs += rows[i].GpuSelfMs;
                into.Calls += rows[i].GpuCalls;
                joinedAway[i] = 1;
            }
        }

        double gpuAsyncSumMs = 0.0;
        for ( size_t i = 0; i < rows.size(); ++i )
        {
            const auto& s = rows[i];
            // SELF time is the only summable column — the inclusive one counts a parent's microseconds
            // again in each child. The frame bracket is the denominator and the Unaccounted rows are its
            // remainder, not passes, so they stay out. Only the GRAPHICS queue's self time tiles the frame
            // bracket: the async compute queue is another timeline overlapping it, summed apart.
            if ( s.GpuCalls > 0 && !isRemainder( s.Name ) )
            {
                gpuSumMs += s.GpuSelfMs - s.GpuAsyncSelfMs;
                gpuAsyncSumMs += s.GpuAsyncSelfMs;
            }
            if ( std::string_view( s.Name ).starts_with( gapPrefix ) || joinedAway[i] != 0 )
                continue; // gaps are printed below, largest first; a joined view row in the GPU table
            JoinedGpu gpu{ s.GpuMs, s.GpuSelfMs, s.GpuCalls };
            if ( joined[i].Calls != 0 )
                gpu = joined[i];
            LOG_INFO( "[Profiler] {:<34} {:>10.3f} {:>6} {:>10.3f} {:>10.3f} {:>6} {:>10.3f} {:>10.3f} {:>10.3f} "
                      "{:>10.3f}",
                      s.Name, s.TotalMs, s.Calls, gpu.Ms, gpu.SelfMs, gpu.Calls, s.SelfMs, s.WaitMs, s.MaxMs,
                      s.P99Ms );
        }

        // THE GPU TABLE (UE "stat gpu"): every timed GPU row - per view, async segments and waits included - by
        // self time, largest first, with the queue it ran on. The last column is a word, so a reader of the
        // per-pass rows' numeric columns (scripts/Dev/profiler_compare.py) does not take these rows for them.
        {
            std::vector<const ::Common::Profiling::ScopeResult*> timed;
            for ( const auto& s : rows )
                if ( s.GpuCalls > 0 && !isRemainder( s.Name ) )
                    timed.push_back( &s );
            std::sort( timed.begin(), timed.end(),
                       []( const auto* a, const auto* b ) { return a->GpuSelfMs > b->GpuSelfMs; } );
            LOG_INFO( "[Profiler] ---- GPU per pass ({} timed row(s), gpu self largest first) ----",
                      timed.size() );
            LOG_INFO( "[Profiler] {:<60} {:>10} {:>10} {:>6} {:>8}", "gpu scope", "gpu self", "gpu ms", "x",
                      "queue" );
            for ( const ::Common::Profiling::ScopeResult* s : timed )
            {
                std::string_view queue = "graphics";
                if ( s->GpuAsyncSelfMs > 0.0 && s->GpuAsyncSelfMs + 1e-9 < s->GpuSelfMs )
                    queue = "both";
                else if ( s->GpuAsyncSelfMs > 0.0 )
                    queue = "async";
                LOG_INFO( "[Profiler] {:<60} {:>10.3f} {:>10.3f} {:>6} {:>8}", s->Name, s->GpuSelfMs, s->GpuMs,
                          s->GpuCalls, queue );
            }
        }

        // [Hitch]: each frame of the window over kHitchFactor x its median wall time (slowest first, at most
        // kMaxPublishedHitches) with its five largest self rows; one "none" line when no frame was.
        if ( prof.LastHitches().empty() )
            LOG_INFO( "[Hitch] none: no frame of {} over {:.1f}x the window median {:.3f} ms",
                      prof.LastWindowFrames(), Common::Profiling::kHitchFactor, prof.LastFrameMedianMs() );
        for ( const Common::Profiling::HitchFrame& hitch : prof.LastHitches() )
        {
            std::string top;
            for ( const auto& [name, selfMs] : hitch.TopRows() )
            {
                if ( !top.empty() )
                    top += ", ";
                top += fmt::format( "{} {:.3f}", name, selfMs );
            }
            LOG_INFO( "[Hitch] frame {} of {}: {:.3f} ms = {:.2f}x median {:.3f} ms ({} hitch(es)); top self: {}",
                      hitch.Frame, prof.LastWindowFrames(), hitch.FrameMs, hitch.FrameMs / hitch.MedianMs,
                      hitch.MedianMs, prof.LastHitchCount(), top );
        }

        // UNACCOUNTED, CPU (UE "stat unit"): the frame thread's wall time outside every outermost scope it
        // opened - frame work no row names. A row is added where this grows, never a guess at its owner.
        {
            const double namedMs = prof.LastFrameNamedMs();
            const double cpuRest = std::max( 0.0, frameMs - namedMs );
            LOG_INFO( "[Profiler] Unaccounted CPU (frame thread) {:.3f} ms of a {:.3f} ms frame ({:.1f} %); "
                      "named outermost scopes hold {:.3f} ms",
                      cpuRest, frameMs, frameMs > 0.0001 ? cpuRest / frameMs * 100.0 : 0.0, namedMs );
        }

        LOG_INFO( "[Profiler] GPU self times sum to {:.3f} ms of a {:.3f} ms GPU frame ({:.1f} %) on the graphics "
                  "queue; the remainder is device work no pass is marked around. Async compute queue self "
                  "times (overlapping it, not in that sum): {:.3f} ms.",
                  gpuSumMs, gpuFrameMs, gpuFrameMs > 0.0001 ? gpuSumMs / gpuFrameMs * 100.0 : 0.0, gpuAsyncSumMs );

        // UNACCOUNTED, PER QUEUE (UE "stat gpu"): the GPU frame minus the union of the labelled graphics scopes,
        // and the async segments minus the union of their passes - the backend's numbers, not this sum (which
        // double-counts overlap). Then WHERE the graphics remainder sits: each stretch of at least
        // kGpuGapReportMs, named by its neighbours, largest first.
        LOG_INFO( "[Profiler] Unaccounted graphics queue {:.3f} ms of a {:.3f} ms GPU frame ({:.1f} %)",
                  unaccountedMs, gpuFrameMs, gpuFrameMs > 0.0001 ? unaccountedMs / gpuFrameMs * 100.0 : 0.0 );
        if ( asyncUnaccountedMs >= 0.0 )
        {
            LOG_INFO( "[Profiler] Unaccounted async compute queue {:.3f} ms (segment time outside its passes)",
                      asyncUnaccountedMs );
        }
        else
        {
            LOG_INFO( "[Profiler] Unaccounted async compute queue: no async segment timed" );
        }
        {
            std::vector<std::pair<double, std::string_view>> gaps;
            for ( const auto& s : prof.LastFrame() )
                if ( std::string_view( s.Name ).starts_with( gapPrefix ) )
                    gaps.emplace_back( s.GpuMs, std::string_view( s.Name ).substr( gapPrefix.size() ) );
            std::sort( gaps.begin(), gaps.end(),
                       []( const auto& a, const auto& b ) { return a.first > b.first; } );
            for ( const auto& [ms, where] : gaps )
                LOG_INFO( "[Profiler]   unaccounted {:>10.3f} ms  {}", ms, where );
        }

        // THE ASYNC QUEUE, NAMED. Every compute-queue segment of the frame is one row the backend publishes under
        // kGpuAsyncSegmentScopePrefix; its passes are the rows between its first and last pass. Their time
        // overlaps the graphics queue, which is why the sum above may pass 100 % once a segment runs async. No
        // segment row means every pass ran on the graphics queue (RenderPipeline.AsyncCompute off, or no
        // separate compute family).
        const std::string_view asyncPrefix = ::Common::Profiling::kGpuAsyncSegmentScopePrefix;
        std::string            asyncLine;
        double                 asyncMs  = 0.0;
        uint32_t               segments = 0;
        for ( const auto& s : prof.LastFrame() )
        {
            // A segment of a view carries the view's name first ("scene view #1 | RDG Async: .."): matched after
            // it (GpuAsyncSegmentLabelStart, the backend's rule), the view kept in front of the label.
            const std::string_view name  = s.Name;
            const size_t           label = ::Common::Profiling::GpuAsyncSegmentLabelStart( name );
            if ( label == std::string_view::npos )
                continue;
            asyncLine += std::format( " [{}{} {:.3f} ms]", name.substr( 0, label - asyncPrefix.size() ),
                                      name.substr( label ), s.GpuMs );
            asyncMs += s.GpuMs;
            ++segments;
        }
        if ( segments == 0 )
        {
            LOG_INFO( "[Profiler] async compute queue: no segment (every pass ran on the graphics queue)" );
        }
        else
        {
            LOG_INFO( "[Profiler] async compute queue: {} segment(s), {:.3f} ms overlapping the graphics queue:{}",
                      segments, asyncMs, asyncLine );
        }

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
        // GRASS-VIS-7: the grass culling read as numbers (Graphic::GrassDrawCounters): camera below full is the
        // shadow casters the camera does not draw, post the instances phase 2 brought back from last frame's HZB.
        // GPU-COST-21: and per cascade view (sun cascades 0..3, then the second shadowed light's) the instances
        // its own list drew - inside its caster box - against the full list each one drew before.
        const Graphic::GrassDrawCounters grass = Graphic::DrawCounter::LastFrameGrass();
        std::string                      cascades;
        for ( std::size_t v = 0; v < grass.Cascades.size(); ++v )
            cascades += std::format( "{}{}",
                                     v == 0                           ? ""
                                     : v == grass.Cascades.size() / 2 ? " | "
                                                                      : " ",
                                     grass.Cascades[v] );
        LOG_INFO(
             "[Profiler] Grass full {} camera {} post {} cascades {} (instances drawn, {} varieties read back "
             "frames in flight late) generated {} (varieties generated this frame)",
             grass.Full, grass.Camera, grass.Post, cascades, grass.Varieties, grass.Generated );
        // GPU-CULL: the binned meshes' two-phase occlusion read as numbers (SceneRenderer::GetDrawBinPhaseCounts,
        // frames in flight late), per culled view: drawn = phase 1 (in view, not held back by last frame's HZB) +
        // phase 2 (found visible in this frame's), against the frustum-only count (drawn + occluded).
        std::string binViews;
        if ( renderer != nullptr )
            for ( const Graphic::GpuDriven::CullPhaseViewCount& view : renderer->GetDrawBinPhaseCounts() )
                binViews += std::format( " {} {}+{}/{}", view.Name, view.Phase1, view.Phase2,
                                         view.Phase1 + view.Phase2 + view.Occluded );
        LOG_INFO( "[Profiler] DrawBins{} (instances drawn phase 1 + phase 2 / in view without occlusion)",
                  binViews.empty() ? " none" : binViews );
        // GPU-FARFIELD-4: what 'Clouds: March' did with its rays (System::CloudMarchCounters, frames in flight
        // late).
        const Graphic::System::CloudMarchCounters* const cloudMarch =
             renderer != nullptr ? renderer->GetCloudMarchCounters() : nullptr;
        if ( cloudMarch != nullptr && cloudMarch->Valid )
        {
            const auto&  words      = cloudMarch->Words;
            const double rays       = std::max( 1.0, static_cast<double>( words[0] ) );
            const double litSamples = std::max( 1.0, static_cast<double>( words[2] ) );
            LOG_INFO( "[Profiler] Clouds march: rays {} | primary steps {} ({:.1f} a ray) | lit {} ({:.1f} a ray) "
                      "| light samples {} ({:.1f} a lit sample, full ray {}) | early exit {} ({:.1f} %) | "
                      "column jumps {} | lanes at the ceiling {} (a frame; primary = view-ray field evaluations, "
                      "lit = samples with density and a shadow ray, early exit = transmittance under the stop)",
                      words[0], words[1], words[1] / rays, words[2], words[2] / rays, words[3],
                      words[3] / litSamples,
                      words[7], words[4], 100.0 * words[4] / rays, words[6], words[5] );
        }
#if DESERT_DEV_INSTRUMENTS
        // PROBE-TURN-1: why this view's screen probe rays took the cache-miss leg - the far field's radiance cache
        // reads by why they answered or not (GlobalIllumination::ScreenProbeMissReasons, frames in flight late).
        // Every read but an answered one is a ray "ScreenProbes: Trace Cache Miss" traces to the full distance.
        const Graphic::GlobalIllumination::ScreenProbeMissReasons* const missReasons =
             renderer != nullptr ? renderer->GetScreenProbeMissReasons() : nullptr;
        if ( missReasons == nullptr || !missReasons->Valid )
        {
            LOG_INFO( "[Profiler] ScreenProbes cache reads: none ({})",
                      missReasons == nullptr ? "the view has no screen probe gather" : "nothing read back yet" );
        }
        else
        {
            const auto& rays = missReasons->Rays;
            LOG_INFO(
                 "[Profiler] ScreenProbes cache reads: answered {} coarser {} | miss empty {} untraced {} "
                 "rejected {} nan {} (far-field rays a frame, frames in flight late; coarser = the coverage's "
                 "clipmap not traced yet, a coarser one answered; empty = no probe in the eight cells, the "
                 "atlas had no free slot; rejected / nan = the traced probes of the coverage, or of the coarser "
                 "level its untraced cells fell back to, saw the point behind something / weighed NaN)",
                 rays[0], rays[5], rays[1], rays[2], rays[3], rays[4] );
            // GPU-FARFIELD-1: what the far-field leg's own trace did with its rays (ScreenProbeMissReasons
            // [6..8]); GPU-FARFIELD-3: the global field's steps of its marches and of the hit-lighting leg
            // [9..11]; GPU-FARFIELD-5: that leg's card-held hits [12].
            const double stepsPerRay = rays[6] > 0u ? static_cast<double>( rays[9] ) / rays[6] : 0.0;
            const double stepsPerHit = rays[10] > 0u ? static_cast<double>( rays[11] ) / rays[10] : 0.0;
            LOG_INFO(
                 "[Profiler] ScreenProbes far field rays: listed {} global-field hit {} outside the cache {} | "
                 "steps {} ({:.1f} a ray) | hit-lighting leg {} (card-held {}, steps {}, {:.1f} a hit) (a frame, "
                 "frames in flight late; a hit is lit in the hit-lighting leg by the cards, else in place with "
                 "one global-field shadow march per directional light; outside = marched to the full trace "
                 "distance)",
                 rays[6], rays[7], rays[8], rays[9], stepsPerRay, rays[10], rays[12], rays[11], stepsPerHit );
        }
        // PROBE-TRACE-COST-3: where the radiance cache's probes went this frame - the entries the readers stamped and
        // how many hold a never-traced probe (per clipmap), what the budget traced, and the slots taken, refused (the
        // atlas full) and freed by why (GlobalIllumination::RadianceCacheUpdateCounts, frames in flight late).
        const Graphic::GlobalIllumination::RadianceCacheUpdateCounts* const cacheUpdate =
             renderer != nullptr ? renderer->GetRadianceCacheUpdateCounts() : nullptr;
        if ( cacheUpdate == nullptr || !cacheUpdate->Valid )
        {
            const char* const why =
                 cacheUpdate == nullptr ? "the view has no radiance cache" : "nothing read back yet";
            LOG_INFO( "[Profiler] RadianceCache update: none ({})", why );
        }
        else
        {
            const Graphic::GlobalIllumination::RadianceCacheUpdateCounts& c = *cacheUpdate;
            const auto&       never    = c.NeverTracedByClipmap;
            const auto&       fallback = c.FallbackByClipmap;
            const char* const pressed  = c.Pressed != 0u ? " (pressed: the last frame refused a slot)" : "";
            // The fallback's never-traced probes over every clipmap.
            uint32_t fallbackNeverTraced = 0u;
            for ( const uint32_t count : fallback )
                fallbackNeverTraced += count;
            LOG_INFO( "[Profiler] RadianceCache update: stamped {} never traced {} (clipmaps {} {} {} {} {} {}) "
                      "fallback never traced {} (clipmaps {} {} {} {} {} {}) traced {}/{} | slots live {}/{} "
                      "allocated {} failed {} | freed stale {} unused {} pressure {}{} (a frame, frames in flight "
                      "late; never traced = stamped by a coverage reader, ranked first; fallback = stamped only "
                      "as the level past a pixel's coarse end, traced from the rest of the budget)",
                      c.Stamped, c.NeverTraced, never[0], never[1], never[2], never[3], never[4], never[5],
                      fallbackNeverTraced, fallback[0], fallback[1], fallback[2], fallback[3], fallback[4],
                      fallback[5], c.Traced, c.Budget, c.Live, c.MaxProbes, c.Allocated, c.Failed, c.FreedStale,
                      c.FreedUnused, c.FreedPressure, pressed );
        }
#endif
        // GPU-CULL-2: per mesh pass how its draws were issued - the numbers of DesertCtl `state views.meshPasses`
        // (ControlService) from the same two sources, so a scenario reads them from the profile dump: bins = bin
        // records issued this frame (every view of the pass, a culled-empty bin included) / bins the draw command
        // cache holds; cpu = per-submesh draws the CPU lists issued in the pass; fallback = per BinFallback reason
        // the submeshes drawn in the pass that stay on the CPU path (once per primitive x submesh), the reason a
        // static mesh or ISM is not GPU-culled (no HZB occlusion on the CPU path).
        static_assert( Graphic::kCountedMeshPasses == Graphic::GpuDriven::kMeshPassCount );
        const Graphic::GpuDriven::DrawCommandCache* const cache =
             renderer != nullptr ? renderer->GetDrawCommandCache() : nullptr;
        std::string meshPasses;
        for ( uint32_t pass = 0; pass < Graphic::kCountedMeshPasses; ++pass )
        {
            const auto                          meshPass = static_cast<Graphic::GpuDriven::MeshPass>( pass );
            const Graphic::MeshPassDrawCounters counted  = Graphic::DrawCounter::LastFrameMeshPass( pass );
            uint32_t                            cached   = 0;
            std::string                         fallbacks;
            if ( cache != nullptr )
            {
                const Graphic::GpuDriven::PassDrawTable& table = cache->Pass( meshPass );
                cached = static_cast<uint32_t>( table.Bins.size() );
                for ( uint32_t reason = 0; reason < Graphic::GpuDriven::kBinFallbackCount; ++reason )
                    if ( table.Fallbacks[reason] > 0u )
                        fallbacks += std::format(
                             "{}{}={}", fallbacks.empty() ? "" : " ",
                             Graphic::GpuDriven::BinFallbackName( static_cast<Graphic::GpuDriven::BinFallback>( reason ) ),
                             table.Fallbacks[reason] );
            }
            meshPasses += std::format( " {} bins {}/{} cpu {} fallback({})",
                                       Graphic::GpuDriven::MeshPassStateName( meshPass ), counted.BinsIssued, cached,
                                       counted.CpuDraws, fallbacks );
        }
        LOG_INFO( "[Profiler] MeshPasses{} (per pass: bin records issued / bins cached, cpu draws, "
                  "fallback(reason=submeshes left on the CPU path){})",
                  meshPasses, cache != nullptr ? "" : "; no draw command cache" );

        // THE MESH DRAW SPLIT (Profiler::MeshDrawSplit; UE ProfileGPU with r.EmitMeshDrawEvents): per pass, its
        // most expensive draw groups, the rest folded into one row, and the groups' sum against the pass's own
        // gpu self - the pairs tile the pass, so the sum reads the split's coverage. Nothing while it is off.
        const std::vector<::Common::Profiling::DrawSplitResult>& split             = prof.LastDrawSplit();
        constexpr size_t                                         kDrawSplitTopRows = 20;
        for ( size_t first = 0; first < split.size(); )
        {
            const std::string& pass = split[first].Pass;
            size_t             end  = first;
            while ( end < split.size() && split[end].Pass == pass )
                ++end;

            double sumMs  = 0.0;
            double restMs = 0.0, restDraws = 0.0, restTriangles = 0.0;
            bool   restPerInstance = false;
            for ( size_t i = first; i < end; ++i )
            {
                const ::Common::Profiling::DrawSplitResult& group = split[i];
                sumMs += group.GpuMs;
                if ( i - first < kDrawSplitTopRows )
                {
                    LOG_INFO(
                         "[Profiler] draw split | {} | {} | {} | gpu {:.3f} ms | draws {:.0f} | tris {:.0f}{}",
                         pass, group.Mesh, group.Material, group.GpuMs, group.Draws, group.Triangles,
                         group.TrianglesPerInstance ? "/inst" : "" );
                    continue;
                }
                restMs += group.GpuMs;
                restDraws += group.Draws;
                restTriangles += group.Triangles;
                restPerInstance = restPerInstance || group.TrianglesPerInstance;
            }
            const size_t groups = end - first;
            if ( groups > kDrawSplitTopRows )
                LOG_INFO( "[Profiler] draw split | {} | (other {} groups) | | gpu {:.3f} ms | draws {:.0f} | tris "
                          "{:.0f}{}",
                          pass, groups - kDrawSplitTopRows, restMs, restDraws, restTriangles,
                          restPerInstance ? "/inst" : "" );

            double passSelfMs = -1.0;
            for ( const auto& s : prof.LastFrame() )
                if ( s.Name == pass && s.GpuCalls > 0 )
                    passSelfMs = s.GpuSelfMs;
            if ( passSelfMs > 0.0 ) {
                LOG_INFO(
                     "[Profiler] draw split | {} | total {} groups | sum gpu {:.3f} ms of pass gpu self {:.3f} "
                     "ms ({:.1f} %)",
                     pass, groups, sumMs, passSelfMs, 100.0 * sumMs / passSelfMs );
            } else {
                LOG_INFO( "[Profiler] draw split | {} | total {} groups | sum gpu {:.3f} ms; the pass has no gpu "
                          "row of that name",
                          pass, groups, sumMs );
            }
            first = end;
        }
        LOG_INFO( "[Profiler] ---- end ----" );
    }

    void ProfilerWindow::RefreshTable()
    {
        const auto&                                          prof = ::Common::Profiling::Profiler::Get();
        const std::vector<::Common::Profiling::ScopeResult>& rows = prof.LastFrame();
        if ( m_TablePublish == prof.PublishCount() && m_TableRows.size() == rows.size() )
            return;
        m_TablePublish = prof.PublishCount();

        const double frameMs = prof.LastFrameMs();
        // The whole-frame GPU bracket the backend records around the command buffer. It is the denominator
        // the per-pass GPU column is checked against: the passes should tile it, not exceed it.
        m_TableGpuFrameMs = 0.0;
        m_TableRows.resize( rows.size() );
        for ( size_t i = 0; i < rows.size(); ++i )
        {
            const ::Common::Profiling::ScopeResult& s     = rows[i];
            TableRow&                               cells = m_TableRows[i];
            if ( s.Name == ::Common::Profiling::kGpuFrameTotalScope )
                m_TableGpuFrameMs = s.GpuMs;

            const double pct = frameMs > 0.0001 ? ( s.TotalMs / frameMs ) * 100.0 : 0.0;
            cells.Waited     = s.WaitMs > 0.0005;
            cells.Timed      = s.GpuCalls > 0;
            cells.Hot        = pct > 25.0;
            std::snprintf( cells.Cpu.data(), cells.Cpu.size(), "%.3f", s.TotalMs );
            if ( cells.Waited )
                std::snprintf( cells.Wait.data(), cells.Wait.size(), "%.3f", s.WaitMs );
            else
                std::snprintf( cells.Wait.data(), cells.Wait.size(), "-" );
            if ( cells.Timed )
            {
                std::snprintf( cells.Gpu.data(), cells.Gpu.size(), "%.3f", s.GpuMs );
                std::snprintf( cells.GpuSelf.data(), cells.GpuSelf.size(), "%.3f", s.GpuSelfMs );
            }
            else
            {
                std::snprintf( cells.Gpu.data(), cells.Gpu.size(), "-" );
                std::snprintf( cells.GpuSelf.data(), cells.GpuSelf.size(), "-" );
            }
            std::snprintf( cells.Pct.data(), cells.Pct.size(), "%.1f", pct );
            std::snprintf( cells.Calls.data(), cells.Calls.size(), "%u", s.Calls );
        }
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

        RefreshTable();

        ImGui::Text( "Frame: %.3f ms  (%.0f FPS)   [avg]", frameMs, fps );
        if ( m_TableGpuFrameMs > 0.0 )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "GPU: %.3f ms", m_TableGpuFrameMs );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Dump to Log" ) )
            DumpProfilerToLog( m_Workspace.PrimaryRenderer() );
        if ( const Graphic::GrassDrawCounters grass = Graphic::DrawCounter::LastFrameGrass(); grass.Varieties > 0u )
            ImGui::Text( "Grass drawn: full %llu  camera %llu  post %llu",
                         static_cast<unsigned long long>( grass.Full ),
                         static_cast<unsigned long long>( grass.Camera ),
                         static_cast<unsigned long long>( grass.Post ) );

        ImGui::Separator();

        if ( ImGui::BeginTable( "##prof", 7,
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
            // The part of "cpu" blocked in a Wait scope (fence, acquire, present, pacer): cpu - wait is the work.
            ImGui::TableSetupColumn( "wait", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "gpu", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "self", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "%", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableSetupColumn( "x", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableHeadersRow();

            // One cell's pre-formatted text in @p color (TextColored / TextDisabled without the printf).
            const auto cell = []( const std::array<char, 16>& text, const ImVec4& color )
            {
                ImGui::PushStyleColor( ImGuiCol_Text, color );
                ImGui::TextUnformatted( text.data() );
                ImGui::PopStyleColor();
            };
            const ImVec4 disabled = ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled );

            // VIRTUALISED (UE SListView): only the rows inside the window's visible rect are submitted. The
            // table holds the whole published window - hundreds of rows - and the docked panel shows a few
            // dozen; every row is one line, so the clipper's step over the rest is exact.
            const std::vector<::Common::Profiling::ScopeResult>& rows = prof.LastFrame();
            ImGuiListClipper                                     clipper;
            clipper.Begin( static_cast<int>( rows.size() ) );
            while ( clipper.Step() )
                for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
                {
                    const ::Common::Profiling::ScopeResult& s     = rows[static_cast<size_t>( i )];
                    const TableRow&                         cells = m_TableRows[static_cast<size_t>( i )];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( s.Name.c_str() );
                    // Docked at its usual width the name column clips, and "Clouds: Sha" / "Clouds: Exe" are
                    // two different passes. The full name on hover costs nothing and settles it.
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "%s", s.Name.c_str() );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( cells.Cpu.data() );
                    ImGui::TableNextColumn();
                    cell( cells.Wait, cells.Waited ? ImVec4( 1.0f, 0.62f, 0.40f, 1.0f ) : disabled );
                    ImGui::TableNextColumn();
                    // A dash, not 0.000: a scope that records no GPU work and a scope the GPU timer could not
                    // reach are different states, and printing zero for both invents a measurement.
                    cell( cells.Gpu, cells.Timed ? ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ) : disabled );
                    ImGui::TableNextColumn();
                    // Nested passes subtracted — the column that can be added up.
                    cell( cells.GpuSelf, cells.Timed ? ImVec4( 0.45f, 0.70f, 0.95f, 1.0f ) : disabled );
                    ImGui::TableNextColumn();
                    // Tint hot scopes (>25% of the frame) red.
                    if ( cells.Hot )
                        cell( cells.Pct, ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ) );
                    else
                        ImGui::TextUnformatted( cells.Pct.data() );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( cells.Calls.data() );
                }
            clipper.End();
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
