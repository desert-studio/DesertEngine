#pragma once

#include "Editor/Core/CommandPalette.hpp"

#include <Common/Core/LocalSocket.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Scripting/ScriptDebugger.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Engine
{
    class Application;
}

namespace Desert::Editor
{
    class PlaySession;

    /// THE SCRIPT DEBUGGER'S DAP SERVER — VS Code (any Debug Adapter Protocol client) attaches to the running
    /// editor over loopback TCP and debugs the scripts of the edited and the played world: breakpoints, the
    /// call stack, locals / upvalues / globals, continue, step over / in / out, pause.
    ///
    /// Started on demand (palette: Script Debugger ▸ Start, port EditorPreferences::ScriptDebugPort) and
    /// serviced once a frame from EditorLayer::OnUpdate. One client at a time; its `attach` attaches the
    /// process's script debugger (Scripting::ScriptDebugger()), and its leaving detaches it, so a script never
    /// stays stopped for a client that is gone.
    ///
    /// AT A STOP the game thread is inside the script, and this is the host of the stopped loop
    /// (IScriptDebugHost::WhileStopped): it answers the client and pumps the window's events so the OS never
    /// calls the editor unresponsive, and nothing ticks a world. The client's `terminate` ends Play: the script
    /// is aborted (Scripting::IScriptDebugger::Abort) and the Stop is REQUESTED, so the played world is torn
    /// down by PlaySession::ServiceRequests at the next frame boundary, never under the script's stack.
    class ScriptDebugServer final : public Scripting::IScriptDebugHost
    {
    public:
        ScriptDebugServer( PlaySession& play, Engine::Application* const& app );
        ~ScriptDebugServer() override;

        ScriptDebugServer( const ScriptDebugServer& )            = delete;
        ScriptDebugServer& operator=( const ScriptDebugServer& ) = delete;

        /// Listens on 127.0.0.1:`port`.
        [[nodiscard]] Common::BoolResultStr Start( int port );
        /// Drops the client (detaching the debugger) and stops listening.
        void               Stop();
        [[nodiscard]] bool IsListening() const noexcept;

        /// Once a frame: accept a client, answer its requests, flush the replies.
        void Service();

        /// The palette's Start / Stop entries.
        void AppendCommands( std::vector<PaletteCommand>& out );

        // ---- IScriptDebugHost ----
        void OnStopped( Scripting::DebugStopReason reason, const std::string& description ) override;
        void WhileStopped() override;
        void OnResumed( std::chrono::steady_clock::duration held ) override;
        void OnBreakpointsBound( const std::filesystem::path&                   file,
                                 const std::vector<Scripting::DebugBreakpoint>& breakpoints ) override;

    private:
        void Accept();
        void Receive();
        void Dispatch( const std::string& body );
        void Flush();
        void DropClient();
        void Send( const std::string& json );

        template <typename Body>
        void Respond( int requestSeq, const std::string& command, const Body& body );
        void Refuse( int requestSeq, const std::string& command, const std::string& message );
        template <typename Body>
        void Emit( const std::string& event, const Body& body );

        PlaySession&                m_Play;
        Engine::Application* const& m_App;

        Common::LocalSocket::Handle m_ListenFd = Common::LocalSocket::kInvalid;
        Common::LocalSocket::Handle m_ClientFd = Common::LocalSocket::kInvalid;
        int                         m_Port     = 0;
        std::string                 m_Incoming;
        std::string                 m_Outgoing;
        int                         m_Seq = 1;
    };
} // namespace Desert::Editor
