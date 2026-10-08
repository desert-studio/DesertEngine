#include "Editor/LevelEditor/ShotDirector.hpp"

#include "Editor/Core/CommandLine.hpp"
#include "Editor/Core/FlightRules.hpp"
#include "Editor/Core/ShotOptions.hpp"
#include "Editor/Core/ViewportCameraProperties.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneMeshBounds.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/ViewportCapture.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace Desert::Editor
{
    // THE ONE PLACEMENT. Both the `--camera`/`--look` capture path and the control channel's
    // `set Camera.Position` land here, which is the rule the protocol states for a property write: the
    // value goes into the same setter the widget calls, so there is one route into the camera and two ways
    // to reach it. Two copies of this would drift the day one of them learned about roll.
    //
    // SnapToDirection + Focus are the EDITOR'S OWN gestures — the clickable view-axis gizmo and F-focus —
    // and that is what makes this the same path a person's hands take rather than a private back door.
    // Focus backs the camera off along the current view direction by the framing distance, so aiming one
    // framing distance ahead is what lands it exactly on the position asked for; the two uses of that
    // distance are one named constant for that reason.
    void PlaceEditorCamera( Desert::Core::EditorCamera& camera, const glm::vec3& position,
                            const glm::vec3& forward )
    {
        camera.SnapToDirection( glm::normalize( forward ) );
        camera.Focus( ViewportCameraFocalPoint( position, forward ), kViewportCameraFramingDistance );
    }

    bool ShotDirector::NamesScene()
    {
        return !ShotOptions::Get().Scene.empty();
    }

    std::optional<int32_t> ShotDirector::QueueScene()
    {
        const auto& shot = ShotOptions::Get();
        // In CAPTURE mode a `--scene` that is not there is fatal, not something to carry on past.
        // The scene loader already logs and leaves the current scene standing, which is right for an
        // editor and wrong for a capture: the run would go on to write PNGs named after the scene
        // that was asked for, holding the picture of a different one. That is worse than no evidence,
        // because it looks exactly like evidence. Interactive `--scene` keeps the old behaviour.
        //
        // The RULE itself lives in Editor/Core/CommandLine.hpp as a pure function taking the existence
        // as a parameter, so it is asserted by a test rather than only observable by launching the
        // editor at a path that is not there. This call site supplies the filesystem it cannot.
        //
        // A relative `--scene` is PROJECT content, so it is read off the project's directory
        // (FPaths::ProjectDir), never off the working directory — the editor may be started from any
        // folder. An absolute path is taken as given (operator/ keeps an absolute right-hand side).
        const std::filesystem::path scenePath = Common::Constants::Path::ProjectDir() / shot.Scene;
        const auto verdict = ValidateSceneForCapture( shot, std::filesystem::exists( scenePath ) );
        if ( !verdict.IsSuccess() )
        {
            LOG_ERROR( "[Shot] {} (looked for '{}')", verdict.GetError(), scenePath.string() );
            // NOT std::exit(). The job system's workers are already running by the time this line is
            // reached, and exit() runs static destructors under them: nine threads threw
            // "recursive_mutex lock failed: Invalid argument" and the process aborted with 134. A
            // status of 134 says "the engine crashed", not "the scene you asked for is missing" — the
            // caller reading it learns the wrong thing. Ask for an ordered close with the real status;
            // Run() then draws no frames and teardown happens exactly as on a normal quit.
            return 2;
        }
        m_SceneFiles.RequestLoad( Common::Filepath( scenePath ) );
        return std::nullopt;
    }

    bool ShotDirector::AdmitFrame( const ShotFrameConditions& frame )
    {
        if ( !ShotOptions::Get().Active() )
            return false;
        const bool wasRecording = m_Gate.Recording();
        const bool recorded     = m_Gate.Admit( frame );
        if ( recorded && !wasRecording )
        {
            // What every view integrated over the start-up frames - frame indices, reprojected histories,
            // exposure, particles - is cut before the first recorded frame renders, so frame 1 of the sequence
            // follows the same history on every run however many frames the start-up took.
            size_t views = 0;
            if ( const auto& scene = m_Workspace.ActiveScene() )
            {
                views = scene->GetViewCount();
                for ( size_t view = 0; view < views; ++view )
                {
                    if ( auto* renderer = scene->GetViewRenderer( view ) )
                    {
                        renderer->ResetTemporalHistory();
                    }
                }
            }
            LOG_INFO( "[Shot] recording from this frame on at {}x{}: the splash is gone, the content has settled "
                      "(no texture read, cook or upload in flight) and the viewport held its size {} frame(s); "
                      "temporal history reset on {} view(s)",
                      m_Gate.RecordWidth(), m_Gate.RecordHeight(), ShotRecordGate::kStableFrames, views );
        }
        return recorded;
    }

    void ShotDirector::BeginPlayIfDue( const bool recordedFrame )
    {
        // Screenshot mode, `--play`: start the world before the first frame that will be counted.
        //
        // Through m_Play.Play(), the same entry the toolbar's Play button uses, so a headless run is a Play
        // session and not a second definition of one — the snapshot it takes is what would let a Stop
        // restore the authored scene, and a capture that entered Play by some private shortcut would drift
        // from the editor the day either changed.
        //
        // The camera is PINNED first, and that is the whole reason this block is not one line. Play hands
        // the view to the scene's own CameraComponent (Scene::UpdateActiveCameraSource), which would take
        // the shot away from `--camera`/`--look` in any scene that has a camera entity — and take it
        // SILENTLY, because the placement below asks for an EditorCamera and would simply not find one.
        // Pinning is the engine's existing "this view is driven from outside" mechanism and headless
        // capture is exactly that case, so `--play` changes what MOVES in the frame and nothing about
        // where the frame is taken from.
        if ( auto& shot = ShotOptions::Get();
             shot.PlayActive() && recordedFrame && m_Workspace.ActiveScene() &&
             m_Workspace.ActiveScene()->GetState() == ::Desert::Core::Scene::SceneState::Edit )
        {
            if ( m_Workspace.ActiveScene()->GetActiveCamera() )
            {
                m_Workspace.ActiveScene()->PinActiveCamera( m_Workspace.ActiveScene()->GetActiveCamera() );
                if ( shot.FlightRoute.has_value() )
                {
                    // A ZERO AVERAGING WINDOW makes the profiler publish every frame, so the numbers read
                    // on frame k are exactly frame k-1's (FlightLog). The window is a display setting; a
                    // headless flight has no panel to smooth for, and one source of timing serves both.
                    Common::Profiling::Profiler::Get().AvgWindowSeconds() = 0.0f;
                    LOG_INFO( "[Flight] '{}': {:.0f} m at {:.0f} cm/s, {} warm-up frame(s) then {} frame(s); "
                              "CSV to '{}'",
                              shot.FlightRoute->Spec, Flight::RouteLength( *shot.FlightRoute ) / 100.0,
                              shot.FlightSpeed, Flight::kWarmupFrames, shot.Frames - Flight::kWarmupFrames,
                              shot.FlightCsv );
                }
                m_Play.Play();
                LOG_INFO( "[Shot] --play: gameplay running at a fixed {} s step; the {} captured frames are "
                          "{} s of simulated time",
                          ShotOptions::PlayStepSeconds, shot.Frames, shot.SimulatedSeconds( shot.Frames ) );
            }
            else
            {
                // Refused rather than played anyway: with nothing to pin, Play would pick a view of its own
                // and the capture would answer a question about a pose nobody asked for — while looking
                // exactly like a legitimate result.
                LOG_ERROR( "[Shot] --play refused: scene '{}' has no active camera to pin, and Play would "
                           "choose the view itself. No gameplay time advanced; this capture is a frozen "
                           "world.",
                           m_Workspace.ActiveScene()->GetSceneName() );
                shot.Play = false;
            }
        }
    }

    void ShotDirector::PlaceCamera( const bool startupLoading )
    {
        // Screenshot mode, FIRST HALF: place the camera for the frame that is about to be rendered.
        //
        // Before the render and not after it, because the capture below reads back whatever the render
        // produced: with the placement after it, the image written as frame N was rendered from the pose
        // of frame N-2, and on a MOVING path that is not a bookkeeping detail — a 120-degree pan over 90
        // frames puts the last captured frame 1.35 degrees, about 28 pixels, short of the endpoint the
        // command line named. Frame N is rendered from pose N, and the final frame lands exactly on
        // `--camera-to` / `--look-to`.
        //
        // With `--camera-to` / `--look-to` the pose is re-placed EVERY frame, walking the path across
        // exactly the warm-up frames. Without them `HasMotion()` is false, the placement happens once at
        // parameter 0, and the pose it computes is (Position, Forward) to the bit.
        if ( auto& shot = ShotOptions::Get(); shot.Active() && shot.HasCamera && !m_SceneFiles.HasPendingLoad() &&
                                              !startupLoading &&
                                              ( !m_ShotCameraPlaced || shot.HasMotion() || shot.FlightRoute ) )
        {
            if ( ::Desert::Core::EditorCamera* cam = m_Workspace.ActiveEditorCamera();
                 ( cam != nullptr ) && shot.FlightRoute )
            {
                const Flight::Pose pose =
                     Flight::PoseAt( *shot.FlightRoute, Flight::DistanceAt( m_ShotFrame, shot.FlightSpeed,
                                                                            ShotOptions::PlayStepSeconds ) );
                PlaceEditorCamera( *cam, pose.Position, pose.Forward );
                cam->SetInputEnabled( false );
            }
            else if ( cam != nullptr )
            {
                // THE SAME PLACEMENT THE CONTROL CHANNEL USES. It used to be spelled out here, with the
                // framing distance written twice on one line as a bare 500.0f — and it was the ONLY way to
                // place the camera at all, so a developer who wanted a viewpoint and no capture had to
                // launch with `--shot --shot-frames 1000000` to reach it. See ViewportCameraProperties.hpp.
                const ShotCamera view = shot.CameraAt( shot.Parameter( m_ShotFrame ) );
                PlaceEditorCamera( *cam, view.Position, view.Forward );
                cam->SetInputEnabled( false ); // nothing may nudge it between here and the capture
            }
            m_ShotCameraPlaced = true;
        }
        else if ( shot.Active() && !shot.HasCamera && !shot.FlightRoute && !m_ShotCameraPlaced &&
                  m_Gate.Recording() )
        {
            FrameScene();
            m_ShotCameraPlaced = true;
        }
    }

    void ShotDirector::FrameScene()
    {
        // NO --camera / --look: THE SHOT FRAMES THE SCENE, as F does on a selection (UE FocusViewportOnBox).
        // Taken on the FIRST RECORDED frame and not earlier, because the gate admits that frame only once
        // the scene load, the background cook and the content stream have settled — so every mesh the
        // picture will show is parsed and measured, and the record size (the aspect) is final. The view
        // direction is FIXED, not the camera's own: --look when given, else UE's default perspective view
        // (DefaultPerspectiveViewForward) — the camera's own pointed up and framed from under the ground.
        const auto&                   scene = m_Workspace.ActiveScene();
        ::Desert::Core::EditorCamera* cam   = m_Workspace.ActiveEditorCamera();
        if ( !scene || cam == nullptr )
        {
            LOG_ERROR(
                 "[Shot] no --camera/--look and no active scene view to frame: the camera stays where it is" );
            return;
        }
        if ( cam->GetProjectionType() != ::Desert::Core::ProjectionType::Perspective )
        {
            LOG_ERROR( "[Shot] no --camera/--look: framing the scene needs a perspective viewport, this one is "
                       "orthographic; the camera stays where it is" );
            return;
        }
        const SceneMeshBounds bounds = MeasureSceneMeshes( scene->GetRegistry(), &SharedPrimitiveSubmeshes );
        if ( bounds.Missing > 0 )
            LOG_WARN( "[Shot] framing: {} mesh(es) not parsed yet are outside the measured bounds",
                      bounds.Missing );
        if ( Geometry::IsEmpty( bounds.Box ) )
        {
            LOG_ERROR( "[Shot] no --camera/--look and the scene has no measurable mesh to frame: the camera "
                       "stays where it is" );
            return;
        }
        const float aspect =
             static_cast<float>( m_Gate.RecordWidth() ) / static_cast<float>( m_Gate.RecordHeight() );
        const auto&      shot    = ShotOptions::Get();
        const glm::vec3  forward = shot.HasLook ? shot.Forward : DefaultPerspectiveViewForward();
        const FramedView view    = FrameBox( bounds.Box, forward, cam->GetFOV(), aspect );
        cam->SetNear( view.Near );
        cam->SetFar( view.Far );
        PlaceEditorCamera( *cam, view.Position, view.Forward );
        cam->SetInputEnabled( false );
        LOG_INFO( "[Shot] framed {} mesh(es): box ({}, {}, {})..({}, {}, {}), camera ({}, {}, {}), near {} far {}",
                  bounds.Meshes, bounds.Box.Min.x, bounds.Box.Min.y, bounds.Box.Min.z, bounds.Box.Max.x,
                  bounds.Box.Max.y, bounds.Box.Max.z, view.Position.x, view.Position.y, view.Position.z, view.Near,
                  view.Far );
    }

    bool ShotDirector::CountRenderedFrame( const bool recordedFrame )
    {
        // Screenshot mode, SECOND HALF: the frame just rendered is the frame that gets written. The frame
        // count is not decoration — a temporally accumulating pass needs several frames to converge, so an
        // early shot is a picture of the dither rather than of the scene.
        // `recordedFrame` CARRIES `!ContentSettling()`, AND THAT IS THE CORRECTNESS OF EVERY CAPTURE THIS
        // REPOSITORY TAKES. `--shot-frames N` counts rendered frames, and before the cloud kinds became
        // demand-driven every one of them was a frame whose content was already resident. Counting from
        // the first frame after a scene load would now start the count while a worker is still reading
        // the sky, so a low-frame capture would photograph a scene with no clouds in it and file it as
        // the picture of the scene -- the same shape as the blank-PNG trap the verification skill warns
        // about, and just as invisible in a diff of two such frames.
        if ( auto& shot = ShotOptions::Get(); shot.Active() && recordedFrame )
        {
            ++m_ShotFrame;

            // The sequence counts RENDERED frames, so `frame_00001` is the first frame rendered, from path
            // parameter 0, and `frame_000NN` at --shot-frames NN is the last, from parameter 1. When
            // `--shot-every` divides `--shot-frames` the last file of the sequence and the `--shot` PNG are
            // the same image — a cheap invariant to check a capture against.
            if ( !shot.Sequence.empty() && ( m_ShotFrame % shot.SequenceEvery ) == 0 )
            {
                char name[64];
                std::snprintf( name, sizeof( name ), "/frame_%05d.png", m_ShotFrame );
                const std::string path = shot.Sequence + name;
                if ( !m_Capture.WriteViewportPng( path ) )
                {
                    LOG_ERROR( "[Shot] sequence frame {} not written to '{}'", m_ShotFrame, path );
                    m_ShotFailed = true;
                }
            }

            if ( m_ShotFrame >= shot.Frames )
            {
                if ( !shot.Output.empty() && !m_Capture.WriteViewportPng( shot.Output ) )
                {
                    LOG_ERROR( "[Shot] the final frame was not captured to '{}'", shot.Output );
                    m_ShotFailed = true;
                }
                return true;
            }
        }
        return false;
    }

    int32_t ShotDirector::Finish() const
    {
        // WHAT THE CAPTURE COST ON THE DEVICE, AT THE ONE INSTANT THE PICTURE DESCRIBES.
        //
        // Until this line the only memory readings a headless run produced came from BOOT and
        // from the moment a renderer slot was built — both of them BEFORE any texture the scene
        // needs has been uploaded, because `TextureService::Get` builds the GPU texture on first
        // use and first use is a frame. Measured on the world scene: at "Renderer slot 0 built"
        // the ledger reports AssetService holding 76 shaders and ZERO Image2D, and the scene's
        // one texture only appears a hundred frames later. So every figure anybody had for
        // "texture memory on this scene" was taken before the textures existed.
        //
        // Unconditional, and not behind `--gpu-profile`: a reading nobody remembers to ask for
        // is a reading nobody has. It is three queries and one locked walk, once, on the frame
        // that ends the process.
        LOG_INFO( "[Memory] shot taken — {}", Graphic::MemoryReadout::Take().Report() );
        LOG_INFO( "[Resources] {}", Graphic::ResourceLedger::Report() );
        LOG_INFO( "[Memory] {}", Graphic::MemoryWatch::Report() );
        // A capture that wrote no PNG must not leave a zero exit status behind: the whole value of
        // an exit code is that a script can trust it, and this one used to say "fine" either way.
        return m_ShotFailed ? 1 : 0;
    }
} // namespace Desert::Editor
