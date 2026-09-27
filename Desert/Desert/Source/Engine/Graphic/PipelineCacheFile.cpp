#include <Engine/Graphic/PipelineCacheFile.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <cstring>
#include <format>

namespace Desert::Graphic::PipelineCacheFile
{
    namespace
    {
        constexpr std::array<char, 4> kMagic{ 'D', 'P', 'S', 'C' };
        // Bump when the header layout changes; an old file is then refused with a reason and rebuilt.
        constexpr uint32_t kFormatVersion = 1;

        struct Header
        {
            std::array<char, 4> Magic;
            uint32_t            Version;
            DeviceIdentity      Identity;
            uint64_t            PayloadSize;
            uint64_t            PayloadHash;
        };
        static_assert( sizeof( DeviceIdentity ) == 3 * sizeof( uint32_t ) + 16,
                       "identity is hashed/stored as bytes" );

        std::string Hex( const uint8_t* bytes, const size_t count )
        {
            std::string out;
            out.reserve( count * 2 );
            for ( size_t i = 0; i < count; ++i )
                out += std::format( "{:02x}", bytes[i] );
            return out;
        }
    } // namespace

    namespace
    {
        std::optional<Host> s_Host;
    } // namespace

    void DeclareHost( const Host host )
    {
        s_Host = host;
    }

    std::optional<Host> DeclaredHost()
    {
        return s_Host;
    }

    std::filesystem::path Directory( const Host host, const std::filesystem::path& userDir,
                                     const std::string_view projectName )
    {
        const std::filesystem::path root = userDir / "PipelineCache";
        if ( host == Host::Game )
            return root;
        return root / Common::Settings::UserFolderName( projectName );
    }

    std::string FileName( const DeviceIdentity& identity )
    {
        return std::format( "{:08x}-{:08x}-{:08x}-{}.bin", identity.Vendor, identity.Device, identity.Driver,
                            Hex( identity.CacheUuid.data(), identity.CacheUuid.size() ) );
    }

    uint64_t HashBytes( const std::string_view bytes )
    {
        uint64_t h = 1469598103934665603ull; // FNV-1a
        for ( const char c : bytes )
        {
            h ^= static_cast<uint8_t>( c );
            h *= 1099511628211ull;
        }
        return h;
    }

    std::string Encode( const DeviceIdentity& identity, const std::string_view driverBlob )
    {
        const Header header{ kMagic, kFormatVersion, identity, driverBlob.size(), HashBytes( driverBlob ) };
        std::string  out( sizeof header, '\0' );
        std::memcpy( out.data(), &header, sizeof header );
        out.append( driverBlob );
        return out;
    }

    Decoded Decode( const DeviceIdentity& identity, const std::string_view file )
    {
        Header header{};
        if ( file.size() < sizeof header )
            return { {},
                     std::format( "{} bytes is shorter than the {}-byte header", file.size(), sizeof header ) };
        std::memcpy( &header, file.data(), sizeof header );
        if ( header.Magic != kMagic )
            return { {}, "not a pipeline cache file (magic mismatch)" };
        if ( header.Version != kFormatVersion )
            return { {},
                     std::format( "format version {} (this build reads {})", header.Version, kFormatVersion ) };
        if ( !( header.Identity == identity ) )
            return { {},
                     std::format( "written for {}, this device is {}", FileName( header.Identity ),
                                  FileName( identity ) ) };
        const std::string_view payload = file.substr( sizeof header );
        if ( payload.size() != header.PayloadSize )
            return { {},
                     std::format( "payload is {} bytes, header says {} (truncated write?)", payload.size(),
                                  header.PayloadSize ) };
        if ( HashBytes( payload ) != header.PayloadHash )
            return { {}, "payload hash mismatch (corrupted file)" };
        return { std::string( payload ), {} };
    }

    bool PersistSchedule::Due( const uint64_t builtSoFar, const std::chrono::steady_clock::time_point now )
    {
        if ( builtSoFar == m_BuiltAtLastWrite )
            return false;
        if ( m_LastWrite && now - *m_LastWrite < m_Interval )
            return false;
        m_BuiltAtLastWrite = builtSoFar;
        m_LastWrite        = now;
        return true;
    }
} // namespace Desert::Graphic::PipelineCacheFile
