#include <Engine/Graphic/Denoise/OidnDenoiser.hpp>

#include <OpenImageDenoise/oidn.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

#if defined( _WIN32 )
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined( __APPLE__ )
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>
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
        decltype( &oidnGetDeviceInt )                      GetDeviceInt                      = nullptr;
        decltype( &oidnSyncDevice )                        SyncDevice                        = nullptr;
        decltype( &oidnNewSharedBufferFromWin32Handle )    NewSharedBufferFromWin32Handle    = nullptr;
        decltype( &oidnNewSharedBufferFromFD )             NewSharedBufferFromFD             = nullptr;
        decltype( &oidnNewSharedBufferFromMetal )          NewSharedBufferFromMetal          = nullptr;
        decltype( &oidnNewSharedSemaphoreFromWin32Handle ) NewSharedSemaphoreFromWin32Handle = nullptr;
        decltype( &oidnNewSharedSemaphoreFromFD )          NewSharedSemaphoreFromFD          = nullptr;
        decltype( &oidnWaitSemaphoresAsync )               WaitSemaphoresAsync               = nullptr;
        decltype( &oidnSignalSemaphoresAsync )             SignalSemaphoresAsync             = nullptr;
        decltype( &oidnReleaseSemaphore )                  ReleaseSemaphore                  = nullptr;
        decltype( &oidnReleaseBuffer )           ReleaseBuffer           = nullptr;
        decltype( &oidnNewFilter )               NewFilter               = nullptr;
        decltype( &oidnSetFilterImage )          SetFilterImage          = nullptr;
        decltype( &oidnSetFilterBool )           SetFilterBool           = nullptr;
        decltype( &oidnSetFilterInt )            SetFilterInt            = nullptr;
        decltype( &oidnCommitFilter )            CommitFilter            = nullptr;
        decltype( &oidnExecuteFilterAsync )                ExecuteFilterAsync                = nullptr;
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
            resolve( GetDeviceInt, "oidnGetDeviceInt" );
            resolve( SyncDevice, "oidnSyncDevice" );
            resolve( NewSharedBufferFromWin32Handle, "oidnNewSharedBufferFromWin32Handle" );
            resolve( NewSharedBufferFromFD, "oidnNewSharedBufferFromFD" );
            resolve( NewSharedBufferFromMetal, "oidnNewSharedBufferFromMetal" );
            resolve( NewSharedSemaphoreFromWin32Handle, "oidnNewSharedSemaphoreFromWin32Handle" );
            resolve( NewSharedSemaphoreFromFD, "oidnNewSharedSemaphoreFromFD" );
            resolve( WaitSemaphoresAsync, "oidnWaitSemaphoresAsync" );
            resolve( SignalSemaphoresAsync, "oidnSignalSemaphoresAsync" );
            resolve( ReleaseSemaphore, "oidnReleaseSemaphore" );
            resolve( ReleaseBuffer, "oidnReleaseBuffer" );
            resolve( NewFilter, "oidnNewFilter" );
            resolve( SetFilterImage, "oidnSetFilterImage" );
            resolve( SetFilterBool, "oidnSetFilterBool" );
            resolve( SetFilterInt, "oidnSetFilterInt" );
            resolve( CommitFilter, "oidnCommitFilter" );
            resolve( ExecuteFilterAsync, "oidnExecuteFilterAsync" );
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
        ReleaseShared();
        if ( m_Device && m_Library )
            m_Library->ReleaseDevice( static_cast<OIDNDevice>( m_Device ) );
    }

    Common::ResultStr<OidnDenoiser*> OidnDenoiser::Acquire( const std::array<uint8_t, 16>&        vulkanUuid,
                                                            const Common::Scalability::GpuSharing sharing )
    {
        using Common::Scalability::GpuSharing;
        if ( sharing == GpuSharing::None )
        {
            const std::string error =
                 "OIDN: CapabilityCatalog::Sharing is None - this device exports no memory or "
                 "timeline semaphore to another API, and OIDN filters only shared GPU memory";
            KeepError( error );
            return Common::MakeError<OidnDenoiser*>( error );
        }
        {
            const std::scoped_lock lock( g_Mutex );
            if ( g_Instance && g_Instance->m_VulkanUuid == vulkanUuid && g_Instance->m_Sharing == sharing )
                return Common::MakeSuccess( g_Instance.get() );
        }
        std::unique_ptr<OidnDenoiser> made( new OidnDenoiser() );
        made->m_VulkanUuid = vulkanUuid;
        made->m_Sharing    = sharing;
        made->m_Library    = std::make_unique<Library>();
        if ( const Common::BoolResultStr loaded = made->m_Library->Load(); !loaded )
        {
            KeepError( loaded.GetError() );
            return Common::MakeError<OidnDenoiser*>( loaded.GetError() );
        }
        const Library& lib = *made->m_Library;

        // THE DEVICE (header): OIDN's physical device carrying the Vulkan device's UUID, a GPU one; else refused.
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
        if ( match < 0 )
        {
            const std::string error = std::format(
                 "OIDN: no OIDN GPU module offers the Vulkan device (UUID {}); OIDN's physical devices: "
                 "{}",
                 FormatUuid( vulkanUuid ), seen.empty() ? "none" : seen );
            KeepError( error );
            return Common::MakeError<OidnDenoiser*>( error );
        }
        OIDNDevice device    = lib.NewDeviceByID( match );
        made->m_DeviceName   = std::format( "{}: {}", DeviceTypeName( lib.GetPhysicalDeviceInt( match, "type" ) ),
                                            lib.GetPhysicalDeviceString( match, "name" ) );
        made->m_DeviceReason = std::format( "OIDN physical device {} has the Vulkan device's UUID {}", match,
                                            FormatUuid( vulkanUuid ) );
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
        // The handle types the renderer exports (header: THE DEVICE). Metal shares MTLBuffers and the queue, not
        // handles: nothing to ask.
        if ( sharing != GpuSharing::Metal )
        {
            const int  memoryTypes    = lib.GetDeviceInt( device, "externalMemoryTypes" );
            const int  semaphoreTypes = lib.GetDeviceInt( device, "externalSemaphoreTypes" );
            const bool win32          = sharing == GpuSharing::Win32;
            const int  memoryNeeded =
                 win32 ? OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_WIN32 : OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD;
            const int semaphoreNeeded = win32 ? OIDN_EXTERNAL_SEMAPHORE_TYPE_FLAG_TIMELINE_SEMAPHORE_WIN32
                                              : OIDN_EXTERNAL_SEMAPHORE_TYPE_FLAG_TIMELINE_SEMAPHORE_FD;
            if ( ( memoryTypes & memoryNeeded ) == 0 || ( semaphoreTypes & semaphoreNeeded ) == 0 )
            {
                lib.ReleaseDevice( device );
                const std::string error = std::format(
                     "OIDN: the {} device imports external memory types 0x{:x} and semaphore types 0x{:x}; the "
                     "renderer exports {} (memory 0x{:x}, timeline 0x{:x})",
                     made->m_DeviceName, memoryTypes, semaphoreTypes, win32 ? "Win32" : "Fd", memoryNeeded,
                     semaphoreNeeded );
                KeepError( error );
                return Common::MakeError<OidnDenoiser*>( error );
            }
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

    void OidnDenoiser::ReleaseShared()
    {
        if ( !m_Library || !m_Device )
            return;
        const Library&   lib    = *m_Library;
        const OIDNDevice device = static_cast<OIDNDevice>( m_Device );
        // OIDN's stream may still run an enqueued filter on these buffers: it finishes before they go.
        if ( !m_Filters.empty() || !m_Buffers.empty() || m_Semaphore )
            lib.SyncDevice( device );
        for ( void* filter : m_Filters )
            lib.ReleaseFilter( static_cast<OIDNFilter>( filter ) );
        for ( void* buffer : m_Buffers )
            lib.ReleaseBuffer( static_cast<OIDNBuffer>( buffer ) );
        if ( m_Semaphore )
            lib.ReleaseSemaphore( static_cast<OIDNSemaphore>( m_Semaphore ) );
        m_Filters.clear();
        m_Buffers.clear();
        m_Semaphore = nullptr;
        (void)lib.TakeError( device );
    }

    Common::BoolResultStr OidnDenoiser::BindShared( const SharedDenoiseImages& images,
                                                    const ImageDenoiseQuality  quality )
    {
        using Common::Scalability::GpuSharing;
        ReleaseShared();
        const Library&   lib    = *m_Library;
        const OIDNDevice device = static_cast<OIDNDevice>( m_Device );
        const uint64_t   bytes  = static_cast<uint64_t>( images.Width ) * images.Height * 3u * sizeof( float );
        auto             fail   = [&]( const std::string& error ) -> Common::BoolResultStr
        {
            ReleaseShared();
            return Common::MakeError( std::format( "OIDN ({}): {}", m_DeviceName, error ) );
        };
        if ( bytes == 0u )
            return fail( "a zero-sized image" );
        for ( const SharedDenoiseImage* image : { &images.Color, &images.Albedo, &images.Normal, &images.Output } )
            if ( image->Buffer >= images.Buffers.size() ||
                 image->ByteOffset + bytes > images.Buffers[image->Buffer].Bytes )
                return fail( std::format( "an image of {}x{} float3 at byte {} lies outside its shared buffer",
                                          images.Width, images.Height, image->ByteOffset ) );

        // The buffers (header: THE MEMORY).
        for ( const SharedDenoiseBuffer& shared : images.Buffers )
        {
            const SharedHandle& handle = shared.Handle;
            if ( handle.Kind != m_Sharing )
                return fail( "a shared buffer of another handle type than the device was made for" );
            const int  dedicated = handle.Dedicated ? OIDN_EXTERNAL_MEMORY_TYPE_FLAG_DEDICATED : 0;
            OIDNBuffer buffer    = nullptr;
            switch ( handle.Kind )
            {
                case GpuSharing::Win32:
                    buffer = lib.NewSharedBufferFromWin32Handle(
                         device, OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_WIN32 | dedicated, handle.Win32, nullptr,
                         static_cast<size_t>( handle.AllocationBytes ) );
                    break;
                case GpuSharing::Fd:
                    buffer =
                         lib.NewSharedBufferFromFD( device, OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD | dedicated,
                                                    handle.Fd, static_cast<size_t>( handle.AllocationBytes ) );
#if !defined( _WIN32 )
                    if ( !buffer && handle.Fd >= 0 )
                        ::close( handle.Fd ); // FromFD takes the descriptor only on success
#endif
                    break;
                case GpuSharing::Metal:
                    buffer = lib.NewSharedBufferFromMetal( device, static_cast<MTLBuffer_id>( handle.Metal ) );
                    break;
                case GpuSharing::None:
                    break;
            }
            if ( !buffer )
            {
                const std::string error = lib.TakeError( device );
                return fail( std::format( "importing a shared buffer of {} bytes failed: {}",
                                          handle.AllocationBytes, error.empty() ? "no buffer" : error ) );
            }
            m_Buffers.push_back( buffer );
        }

        // The timeline (none on Metal: one queue).
        if ( images.Timeline.Kind != GpuSharing::None )
        {
            if ( images.Timeline.Kind != m_Sharing )
                return fail( "a timeline of another handle type than the device was made for" );
            OIDNSemaphore semaphore = nullptr;
            if ( images.Timeline.Kind == GpuSharing::Win32 )
                semaphore = lib.NewSharedSemaphoreFromWin32Handle(
                     device, OIDN_EXTERNAL_SEMAPHORE_TYPE_FLAG_TIMELINE_SEMAPHORE_WIN32, images.Timeline.Win32,
                     nullptr );
            else if ( images.Timeline.Kind == GpuSharing::Fd )
            {
                semaphore = lib.NewSharedSemaphoreFromFD(
                     device, OIDN_EXTERNAL_SEMAPHORE_TYPE_FLAG_TIMELINE_SEMAPHORE_FD, images.Timeline.Fd );
#if !defined( _WIN32 )
                if ( !semaphore && images.Timeline.Fd >= 0 )
                    ::close( images.Timeline.Fd );
#endif
            }
            if ( !semaphore )
            {
                const std::string error = lib.TakeError( device );
                return fail( std::format( "importing the shared timeline failed: {}",
                                          error.empty() ? "no semaphore" : error ) );
            }
            m_Semaphore = semaphore;
        }
        else if ( m_Sharing != GpuSharing::Metal )
            return fail( "no shared timeline: the renderer's queue and OIDN's would be unordered" );

        // THE FILTER (header): the guides' prefilters in place, then the beauty with cleanAux.
        const int  oidnQuality = quality == ImageDenoiseQuality::High ? OIDN_QUALITY_HIGH : OIDN_QUALITY_BALANCED;
        const auto image       = [&]( OIDNFilter filter, const char* name, const SharedDenoiseImage& at )
        {
            lib.SetFilterImage( filter, name, static_cast<OIDNBuffer>( m_Buffers[at.Buffer] ), OIDN_FORMAT_FLOAT3,
                                images.Width, images.Height, static_cast<size_t>( at.ByteOffset ), 0, 0 );
        };
        const std::pair<const char*, SharedDenoiseImage> guides[] = { { "albedo", images.Albedo },
                                                                      { "normal", images.Normal } };
        for ( const auto& [name, at] : guides )
        {
            OIDNFilter prefilter = lib.NewFilter( device, "RT" );
            if ( !prefilter )
                return fail( std::format( "the {} prefilter: {}", name, lib.TakeError( device ) ) );
            m_Filters.push_back( prefilter );
            image( prefilter, name, at );
            image( prefilter, "output", at );
            lib.SetFilterInt( prefilter, "quality", oidnQuality );
            lib.CommitFilter( prefilter );
            if ( std::string error = lib.TakeError( device ); !error.empty() )
                return fail( std::format( "the {} prefilter: {}", name, error ) );
        }
        OIDNFilter beauty = lib.NewFilter( device, "RT" );
        if ( !beauty )
            return fail( std::format( "the beauty filter: {}", lib.TakeError( device ) ) );
        m_Filters.push_back( beauty );
        image( beauty, "color", images.Color );
        image( beauty, "albedo", images.Albedo );
        image( beauty, "normal", images.Normal );
        image( beauty, "output", images.Output );
        lib.SetFilterBool( beauty, "hdr", true );
        lib.SetFilterBool( beauty, "cleanAux", true );
        lib.SetFilterInt( beauty, "quality", oidnQuality );
        lib.CommitFilter( beauty );
        if ( std::string error = lib.TakeError( device ); !error.empty() )
            return fail( std::format( "the beauty filter: {}", error ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr OidnDenoiser::EnqueueShared( const uint64_t waitValue, const uint64_t signalValue )
    {
        if ( m_Filters.empty() )
            return Common::MakeError( std::format( "OIDN ({}): EnqueueShared before BindShared", m_DeviceName ) );
        const Library&   lib       = *m_Library;
        const OIDNDevice device    = static_cast<OIDNDevice>( m_Device );
        OIDNSemaphore    semaphore = static_cast<OIDNSemaphore>( m_Semaphore );
        if ( semaphore )
            lib.WaitSemaphoresAsync( device, &semaphore, &waitValue, nullptr, 1 );
        for ( void* filter : m_Filters )
            lib.ExecuteFilterAsync( static_cast<OIDNFilter>( filter ) );
        if ( semaphore )
            lib.SignalSemaphoresAsync( device, &semaphore, &signalValue, 1 );
        if ( std::string error = lib.TakeError( device ); !error.empty() )
            return Common::MakeError(
                 std::format( "OIDN ({}): enqueueing the denoise failed: {}", m_DeviceName, error ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::Denoise
