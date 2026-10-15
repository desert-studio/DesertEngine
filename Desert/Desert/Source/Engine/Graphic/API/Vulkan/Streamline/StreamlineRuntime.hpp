#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDeviceExtensions.hpp>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// NVIDIA STREAMLINE, MANUALLY HOOKED (Windows only: premake compiles every Streamline/ directory on Windows alone
// and defines DESERT_WITH_STREAMLINE there).
//
// UE's DLSS plugin owns the SDK in one module and hands the renderer an upscaler; this is that module's runtime
// half. It loads sl.interposer.dll from the executable's directory at run time (never linked: a missing or
// unsigned DLL is a named refusal, not a process that cannot start), initialises Streamline in manual-hooking mode
// (the engine keeps its own loader and creates the instance and device itself, adding what
// slGetFeatureRequirements asks for), and gives the device to Streamline with slSetVulkanInfo. Nothing else in the
// engine includes a Streamline header: the upscaler (Graphic/View/Streamline/DLSSUpscaler) talks to this class in
// engine types.
//
// The per-view DLSS feature (Streamline's ViewportHandle, NGX's feature handle) lives here, keyed by the view's
// SceneViewState::ViewKey: created by the first evaluation of a view, re-optioned when its mode or output size
// changes, freed when the view has not evaluated for more frames than are in flight (UE: the NGX feature cache's
// frame-age release), and every remaining one at device destruction.
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
        uint32_t ViewKey      = 0; // SceneViewState::ViewKey, never 0
        DlssMode Mode         = DlssMode::DLAA;
        uint32_t OutputWidth  = 0;
        uint32_t OutputHeight = 0;

        DlssImage Color;         // linear HDR, render extent, jittered
        DlssImage Depth;         // reversed-Z hardware depth, render extent
        DlssImage MotionVectors; // render extent
        DlssImage Output;        // output extent, storage

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
        float     NearPlane    = 0.0f;
        float     FarPlane     = 0.0f;
        float     VerticalFov  = 0.0f; // radians
        float     AspectRatio  = 0.0f;
        bool      Reset        = false; // the view's history is invalid (camera cut, resize, first frame)
        uint64_t  EngineFrame  = 0;     // FrameManager absolute frame count: the frame token and the feature's age
        uint32_t  FramesToKeep = 0;     // frames in flight + 1: an idle feature older than this is freed
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

        // Records one DLSS evaluation into @p commandBuffer (tags, constants, options when they changed,
        // evaluate).
        [[nodiscard]] Common::BoolResultStr Evaluate( VkCommandBuffer commandBuffer, const DlssEvaluation& input );

        // IVulkanDeviceExtension
        [[nodiscard]] std::string_view         Name() const override;
        [[nodiscard]] std::vector<const char*> InstanceExtensions() override;
        void OnPhysicalDeviceSelected( VkInstance instance, vkb::PhysicalDevice& device ) override;
        void AmendCatalogProbe( API::Vulkan::CatalogProbe& probe ) const override;
        void OnDeviceCreated( const API::Vulkan::VulkanDeviceHandles& handles ) override;
        void OnDeviceDestroying() override;

    private:
        struct Library; // the loaded interposer and its function table (StreamlineRuntime.cpp)
        struct ViewFeature
        {
            DlssMode Mode         = DlssMode::DLAA;
            uint32_t OutputWidth  = 0;
            uint32_t OutputHeight = 0;
            uint64_t LastFrame    = 0;
        };

        void Refuse( std::string reason );
        void FreeIdleFeatures( uint64_t engineFrame, uint32_t framesToKeep );

        mutable std::mutex              m_Mutex;
        std::unique_ptr<Library>        m_Library;
        bool                            m_Initialized   = false; // slInit succeeded
        bool                            m_DlssCandidate = false; // supported + requirements enabled on the device
        bool                            m_DlssReady     = false; // slSetVulkanInfo accepted
        std::string                     m_Refusal;
        std::vector<std::string>        m_InstanceExtensionNames; // storage for InstanceExtensions()'s pointers
        std::map<uint32_t, ViewFeature> m_Features;               // by ViewKey
        uint64_t                        m_TokenFrame = UINT64_MAX;
        void*                           m_Token      = nullptr; // sl::FrameToken* of m_TokenFrame
    };
} // namespace Desert::Graphic::Streamline
