// BuildPool platform, POSIX: errno text, CPUs, the environment, the endpoint name and atomic file writes.
#include "BuildPool/Platform.hpp"

#include "../Internal.hpp"
#include "Posix.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace buildpool
{
    namespace posix
    {
        std::string ErrnoText( std::string_view what, int error )
        {
            std::string text( what );
            text += ": ";
            text += std::strerror( error );
            text += " (errno " + std::to_string( error ) + ")";
            return text;
        }
    } // namespace posix

    uint32_t LogicalCpuCount() noexcept
    {
        const long count = sysconf( _SC_NPROCESSORS_ONLN );
        return count > 0 ? static_cast<uint32_t>( count ) : 1u;
    }

    std::optional<std::string> GetEnv( std::string_view name ) noexcept
    {
        const std::string key( name );
        const char*       value = std::getenv( key.c_str() );
        if ( value == nullptr )
        {
            return std::nullopt;
        }
        return std::string( value );
    }

    std::string EndpointFor( const std::filesystem::path& home )
    {
        std::error_code       ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical( home, ec );
        if ( ec )
        {
            canonical = std::filesystem::absolute( home, ec );
        }
        std::string text = canonical.generic_string();
        while ( text.size() > 1 && text.back() == '/' )
        {
            text.pop_back();
        }
        for ( char& c : text )
        {
            c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        }
        return "/tmp/buildpool-" + std::to_string( static_cast<unsigned long long>( getuid() ) ) + "-" +
               detail::EndpointHash( text ) + ".sock";
    }

    Status WriteFileAtomic( const std::filesystem::path& path, std::string_view bytes ) noexcept
    {
        std::filesystem::path temporary = path;
        temporary += ".tmp-" + std::to_string( static_cast<long long>( getpid() ) );
        posix::Fd file( ::open( temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 ) );
        if ( !file.Valid() )
        {
            return Fail( posix::ErrnoText( "create " + temporary.string(), errno ) );
        }
        size_t written = 0;
        while ( written < bytes.size() )
        {
            const ssize_t done = ::write( file.Get(), bytes.data() + written, bytes.size() - written );
            if ( done < 0 )
            {
                if ( errno == EINTR )
                {
                    continue;
                }
                const std::string reason = posix::ErrnoText( "write " + temporary.string(), errno );
                file.Reset();
                ::unlink( temporary.c_str() );
                return Fail( reason );
            }
            written += static_cast<size_t>( done );
        }
        if ( ::fsync( file.Get() ) != 0 )
        {
            const std::string reason = posix::ErrnoText( "fsync " + temporary.string(), errno );
            file.Reset();
            ::unlink( temporary.c_str() );
            return Fail( reason );
        }
        file.Reset();
        if ( ::rename( temporary.c_str(), path.c_str() ) != 0 )
        {
            const std::string reason =
                 posix::ErrnoText( "rename " + temporary.string() + " -> " + path.string(), errno );
            ::unlink( temporary.c_str() );
            return Fail( reason );
        }
        return Ok();
    }
} // namespace buildpool
