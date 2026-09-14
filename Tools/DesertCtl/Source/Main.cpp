// DesertCtl — one request to a running editor, one reply, and the outcome in the exit status.
//
// WHAT THIS REPLACES. The editor used to be driven by command-line flags: `--select`, `--open-panel`,
// `--open-menu`, `--preview-orbit`. Each existed because macOS refuses this machine synthetic input, so a
// click was not available; each could be spent exactly once, at boot; and each new task that needed a
// different window added another flag. Four of them were deleted when the control channel landed, and this
// is what took their place — the same capabilities, reachable at any moment of a session, as many times as
// asked, because they are the COMMAND PALETTE's own entries rather than a list somebody maintains here.
//
// WHY THE EXIT STATUS MATTERS MORE THAN THE OUTPUT. A script that drives the editor has to stop when a
// command was refused. Grepping the reply for "ok" would pass on any payload containing that text, so the
// reply is parsed and the status comes from the response's own field:
//
//   0  the editor did it
//   1  the editor refused, and said why (the reply is on stdout, the reason on stderr)
//   2  this tool could not reach the editor at all — a different thing entirely, and worth its own code
//
// ONE REQUEST PER RUN, deliberately. The editor answers a command only after a frame that already reflects
// it, so a reply arriving IS the synchronisation: `desertctl run ... && desertctl shot-window ...` needs no
// sleep between the two, and a sleep is what every one of these scripts used to be full of.

#include <ToolMain.hpp>

#include <rflcpp/rfl/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined( DESERT_PLATFORM_WINDOWS )
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#endif

namespace
{
    constexpr int kOk       = 0;
    constexpr int kRefused  = 1;
    constexpr int kNoEditor = 2;

    void Usage()
    {
        std::fprintf( stderr,
                      "desertctl --socket <path> [--wait <seconds>] [--subject <who>] <operation>\n"
                      "\n"
                      "  commands                      list every command the editor offers right now\n"
                      "  run <group> <label>           run one of them, addressed as the palette shows it\n"
                      "  properties                    list the subject's properties and their values\n"
                      "  set <name> <v1[,v2,v3,v4]>    write one of them (the drag a mouse would do)\n"
                      "  state [section ...]           read the editor's state as JSON (default: all)\n"
                      "  shot-window <file.png>        capture the WHOLE editor, interface included\n"
                      "  shot-viewport <file.png>      capture the 3D viewport only, no interface\n"
                      "  quit [code]                   end the session with that exit status\n"
                      "\n"
                      "  --wait <seconds>  wait for the socket to appear before connecting; an editor\n"
                      "                    takes a few seconds to boot, and a client that raced it used\n"
                      "                    to look exactly like an editor that never started.\n"
                      "                    Note that CONNECTING is not READINESS: past that, the editor\n"
                      "                    holds a request until it has finished coming up, so a single\n"
                      "                    'commands' or 'run' is already the wait -- no polling loop.\n"
                      "  --subject <who>   whose properties 'properties' and 'set' are about: 'document'\n"
                      "                    (the focused one, the default) or 'viewport' -- the editor's\n"
                      "                    own view, whose Camera.Position and Camera.Direction are how\n"
                      "                    the camera is placed without a capture flag.\n"
                      "\n"
                      "Exit status: 0 the editor did it, 1 it refused (reason on stderr), 2 unreachable.\n" );
    }

    /// JSON string escaping, for the two places a caller's text goes onto the wire (a command label, a
    /// path). Hand-written because this tool builds the request and reflect-cpp's writer wants a value
    /// tree; the escape set is the one JSON requires and no more.
    std::string Escape( const std::string& text )
    {
        std::string out;
        out.reserve( text.size() + 2 );
        for ( const char c : text )
        {
            switch ( c )
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if ( static_cast<unsigned char>( c ) < 0x20 )
                    {
                        char buffer[8];
                        std::snprintf( buffer, sizeof( buffer ), "\\u%04x", c );
                        out += buffer;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    }

    /// `,"subject":"viewport"` — or nothing at all when none was named.
    ///
    /// OMITTED RATHER THAN SPELLED "document", so a request this tool sends without --subject is byte for
    /// byte the request it sent before the field existed. The default lives in ONE place, the editor's
    /// parser, and a client that spelled it out here would be the second copy of it.
    std::string SubjectField( const std::string& subject )
    {
        return subject.empty() ? std::string{} : R"(,"subject":")" + Escape( subject ) + R"(")";
    }

    /// "0.05,0.35,0.95" -> "[0.05,0.35,0.95]", or false having said what was wrong.
    ///
    /// THE COUNT IS PRESERVED, never padded. One number for a float and three for a colour is what the
    /// editor checks against the property's own declaration, and a client that quietly widened "0.2" to
    /// four components would turn a refusal a person can read into a write they did not ask for.
    bool NumberArray( const std::string& text, std::string& out )
    {
        out               = "[";
        std::size_t start = 0;
        for ( ;; )
        {
            const std::size_t comma = text.find( ',', start );
            const std::string piece =
                 ( comma == std::string::npos ) ? text.substr( start ) : text.substr( start, comma - start );

            // Parsed here rather than passed through, so a typo is caught at the client with the text in
            // hand instead of arriving as a JSON error one round trip later. strtod's end pointer is what
            // turns "0.5abc" from a partial read into the mistake it is.
            char*        end   = nullptr;
            const double value = std::strtod( piece.c_str(), &end );
            if ( piece.empty() || end != piece.c_str() + piece.size() )
            {
                std::fprintf( stderr, "desertctl: '%s' is not a number. Write the value as v1[,v2,v3,v4].\n",
                              piece.c_str() );
                return false;
            }

            char buffer[64];
            std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
            if ( out.size() > 1 )
                out += ',';
            out += buffer;

            if ( comma == std::string::npos )
                break;
            start = comma + 1;
        }
        out += ']';
        return true;
    }
} // namespace

#if defined( DESERT_PLATFORM_WINDOWS )

int main( int, char** )
{
    // The editor's own transport refuses on Windows for the same reason, written out at
    // Editor/Source/Editor/Core/Control/ControlSocket.cpp: it is developed and verified where the editor
    // runs, and that is macOS today. Refusing in one line here beats a client that connects to nothing.
    std::fprintf( stderr,
                  "desertctl: the control channel has no transport on Windows yet. The editor refuses "
                  "--control-socket there for the same reason, so there would be nothing to connect to.\n" );
    return kNoEditor;
}

#else

namespace
{
    /// Connect, waiting up to @p waitSeconds for the socket to appear. Returns -1 having said why.
    int Connect( const std::string& path, double waitSeconds )
    {
        sockaddr_un address{};
        if ( path.size() >= sizeof( address.sun_path ) )
        {
            std::fprintf( stderr, "desertctl: socket path is too long (%zu of %zu characters).\n", path.size(),
                          sizeof( address.sun_path ) - 1 );
            return -1;
        }
        address.sun_family = AF_UNIX;
        std::strncpy( address.sun_path, path.c_str(), sizeof( address.sun_path ) - 1 );

        // 50 ms between attempts: fast enough that a ready editor is not kept waiting, slow enough that a
        // thirty-second wait is six hundred syscalls rather than a spin.
        constexpr double kRetrySeconds = 0.05;
        double           waited        = 0.0;
        std::string      lastError     = "no attempt was made";

        for ( ;; )
        {
            const int fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
            if ( fd < 0 )
            {
                std::fprintf( stderr, "desertctl: socket() failed: %s\n", std::strerror( errno ) );
                return -1;
            }

            if ( ::connect( fd, reinterpret_cast<const sockaddr*>( &address ), sizeof( address ) ) == 0 )
                return fd;

            lastError = std::strerror( errno );
            ::close( fd );

            if ( waited >= waitSeconds )
                break;

            timespec pause{};
            pause.tv_sec  = 0;
            pause.tv_nsec = static_cast<long>( kRetrySeconds * 1e9 );
            ::nanosleep( &pause, nullptr );
            waited += kRetrySeconds;
        }

        std::fprintf( stderr,
                      "desertctl: no editor is listening on '%s' (%s). Start one with "
                      "--control-socket '%s', or raise --wait if it is still booting.\n",
                      path.c_str(), lastError.c_str(), path.c_str() );
        return -1;
    }

    bool SendAll( int fd, const std::string& text )
    {
        std::size_t sent = 0;
        while ( sent < text.size() )
        {
#if defined( MSG_NOSIGNAL )
            const ssize_t wrote = ::send( fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL );
#else
            const ssize_t wrote = ::send( fd, text.data() + sent, text.size() - sent, 0 );
#endif
            if ( wrote > 0 )
            {
                sent += static_cast<std::size_t>( wrote );
                continue;
            }
            if ( wrote < 0 && errno == EINTR )
                continue;
            return false;
        }
        return true;
    }

    /// Read until the first newline. BLOCKING, and that is the point: the editor answers a command only
    /// after a frame that already reflects it, so this wait IS the synchronisation the caller wanted.
    bool ReadLine( int fd, std::string& out )
    {
        char buffer[4096];
        for ( ;; )
        {
            const auto newline = out.find( '\n' );
            if ( newline != std::string::npos )
            {
                out.erase( newline );
                return true;
            }

            const ssize_t received = ::recv( fd, buffer, sizeof( buffer ), 0 );
            if ( received > 0 )
            {
                out.append( buffer, static_cast<std::size_t>( received ) );
                continue;
            }
            if ( received < 0 && errno == EINTR )
                continue;

            // Closed with a partial line, or closed with nothing. Either way there is no reply, and
            // saying so beats printing half of one.
            return false;
        }
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    std::string              socketPath;
    std::string              subject;
    double                   waitSeconds = 0.0;
    std::vector<std::string> rest;

    for ( int i = 1; i < argc; ++i )
    {
        const std::string arg = argv[i];
        if ( arg == "--socket" && i + 1 < argc )
        {
            socketPath = argv[++i];
        }
        else if ( arg == "--wait" && i + 1 < argc )
        {
            waitSeconds = std::strtod( argv[++i], nullptr );
        }
        else if ( arg == "--subject" && i + 1 < argc )
        {
            // PASSED THROUGH UNCHECKED, unlike the operation name below. The editor owns the closed set of
            // subjects and refuses an unknown one by name; a second list here would be the copy that falls
            // behind the day a third subject is added, and it would refuse a subject the editor supports.
            subject = argv[++i];
        }
        else if ( arg == "-h" || arg == "--help" )
        {
            Usage();
            return kOk;
        }
        else
        {
            rest.push_back( arg );
        }
    }

    if ( socketPath.empty() || rest.empty() )
    {
        Usage();
        return kNoEditor;
    }

    // Build the request. Every operation the editor knows is spelled here once; an operation this tool
    // does not know is REFUSED rather than sent, so a typo is caught at the client instead of coming back
    // as "that is not an operation this editor knows" one round trip later.
    const std::string& operation = rest[0];
    std::string        request;

    if ( operation == "commands" )
    {
        request = R"({"id":1,"op":"commands"})";
    }
    else if ( operation == "run" )
    {
        if ( rest.size() < 3 )
        {
            std::fprintf( stderr, "desertctl: run needs a group and a label. Ask 'commands' for the pairs.\n" );
            return kNoEditor;
        }
        request = R"({"id":1,"op":"run","group":")" + Escape( rest[1] ) + R"(","label":")" + Escape( rest[2] ) +
                  R"("})";
    }
    else if ( operation == "properties" )
    {
        request = R"({"id":1,"op":"properties")" + SubjectField( subject ) + "}";
    }
    else if ( operation == "set" )
    {
        if ( rest.size() < 3 )
        {
            std::fprintf( stderr,
                          "desertctl: set needs a property and a value. Ask 'properties' for the names and "
                          "how many numbers each one takes.\n" );
            return kNoEditor;
        }
        std::string value;
        if ( !NumberArray( rest[2], value ) )
            return kNoEditor;
        request = R"({"id":1,"op":"set","property":")" + Escape( rest[1] ) + R"(","value":)" + value +
                  SubjectField( subject ) + "}";
    }
    else if ( operation == "state" )
    {
        std::string sections;
        for ( std::size_t i = 1; i < rest.size(); ++i )
            sections += ( sections.empty() ? "\"" : ",\"" ) + Escape( rest[i] ) + "\"";
        request = R"({"id":1,"op":"state","sections":[)" + sections + "]}";
    }
    else if ( operation == "shot-window" || operation == "shot-viewport" )
    {
        if ( rest.size() < 2 )
        {
            std::fprintf( stderr, "desertctl: %s needs a file to write.\n", operation.c_str() );
            return kNoEditor;
        }
        const char* op = ( operation == "shot-window" ) ? "shot.window" : "shot.viewport";
        request        = std::string( R"({"id":1,"op":")" ) + op + R"(","path":")" + Escape( rest[1] ) + R"("})";
    }
    else if ( operation == "quit" )
    {
        const std::string code = ( rest.size() > 1 ) ? rest[1] : "0";
        request                = R"({"id":1,"op":"quit","code":)" + code + "}";
    }
    else
    {
        std::fprintf( stderr, "desertctl: '%s' is not an operation.\n\n", operation.c_str() );
        Usage();
        return kNoEditor;
    }

    const int fd = Connect( socketPath, waitSeconds );
    if ( fd < 0 )
        return kNoEditor;

    if ( !SendAll( fd, request + "\n" ) )
    {
        std::fprintf( stderr, "desertctl: the editor closed the connection before the request went out.\n" );
        ::close( fd );
        return kNoEditor;
    }

    std::string reply;
    const bool  got = ReadLine( fd, reply );
    ::close( fd );

    if ( !got )
    {
        std::fprintf( stderr,
                      "desertctl: the editor closed the connection without answering. It may have crashed, "
                      "or been closed while the request was in flight.\n" );
        return kNoEditor;
    }

    std::printf( "%s\n", reply.c_str() );

    // THE STATUS COMES FROM THE PARSED REPLY, not from a substring of it. A payload containing the text
    // "ok":true would fool a grep, and a script that trusted the grep would carry on past a refusal —
    // which is exactly the failure this whole channel is built to make impossible.
    const auto parsed = rfl::json::read<rfl::Generic>( reply );
    if ( !parsed )
    {
        std::fprintf( stderr, "desertctl: the editor's reply is not readable JSON.\n" );
        return kNoEditor;
    }

    const auto object = parsed.value().to_object();
    if ( !object )
    {
        std::fprintf( stderr, "desertctl: the editor's reply is JSON but not an object.\n" );
        return kNoEditor;
    }

    const auto ok = object.value().get( "ok" );
    if ( !ok )
    {
        // A reply with no outcome is a defect in the editor's channel, not a refusal — and it must not be
        // reported as success, which is what a missing field would default to anywhere else.
        std::fprintf( stderr, "desertctl: the reply carries no outcome; that is a defect in the channel.\n" );
        return kNoEditor;
    }

    const auto succeeded = ok.value().to_bool();
    if ( succeeded && succeeded.value() )
        return kOk;

    if ( const auto error = object.value().get( "error" ) )
    {
        if ( const auto reason = error.value().to_string() )
            std::fprintf( stderr, "desertctl: refused: %s\n", reason.value().c_str() );
    }
    return kRefused;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "DesertCtl", argc, argv, &RunTool );
}

#endif // DESERT_PLATFORM_WINDOWS
