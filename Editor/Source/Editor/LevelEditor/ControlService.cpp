#include "Editor/LevelEditor/ControlService.hpp"

#include "Editor/Core/AssetOpen.hpp"
#include "Editor/Core/Commands/CommandRegistry.hpp"
#include "Editor/Core/Commands/LandscapeLayerCommands.hpp"
#include "Editor/Core/Commands/SceneCommands.hpp"
#include "Editor/Core/Control/ControlDispatch.hpp"
#include "Editor/Core/Control/InputInjection.hpp"
#include "Editor/Core/Control/PointerDrag.hpp"
#include "Editor/Core/ControlNudgeRequest.hpp"
#include "Editor/Core/DocumentWell.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/Selection/ModelingState.hpp"
#include "Editor/Core/Selection/ModelingStateProperties.hpp"
#include "Editor/Core/Selection/SelectionManager.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/SubjectOpenRequest.hpp"
#include "Editor/Core/ViewportCameraProperties.hpp"
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/ShotDirector.hpp"
#include "Editor/LevelEditor/ViewportCapture.hpp"
#include "Editor/LevelEditor/WindowTitles.hpp"
#include "Editor/Panels/Logs/LogsPanel.hpp"

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>

#include <format>

namespace Desert::Editor
{
    // =============================================================================================
    // THE CONTROL CHANNEL
    //
    // Four functions and one promise. The promise is that a reply leaves only after a frame that already
    // reflects the command it answers — see Editor/Core/Control/ControlPipeline.hpp for why that is not
    // the same as "the next frame", and what it took to make it true rather than usually true.
    //
    // The order around one frame is:
    //   OnUpdate      ServiceControlChannel()   read a request, run it, arm the gate
    //   OnUpdate      ...deferred queues drain, the scene renders...
    //   OnUpdate      SampleFrameQuiescence()   what was still outstanding while this frame was made
    //   OnUIRender ...the interface is recorded into the swapchain...
    //   present
    //   OnFramePresented                        judge the frame; take the shot; release the reply
    // =============================================================================================

    void ControlService::ServiceChannel()
    {
        if ( !m_ControlSocket.IsListening() )
            return;

        // A request whose reply has not gone out yet holds the channel. Reading a second one here would
        // hand the ordering guarantee to whoever wrote the client: two commands in flight cannot both be
        // "the command the next settled frame proves".
        if ( m_ControlInFlight )
        {
            // THE CONNECTION IS STILL SERVICED, THE SOCKET IS JUST NOT READ FROM. A second request must
            // not be taken while one is in flight — that is the whole of the ordering guarantee — but a
            // readiness wait can last a whole boot, and a peer that has GONE is only ever detected by
            // reading from it. Without this the editor would hold the channel open for a client that is no
            // longer there and refuse every new one until the wait ended by itself.
            m_ControlSocket.ServiceConnection();
            if ( AbandonControlRequestIfItsAskerIsGone() )
                return;

            // ONE STATE MEANS ONE THING: a request in flight with an IDLE gate is a request the readiness
            // wait has just released and that has not run yet. Every other combination clears itself in
            // OnFramePresented, so this is the only way to be here. Running it at the top of OnUpdate and
            // not at the point of release keeps ONE execution site for control commands — the same point
            // in the frame a request that never had to wait is run at.
            if ( m_ControlGate.IsArmed() || m_ControlPendingReply )
                return;

            const Control::Request held = *m_ControlInFlight;
            m_ControlInFlight.reset();
            RunControlRequest( held );
            return;
        }

        const std::optional<std::string> line = m_ControlSocket.PollRequestLine();
        if ( !line )
            return;

        const auto parsed = Control::ParseRequest( *line );
        if ( !parsed )
        {
            // Answered immediately: a request that did not parse has no id to echo and nothing to wait
            // for. Silence here would be indistinguishable from an editor that had stopped reading.
            m_ControlSocket.SendResponseLine(
                 Control::FormatResponse( Control::Response::Failure( 0, parsed.GetError() ) ) );
            return;
        }

        const Control::Request request = parsed.GetValue();

        // AN EDITOR THAT HAS NOT READ THE PROJECT DOES NOT ANSWER ABOUT IT.
        //
        // This is the whole of A6-1's second half, and it is one branch because the mechanism it needs
        // already existed: PendingWork::StartupLoading has been in the quiescence census since the channel
        // landed, and the gate has always been able to hold something until a presented frame proves the
        // census empty. What was missing is that the READS never asked. `commands` was answered from the
        // asset cache the moment it arrived, and the cache is filled by five separate startup stages — so
        // for 3.3 s of every boot the palette successfully offers 106 of this project's 130 openable
        // assets, and for the seconds before that, none of them. Neither answer says which it is.
        //
        // Held, not refused, because a refusal only moves the problem: the client would have to guess how
        // long to wait and ask again, which is the polling loop this channel exists to delete. The refusal
        // still exists — it is what a gate timeout produces, and it names what never finished.
        //
        // WHICH operations need this is the protocol's decision, not this file's: `state` and `quit` are
        // exempt, for reasons written where the table is (Control/ControlProtocol.hpp). ControlService.cpp is
        // compiled by no test suite, so a rule stated here is a rule nothing can show going red.
        if ( Control::NeedsReadyEditor( request.Operation ) && !m_FrameQuiescence.Settled() )
        {
            LOG_INFO( "[Control] request {} is waiting for the editor to finish coming up: {}.", request.Id,
                      m_FrameQuiescence.Describe() );
            m_ControlInFlight       = request;
            m_ControlInFlightClient = m_ControlSocket.ClientGeneration();
            m_ControlGate.ArmForReadiness( m_FrameIndex );
            return;
        }

        RunControlRequest( request );
    }

    // Execute one request and decide whether its reply leaves now or waits for the frame that proves it.
    //
    // Split out of ServiceControlChannel because there are now two ways to ARRIVE at a request — read from
    // the socket, or released by the readiness gate a few frames later — and exactly one way to RUN one.
    // Two execution sites for one thing is the shape this codebase spends its days removing.
    void ControlService::RunControlRequest( const Control::Request& request )
    {
        Control::Response response = ExecuteControlRequest( request );

        // READS ANSWER NOW; ANYTHING THAT CAN CHANGE THE PICTURE WAITS FOR ONE.
        //
        // `commands`, `properties` and `state` observe and change nothing, so making them wait for a
        // FURTHER frame would buy latency and no guarantee at all. `run`, `set` and the two shots are the
        // ones the promise is about — and a shot does not merely wait for the settled frame, it IS taken
        // on it, which is why its response is finished in OnFramePresented rather than here.
        //
        // NOT TO BE CONFUSED WITH THE READINESS WAIT ABOVE, which the reads DO take part in. The two are
        // different questions about different moments: "has the editor finished coming up, so that this
        // answer is about the real project?" is asked BEFORE a request runs, of every operation but the
        // two exemptions; "has a frame been presented that shows what this command did?" is asked AFTER,
        // and only of the commands that did something. By the time execution reaches this line the editor
        // is settled either way, so a read answers from a state it has actually finished building.
        //
        // `set` is in the list for exactly the reason `run` is: it moves the preview, and a client that
        // set a value and captured immediately would photograph the frame BEFORE it. That failure is the
        // whole subject of the sequence this document exists to prove.
        const bool waitsForAFrame =
             response.Ok() && ( request.Operation == Control::Op::Run || request.Operation == Control::Op::Set ||
                                request.Operation == Control::Op::Drag ||
                                request.Operation == Control::Op::Input || Control::IsShot( request.Operation ) );

        if ( !waitsForAFrame )
        {
            m_ControlSocket.SendResponseLine( Control::FormatResponse( response ) );
            if ( request.Operation == Control::Op::Quit && response.Ok() )
                m_ControlQuitCode = request.ExitCode;
            return;
        }

        m_ControlInFlight       = request;
        m_ControlPendingReply   = std::move( response );
        m_ControlInFlightClient = m_ControlSocket.ClientGeneration();
        m_ControlGate.ArmAfterExecution( m_FrameIndex );
    }

    /**
     * @brief Is the connection that asked still the connection on the other end? Abandon the request if
     *        not, and say whether it did.
     *
     * A REPLY MUST REACH THE CLIENT THAT ASKED FOR IT, and "somebody is connected" does not say that.
     * Measured while building the readiness wait: client A parked a `commands` during the boot and was
     * killed; the editor noticed the loss and, in the SAME service call, accepted client B into the freed
     * slot — the accept loop runs immediately after the read that detects a disconnect. HasClient() was
     * true again, the parked request went on, and A's answer of 311 commands was written to B's socket.
     * B had also sent id 1, so the reply was indistinguishable from its own at both ends.
     *
     * The generation is what makes the question answerable. Nothing is sent to the vanished client — there
     * is nobody to tell — and nothing is sent to its successor either, which is the whole point.
     */
    bool ControlService::AbandonControlRequestIfItsAskerIsGone()
    {
        if ( !m_ControlInFlight )
            return false;
        if ( m_ControlSocket.HasClient() && m_ControlSocket.ClientGeneration() == m_ControlInFlightClient )
            return false;

        LOG_INFO( "[Control] the client that asked for request {} is gone (generation {} -> {}); abandoning "
                  "it unanswered rather than replying to whoever holds the channel now.",
                  m_ControlInFlight->Id, m_ControlInFlightClient, m_ControlSocket.ClientGeneration() );

        m_ControlGate.Disarm();
        m_ControlInFlight.reset();
        m_ControlPendingReply.reset();
        m_ControlInFlightClient = 0;
        return true;
    }

    void ControlService::SampleFrameQuiescence( const bool startupLoading )
    {
        Control::EditorQuiescence quiescence;
        // CONTENT SETTLING COUNTS AS STARTUP LOADING FOR THE CHANNEL, and it has to: the channel's whole
        // contract is that a reply is released only after a presented frame on whose entry no deferred
        // work from that command remained. A `shot.viewport` answered while a worker is still reading the
        // sky would hand back a picture of a scene without its clouds and call it the scene.
        quiescence.Set( Control::PendingWork::StartupLoading, startupLoading );
        quiescence.Set( Control::PendingWork::SceneLoad, m_SceneFiles.HasPendingLoad() );
        quiescence.Set( Control::PendingWork::NewScene, m_SceneFiles.HasPendingNew() );
        // THE GRID BELONGS HERE TOO, and the pending DOCK is part of it: the layout is applied a frame
        // after the views open, so a reply released between the two would hand back a screenshot of four
        // floating windows and call it the grid.
        quiescence.Set( Control::PendingWork::SceneView, m_Workspace.HasPendingRequests() );
        quiescence.Set( Control::PendingWork::SceneStop, m_Play.HasPendingRequests() );
        quiescence.Set( Control::PendingWork::DocumentCloses, m_Documents.HasPendingCloses() );
        // The queue is a file-static inbox drained by ServiceSubjectOpenRequests, so "is anything queued"
        // is asked of the queue itself rather than of a copy this layer keeps — a copy would be a second
        // answer, and the two would disagree on exactly the frame an open was handled halfway.
        quiescence.Set( Control::PendingWork::AssetOpens,
                        Core::SubjectOpenRequests::HasPending() || Core::AssetFieldRequests::HasPending() );
        quiescence.Set( Control::PendingWork::OpenRefusal, m_Documents.HasPendingRefusal() );
        // Asked of the request itself, for SubjectOpenRequests' reason above. It stays pending for one
        // frame AFTER it is performed, because the frame that performs a nudge is not the frame that
        // draws it -- see Editor/Core/ControlNudgeRequest.hpp.
        quiescence.Set( Control::PendingWork::ControlNudge, Core::ControlNudgeRequests::HasPending() );
        quiescence.Set( Control::PendingWork::PointerDrag,
                        Control::PointerInjection::Playing() || Control::InputInjection::Playing() );
        // Asked of the run itself: it stays running until FinishCreateLandscape has applied it. Background work:
        // Create answers "started" at once and a client polls `state` until idle before photographing it.
        quiescence.Set( Control::BackgroundWork::LandscapeGenerate, Commands::IsCreatingLandscape() );
        m_FrameQuiescence = quiescence;
    }

    std::optional<int32_t> ControlService::OnFramePresented()
    {
        ++m_FrameIndex;

        if ( !m_ControlSocket.IsListening() )
            return std::nullopt;

        // A quit waits for its own answer to leave the machine. A client that asked the editor to close
        // and never heard back cannot tell a clean shutdown from a crash, and it is the last thing it
        // will ever hear from this process.
        if ( m_ControlQuitCode && !m_ControlSocket.HasUnsentOutput() )
        {
            const int32_t code = *m_ControlQuitCode;
            m_ControlQuitCode.reset();
            LOG_INFO( "[Control] quit requested; closing with status {}.", code );
            return code;
        }

        if ( !m_ControlInFlight )
            return std::nullopt;

        // A reply must reach the client that ASKED. Checked here as well as in ServiceControlChannel
        // because a shot's capture and its answer both happen on this side of the frame, and writing
        // either to a successor connection would hand one client another's picture.
        if ( AbandonControlRequestIfItsAskerIsGone() )
            return std::nullopt;

        // WHICH OF THE TWO WAITS this frame is being judged for, sampled BEFORE the verdict: a discharge
        // clears the subject, so asking afterwards would always answer "nothing".
        const Control::GateSubject holding = m_ControlGate.Holding();

        // The frame just presented is judged by the quiescence sampled while it was being BUILT. The gate
        // uses m_FrameIndex - 1 because the counter was advanced above: the frame that has just gone out
        // is the one that was being made when ServiceControlChannel armed the gate.
        const Control::GateVerdict verdict =
             m_ControlGate.ObserveFramePresented( m_FrameIndex - 1, m_FrameQuiescence );

        if ( verdict == Control::GateVerdict::Waiting || verdict == Control::GateVerdict::Idle )
            return std::nullopt;

        // THE READINESS HALF. The request has not run; this frame says whether it may.
        //
        // A discharge does nothing here on purpose: it leaves the request parked with an idle gate, which
        // is the one state ServiceControlChannel reads as "run this at the top of the next update". That
        // costs one frame and buys a single execution site for every control command — a request released
        // by this wait runs at exactly the point in the frame a request that never waited runs at.
        if ( holding == Control::GateSubject::Readiness )
        {
            if ( verdict == Control::GateVerdict::TimedOut )
            {
                m_ControlSocket.SendResponseLine( Control::FormatResponse( Control::Response::Failure(
                     m_ControlInFlight->Id,
                     Control::DescribeReadinessTimeout( m_FrameQuiescence, m_ControlGate.FramesWaited() ) ) ) );
                m_ControlInFlight.reset();
            }
            return std::nullopt;
        }

        // Unreachable while the two arms above are the only ways to arm the gate, and stated rather than
        // dereferenced: an Effect wait without a reply to release would be a request that ran and produced
        // nothing, which is the silent failure Response exists to make impossible.
        if ( !m_ControlPendingReply )
        {
            m_ControlSocket.SendResponseLine( Control::FormatResponse( Control::Response::Failure(
                 m_ControlInFlight->Id, "the channel held a reply-less request past its frame; that is a "
                                        "defect in the control channel, not in the request." ) ) );
            m_ControlInFlight.reset();
            return std::nullopt;
        }

        Control::Response reply = *m_ControlPendingReply;

        if ( verdict == Control::GateVerdict::TimedOut )
        {
            reply = Control::Response::Failure(
                 m_ControlInFlight->Id,
                 Control::DescribeSettleTimeout( m_FrameQuiescence, m_ControlGate.FramesWaited() ) );
        }
        else if ( Control::IsShot( m_ControlInFlight->Operation ) )
        {
            // THE SHOT IS TAKEN HERE AND NOWHERE ELSE, and that is the whole reason this hook exists.
            // This instant — after present, before the next acquire — is the only one at which the frame
            // a person would be looking at exists as bytes on the device, and it is a frame this gate has
            // just certified as reflecting the command that came before it.
            std::string error;
            const bool  window  = ( m_ControlInFlight->Operation == Control::Op::ShotWindow );
            const bool  written = window ? m_Capture.WriteWindowPng( m_ControlInFlight->Path, error )
                                         : m_Capture.WriteViewportPng( m_ControlInFlight->Path );

            if ( written )
            {
                rfl::Generic::Object payload;
                payload["path"] = rfl::Generic( m_ControlInFlight->Path );
                // Named on the wire so a report cannot quote a viewport capture as a picture of the
                // editor. The two are different subjects and only one of them contains an interface.
                payload["subject"] = rfl::Generic( std::string( window ? "window" : "viewport" ) );
                reply              = Control::Response::Success( m_ControlInFlight->Id, std::move( payload ) );
            }
            else
            {
                reply = Control::Response::Failure(
                     m_ControlInFlight->Id,
                     error.empty() ? "the capture could not be written; the log has the reason." : error );
            }
        }

        m_ControlSocket.SendResponseLine( Control::FormatResponse( reply ) );
        m_ControlInFlight.reset();
        m_ControlPendingReply.reset();
        return std::nullopt;
    }

    Control::Response ControlService::ExecuteControlRequest( const Control::Request& request )
    {
        switch ( request.Operation )
        {
            case Control::Op::Commands:
            {
                rfl::Generic::Array entries;
                for ( const PaletteCommand& command : BuildDictionary() )
                {
                    rfl::Generic::Object entry;
                    entry["group"] = rfl::Generic( command.Group );
                    entry["label"] = rfl::Generic( command.Label );
                    entries.push_back( rfl::Generic( entry ) );
                }

                rfl::Generic::Object payload;
                payload["commands"] = rfl::Generic( entries );
                return Control::Response::Success( request.Id, std::move( payload ) );
            }

            case Control::Op::Run:
            {
                // ONE dictionary, built once, both resolved against and run out of. Building it twice —
                // once to look the command up and once to run it — would let the two disagree on any
                // frame where something opened or closed in between, and the command that ran would not
                // be the command that was found.
                const std::vector<PaletteCommand> dictionary = BuildDictionary();
                const Control::CommandAddress     wanted{ request.Group, request.Label };
                const Control::Resolution         resolved = Control::ResolveCommand( dictionary, wanted );

                if ( !resolved.Found )
                {
                    return Control::Response::Failure( request.Id,
                                                       Control::DescribeUnknownCommand( wanted, resolved ) );
                }

                // THE COMMAND'S OWN ANSWER IS THE REPLY'S. `Run()` returned void until A6-2, so this line
                // reported success for every command that had failed — a document that would not resolve,
                // a scene that would not save, an Apply that published nothing. A script driving the
                // editor could not stop on any of them, and the whole point of the exit status is that it
                // can be trusted (Tools/DesertCtl/Source/Main.cpp says so at the top).
                if ( const auto ran = dictionary[resolved.Index].Run(); !ran )
                {
                    return Control::Response::Failure( request.Id, "'" + request.Group + "' / '" + request.Label +
                                                                        "' ran and refused: " + ran.GetError() );
                }
                return Control::Response::Success( request.Id );
            }

            case Control::Op::Properties:
            {
                if ( request.Whose == Control::Subject::Modeling )
                {
                    return Control::Response::Success(
                         request.Id,
                         Control::PropertiesToJson(
                              Control::kSubjects[static_cast<std::size_t>( Control::Subject::Modeling )].Name,
                              Core::DescribeModelingState( Core::ModelingState::Get() ) ) );
                }
                if ( request.Whose == Control::Subject::Selection )
                {
                    const auto selected = SelectedTransform();
                    if ( !selected )
                        return Control::Response::Failure( request.Id, selected.GetError() );
                    return Control::Response::Success(
                         request.Id,
                         Control::PropertiesToJson(
                              Control::kSubjects[static_cast<std::size_t>( Control::Subject::Selection )].Name,
                              Core::DescribeSelectionTransform( selected.GetValue().second ) ) );
                }
                if ( request.Whose == Control::Subject::Viewport )
                {
                    ::Desert::Core::EditorCamera* camera = m_Workspace.ActiveEditorCamera();
                    if ( !camera )
                        return Control::Response::Failure( request.Id, NoEditorCameraReason() );

                    return Control::Response::Success(
                         request.Id,
                         Control::PropertiesToJson(
                              Control::kSubjects[static_cast<std::size_t>( Control::Subject::Viewport )].Name,
                              DescribeViewportCamera( camera->GetPosition(), camera->GetDirection() ) ) );
                }

                // `m_Documents` here on А6-1's side; О9-2 moved document OWNERSHIP out of the well into
                // Editor/Core/OpenDocuments.hpp and renamed the member, so the merged line asks the owner.
                ISubjectDocument* focused = m_Documents.Documents().Find( m_Documents.FocusedDocument() );
                if ( !focused )
                {
                    return Control::Response::Failure(
                         request.Id,
                         "no document has the focus, so there is nothing whose properties could be listed. "
                         "Open one — 'commands' offers an entry per openable asset under the group 'Open'. "
                         "The editor's own view is a subject of its own: ask with subject 'viewport'." );
                }
                return Control::Response::Success(
                     request.Id, Control::PropertiesToJson( DocumentDisplayName( focused->GetName() ),
                                                            focused->EditableProperties() ) );
            }

            case Control::Op::Set:
            {
                if ( request.Whose == Control::Subject::Viewport )
                    return SetViewportCameraProperty( request );
                if ( request.Whose == Control::Subject::Selection )
                {
                    // One undo step, the TransformCommand a gizmo drag records: the new values are written,
                    // then RecordTransformEdit reads them back as the "after" of the step.
                    const auto selected = SelectedTransform();
                    if ( !selected )
                        return Control::Response::Failure( request.Id, selected.GetError() );
                    const auto& [uuid, before] = selected.GetValue();
                    const auto after = Core::WriteSelectionTransform( before, request.Property, request.Value );
                    if ( !after )
                        return Control::Response::Failure( request.Id, after.GetError() );
                    const auto ref = m_Workspace.ActiveScene()->FindEntityByID( uuid );
                    if ( !ref )
                        return Control::Response::Failure(
                             request.Id, fmt::format( "the selected entity {} is not in the scene",
                                                      static_cast<uint64_t>( uuid ) ) );
                    const ECS::Entity entity = ref->get();
                    auto&       tc     = entity.GetComponent<ECS::TransformComponent>();
                    tc.Translation     = after.GetValue().Translation;
                    tc.Rotation        = after.GetValue().Rotation;
                    tc.Scale           = after.GetValue().Scale;
                    Commands::RecordTransformEdit( uuid, before.Translation, before.Rotation, before.Scale );
                    return Control::Response::Success( request.Id );
                }
                if ( request.Whose == Control::Subject::Modeling )
                {
                    if ( const auto written = Core::SetModelingStateProperty( Core::ModelingState::Get(),
                                                                              request.Property, request.Value );
                         !written )
                        return Control::Response::Failure( request.Id, written.GetError() );
                    return Control::Response::Success( request.Id );
                }

                // THE FOCUSED DOCUMENT AND NO OTHER. A property named without a document would have to be
                // searched for across every open window, and the first match would win — which is a
                // different document from the one the person or the capture is looking at, on any frame
                // where two materials declare the same parameter. They almost all do.
                ISubjectDocument* focused = m_Documents.Documents().Find( m_Documents.FocusedDocument() );
                if ( !focused )
                {
                    return Control::Response::Failure(
                         request.Id, "no document has the focus, so '" + request.Property +
                                          "' belongs to nothing. Open the document first; 'state' names the "
                                          "one that has the focus." );
                }

                if ( const auto written = focused->SetEditableProperty( request.Property, request.Value );
                     !written )
                {
                    return Control::Response::Failure( request.Id, written.GetError() );
                }
                return Control::Response::Success( request.Id );
            }

            case Control::Op::State:
            {
                if ( const auto valid = Control::ValidateSections( request.Sections ); !valid )
                    return Control::Response::Failure( request.Id, valid.GetError() );

                return Control::Response::Success( request.Id,
                                                   Control::ToJson( TakeEditorSnapshot(), request.Sections ) );
            }

            case Control::Op::ShotWindow:
            case Control::Op::ShotViewport:
                // Nothing happens now. The capture belongs to the settled frame this request is about to
                // wait for, and is taken in OnFramePresented; answering here would be a picture of the
                // frame BEFORE the commands that preceded it had been drawn.
                return Control::Response::Success( request.Id );

            case Control::Op::Quit:
                return Control::Response::Success( request.Id );

            case Control::Op::Drag:
            {
                const auto target =
                     Control::PointerInjection::FreshTarget( request.Whose, ::ImGui::GetFrameCount() );
                if ( !target )
                {
                    return Control::Response::Failure(
                         request.Id, request.Whose == Control::Subject::Viewport
                                          ? "no level viewport image was drawn in the last frame."
                                          : "no document view (the Animation window's preview) was drawn in the "
                                            "last frame; open and focus one first." );
                }
                const auto plan = Control::PointerDrag::Plan(
                     *target, { request.Value[0], request.Value[1], request.Value[2], request.Value[3] },
                     request.Steps, ::ImGui::GetIO().DisplayFramebufferScale.x );
                if ( !plan.IsSuccess() )
                    return Control::Response::Failure( request.Id, plan.GetError() );
                if ( const auto armed = Control::PointerInjection::Arm( plan.GetValue() ); !armed.IsSuccess() )
                    return Control::Response::Failure( request.Id, armed.GetError() );
                return Control::Response::Success( request.Id );
            }

            case Control::Op::Input:
            {
                const ImGuiViewport* mainViewport = ::ImGui::GetMainViewport();
                ImVec2               origin( 0.0f, 0.0f );
                if ( !request.Panel.empty() )
                {
                    const ImGuiWindow* window = nullptr;
                    for ( const auto& panel : m_Panels )
                    {
                        if ( panel->GetVisibility() && ( panel->GetName() == request.Panel ||
                                                         PanelShownName( panel->GetName() ) == request.Panel ) )
                            window = ::ImGui::FindWindowByName( PanelDisplayTitle( panel->GetName() ).c_str() );
                    }
                    if ( window == nullptr || !window->WasActive )
                        return Control::Response::Failure(
                             request.Id, std::format( "no open panel named '{}' was drawn in the last frame.",
                                                      request.Panel ) );
                    if ( request.InputKind != "key" &&
                         ( request.Value[0] < 0.0f || request.Value[1] < 0.0f ||
                           request.Value[0] >= window->Size.x || request.Value[1] >= window->Size.y ) )
                        return Control::Response::Failure(
                             request.Id,
                             std::format( "({}, {}) lies outside '{}', which is {}x{} points.", request.Value[0],
                                          request.Value[1], request.Panel, window->Size.x, window->Size.y ) );
                    origin = ImVec2( window->Pos.x - mainViewport->Pos.x, window->Pos.y - mainViewport->Pos.y );
                }
                auto plan =
                     Control::PlanInput( request.InputKind, origin.x + request.Value[0],
                                         origin.y + request.Value[1], request.Button, request.Key, request.Paths );
                if ( !plan.IsSuccess() )
                    return Control::Response::Failure( request.Id, plan.GetError() );
                if ( const auto armed = Control::InputInjection::Arm( plan.ExtractValue() ); !armed.IsSuccess() )
                    return Control::Response::Failure( request.Id, armed.GetError() );
                return Control::Response::Success( request.Id );
            }
        }

        // Unreachable while every Op is handled above, and stated rather than left to fall off the end:
        // an Op added without a case here would otherwise return a default-constructed response, which is
        // a failure with nothing said — the one thing Response is built to make impossible.
        return Control::Response::Failure( request.Id,
                                           "this operation parsed but has no implementation; that is a "
                                           "defect in the control channel." );
    }

    std::vector<PaletteCommand> ControlService::BuildDictionary() const
    {
        std::vector<PaletteCommand> commands;
        m_Commands.Build( commands );
        return commands;
    }

    std::string ControlService::NoEditorCameraReason() const
    {
        if ( !m_Workspace.ActiveScene() )
            return "there is no scene, so there is no view to address.";
        return "the active view is not the editor's fly camera — the scene is in Play and its own "
               "CameraComponent is driving. Leave Play ('Action' / 'Stop' in the palette) and ask again; "
               "moving the editor camera now would change a view nobody is looking through.";
    }

    Common::ResultStr<std::pair<Common::UUID, Core::SelectionTransform>> ControlService::SelectedTransform() const
    {
        using Result = std::pair<Common::UUID, Core::SelectionTransform>;
        const auto selected = Core::SelectionManager::GetSelected();
        if ( Core::SelectionManager::Count() != 1 || !selected.has_value() )
            return Common::MakeFormattedError<Result>(
                 "the selection subject is one selected entity, and {} are selected. Select one first: 'run "
                 "Entity <tag>'.",
                 Core::SelectionManager::Count() );
        const Common::UUID uuid = *selected;
        const auto         ref =
             m_Workspace.ActiveScene() ? m_Workspace.ActiveScene()->FindEntityByID( uuid ) : std::nullopt;
        if ( !ref )
            return Common::MakeFormattedError<Result>( "the selected entity {} is not in the scene",
                                                       static_cast<uint64_t>( uuid ) );
        const ECS::Entity entity = ref->get();
        if ( !entity.HasComponent<ECS::TransformComponent>() )
            return Common::MakeFormattedError<Result>( "the selected entity {} has no transform",
                                                       static_cast<uint64_t>( uuid ) );
        const auto& tc = entity.GetComponent<ECS::TransformComponent>();
        return Common::MakeSuccess(
             Result{ uuid, Core::SelectionTransform{ tc.Translation, tc.Rotation, tc.Scale } } );
    }

    Control::Response ControlService::SetViewportCameraProperty( const Control::Request& request )
    {
        ::Desert::Core::EditorCamera* camera = m_Workspace.ActiveEditorCamera();
        if ( !camera )
            return Control::Response::Failure( request.Id, NoEditorCameraReason() );

        const auto which = ValidateViewportCameraWrite( request.Property, request.Value );
        if ( !which )
            return Control::Response::Failure( request.Id, which.GetError() );

        // THE OTHER HALF OF THE POSE IS READ BACK FROM THE CAMERA, not remembered here. A client that sets
        // only the direction means "look that way from where you are", and a copy of the position kept on
        // this side would be the second answer to where the camera is — wrong the first time a person
        // dragged it.
        const glm::vec3 position = ( which.GetValue() == ViewportCameraWrite::Position )
                                        ? glm::vec3( request.Value[0], request.Value[1], request.Value[2] )
                                        : camera->GetPosition();
        const glm::vec3 forward  = ( which.GetValue() == ViewportCameraWrite::Direction )
                                        ? glm::vec3( request.Value[0], request.Value[1], request.Value[2] )
                                        : camera->GetDirection();

        PlaceEditorCamera( *camera, position, forward );

        // INPUT IS NOT DISABLED, and the difference from the capture path is deliberate. `--shot` turns it
        // off because nothing may nudge the camera between the placement and the readback, and there is no
        // one at the keyboard anyway. A channel client may well be driving an editor a person is also
        // sitting at, and taking their camera away for the rest of the session would be a side effect they
        // never asked for and could not undo. The ordering guarantee already covers the capture case: the
        // reply is released only after a frame rendered from this pose.
        return Control::Response::Success( request.Id );
    }

    Control::EditorSnapshot ControlService::TakeEditorSnapshot() const
    {
        Control::EditorSnapshot snapshot;

        snapshot.SceneName = m_Workspace.ActiveScene() ? m_Workspace.ActiveScene()->GetSceneName() : std::string();
        snapshot.SceneHasUnsavedChanges = m_SceneFiles.HasUnsavedChanges();
        snapshot.InPlayMode             = m_Play.InPlayMode();

        for ( const Common::UUID& uuid : Core::SelectionManager::GetSelection() )
        {
            Control::EntitySnapshot entity;
            entity.Uuid = uuid.ToString();
            if ( m_Workspace.ActiveScene() )
            {
                for ( const auto& candidate : m_Workspace.ActiveScene()->GetAllEntities() )
                {
                    if ( !candidate.HasComponent<ECS::UUIDComponent>() )
                        continue;
                    if ( candidate.GetComponent<ECS::UUIDComponent>().UUID != uuid )
                        continue;
                    if ( candidate.HasComponent<ECS::TagComponent>() )
                        entity.Tag = candidate.GetComponent<ECS::TagComponent>().Tag;
                    break;
                }
            }
            snapshot.Selection.push_back( std::move( entity ) );
        }

        // MOST RECENTLY USED ORDER, which is the order the well lists and Ctrl+Tab walks. Reporting the
        // storage order instead would be a second sequence for the same documents, and a client reading
        // it would predict a different answer from Ctrl+Tab than the editor gives.
        for ( const SubjectId& subject : m_Documents.Well().MostRecentOrder() )
        {
            const ISubjectDocument* document = m_Documents.Documents().Find( subject );
            if ( !document )
                continue;

            Control::DocumentSnapshot entry;
            entry.Name               = DocumentDisplayName( document->GetName() );
            entry.Type               = m_Documents.SubjectEditors().TypeName( subject );
            entry.Subject            = subject.ToString();
            entry.HoldsView          = document->HoldsView();
            entry.ClaimsView         = document->ClaimsView();
            entry.ViewForecastBytes  = document->ViewForecastBytes();
            entry.Focused            = ( subject == m_Documents.FocusedDocument() );

            // The three states, asked of the document itself. Written out as words here rather than
            // exported as enums, because the wire is read by clients that have none of our headers.
            entry.EditModel =
                 ( document->GetEditModel() == ISubjectDocument::EditModel::Staged ) ? "staged" : "write-through";
            entry.HasUnappliedEdits = document->HasUnappliedEdits();
            switch ( document->GetDiskState() )
            {
                case ISubjectDocument::DiskState::Clean:
                    entry.DiskState = "clean";
                    break;
                case ISubjectDocument::DiskState::Dirty:
                    entry.DiskState = "dirty";
                    break;
                case ISubjectDocument::DiskState::Untracked:
                    // NOT "clean". A document that took no snapshot has no evidence about its file, and a
                    // client that read the two as one would report an unsaved edit as saved.
                    entry.DiskState = "untracked";
                    break;
            }

            snapshot.Documents.push_back( std::move( entry ) );
        }

        snapshot.DocumentWellOpen = m_Documents.Well().IsWindowOpen();
        for ( const ClosedDocument& closed : m_Documents.Well().RecentlyClosed() )
        {
            Control::ClosedDocumentSnapshot entry;
            entry.Name    = closed.DisplayName;
            entry.Type    = m_Documents.SubjectEditors().TypeName( closed.Subject );
            entry.Subject = closed.Subject.ToString();
            snapshot.RecentlyClosed.push_back( std::move( entry ) );
        }

        // TOOLS ONLY, and by construction: m_Panels is a PanelRegistry, which cannot hold a document.
        // A client reading this list is reading exactly what the View menu lists.
        for ( const auto& panel : m_Panels )
        {
            Control::PanelSnapshot entry;
            entry.Name = panel->GetName();
            if ( const auto hash = entry.Name.find( "##" ); hash != std::string::npos )
                entry.Name.erase( hash );
            entry.Visible    = panel->GetVisibility();
            entry.Pinned     = panel->Pinned();
            entry.Contextual = panel->IsContextual();
            entry.Relevant   = panel->IsRelevant();
            snapshot.Panels.push_back( std::move( entry ) );
        }

        {
            // The same reading and holdings a refusal is decided on (Graphic::ReadViewBudget), so the state a
            // client reads and the verdict the editor gives cannot disagree.
            const auto holdings = Graphic::SceneRenderer::LiveHoldings();
            snapshot.ViewsLive  = static_cast<uint32_t>( holdings.size() );
            for ( const Engine::ViewBudget::HeldView& view : holdings )
                snapshot.ViewBytes += view.Bytes;
            snapshot.PendingViewBytes                 = PendingViewBytes( m_Documents.Documents().Documents() );
            const Engine::ViewBudget::Reading reading = Graphic::ReadViewBudget();
            snapshot.BudgetBytes                      = reading.CeilingBytes;
            snapshot.UsageBytes                       = reading.UsageBytes;
            snapshot.IsmInstancesDrawn =
                 m_Workspace.PrimaryRenderer() ? m_Workspace.PrimaryRenderer()->GetIsmInstancesDrawn() : 0u;
        }

        snapshot.LogInfoCount    = LogsPanel::InfoCount();
        snapshot.LogWarningCount = LogsPanel::WarningCount();
        snapshot.LogErrorCount   = LogsPanel::ErrorCount();
        snapshot.LogTail         = LogsPanel::Tail( 40 );

        // 07 §14.2's mode, so a window capture of the overlay can be read with a number beside it.
        {
            const auto& authoring     = Core::ActiveAuthoringContext();
            snapshot.Authoring.Mode   = Core::AuthoringModeName( authoring.Mode() );
            snapshot.Authoring.Holder = authoring.Holder().Describe();
            snapshot.Authoring.Entity =
                 authoring.Entity().IsNull() ? std::string() : authoring.Entity().ToString();
            snapshot.Authoring.SelectedBone = authoring.SelectedBoneIndex();
            snapshot.Authoring.SelectedControl =
                 authoring.SelectedControl() ? static_cast<int>( *authoring.SelectedControl() ) : -1;
            snapshot.Authoring.ShowBoneNames    = authoring.ShowBoneNames();
            snapshot.Authoring.PreviewsBindPose = authoring.PreviewsBindPose();
        }

        snapshot.Quiescence = m_FrameQuiescence;
        return snapshot;
    }

    void ControlService::RecordWindowCaptureIfDue()
    {
        // THE CAPTURE IS RECORDED WHILE THE FRAME IS STILL BEING BUILT, and that is not an optimisation.
        //
        // A swapchain image may only be touched between its acquire and its present. The first version of
        // this read it back after the present, in OnFramePresented, and the picture was correct — which is
        // exactly what made it dangerous. Only the validation layer objected: "vkQueueSubmit(): performs a
        // layout transition on presentable VkImage, but the image has not been acquired from
        // VkSwapchainKHR". A capture that quietly breaks the frame loop it was taken to document is worth
        // less than no capture.
        //
        // So the editor asks the gate, before the submit, whether THIS frame is the one the reply waits
        // for — the same question, on the same inputs, that OnFramePresented will answer afterwards.
        if ( !m_ControlInFlight || m_ControlInFlight->Operation != Control::Op::ShotWindow )
            return;
        // THE EFFECT WAIT AND NOT THE READINESS ONE. A shot parked behind the readiness gate has not been
        // taken yet — its `run`-like half is precisely this capture — so recording on the frame that
        // merely proves the editor came up would photograph the boot instead of the thing asked for, and
        // it would still be released as the answer to the request. The gate's two subjects are what keep
        // "the editor is ready" and "the command has landed" from being read as one fact.
        if ( m_ControlGate.Holding() != Control::GateSubject::Effect )
            return;
        if ( !m_ControlGate.WouldDischarge( m_FrameIndex, m_FrameQuiescence ) )
            return;

        m_Capture.RecordWindowCapture();
    }

    void ControlService::Close()
    {
        // The socket goes, and its file with it. A leftover path is not harmless: the next editor
        // to be given it PROBES what is there, and while a dead one only costs a log line, leaving the
        // file behind on every exit would train everybody to ignore that line.
        //
        // A request still in flight is abandoned rather than answered — the frame that would have proved
        // it is never going to be drawn, and a reply promising otherwise is exactly the lie this channel
        // is built to prevent. The client sees the connection close, which is the truth.
        if ( m_ControlInFlight )
        {
            LOG_WARN( "[Control] the editor is closing with a '{}' still in flight; it is abandoned rather "
                      "than answered, because the frame that would have proved it will not be drawn.",
                      m_ControlInFlight->Group.empty() ? "request" : m_ControlInFlight->Label );
        }
        m_ControlGate.Disarm();
        m_ControlSocket.Close();
    }
} // namespace Desert::Editor
