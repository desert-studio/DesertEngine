// BuildPool platform, Linux: the seek-penalty probe (/sys/block rotational), the own executable and identity
// (/proc/self), and SHA-256 from the kernel's crypto API (an AF_ALG "hash" socket).
#include "BuildPool/Platform.hpp"

#include "../Posix/Posix.hpp"

#include <fcntl.h>
#include <linux/if_alg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

namespace buildpool
{
    namespace
    {
        // The first line of a small sysfs or procfs file.
        Result<std::string> ReadLine( const std::filesystem::path& path )
        {
            posix::Fd file( ::open( path.c_str(), O_RDONLY | O_CLOEXEC ) );
            if ( !file.Valid() )
            {
                return Fail( posix::ErrnoText( "open " + path.string(), errno ) );
            }
            char    buffer[4096];
            ssize_t got = 0;
            do
            {
                got = ::read( file.Get(), buffer, sizeof( buffer ) - 1 );
            } while ( got < 0 && errno == EINTR );
            if ( got < 0 )
            {
                return Fail( posix::ErrnoText( "read " + path.string(), errno ) );
            }
            std::string  line( buffer, static_cast<size_t>( got ) );
            const size_t end = line.find( '\n' );
            if ( end != std::string::npos )
            {
                line.resize( end );
            }
            return line;
        }
    } // namespace

    Result<bool> HasSeekPenalty( const std::filesystem::path& path ) noexcept
    {
        struct stat info
        {
        };
        if ( ::stat( path.c_str(), &info ) != 0 )
        {
            return Fail( posix::ErrnoText( "stat " + path.string(), errno ) );
        }
        if ( major( info.st_dev ) == 0 )
        {
            return Fail( path.string() + " is on a volume without a block device (tmpfs, overlay, btrfs "
                                         "subvolume); its disk kind is unknown" );
        }
        const std::string device =
             std::to_string( major( info.st_dev ) ) + ":" + std::to_string( minor( info.st_dev ) );
        std::error_code             ec;
        const std::filesystem::path sysfs =
             std::filesystem::canonical( std::filesystem::path( "/sys/dev/block" ) / device, ec );
        if ( ec )
        {
            return Fail( "resolve /sys/dev/block/" + device + ": " + ec.message() );
        }
        // A whole disk carries queue/ itself; a partition's queue/ is its parent disk's.
        std::filesystem::path rotational = sysfs / "queue" / "rotational";
        if ( !std::filesystem::exists( rotational, ec ) )
        {
            rotational = sysfs.parent_path() / "queue" / "rotational";
        }
        Result<std::string> line = ReadLine( rotational );
        if ( !line )
        {
            return line.error();
        }
        return line.value() == "1";
    }

    Result<std::filesystem::path> SelfExecutable() noexcept
    {
        std::error_code             ec;
        const std::filesystem::path path = std::filesystem::read_symlink( "/proc/self/exe", ec );
        if ( ec )
        {
            return Fail( "read /proc/self/exe: " + ec.message() );
        }
        return path;
    }

    ProcessIdentity SelfProcess() noexcept
    {
        ProcessIdentity identity;
        identity.pid             = static_cast<uint64_t>( getpid() );
        Result<std::string> stat = ReadLine( "/proc/self/stat" );
        if ( !stat )
        {
            return identity;
        }
        // Field 22 (starttime, clock ticks since boot), counted from the ')' that ends the command name.
        const size_t close = stat.value().rfind( ')' );
        if ( close == std::string::npos )
        {
            return identity;
        }
        const char* cursor = stat.value().c_str() + close + 1;
        char*       next   = const_cast<char*>( cursor );
        while ( *next == ' ' )
        {
            ++next;
        }
        if ( *next == '\0' )
        {
            return identity;
        }
        ++next; // field 3, the state letter
        for ( int field = 4; field < 22; ++field )
        {
            std::strtoll( next, &next, 10 );
        }
        identity.startTime = std::strtoull( next, nullptr, 10 );
        return identity;
    }

    // ---- SHA-256
    // ---------------------------------------------------------------------------------------------------
    struct Sha256::Impl
    {
        posix::Fd algorithm; // the bound "hash"/"sha256" transform
        posix::Fd operation; // one hashing operation accepted from it
    };

    namespace
    {
        // The kernel's sha256 is part of every supported kernel; a failure is a broken system, not a data
        // error: no digest can be produced and none is invented. The process stops with the reason.
        [[noreturn]] void KernelHashBroken( const char* what, int error ) noexcept
        {
            std::fprintf( stderr, "buildpool: %s failed: %s (errno %d)\n", what, std::strerror( error ), error );
            std::abort();
        }
    } // namespace

    Sha256::Sha256() noexcept : m_impl( std::make_unique<Impl>() )
    {
        m_impl->algorithm.Reset( ::socket( AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0 ) );
        if ( !m_impl->algorithm.Valid() )
        {
            KernelHashBroken( "socket(AF_ALG)", errno );
        }
        sockaddr_alg address{};
        address.salg_family = AF_ALG;
        std::memcpy( address.salg_type, "hash", sizeof( "hash" ) );
        std::memcpy( address.salg_name, "sha256", sizeof( "sha256" ) );
        if ( ::bind( m_impl->algorithm.Get(), reinterpret_cast<const sockaddr*>( &address ), sizeof( address ) ) !=
             0 )
        {
            KernelHashBroken( "bind(AF_ALG hash sha256)", errno );
        }
        m_impl->operation.Reset( ::accept4( m_impl->algorithm.Get(), nullptr, nullptr, SOCK_CLOEXEC ) );
        if ( !m_impl->operation.Valid() )
        {
            KernelHashBroken( "accept(AF_ALG)", errno );
        }
    }

    Sha256::Sha256( Sha256&& ) noexcept            = default;
    Sha256& Sha256::operator=( Sha256&& ) noexcept = default;
    Sha256::~Sha256()                              = default;

    void Sha256::Update( std::span<const std::byte> bytes ) noexcept
    {
        while ( !bytes.empty() )
        {
            const size_t  chunk = std::min<size_t>( bytes.size(), 1u << 20 );
            const ssize_t sent  = ::send( m_impl->operation.Get(), bytes.data(), chunk, MSG_MORE );
            if ( sent < 0 )
            {
                if ( errno == EINTR )
                {
                    continue;
                }
                KernelHashBroken( "send(AF_ALG)", errno );
            }
            bytes = bytes.subspan( static_cast<size_t>( sent ) );
        }
    }

    void Sha256::Update( std::string_view text ) noexcept
    {
        Update( std::as_bytes( std::span<const char>( text.data(), text.size() ) ) );
    }

    Digest Sha256::Finish() noexcept
    {
        // A send without MSG_MORE ends the message (an empty one is fine), then the digest is read.
        while ( ::send( m_impl->operation.Get(), nullptr, 0, 0 ) < 0 )
        {
            if ( errno != EINTR )
            {
                KernelHashBroken( "send(AF_ALG, final)", errno );
            }
        }
        Digest  digest{};
        ssize_t got = 0;
        do
        {
            got = ::read( m_impl->operation.Get(), digest.data(), digest.size() );
        } while ( got < 0 && errno == EINTR );
        if ( got != static_cast<ssize_t>( digest.size() ) )
        {
            KernelHashBroken( "read(AF_ALG digest)", got < 0 ? errno : EIO );
        }
        return digest;
    }
} // namespace buildpool
