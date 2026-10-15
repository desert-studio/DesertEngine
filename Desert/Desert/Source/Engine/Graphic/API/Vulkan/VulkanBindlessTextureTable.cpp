#include <Engine/Graphic/API/Vulkan/VulkanBindlessTextureTable.hpp>

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

#include <Common/Core/Core.hpp>

#include <algorithm>
#include <format>

namespace Desert::Graphic::API::Vulkan
{
    static_assert( kBindlessSamplerStateCount == kBindlessSamplerCount,
                   "the device check (DeviceCaps) and the table must agree on the fixed sampler set" );

    VulkanBindlessTextureTable::VulkanBindlessTextureTable( VkDevice device, uint32_t maxCapacity,
                                                            SamplerSource samplers )
         : m_Device( device ), m_MaxCapacity( maxCapacity ), m_SamplerSource( std::move( samplers ) ),
           m_Slots( kBindlessInitialTextureCapacity ), m_Views( kBindlessInitialTextureCapacity )
    {
        DESERT_VERIFY(
             maxCapacity >= kBindlessInitialTextureCapacity,
             "bindless table ceiling {} below its initial capacity {} (CheckRequired admits no such device)",
             maxCapacity, kBindlessInitialTextureCapacity );
    }

    VulkanBindlessTextureTable::~VulkanBindlessTextureTable()
    {
        // The device is idle here (VulkanLogicalDevice::Destroy): every retiring set can go at once.
        for ( const RetiringSet& retiring : m_Retiring )
            DestroySet( retiring.Table );
        DestroySet( m_Current );
    }

    Common::BoolResultStr VulkanBindlessTextureTable::Initialize()
    {
        const BindlessBindingFlags                      flags    = ComputeBindlessBindingFlags();
        const std::vector<VkDescriptorSetLayoutBinding> bindings = {
             { kBindlessSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, kBindlessSamplerStateCount,
               VK_SHADER_STAGE_ALL, nullptr },
             { kBindlessTextureBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_MaxCapacity, VK_SHADER_STAGE_ALL,
               nullptr } };
        m_Layout = std::make_shared<const VulkanDescriptorSetLayout>(
             m_Device, kBindlessSetIndex, bindings, "BindlessTextureTable",
             std::vector<VkDescriptorBindingFlags>{ flags.Samplers, flags.Textures },
             VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT );
        if ( m_Layout->Handle() == VK_NULL_HANDLE )
            return Common::MakeFormattedError<bool>(
                 "bindless table: the set layout ({} samplers, up to {} images) "
                 "could not be created",
                 kBindlessSamplerStateCount, m_MaxCapacity );
        for ( uint32_t index = 0; index < kBindlessSamplerStateCount; ++index )
            m_Samplers[index] = m_SamplerSource( BindlessSamplerState( index ) );

        auto first = CreateSet( kBindlessInitialTextureCapacity );
        if ( !first.IsSuccess() )
            return Common::MakeError<bool>( first.GetError() );
        m_Current = first.GetValue();
        WriteSamplers( m_Current.Set );

        // Slot 0: magenta, so a record that never got a real slot shows itself instead of passing for white.
        Core::Formats::Image2DSpecification spec = {
             .Tag        = "BindlessTextureTable-Missing",
             .Width      = 1,
             .Height     = 1,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Mips       = 1,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::ImageProperties::Sample,
        };
        spec.Data          = std::vector<unsigned char>{ 255, 0, 255, 255 };
        auto       missing = std::make_shared<VulkanImage2D>( spec );
        const auto loaded  = missing->RT_Invalidate();
        if ( !loaded.IsSuccess() )
            return Common::MakeFormattedError<bool>( "bindless table: the missing texture failed: {}",
                                                     loaded.GetError() );
        const std::scoped_lock     lock( m_Mutex );
        const VulkanImageResource& resource = missing->GetResource();
        m_Views[0]                          = { resource.ImageView, missing->GetDefaultLayout() };
        WriteTexture( m_Current.Set, 0, resource.ImageView, missing->GetDefaultLayout() );
        m_Missing = std::move( missing );
        return BOOLSUCCESS;
    }

    void VulkanBindlessTextureTable::ReleaseMissingTexture()
    {
        m_Missing.reset();
    }

    Common::ResultStr<VulkanBindlessTextureTable::TableSet>
    VulkanBindlessTextureTable::CreateSet( uint32_t capacity ) const
    {
        const std::array<VkDescriptorPoolSize, 2> sizes = {
             { { VK_DESCRIPTOR_TYPE_SAMPLER, kBindlessSamplerStateCount },
               { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity } } };
        const VkDescriptorPoolCreateInfo poolInfo{ .sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                   .flags   = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                                   .maxSets = 1,
                                                   .poolSizeCount = static_cast<uint32_t>( sizes.size() ),
                                                   .pPoolSizes    = sizes.data() };
        TableSet                         table;
        table.Capacity = capacity;
        if ( const VkResult made = vkCreateDescriptorPool( m_Device, &poolInfo, nullptr, &table.Pool );
             made != VK_SUCCESS )
            return Common::MakeFormattedError<TableSet>(
                 "bindless table: descriptor pool for {} images failed ({})", capacity, static_cast<int>( made ) );

        const VkDescriptorSetVariableDescriptorCountAllocateInfo variable{
             .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
             .descriptorSetCount = 1,
             .pDescriptorCounts  = &capacity };
        const VkDescriptorSetLayout       layout = m_Layout->Handle();
        const VkDescriptorSetAllocateInfo allocInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                     .pNext = &variable,
                                                     .descriptorPool     = table.Pool,
                                                     .descriptorSetCount = 1,
                                                     .pSetLayouts        = &layout };
        if ( const VkResult allocated = vkAllocateDescriptorSets( m_Device, &allocInfo, &table.Set );
             allocated != VK_SUCCESS )
        {
            vkDestroyDescriptorPool( m_Device, table.Pool, nullptr );
            return Common::MakeFormattedError<TableSet>( "bindless table: descriptor set of {} images failed ({})",
                                                         capacity, static_cast<int>( allocated ) );
        }
        return Common::MakeSuccess( table );
    }

    void VulkanBindlessTextureTable::DestroySet( const TableSet& set ) const
    {
        if ( set.Pool != VK_NULL_HANDLE )
            vkDestroyDescriptorPool( m_Device, set.Pool, nullptr ); // frees its one set
    }

    void VulkanBindlessTextureTable::WriteTexture( VkDescriptorSet set, uint32_t slot, VkImageView view,
                                                   VkImageLayout layout ) const
    {
        const VkDescriptorImageInfo image{ VK_NULL_HANDLE, view, layout };
        const VkWriteDescriptorSet  write{ .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet          = set,
                                           .dstBinding      = kBindlessTextureBinding,
                                           .dstArrayElement = slot,
                                           .descriptorCount = 1,
                                           .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                           .pImageInfo      = &image };
        vkUpdateDescriptorSets( m_Device, 1, &write, 0, nullptr );
    }

    void VulkanBindlessTextureTable::WriteSamplers( VkDescriptorSet set ) const
    {
        std::array<VkDescriptorImageInfo, kBindlessSamplerStateCount> samplers{};
        for ( uint32_t index = 0; index < kBindlessSamplerStateCount; ++index )
            samplers[index].sampler = m_Samplers[index];
        const VkWriteDescriptorSet write{ .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet          = set,
                                          .dstBinding      = kBindlessSamplerBinding,
                                          .dstArrayElement = 0,
                                          .descriptorCount = kBindlessSamplerStateCount,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                                          .pImageInfo      = samplers.data() };
        vkUpdateDescriptorSets( m_Device, 1, &write, 0, nullptr );
    }

    Common::BoolResultStr VulkanBindlessTextureTable::Rebuild( uint32_t capacity )
    {
        auto made = CreateSet( capacity );
        if ( !made.IsSuccess() )
            return Common::MakeError<bool>( made.GetError() );
        const TableSet next = made.GetValue();
        WriteSamplers( next.Set );
        for ( uint32_t slot = 0; slot < static_cast<uint32_t>( m_Views.size() ); ++slot )
            if ( m_Views[slot].View != VK_NULL_HANDLE )
                WriteTexture( next.Set, slot, m_Views[slot].View, m_Views[slot].Layout );
        // Commands recorded before this point bound the old set; it lives until their frames have completed.
        m_Retiring.push_back( { m_Current, m_Frame } );
        m_Current = next;
        return BOOLSUCCESS;
    }

    Common::ResultStr<BindlessTextureSlot> VulkanBindlessTextureTable::Register( const Image2D& image )
    {
        const auto* vulkanImage = dynamic_cast<const VulkanImage2D*>( &image );
        if ( vulkanImage == nullptr )
            return Common::MakeError<BindlessTextureSlot>( "bindless table: not a Vulkan 2D image" );
        const Core::Formats::Image2DSpecification& spec = vulkanImage->GetSpecification();
        if ( spec.Usage == Core::Formats::Image2DUsage::Attachment ||
             ( spec.Properties & Core::Formats::Storage ) != 0 || spec.Samples > 1 )
            return Common::MakeFormattedError<BindlessTextureSlot>(
                 "bindless table: image '{}' is an attachment, storage or multisampled image; the render graph "
                 "cannot order a bindless read of it",
                 spec.Tag );
        const VulkanImageResource& resource = vulkanImage->GetResource();
        if ( resource.ImageView == VK_NULL_HANDLE )
            return Common::MakeFormattedError<BindlessTextureSlot>( "bindless table: image '{}' has no view",
                                                                    spec.Tag );

        const std::scoped_lock  lock( m_Mutex );
        std::optional<uint32_t> slot = m_Slots.Allocate();
        if ( !slot )
        {
            const uint32_t capacity = m_Slots.Capacity();
            if ( capacity >= m_MaxCapacity )
                return Common::MakeFormattedError<BindlessTextureSlot>(
                     "bindless table full registering '{}': {} live, {} retiring, device ceiling {}", spec.Tag,
                     m_Slots.Live(), m_Slots.Retiring(), m_MaxCapacity );
            const uint32_t grown = std::min( capacity * 2, m_MaxCapacity );
            m_Slots.Grow( grown );
            m_Views.resize( grown );
            if ( const auto rebuilt = Rebuild( grown ); !rebuilt.IsSuccess() )
                return Common::MakeError<BindlessTextureSlot>( rebuilt.GetError() );
            slot = m_Slots.Allocate();
            DESERT_VERIFY( slot.has_value(), "bindless table: no slot after growing to {}", grown );
        }
        const VkImageLayout layout = vulkanImage->GetDefaultLayout();
        m_Views[*slot]             = { resource.ImageView, layout };
        WriteTexture( m_Current.Set, *slot, resource.ImageView, layout );
        return Common::MakeSuccess( BindlessTextureSlot{ *slot } );
    }

    void VulkanBindlessTextureTable::Release( BindlessTextureSlot slot )
    {
        const std::scoped_lock lock( m_Mutex );
        // The descriptor keeps naming the destroyed view until the slot is reused: legal, because the binding is
        // PARTIALLY_BOUND and no frame recorded from now on indexes it.
        m_Views[slot.Index] = {};
        m_Slots.Release( slot.Index, m_Frame );
    }

    void VulkanBindlessTextureTable::EndFrame( uint64_t frame, uint32_t framesInFlight )
    {
        const std::scoped_lock lock( m_Mutex );
        m_Frame = frame;
        m_Slots.Retire( frame, framesInFlight );
        std::erase_if( m_Retiring,
                       [&]( const RetiringSet& retiring )
                       {
                           if ( frame < retiring.Frame + framesInFlight + 1 )
                               return false;
                           DestroySet( retiring.Table );
                           return true;
                       } );
        // The machine's texture filter is part of every sampler's state: when it changed, the fixed array is
        // rewritten into a NEW set (a live set's samplers may be read by frames in flight).
        bool changed = false;
        for ( uint32_t index = 0; index < kBindlessSamplerStateCount; ++index )
        {
            const VkSampler sampler = m_SamplerSource( BindlessSamplerState( index ) );
            changed                 = changed || sampler != m_Samplers[index];
            m_Samplers[index]       = sampler;
        }
        if ( changed )
        {
            const auto rebuilt = Rebuild( m_Slots.Capacity() );
            DESERT_VERIFY( rebuilt.IsSuccess(), "{}", rebuilt.IsSuccess() ? std::string{} : rebuilt.GetError() );
        }
    }

    uint32_t VulkanBindlessTextureTable::LiveSlots() const
    {
        const std::scoped_lock lock( m_Mutex );
        return m_Slots.Live();
    }

    uint32_t VulkanBindlessTextureTable::Capacity() const
    {
        const std::scoped_lock lock( m_Mutex );
        return m_Slots.Capacity();
    }

    VkDescriptorSet VulkanBindlessTextureTable::GetDescriptorSet() const
    {
        const std::scoped_lock lock( m_Mutex );
        return m_Current.Set;
    }
} // namespace Desert::Graphic::API::Vulkan
