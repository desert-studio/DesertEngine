#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDeviceExtensions.hpp>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <set>
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
        uint32_t Viewport = 0;              // Runtime::AcquireViewport, options set by SetOptions
        DlssMode Mode     = DlssMode::DLAA; // the mode SetOptions last set (named in errors)

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
    };

    // What one view's DLSS feature is created with. A change re-options the feature (slDLSSSetOptions), which
    // Streamline answers by recreating the NGX feature when the mode, the output size or the preset differ.
    struct DlssOptions
    {
        DlssMode                        Mode         = DlssMode::DLAA;
        uint32_t                        OutputWidth  = 0;
        uint32_t                        OutputHeight = 0;
        Common::Scalability::DlssPreset Preset       = Common::Scalability::DlssPreset::Default; // for Mode

        [[nodiscard]] bool operator==( const DlssOptions& ) const = default;
    };

    class Runtime final : public API::Vulkan::IVulkanDeviceExtension
    {
    public:
        static Runtime& Get();

        Runtime();
        ~Runtime() override;
        Runtime( const Runtime& )            = delete;
        Runtime& operator=( const Runtime& ) = delete;

        // DLSS Super Resolution can evaluate on this device: Streamline loaded and initialised, the adapter
        // supported, the device's requirements enabled, slSetVulkanInfo accepted.
        [[nodiscard]] bool DlssReady() const;
        // Why DlssReady() is false (empty while it is true or before the device exists).
        [[nodiscard]] std::string DlssRefusal() const;

        // The render extents @p mode accepts for an output of @p outputWidth x @p outputHeight.
        [[nodiscard]] Common::ResultStr<DlssRenderRange> RenderRange( DlssMode mode, uint32_t outputWidth,
                                                                      uint32_t outputHeight ) const;

        // A new per-view viewport, unique for the runtime's life and never 0. The caller owns it until
        // ReleaseViewport.
        [[nodiscard]] uint32_t AcquireViewport();
        // Frees the viewport's DLSS feature (slFreeResources) once no frame in flight uses it — the caller
        // retires it through the frame ring. A no-op after device destruction (everything was freed there).
        void ReleaseViewport( uint32_t viewport );
        // Sets @p viewport's options (slDLSSSetOptions); returns the feature's estimated GPU bytes
        // (slDLSSGetState), which the view counts as held by its history.
        [[nodiscard]] Common::ResultStr<uint64_t> SetOptions( uint32_t viewport, const DlssOptions& options );

        // Records one DLSS evaluation of a viewport whose options are set into @p commandBuffer (constants, the
        // five tagged images, evaluate).
        [[nodiscard]] Common::BoolResultStr Evaluate( VkCommandBuffer commandBuffer, const DlssEvaluation& input );

        // IVulkanDeviceExtension
        [[nodiscard]] std::string_view         Name() const override;
        [[nodiscard]] API::Vulkan::InstanceRequirements InstanceExtensions() override;
        void OnPhysicalDeviceSelected( VkInstance instance, vkb::PhysicalDevice& device ) override;
        void AmendCatalogProbe( API::Vulkan::CatalogProbe& probe ) const override;
        void OnDeviceCreated( const API::Vulkan::VulkanDeviceHandles& handles ) override;
        void OnDeviceDestroying() override;

    private:
        struct Library; // the loaded interposer and its function table (StreamlineRuntime.cpp)
        void Refuse( std::string reason );

        mutable std::mutex       m_Mutex;
        std::unique_ptr<Library> m_Library;
        bool                     m_Initialized   = false; // slInit succeeded
        bool                     m_DlssCandidate = false; // supported + requirements enabled on the device
        bool                     m_DlssReady     = false; // slSetVulkanInfo accepted
        std::string              m_Refusal;
        std::vector<std::string> m_InstanceExtensionNames; // storage for InstanceExtensions()'s pointers
        std::set<uint32_t>       m_Viewports;              // acquired and not yet released
        uint32_t                 m_NextViewport = 1;
        uint64_t                 m_TokenFrame   = UINT64_MAX;
        void*                    m_Token        = nullptr; // sl::FrameToken* of m_TokenFrame
    };
} // namespace Desert::Graphic::Streamline
