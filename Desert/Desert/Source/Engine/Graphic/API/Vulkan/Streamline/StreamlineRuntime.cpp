#include <Engine/Graphic/API/Vulkan/Streamline/StreamlineRuntime.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp>

#include <vk-bootstrap/VkBootstrap.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers.h>
#include <sl_helpers_vk.h>
#include <sl_security.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>

namespace Desert::Graphic::Streamline
{
    namespace
    {
        constexpr uint32_t          kVendorNvidia = 0x10DE;
        constexpr std::wstring_view kInterposer   = L"sl.interposer.dll";
        // Identifies this engine to NGX when the application has no NVIDIA application id (Streamline's
        // projectId).
        constexpr const char* kProjectId     = "63f85582-ed00-44f9-86cc-8c1fecc48832";
        constexpr const char* kEngineVersion = "DesertEngine";

        std::filesystem::path ExecutableDirectory()
        {
            std::array<wchar_t, 32768> buffer{};
            const DWORD length = GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size() ) );
            return std::filesystem::path( std::wstring( buffer.data(), length ) ).parent_path();
        }

        std::string_view ResultName( const sl::Result result )
        {
            return sl::getResultAsStr( result );
        }

        sl::DLSSMode ToSl( const DlssMode mode )
        {
            switch ( mode )
            {
                case DlssMode::UltraPerformance:
                    return sl::DLSSMode::eUltraPerformance;
                case DlssMode::Performance:
                    return sl::DLSSMode::eMaxPerformance;
                case DlssMode::Balanced:
                    return sl::DLSSMode::eBalanced;
                case DlssMode::Quality:
                    return sl::DLSSMode::eMaxQuality;
                case DlssMode::UltraQuality:
                    return sl::DLSSMode::eUltraQuality;
                case DlssMode::DLAA:
                    return sl::DLSSMode::eDLAA;
            }
            return sl::DLSSMode::eOff;
        }

        sl::float4x4 ToSl( const glm::mat4& m )
        {
            // glm stores M column-major, which is the row-major storage of M^T: exactly the matrix Streamline's
            // row-vector convention (v * M) multiplies by.
            sl::float4x4 out;
            static_assert( sizeof( out ) == sizeof( m ) );
            std::memcpy( static_cast<void*>( &out ), &m, sizeof( m ) );
            return out;
        }

        sl::float3 ToSl( const glm::vec3& v )
        {
            return { v.x, v.y, v.z };
        }

        sl::Resource ToSl( const DlssImage& image )
        {
            sl::Resource resource( sl::ResourceType::eTex2d, reinterpret_cast<void*>( image.Image ), nullptr,
                                   reinterpret_cast<void*>( image.View ), static_cast<uint32_t>( image.Layout ) );
            resource.width        = image.Width;
            resource.height       = image.Height;
            resource.nativeFormat = static_cast<uint32_t>( image.Format );
            resource.mipLevels    = 1;
            resource.arrayLayers  = 1;
            resource.flags        = 0;
            resource.usage        = image.Usage;
            return resource;
        }

        // Streamline names the Vulkan 1.2 / 1.3 features it needs by their member name; these are the members a
        // Streamline feature can name. An unknown name refuses DLSS with the name, never enables a guess.
        struct FeatureBit12
        {
            std::string_view Name;
            VkBool32 VkPhysicalDeviceVulkan12Features::*Member;
        };
        struct FeatureBit13
        {
            std::string_view Name;
            VkBool32 VkPhysicalDeviceVulkan13Features::*Member;
        };
        constexpr std::array kFeatures12{
             FeatureBit12{ "timelineSemaphore", &VkPhysicalDeviceVulkan12Features::timelineSemaphore },
             FeatureBit12{ "bufferDeviceAddress", &VkPhysicalDeviceVulkan12Features::bufferDeviceAddress },
             FeatureBit12{ "descriptorIndexing", &VkPhysicalDeviceVulkan12Features::descriptorIndexing },
             FeatureBit12{ "runtimeDescriptorArray", &VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray },
             FeatureBit12{ "shaderFloat16", &VkPhysicalDeviceVulkan12Features::shaderFloat16 },
             FeatureBit12{ "storageBuffer8BitAccess", &VkPhysicalDeviceVulkan12Features::storageBuffer8BitAccess },
             FeatureBit12{ "scalarBlockLayout", &VkPhysicalDeviceVulkan12Features::scalarBlockLayout },
             FeatureBit12{ "hostQueryReset", &VkPhysicalDeviceVulkan12Features::hostQueryReset },
             FeatureBit12{ "shaderSampledImageArrayNonUniformIndexing",
                           &VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing },
             FeatureBit12{ "descriptorBindingPartiallyBound",
                           &VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound },
             FeatureBit12{ "descriptorBindingVariableDescriptorCount",
                           &VkPhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount },
             FeatureBit12{ "vulkanMemoryModel", &VkPhysicalDeviceVulkan12Features::vulkanMemoryModel },
        };
        constexpr std::array kFeatures13{
             FeatureBit13{ "synchronization2", &VkPhysicalDeviceVulkan13Features::synchronization2 },
             FeatureBit13{ "dynamicRendering", &VkPhysicalDeviceVulkan13Features::dynamicRendering },
             FeatureBit13{ "maintenance4", &VkPhysicalDeviceVulkan13Features::maintenance4 },
             FeatureBit13{ "shaderDemoteToHelperInvocation",
                           &VkPhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation },
             FeatureBit13{ "subgroupSizeControl", &VkPhysicalDeviceVulkan13Features::subgroupSizeControl },
             FeatureBit13{ "computeFullSubgroups", &VkPhysicalDeviceVulkan13Features::computeFullSubgroups },
        };

        template <typename Table, typename Struct>
        Common::BoolResultStr SetFeatures( const Table& table, const char* const* names, const uint32_t count,
                                           Struct& features, const std::string_view version )
        {
            for ( uint32_t i = 0; i < count; ++i )
            {
                const std::string_view name = names[i];
                const auto*            row =
                     std::find_if( table.begin(), table.end(), [&]( const auto& r ) { return r.Name == name; } );
                if ( row == table.end() )
                    return Common::MakeFormattedError(
                         "Streamline asks for the Vulkan {} feature '{}', which this engine cannot name", version,
                         name );
                features.*( row->Member ) = VK_TRUE;
            }
            return Common::MakeSuccess( true );
        }

        void LogFromStreamline( const sl::LogType type, const char* message )
        {
            const std::string_view text = message != nullptr ? message : "";
            switch ( type )
            {
                case sl::LogType::eError:
                    LOG_ERROR( "[Streamline] {}", text );
                    return;
                case sl::LogType::eWarn:
                    LOG_WARN( "[Streamline] {}", text );
                    return;
                default:
                    LOG_TRACE( "[Streamline] {}", text );
                    return;
            }
        }
    } // namespace

    std::string_view DlssModeName( const DlssMode mode )
    {
        switch ( mode )
        {
            case DlssMode::UltraPerformance:
                return "UltraPerformance";
            case DlssMode::Performance:
                return "Performance";
            case DlssMode::Balanced:
                return "Balanced";
            case DlssMode::Quality:
                return "Quality";
            case DlssMode::UltraQuality:
                return "UltraQuality";
            case DlssMode::DLAA:
                return "DLAA";
        }
        return "<unknown DlssMode>";
    }

    struct Runtime::Library
    {
        HMODULE                        Module                 = nullptr;
        PFun_slInit*                   Init                   = nullptr;
        PFun_slShutdown*               Shutdown               = nullptr;
        PFun_slIsFeatureSupported*     IsFeatureSupported     = nullptr;
        PFun_slGetFeatureRequirements* GetFeatureRequirements = nullptr;
        PFun_slSetVulkanInfo*          SetVulkanInfo          = nullptr;
        PFun_slGetFeatureFunction*     GetFeatureFunction     = nullptr;
        PFun_slGetNewFrameToken*       GetNewFrameToken       = nullptr;
        PFun_slSetConstants*           SetConstants           = nullptr;
        PFun_slEvaluateFeature*        EvaluateFeature        = nullptr;
        PFun_slFreeResources*          FreeResources          = nullptr;
        // DLSS's own entry points, from slGetFeatureFunction once the device is set.
        PFun_slDLSSGetOptimalSettings* GetOptimalSettings = nullptr;
        PFun_slDLSSSetOptions*         SetOptions         = nullptr;

        ~Library()
        {
            if ( Module != nullptr )
                FreeLibrary( Module );
        }

        template <typename T>
        Common::BoolResultStr Resolve( T*& slot, const char* name )
        {
            slot = reinterpret_cast<T*>( reinterpret_cast<void*>( GetProcAddress( Module, name ) ) );
            if ( slot == nullptr )
                return Common::MakeFormattedError( "sl.interposer.dll exports no '{}'", name );
            return Common::MakeSuccess( true );
        }
    };

    Runtime& Runtime::Get()
    {
        static Runtime runtime;
        return runtime;
    }

    Runtime::Runtime()  = default;
    Runtime::~Runtime() = default;

    std::string_view Runtime::Name() const
    {
        return "Streamline";
    }

    bool Runtime::DlssReady() const
    {
        const std::scoped_lock lock( m_Mutex );
        return m_DlssReady;
    }

    std::string Runtime::DlssRefusal() const
    {
        const std::scoped_lock lock( m_Mutex );
        return m_Refusal;
    }

    void Runtime::Refuse( std::string reason )
    {
        LOG_WARN( "[Streamline] DLSS is not offered: {}", reason );
        m_Refusal       = std::move( reason );
        m_DlssCandidate = false;
        m_DlssReady     = false;
    }

    std::vector<const char*> Runtime::InstanceExtensions()
    {
        const std::scoped_lock      lock( m_Mutex );
        const std::filesystem::path directory  = ExecutableDirectory();
        const std::filesystem::path interposer = directory / kInterposer;
        if ( !std::filesystem::exists( interposer ) )
        {
            Refuse( std::format( "{} is not next to the executable", interposer.string() ) );
            return {};
        }
        // Streamline's integration rule: never load an interposer whose NVIDIA signature does not verify (a
        // replaced DLL is code run inside this process).
        if ( !sl::security::verifyEmbeddedSignature( interposer.c_str() ) )
        {
            Refuse( std::format( "{} carries no valid NVIDIA signature", interposer.string() ) );
            return {};
        }
        auto library    = std::make_unique<Library>();
        library->Module = LoadLibraryW( interposer.c_str() );
        if ( library->Module == nullptr )
        {
            Refuse( std::format( "LoadLibrary({}) failed with Win32 error {}", interposer.string(),
                                 GetLastError() ) );
            return {};
        }
        for ( const auto& resolved :
              { library->Resolve( library->Init, "slInit" ), library->Resolve( library->Shutdown, "slShutdown" ),
                library->Resolve( library->IsFeatureSupported, "slIsFeatureSupported" ),
                library->Resolve( library->GetFeatureRequirements, "slGetFeatureRequirements" ),
                library->Resolve( library->SetVulkanInfo, "slSetVulkanInfo" ),
                library->Resolve( library->GetFeatureFunction, "slGetFeatureFunction" ),
                library->Resolve( library->GetNewFrameToken, "slGetNewFrameToken" ),
                library->Resolve( library->SetConstants, "slSetConstants" ),
                library->Resolve( library->EvaluateFeature, "slEvaluateFeature" ),
                library->Resolve( library->FreeResources, "slFreeResources" ) } )
            if ( !resolved )
            {
                Refuse( resolved.GetError() );
                return {};
            }

        const std::wstring                  pluginDirectory = directory.wstring();
        const std::array<const wchar_t*, 1> pluginPaths{ pluginDirectory.c_str() };
        const std::array<sl::Feature, 1>    features{ sl::kFeatureDLSS };
        sl::Preferences                     preferences;
        preferences.showConsole        = false;
        preferences.logLevel           = sl::LogLevel::eDefault;
        preferences.logMessageCallback = LogFromStreamline;
        preferences.pathsToPlugins     = const_cast<const wchar_t**>( pluginPaths.data() );
        preferences.numPathsToPlugins  = static_cast<uint32_t>( pluginPaths.size() );
        // Manual hooking: the engine keeps the Vulkan loader it links and creates the instance and device itself.
        // No OTA: the DLSS runtime that runs is the one this build ships (ThirdParty/Streamline/VERSION), not one
        // downloaded at run time. The engine's passes bind their own state, so command-list state tracking is off.
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
        preferences.featuresToLoad    = features.data();
        preferences.numFeaturesToLoad = static_cast<uint32_t>( features.size() );
        preferences.engine            = sl::EngineType::eCustom;
        preferences.engineVersion     = kEngineVersion;
        preferences.projectId         = kProjectId;
        preferences.renderAPI         = sl::RenderAPI::eVulkan;
        if ( const sl::Result result = library->Init( preferences, sl::kSDKVersion ); result != sl::Result::eOk )
        {
            Refuse( std::format( "slInit failed: {}", ResultName( result ) ) );
            return {};
        }
        m_Library     = std::move( library );
        m_Initialized = true;

        sl::FeatureRequirements requirements;
        if ( const sl::Result result = m_Library->GetFeatureRequirements( sl::kFeatureDLSS, requirements );
             result != sl::Result::eOk )
        {
            Refuse( std::format( "slGetFeatureRequirements(DLSS) failed: {}", ResultName( result ) ) );
            return {};
        }
        if ( ( static_cast<uint32_t>( requirements.flags ) &
               static_cast<uint32_t>( sl::FeatureRequirementFlags::eVulkanSupported ) ) == 0 )
        {
            Refuse( "this Streamline build's DLSS plugin does not support Vulkan" );
            return {};
        }
        const auto system = vkb::SystemInfo::get_system_info( vkGetInstanceProcAddr );
        if ( !system )
        {
            Refuse( std::format( "the Vulkan loader could not list instance extensions: {}",
                                 system.error().message() ) );
            return {};
        }
        std::vector<const char*> extensions;
        for ( uint32_t i = 0; i < requirements.vkNumInstanceExtensions; ++i )
        {
            const char* name = requirements.vkInstanceExtensions[i];
            if ( !system.value().is_extension_available( name ) )
            {
                Refuse( std::format( "DLSS needs the instance extension {}, which the loader does not offer",
                                     name ) );
                return {};
            }
            m_InstanceExtensionNames.emplace_back( name );
        }
        for ( const std::string& name : m_InstanceExtensionNames )
            extensions.push_back( name.c_str() );
        m_DlssCandidate = true; // until the device says otherwise
        return extensions;
    }

    void Runtime::OnPhysicalDeviceSelected( VkInstance /*instance*/, vkb::PhysicalDevice& device )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_Initialized || !m_DlssCandidate )
            return;
        if ( device.properties.vendorID != kVendorNvidia )
        {
            Refuse( std::format( "'{}' is not an NVIDIA device (vendor 0x{:04X})", device.properties.deviceName,
                                 device.properties.vendorID ) );
            return;
        }

        sl::AdapterInfo adapter;
        adapter.vkPhysicalDevice = device.physical_device;
        sl::FeatureRequirements requirements;
        const sl::Result requirementsResult = m_Library->GetFeatureRequirements( sl::kFeatureDLSS, requirements );
        if ( const sl::Result result = m_Library->IsFeatureSupported( sl::kFeatureDLSS, adapter );
             result != sl::Result::eOk )
        {
            const bool known = requirementsResult == sl::Result::eOk;
            Refuse( std::format(
                 "slIsFeatureSupported(DLSS) on '{}': {} (driver {}.{}.{} detected, {}.{}.{} required)",
                 device.properties.deviceName, ResultName( result ),
                 known ? requirements.driverVersionDetected.major : 0,
                 known ? requirements.driverVersionDetected.minor : 0,
                 known ? requirements.driverVersionDetected.build : 0,
                 known ? requirements.driverVersionRequired.major : 0,
                 known ? requirements.driverVersionRequired.minor : 0,
                 known ? requirements.driverVersionRequired.build : 0 ) );
            return;
        }
        if ( requirementsResult != sl::Result::eOk )
        {
            Refuse( std::format( "slGetFeatureRequirements(DLSS) failed: {}", ResultName( requirementsResult ) ) );
            return;
        }
        // The engine creates one queue per family it uses; a feature asking for queues of its own would need the
        // device created differently, which DLSS Super Resolution does not.
        if ( requirements.vkNumComputeQueuesRequired != 0 || requirements.vkNumGraphicsQueuesRequired != 0 ||
             requirements.vkNumOpticalFlowQueuesRequired != 0 )
        {
            Refuse(
                 std::format( "DLSS asks for {} compute, {} graphics and {} optical-flow queues of its own; the "
                              "engine's device creates none for it",
                              requirements.vkNumComputeQueuesRequired, requirements.vkNumGraphicsQueuesRequired,
                              requirements.vkNumOpticalFlowQueuesRequired ) );
            return;
        }

        // Everything is checked before anything is enabled: a refusal leaves the device exactly as the engine
        // asked for it.
        for ( uint32_t i = 0; i < requirements.vkNumDeviceExtensions; ++i )
            if ( !device.is_extension_present( requirements.vkDeviceExtensions[i] ) )
            {
                Refuse( std::format( "'{}' lacks the device extension {} DLSS needs", device.properties.deviceName,
                                     requirements.vkDeviceExtensions[i] ) );
                return;
            }
        VkPhysicalDeviceVulkan12Features features12{ .sType =
                                                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features features13{ .sType =
                                                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        if ( const auto set = SetFeatures( kFeatures12, requirements.vkFeatures12, requirements.vkNumFeatures12,
                                           features12, "1.2" );
             !set )
        {
            Refuse( set.GetError() );
            return;
        }
        if ( const auto set = SetFeatures( kFeatures13, requirements.vkFeatures13, requirements.vkNumFeatures13,
                                           features13, "1.3" );
             !set )
        {
            Refuse( set.GetError() );
            return;
        }
        if ( ( requirements.vkNumFeatures12 != 0 && !device.are_extension_features_present( features12 ) ) ||
             ( requirements.vkNumFeatures13 != 0 && !device.are_extension_features_present( features13 ) ) )
        {
            Refuse(
                 std::format( "'{}' lacks a Vulkan 1.2/1.3 feature DLSS needs", device.properties.deviceName ) );
            return;
        }

        for ( uint32_t i = 0; i < requirements.vkNumDeviceExtensions; ++i )
            device.enable_extension_if_present( requirements.vkDeviceExtensions[i] );
        if ( requirements.vkNumFeatures12 != 0 )
            device.enable_extension_features_if_present( features12 );
        if ( requirements.vkNumFeatures13 != 0 )
            device.enable_extension_features_if_present( features13 );
        LOG_INFO( "[Streamline] DLSS requirements enabled on '{}' ({} device extensions, {} + {} features)",
                  device.properties.deviceName, requirements.vkNumDeviceExtensions, requirements.vkNumFeatures12,
                  requirements.vkNumFeatures13 );
    }

    void Runtime::AmendCatalogProbe( API::Vulkan::CatalogProbe& probe ) const
    {
        const std::scoped_lock lock( m_Mutex );
        probe.DlssAvailable = m_DlssCandidate;
    }

    void Runtime::OnDeviceCreated( const API::Vulkan::VulkanDeviceHandles& handles )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_Initialized || !m_DlssCandidate )
            return;
        sl::VulkanInfo info;
        info.device         = handles.Device;
        info.instance       = handles.Instance;
        info.physicalDevice = handles.PhysicalDevice;
        // Where Streamline would create queues of its own (it creates none for DLSS Super Resolution: checked in
        // OnPhysicalDeviceSelected): after the engine's in each family.
        info.computeQueueFamily  = handles.ComputeFamily;
        info.computeQueueIndex   = handles.ComputeQueuesCreated;
        info.graphicsQueueFamily = handles.GraphicsFamily;
        info.graphicsQueueIndex  = handles.GraphicsQueuesCreated;
        if ( const sl::Result result = m_Library->SetVulkanInfo( info ); result != sl::Result::eOk )
        {
            LOG_ERROR( "[Streamline] slSetVulkanInfo failed: {}", ResultName( result ) );
            Refuse( std::format( "slSetVulkanInfo failed: {}", ResultName( result ) ) );
            return;
        }
        void*            getOptimal = nullptr;
        void*            setOptions = nullptr;
        const sl::Result optimalResult =
             m_Library->GetFeatureFunction( sl::kFeatureDLSS, "slDLSSGetOptimalSettings", getOptimal );
        const sl::Result optionsResult =
             m_Library->GetFeatureFunction( sl::kFeatureDLSS, "slDLSSSetOptions", setOptions );
        if ( optimalResult != sl::Result::eOk || optionsResult != sl::Result::eOk )
        {
            Refuse(
                 std::format( "the DLSS plugin's entry points: slDLSSGetOptimalSettings {}, slDLSSSetOptions {}",
                              ResultName( optimalResult ), ResultName( optionsResult ) ) );
            return;
        }
        m_Library->GetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>( getOptimal );
        m_Library->SetOptions         = reinterpret_cast<PFun_slDLSSSetOptions*>( setOptions );
        m_DlssReady                   = true;
        m_Refusal.clear();
        LOG_INFO( "[Streamline] DLSS Super Resolution ready" );
    }

    void Runtime::OnDeviceDestroying()
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_Initialized )
            return;
        for ( const auto& [key, feature] : m_Features )
            if ( const sl::Result result = m_Library->FreeResources( sl::kFeatureDLSS, sl::ViewportHandle( key ) );
                 result != sl::Result::eOk )
                LOG_ERROR( "[Streamline] slFreeResources(DLSS, view {}) at device destruction: {}", key,
                           ResultName( result ) );
        m_Features.clear();
        if ( const sl::Result result = m_Library->Shutdown(); result != sl::Result::eOk )
            LOG_ERROR( "[Streamline] slShutdown: {}", ResultName( result ) );
        m_Library.reset();
        m_Initialized   = false;
        m_DlssCandidate = false;
        m_DlssReady     = false;
        m_Token         = nullptr;
        m_TokenFrame    = UINT64_MAX;
    }

    Common::ResultStr<DlssRenderRange> Runtime::RenderRange( const DlssMode mode, const uint32_t outputWidth,
                                                             const uint32_t outputHeight ) const
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_DlssReady )
            return Common::MakeFormattedError<DlssRenderRange>( "DLSS is not ready: {}", m_Refusal );
        sl::DLSSOptions options;
        options.mode         = ToSl( mode );
        options.outputWidth  = outputWidth;
        options.outputHeight = outputHeight;
        sl::DLSSOptimalSettings settings;
        if ( const sl::Result result = m_Library->GetOptimalSettings( options, settings );
             result != sl::Result::eOk )
            return Common::MakeFormattedError<DlssRenderRange>( "slDLSSGetOptimalSettings({}, {}x{}): {}",
                                                                DlssModeName( mode ), outputWidth, outputHeight,
                                                                ResultName( result ) );
        return Common::MakeSuccess( DlssRenderRange{ .OptimalWidth  = settings.optimalRenderWidth,
                                                     .OptimalHeight = settings.optimalRenderHeight,
                                                     .MinWidth      = settings.renderWidthMin,
                                                     .MinHeight     = settings.renderHeightMin,
                                                     .MaxWidth      = settings.renderWidthMax,
                                                     .MaxHeight     = settings.renderHeightMax } );
    }

    void Runtime::FreeIdleFeatures( const uint64_t engineFrame, const uint32_t framesToKeep )
    {
        for ( auto it = m_Features.begin(); it != m_Features.end(); )
        {
            // Older than every frame still in flight: the GPU has finished every command that used it.
            if ( engineFrame > it->second.LastFrame + framesToKeep )
            {
                if ( const sl::Result result =
                          m_Library->FreeResources( sl::kFeatureDLSS, sl::ViewportHandle( it->first ) );
                     result != sl::Result::eOk )
                    LOG_ERROR( "[Streamline] slFreeResources(DLSS, view {}): {}", it->first,
                               ResultName( result ) );
                it = m_Features.erase( it );
            }
            else
                ++it;
        }
    }

    Common::BoolResultStr Runtime::Evaluate( VkCommandBuffer commandBuffer, const DlssEvaluation& input )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_DlssReady )
            return Common::MakeFormattedError( "DLSS evaluate: DLSS is not ready: {}", m_Refusal );
        if ( input.ViewKey == 0 )
            return Common::MakeError( "DLSS evaluate: view key 0 (a view without a SceneViewState)" );

        if ( m_TokenFrame != input.EngineFrame )
        {
            sl::FrameToken* token = nullptr;
            const auto      index = static_cast<uint32_t>( input.EngineFrame );
            if ( const sl::Result result = m_Library->GetNewFrameToken( token, &index );
                 result != sl::Result::eOk )
                return Common::MakeFormattedError( "slGetNewFrameToken(frame {}): {}", input.EngineFrame,
                                                   ResultName( result ) );
            m_Token      = token;
            m_TokenFrame = input.EngineFrame;
        }
        const sl::FrameToken& token = *static_cast<sl::FrameToken*>( m_Token );
        FreeIdleFeatures( input.EngineFrame, input.FramesToKeep );

        const sl::ViewportHandle viewport( input.ViewKey );
        const auto [entry, created] = m_Features.try_emplace( input.ViewKey );
        ViewFeature& feature        = entry->second;
        if ( created || feature.Mode != input.Mode || feature.OutputWidth != input.OutputWidth ||
             feature.OutputHeight != input.OutputHeight )
        {
            sl::DLSSOptions options;
            options.mode            = ToSl( input.Mode );
            options.outputWidth     = input.OutputWidth;
            options.outputHeight    = input.OutputHeight;
            options.colorBuffersHDR = sl::Boolean::eTrue;
            // The engine's exposure texture holds the adapted LUMINANCE, and the key that turns it into an
            // exposure lives in the tonemapper's settings (and auto exposure may be off): DLSS measures its own
            // exposure instead of being handed a value that is not one.
            options.useAutoExposure = sl::Boolean::eTrue;
            if ( const sl::Result result = m_Library->SetOptions( viewport, options ); result != sl::Result::eOk )
            {
                m_Features.erase( entry );
                return Common::MakeFormattedError( "slDLSSSetOptions(view {}, {}, {}x{}): {}", input.ViewKey,
                                                   DlssModeName( input.Mode ), input.OutputWidth,
                                                   input.OutputHeight, ResultName( result ) );
            }
            feature.Mode         = input.Mode;
            feature.OutputWidth  = input.OutputWidth;
            feature.OutputHeight = input.OutputHeight;
        }
        feature.LastFrame = input.EngineFrame;

        sl::Constants constants;
        constants.cameraViewToClip       = ToSl( input.ViewToClip );
        constants.clipToCameraView       = ToSl( input.ClipToView );
        constants.clipToLensClip         = ToSl( glm::mat4( 1.0f ) );
        constants.clipToPrevClip         = ToSl( input.ClipToPrevClip );
        constants.prevClipToClip         = ToSl( input.PrevClipToClip );
        constants.jitterOffset           = { input.JitterPixels.x, input.JitterPixels.y };
        constants.mvecScale              = { input.MotionVectorScale.x, input.MotionVectorScale.y };
        constants.cameraPinholeOffset    = { 0.0f, 0.0f };
        constants.cameraPos              = ToSl( input.CameraPosition );
        constants.cameraUp               = ToSl( input.CameraUp );
        constants.cameraRight            = ToSl( input.CameraRight );
        constants.cameraFwd              = ToSl( input.CameraForward );
        constants.cameraNear             = input.NearPlane;
        constants.cameraFar              = input.FarPlane;
        constants.cameraFOV              = input.VerticalFov;
        constants.cameraAspectRatio      = input.AspectRatio;
        constants.depthInverted          = sl::Boolean::eTrue; // reversed-Z
        constants.cameraMotionIncluded   = sl::Boolean::eTrue; // velocity is the whole screen motion
        constants.motionVectors3D        = sl::Boolean::eFalse;
        constants.reset                  = input.Reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eFalse;
        constants.motionVectorsDilated   = sl::Boolean::eFalse;
        constants.motionVectorsJittered  = sl::Boolean::eFalse; // velocity never carries the jitter
        if ( const sl::Result result = m_Library->SetConstants( constants, token, viewport );
             result != sl::Result::eOk )
            return Common::MakeFormattedError( "slSetConstants(view {}): {}", input.ViewKey,
                                               ResultName( result ) );

        // Tagged locally, for this evaluation only: the images are graph resources valid for this pass, so nothing
        // is copied and nothing outlives the call.
        sl::Resource                   color  = ToSl( input.Color );
        sl::Resource                   depth  = ToSl( input.Depth );
        sl::Resource                   motion = ToSl( input.MotionVectors );
        sl::Resource                   output = ToSl( input.Output );
        const sl::Extent               render{ 0, 0, input.Color.Width, input.Color.Height };
        const sl::Extent               full{ 0, 0, input.Output.Width, input.Output.Height };
        std::array<sl::ResourceTag, 4> tags{
             sl::ResourceTag( &color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent,
                              &render ),
             sl::ResourceTag( &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &render ),
             sl::ResourceTag( &motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent,
                              &render ),
             sl::ResourceTag( &output, sl::kBufferTypeScalingOutputColor,
                              sl::ResourceLifecycle::eValidUntilPresent, &full ),
        };
        const std::array<const sl::BaseStructure*, 5> inputs{ &viewport, &tags[0], &tags[1], &tags[2], &tags[3] };
        if ( const sl::Result result = m_Library->EvaluateFeature(
                  sl::kFeatureDLSS, token, const_cast<const sl::BaseStructure**>( inputs.data() ),
                  static_cast<uint32_t>( inputs.size() ), commandBuffer );
             result != sl::Result::eOk )
            return Common::MakeFormattedError( "slEvaluateFeature(DLSS, view {}, {} {}x{} -> {}x{}): {}",
                                               input.ViewKey, DlssModeName( input.Mode ), input.Color.Width,
                                               input.Color.Height, input.Output.Width, input.Output.Height,
                                               ResultName( result ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::Streamline
