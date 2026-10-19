#pragma once

#include <Engine/Core/ShaderCompiler/BindlessSetIndex.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDescriptorSetLayout.hpp>
#include <Engine/Graphic/BindlessTextureTable.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// The Vulkan side of the global bindless table (BindlessTextureTable.hpp): one descriptor set layout, one live
// descriptor set at a time, and the fixed sampler array. Shaders see it at set DESERT_BINDLESS_SET
// (Common/BindlessTextures.glslh); VulkanShader substitutes this layout for that set index.
namespace Desert::Graphic::API::Vulkan
{
    // The set index every pipeline layout that reflects u_BindlessTextures uses for the table (one constant shared
    // with the DESERT_BINDLESS_SET define, Core/ShaderCompiler/BindlessSetIndex.hpp). A gap below it in a pipeline
    // layout is filled with an empty layout (VulkanShader).
    using ::Desert::Core::kBindlessSetIndex;

    // Binding numbers inside the set: the same constants the shaders get as DESERT_BINDLESS_SAMPLER_BINDING /
    // DESERT_BINDLESS_TEXTURE_BINDING (BindlessSetIndex.hpp says why samplers come first).
    using ::Desert::Core::kBindlessSamplerBinding;
    using ::Desert::Core::kBindlessTextureBinding;

    struct BindlessBindingFlags
    {
        VkDescriptorBindingFlags Samplers = 0;
        VkDescriptorBindingFlags Textures = 0;
    };

    // PURE. Textures: written while earlier frames that never read the written slot are still in flight
    // (UPDATE_AFTER_BIND + UPDATE_UNUSED_WHILE_PENDING), partially bound (free slots hold nothing), variable count
    // (a set is allocated at the table's current capacity under the layout's ceiling). Samplers: fixed and never
    // rewritten in a live set — a sampler change makes a new set — so they need none of it.
    [[nodiscard]] constexpr BindlessBindingFlags ComputeBindlessBindingFlags()
    {
        return { 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                         VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                         VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT };
    }

    class VulkanBindlessTextureTable final : public BindlessTextureTable
    {
    public:
        // The sampler for a state, as the engine makes it (AcquireSlotSampler: the cache keyed on the state AND
        // the machine's texture filter, so a filter change returns a different handle).
        using SamplerSource = std::function<VkSampler( const Core::Formats::SamplerState& )>;

        // @p maxCapacity: BindlessCapacityCeiling of the device's update-after-bind limits.
        VulkanBindlessTextureTable( VkDevice device, uint32_t maxCapacity, SamplerSource samplers );
        ~VulkanBindlessTextureTable() override;
        VulkanBindlessTextureTable( const VulkanBindlessTextureTable& )            = delete;
        VulkanBindlessTextureTable& operator=( const VulkanBindlessTextureTable& ) = delete;

        // Creates the layout, the first set (kBindlessInitialTextureCapacity) and the magenta "missing" image
        // written at slot 0. Called once by the device right after construction (the missing image's own
        // registration reaches this table through GetBindlessTextureTable, so it must already be published).
        [[nodiscard]] Common::BoolResultStr Initialize();
        // Drops the missing image while the renderer context can still retire it (VulkanLogicalDevice::Destroy,
        // before the context's shutdown).
        void ReleaseMissingTexture();

        [[nodiscard]] Common::ResultStr<BindlessTextureSlot> Register( const Image2D& image ) override;
        void                                                 Release( BindlessTextureSlot slot ) override;
        void                   EndFrame( uint64_t frame, uint32_t framesInFlight ) override;
        [[nodiscard]] uint32_t LiveSlots() const override;
        [[nodiscard]] uint32_t Capacity() const override;

        [[nodiscard]] const DescriptorSetLayoutRef& GetLayout() const
        {
            return m_Layout;
        }
        // The set to bind at kBindlessSetIndex for commands recorded NOW. Growth or a sampler change replaces it;
        // command buffers recorded earlier keep the old one, which is retired after the frames in flight.
        [[nodiscard]] VkDescriptorSet GetDescriptorSet() const;

    private:
        struct TableSet
        {
            VkDescriptorPool Pool     = VK_NULL_HANDLE;
            VkDescriptorSet  Set      = VK_NULL_HANDLE;
            uint32_t         Capacity = 0;
        };
        struct RetiringSet
        {
            TableSet Table;
            uint64_t Frame = 0;
        };

        [[nodiscard]] Common::ResultStr<TableSet> CreateSet( uint32_t capacity ) const;
        void                                      DestroySet( const TableSet& set ) const;
        void WriteTexture( VkDescriptorSet set, uint32_t slot, VkImageView view, VkImageLayout layout ) const;
        void WriteSamplers( VkDescriptorSet set ) const;
        // A new set at @p capacity holding every live slot and the current samplers; the old one is retired.
        [[nodiscard]] Common::BoolResultStr Rebuild( uint32_t capacity );

        struct SlotView
        {
            VkImageView   View   = VK_NULL_HANDLE;
            VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
        };

        VkDevice                                          m_Device      = VK_NULL_HANDLE;
        uint32_t                                          m_MaxCapacity = 0;
        SamplerSource                                     m_SamplerSource;
        std::shared_ptr<const VulkanDescriptorSetLayout>  m_Layout;
        BindlessSlotAllocator                             m_Slots;
        std::vector<SlotView>                             m_Views; // by slot; slot 0 = the missing image
        std::array<VkSampler, kBindlessSamplerStateCount> m_Samplers{};
        TableSet                                          m_Current;
        std::vector<RetiringSet>                          m_Retiring;
        std::shared_ptr<Image2D>                          m_Missing;
        uint64_t                                          m_Frame = 0;
        mutable std::mutex                                m_Mutex;
    };

    // The device's table when @p layouts (a pipeline's, set order) holds its layout at kBindlessSetIndex — the
    // shader reflected the table (VulkanShader::CreateDescriptorsLayout) — else null. The one test every consumer
    // of pipeline layouts uses to tell the table's set from a set it allocates itself.
    [[nodiscard]] VulkanBindlessTextureTable*
    BindlessTableIn( const std::vector<DescriptorSetLayoutRef>& layouts );
} // namespace Desert::Graphic::API::Vulkan
