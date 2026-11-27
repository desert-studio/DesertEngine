// BuildPool platform, POSIX (macOS and Linux): file descriptors, errno text, and the per-kernel probes that
// Mac/ and Linux/ implement for the shared Posix/ code.
#pragma once

#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace buildpool::posix
{
    // "<what>: <strerror> (errno <n>)".
    std::string ErrnoText( std::string_view what, int error );

    // Owns one file descriptor; -1 means none.
    class Fd
    {
    public:
        Fd() noexcept = default;
        explicit Fd( int fd ) noexcept : m_fd( fd )
        {
        }
        Fd( Fd&& other ) noexcept : m_fd( other.Release() )
        {
        }
        Fd& operator=( Fd&& other ) noexcept
        {
            if ( this != &other )
            {
                Reset( other.Release() );
            }
            return *this;
        }
        Fd( const Fd& )            = delete;
        Fd& operator=( const Fd& ) = delete;
        ~Fd()
        {
            Reset();
        }

        int Get() const noexcept
        {
            return m_fd;
        }
        bool Valid() const noexcept
        {
            return m_fd >= 0;
        }
        int Release() noexcept
        {
            const int fd = m_fd;
            m_fd         = -1;
            return fd;
        }
        void Reset( int fd = -1 ) noexcept
        {
            if ( m_fd >= 0 )
            {
                ::close( m_fd );
            }
            m_fd = fd;
        }

    private:
        int m_fd = -1;
    };

    // What the processes of one process group use right now (Mac/Memory.cpp, Linux/Memory.cpp).
    struct GroupSample
    {
        uint64_t currentBytes = 0; // macOS: sum of phys_footprint; Linux: sum of VmRSS + VmSwap
        uint64_t peakBytes    = 0; // the largest lifetime peak of any one member (macOS), else currentBytes
        uint64_t cpuMs        = 0; // user + system of the live members
        size_t   members      = 0;
    };
    GroupSample SampleGroup( pid_t group ) noexcept;

    // ru_maxrss in bytes (macOS reports bytes, Linux kilobytes).
    uint64_t MaxRssBytes( const struct rusage& usage ) noexcept;
} // namespace buildpool::posix
