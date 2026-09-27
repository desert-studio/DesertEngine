#pragma once

#include <Engine/Graphic/PipelineCacheFile.hpp>

#include <Engine/Core/Device.hpp>

#include <vulkan/vulkan.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_set>

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

        /// The families the device advertised, read once at construction; callers that need a family's
        /// flags read them here instead of enumerating the device a second time.
        [[nodiscard]] const std::vector<VkQueueFamilyProperties>& GetQueueFamilyProperties() const
        {
            return m_QueueFamilyProperties;
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
        [[nodiscard]] Common::BoolResultStr             PersistPipelineCache() override;
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

        // ONE device-wide pipeline cache, seeded from its DDC entry on create and written back while the
        // app runs (PersistPipelineCache) and on Destroy — so the driver reuses previously-built pipeline binaries
        // across runs instead of rebuilding every graphics/compute pipeline from scratch each startup. Passed to
        // every vkCreate*Pipelines call (graphics + compute).
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

        /// THE ONLY vkQueueSubmit OF THE ENGINE. A VkQueue must be externally synchronised (VUID-vkQueueSubmit-
        /// queue-parameter): two threads submitting to it at once is undefined behaviour, and the validation
        /// layer reports it as a THREADING ERROR. Uploads flush through CommandBufferAllocator from whatever
        /// thread creates the resource, while the frame submits from the main thread, so the lock lives here,
        /// with the queues, and not in either caller. Pattern of UE's FVulkanQueue (one queue, one mutex).
        [[nodiscard]] VkResult SubmitToQueue( VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits,
                                              VkFence fence ) const;
        /// vkQueuePresentKHR under the same lock: the present queue is the graphics queue.
        [[nodiscard]] VkResult PresentToQueue( VkQueue queue, const VkPresentInfoKHR& present ) const;
        /// The same lock, held by a caller whose queue calls live in code it does not own: the ImGui Vulkan
        /// backend submits, presents and waits for the detached platform windows itself. The main thread holds
        /// this across UpdatePlatformWindows + RenderPlatformWindowsDefault so an upload flushing from another
        /// thread cannot meet those raw calls on the shared queue. Nothing reached while holding it may call
        /// SubmitToQueue / PresentToQueue / WaitIdle -- the mutex is not recursive.
        [[nodiscard]] std::unique_lock<std::mutex> LockQueues() const;

        void Destroy();

        Common::ResultStr<bool> CreateDevice();

    private:
        // Create the device-wide VkPipelineCache, seeding it from the on-disk cache if present. The driver
        // validates the header (vendor/device/UUID) and silently ignores mismatched or corrupt data.
        void CreatePipelineCache();
        // The driver's current cache into the DDC entry (atomic), unless it equals what is already there.
        Common::BoolResultStr WritePipelineCache();

    private:
        std::shared_ptr<VulkanPhysicalDevice> m_PhysicalDevice;
        VkDevice                              m_LogicalDevice;
        VkPipelineCache                       m_PipelineCache = VK_NULL_HANDLE;
        PipelineCacheFile::DeviceIdentity     m_PipelineIdentity;
        std::filesystem::path                 m_PipelineCacheFile; // empty = not persisted (reason logged)
        uint64_t                              m_PersistedPipelineHash  = 0; // of the entry on disk, 0 = none
        // Rewritten during the run, not only at a clean exit, which a crash or a kill never reaches.
        PipelineCacheFile::PersistSchedule    m_PersistSchedule{ std::chrono::seconds( 2 ) };
        std::string                           m_DeviceName;

        // Whether VK_EXT_memory_budget was ENABLED on this device, not merely supported by it. Chaining
        // `VkPhysicalDeviceMemoryBudgetPropertiesEXT` into a properties query whose extension the device
        // was not created with is undefined behaviour that in practice returns silent zeros — which would
        // be read as "nothing is allocated" by the one reader whose whole purpose is to notice growth.
        bool m_MemoryBudgetEnabled = false;

        VkQueue m_GraphicsQueue;
        VkQueue m_ComputeQueue;
        VkQueue m_TransferQueue;

        /// ONE LOCK FOR ALL THREE QUEUES, not one per queue: on MoltenVK (and any device with a single
        /// family) graphics, compute and transfer are the SAME VkQueue, and vkDeviceWaitIdle requires every
        /// queue of the device to be externally synchronised at once (VUID-vkDeviceWaitIdle-device).
        mutable std::mutex m_QueueMutex;

        friend class CommandBufferAllocator;
    };
} // namespace Desert::Graphic::API::Vulkan