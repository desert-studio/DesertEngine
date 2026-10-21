#include "Editor/LevelEditor/ScriptDebugServer.hpp"

#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <Engine/Core/Application.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <optional>
#include <thread>

namespace Desert::Editor
{
    namespace Socket = Common::LocalSocket;

    // ---- THE WIRE: the Debug Adapter Protocol's messages ------------------------------------------------------
    // Replies are plain structs written through the facade; member names are the protocol's own keys (camelCase,
    // `request_seq`), because reflection writes a member's name as its key.
    // A REQUEST is another program's message, not a document this engine owns: the client sends keys the server
    // never reads (columns, conditions, presentation hints, a source reference). So a request is not read as a
    // struct (Json::Read refuses an unknown key, which is right for our own files); it is parsed once and walked
    // through Json::Node, reading exactly the members the server uses. An absent member keeps its default (DAP's
    // optional arguments), a wrong-typed one is an Issue naming its path ("arguments.frameId: expected ...").
    namespace Dap
    {
        using Common::Json::Issues;
        using Common::Json::Kind;
        using Common::Json::Node;

        struct RequestHeader
        {
            int         seq = 0;
            std::string type;
            std::string command;
        };

        RequestHeader ReadHeader( const Node& request, Issues& issues )
        {
            RequestHeader header;
            request.ReadInto( "seq", header.seq, issues );
            request.ReadInto( "type", header.type, issues );
            request.ReadInto( "command", header.command, issues );
            return header;
        }

        // The request's `arguments` object, or nothing when the request states none (every argument keeps its
        // default); a non-object `arguments` is an Issue.
        std::optional<Node> Arguments( const Node& request, Issues& issues )
        {
            auto arguments = request.Find( "arguments" );
            if ( arguments && !arguments->ExpectKind( Kind::Object, issues ) )
                return std::nullopt;
            return arguments;
        }

        struct Source
        {
            std::string name;
            std::string path;
        };

        struct SetBreakpointsArguments
        {
            std::string      sourcePath;
            std::vector<int> lines;
        };
        SetBreakpointsArguments ReadSetBreakpoints( const Node& request, Issues& issues )
        {
            SetBreakpointsArguments out;
            const auto              arguments = Arguments( request, issues );
            if ( !arguments )
                return out;
            if ( const auto source = arguments->Find( "source" );
                 source && source->ExpectKind( Kind::Object, issues ) )
                source->ReadInto( "path", out.sourcePath, issues );
            if ( const auto breakpoints = arguments->Find( "breakpoints" );
                 breakpoints && breakpoints->ExpectKind( Kind::Array, issues ) )
                breakpoints->ForEachElement(
                     [&]( std::size_t, const Node& breakpoint )
                     {
                         int line = 0;
                         breakpoint.ReadInto( "line", line, issues );
                         out.lines.push_back( line );
                     } );
            return out;
        }

        // One integer argument (scopes' frameId, variables' variablesReference); 0 when the request omits it.
        int ReadIntArgument( const Node& request, std::string_view key, Issues& issues )
        {
            int value = 0;
            if ( const auto arguments = Arguments( request, issues ) )
                arguments->ReadInto( key, value, issues );
            return value;
        }

        bool ReadTerminateDebuggee( const Node& request, Issues& issues )
        {
            bool terminate = false;
            if ( const auto arguments = Arguments( request, issues ) )
                arguments->ReadInto( "terminateDebuggee", terminate, issues );
            return terminate;
        }

        template <typename Body>
        struct Response
        {
            int                        seq         = 0;
            std::string                type        = "response";
            int                        request_seq = 0; // NOLINT(readability-identifier-naming): the wire key
            bool                       success     = true;
            std::string                command;
            std::optional<std::string> message;
            Body                       body;
        };

        template <typename Body>
        struct Event
        {
            int         seq  = 0;
            std::string type = "event";
            std::string event;
            Body        body;
        };

        using NoBody = Common::Json::Object;

        struct Capabilities
        {
            bool supportsConfigurationDoneRequest = true;
            bool supportsTerminateRequest         = true;
        };

        struct Breakpoint
        {
            int                        id       = 0;
            bool                       verified = false;
            int                        line     = 0;
            std::optional<std::string> message;
        };
        struct BreakpointsBody
        {
            std::vector<Breakpoint> breakpoints;
        };
        struct BreakpointEventBody
        {
            std::string reason = "changed";
            Breakpoint  breakpoint;
        };

        struct Thread
        {
            int         id = 0;
            std::string name;
        };
        struct ThreadsBody
        {
            std::vector<Thread> threads;
        };

        struct StackFrame
        {
            int                   id = 0;
            std::string           name;
            std::optional<Source> source;
            int                   line   = 0;
            int                   column = 0;
        };
        struct StackTraceBody
        {
            std::vector<StackFrame> stackFrames;
            int                     totalFrames = 0;
        };

        struct Scope
        {
            std::string name;
            int         variablesReference = 0;
            bool        expensive          = false;
        };
        struct ScopesBody
        {
            std::vector<Scope> scopes;
        };

        struct Variable
        {
            std::string name;
            std::string value;
            std::string type;
            int         variablesReference = 0;
        };
        struct VariablesBody
        {
            std::vector<Variable> variables;
        };

        struct ContinueBody
        {
            bool allThreadsContinued = true;
        };

        struct StoppedBody
        {
            std::string reason;
            std::string description;
            int         threadId          = 1;
            bool        allThreadsStopped = true;
        };
    } // namespace Dap

    namespace
    {
        /// The one thread a client sees: scripts run on the game thread only.
        constexpr int kGameThread = 1;

        /// A DAP message larger than this is a client that lost the framing, not a request.
        constexpr std::size_t kMaxMessageBytes = std::size_t{ 16 } << 20U;

        constexpr std::size_t kMaxSendChunk = std::size_t{ 64 } * 1024;

        /// How long one turn of the stopped loop sleeps: the client is a person in an IDE, the window wants its
        /// events a few dozen times a second, and a busy loop would hold a core for as long as the stop lasts.
        constexpr std::chrono::milliseconds kStoppedTurn{ 5 };

        constexpr std::string_view kContentLength = "Content-Length:";

        Dap::Breakpoint ToWire( const Scripting::DebugBreakpoint& breakpoint )
        {
            Dap::Breakpoint wire{ .id       = breakpoint.Id,
                                  .verified = breakpoint.Verified,
                                  .line     = breakpoint.Verified ? breakpoint.Line : breakpoint.RequestedLine,
                                  .message  = std::nullopt };
            if ( !breakpoint.Verified )
                wire.message = "no loaded script has this file yet; it binds when one loads";
            return wire;
        }

        const char* ReasonName( Scripting::DebugStopReason reason )
        {
            switch ( reason )
            {
                case Scripting::DebugStopReason::Breakpoint:
                    return "breakpoint";
                case Scripting::DebugStopReason::Step:
                    return "step";
                case Scripting::DebugStopReason::Pause:
                    return "pause";
            }
            return "pause";
        }
    } // namespace

    ScriptDebugServer::ScriptDebugServer( PlaySession& play, Engine::Application* const& app )
         : m_Play( play ), m_App( app )
    {
    }

    ScriptDebugServer::~ScriptDebugServer()
    {
        Stop();
    }

    bool ScriptDebugServer::IsListening() const noexcept
    {
        return m_ListenFd >= 0;
    }

    Common::BoolResultStr ScriptDebugServer::Start( int port )
    {
        if ( IsListening() )
            return Common::MakeFormattedError<bool>( "the script debug server already listens on port {}",
                                                     m_Port );
        if ( port <= 0 || port > 65535 )
            return Common::MakeFormattedError<bool>( "script debug port {} is not a TCP port (1..65535)", port );
        if ( const std::string unavailable = Socket::EnsureLibraryReady(); !unavailable.empty() )
            return Common::MakeFormattedError<bool>( "script debug server: the socket library could not start: {}",
                                                     unavailable );

        const Socket::Handle fd = Socket::OpenTcp();
        if ( fd < 0 )
            return Common::MakeFormattedError<bool>( "script debug server: socket() failed: {}",
                                                     Socket::LastErrorText() );
        const sockaddr_in address = Socket::LoopbackAddress( static_cast<std::uint16_t>( port ) );
        if ( !Socket::AllowAddressReuse( fd ) ||
             ::bind( Socket::Raw( fd ), Socket::AsSockaddr( address ), sizeof( address ) ) != 0 ||
             ::listen( Socket::Raw( fd ), 1 ) != 0 || !Socket::SetNonBlocking( fd ) )
        {
            const std::string reason = Socket::LastErrorText();
            Socket::Close( fd );
            return Common::MakeFormattedError<bool>( "script debug server: cannot listen on 127.0.0.1:{}: {}",
                                                     port, reason );
        }
        m_ListenFd = fd;
        m_Port     = port;
        LOG_INFO( "[ScriptDebug] DAP server listening on 127.0.0.1:{}", port );
        return Common::MakeSuccess( true );
    }

    void ScriptDebugServer::Stop()
    {
        DropClient();
        if ( m_ListenFd >= 0 )
        {
            Socket::Close( m_ListenFd );
            m_ListenFd = Socket::kInvalid;
            LOG_INFO( "[ScriptDebug] DAP server on port {} stopped", m_Port );
        }
    }

    void ScriptDebugServer::Service()
    {
        if ( !IsListening() )
            return;
        Flush();
        Receive();
        Accept();
    }

    void ScriptDebugServer::AppendCommands( std::vector<PaletteCommand>& out )
    {
        out.push_back( PaletteCommand{ .Group = "Script Debugger", .Label = "Start DAP Server", .Run = [this] {
                                          return Start( EditorPreferences::Get().ScriptDebugPort );
                                      } } );
        out.push_back( PaletteCommand{ .Group = "Script Debugger",
                                       .Label = "Stop DAP Server",
                                       .Run   = [this]
                                       {
                                           Stop();
                                           return PaletteCommandDone();
                                       } } );
    }

    // ---- IScriptDebugHost ----------------------------------------------------------------------------------

    void ScriptDebugServer::OnStopped( Scripting::DebugStopReason reason, const std::string& description )
    {
        LOG_INFO( "[ScriptDebug] stopped: {}", description );
        Emit( "stopped", Dap::StoppedBody{ .reason            = ReasonName( reason ),
                                           .description       = description,
                                           .threadId          = kGameThread,
                                           .allThreadsStopped = true } );
    }

    void ScriptDebugServer::WhileStopped()
    {
        Service();
        if ( m_App != nullptr && m_App->GetWindow() )
            m_App->GetWindow()->ProcessEvents();
        std::this_thread::sleep_for( kStoppedTurn );
    }

    void ScriptDebugServer::OnResumed( std::chrono::steady_clock::duration held )
    {
        // The frame that stopped resumes now; the next one must not age the world by the stop.
        if ( m_App != nullptr )
            m_App->ExcludeFromNextTimestep( std::chrono::duration<float>( held ).count() );
    }

    void ScriptDebugServer::OnBreakpointsBound( const std::filesystem::path&                   file,
                                                const std::vector<Scripting::DebugBreakpoint>& breakpoints )
    {
        LOG_INFO( "[ScriptDebug] breakpoints of '{}' bound on load", file.generic_string() );
        for ( const Scripting::DebugBreakpoint& breakpoint : breakpoints )
            Emit( "breakpoint",
                  Dap::BreakpointEventBody{ .reason = "changed", .breakpoint = ToWire( breakpoint ) } );
    }

    // ---- transport -----------------------------------------------------------------------------------------

    void ScriptDebugServer::Accept()
    {
        for ( ;; )
        {
            const auto incoming =
                 static_cast<Socket::Handle>( ::accept( Socket::Raw( m_ListenFd ), nullptr, nullptr ) );
            if ( incoming < 0 )
                return;
            if ( m_ClientFd >= 0 || !Socket::SetNonBlocking( incoming ) )
            {
                LOG_WARN( "[ScriptDebug] refused a second debugger client (one at a time)" );
                Socket::Close( incoming );
                continue;
            }
#if defined( SO_NOSIGPIPE )
            const int on = 1;
            (void)::setsockopt( Socket::Raw( incoming ), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof( on ) );
#endif
            m_ClientFd = incoming;
            m_Incoming.clear();
            m_Outgoing.clear();
            LOG_INFO( "[ScriptDebug] debugger client connected" );
        }
    }

    void ScriptDebugServer::Receive()
    {
        std::array<char, 4096> buffer{};
        while ( m_ClientFd >= 0 )
        {
            const auto received = static_cast<std::ptrdiff_t>(
                 ::recv( Socket::Raw( m_ClientFd ), buffer.data(), static_cast<int>( buffer.size() ), 0 ) );
            if ( received > 0 )
            {
                m_Incoming.append( buffer.data(), static_cast<std::size_t>( received ) );
                continue;
            }
            if ( received < 0 && Socket::LastErrorIsWouldBlock() )
                break;
            if ( received < 0 && Socket::LastErrorIsInterrupted() )
                continue;
            LOG_INFO( "[ScriptDebug] debugger client disconnected" );
            DropClient();
            return;
        }

        // Every complete message: `Content-Length: N` header lines, a blank line, N bytes of JSON.
        while ( m_ClientFd >= 0 )
        {
            const std::size_t headerEnd = m_Incoming.find( "\r\n\r\n" );
            if ( headerEnd == std::string::npos )
            {
                if ( m_Incoming.size() > kMaxMessageBytes )
                    DropClient();
                return;
            }
            const std::size_t at     = m_Incoming.find( kContentLength );
            std::size_t       length = 0;
            if ( at == std::string::npos || at > headerEnd )
            {
                LOG_WARN( "[ScriptDebug] a message without Content-Length; dropping the client" );
                DropClient();
                return;
            }
            std::size_t first = at + kContentLength.size();
            while ( first < headerEnd && m_Incoming[first] == ' ' )
                ++first;
            const auto parsed =
                 std::from_chars( m_Incoming.data() + first, m_Incoming.data() + headerEnd, length );
            if ( parsed.ec != std::errc{} || length > kMaxMessageBytes )
            {
                LOG_WARN( "[ScriptDebug] an unreadable Content-Length; dropping the client" );
                DropClient();
                return;
            }
            const std::size_t bodyStart = headerEnd + 4;
            if ( m_Incoming.size() < bodyStart + length )
                return;
            const std::string body = m_Incoming.substr( bodyStart, length );
            m_Incoming.erase( 0, bodyStart + length );
            Dispatch( body );
        }
    }

    void ScriptDebugServer::Flush()
    {
        while ( m_ClientFd >= 0 && !m_Outgoing.empty() )
        {
            const auto chunk = static_cast<int>( std::min( m_Outgoing.size(), kMaxSendChunk ) );
#if defined( MSG_NOSIGNAL )
            const auto sent = static_cast<std::ptrdiff_t>(
                 ::send( Socket::Raw( m_ClientFd ), m_Outgoing.data(), chunk, MSG_NOSIGNAL ) );
#else
            const auto sent =
                 static_cast<std::ptrdiff_t>( ::send( Socket::Raw( m_ClientFd ), m_Outgoing.data(), chunk, 0 ) );
#endif
            if ( sent > 0 )
            {
                m_Outgoing.erase( 0, static_cast<std::size_t>( sent ) );
                continue;
            }
            if ( sent < 0 && Socket::LastErrorIsWouldBlock() )
                return;
            LOG_WARN( "[ScriptDebug] the debugger client went away with {} bytes unsent", m_Outgoing.size() );
            DropClient();
            return;
        }
    }

    void ScriptDebugServer::DropClient()
    {
        if ( m_ClientFd < 0 )
            return;
        Socket::Close( m_ClientFd );
        m_ClientFd = Socket::kInvalid;
        m_Incoming.clear();
        m_Outgoing.clear();
        // No client, no stops: a script stopped for a client that left resumes now.
        if ( Scripting::ScriptDebugger().Attached() )
            Scripting::ScriptDebugger().Detach();
    }

    void ScriptDebugServer::Send( const std::string& json )
    {
        if ( m_ClientFd < 0 )
            return;
        std::format_to( std::back_inserter( m_Outgoing ), "Content-Length: {}\r\n\r\n{}", json.size(), json );
        Flush();
    }

    template <typename Body>
    void ScriptDebugServer::Respond( int requestSeq, const std::string& command, const Body& body )
    {
        Send( Common::Json::Write( Dap::Response<Body>{ .seq         = m_Seq++,
                                                        .type        = "response",
                                                        .request_seq = requestSeq,
                                                        .success     = true,
                                                        .command     = command,
                                                        .message     = std::nullopt,
                                                        .body        = body } ) );
    }

    void ScriptDebugServer::Refuse( int requestSeq, const std::string& command, const std::string& message )
    {
        LOG_WARN( "[ScriptDebug] {} refused: {}", command, message );
        Send( Common::Json::Write( Dap::Response<Dap::NoBody>{ .seq         = m_Seq++,
                                                               .type        = "response",
                                                               .request_seq = requestSeq,
                                                               .success     = false,
                                                               .command     = command,
                                                               .message     = message,
                                                               .body        = {} } ) );
    }

    template <typename Body>
    void ScriptDebugServer::Emit( const std::string& event, const Body& body )
    {
        Send( Common::Json::Write(
             Dap::Event<Body>{ .seq = m_Seq++, .type = "event", .event = event, .body = body } ) );
    }

    // ---- requests ------------------------------------------------------------------------------------------

    void ScriptDebugServer::Dispatch( const std::string& body )
    {
        const auto document = Common::Json::Parse( body );
        if ( !document.IsSuccess() )
        {
            LOG_WARN( "[ScriptDebug] not a DAP request: {}", document.GetError() );
            return;
        }
        const Common::Json::Node request = Common::Json::Root( document.GetValue() );
        Common::Json::Issues     headerIssues;
        const Dap::RequestHeader header = Dap::ReadHeader( request, headerIssues );
        if ( !headerIssues.empty() || header.type != "request" )
        {
            LOG_WARN( "[ScriptDebug] not a DAP request: {}",
                      headerIssues.empty() ? body : Common::Json::Describe( headerIssues.front() ) );
            return;
        }
        const int                   seq      = header.seq;
        const std::string&          command  = header.command;
        Scripting::IScriptDebugger& debugger = Scripting::ScriptDebugger();

        // A request's arguments read through Node; any Issue refuses the request, naming the member's path.
        Common::Json::Issues issues;
        const auto           refused = [&]() -> bool
        {
            if ( issues.empty() )
                return false;
            Refuse( seq, command, Common::Json::Describe( issues.front() ) );
            return true;
        };

        if ( command == "initialize" )
        {
            Respond( seq, command, Dap::Capabilities{} );
            Emit( "initialized", Dap::NoBody{} );
        }
        else if ( command == "attach" || command == "launch" )
        {
            // `launch` too: the editor is already running, so launching it is attaching to it.
            debugger.Attach( *this );
            Respond( seq, command, Dap::NoBody{} );
        }
        else if ( command == "setBreakpoints" )
        {
            const Dap::SetBreakpointsArguments arguments = Dap::ReadSetBreakpoints( request, issues );
            if ( refused() )
                return;
            Dap::BreakpointsBody reply;
            for ( const Scripting::DebugBreakpoint& breakpoint :
                  debugger.SetBreakpoints( arguments.sourcePath, arguments.lines ) )
                reply.breakpoints.push_back( ToWire( breakpoint ) );
            Respond( seq, command, reply );
        }
        else if ( command == "setExceptionBreakpoints" )
        {
            Respond( seq, command, Dap::BreakpointsBody{} );
        }
        else if ( command == "configurationDone" )
        {
            Respond( seq, command, Dap::NoBody{} );
        }
        else if ( command == "threads" )
        {
            Respond( seq, command,
                     Dap::ThreadsBody{ .threads = { Dap::Thread{ .id = kGameThread, .name = "Game thread" } } } );
        }
        else if ( command == "stackTrace" )
        {
            Dap::StackTraceBody reply;
            for ( const Scripting::DebugFrame& frame : debugger.StackTrace() )
            {
                Dap::StackFrame wire{
                     .id = frame.Id, .name = frame.Name, .source = std::nullopt, .line = frame.Line, .column = 1 };
                if ( !frame.File.empty() )
                    wire.source = Dap::Source{ .name = frame.File.filename().generic_string(),
                                               .path = frame.File.generic_string() };
                reply.stackFrames.push_back( std::move( wire ) );
            }
            reply.totalFrames = static_cast<int>( reply.stackFrames.size() );
            Respond( seq, command, reply );
        }
        else if ( command == "scopes" )
        {
            const int frameId = Dap::ReadIntArgument( request, "frameId", issues );
            if ( refused() )
                return;
            Dap::ScopesBody reply;
            for ( const Scripting::DebugScope& scope : debugger.Scopes( frameId ) )
                reply.scopes.push_back(
                     Dap::Scope{ .name = scope.Name, .variablesReference = scope.Reference, .expensive = false } );
            Respond( seq, command, reply );
        }
        else if ( command == "variables" )
        {
            const int reference = Dap::ReadIntArgument( request, "variablesReference", issues );
            if ( refused() )
                return;
            Dap::VariablesBody reply;
            for ( const Scripting::DebugVariable& variable : debugger.Variables( reference ) )
                reply.variables.push_back( Dap::Variable{ .name               = variable.Name,
                                                          .value              = variable.Value,
                                                          .type               = variable.Type,
                                                          .variablesReference = variable.Reference } );
            Respond( seq, command, reply );
        }
        else if ( command == "continue" )
        {
            Respond( seq, command, Dap::ContinueBody{} );
            debugger.Resume( Scripting::DebugResume::Continue );
        }
        else if ( command == "next" || command == "stepIn" || command == "stepOut" )
        {
            Respond( seq, command, Dap::NoBody{} );
            debugger.Resume( command == "next"     ? Scripting::DebugResume::StepOver
                             : command == "stepIn" ? Scripting::DebugResume::StepIn
                                                   : Scripting::DebugResume::StepOut );
        }
        else if ( command == "pause" )
        {
            Respond( seq, command, Dap::NoBody{} );
            debugger.Pause();
        }
        else if ( command == "terminate" )
        {
            Respond( seq, command, Dap::NoBody{} );
            debugger.Abort();
            m_Play.RequestStop();
            Emit( "terminated", Dap::NoBody{} );
        }
        else if ( command == "disconnect" )
        {
            const bool terminateDebuggee = Dap::ReadTerminateDebuggee( request, issues );
            if ( refused() )
                return;
            Respond( seq, command, Dap::NoBody{} );
            if ( terminateDebuggee )
            {
                debugger.Abort();
                m_Play.RequestStop();
            }
            Flush();
            DropClient();
        }
        else
        {
            Refuse( seq, command,
                    std::format( "'{}' is not supported by the DesertEngine script debugger", command ) );
        }
    }
} // namespace Desert::Editor
