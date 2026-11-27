// BuildPool platform, Windows: machine memory, CPUs, volumes, the process itself, the endpoint name, atomic
// file writes and SHA-256 (BCrypt).
#include "BuildPool/Platform.hpp"

#include "../Internal.hpp"
#include "Win.hpp"

#include <bcrypt.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <system_error>

#pragma comment( lib, "bcrypt.lib" )

namespace buildpool
{
    // ---- machine
    // ---------------------------------------------------------------------------------------------------
    Result<MemoryState> QueryMemory() noexcept
    {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof( status );
        if ( !GlobalMemoryStatusEx( &status ) )
        {
            return Fail( win::LastErrorText( "GlobalMemoryStatusEx" ) );
        }
        // The low-memory notification is the kernel's own threshold (it is signalled while available physical
        // memory is below it); asking it per sample keeps no handle across ticks.
        win::Handle notification( CreateMemoryResourceNotification( LowMemoryResourceNotification ) );
        if ( !notification.Valid() )
        {
            return Fail( win::LastErrorText( "CreateMemoryResourceNotification" ) );
        }
        BOOL low = FALSE;
        if ( !QueryMemoryResourceNotification( notification.Get(), &low ) )
        {
            return Fail( win::LastErrorText( "QueryMemoryResourceNotification" ) );
        }
        MemoryState state;
        state.physicalTotal     = status.ullTotalPhys;
        state.physicalAvailable = status.ullAvailPhys;
        state.commitFree        = status.ullAvailPageFile;
        state.commitLimit       = status.ullTotalPageFile;
        state.osPressure        = low != FALSE;
        return state;
    }

    uint32_t LogicalCpuCount() noexcept
    {
        const DWORD count = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
        return count > 0 ? static_cast<uint32_t>( count ) : 1u;
    }

    Result<bool> HasSeekPenalty( const std::filesystem::path& path ) noexcept
    {
        std::error_code             ec;
        const std::filesystem::path absolute = std::filesystem::absolute( path, ec );
        if ( ec )
        {
            return Fail( "HasSeekPenalty: " + path.string() + ": " + ec.message() );
        }
        wchar_t mount[MAX_PATH + 1] = {};
        if ( !GetVolumePathNameW( absolute.c_str(), mount, MAX_PATH ) )
        {
            return Fail( win::LastErrorText( "GetVolumePathNameW " + absolute.string() ) );
        }
        wchar_t volume[MAX_PATH + 1] = {};
        if ( !GetVolumeNameForVolumeMountPointW( mount, volume, MAX_PATH ) )
        {
            return Fail( win::LastErrorText( "GetVolumeNameForVolumeMountPointW " + win::Narrow( mount ) ) );
        }
        // "\\?\Volume{GUID}\" names the root directory; without the trailing backslash it names the device.
        std::wstring device = volume;
        if ( !device.empty() && device.back() == L'\\' )
        {
            device.pop_back();
        }
        win::Handle handle( CreateFileW( device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, 0, nullptr ) );
        if ( !handle.Valid() )
        {
            return Fail( win::LastErrorText( "open volume " + win::Narrow( device ) ) );
        }
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType  = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
        DWORD                          returned = 0;
        if ( !DeviceIoControl( handle.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof( query ), &descriptor,
                               sizeof( descriptor ), &returned, nullptr ) ||
             returned < sizeof( descriptor ) )
        {
            return Fail(
                 win::LastErrorText( "IOCTL_STORAGE_QUERY_PROPERTY seek penalty " + win::Narrow( device ) ) );
        }
        return descriptor.IncursSeekPenalty != FALSE;
    }

    Result<std::filesystem::path> SelfExecutable() noexcept
    {
        std::wstring buffer( MAX_PATH, L'\0' );
        for ( ;; )
        {
            const DWORD length = GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size() ) );
            if ( length == 0 )
            {
                return Fail( win::LastErrorText( "GetModuleFileNameW" ) );
            }
            if ( length < buffer.size() )
            {
                buffer.resize( length );
                return std::filesystem::path( buffer );
            }
            buffer.resize( buffer.size() * 2 );
        }
    }

    std::optional<std::string> GetEnv( std::string_view name ) noexcept
    {
        const std::wstring wideName = win::Widen( name );
        SetLastError( ERROR_SUCCESS );
        const DWORD size = GetEnvironmentVariableW( wideName.c_str(), nullptr, 0 );
        if ( size == 0 )
        {
            if ( GetLastError() == ERROR_ENVVAR_NOT_FOUND )
            {
                return std::nullopt;
            }
            return std::string();
        }
        std::wstring value( size, L'\0' );
        const DWORD  length = GetEnvironmentVariableW( wideName.c_str(), value.data(), size );
        value.resize( length < size ? length : 0 );
        return win::Narrow( value );
    }

    Status WriteStdout( std::span<const std::byte> bytes ) noexcept
    {
        const HANDLE out = GetStdHandle( STD_OUTPUT_HANDLE );
        if ( out == INVALID_HANDLE_VALUE || out == nullptr )
        {
            return Fail( win::LastErrorText( "write stdout" ) );
        }
        size_t at = 0;
        while ( at < bytes.size() )
        {
            const DWORD chunk = static_cast<DWORD>( std::min<size_t>( bytes.size() - at, 1u << 30 ) );
            DWORD       written = 0;
            if ( !WriteFile( out, bytes.data() + at, chunk, &written, nullptr ) )
            {
                return Fail( win::LastErrorText( "write stdout" ) );
            }
            at += written;
        }
        return Ok();
    }

    ProcessIdentity SelfProcess() noexcept
    {
        ProcessIdentity identity;
        identity.pid = GetCurrentProcessId();
        FILETIME creation{}, exitTime{}, kernel{}, user{};
        if ( GetProcessTimes( GetCurrentProcess(), &creation, &exitTime, &kernel, &user ) )
        {
            identity.startTime =
                 ( static_cast<uint64_t>( creation.dwHighDateTime ) << 32 ) | creation.dwLowDateTime;
        }
        return identity;
    }

    // ---- endpoint
    // --------------------------------------------------------------------------------------------------
    std::string EndpointFor( const std::filesystem::path& home )
    {
        std::error_code       ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical( home, ec );
        if ( ec )
        {
            canonical = std::filesystem::absolute( home, ec );
        }
        std::wstring text = canonical.generic_wstring();
        while ( text.size() > 3 && text.back() == L'/' )
        {
            text.pop_back();
        }
        if ( !text.empty() )
        {
            CharLowerBuffW( text.data(), static_cast<DWORD>( text.size() ) );
        }
        return "\\\\.\\pipe\\buildpool-" + detail::EndpointHash( win::Narrow( text ) );
    }

    // ---- files
    // -----------------------------------------------------------------------------------------------------
    Status WriteFileAtomic( const std::filesystem::path& path, std::string_view bytes ) noexcept
    {
        std::filesystem::path temporary = path;
        temporary +=
             L".tmp-" + std::to_wstring( GetCurrentProcessId() ) + L"-" + std::to_wstring( GetCurrentThreadId() );
        {
            win::Handle file( CreateFileW( temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr ) );
            if ( !file.Valid() )
            {
                return Fail( win::LastErrorText( "create " + temporary.string() ) );
            }
            size_t written = 0;
            while ( written < bytes.size() )
            {
                const DWORD chunk = static_cast<DWORD>( std::min<size_t>( bytes.size() - written, 1u << 30 ) );
                DWORD       done  = 0;
                if ( !WriteFile( file.Get(), bytes.data() + written, chunk, &done, nullptr ) )
                {
                    const std::string reason = win::LastErrorText( "write " + temporary.string() );
                    file.Reset();
                    DeleteFileW( temporary.c_str() );
                    return Fail( reason );
                }
                written += done;
            }
            if ( !FlushFileBuffers( file.Get() ) )
            {
                const std::string reason = win::LastErrorText( "flush " + temporary.string() );
                file.Reset();
                DeleteFileW( temporary.c_str() );
                return Fail( reason );
            }
        }
        // A reader that opened the target without FILE_SHARE_DELETE blocks the rename for the moment it reads;
        // the rename is retried for up to two seconds, then the reason is returned.
        const uint64_t deadline = MonotonicMs() + 2000;
        for ( ;; )
        {
            if ( MoveFileExW( temporary.c_str(), path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
            {
                return Ok();
            }
            const DWORD code = GetLastError();
            const bool  transient =
                 code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION;
            if ( !transient || MonotonicMs() >= deadline )
            {
                DeleteFileW( temporary.c_str() );
                return Fail( win::ErrorText( "rename " + temporary.string() + " -> " + path.string(), code ) );
            }
            Sleep( 20 );
        }
    }

    // ---- SHA-256
    // ---------------------------------------------------------------------------------------------------
    struct Sha256::Impl
    {
        BCRYPT_HASH_HANDLE hash = nullptr;
    };

    namespace
    {
        // A failure of BCrypt's built-in SHA-256 provider is a broken OS, not a data error: no digest can be
        // produced and none is invented. The process stops with the reason.
        [[noreturn]] void BCryptBroken( const char* what, NTSTATUS status ) noexcept
        {
            std::fprintf( stderr, "buildpool: %s failed (NTSTATUS 0x%08lx)\n", what,
                          static_cast<unsigned long>( status ) );
            std::abort();
        }
    } // namespace

    Sha256::Sha256() noexcept : m_impl( std::make_unique<Impl>() )
    {
        const NTSTATUS status =
             BCryptCreateHash( BCRYPT_SHA256_ALG_HANDLE, &m_impl->hash, nullptr, 0, nullptr, 0, 0 );
        if ( !BCRYPT_SUCCESS( status ) )
        {
            BCryptBroken( "BCryptCreateHash(SHA256)", status );
        }
    }

    Sha256::Sha256( Sha256&& ) noexcept            = default;
    Sha256& Sha256::operator=( Sha256&& ) noexcept = default;

    Sha256::~Sha256()
    {
        if ( m_impl && m_impl->hash != nullptr )
        {
            BCryptDestroyHash( m_impl->hash );
        }
    }

    void Sha256::Update( std::span<const std::byte> bytes ) noexcept
    {
        while ( !bytes.empty() )
        {
            const ULONG    chunk  = static_cast<ULONG>( std::min<size_t>( bytes.size(), 1u << 30 ) );
            const NTSTATUS status = BCryptHashData(
                 m_impl->hash, reinterpret_cast<PUCHAR>( const_cast<std::byte*>( bytes.data() ) ), chunk, 0 );
            if ( !BCRYPT_SUCCESS( status ) )
            {
                BCryptBroken( "BCryptHashData", status );
            }
            bytes = bytes.subspan( chunk );
        }
    }

    void Sha256::Update( std::string_view text ) noexcept
    {
        Update( std::as_bytes( std::span<const char>( text.data(), text.size() ) ) );
    }

    Digest Sha256::Finish() noexcept
    {
        Digest         digest{};
        const NTSTATUS status =
             BCryptFinishHash( m_impl->hash, digest.data(), static_cast<ULONG>( digest.size() ), 0 );
        if ( !BCRYPT_SUCCESS( status ) )
        {
            BCryptBroken( "BCryptFinishHash", status );
        }
        BCryptDestroyHash( m_impl->hash );
        m_impl->hash = nullptr;
        return digest;
    }
} // namespace buildpool
