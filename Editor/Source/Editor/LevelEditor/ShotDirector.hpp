#pragma once

// HEADLESS CAPTURE (UE: the automation screenshot director — `--shot`, `--play`, `--camera`/`--look`, a
// `--shot-sequence`). What a capture run does to the editor frame by frame: queue the scene it names, start
// the world under `--play`, place the camera before the render, count the rendered frames and write the PNGs
// after it. The command line itself is Editor/Core/ShotOptions.hpp; this is the one thing that acts on it.
//
// A member of EditorLayer BY VALUE, declared after the modules it drives. It knows nothing of EditorLayer:
// whether the start-up is still loading crosses as an argument, and ending the process is the layer's act on
// the status Finish() returns.

#include <Engine/Desert.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Core
{
    class EditorCamera;
}

namespace Desert::Editor
{
    class PlaySession;
    class SceneFiles;
    class SceneWorkspace;
    class ViewportCapture;

    // THE ONE PLACEMENT. Both the `--camera`/`--look` capture path and the control channel's
    // `set Camera.Position` land here, through the editor's own view-axis-gizmo and F-focus gestures.
    // Two copies would drift.
    void PlaceEditorCamera( Desert::Core::EditorCamera& camera, const glm::vec3& position,
                            const glm::vec3& forward );

    class ShotDirector
    {
    public:
        ShotDirector( SceneWorkspace& workspace, SceneFiles& sceneFiles, PlaySession& play,
                      ViewportCapture& capture )
             : m_Workspace( workspace ), m_SceneFiles( sceneFiles ), m_Play( play ), m_Capture( capture )
        {
        }

        // Does the command line name a scene to capture? Then it, and not the project's default, is opened.
        [[nodiscard]] static bool NamesScene();
        // Queues the named scene for the deferred load. A scene that is not there ends a capture: the exit
        // status to close with is returned, and nothing is queued.
        [[nodiscard]] std::optional<int32_t> QueueScene();

        // `--play`: start the world (pinned to the capture's camera) before the first counted frame.
        void BeginPlayIfDue( bool startupLoading );
        // FIRST HALF: place the camera for the frame that is about to be rendered.
        void PlaceCamera( bool startupLoading );
        // SECOND HALF: count the frame just rendered and write what is due. True on the capture's LAST frame,
        // after its PNG — the layer then finishes its own records and closes with Finish().
        // @p contentLoading is the start-up staging or content settle, still running.
        [[nodiscard]] bool CountRenderedFrame( bool contentLoading );
        // A record the layer keeps for the capture (the --flight CSV) failed: the capture fails with it.
        void MarkFailed()
        {
            m_ShotFailed = true;
        }
        // Logs what the capture cost on the device and returns the process exit status.
        [[nodiscard]] int32_t Finish() const;

        // Rendered frames counted so far (the --flight row's clock).
        [[nodiscard]] int Frame() const
        {
            return m_ShotFrame;
        }

    private:
        SceneWorkspace&  m_Workspace;
        SceneFiles&      m_SceneFiles;
        PlaySession&     m_Play;
        ViewportCapture& m_Capture;

        int  m_ShotFrame        = 0;
        bool m_ShotCameraPlaced = false;
        // Set when any PNG of this capture could not be written; becomes the process exit status.
        bool m_ShotFailed = false;
    };
} // namespace Desert::Editor
