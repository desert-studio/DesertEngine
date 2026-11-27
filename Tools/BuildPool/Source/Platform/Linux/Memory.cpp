// BuildPool platform, Linux: the machine's memory (/proc/meminfo, vm.overcommit_memory, PSI) and what one process
// group uses (/proc/<pid>/stat and status).
#include "BuildPool/Platform.hpp"

#include "../Posix/Posix.hpp"

#include <dirent.h>
#include <fcntl.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace buildpool
{
    namespace
    {
        // The whole of a small /proc file; empty when it cannot be read.
        std::optional<std::string> ReadProcFile( const char* path ) noexcept
        {
            posix::Fd file( ::open( path, O_RDONLY | O_CLOEXEC ) );
            if ( !file.Valid() )
            {
                return std::nullopt;
            }
            std::string text;
            char        buffer[4096];
            for ( ;; )
            {
                const ssize_t got = ::read( file.Get(), buffer, sizeof( buffer ) );
                if ( got < 0 && errno == EINTR )
                {
                    continue;
                }
                if ( got < 0 )
                {
                    return std::nullopt;
                }
                if ( got == 0 )
                {
                    return text;
                }
                text.append( buffer, static_cast<size_t>( got ) );
            }
        }

        // "<key>:   <n> kB" in /proc/meminfo or /proc/<pid>/status, in bytes.
        std::optional<uint64_t> KilobyteField( std::string_view text, std::string_view key ) noexcept
        {
            size_t at = 0;
            while ( at < text.size() )
            {
                const size_t     end  = text.find( '\n', at );
                std::string_view line = text.substr( at, end == std::string_view::npos ? text.npos : end - at );
                if ( line.size() > key.size() && line.substr( 0, key.size() ) == key && line[key.size()] == ':' )
                {
                    const std::string digits( line.substr( key.size() + 1 ) );
                    return std::strtoull( digits.c_str(), nullptr, 10 ) * 1024u;
                }
                if ( end == std::string_view::npos )
                {
                    break;
                }
                at = end + 1;
            }
            return std::nullopt;
        }

        // PSI "some avg10=<x> ..." of /proc/pressure/memory, the share of the last 10 s some task stalled on
        // memory.
        constexpr double kPressureSomeAvg10 = 10.0;
    } // namespace

    Result<MemoryState> QueryMemory() noexcept
    {
        const std::optional<std::string> meminfo = ReadProcFile( "/proc/meminfo" );
        if ( !meminfo )
        {
            return Fail( posix::ErrnoText( "read /proc/meminfo", errno ) );
        }
        const std::optional<uint64_t> total     = KilobyteField( *meminfo, "MemTotal" );
        const std::optional<uint64_t> available = KilobyteField( *meminfo, "MemAvailable" );
        if ( !total || !available )
        {
            return Fail( "/proc/meminfo has no MemTotal or MemAvailable (kernel older than 3.14)" );
        }
        MemoryState state;
        state.physicalTotal     = *total;
        state.physicalAvailable = *available;

        const std::optional<std::string> overcommit = ReadProcFile( "/proc/sys/vm/overcommit_memory" );
        if ( !overcommit )
        {
            return Fail( posix::ErrnoText( "read /proc/sys/vm/overcommit_memory", errno ) );
        }
        if ( std::atoi( overcommit->c_str() ) == 2 )
        {
            const std::optional<uint64_t> limit     = KilobyteField( *meminfo, "CommitLimit" );
            const std::optional<uint64_t> committed = KilobyteField( *meminfo, "Committed_AS" );
            if ( !limit || !committed )
            {
                return Fail( "/proc/meminfo has no CommitLimit or Committed_AS" );
            }
            state.commitFree = *limit > *committed ? *limit - *committed : 0;
        }

        // A kernel without PSI (CONFIG_PSI off) has no pressure file: the kernel then raises no alarm.
        if ( const std::optional<std::string> psi = ReadProcFile( "/proc/pressure/memory" ) )
        {
            const size_t at = psi->find( "some avg10=" );
            if ( at == std::string::npos )
            {
                return Fail( "/proc/pressure/memory has no \"some avg10=\" field" );
            }
            state.osPressure = std::strtod( psi->c_str() + at + 11, nullptr ) > kPressureSomeAvg10;
        }
        return state;
    }

    namespace posix
    {
        GroupSample SampleGroup( pid_t group ) noexcept
        {
            GroupSample sample;
            DIR*        proc = ::opendir( "/proc" );
            if ( proc == nullptr )
            {
                return sample;
            }
            const long ticksPerSecond = sysconf( _SC_CLK_TCK );
            while ( const dirent* entry = ::readdir( proc ) )
            {
                if ( entry->d_name[0] < '1' || entry->d_name[0] > '9' )
                {
                    continue;
                }
                const std::string                base = std::string( "/proc/" ) + entry->d_name;
                const std::optional<std::string> stat = ReadProcFile( ( base + "/stat" ).c_str() );
                if ( !stat )
                {
                    continue; // exited between the listing and the read
                }
                // "<pid> (<comm>) <state> <ppid> <pgrp> ... <utime(14)> <stime(15)>": comm may hold spaces and
                // parentheses, so the fields are counted from the last ')'.
                const size_t close = stat->rfind( ')' );
                if ( close == std::string::npos )
                {
                    continue;
                }
                const char* cursor = stat->c_str() + close + 1;
                char*       next   = nullptr;
                // field 3 (state) is one character
                while ( *cursor == ' ' )
                {
                    ++cursor;
                }
                if ( *cursor == '\0' )
                {
                    continue;
                }
                ++cursor;
                std::strtoll( cursor, &next, 10 );                      // 4 ppid
                const long long pgrp = std::strtoll( next, &next, 10 ); // 5 pgrp
                if ( pgrp != group )
                {
                    continue;
                }
                for ( int field = 6; field < 14; ++field )
                {
                    std::strtoll( next, &next, 10 );
                }
                const unsigned long long utime = std::strtoull( next, &next, 10 ); // 14
                const unsigned long long stime = std::strtoull( next, &next, 10 ); // 15

                const std::optional<std::string> status = ReadProcFile( ( base + "/status" ).c_str() );
                if ( status )
                {
                    sample.currentBytes += KilobyteField( *status, "VmRSS" ).value_or( 0 ) +
                                           KilobyteField( *status, "VmSwap" ).value_or( 0 );
                }
                if ( ticksPerSecond > 0 )
                {
                    sample.cpuMs += ( utime + stime ) * 1000u / static_cast<uint64_t>( ticksPerSecond );
                }
                ++sample.members;
            }
            ::closedir( proc );
            sample.peakBytes = sample.currentBytes; // Linux keeps no per-process lifetime peak of RSS + swap
            return sample;
        }

        uint64_t MaxRssBytes( const struct rusage& usage ) noexcept
        {
            return static_cast<uint64_t>( usage.ru_maxrss ) * 1024u; // kilobytes on Linux
        }
    } // namespace posix
} // namespace buildpool
