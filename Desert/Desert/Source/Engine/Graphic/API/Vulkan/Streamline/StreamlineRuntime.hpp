#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDeviceExtensions.hpp>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// NVIDIA STREAMLINE, MANUALLY HOOKED (Windows only: premake compiles every Streamline/ directory on Windows alone
// and defines DESERT_UPSCALER_DLSS there).
//
// UE's DLSS plugin owns the SDK in one module and hands the renderer an upscaler; this is that module's runtime
// half. It loads sl.interposer.dll from the executable's directory at run time (never linked: a missing or
// unsigned DLL is a named refusal, not a process that cannot start), initialises Streamline in manual-hooking mode
// (the engine keeps its own loader and creates the instance and device itself, adding what
// slGetFeatureRequirements asks for), and gives the device to Streamline with slSetVulkanInfo. Nothing else in the
// engine includes a Streamline header: the upscaler (Graphic/Upscalers/DLSS/DLSSUpscaler) talks to this class in
// engine types.
//
// The per-view DLSS feature (Streamline's ViewportHandle, NGX's feature handle) is OWNED BY THE VIEW: the
// upscaler's ITemporalUpscalerHistory (DLSSUpscaler.cpp: DlssHistory, the same per-view object FSR keeps its
// context in) acquires a viewport here, sets its options when they change, and releases it when the view drops
// its history (reset, another method, the view's end) — through VulkanAllocator::RT_DestroyLater, after the
// frames that recorded with it. Every viewport still held at device destruction is freed before slShutdown.
namespace Desert::Graphic::Streamline
{
    // The two DLSS features the runtime drives, each its own Streamline plugin and NGX feature with its own device
    // requirements and its own refusal: Super Resolution (sl.dlss, sl::kFeatureDLSS) and Ray Reconstruction
    // (sl.dlss_d, sl::kFeatureDLSS_RR) - the upscaler that is also the denoiser of the noisy lighting signals,
    // and takes the G-buffer's guides beside the colour. Same modes, same viewports, same constants.
    enum class DlssFeature : uint8_t
    {
        SuperResolution,
        RayReconstruction,
    };
    inline constexpr std::size_t kDlssFeatureCount = 2;
    [[nodiscard]] std::string_view DlssFeatureName( DlssFeature feature );

    // The DLSS quality modes, by render-to-output ratio (the network is trained per mode). DLAA is the 100 % mode.
    enum class DlssMode : uint8_t
    {
        UltraPerformance,
        Performance,
        Balanced,
        Quality,
        UltraQuality,
        DLAA,
    };
    [[nodiscard]] std::string_view DlssModeName( DlssMode mode );

    // The render extents a mode accepts for one output extent (slDLSSGetOptimalSettings).
    struct DlssRenderRange
    {
        uint32_t OptimalWidth  = 0;
        uint32_t OptimalHeight = 0;
        uint32_t MinWidth      = 0;
        uint32_t MinHeight     = 0;
        uint32_t MaxWidth      = 0;
        uint32_t MaxHeight     = 0;
    };

    // What one view's DLSS feature is created with. A change re-options the feature (slDLSSSetOptions), which
    // Streamline answers by recreating the NGX feature when the mode, the output size or the preset differ.
    struct DlssOptions
    {
        DlssFeature                     Feature      = DlssFeature::SuperResolution;
        DlssMode                        Mode         = DlssMode::DLAA;
        uint32_t                        OutputWidth  = 0;
        uint32_t                        OutputHeight = 0;
        Common::Scalability::DlssPreset Preset       = Common::Scalability::DlssPreset::Default; // for Mode
        // For Mode under RayReconstruction (Ray Reconstruction has its own networks).
        Common::Scalability::DlssRRPreset RRPreset = Common::Scalability::DlssRRPreset::Default;

        [[nodiscard]] bool operator==( const DlssOptions& ) const = default;
    };

    // One image as Streamline needs it on Vulkan: the handles, the layout it is in while DLSS runs, its usage.
    struct DlssImage
    {
        VkImage           Image  = VK_NULL_HANDLE;
        VkImageView       View   = VK_NULL_HANDLE;
        VkFormat          Format = VK_FORMAT_UNDEFINED;
        VkImageLayout     Layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageUsageFlags Usage  = 0;
        uint32_t          Width  = 0;
        uint32_t          Height = 0;
    };

    // Everything one DLSS evaluation of one view reads. Matrices are the engine's (glm, column vectors); the
    // runtime hands them to Streamline's row-vector convention unchanged in memory (a column-major M is the
    // row-major M^T a row-vector convention multiplies by).
    struct DlssEvaluation
    {
        uint32_t    Viewport = 0; // Runtime::AcquireViewport, options set by SetOptions
        DlssOptions Options;      // what SetOptions last set (its Feature evaluates; the mode is named in errors)

        // THE VIEW'S SPLIT (ViewFrame::Split): what DLSS upscales from and to. The tags' extents are these, never
        // the images' sizes - an image may be larger than the view it holds (a pooled or not-yet-shrunk target),
        // and DLSS then reads past the view (UE tags the view rect, FDLSSPassParameters::InputViewRect).
        uint32_t RenderWidth  = 0;
        uint32_t RenderHeight = 0;
        uint32_t OutputWidth  = 0;
        uint32_t OutputHeight = 0;

        DlssImage Color;         // linear HDR, render extent, jittered
        DlssImage Depth;         // reversed-Z hardware depth, render extent
        DlssImage MotionVectors; // render extent
        DlssImage Output;        // output extent, RDG::Access::ExternalWrite (NGX writes and clears it)
        DlssImage Exposure;      // 1x1 R32F, the exposure multiplier (UpscalerExposure)

        // RAY RECONSTRUCTION'S GUIDES (Options.Feature RayReconstruction only, render extent; DLSSUpscaler's
        // "DLSS-RR: Guides" pass writes the first three from the G-buffer). UE's DLSS plugin hands the same set:
        // diffuse albedo, specular albedo (the split-sum reflectance the composite multiplied the reflection by),
        // world normal with roughness in w (sl::DLSSDNormalRoughnessMode::ePacked), and the specular hit distance
        // of the traced reflection (world units), absent (Image null) where the view traces no reflection.
        DlssImage DiffuseAlbedo;
        DlssImage SpecularAlbedo;
        DlssImage NormalRoughness;
        DlssImage SpecularHitDistance;
        glm::mat4 WorldToView{ 1.0f }; // the normals' space to the camera's: sl::DLSSDOptions::worldToCameraView
        glm::mat4 ViewToWorld{ 1.0f };

        glm::mat4 ViewToClip{ 1.0f }; // unjittered projection
        glm::mat4 ClipToView{ 1.0f };
        glm::mat4 ClipToPrevClip{ 1.0f };
        glm::mat4 PrevClipToClip{ 1.0f };
        glm::vec2 JitterPixels{ 0.0f };      // Streamline's convention (render pixels)
        glm::vec2 MotionVectorScale{ 0.0f }; // multiplies a texel of MotionVectors into Streamline's UV offset
        glm::vec3 CameraPosition{ 0.0f };
        glm::vec3 CameraUp{ 0.0f };
        glm::vec3 CameraRight{ 0.0f };
        glm::vec3 CameraForward{ 0.0f };
        float     NearPlane   = 0.0f;
        float     FarPlane    = 0.0f;
        float     VerticalFov = 0.0f; // radians
        float     AspectRatio = 0.0f;
        bool      Reset       = false; // the view's history is invalid (camera cut, resize, first frame)
        uint64_t  EngineFrame = 0;     // FrameManager absolute frame count: the frame token
        // ViewFrame::PreExposure: Color arrives multiplied by it (sl DLSSOptions/DLSSDOptions::preExposure; DLSS
        // divides it out before Exposure and puts it back on Output, as UE's DLSS plugin hands View.PreExposure).
        float PreExposure = 1.0f;
    };

    class Runtime final : public API::Vulkan::IVulkanDeviceExtension
    {
    public:
        static Runtime& Get();

        Runtime();
        ~Runtime() override;
        Runtime( const Runtime& )            = delete;
        Runtime& operator=( const Runtime& ) = delete;

        // @p feature can evaluate on this device: Streamline loaded and initialised, the adapter supported, the
        // feature's device requirements enabled, slSetVulkanInfo accepted, its plugin's entry points resolved.
        [[nodiscard]] bool Ready( DlssFeature feature ) const;
        // Why Ready( @p feature ) is false (empty while it is true or before the device exists).
        [[nodiscard]] std::string Refusal( DlssFeature feature ) const;

        // The render extents @p mode of @p feature accepts for an output of @p outputWidth x @p outputHeight.
        [[nodiscard]] Common::ResultStr<DlssRenderRange> RenderRange( DlssFeature feature, DlssMode mode,
                                                                      uint32_t outputWidth,
                                                                      uint32_t outputHeight ) const;

        // A new per-view viewport of @p feature, unique for the runtime's life and never 0. The caller owns it
        // until ReleaseViewport.
        [[nodiscard]] uint32_t AcquireViewport( DlssFeature feature );
        // Frees the viewport's feature (slFreeResources) once no frame in flight uses it — the caller retires it
        // through the frame ring. A no-op after device destruction (everything was freed there).
        void ReleaseViewport( uint32_t viewport );
        // Sets @p viewport's options (slDLSSSetOptions / slDLSSDSetOptions, by options.Feature, which must be the
        // viewport's); returns the feature's estimated GPU bytes (slDLSSGetState / slDLSSDGetState), which the
        // view counts as held by its history.
        [[nodiscard]] Common::ResultStr<uint64_t> SetOptions( uint32_t viewport, const DlssOptions& options );

        // Records one evaluation of a viewport whose options are set into @p commandBuffer (constants, the tagged
        // images, evaluate). Ray Reconstruction re-sets its options first: they carry this frame's camera
        // matrices (the normals' space), which Streamline takes without recreating the feature.
        [[nodiscard]] Common::BoolResultStr Evaluate( VkCommandBuffer commandBuffer, const DlssEvaluation& input );

        // IVulkanDeviceExtension
        [[nodiscard]] std::string_view         Name() const override;
        [[nodiscard]] API::Vulkan::InstanceRequirements InstanceExtensions() override;
        void OnPhysicalDeviceSelected( VkInstance instance, vkb::PhysicalDevice& device ) override;
        void AmendCatalogProbe( API::Vulkan::CatalogProbe& probe ) const override;
        void OnDeviceCreated( const API::Vulkan::VulkanDeviceHandles& handles ) override;
        // eUseManualHooking: present, acquire, the swapchain's create/destroy/images and vkDeviceWaitIdle go
        // through sl.interposer's exports, so sl.common's presentCommon runs on every frame's present.
        void AmendPresentationDispatch( API::Vulkan::VulkanPresentationDispatch& dispatch ) const override;
        void OnDeviceDestroying() override;

    private:
        struct Library; // the loaded interposer and its function table (StreamlineRuntime.cpp)
        struct FeatureState
        {
            bool        Candidate = false; // supported + requirements enabled on the device
            bool        Ready     = false; // slSetVulkanInfo accepted, entry points resolved
            std::string Refusal;
        };
        // One feature refuses itself; the other keeps its state.
        void Refuse( DlssFeature feature, std::string reason );
        // Streamline itself refuses (not loaded, not initialised, not an NVIDIA device): both features.
        void                              RefuseAll( const std::string& reason );
        [[nodiscard]] FeatureState&       StateOf( DlssFeature feature );
        [[nodiscard]] const FeatureState& StateOf( DlssFeature feature ) const;
        [[nodiscard]] bool                AnyCandidate() const;
        // SetOptions under m_Mutex. @p worldToView / @p viewToWorld: Ray Reconstruction's camera (identity at
        // creation, the frame's at evaluation). @p preExposure: the colour's pre-exposure (1 at creation, the
        // frame's at evaluation). @p heldBytes: where the feature's estimate goes, null to skip it.
        [[nodiscard]] Common::BoolResultStr SetOptionsLocked( uint32_t viewport, const DlssOptions& input,
                                                              const glm::mat4& worldToView,
                                                              const glm::mat4& viewToWorld, float preExposure,
                                                              uint64_t* heldBytes );

        mutable std::mutex                          m_Mutex;
        std::unique_ptr<Library>                    m_Library;
        bool                                        m_Initialized = false; // slInit succeeded
        std::array<FeatureState, kDlssFeatureCount> m_Features;
        std::vector<std::string>        m_InstanceExtensionNames; // storage for InstanceExtensions()'s pointers
        std::map<uint32_t, DlssFeature> m_Viewports; // acquired and not yet released, by their feature
        uint32_t                 m_NextViewport = 1;
        uint64_t                 m_TokenFrame   = UINT64_MAX;
        void*                    m_Token        = nullptr; // sl::FrameToken* of m_TokenFrame
    };
} // namespace Desert::Graphic::Streamline
