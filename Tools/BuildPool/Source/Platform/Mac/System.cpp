// BuildPool platform, macOS: the seek-penalty probe, the own executable and identity, and SHA-256 from
// CommonCrypto.
#include "BuildPool/Platform.hpp"

#include "../Posix/Posix.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/proc_info.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <vector>

namespace buildpool
{
    Result<bool> HasSeekPenalty( const std::filesystem::path& ) noexcept
    {
        return false; // the project supports no rotating volume on macOS (Platform.hpp)
    }

    Result<std::filesystem::path> SelfExecutable() noexcept
    {
        uint32_t size = 0;
        _NSGetExecutablePath( nullptr, &size );
        std::vector<char> buffer( size + 1, '\0' );
        if ( _NSGetExecutablePath( buffer.data(), &size ) != 0 )
        {
            return Fail( "_NSGetExecutablePath failed" );
        }
        std::error_code             ec;
        const std::filesystem::path path( buffer.data() );
        std::filesystem::path       canonical = std::filesystem::canonical( path, ec );
        if ( ec )
        {
            return Fail( "canonical path of " + path.string() + ": " + ec.message() );
        }
        return canonical;
    }

    ProcessIdentity SelfProcess() noexcept
    {
        ProcessIdentity identity;
        identity.pid = static_cast<uint64_t>( getpid() );
        proc_bsdinfo info{};
        if ( proc_pidinfo( getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof( info ) ) ==
             static_cast<int>( sizeof( info ) ) )
        {
            identity.startTime = static_cast<uint64_t>( info.pbi_start_tvsec ) * 1000000u + info.pbi_start_tvusec;
        }
        return identity;
    }

    // ---- SHA-256
    // ---------------------------------------------------------------------------------------------------
    struct Sha256::Impl
    {
        CC_SHA256_CTX context{};
    };

    Sha256::Sha256() noexcept : m_impl( std::make_unique<Impl>() )
    {
        CC_SHA256_Init( &m_impl->context );
    }

    Sha256::Sha256( Sha256&& ) noexcept            = default;
    Sha256& Sha256::operator=( Sha256&& ) noexcept = default;
    Sha256::~Sha256()                              = default;

    void Sha256::Update( std::span<const std::byte> bytes ) noexcept
    {
        while ( !bytes.empty() )
        {
            const CC_LONG chunk = static_cast<CC_LONG>( std::min<size_t>( bytes.size(), 1u << 30 ) );
            CC_SHA256_Update( &m_impl->context, bytes.data(), chunk );
            bytes = bytes.subspan( chunk );
        }
    }

    void Sha256::Update( std::string_view text ) noexcept
    {
        Update( std::as_bytes( std::span<const char>( text.data(), text.size() ) ) );
    }

    Digest Sha256::Finish() noexcept
    {
        Digest digest{};
        CC_SHA256_Final( digest.data(), &m_impl->context );
        return digest;
    }
} // namespace buildpool
