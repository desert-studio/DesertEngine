#include <Engine/Graphic/Denoise/OidnDenoiser.hpp>

#include <OpenImageDenoise/oidn.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <vector>

#if defined( _WIN32 )
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined( __APPLE__ )
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

namespace Desert::Graphic::Denoise
{
    namespace
    {
        std::mutex                    g_Mutex; // guards g_Status and g_Instance
        OidnStatus                    g_Status;
        std::unique_ptr<OidnDenoiser> g_Instance;

        void KeepError( const std::string& error )
        {
            const std::scoped_lock lock( g_Mutex );
            g_Status.Error = error;
        }

        std::filesystem::path ExecutableDirectory()
        {
#if defined( _WIN32 )
            std::wstring buffer( 32768, L'\0' );
            const DWORD length = GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size() ) );
            buffer.resize( length );
            return std::filesystem::path( buffer ).parent_path();
#elif defined( __APPLE__ )
            uint32_t size = 0;
            _NSGetExecutablePath( nullptr, &size );
            std::string buffer( size, '\0' );
            if ( _NSGetExecutablePath( buffer.data(), &size ) != 0 )
                return {};
            std::error_code error;
            const auto      canonical = std::filesystem::weakly_canonical( buffer.c_str(), error );
            return ( error ? std::filesystem::path( buffer.c_str() ) : canonical ).parent_path();
#else
            return {};
#endif
        }

        const char* DeviceTypeName( const int type )
        {
            switch ( type )
            {
                case OIDN_DEVICE_TYPE_CPU:
                    return "CPU";
                case OIDN_DEVICE_TYPE_SYCL:
                    return "SYCL";
                case OIDN_DEVICE_TYPE_CUDA:
                    return "CUDA";
                case OIDN_DEVICE_TYPE_HIP:
                    return "HIP";
                case OIDN_DEVICE_TYPE_METAL:
                    return "Metal";
                default:
                    return "unknown";
            }
        }

        std::string FormatUuid( const std::array<uint8_t, 16>& uuid )
        {
            std::string text;
            for ( const uint8_t byte : uuid )
                text += std::format( "{:02x}", byte );
            return text;
        }
    } // namespace

    // The entry points, resolved once from the loaded library (nothing links OIDN).
    struct OidnDenoiser::Library
    {
#if defined( _WIN32 )
        HMODULE Module = nullptr;
#else
        void* Module = nullptr;
#endif
        decltype( &oidnGetNumPhysicalDevices )   GetNumPhysicalDevices   = nullptr;
        decltype( &oidnGetPhysicalDeviceBool )   GetPhysicalDeviceBool   = nullptr;
        decltype( &oidnGetPhysicalDeviceInt )    GetPhysicalDeviceInt    = nullptr;
        decltype( &oidnGetPhysicalDeviceString ) GetPhysicalDeviceString = nullptr;
        decltype( &oidnGetPhysicalDeviceData )   GetPhysicalDeviceData   = nullptr;
        decltype( &oidnNewDevice )               NewDevice               = nullptr;
        decltype( &oidnNewDeviceByID )           NewDeviceByID           = nullptr;
        decltype( &oidnCommitDevice )            CommitDevice            = nullptr;
        decltype( &oidnReleaseDevice )           ReleaseDevice           = nullptr;
        decltype( &oidnGetDeviceError )          GetDeviceError          = nullptr;
        decltype( &oidnNewBuffer )               NewBuffer               = nullptr;
        decltype( &oidnWriteBuffer )             WriteBuffer             = nullptr;
        decltype( &oidnReadBuffer )              ReadBuffer              = nullptr;
        decltype( &oidnReleaseBuffer )           ReleaseBuffer           = nullptr;
        decltype( &oidnNewFilter )               NewFilter               = nullptr;
        decltype( &oidnSetFilterImage )          SetFilterImage          = nullptr;
        decltype( &oidnSetFilterBool )           SetFilterBool           = nullptr;
        decltype( &oidnSetFilterInt )            SetFilterInt            = nullptr;
        decltype( &oidnCommitFilter )            CommitFilter            = nullptr;
        decltype( &oidnExecuteFilter )           ExecuteFilter           = nullptr;
        decltype( &oidnReleaseFilter )           ReleaseFilter           = nullptr;

        ~Library()
        {
#if defined( _WIN32 )
            if ( Module )
                FreeLibrary( Module );
#else
            if ( Module )
                dlclose( Module );
#endif
        }

        [[nodiscard]] Common::BoolResultStr Load()
        {
            const std::filesystem::path directory = ExecutableDirectory();
#if defined( _WIN32 )
            const std::filesystem::path file = directory / "OpenImageDenoise.dll";
            // LOAD_WITH_ALTERED_SEARCH_PATH: OpenImageDenoise_core.dll and the tbb / SYCL runtimes it needs
            // resolve from the library's own directory first.
            Module = LoadLibraryExW( file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
            if ( !Module )
                return Common::MakeError(
                     std::format( "OIDN: LoadLibrary({}) failed with Win32 error {}; run python "
                                  "scripts/Windows/FetchVendorBinaries.py and rebuild",
                                  file.string(), GetLastError() ) );
#elif defined( __APPLE__ )
            const std::filesystem::path file = directory / "lib" / "libOpenImageDenoise.2.dylib";
            Module                           = dlopen( file.c_str(), RTLD_NOW | RTLD_LOCAL );
            if ( !Module )
                return Common::MakeError( std::format( "OIDN: dlopen({}) failed: {}; run python3 "
                                                       "scripts/Windows/FetchVendorBinaries.py and rebuild",
                                                       file.string(), dlerror() ) );
#else
            return Common::MakeError( "OIDN: no Open Image Denoise runtime is shipped for this platform "
                                      "(ThirdParty/OIDN has Windows x64 "
                                      "and macOS arm64)" );
#endif
            std::string missing;
            auto        resolve = [&]( auto& slot, const char* name )
            {
#if defined( _WIN32 )
                void* address = reinterpret_cast<void*>( GetProcAddress( Module, name ) );
#else
                void* address = dlsym( Module, name );
#endif
                slot = reinterpret_cast<std::remove_reference_t<decltype( slot )>>( address );
                if ( !address )
                    missing += std::format( "{}{}", missing.empty() ? "" : ", ", name );
            };
            resolve( GetNumPhysicalDevices, "oidnGetNumPhysicalDevices" );
            resolve( GetPhysicalDeviceBool, "oidnGetPhysicalDeviceBool" );
            resolve( GetPhysicalDeviceInt, "oidnGetPhysicalDeviceInt" );
            resolve( GetPhysicalDeviceString, "oidnGetPhysicalDeviceString" );
            resolve( GetPhysicalDeviceData, "oidnGetPhysicalDeviceData" );
            resolve( NewDevice, "oidnNewDevice" );
            resolve( NewDeviceByID, "oidnNewDeviceByID" );
            resolve( CommitDevice, "oidnCommitDevice" );
            resolve( ReleaseDevice, "oidnReleaseDevice" );
            resolve( GetDeviceError, "oidnGetDeviceError" );
            resolve( NewBuffer, "oidnNewBuffer" );
            resolve( WriteBuffer, "oidnWriteBuffer" );
            resolve( ReadBuffer, "oidnReadBuffer" );
            resolve( ReleaseBuffer, "oidnReleaseBuffer" );
            resolve( NewFilter, "oidnNewFilter" );
            resolve( SetFilterImage, "oidnSetFilterImage" );
            resolve( SetFilterBool, "oidnSetFilterBool" );
            resolve( SetFilterInt, "oidnSetFilterInt" );
            resolve( CommitFilter, "oidnCommitFilter" );
            resolve( ExecuteFilter, "oidnExecuteFilter" );
            resolve( ReleaseFilter, "oidnReleaseFilter" );
            if ( !missing.empty() )
                return Common::MakeError( std::format(
                     "OIDN: {} lacks {} (not the 2.x runtime ThirdParty/OIDN pins)", file.string(), missing ) );
            return Common::MakeSuccess( true );
        }

        // The device's pending error as text (clears it), empty when none.
        [[nodiscard]] std::string TakeError( OIDNDevice device ) const
        {
            const char*     message = nullptr;
            const OIDNError code    = GetDeviceError( device, &message );
            if ( code == OIDN_ERROR_NONE )
                return {};
            return std::format( "OIDN error {}: {}", static_cast<int>( code ),
                                message ? message : "(no message)" );
        }
    };

    OidnDenoiser::~OidnDenoiser()
    {
        if ( m_Device && m_Library )
            m_Library->ReleaseDevice( static_cast<OIDNDevice>( m_Device ) );
    }

    Common::ResultStr<OidnDenoiser*> OidnDenoiser::Acquire( const std::array<uint8_t, 16>& vulkanUuid )
    {
        {
            const std::scoped_lock lock( g_Mutex );
            if ( g_Instance && g_Instance->m_VulkanUuid == vulkanUuid )
                return Common::MakeSuccess( g_Instance.get() );
        }
        std::unique_ptr<OidnDenoiser> made( new OidnDenoiser() );
        made->m_VulkanUuid = vulkanUuid;
        made->m_Library    = std::make_unique<Library>();
        if ( const Common::BoolResultStr loaded = made->m_Library->Load(); !loaded )
        {
            KeepError( loaded.GetError() );
            return Common::MakeError<OidnDenoiser*>( loaded.GetError() );
        }
        const Library& lib = *made->m_Library;

        // THE DEVICE (header): OIDN's physical device carrying the Vulkan device's UUID, a GPU one; else the CPU.
        const int   count = lib.GetNumPhysicalDevices();
        int         match = -1;
        std::string seen;
        for ( int id = 0; id < count; ++id )
        {
            const int   type = lib.GetPhysicalDeviceInt( id, "type" );
            const char* name = lib.GetPhysicalDeviceString( id, "name" );
            seen +=
                 std::format( "{}{} '{}'", seen.empty() ? "" : ", ", DeviceTypeName( type ), name ? name : "?" );
            if ( type == OIDN_DEVICE_TYPE_CPU || !lib.GetPhysicalDeviceBool( id, "uuidSupported" ) )
                continue;
            size_t      bytes = 0;
            const void* uuid  = lib.GetPhysicalDeviceData( id, "uuid", &bytes );
            if ( uuid && bytes == OIDN_UUID_SIZE && std::memcmp( uuid, vulkanUuid.data(), OIDN_UUID_SIZE ) == 0 )
            {
                match = id;
                break;
            }
        }
        OIDNDevice device = nullptr;
        if ( match >= 0 )
        {
            device = lib.NewDeviceByID( match );
            made->m_DeviceName =
                 std::format( "{}: {}", DeviceTypeName( lib.GetPhysicalDeviceInt( match, "type" ) ),
                              lib.GetPhysicalDeviceString( match, "name" ) );
            made->m_DeviceReason = std::format( "OIDN physical device {} has the Vulkan device's UUID {}", match,
                                                FormatUuid( vulkanUuid ) );
        }
        else
        {
            device             = lib.NewDevice( OIDN_DEVICE_TYPE_CPU );
            made->m_DeviceName = "CPU";
            made->m_DeviceReason =
                 std::format( "no OIDN GPU module offers the Vulkan device (UUID {}); OIDN's physical devices: {}",
                              FormatUuid( vulkanUuid ), seen.empty() ? "none" : seen );
        }
        if ( !device )
        {
            const std::string error = std::format( "OIDN: could not create the {} device: {}", made->m_DeviceName,
                                                   lib.TakeError( nullptr ) );
            KeepError( error );
            return Common::MakeError<OidnDenoiser*>( error );
        }
        lib.CommitDevice( device );
        if ( std::string error = lib.TakeError( device ); !error.empty() )
        {
            lib.ReleaseDevice( device );
            error = std::format( "OIDN: committing the {} device failed: {}", made->m_DeviceName, error );
            KeepError( error );
            return Common::MakeError<OidnDenoiser*>( error );
        }
        made->m_Device = device;
        LOG_INFO( "[OIDN] Open Image Denoise {} device {} ({})", OIDN_VERSION_STRING, made->m_DeviceName,
                  made->m_DeviceReason );

        const std::scoped_lock lock( g_Mutex );
        g_Status.Created = true;
        g_Status.Device  = made->m_DeviceName;
        g_Status.Reason  = made->m_DeviceReason;
        g_Status.Error.clear();
        g_Instance = std::move( made );
        return Common::MakeSuccess( g_Instance.get() );
    }

    OidnStatus OidnDenoiser::Status()
    {
        const std::scoped_lock lock( g_Mutex );
        return g_Status;
    }

    void OidnDenoiser::NoteRun( const uint32_t paths, const uint32_t width, const uint32_t height,
                                const double milliseconds )
    {
        const std::scoped_lock lock( g_Mutex );
        ++g_Status.Runs;
        g_Status.LastPaths        = paths;
        g_Status.LastWidth        = width;
        g_Status.LastHeight       = height;
        g_Status.LastMilliseconds = milliseconds;
        g_Status.Error.clear();
    }

    void OidnDenoiser::NoteError( const std::string& error )
    {
        KeepError( error );
    }

    Common::ResultStr<ImageDenoiseReport> OidnDenoiser::Denoise( const ImageDenoiseInputs& inputs,
                                                                 const ImageDenoiseQuality quality,
                                                                 const std::span<float>    output )
    {
        const auto   start  = std::chrono::steady_clock::now();
        const size_t pixels = static_cast<size_t>( inputs.Width ) * inputs.Height;
        const size_t floats = pixels * 3u;
        if ( pixels == 0u || inputs.Color.size() != floats || output.size() != floats ||
             ( !inputs.Albedo.empty() && inputs.Albedo.size() != floats ) ||
             ( !inputs.Normal.empty() && inputs.Normal.size() != floats ) )
            return Common::MakeError<ImageDenoiseReport>( std::format(
                 "OIDN: {}x{} needs {} floats per image; got colour {}, albedo {}, normal {}, output {}",
                 inputs.Width, inputs.Height, floats, inputs.Color.size(), inputs.Albedo.size(),
                 inputs.Normal.size(), output.size() ) );
        if ( !inputs.Normal.empty() && inputs.Albedo.empty() )
            return Common::MakeError<ImageDenoiseReport>( "OIDN: a normal guide needs the albedo guide as well (OIDN's RT filter)" );

        const Library&   lib    = *m_Library;
        const OIDNDevice device = static_cast<OIDNDevice>( m_Device );
        const size_t     bytes  = floats * sizeof( float );
        const int oidnQuality   = quality == ImageDenoiseQuality::High ? OIDN_QUALITY_HIGH : OIDN_QUALITY_BALANCED;

        // The device's own buffers: on a GPU device host memory is not addressable by the filter.
        OIDNBuffer              color  = lib.NewBuffer( device, bytes );
        OIDNBuffer              albedo = inputs.Albedo.empty() ? nullptr : lib.NewBuffer( device, bytes );
        OIDNBuffer              normal = inputs.Normal.empty() ? nullptr : lib.NewBuffer( device, bytes );
        OIDNBuffer              result = lib.NewBuffer( device, bytes );
        std::vector<OIDNFilter> filters;
        auto                    release = [&]()
        {
            for ( OIDNFilter filter : filters )
                lib.ReleaseFilter( filter );
            for ( OIDNBuffer buffer : { color, albedo, normal, result } )
                if ( buffer )
                    lib.ReleaseBuffer( buffer );
        };
        auto refuse = [&]( const char* stage ) -> std::string
        {
            const std::string error = lib.TakeError( device );
            return error.empty() ? std::string{} : std::format( "OIDN {} ({}): {}", stage, m_DeviceName, error );
        };
        if ( std::string error = refuse( "buffers" ); !error.empty() || !color || !result )
        {
            release();
            return Common::MakeError<ImageDenoiseReport>( error.empty() ? "OIDN: buffer allocation failed" : error );
        }
        lib.WriteBuffer( color, 0, bytes, inputs.Color.data() );
        if ( albedo )
            lib.WriteBuffer( albedo, 0, bytes, inputs.Albedo.data() );
        if ( normal )
            lib.WriteBuffer( normal, 0, bytes, inputs.Normal.data() );

        auto image = [&]( OIDNFilter filter, const char* name, OIDNBuffer buffer )
        { lib.SetFilterImage( filter, name, buffer, OIDN_FORMAT_FLOAT3, inputs.Width, inputs.Height, 0, 0, 0 ); };

        // The guides' own pass (header: THE FILTER): each denoised in place by an RT filter of its own.
        for ( auto [name, buffer] : { std::pair{ "albedo", albedo }, std::pair{ "normal", normal } } )
        {
            if ( !buffer )
                continue;
            OIDNFilter prefilter = lib.NewFilter( device, "RT" );
            filters.push_back( prefilter );
            image( prefilter, name, buffer );
            image( prefilter, "output", buffer );
            lib.SetFilterInt( prefilter, "quality", oidnQuality );
            lib.CommitFilter( prefilter );
            lib.ExecuteFilter( prefilter );
            if ( std::string error = refuse( name ); !error.empty() )
            {
                release();
                return Common::MakeError<ImageDenoiseReport>( error );
            }
        }

        OIDNFilter beauty = lib.NewFilter( device, "RT" );
        filters.push_back( beauty );
        image( beauty, "color", color );
        if ( albedo )
            image( beauty, "albedo", albedo );
        if ( normal )
            image( beauty, "normal", normal );
        image( beauty, "output", result );
        lib.SetFilterBool( beauty, "hdr", true );
        lib.SetFilterBool( beauty, "cleanAux", albedo != nullptr );
        lib.SetFilterInt( beauty, "quality", oidnQuality );
        lib.CommitFilter( beauty );
        lib.ExecuteFilter( beauty );
        if ( std::string error = refuse( "filter" ); !error.empty() )
        {
            release();
            return Common::MakeError<ImageDenoiseReport>( error );
        }
        lib.ReadBuffer( result, 0, bytes, output.data() );
        if ( std::string error = refuse( "read-back" ); !error.empty() )
        {
            release();
            return Common::MakeError<ImageDenoiseReport>( error );
        }
        release();
        const double ms =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
        return Common::MakeSuccess( ImageDenoiseReport{ .Milliseconds = ms } );
    }
} // namespace Desert::Graphic::Denoise
