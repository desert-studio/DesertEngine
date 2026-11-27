// BuildPool client internals (package C). The entry is RunClient (BuildPool/Invocation.hpp); nothing outside
// Source/Client includes this file.
//
//   ninja -> buildpool cc (Front) -> cache -> buildpool (Back, BUILDPOOL_STAGE=back) -> compiler
//   ninja -> buildpool link -> linker or archiver
//
// The Front owns a session for the life of one build step; every Back the cache starts joins it. A lease is
// held only while a tool runs, and the connection holding it closing is its release.
#pragma once

#include "BuildPool/Invocation.hpp"
#include "BuildPool/Platform.hpp"
#include "BuildPool/Protocol.hpp"
#include "BuildPool/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace buildpool::client
{
    // Prints "buildpool: <message>" on stderr and returns 1, the exit code of a step the pool refused.
    int Report( std::string_view message ) noexcept;

    // UTF-8 text of a path and back (the strings of command lines and of the wire are UTF-8).
    std::string           PathText( const std::filesystem::path& path );
    std::filesystem::path PathFrom( std::string_view utf8 );

    // A directory as the pool names it: absolute, lexically normal, forward slashes, no trailing slash. Never
    // canonical: <tree>/build/cmake may be a junction to the pool home, and command lines name the tree's path.
    std::string RootText( std::string_view directory );

    // `name` as an absolute path: itself when it names a directory, else the first match on PATH.
    Result<std::filesystem::path> ResolveProgram( std::string_view name ) noexcept;

    // What follows the mode word on the launcher's command line.
    struct Options
    {
        std::filesystem::path                            home;
        TreeRoots                                        roots;
        std::vector<std::string>                         cache;    // program + arguments; empty = no cache
        std::vector<std::pair<std::string, std::string>> cacheEnv; // NAME, VALUE (with {compiler_id})
        std::vector<std::string>                         command;  // everything after "--"
    };

    // ---- Connect.cpp: the daemon of one home, spoken to through one connection
    // -----------------------------------
    class DaemonLink
    {
    public:
        enum class Start : uint8_t
        {
            IfMissing, // no daemon listens: start one detached and connect to it
            Never,     // no daemon listens: an Error
        };

        // Connects, says `hello` and waits for Welcome. A daemon of an older protocol that refuses is waited out
        // (it drains, then exits) and replaced.
        static Result<DaemonLink> Open( std::filesystem::path home, Hello hello, Start start ) noexcept;

        bool Connected() const noexcept
        {
            return m_connection.has_value();
        }
        const Welcome& GetWelcome() const noexcept
        {
            return m_welcome;
        }
        const std::filesystem::path& Home() const noexcept
        {
            return m_home;
        }

        // false: the daemon is gone; the link is disconnected from then on.
        bool Send( const Message& message ) noexcept;
        // One message, or empty after `timeoutMs`. An Error when the daemon is gone (Connected() turns false) or
        // sent bytes that do not decode.
        Result<std::optional<Message>> Receive( uint32_t timeoutMs ) noexcept;
        // After the daemon was lost: connect again (starting a new daemon when allowed), say the same Hello.
        Status Reopen() noexcept;

    private:
        DaemonLink( std::filesystem::path home, Hello hello, Start start ) noexcept;
        Status Connect() noexcept;

        std::filesystem::path     m_home;
        Hello                     m_hello;
        Start                     m_start = Start::IfMissing;
        std::optional<Connection> m_connection;
        Welcome                   m_welcome;
    };

    // ---- Back.cpp
    // --------------------------------------------------------------------------------------------------- Asks for
    // a lease with `acquire`, runs `spec` under it in a ChildJob, reports Heartbeats, obeys Preempt (kill,
    // Released{preempted}, wait for the next Grant, run again) and ends with Released. Returns the tool's exit
    // code, kRetryExitCode when the daemon answered Retry, or 1 after Report. `tee`: the child's stdout is
    // captured, written to this process's stdout and fed to the digest (a preprocess to stdout).
    int RunLeased( DaemonLink& link, const Acquire& acquire, const ChildSpec& spec,
                   PreprocessedDigest* tee ) noexcept;

    // BUILDPOOL_STAGE=back: argv[1..] are the compiler's arguments, the compiler is BUILDPOOL_CC.
    int RunBack( int argc, char** argv ) noexcept;

    // ---- Front.cpp, Link.cpp, Control.cpp
    // ----------------------------------------------------------------------------
    int RunFront( const Options& options ) noexcept;
    int RunLink( const Options& options ) noexcept;
    int RunControl( Query::What what, const std::filesystem::path& home ) noexcept;

    // The Hello of a Front or a link step of the build at `buildRoot` (class and run label from the environment).
    Hello FrontHello( const std::string& buildRoot ) noexcept;
} // namespace buildpool::client
