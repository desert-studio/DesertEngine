// BuildPool platform, POSIX: ChildJob on a process group and SpawnDetached.
#include "BuildPool/Platform.hpp"

#include "Posix.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <map>

extern char** environ;

namespace buildpool
{
    namespace
    {
        int NiceOf( OsPriority priority ) noexcept
        {
            switch ( priority )
            {
                case OsPriority::BelowNormal:
                    return 10;
                case OsPriority::Idle:
                    return 19;
                case OsPriority::Normal:
                    break;
            }
            return 0;
        }

        void SleepMs( uint32_t ms ) noexcept
        {
            timespec request{ static_cast<time_t>( ms / 1000 ), static_cast<long>( ms % 1000 ) * 1000000L };
            while ( nanosleep( &request, &request ) != 0 && errno == EINTR )
            {
            }
        }

        int ExitCodeOf( int status ) noexcept
        {
            if ( WIFEXITED( status ) )
            {
                return WEXITSTATUS( status );
            }
            if ( WIFSIGNALED( status ) )
            {
                return 128 + WTERMSIG( status ); // the shell's convention
            }
            return 1;
        }

        // "KEY=VALUE" strings of the inherited environment with `overrides` applied.
        std::vector<std::string> EnvironmentOf( const std::vector<std::pair<std::string, std::string>>& overrides )
        {
            std::map<std::string, std::string> variables;
            for ( char** entry = environ; entry != nullptr && *entry != nullptr; ++entry )
            {
                const std::string_view text( *entry );
                const size_t           equals = text.find( '=' );
                if ( equals == std::string_view::npos || equals == 0 )
                {
                    continue;
                }
                variables[std::string( text.substr( 0, equals ) )] = std::string( text.substr( equals + 1 ) );
            }
            for ( const auto& [name, value] : overrides )
            {
                if ( value.empty() )
                {
                    variables.erase( name );
                }
                else
                {
                    variables[name] = value;
                }
            }
            std::vector<std::string> result;
            result.reserve( variables.size() );
            for ( const auto& [name, value] : variables )
            {
                result.push_back( name + "=" + value );
            }
            return result;
        }

        std::vector<char*> PointersTo( std::vector<std::string>& strings )
        {
            std::vector<char*> pointers;
            pointers.reserve( strings.size() + 1 );
            for ( std::string& text : strings )
            {
                pointers.push_back( text.data() );
            }
            pointers.push_back( nullptr );
            return pointers;
        }
    } // namespace

    struct ChildJob::Impl
    {
        pid_t            pid = -1; // also the process group id
        posix::Fd        stdoutRead;
        bool             reaped     = false;
        int              exitCode   = 0;
        bool             terminated = false;
        int              reapError  = 0; // errno of the last failed wait4
        uint64_t         reapedPeak = 0; // ru_maxrss of the direct child and what it waited for
        uint64_t         reapedCpu  = 0;
        mutable uint64_t peakSeen   = 0;
        mutable uint64_t cpuSeen    = 0;

        // Reaps the direct child if it has exited; `block` waits for it.
        bool TryReap( bool block ) noexcept
        {
            if ( reaped )
            {
                return true;
            }
            int status = 0;
            struct rusage usage
            {
            };
            pid_t got = 0;
            do
            {
                got = wait4( pid, &status, block ? 0 : WNOHANG, &usage );
            } while ( got < 0 && errno == EINTR );
            reapError = got < 0 ? errno : 0;
            if ( got != pid )
            {
                return false;
            }
            reaped     = true;
            exitCode   = ExitCodeOf( status );
            reapedPeak = posix::MaxRssBytes( usage );
            reapedCpu  = static_cast<uint64_t>( usage.ru_utime.tv_sec + usage.ru_stime.tv_sec ) * 1000u +
                        static_cast<uint64_t>( usage.ru_utime.tv_usec + usage.ru_stime.tv_usec ) / 1000u;
            return true;
        }
    };

    ChildJob::ChildJob( std::unique_ptr<Impl> impl ) noexcept : m_impl( std::move( impl ) )
    {
    }
    ChildJob::ChildJob( ChildJob&& ) noexcept = default;

    ChildJob& ChildJob::operator=( ChildJob&& other ) noexcept
    {
        if ( this != &other )
        {
            {
                ChildJob retired( std::move( *this ) ); // the destructor's rules apply to the job being replaced
            }
            m_impl = std::move( other.m_impl );
        }
        return *this;
    }

    ChildJob::~ChildJob()
    {
        if ( !m_impl )
        {
            return;
        }
        if ( !m_impl->TryReap( false ) && !m_impl->terminated )
        {
            Terminate(); // still running: the RAII contract kills the whole tree
        }
        // The direct child exited by itself: a process it left behind on purpose keeps running.
    }

    Result<ChildJob> ChildJob::Spawn( const ChildSpec& spec ) noexcept
    {
        if ( spec.argv.empty() )
        {
            return Fail( "ChildJob::Spawn: empty argv" );
        }
        auto                     impl        = std::make_unique<Impl>();
        std::vector<std::string> argvStrings = spec.argv;
        std::vector<std::string> envStrings  = EnvironmentOf( spec.environment );
        std::vector<char*>       argv        = PointersTo( argvStrings );
        std::vector<char*>       envp        = PointersTo( envStrings );

        posix::Fd stdoutWrite;
        if ( spec.captureStdout )
        {
            int ends[2] = { -1, -1 };
            if ( ::pipe( ends ) != 0 )
            {
                return Fail( posix::ErrnoText( "pipe", errno ) );
            }
            impl->stdoutRead.Reset( ends[0] );
            stdoutWrite.Reset( ends[1] );
            fcntl( ends[0], F_SETFD, FD_CLOEXEC );
            fcntl( ends[1], F_SETFD, FD_CLOEXEC );
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init( &actions );
        if ( spec.captureStdout )
        {
            posix_spawn_file_actions_adddup2( &actions, stdoutWrite.Get(), STDOUT_FILENO );
        }
        posix_spawnattr_t attributes;
        posix_spawnattr_init( &attributes );
        // Its own process group (measured and killed as one), SIGPIPE back to its default whatever this process
        // did.
        sigset_t defaults;
        sigemptyset( &defaults );
        sigaddset( &defaults, SIGPIPE );
        posix_spawnattr_setsigdefault( &attributes, &defaults );
        posix_spawnattr_setpgroup( &attributes, 0 );
        posix_spawnattr_setflags( &attributes,
                                  static_cast<short>( POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF ) );

        pid_t     pid   = -1;
        const int error = posix_spawnp( &pid, argv[0], &actions, &attributes, argv.data(), envp.data() );
        posix_spawn_file_actions_destroy( &actions );
        posix_spawnattr_destroy( &attributes );
        stdoutWrite.Reset(); // the child holds the only write end now: its exit ends the stream
        if ( error != 0 )
        {
            return Fail( posix::ErrnoText( "posix_spawnp " + spec.argv[0], error ) );
        }
        impl->pid = pid;
        if ( const int nice = NiceOf( spec.priority ); nice != 0 )
        {
            // Applied to the whole group; processes it starts later inherit it.
            setpriority( PRIO_PGRP, static_cast<id_t>( pid ), nice );
        }
        return ChildJob( std::move( impl ) );
    }

    Result<std::optional<int>> ChildJob::Wait( uint32_t timeoutMs ) noexcept
    {
        const uint64_t deadline = MonotonicMs() + timeoutMs;
        uint32_t       slice    = 1;
        for ( ;; )
        {
            if ( m_impl->TryReap( false ) )
            {
                return std::optional<int>( m_impl->exitCode );
            }
            if ( m_impl->reapError != 0 )
            {
                return Fail(
                     posix::ErrnoText( "wait4 child " + std::to_string( m_impl->pid ), m_impl->reapError ) );
            }
            const uint64_t now = MonotonicMs();
            if ( now >= deadline )
            {
                return std::optional<int>();
            }
            SleepMs( static_cast<uint32_t>( std::min<uint64_t>( slice, deadline - now ) ) );
            slice = std::min<uint32_t>( slice * 2, 20 );
        }
    }

    Result<std::optional<size_t>> ChildJob::ReadStdout( std::span<std::byte> into, uint32_t timeoutMs ) noexcept
    {
        if ( !m_impl->stdoutRead.Valid() )
        {
            return Fail( "ChildJob::ReadStdout: stdout was not captured" );
        }
        if ( into.empty() )
        {
            return std::optional<size_t>( 0 );
        }
        const uint64_t deadline   = MonotonicMs() + timeoutMs;
        uint64_t       quietSince = 0; // when the child was first seen gone with an empty pipe
        for ( ;; )
        {
            const uint64_t now = MonotonicMs();
            const int slice    = static_cast<int>( std::min<uint64_t>( deadline > now ? deadline - now : 0, 50 ) );
            pollfd    entry{ m_impl->stdoutRead.Get(), POLLIN, 0 };
            const int ready = ::poll( &entry, 1, slice );
            if ( ready < 0 && errno != EINTR )
            {
                return Fail( posix::ErrnoText( "poll child stdout", errno ) );
            }
            if ( ready > 0 )
            {
                const ssize_t got = ::read( entry.fd, into.data(), into.size() );
                if ( got < 0 )
                {
                    if ( errno == EINTR || errno == EAGAIN )
                    {
                        continue;
                    }
                    return Fail( posix::ErrnoText( "read child stdout", errno ) );
                }
                return std::optional<size_t>( static_cast<size_t>( got ) );
            }
            // Nothing to read. When the child has exited, the stream is drained after a short grace even if a
            // process it left behind still holds the write end.
            if ( m_impl->TryReap( false ) )
            {
                const uint64_t seen = MonotonicMs();
                if ( quietSince == 0 )
                {
                    quietSince = seen;
                }
                else if ( seen - quietSince >= 50 )
                {
                    return std::optional<size_t>( 0 );
                }
            }
            if ( MonotonicMs() >= deadline )
            {
                return std::optional<size_t>();
            }
        }
    }

    JobUsage ChildJob::Usage() const noexcept
    {
        JobUsage usage;
        if ( !m_impl )
        {
            return usage;
        }
        const posix::GroupSample sample = posix::SampleGroup( m_impl->pid );
        usage.currentBytes              = sample.currentBytes;
        m_impl->peakSeen =
             std::max( { m_impl->peakSeen, sample.currentBytes, sample.peakBytes, m_impl->reapedPeak } );
        m_impl->cpuSeen = std::max( { m_impl->cpuSeen, sample.cpuMs, m_impl->reapedCpu } );
        usage.peakBytes = m_impl->peakSeen;
        usage.cpuMs     = m_impl->cpuSeen;
        return usage;
    }

    void ChildJob::Terminate() noexcept
    {
        if ( !m_impl || m_impl->terminated )
        {
            return;
        }
        m_impl->terminated = true;
        // Before the direct child is reaped its pid names the group for certain; afterwards only while the group
        // still has members (a group id is not reused while the group exists).
        if ( !m_impl->reaped || posix::SampleGroup( m_impl->pid ).members > 0 )
        {
            killpg( m_impl->pid, SIGKILL );
        }
        m_impl->TryReap( true );
        for ( int i = 0; i < 100 && posix::SampleGroup( m_impl->pid ).members > 0; ++i )
        {
            SleepMs( 10 ); // members reparented to init are reaped by it
        }
    }

    // ---- SpawnDetached
    // ---------------------------------------------------------------------------------------------
    Status SpawnDetached( const std::vector<std::string>&                         argv,
                          const std::vector<std::pair<std::string, std::string>>& environment,
                          const std::filesystem::path&                            logFile ) noexcept
    {
        if ( argv.empty() )
        {
            return Fail( "SpawnDetached: empty argv" );
        }
        std::error_code ec;
        std::filesystem::create_directories( logFile.parent_path(), ec );
        posix::Fd log( ::open( logFile.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644 ) );
        if ( !log.Valid() )
        {
            return Fail( posix::ErrnoText( "open " + logFile.string(), errno ) );
        }
        posix::Fd null( ::open( "/dev/null", O_RDONLY | O_CLOEXEC ) );
        if ( !null.Valid() )
        {
            return Fail( posix::ErrnoText( "open /dev/null", errno ) );
        }
        // Everything the forked side uses is prepared here: after fork only async-signal-safe calls are made.
        std::vector<std::string> strings    = argv;
        std::vector<char*>       pointers   = PointersTo( strings );
        std::vector<std::string> envStrings = EnvironmentOf( environment );
        std::vector<char*>       envp       = PointersTo( envStrings );
        struct rlimit limit
        {
        };
        int highestFd = 1024;
        if ( getrlimit( RLIMIT_NOFILE, &limit ) == 0 && limit.rlim_cur != RLIM_INFINITY )
        {
            highestFd = static_cast<int>( std::min<rlim_t>( limit.rlim_cur, 65536 ) );
        }
        static constexpr char kExecFailed[] = "buildpool: exec of the daemon failed, errno ";

        const pid_t first = fork();
        if ( first < 0 )
        {
            return Fail( posix::ErrnoText( "fork", errno ) );
        }
        if ( first == 0 )
        {
            // A new session (no controlling terminal, out of the build's process group), then a second fork so
            // the daemon is not a session leader and is reparented to init at once.
            setsid();
            const pid_t second = fork();
            if ( second != 0 )
            {
                _exit( second < 0 ? 1 : 0 );
            }
            dup2( null.Get(), STDIN_FILENO );
            dup2( log.Get(), STDOUT_FILENO );
            dup2( log.Get(), STDERR_FILENO );
            for ( int fd = 3; fd < highestFd; ++fd )
            {
                close( fd ); // nothing of the build (its output pipe above all) may stay open in the daemon
            }
            // macOS has no execvpe: the prepared block becomes this process's environment (a pointer store),
            // then execvp searches PATH and passes `environ` on.
            environ = envp.data();
            execvp( pointers[0], pointers.data() );
            // Only async-signal-safe calls here: the errno is written as decimal digits by hand.
            char digits[16];
            int  length = 0;
            for ( int value = errno; length == 0 || value > 0; value /= 10 )
            {
                digits[length++] = static_cast<char>( '0' + value % 10 );
            }
            std::reverse( digits, digits + length );
            digits[length++]              = '\n';
            [[maybe_unused]] ssize_t sink = write( STDERR_FILENO, kExecFailed, sizeof( kExecFailed ) - 1 );
            sink                          = write( STDERR_FILENO, digits, static_cast<size_t>( length ) );
            _exit( 127 );
        }
        int status = 0;
        while ( waitpid( first, &status, 0 ) < 0 && errno == EINTR )
        {
        }
        if ( !WIFEXITED( status ) || WEXITSTATUS( status ) != 0 )
        {
            return Fail( "SpawnDetached: the intermediate process failed to fork the daemon" );
        }
        return Ok();
    }
} // namespace buildpool
