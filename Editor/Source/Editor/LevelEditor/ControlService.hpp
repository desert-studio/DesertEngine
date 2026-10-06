#pragma once

// THE CONTROL CHANNEL (UE: Remote Control / Automation Driver — a service beside the editor, not the editor).
//
// The socket a client (DesertCtl, the MCP server) drives the editor through: accept, read one request, run
// it against the live editor, and release its reply only after a frame that already reflects it
// (Editor/Core/Control/ControlPipeline.hpp has the argument). A member of EditorLayer BY VALUE, declared after
// every module it reads; it asks them through their own verbs (HasPendingRequests, HasPendingLoad...) and never
// through their fields, and it knows nothing of EditorLayer — what only the layer knows (is the start-up still
// loading, close the application) crosses as an argument or a return value.
//
// The order around one frame is:
//   OnUpdate      ServiceChannel()            read a request, run it, arm the gate
//   OnUpdate      ...deferred queues drain, the scene renders...
//   OnUpdate      SampleFrameQuiescence()     what was still outstanding while this frame was made
//   OnUIRender    RecordWindowCaptureIfDue()  ...the interface is recorded into the swapchain...
//   present
//   OnFramePresented                          judge the frame; take the shot; release the reply

#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/Control/ControlPipeline.hpp"
#include "Editor/Core/Control/ControlProtocol.hpp"
#include "Editor/Core/Control/ControlSocket.hpp"
#include "Editor/Core/Control/ControlState.hpp"
#include "Editor/Core/Selection/SelectionTransformProperties.hpp"

#include <Engine/Desert.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    class CommandRegistry;
    class DocumentHost;
    class PanelRegistry;
    class PlaySession;
    class SceneFiles;
    class SceneWorkspace;
    class ViewportCapture;

    class ControlService
    {
    public:
        ControlService( SceneWorkspace& workspace, SceneFiles& sceneFiles, PlaySession& play,
                        DocumentHost& documents, ViewportCapture& capture, PanelRegistry& panels,
                        const CommandRegistry& commands )
             : m_Workspace( workspace ), m_SceneFiles( sceneFiles ), m_Play( play ), m_Documents( documents ),
               m_Capture( capture ), m_Panels( panels ), m_Commands( commands )
        {
        }

        // Present only when `--control-socket` named one; silent otherwise. See
        // Editor/Core/Control/ControlChannelOptions.hpp for why an editor does not listen by default.
        [[nodiscard]] Common::BoolResultStr Listen( const std::string& socketPath )
        {
            return m_ControlSocket.Listen( socketPath );
        }

        // Drained at the TOP of OnUpdate: accept, read one request, execute it. Everything it can run is
        // a palette entry.
        void ServiceChannel();
        // Sampled after the deferred queues have drained and BEFORE the scene is rendered — "was anything
        // outstanding while this frame was being made". Judged later, by the gate, at OnFramePresented.
        // Also called once at the end of OnAttach: an unsampled census must not read as a settled editor.
        // @p startupLoading is the layer's start-up staging or content settle, still running.
        void SampleFrameQuiescence( bool startupLoading );
        // CAPTURING THE COMPOSITED FRAME, in two halves, because a swapchain image may only be touched
        // between its acquire and its present.
        //
        // Recorded at the end of OnUIRender, while the frame is still being built and the image is
        // legitimately ours; collected in OnFramePresented, once the present that carried the copy has
        // gone out. Doing it all after the present produced a correct picture and a Vulkan spec violation
        // that only the validation layer mentioned — see the definition.
        void RecordWindowCaptureIfDue();
        // The frame is out: judge it, take a `shot.*`, release the reply. Returns the exit status of a `quit`
        // whose reply has gone out — the layer closes the application with it.
        [[nodiscard]] std::optional<int32_t> OnFramePresented();

        // OnDetach: abandon a request still in flight (logged, never answered) and remove the socket file.
        void Close();

    private:
        // Executes one request and decides whether its reply leaves now or waits for the frame that proves
        // it. THE ONLY place a control request is run: there are two ways to arrive at one — read off the
        // socket, or released by the readiness gate several frames later — and one way to run it.
        void RunControlRequest( const Control::Request& request );
        // Drop an in-flight request whose CONNECTION has gone, rather than answering its successor. True
        // when it did. See the definition: a reply of 311 commands was measured reaching the wrong client.
        [[nodiscard]] bool AbandonControlRequestIfItsAskerIsGone();
        // Runs one request against the live editor. Never throws, always answers.
        [[nodiscard]] Control::Response ExecuteControlRequest( const Control::Request& request );
        // The palette's dictionary, built from the CommandRegistry the palette reads (one list, two readers).
        [[nodiscard]] std::vector<PaletteCommand> BuildDictionary() const;
        // The `set` for the channel's second subject — the editor's own view. See
        // Editor/Core/ViewportCameraProperties.hpp for why a camera pose is a property write and not a
        // palette command.
        [[nodiscard]] Control::Response SetViewportCameraProperty( const Control::Request& request );
        // The `selection` subject's entity and its transform: exactly one selected entity that has a
        // TransformComponent, or a refusal saying what is selected instead.
        [[nodiscard]] Common::ResultStr<std::pair<Common::UUID, Core::SelectionTransform>>
        SelectedTransform() const;
        // The refusal that goes with a null SceneWorkspace::ActiveEditorCamera().
        [[nodiscard]] std::string NoEditorCameraReason() const;
        // Everything ControlState needs, read off the editor's modules in one pass.
        [[nodiscard]] Control::EditorSnapshot TakeEditorSnapshot() const;

        SceneWorkspace&        m_Workspace;
        SceneFiles&            m_SceneFiles;
        PlaySession&           m_Play;
        DocumentHost&          m_Documents;
        ViewportCapture&       m_Capture;
        PanelRegistry&         m_Panels;
        const CommandRegistry& m_Commands;

        Control::ControlSocket m_ControlSocket;

        // The gate that makes "command -> frame -> snapshot" a property rather than a coincidence. Armed
        // when a request executes; discharged by the first PRESENTED frame that was rendered with nothing
        // outstanding. Editor/Core/Control/ControlPipeline.hpp has the argument.
        Control::FrameGate m_ControlGate;

        // The request whose reply the gate is holding, and the reply itself. Held together because they
        // are one thing: a reply parked without its request could not say what it was answering, and a
        // request parked without its reply would have to be re-run to produce one.
        std::optional<Control::Request>  m_ControlInFlight;
        std::optional<Control::Response> m_ControlPendingReply;

        // WHICH CONNECTION asked for it. Not "was somebody connected": the editor notices a disconnect and
        // accepts the next client in the SAME service call, so a request parked across that gap would have
        // its reply written to a stranger. Measured — a 311-command answer delivered to the wrong client,
        // with an id that matched because both had sent 1.
        uint64_t m_ControlInFlightClient = 0;

        // The outstanding work sampled while THIS frame was being built. Not read at the moment the gate
        // judges it: by then the answer has moved on, and the question is about the picture.
        Control::EditorQuiescence m_FrameQuiescence;

        // Frames presented since the layer attached. The gate's clock — deliberately the channel's own count
        // and not the renderer's frame-in-flight index, which wraps at three and could not order anything.
        uint64_t m_FrameIndex = 0;

        // A `quit` the channel asked for. Honoured after its reply has actually gone out, so the last
        // answer is not lost to the exit — a client that never hears "ok" cannot tell a clean shutdown
        // from a crash.
        std::optional<int32_t> m_ControlQuitCode;
    };
} // namespace Desert::Editor
