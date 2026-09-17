#pragma once

#include <Engine/Core/Device.hpp>

#include <vulkan/vulkan.h>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanPhysicalDevice final
    {
    public:
        VulkanPhysicalDevice();
        ~VulkanPhysicalDevice() = default;

        /// THE RAW PROBE RESULT, and the only place an absent family may be represented. `PresentFamily`
        /// used to sit here and was never assigned by anything — a fourth optional that every reader would
        /// have found empty.
        struct QueueFamilyIndices
        {
            std::optional<uint32_t> GraphicsFamily;
            std::optional<uint32_t> ComputeFamily;
            std::optional<uint32_t> TransferFamily;
        };

        /// WHAT A CREATED DEVICE IS GUARANTEED TO HAVE. Plain indices, because after CreateDevice() there
        /// is no such thing as a missing one: a physical device whose driver reports no graphics family is
        /// refused there, by name, with the families it did report. Before that refusal existed, five call
        /// sites unwrapped the optionals above — three with `.value()`, which throws
        /// `std::bad_optional_access` out of a constructor that has no handler, and two with `*`, which is
        /// undefined behaviour — and all five were asking a question device selection had already settled.
        struct ResolvedQueueFamilies
        {
            uint32_t Graphics = 0;
            uint32_t Compute  = 0;
            uint32_t Transfer = 0;
        };

        const VkPhysicalDevice& GetVulkanPhysicalDevice() const
        {
            return m_PhysicalDevice;
        }
        bool IsExtensionSupported( const std::string& extensionName ) const
        {
            return m_SupportedExtensions.find( extensionName ) != m_SupportedExtensions.end();
        }

        [[nodiscard]] uint32_t GetGraphicsFamily() const
        {
            DESERT_VERIFY( m_QueueFamiliesResolved, "queue families read before CreateDevice() settled them" );
            return m_ResolvedQueueFamilies.Graphics;
        }

        [[nodiscard]] uint32_t GetComputeFamily() const
        {
            DESERT_VERIFY( m_QueueFamiliesResolved, "queue families read before CreateDevice() settled them" );
            return m_ResolvedQueueFamilies.Compute;
        }

        [[nodiscard]] uint32_t GetTransferFamily() const
        {
            DESERT_VERIFY( m_QueueFamiliesResolved, "queue families read before CreateDevice() settled them" );
            return m_ResolvedQueueFamilies.Transfer;
        }

        Common::ResultStr<bool> CreateDevice();

        VkFormat GetDepthFormat() const
        {
            return m_DepthFormat;
        }

        const auto& GetCapabilities() const
        {
            return m_Capabilities;
        }

        static std::shared_ptr<VulkanPhysicalDevice> Create();

    private:
        VkFormat           FindDepthFormat() const;
        QueueFamilyIndices GetQueueFamilyIndices( int flags );

    private:
        VkPhysicalDevice                     m_PhysicalDevice = VK_NULL_HANDLE;
        std::vector<VkQueueFamilyProperties> m_QueueFamilyProperties;
        std::vector<VkDeviceQueueCreateInfo> m_QueueCreateInfos;
        QueueFamilyIndices                   m_QueueFamilyIndices;
        ResolvedQueueFamilies                m_ResolvedQueueFamilies;
        bool                                 m_QueueFamiliesResolved = false;

        VkFormat m_DepthFormat = VK_FORMAT_UNDEFINED;

        Engine::DeviceCapabilities m_Capabilities;

        std::unordered_set<std::string> m_SupportedExtensions;

    private:
        friend class VulkanLogicalDevice;
    };

    class VulkanLogicalDevice : public Engine::Device
    {
    public:
        VulkanLogicalDevice();
        ~VulkanLogicalDevice() override;

        // Device interface implementation
        [[nodiscard]] const Engine::DeviceCapabilities& GetCapabilities() const override;
        virtual void                                    WaitIdle() const override;
        [[nodiscard]] Engine::DeviceMemoryReport        QueryMemory() const override;
        [[nodiscard]] virtual std::string               GetName() const override;
        [[nodiscard]] bool IsFormatSupported( ::Desert::Core::Formats::ImageFormat format,
                                              Engine::FormatUsage                  usage ) const override;

        const auto& GetPhysicalDevice() const
        {
            return m_PhysicalDevice;
        }
        VkDevice GetVulkanLogicalDevice() const
        {
            return m_LogicalDevice;
        }

        // ONE device-wide pipeline cache, seeded from Cooked/PipelineCache.bin on create and written
        // back on Destroy — so the driver reuses previously-built pipeline binaries across runs instead
        // of rebuilding every graphics/compute pipeline from scratch each startup. Passed to every
        // vkCreate*Pipelines call (graphics + compute).
        VkPipelineCache GetPipelineCache() const
        {
            return m_PipelineCache;
        }

        VkQueue GetGraphicsQueue()
        {
            return m_GraphicsQueue;
        }
        VkQueue GetComputeQueue()
        {
            return m_ComputeQueue;
        }

        void Destroy();

        Common::ResultStr<bool> CreateDevice();

    private:
        // Create the device-wide VkPipelineCache, seeding it from the on-disk cache if present. The driver
        // validates the header (vendor/device/UUID) and silently ignores mismatched or corrupt data.
        void CreatePipelineCache();
        // Serialize the current pipeline cache to disk (best-effort — read-only installs just skip it).
        void SavePipelineCache() const;

    private:
        std::shared_ptr<VulkanPhysicalDevice> m_PhysicalDevice;
        VkDevice                              m_LogicalDevice;
        VkPipelineCache                       m_PipelineCache = VK_NULL_HANDLE;
        std::string                           m_DeviceName;

        // Whether VK_EXT_memory_budget was ENABLED on this device, not merely supported by it. Chaining
        // `VkPhysicalDeviceMemoryBudgetPropertiesEXT` into a properties query whose extension the device
        // was not created with is undefined behaviour that in practice returns silent zeros — which would
        // be read as "nothing is allocated" by the one reader whose whole purpose is to notice growth.
        bool m_MemoryBudgetEnabled = false;

        VkQueue m_GraphicsQueue;
        VkQueue m_ComputeQueue;
        VkQueue m_TransferQueue;

        friend class CommandBufferAllocator;
    };
} // namespace Desert::Graphic::API::Vulkan