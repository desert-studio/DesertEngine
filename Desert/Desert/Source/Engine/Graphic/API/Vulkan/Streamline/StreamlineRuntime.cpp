#include <Engine/Graphic/API/Vulkan/Streamline/StreamlineRuntime.hpp>

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>
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
#include <vector>

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

        sl::DLSSPreset ToSl( const Common::Scalability::DlssPreset preset )
        {
            using Common::Scalability::DlssPreset;
            switch ( preset )
            {
                case DlssPreset::Default:
                    return sl::DLSSPreset::eDefault;
                case DlssPreset::J:
                    return sl::DLSSPreset::ePresetJ;
                case DlssPreset::K:
                    return sl::DLSSPreset::ePresetK;
                case DlssPreset::L:
                    return sl::DLSSPreset::ePresetL;
                case DlssPreset::M:
                    return sl::DLSSPreset::ePresetM;
            }
            return sl::DLSSPreset::eDefault;
        }

        // sl::DLSSOptions carries one preset per mode; only the running mode's is read, so the view's preset is
        // written into that mode's slot and the rest stay Default.
        void SetPreset( sl::DLSSOptions& options, const DlssMode mode, const sl::DLSSPreset preset )
        {
            switch ( mode )
            {
                case DlssMode::UltraPerformance:
                    options.ultraPerformancePreset = preset;
                    return;
                case DlssMode::Performance:
                    options.performancePreset = preset;
                    return;
                case DlssMode::Balanced:
                    options.balancedPreset = preset;
                    return;
                case DlssMode::Quality:
                    options.qualityPreset = preset;
                    return;
                case DlssMode::UltraQuality:
                    options.ultraQualityPreset = preset;
                    return;
                case DlssMode::DLAA:
                    options.dlaaPreset = preset;
                    return;
            }
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
             FeatureBit13{ "privateData", &VkPhysicalDeviceVulkan13Features::privateData },
        };

        // A device extension Streamline lists whose whole use is a feature the device's core version carries:
        // on such a device the feature bit goes into the engine's one 1.2/1.3 chain and the extension name is not
        // enabled (UE's Vulkan RHI does the same with its promoted-extension table). VK_EXT_buffer_device_address
        // is the one that MUST be dropped: it is the superseded form of the KHR extension 1.2 promoted, and
        // naming it beside Vulkan12Features::bufferDeviceAddress (or beside VK_KHR_buffer_device_address) is
        // VUID-VkDeviceCreateInfo-pNext-04748 / -ppEnabledExtensionNames-03328 - what sl.dlss's list did on an
        // RTX 3070 Ti.
        struct CoreExtension
        {
            std::string_view Name;
            uint32_t         CoreVersion;
            VkBool32 VkPhysicalDeviceVulkan12Features::*Feature12 = nullptr;
            VkBool32 VkPhysicalDeviceVulkan13Features::*Feature13 = nullptr;
        };
        constexpr std::array kCoreExtensions{
             CoreExtension{ VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_API_VERSION_1_2,
                            &VkPhysicalDeviceVulkan12Features::bufferDeviceAddress },
             CoreExtension{ VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_API_VERSION_1_2,
                            &VkPhysicalDeviceVulkan12Features::bufferDeviceAddress },
             CoreExtension{ VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_API_VERSION_1_2,
                            &VkPhysicalDeviceVulkan12Features::timelineSemaphore },
             CoreExtension{ VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, VK_API_VERSION_1_2,
                            &VkPhysicalDeviceVulkan12Features::descriptorIndexing },
             CoreExtension{ VK_EXT_PRIVATE_DATA_EXTENSION_NAME, VK_API_VERSION_1_3, nullptr,
                            &VkPhysicalDeviceVulkan13Features::privateData },
             CoreExtension{ VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_API_VERSION_1_3, nullptr,
                            &VkPhysicalDeviceVulkan13Features::synchronization2 },
             CoreExtension{ VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_API_VERSION_1_3, nullptr,
                            &VkPhysicalDeviceVulkan13Features::dynamicRendering },
             CoreExtension{ VK_KHR_MAINTENANCE_4_EXTENSION_NAME, VK_API_VERSION_1_3, nullptr,
                            &VkPhysicalDeviceVulkan13Features::maintenance4 },
             CoreExtension{ VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_API_VERSION_1_3, nullptr,
                            &VkPhysicalDeviceVulkan13Features::subgroupSizeControl },
        };

        // The row that turns @p extension into a core feature on a device of version @p api; null when the
        // extension is enabled by name.
        const CoreExtension* CoreFormOf( const std::string_view extension, const uint32_t api )
        {
            for ( const CoreExtension& row : kCoreExtensions )
                if ( row.Name == extension && api >= row.CoreVersion )
                    return &row;
            return nullptr;
        }

        template <typename Table, typename Struct>
        Common::BoolResultStr SetFeatures( const Table& table, const char* const* names, const uint32_t count,
                                           Struct& features, const std::string_view version )
        {
            for ( uint32_t i = 0; i < count; ++i )
            {
                const std::string_view name = names[i];
                const auto             row =
                     std::find_if( table.begin(), table.end(), [&]( const auto& r ) { return r.Name == name; } );
                if ( row == table.end() )
                    return Common::MakeFormattedError(
                         "Streamline asks for the Vulkan {} feature '{}', which this engine cannot name", version,
                         name );
                features.*( row->Member ) = VK_TRUE;
            }
            return Common::MakeSuccess( true );
        }

        // The first feature @p wanted asks for that @p supported does not have; empty when every one is there.
        template <typename Table, typename Struct>
        std::string_view FirstMissingFeature( const Table& table, const Struct& wanted, const Struct& supported )
        {
            for ( const auto& row : table )
                if ( wanted.*( row.Member ) == VK_TRUE && supported.*( row.Member ) != VK_TRUE )
                    return row.Name;
            return {};
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
        PFun_slDLSSGetState*           GetState           = nullptr;

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

    API::Vulkan::InstanceRequirements Runtime::InstanceExtensions()
    {
        const std::scoped_lock      lock( m_Mutex );
        const std::filesystem::path directory  = ExecutableDirectory();
        const std::filesystem::path interposer = directory / kInterposer;
        if ( !std::filesystem::exists( interposer ) )
        {
            // The binaries are downloaded, not committed (ThirdParty/Streamline/manifest.json): a build made before
            // the fetch, or a package from a machine without it, lands here and says how to fix it.
            Refuse( std::format( "{} is not next to the executable (not fetched? run `python "
                                 "scripts/Windows/FetchVendorBinaries.py`, then rebuild)",
                                 interposer.string() ) );
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
        API::Vulkan::InstanceRequirements instance;
        // NGX (DLSS) records its work through the driver directly with the handles slEvaluateFeature is given:
        // a validation layer that wraps handles would hand it ids the driver dereferences (the access violation
        // in nvoglv64 under slEvaluateFeature, Result::eErrorExceptionHandler).
        instance.PassesHandlesBesideTheLayers = true;
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
            instance.Extensions.push_back( name.c_str() );
        m_DlssCandidate = true; // until the device says otherwise
        return instance;
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
        const uint32_t api = API::Vulkan::UsedApiVersion( device.properties.apiVersion );
        // Streamline's own Vulkan layer keeps per-object state in private data slots (vkCreatePrivateDataSlot,
        // the 1.3 entry point) and slGetFeatureRequirements does not list the feature: on an RTX 3070 Ti it
        // reported 0 Vulkan 1.3 features and sl.common then failed VUID-vkCreatePrivateDataSlot-privateData-04564.
        if ( api < VK_API_VERSION_1_3 )
        {
            Refuse( std::format( "'{}' is a Vulkan {} device; Streamline creates private data slots through the "
                                 "Vulkan 1.3 entry point",
                                 device.properties.deviceName, API::Vulkan::FormatApiVersion( api ) ) );
            return;
        }
        VkPhysicalDeviceVulkan12Features features12{ .sType =
                                                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features features13{ .sType =
                                                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.privateData = VK_TRUE;
        std::vector<const char*> extensionsByName;
        for ( uint32_t i = 0; i < requirements.vkNumDeviceExtensions; ++i )
        {
            const char* name = requirements.vkDeviceExtensions[i];
            if ( const CoreExtension* core = CoreFormOf( name, api ) )
            {
                if ( core->Feature12 != nullptr )
                    features12.*( core->Feature12 ) = VK_TRUE;
                if ( core->Feature13 != nullptr )
                    features13.*( core->Feature13 ) = VK_TRUE;
                continue;
            }
            if ( !device.is_extension_present( name ) )
            {
                Refuse( std::format( "'{}' lacks the device extension {} DLSS needs", device.properties.deviceName,
                                     name ) );
                return;
            }
            extensionsByName.push_back( name );
        }
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
        // What the DEVICE supports, asked of the device (vkGetPhysicalDeviceFeatures2). vkb's
        // are_extension_features_present is not that question: it matches against the features already ENABLED
        // on the selection, so it refused DLSS on every GPU whose 1.2/1.3 struct the engine had not enabled yet.
        VkPhysicalDeviceVulkan13Features supported13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features supported12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                      .pNext = &supported13 };
        VkPhysicalDeviceFeatures2 supported{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                             .pNext = &supported12 };
        vkGetPhysicalDeviceFeatures2( device.physical_device, &supported );
        if ( const std::string_view missing = FirstMissingFeature( kFeatures12, features12, supported12 );
             !missing.empty() )
        {
            Refuse( std::format( "'{}' lacks the Vulkan 1.2 feature {} DLSS needs", device.properties.deviceName,
                                 missing ) );
            return;
        }
        if ( const std::string_view missing = FirstMissingFeature( kFeatures13, features13, supported13 );
             !missing.empty() )
        {
            Refuse( std::format( "'{}' lacks the Vulkan 1.3 feature {} DLSS needs", device.properties.deviceName,
                                 missing ) );
            return;
        }

        for ( const char* name : extensionsByName )
            device.enable_extension_if_present( name );
        // Merged by vkb into the engine's own VkPhysicalDeviceVulkan12/13Features (DeviceCapsProbe.cpp enables
        // its bits into the same two structs): one chain, no per-extension feature struct beside it.
        device.enable_extension_features_if_present( features12 );
        device.enable_extension_features_if_present( features13 );
        LOG_INFO( "[Streamline] DLSS requirements enabled on '{}': {} of {} device extensions by name (the rest "
                  "as Vulkan {} core features), {} + {} features asked, privateData for Streamline's layer",
                  device.properties.deviceName, extensionsByName.size(), requirements.vkNumDeviceExtensions,
                  API::Vulkan::FormatApiVersion( api ), requirements.vkNumFeatures12, requirements.vkNumFeatures13 );
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
        void*            getState   = nullptr;
        const sl::Result optimalResult =
             m_Library->GetFeatureFunction( sl::kFeatureDLSS, "slDLSSGetOptimalSettings", getOptimal );
        const sl::Result optionsResult =
             m_Library->GetFeatureFunction( sl::kFeatureDLSS, "slDLSSSetOptions", setOptions );
        const sl::Result stateResult =
             m_Library->GetFeatureFunction( sl::kFeatureDLSS, "slDLSSGetState", getState );
        if ( optimalResult != sl::Result::eOk || optionsResult != sl::Result::eOk ||
             stateResult != sl::Result::eOk )
        {
            Refuse( std::format(
                 "the DLSS plugin's entry points: slDLSSGetOptimalSettings {}, slDLSSSetOptions {}, "
                 "slDLSSGetState {}",
                 ResultName( optimalResult ), ResultName( optionsResult ), ResultName( stateResult ) ) );
            return;
        }
        m_Library->GetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>( getOptimal );
        m_Library->SetOptions         = reinterpret_cast<PFun_slDLSSSetOptions*>( setOptions );
        m_Library->GetState           = reinterpret_cast<PFun_slDLSSGetState*>( getState );
        m_DlssReady                   = true;
        m_Refusal.clear();
        LOG_INFO( "[Streamline] DLSS Super Resolution ready" );
    }

    void Runtime::OnDeviceDestroying()
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_Initialized )
            return;
        // Views still holding a viewport (their histories are dropped after the device): free them here, before
        // slShutdown; their later ReleaseViewport finds the viewport gone and does nothing.
        for ( const uint32_t viewport : m_Viewports )
            if ( const sl::Result result =
                      m_Library->FreeResources( sl::kFeatureDLSS, sl::ViewportHandle( viewport ) );
                 result != sl::Result::eOk )
                LOG_ERROR( "[Streamline] slFreeResources(DLSS, viewport {}) at device destruction: {}", viewport,
                           ResultName( result ) );
        m_Viewports.clear();
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

    uint32_t Runtime::AcquireViewport()
    {
        const std::scoped_lock lock( m_Mutex );
        // 0 is skipped on wrap: no viewport is 0. Viewports are released with their views, so a wrap never meets
        // a live one in practice; one still held is skipped all the same.
        uint32_t viewport = 0;
        while ( viewport == 0 || m_Viewports.contains( viewport ) )
            viewport = m_NextViewport++;
        m_Viewports.insert( viewport );
        return viewport;
    }

    void Runtime::ReleaseViewport( const uint32_t viewport )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( m_Viewports.erase( viewport ) == 0 || !m_Initialized )
            return; // freed at device destruction already
        // eErrorInvalidParameter only means the viewport never evaluated: there was no feature to free.
        if ( const sl::Result result =
                  m_Library->FreeResources( sl::kFeatureDLSS, sl::ViewportHandle( viewport ) );
             result != sl::Result::eOk && result != sl::Result::eErrorInvalidParameter )
            LOG_ERROR( "[Streamline] slFreeResources(DLSS, viewport {}): {}", viewport, ResultName( result ) );
    }

    Common::ResultStr<uint64_t> Runtime::SetOptions( const uint32_t viewport, const DlssOptions& input )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_DlssReady )
            return Common::MakeFormattedError<uint64_t>( "DLSS options: DLSS is not ready: {}", m_Refusal );
        if ( !m_Viewports.contains( viewport ) )
            return Common::MakeFormattedError<uint64_t>( "DLSS options: viewport {} was not acquired", viewport );

        sl::DLSSOptions options;
        options.mode            = ToSl( input.Mode );
        options.outputWidth     = input.OutputWidth;
        options.outputHeight    = input.OutputHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        // The engine hands its own exposure (the tagged kBufferTypeExposure image, UpscalerExposure: the same
        // multiplier FSR and the engine's TAA use), as UE hands DLSS its eye-adaptation exposure.
        options.useAutoExposure = sl::Boolean::eFalse;
        SetPreset( options, input.Mode, ToSl( input.Preset ) );
        const sl::ViewportHandle handle( viewport );
        if ( const sl::Result result = m_Library->SetOptions( handle, options ); result != sl::Result::eOk )
            return Common::MakeFormattedError<uint64_t>(
                 "slDLSSSetOptions(viewport {}, {}, preset {}, {}x{}): {}", viewport, DlssModeName( input.Mode ),
                 Common::Scalability::kDlssPresetNames[static_cast<std::size_t>( input.Preset )],
                 input.OutputWidth, input.OutputHeight, ResultName( result ) );
        sl::DLSSState state;
        if ( const sl::Result result = m_Library->GetState( handle, state ); result != sl::Result::eOk )
            return Common::MakeFormattedError<uint64_t>( "slDLSSGetState(viewport {}): {}", viewport,
                                                         ResultName( result ) );
        return Common::MakeSuccess( state.estimatedVRAMUsageInBytes );
    }

    Common::BoolResultStr Runtime::Evaluate( VkCommandBuffer commandBuffer, const DlssEvaluation& input )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_DlssReady )
            return Common::MakeFormattedError( "DLSS evaluate: DLSS is not ready: {}", m_Refusal );
        if ( !m_Viewports.contains( input.Viewport ) )
            return Common::MakeFormattedError( "DLSS evaluate: viewport {} was not acquired", input.Viewport );
        // Every tagged image holds the extent it is tagged with: a smaller one is a view the graph did not size
        // for this split, and DLSS would read or write outside it.
        const auto holds = []( const DlssImage& image, const uint32_t width, const uint32_t height )
        { return image.Width >= width && image.Height >= height; };
        if ( input.RenderWidth == 0 || input.RenderHeight == 0 || input.OutputWidth == 0 || input.OutputHeight == 0 ||
             !holds( input.Color, input.RenderWidth, input.RenderHeight ) ||
             !holds( input.Depth, input.RenderWidth, input.RenderHeight ) ||
             !holds( input.MotionVectors, input.RenderWidth, input.RenderHeight ) ||
             !holds( input.Output, input.OutputWidth, input.OutputHeight ) )
            return Common::MakeFormattedError(
                 "DLSS evaluate (viewport {}): the view's split {}x{} -> {}x{} does not fit its images (color {}x{}, "
                 "depth {}x{}, motion {}x{}, output {}x{})",
                 input.Viewport, input.RenderWidth, input.RenderHeight, input.OutputWidth, input.OutputHeight,
                 input.Color.Width, input.Color.Height, input.Depth.Width, input.Depth.Height,
                 input.MotionVectors.Width, input.MotionVectors.Height, input.Output.Width, input.Output.Height );

        // A Streamline call that fails here has not written the output this pass promised (Result::
        // eErrorExceptionHandler: NGX faulted inside the driver), and nothing says the next frame fares better:
        // DLSS refuses itself for the device's life, with the call and its result as the reason, and the view
        // renders with the engine's method from the next frame on (TemporalMethodUnavailable).
        const auto refuseEvaluation = [this]( std::string why ) -> Common::BoolResultStr
        {
            Refuse( why );
            return Common::MakeFormattedError( "{}", why );
        };
        if ( m_TokenFrame != input.EngineFrame )
        {
            sl::FrameToken* token = nullptr;
            const auto      index = static_cast<uint32_t>( input.EngineFrame );
            if ( const sl::Result result = m_Library->GetNewFrameToken( token, &index );
                 result != sl::Result::eOk )
                return refuseEvaluation(
                     std::format( "slGetNewFrameToken(frame {}): {}", input.EngineFrame, ResultName( result ) ) );
            m_Token      = token;
            m_TokenFrame = input.EngineFrame;
        }
        const sl::FrameToken&    token = *static_cast<sl::FrameToken*>( m_Token );
        const sl::ViewportHandle viewport( input.Viewport );

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
            return refuseEvaluation(
                 std::format( "slSetConstants(viewport {}): {}", input.Viewport, ResultName( result ) ) );

        // Tagged locally, for this evaluation only: the images are graph resources valid for this pass, so nothing
        // is copied and nothing outlives the call.
        sl::Resource                   color    = ToSl( input.Color );
        sl::Resource                   depth    = ToSl( input.Depth );
        sl::Resource                   motion   = ToSl( input.MotionVectors );
        sl::Resource                   output   = ToSl( input.Output );
        sl::Resource                   exposure = ToSl( input.Exposure );
        const sl::Extent               render{ 0, 0, input.RenderWidth, input.RenderHeight };
        const sl::Extent               full{ 0, 0, input.OutputWidth, input.OutputHeight };
        const sl::Extent               texel{ 0, 0, 1, 1 };
        std::array<sl::ResourceTag, 5> tags{
             sl::ResourceTag( &color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent,
                              &render ),
             sl::ResourceTag( &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &render ),
             sl::ResourceTag( &motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent,
                              &render ),
             sl::ResourceTag( &output, sl::kBufferTypeScalingOutputColor,
                              sl::ResourceLifecycle::eValidUntilPresent, &full ),
             sl::ResourceTag( &exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent,
                              &texel ),
        };
        const std::array<const sl::BaseStructure*, 6> inputs{ &viewport, &tags[0], &tags[1],
                                                              &tags[2],  &tags[3], &tags[4] };
        if ( const sl::Result result = m_Library->EvaluateFeature(
                  sl::kFeatureDLSS, token, const_cast<const sl::BaseStructure**>( inputs.data() ),
                  static_cast<uint32_t>( inputs.size() ), commandBuffer );
             result != sl::Result::eOk )
            return refuseEvaluation( std::format( "slEvaluateFeature(DLSS, viewport {}, {} {}x{} -> {}x{}): {}",
                                                  input.Viewport, DlssModeName( input.Mode ), input.RenderWidth,
                                                  input.RenderHeight, input.OutputWidth, input.OutputHeight,
                                                  ResultName( result ) ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::Streamline
