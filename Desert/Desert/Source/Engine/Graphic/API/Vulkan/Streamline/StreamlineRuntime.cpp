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
#include <sl_dlss_d.h>
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

        sl::DLSSDPreset ToSl( const Common::Scalability::DlssRRPreset preset )
        {
            using Common::Scalability::DlssRRPreset;
            switch ( preset )
            {
                case DlssRRPreset::Default:
                    return sl::DLSSDPreset::eDefault;
                case DlssRRPreset::D:
                    return sl::DLSSDPreset::ePresetD;
                case DlssRRPreset::E:
                    return sl::DLSSDPreset::ePresetE;
            }
            return sl::DLSSDPreset::eDefault;
        }

        sl::Feature ToSl( const DlssFeature feature )
        {
            return feature == DlssFeature::RayReconstruction ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS;
        }

        // sl::DLSSOptions / sl::DLSSDOptions carry one preset per mode; only the running mode's is read, so the
        // view's preset is written into that mode's slot and the rest stay Default.
        template <typename Options, typename Preset>
        void SetPreset( Options& options, const DlssMode mode, const Preset preset )
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

    std::string_view DlssFeatureName( const DlssFeature feature )
    {
        switch ( feature )
        {
            case DlssFeature::SuperResolution:
                return "DLSS Super Resolution";
            case DlssFeature::RayReconstruction:
                return "DLSS Ray Reconstruction";
        }
        return "<unknown DlssFeature>";
    }

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
        // Ray Reconstruction's (sl.dlss_d), the same way.
        PFun_slDLSSDGetOptimalSettings* RRGetOptimalSettings = nullptr;
        PFun_slDLSSDSetOptions*         RRSetOptions         = nullptr;
        PFun_slDLSSDGetState*           RRGetState           = nullptr;
        // The interposer's presentation proxies (manual hooking), resolved once slSetVulkanInfo has handed it the
        // device: each runs the plugins' before/after hooks (sl.common's presentCommon on vkQueuePresentKHR)
        // around the driver's entry point. Hooked = all of them resolved.
        API::Vulkan::VulkanPresentationDispatch Presentation;
        bool                                    Hooked = false;

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

    Runtime::FeatureState& Runtime::StateOf( const DlssFeature feature )
    {
        return m_Features[static_cast<std::size_t>( feature )];
    }

    const Runtime::FeatureState& Runtime::StateOf( const DlssFeature feature ) const
    {
        return m_Features[static_cast<std::size_t>( feature )];
    }

    bool Runtime::AnyCandidate() const
    {
        return std::any_of( m_Features.begin(), m_Features.end(),
                            []( const FeatureState& state ) { return state.Candidate; } );
    }

    bool Runtime::Ready( const DlssFeature feature ) const
    {
        const std::scoped_lock lock( m_Mutex );
        return StateOf( feature ).Ready;
    }

    std::string Runtime::Refusal( const DlssFeature feature ) const
    {
        const std::scoped_lock lock( m_Mutex );
        return StateOf( feature ).Refusal;
    }

    void Runtime::Refuse( const DlssFeature feature, std::string reason )
    {
        LOG_WARN( "[Streamline] {} is not offered: {}", DlssFeatureName( feature ), reason );
        FeatureState& state = StateOf( feature );
        state.Refusal       = std::move( reason );
        state.Candidate     = false;
        state.Ready         = false;
    }

    void Runtime::RefuseAll( const std::string& reason )
    {
        for ( const DlssFeature feature : { DlssFeature::SuperResolution, DlssFeature::RayReconstruction } )
            Refuse( feature, reason );
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
            RefuseAll( std::format( "{} is not next to the executable (not fetched? run `python "
                                 "scripts/Windows/FetchVendorBinaries.py`, then rebuild)",
                                 interposer.string() ) );
            return {};
        }
        // Streamline's integration rule: never load an interposer whose NVIDIA signature does not verify (a
        // replaced DLL is code run inside this process).
        if ( !sl::security::verifyEmbeddedSignature( interposer.c_str() ) )
        {
            RefuseAll( std::format( "{} carries no valid NVIDIA signature", interposer.string() ) );
            return {};
        }
        auto library    = std::make_unique<Library>();
        library->Module = LoadLibraryW( interposer.c_str() );
        if ( library->Module == nullptr )
        {
            RefuseAll( std::format( "LoadLibrary({}) failed with Win32 error {}", interposer.string(),
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
                RefuseAll( resolved.GetError() );
                return {};
            }

        const std::wstring                  pluginDirectory = directory.wstring();
        const std::array<const wchar_t*, 1> pluginPaths{ pluginDirectory.c_str() };
        const std::array<sl::Feature, 2>    features{ sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
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
            RefuseAll( std::format( "slInit failed: {}", ResultName( result ) ) );
            return {};
        }
        m_Library     = std::move( library );
        m_Initialized = true;

        const auto system = vkb::SystemInfo::get_system_info( vkGetInstanceProcAddr );
        if ( !system )
        {
            RefuseAll( std::format( "the Vulkan loader could not list instance extensions: {}",
                                    system.error().message() ) );
            return {};
        }
        // Each feature states its own needs; one the loader cannot meet refuses that feature only.
        for ( const DlssFeature feature : { DlssFeature::SuperResolution, DlssFeature::RayReconstruction } )
        {
            sl::FeatureRequirements requirements;
            if ( const sl::Result result = m_Library->GetFeatureRequirements( ToSl( feature ), requirements );
                 result != sl::Result::eOk )
            {
                Refuse( feature, std::format( "slGetFeatureRequirements({}) failed: {}", DlssFeatureName( feature ),
                                              ResultName( result ) ) );
                continue;
            }
            if ( ( static_cast<uint32_t>( requirements.flags ) &
                   static_cast<uint32_t>( sl::FeatureRequirementFlags::eVulkanSupported ) ) == 0 )
            {
                Refuse( feature, "this Streamline build's plugin does not support Vulkan" );
                continue;
            }
            std::vector<std::string> names;
            bool                     available = true;
            for ( uint32_t i = 0; i < requirements.vkNumInstanceExtensions && available; ++i )
            {
                const char* name = requirements.vkInstanceExtensions[i];
                if ( !system.value().is_extension_available( name ) )
                {
                    Refuse( feature,
                            std::format( "it needs the instance extension {}, which the loader does not offer", name ) );
                    available = false;
                }
                names.emplace_back( name );
            }
            if ( !available )
                continue;
            for ( std::string& name : names )
                if ( std::find( m_InstanceExtensionNames.begin(), m_InstanceExtensionNames.end(), name ) ==
                     m_InstanceExtensionNames.end() )
                    m_InstanceExtensionNames.push_back( std::move( name ) );
            StateOf( feature ).Candidate = true; // until the device says otherwise
        }
        if ( !AnyCandidate() )
            return {};
        API::Vulkan::InstanceRequirements instance;
        // NGX (DLSS) records its work through the driver directly with the handles slEvaluateFeature is given:
        // a validation layer that wraps handles would hand it ids the driver dereferences (the access violation
        // in nvoglv64 under slEvaluateFeature, Result::eErrorExceptionHandler).
        instance.PassesHandlesBesideTheLayers = true;
        for ( const std::string& name : m_InstanceExtensionNames )
            instance.Extensions.push_back( name.c_str() );
        return instance;
    }

    void Runtime::OnPhysicalDeviceSelected( VkInstance /*instance*/, vkb::PhysicalDevice& device )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_Initialized || !AnyCandidate() )
            return;
        if ( device.properties.vendorID != kVendorNvidia )
        {
            RefuseAll( std::format( "'{}' is not an NVIDIA device (vendor 0x{:04X})", device.properties.deviceName,
                                    device.properties.vendorID ) );
            return;
        }
        const uint32_t api = API::Vulkan::UsedApiVersion( device.properties.apiVersion );
        // Streamline's own Vulkan layer keeps per-object state in private data slots (vkCreatePrivateDataSlot,
        // the 1.3 entry point) and slGetFeatureRequirements does not list the feature: on an RTX 3070 Ti it
        // reported 0 Vulkan 1.3 features and sl.common then failed VUID-vkCreatePrivateDataSlot-privateData-04564.
        if ( api < VK_API_VERSION_1_3 )
        {
            RefuseAll( std::format( "'{}' is a Vulkan {} device; Streamline creates private data slots through the "
                                    "Vulkan 1.3 entry point",
                                    device.properties.deviceName, API::Vulkan::FormatApiVersion( api ) ) );
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

        sl::AdapterInfo adapter;
        adapter.vkPhysicalDevice = device.physical_device;
        // Each feature is checked whole before anything of it is enabled: a refusal leaves the device exactly as
        // the engine and the other feature asked for it.
        for ( const DlssFeature feature : { DlssFeature::SuperResolution, DlssFeature::RayReconstruction } )
        {
            if ( !StateOf( feature ).Candidate )
                continue;
            const std::string_view  name = DlssFeatureName( feature );
            sl::FeatureRequirements requirements;
            const sl::Result requirementsResult = m_Library->GetFeatureRequirements( ToSl( feature ), requirements );
            if ( const sl::Result result = m_Library->IsFeatureSupported( ToSl( feature ), adapter );
                 result != sl::Result::eOk )
            {
                const bool known = requirementsResult == sl::Result::eOk;
                Refuse( feature,
                        std::format( "slIsFeatureSupported({}) on '{}': {} (driver {}.{}.{} detected, {}.{}.{} "
                                     "required)",
                                     name, device.properties.deviceName, ResultName( result ),
                                     known ? requirements.driverVersionDetected.major : 0,
                                     known ? requirements.driverVersionDetected.minor : 0,
                                     known ? requirements.driverVersionDetected.build : 0,
                                     known ? requirements.driverVersionRequired.major : 0,
                                     known ? requirements.driverVersionRequired.minor : 0,
                                     known ? requirements.driverVersionRequired.build : 0 ) );
                continue;
            }
            if ( requirementsResult != sl::Result::eOk )
            {
                Refuse( feature, std::format( "slGetFeatureRequirements({}) failed: {}", name,
                                              ResultName( requirementsResult ) ) );
                continue;
            }
            // The engine creates one queue per family it uses; a feature asking for queues of its own would need
            // the device created differently, which neither DLSS feature does.
            if ( requirements.vkNumComputeQueuesRequired != 0 || requirements.vkNumGraphicsQueuesRequired != 0 ||
                 requirements.vkNumOpticalFlowQueuesRequired != 0 )
            {
                Refuse( feature, std::format( "it asks for {} compute, {} graphics and {} optical-flow queues of "
                                              "its own; the engine's device creates none for it",
                                              requirements.vkNumComputeQueuesRequired,
                                              requirements.vkNumGraphicsQueuesRequired,
                                              requirements.vkNumOpticalFlowQueuesRequired ) );
                continue;
            }
            VkPhysicalDeviceVulkan12Features features12{ .sType =
                                                              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            VkPhysicalDeviceVulkan13Features features13{ .sType =
                                                              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            features13.privateData = VK_TRUE;
            std::vector<const char*> extensionsByName;
            std::string              lacking;
            for ( uint32_t i = 0; i < requirements.vkNumDeviceExtensions && lacking.empty(); ++i )
            {
                const char* extension = requirements.vkDeviceExtensions[i];
                if ( const CoreExtension* core = CoreFormOf( extension, api ) )
                {
                    if ( core->Feature12 != nullptr )
                        features12.*( core->Feature12 ) = VK_TRUE;
                    if ( core->Feature13 != nullptr )
                        features13.*( core->Feature13 ) = VK_TRUE;
                    continue;
                }
                if ( !device.is_extension_present( extension ) )
                    lacking = std::format( "'{}' lacks the device extension {} it needs", device.properties.deviceName,
                                           extension );
                extensionsByName.push_back( extension );
            }
            if ( !lacking.empty() )
            {
                Refuse( feature, lacking );
                continue;
            }
            if ( const auto set = SetFeatures( kFeatures12, requirements.vkFeatures12, requirements.vkNumFeatures12,
                                               features12, "1.2" );
                 !set )
            {
                Refuse( feature, set.GetError() );
                continue;
            }
            if ( const auto set = SetFeatures( kFeatures13, requirements.vkFeatures13, requirements.vkNumFeatures13,
                                               features13, "1.3" );
                 !set )
            {
                Refuse( feature, set.GetError() );
                continue;
            }
            if ( const std::string_view missing = FirstMissingFeature( kFeatures12, features12, supported12 );
                 !missing.empty() )
            {
                Refuse( feature, std::format( "'{}' lacks the Vulkan 1.2 feature {} it needs",
                                              device.properties.deviceName, missing ) );
                continue;
            }
            if ( const std::string_view missing = FirstMissingFeature( kFeatures13, features13, supported13 );
                 !missing.empty() )
            {
                Refuse( feature, std::format( "'{}' lacks the Vulkan 1.3 feature {} it needs",
                                              device.properties.deviceName, missing ) );
                continue;
            }

            for ( const char* extension : extensionsByName )
                device.enable_extension_if_present( extension );
            // Merged by vkb into the engine's own VkPhysicalDeviceVulkan12/13Features (DeviceCapsProbe.cpp
            // enables its bits into the same two structs, and the other DLSS feature's too): one chain, no
            // per-extension feature struct beside it.
            device.enable_extension_features_if_present( features12 );
            device.enable_extension_features_if_present( features13 );
            LOG_INFO( "[Streamline] {} requirements enabled on '{}': {} of {} device extensions by name (the rest "
                      "as Vulkan {} core features), {} + {} features asked, privateData for Streamline's layer",
                      name, device.properties.deviceName, extensionsByName.size(), requirements.vkNumDeviceExtensions,
                      API::Vulkan::FormatApiVersion( api ), requirements.vkNumFeatures12,
                      requirements.vkNumFeatures13 );
        }
    }

    void Runtime::AmendCatalogProbe( API::Vulkan::CatalogProbe& probe ) const
    {
        const std::scoped_lock lock( m_Mutex );
        probe.DlssAvailable = StateOf( DlssFeature::SuperResolution ).Candidate;
        // Ray Reconstruction is offered beside Super Resolution only: its refusal falls back to it.
        probe.DlssRayReconstructionAvailable =
             probe.DlssAvailable && StateOf( DlssFeature::RayReconstruction ).Candidate;
    }

    void Runtime::OnDeviceCreated( const API::Vulkan::VulkanDeviceHandles& handles )
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_Initialized || !AnyCandidate() )
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
            RefuseAll( std::format( "slSetVulkanInfo failed: {}", ResultName( result ) ) );
            return;
        }
        // Manual hooking (Streamline ProgrammingGuideManualHooking, Vulkan; sl_hooks.h "Mandatory - Vulkan"):
        // with eUseManualHooking the interposer intercepts nothing by itself, so the engine presents, acquires
        // and manages its swapchain through the interposer's exports. Without that sl.common's presentCommon
        // never runs - "presentCommon() was not observed" on every evaluate, and its per-frame bookkeeping and
        // garbage collection never run either. The surface entry points of that list are not routed: the surface
        // is made before the device (to choose a device that presents to it) and outlives slShutdown, and
        // Streamline registers no surface hook for DLSS Super Resolution (only sl.common's present before/after).
        // A detached ImGui window presents its own swapchain through the backend's loader calls; the engine's
        // frame is the main swapchain's present, the one Streamline counts frames by.
        API::Vulkan::VulkanPresentationDispatch& hooks = m_Library->Presentation;
        for ( const auto& resolved :
              { m_Library->Resolve( hooks.vkQueuePresentKHR, "vkQueuePresentKHR" ),
                m_Library->Resolve( hooks.vkAcquireNextImageKHR, "vkAcquireNextImageKHR" ),
                m_Library->Resolve( hooks.vkCreateSwapchainKHR, "vkCreateSwapchainKHR" ),
                m_Library->Resolve( hooks.vkDestroySwapchainKHR, "vkDestroySwapchainKHR" ),
                m_Library->Resolve( hooks.vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR" ),
                m_Library->Resolve( hooks.vkDeviceWaitIdle, "vkDeviceWaitIdle" ) } )
            if ( !resolved )
            {
                RefuseAll( std::format( "manual hooking: {}", resolved.GetError() ) );
                return;
            }
        m_Library->Hooked = true;
        // Each feature's entry points from its own plugin; a plugin that does not answer refuses its feature.
        const auto resolveFeature = [this]( const DlssFeature feature, const std::array<const char*, 3>& names,
                                            const std::array<void**, 3>& slots )
        {
            if ( !StateOf( feature ).Candidate )
                return;
            std::array<sl::Result, 3> results{};
            for ( std::size_t i = 0; i < names.size(); ++i )
                results[i] = m_Library->GetFeatureFunction( ToSl( feature ), names[i], *slots[i] );
            if ( std::any_of( results.begin(), results.end(),
                              []( const sl::Result result ) { return result != sl::Result::eOk; } ) )
            {
                Refuse( feature, std::format( "the plugin's entry points: {} {}, {} {}, {} {}", names[0],
                                              ResultName( results[0] ), names[1], ResultName( results[1] ), names[2],
                                              ResultName( results[2] ) ) );
                return;
            }
            FeatureState& state = StateOf( feature );
            state.Ready         = true;
            state.Refusal.clear();
            LOG_INFO( "[Streamline] {} ready", DlssFeatureName( feature ) );
        };
        void* getOptimal   = nullptr;
        void* setOptions   = nullptr;
        void* getState     = nullptr;
        void* rrGetOptimal = nullptr;
        void* rrSetOptions = nullptr;
        void* rrGetState   = nullptr;
        resolveFeature( DlssFeature::SuperResolution,
                        { "slDLSSGetOptimalSettings", "slDLSSSetOptions", "slDLSSGetState" },
                        { &getOptimal, &setOptions, &getState } );
        resolveFeature( DlssFeature::RayReconstruction,
                        { "slDLSSDGetOptimalSettings", "slDLSSDSetOptions", "slDLSSDGetState" },
                        { &rrGetOptimal, &rrSetOptions, &rrGetState } );
        m_Library->GetOptimalSettings   = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>( getOptimal );
        m_Library->SetOptions           = reinterpret_cast<PFun_slDLSSSetOptions*>( setOptions );
        m_Library->GetState             = reinterpret_cast<PFun_slDLSSGetState*>( getState );
        m_Library->RRGetOptimalSettings = reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>( rrGetOptimal );
        m_Library->RRSetOptions         = reinterpret_cast<PFun_slDLSSDSetOptions*>( rrSetOptions );
        m_Library->RRGetState           = reinterpret_cast<PFun_slDLSSDGetState*>( rrGetState );
    }

    void Runtime::AmendPresentationDispatch( API::Vulkan::VulkanPresentationDispatch& dispatch ) const
    {
        const std::scoped_lock lock( m_Mutex );
        // Hooked even when DLSS refused itself after slSetVulkanInfo: Streamline owns the device's bookkeeping
        // from that call until slShutdown either way.
        if ( !m_Initialized || m_Library == nullptr || !m_Library->Hooked )
            return;
        dispatch = m_Library->Presentation;
        LOG_INFO( "[Streamline] presentation runs through sl.interposer (manual hooking): present, acquire, "
                  "swapchain create/destroy/images, device wait idle" );
    }

    void Runtime::OnDeviceDestroying()
    {
        const std::scoped_lock lock( m_Mutex );
        if ( !m_Initialized )
            return;
        // Views still holding a viewport (their histories are dropped after the device): free them here, before
        // slShutdown; their later ReleaseViewport finds the viewport gone and does nothing.
        for ( const auto& [viewport, feature] : m_Viewports )
            if ( const sl::Result result = m_Library->FreeResources( ToSl( feature ), sl::ViewportHandle( viewport ) );
                 result != sl::Result::eOk )
                LOG_ERROR( "[Streamline] slFreeResources({}, viewport {}) at device destruction: {}",
                           DlssFeatureName( feature ), viewport, ResultName( result ) );
        m_Viewports.clear();
        if ( const sl::Result result = m_Library->Shutdown(); result != sl::Result::eOk )
            LOG_ERROR( "[Streamline] slShutdown: {}", ResultName( result ) );
        m_Library.reset();
        m_Initialized = false;
        m_Features    = {};
        m_Token       = nullptr;
        m_TokenFrame    = UINT64_MAX;
    }

    Common::ResultStr<DlssRenderRange> Runtime::RenderRange( const DlssFeature feature, const DlssMode mode,
                                                             const uint32_t outputWidth,
                                                             const uint32_t outputHeight ) const
    {
        const std::scoped_lock lock( m_Mutex );
        const FeatureState&    state = StateOf( feature );
        if ( !state.Ready )
            return Common::MakeFormattedError<DlssRenderRange>( "{} is not ready: {}", DlssFeatureName( feature ),
                                                                state.Refusal );
        const auto range = [&]( auto& options, auto& settings, auto* getOptimal,
                                const std::string_view call ) -> Common::ResultStr<DlssRenderRange>
        {
            options.mode         = ToSl( mode );
            options.outputWidth  = outputWidth;
            options.outputHeight = outputHeight;
            if ( const sl::Result result = getOptimal( options, settings ); result != sl::Result::eOk )
                return Common::MakeFormattedError<DlssRenderRange>( "{}({}, {}x{}): {}", call, DlssModeName( mode ),
                                                                    outputWidth, outputHeight, ResultName( result ) );
            return Common::MakeSuccess( DlssRenderRange{ .OptimalWidth  = settings.optimalRenderWidth,
                                                         .OptimalHeight = settings.optimalRenderHeight,
                                                         .MinWidth      = settings.renderWidthMin,
                                                         .MinHeight     = settings.renderHeightMin,
                                                         .MaxWidth      = settings.renderWidthMax,
                                                         .MaxHeight     = settings.renderHeightMax } );
        };
        if ( feature == DlssFeature::RayReconstruction )
        {
            sl::DLSSDOptions         options;
            sl::DLSSDOptimalSettings settings;
            return range( options, settings, m_Library->RRGetOptimalSettings, "slDLSSDGetOptimalSettings" );
        }
        sl::DLSSOptions         options;
        sl::DLSSOptimalSettings settings;
        return range( options, settings, m_Library->GetOptimalSettings, "slDLSSGetOptimalSettings" );
    }

    uint32_t Runtime::AcquireViewport( const DlssFeature feature )
    {
        const std::scoped_lock lock( m_Mutex );
        // 0 is skipped on wrap: no viewport is 0. Viewports are released with their views, so a wrap never meets
        // a live one in practice; one still held is skipped all the same.
        uint32_t viewport = 0;
        while ( viewport == 0 || m_Viewports.contains( viewport ) )
            viewport = m_NextViewport++;
        m_Viewports.emplace( viewport, feature );
        return viewport;
    }

    void Runtime::ReleaseViewport( const uint32_t viewport )
    {
        const std::scoped_lock lock( m_Mutex );
        const auto found = m_Viewports.find( viewport );
        if ( found == m_Viewports.end() || !m_Initialized )
            return; // freed at device destruction already
        const DlssFeature feature = found->second;
        m_Viewports.erase( found );
        // eErrorInvalidParameter only means the viewport never evaluated: there was no feature to free.
        if ( const sl::Result result = m_Library->FreeResources( ToSl( feature ), sl::ViewportHandle( viewport ) );
             result != sl::Result::eOk && result != sl::Result::eErrorInvalidParameter )
            LOG_ERROR( "[Streamline] slFreeResources({}, viewport {}): {}", DlssFeatureName( feature ), viewport,
                       ResultName( result ) );
    }

    Common::ResultStr<uint64_t> Runtime::SetOptions( const uint32_t viewport, const DlssOptions& input )
    {
        const std::scoped_lock lock( m_Mutex );
        uint64_t               bytes = 0;
        // At creation the camera is not known yet; Evaluate hands Ray Reconstruction this frame's.
        if ( const auto set = SetOptionsLocked( viewport, input, glm::mat4( 1.0f ), glm::mat4( 1.0f ), &bytes ); !set )
            return Common::MakeError<uint64_t>( set.GetError() );
        return Common::MakeSuccess( bytes );
    }

    Common::BoolResultStr Runtime::SetOptionsLocked( const uint32_t viewport, const DlssOptions& input,
                                                     const glm::mat4& worldToView, const glm::mat4& viewToWorld,
                                                     uint64_t* const heldBytes )
    {
        const std::string_view name  = DlssFeatureName( input.Feature );
        const FeatureState&    state = StateOf( input.Feature );
        if ( !state.Ready )
            return Common::MakeFormattedError( "{} options: it is not ready: {}", name, state.Refusal );
        const auto owner = m_Viewports.find( viewport );
        if ( owner == m_Viewports.end() )
            return Common::MakeFormattedError( "{} options: viewport {} was not acquired", name, viewport );
        if ( owner->second != input.Feature )
            return Common::MakeFormattedError( "{} options: viewport {} was acquired for {}", name, viewport,
                                               DlssFeatureName( owner->second ) );
        const sl::ViewportHandle handle( viewport );

        if ( input.Feature == DlssFeature::RayReconstruction )
        {
            sl::DLSSDOptions options;
            options.mode            = ToSl( input.Mode );
            options.outputWidth     = input.OutputWidth;
            options.outputHeight    = input.OutputHeight;
            options.colorBuffersHDR = sl::Boolean::eTrue;
            // The G-buffer's normal carries the roughness in w (DLSSUpscaler's guide pass writes it so).
            options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
            options.worldToCameraView   = ToSl( worldToView );
            options.cameraViewToWorld   = ToSl( viewToWorld );
            SetPreset( options, input.Mode, ToSl( input.RRPreset ) );
            if ( const sl::Result result = m_Library->RRSetOptions( handle, options ); result != sl::Result::eOk )
                return Common::MakeFormattedError(
                     "slDLSSDSetOptions(viewport {}, {}, preset {}, {}x{}): {}", viewport, DlssModeName( input.Mode ),
                     Common::Scalability::kDlssRRPresetNames[static_cast<std::size_t>( input.RRPreset )],
                     input.OutputWidth, input.OutputHeight, ResultName( result ) );
            if ( heldBytes == nullptr )
                return Common::MakeSuccess( true );
            sl::DLSSDState rrState;
            if ( const sl::Result result = m_Library->RRGetState( handle, rrState ); result != sl::Result::eOk )
                return Common::MakeFormattedError( "slDLSSDGetState(viewport {}): {}", viewport,
                                                   ResultName( result ) );
            *heldBytes = rrState.estimatedVRAMUsageInBytes;
            return Common::MakeSuccess( true );
        }

        sl::DLSSOptions options;
        options.mode            = ToSl( input.Mode );
        options.outputWidth     = input.OutputWidth;
        options.outputHeight    = input.OutputHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        // The engine hands its own exposure (the tagged kBufferTypeExposure image, UpscalerExposure: the same
        // multiplier FSR and the engine's TAA use), as UE hands DLSS its eye-adaptation exposure.
        options.useAutoExposure = sl::Boolean::eFalse;
        SetPreset( options, input.Mode, ToSl( input.Preset ) );
        if ( const sl::Result result = m_Library->SetOptions( handle, options ); result != sl::Result::eOk )
            return Common::MakeFormattedError(
                 "slDLSSSetOptions(viewport {}, {}, preset {}, {}x{}): {}", viewport, DlssModeName( input.Mode ),
                 Common::Scalability::kDlssPresetNames[static_cast<std::size_t>( input.Preset )], input.OutputWidth,
                 input.OutputHeight, ResultName( result ) );
        if ( heldBytes == nullptr )
            return Common::MakeSuccess( true );
        sl::DLSSState srState;
        if ( const sl::Result result = m_Library->GetState( handle, srState ); result != sl::Result::eOk )
            return Common::MakeFormattedError( "slDLSSGetState(viewport {}): {}", viewport, ResultName( result ) );
        *heldBytes = srState.estimatedVRAMUsageInBytes;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Runtime::Evaluate( VkCommandBuffer commandBuffer, const DlssEvaluation& input )
    {
        const std::scoped_lock lock( m_Mutex );
        const DlssFeature      feature = input.Options.Feature;
        const std::string_view name    = DlssFeatureName( feature );
        const bool             rr      = feature == DlssFeature::RayReconstruction;
        if ( !StateOf( feature ).Ready )
            return Common::MakeFormattedError( "{} evaluate: it is not ready: {}", name, StateOf( feature ).Refusal );
        if ( !m_Viewports.contains( input.Viewport ) )
            return Common::MakeFormattedError( "{} evaluate: viewport {} was not acquired", name, input.Viewport );
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
                 "{} evaluate (viewport {}): the view's split {}x{} -> {}x{} does not fit its images (color {}x{}, "
                 "depth {}x{}, motion {}x{}, output {}x{})",
                 name, input.Viewport, input.RenderWidth, input.RenderHeight, input.OutputWidth, input.OutputHeight,
                 input.Color.Width, input.Color.Height, input.Depth.Width, input.Depth.Height,
                 input.MotionVectors.Width, input.MotionVectors.Height, input.Output.Width, input.Output.Height );

        // A Streamline call that fails here has not written the output this pass promised (Result::
        // eErrorExceptionHandler: NGX faulted inside the driver), and nothing says the next frame fares better:
        // DLSS refuses itself for the device's life, with the call and its result as the reason, and the view
        // renders with the engine's method from the next frame on (TemporalMethodUnavailable).
        if ( rr && ( !holds( input.DiffuseAlbedo, input.RenderWidth, input.RenderHeight ) ||
                     !holds( input.SpecularAlbedo, input.RenderWidth, input.RenderHeight ) ||
                     !holds( input.NormalRoughness, input.RenderWidth, input.RenderHeight ) ||
                     ( input.SpecularHitDistance.Image != VK_NULL_HANDLE &&
                       !holds( input.SpecularHitDistance, input.RenderWidth, input.RenderHeight ) ) ) )
            return Common::MakeFormattedError(
                 "{} evaluate (viewport {}): a guide does not hold the render extent {}x{} (diffuse albedo {}x{}, "
                 "specular albedo {}x{}, normal+roughness {}x{}, specular hit distance {}x{})",
                 name, input.Viewport, input.RenderWidth, input.RenderHeight, input.DiffuseAlbedo.Width,
                 input.DiffuseAlbedo.Height, input.SpecularAlbedo.Width, input.SpecularAlbedo.Height,
                 input.NormalRoughness.Width, input.NormalRoughness.Height, input.SpecularHitDistance.Width,
                 input.SpecularHitDistance.Height );
        const auto refuseEvaluation = [this, feature]( std::string why ) -> Common::BoolResultStr
        {
            Refuse( feature, why );
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
        if ( rr )
            if ( const auto set = SetOptionsLocked( input.Viewport, input.Options, input.WorldToView,
                                                    input.ViewToWorld, nullptr );
                 !set )
                return refuseEvaluation( set.GetError() );

        // Tagged locally, for this evaluation only: the images are graph resources valid for this pass, so nothing
        // is copied and nothing outlives the call.
        sl::Resource                 color          = ToSl( input.Color );
        sl::Resource                 depth          = ToSl( input.Depth );
        sl::Resource                 motion         = ToSl( input.MotionVectors );
        sl::Resource                 output         = ToSl( input.Output );
        sl::Resource                 exposure       = ToSl( input.Exposure );
        sl::Resource                 diffuseAlbedo  = ToSl( input.DiffuseAlbedo );
        sl::Resource                 specularAlbedo = ToSl( input.SpecularAlbedo );
        sl::Resource                 normals        = ToSl( input.NormalRoughness );
        sl::Resource                 hitDistance    = ToSl( input.SpecularHitDistance );
        const sl::Extent             render{ 0, 0, input.RenderWidth, input.RenderHeight };
        const sl::Extent             full{ 0, 0, input.OutputWidth, input.OutputHeight };
        const sl::Extent             texel{ 0, 0, 1, 1 };
        std::vector<sl::ResourceTag> tags{
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
        if ( rr )
        {
            tags.emplace_back( &diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent,
                               &render );
            tags.emplace_back( &specularAlbedo, sl::kBufferTypeSpecularAlbedo,
                               sl::ResourceLifecycle::eValidUntilPresent, &render );
            tags.emplace_back( &normals, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent,
                               &render );
            if ( input.SpecularHitDistance.Image != VK_NULL_HANDLE )
                tags.emplace_back( &hitDistance, sl::kBufferTypeSpecularHitDistance,
                                   sl::ResourceLifecycle::eValidUntilPresent, &render );
        }
        std::vector<const sl::BaseStructure*> inputs{ &viewport };
        for ( const sl::ResourceTag& tag : tags )
            inputs.push_back( &tag );
        if ( const sl::Result result = m_Library->EvaluateFeature(
                  ToSl( feature ), token, const_cast<const sl::BaseStructure**>( inputs.data() ),
                  static_cast<uint32_t>( inputs.size() ), commandBuffer );
             result != sl::Result::eOk )
            return refuseEvaluation( std::format( "slEvaluateFeature({}, viewport {}, {} {}x{} -> {}x{}): {}", name,
                                                  input.Viewport, DlssModeName( input.Options.Mode ),
                                                  input.RenderWidth, input.RenderHeight, input.OutputWidth,
                                                  input.OutputHeight, ResultName( result ) ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::Streamline
