#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanFormat.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderPassDependencies.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <format>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        constexpr uint32_t RdgBit( RDG::Access access )
        {
            return 1u << static_cast<uint32_t>( access );
        }

        bool RdgHas( uint32_t mask, RDG::Access access )
        {
            return ( mask & RdgBit( access ) ) != 0;
        }

        bool RdgSameDesc( const RDG::TextureDesc& a, const RDG::TextureDesc& b )
        {
            return a.Size == b.Size && a.Format == b.Format && a.Mips == b.Mips && a.Layers == b.Layers &&
                   a.Dim == b.Dim;
        }

        VkFormat RdgFormat( const VulkanRdgDevice& device, Core::Formats::ImageFormat format )
        {
            return Utils::GetVulkanFormat( format, device.DepthStencilFormat );
        }

        // THE create info of a transient image: the pool creates from it and the memory requirements are
        // asked of it, so the size Compile plans with is the size the pool allocates.
        VkImageCreateInfo RdgImageInfo( const VulkanRdgDevice& device, const RDG::TextureDesc& desc,
                                        uint32_t accessMask )
        {
            const VkFormat    format = RdgFormat( device, desc.Format );
            const bool        depth  = ( GetImageVulkanAspect( desc.Format ) & VK_IMAGE_ASPECT_DEPTH_BIT ) != 0;
            VkImageCreateInfo info{};
            info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.flags         = desc.Dim == RDG::TextureDim::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
            info.imageType     = desc.Dim == RDG::TextureDim::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            info.format        = format;
            info.extent        = { desc.Size.Width, desc.Size.Height,
                            desc.Dim == RDG::TextureDim::Tex3D ? desc.Size.Depth : 1u };
            info.mipLevels     = desc.Mips;
            info.arrayLayers   = desc.Layers;
            info.samples       = static_cast<VkSampleCountFlagBits>( std::max( 1u, desc.Samples ) );
            info.tiling        = VK_IMAGE_TILING_OPTIMAL;
            info.usage         = RdgImageUsage( accessMask, depth );
            info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            return info;
        }

        VkBufferCreateInfo RdgBufferInfo( const RDG::BufferDesc& desc, uint32_t accessMask )
        {
            VkBufferCreateInfo info{};
            info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size        = desc.Bytes;
            info.usage       = RdgBufferUsage( accessMask );
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            return info;
        }

        VkAttachmentLoadOp RdgLoadOp( RDG::LoadAction action )
        {
            switch ( action )
            {
                case RDG::LoadAction::Load:
                    return VK_ATTACHMENT_LOAD_OP_LOAD;
                case RDG::LoadAction::Clear:
                    return VK_ATTACHMENT_LOAD_OP_CLEAR;
                case RDG::LoadAction::DontCare:
                    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            }
            return VK_ATTACHMENT_LOAD_OP_LOAD;
        }

    } // namespace

    // ── Translation tables ─────────────────────────────────────────────────────────────────────────────

    VkImageUsageFlags RdgImageUsage( uint32_t accessMask, bool depthFormat )
    {
        using RDG::Access;
        VkImageUsageFlags usage = 0;
        if ( RdgHas( accessMask, Access::SampledGraphics ) || RdgHas( accessMask, Access::SampledCompute ) ||
             RdgHas( accessMask, Access::LegacyRead ) )
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if ( RdgHas( accessMask, Access::StorageRead ) || RdgHas( accessMask, Access::StorageWrite ) )
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if ( RdgHas( accessMask, Access::ColorTarget ) )
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if ( RdgHas( accessMask, Access::DepthWrite ) || RdgHas( accessMask, Access::DepthRead ) )
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ( RdgHas( accessMask, Access::CopySrc ) )
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ( RdgHas( accessMask, Access::CopyDst ) )
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // Legacy code renders into the image through its own render pass, samples it and copies it; what it
        // does exactly is invisible to the graph.
        if ( RdgHas( accessMask, Access::LegacyWrite ) )
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     ( depthFormat ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT );
        return usage;
    }

    VkBufferUsageFlags RdgBufferUsage( uint32_t accessMask )
    {
        using RDG::Access;
        VkBufferUsageFlags usage = 0;
        if ( RdgHas( accessMask, Access::StorageRead ) || RdgHas( accessMask, Access::StorageWrite ) )
            usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if ( RdgHas( accessMask, Access::CopySrc ) )
            usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if ( RdgHas( accessMask, Access::CopyDst ) )
            usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if ( RdgHas( accessMask, Access::IndirectArgs ) )
            usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if ( RdgHas( accessMask, Access::VertexIndex ) )
            usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if ( RdgHas( accessMask, Access::UniformRead ) )
            usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if ( RdgHas( accessMask, Access::AccelStructBuildInput ) )
            usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if ( RdgHas( accessMask, Access::AccelStructBuildWrite ) || RdgHas( accessMask, Access::AccelStructRead ) )
            usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        return usage;
    }

    // Vulkan 1.0 barriers (owner decision 2026-09-27: no synchronization2). The graph's stages are finer
    // than v1 in one place: Copy becomes TRANSFER.
    VkPipelineStageFlags RdgVulkanStages( RDG::PipelineStageFlags stages )
    {
        static constexpr std::array<std::pair<uint32_t, VkPipelineStageFlags>, 11> kTable = { {
             { RDG::PipelineStage_DrawIndirect, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT },
             { RDG::PipelineStage_VertexInput, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT },
             { RDG::PipelineStage_VertexShader, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT },
             { RDG::PipelineStage_FragmentShader, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT },
             { RDG::PipelineStage_EarlyFragmentTests, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT },
             { RDG::PipelineStage_LateFragmentTests, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT },
             { RDG::PipelineStage_ColorAttachmentOutput, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT },
             { RDG::PipelineStage_ComputeShader, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT },
             { RDG::PipelineStage_Copy, VK_PIPELINE_STAGE_TRANSFER_BIT },
             { RDG::PipelineStage_Host, VK_PIPELINE_STAGE_HOST_BIT },
             { RDG::PipelineStage_AccelStructBuild, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR },
        } };
        VkPipelineStageFlags                                                       out    = 0;
        for ( const auto& [bit, vk] : kTable )
        {
            if ( ( stages & bit ) != 0 )
                out |= vk;
        }
        return out;
    }

    VkAccessFlags RdgVulkanAccess( RDG::MemoryAccessFlags access )
    {
        static constexpr std::array<std::pair<uint32_t, VkAccessFlags>, 16> kTable = { {
             { RDG::MemoryAccess_IndirectCommandRead, VK_ACCESS_INDIRECT_COMMAND_READ_BIT },
             { RDG::MemoryAccess_IndexRead, VK_ACCESS_INDEX_READ_BIT },
             { RDG::MemoryAccess_VertexAttributeRead, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT },
             { RDG::MemoryAccess_UniformRead, VK_ACCESS_UNIFORM_READ_BIT },
             { RDG::MemoryAccess_ShaderSampledRead, VK_ACCESS_SHADER_READ_BIT },
             { RDG::MemoryAccess_ShaderStorageRead, VK_ACCESS_SHADER_READ_BIT },
             { RDG::MemoryAccess_ShaderStorageWrite, VK_ACCESS_SHADER_WRITE_BIT },
             { RDG::MemoryAccess_ColorAttachmentRead, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT },
             { RDG::MemoryAccess_ColorAttachmentWrite, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
             { RDG::MemoryAccess_DepthStencilRead, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT },
             { RDG::MemoryAccess_DepthStencilWrite, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
             { RDG::MemoryAccess_TransferRead, VK_ACCESS_TRANSFER_READ_BIT },
             { RDG::MemoryAccess_TransferWrite, VK_ACCESS_TRANSFER_WRITE_BIT },
             { RDG::MemoryAccess_HostRead, VK_ACCESS_HOST_READ_BIT },
             { RDG::MemoryAccess_AccelStructRead, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR },
             { RDG::MemoryAccess_AccelStructWrite, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR },
        } };
        VkAccessFlags                                                       out    = 0;
        for ( const auto& [bit, vk] : kTable )
        {
            if ( ( access & bit ) != 0 )
                out |= vk;
        }
        return out;
    }

    std::optional<RDG::ImageLayout> RdgLayoutFromVulkan( VkImageLayout layout )
    {
        switch ( layout )
        {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                return RDG::ImageLayout::Undefined;
            case VK_IMAGE_LAYOUT_GENERAL:
                return RDG::ImageLayout::General;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return RDG::ImageLayout::ColorAttachment;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return RDG::ImageLayout::DepthStencilAttachment;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return RDG::ImageLayout::DepthStencilReadOnly;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return RDG::ImageLayout::ShaderReadOnly;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return RDG::ImageLayout::TransferSrc;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return RDG::ImageLayout::TransferDst;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return RDG::ImageLayout::Present;
            default:
                return std::nullopt;
        }
    }

    VkImageLayout RdgVulkanLayout( RDG::ImageLayout layout )
    {
        switch ( layout )
        {
            case RDG::ImageLayout::Undefined:
                return VK_IMAGE_LAYOUT_UNDEFINED;
            case RDG::ImageLayout::General:
                return VK_IMAGE_LAYOUT_GENERAL;
            case RDG::ImageLayout::ColorAttachment:
                return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case RDG::ImageLayout::DepthStencilAttachment:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case RDG::ImageLayout::DepthStencilReadOnly:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            case RDG::ImageLayout::ShaderReadOnly:
                return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case RDG::ImageLayout::TransferSrc:
                return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case RDG::ImageLayout::TransferDst:
                return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            case RDG::ImageLayout::Present:
                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // ── Render passes ──────────────────────────────────────────────────────────────────────────────────

    RdgRenderPassKey RdgCompatibleRenderPassKey( const std::vector<VkFormat>& colourFormats, VkFormat depthFormat,
                                                 bool depthHasStencil, uint32_t samples )
    {
        RdgRenderPassKey key;
        key.Samples = samples;
        for ( const VkFormat format : colourFormats )
            key.Colours.push_back( { format, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } );
        if ( depthFormat != VK_FORMAT_UNDEFINED )
        {
            key.Depth =
                 RdgAttachmentKey{ depthFormat, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            key.HasStencil = depthHasStencil;
        }
        return key;
    }

    Common::ResultStr<VkRenderPass> CreateRdgRenderPass( VkDevice device, const RdgRenderPassKey& key )
    {
        const VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>( std::max( 1u, key.Samples ) );
        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference>   colourRefs;
        std::vector<VkAttachmentReference>   resolveRefs;
        auto describe = [&]( const RdgAttachmentKey& attachment, bool stencil, VkSampleCountFlagBits count )
        {
            VkAttachmentDescription description{};
            description.format         = attachment.Format;
            description.samples        = count;
            description.loadOp         = attachment.Load;
            description.storeOp        = attachment.Store;
            description.stencilLoadOp  = stencil ? attachment.Load : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            description.stencilStoreOp = stencil ? attachment.Store : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // The graph's barrier before the pass has already put the image in its attachment layout, and
            // the next barrier starts from it: the render pass itself transitions nothing.
            description.initialLayout = attachment.Layout;
            description.finalLayout   = attachment.Layout;
            attachments.push_back( description );
            return VkAttachmentReference{ static_cast<uint32_t>( attachments.size() - 1 ), attachment.Layout };
        };
        colourRefs.reserve( key.Colours.size() );
        for ( const RdgAttachmentKey& colour : key.Colours )
        {
            colourRefs.push_back( colour.Format == VK_FORMAT_UNDEFINED
                                       ? VkAttachmentReference{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED }
                                       : describe( colour, false, samples ) );
        }
        // Attachment order: colours, depth, resolves (BeginRenderPass hands the views in the same order).
        VkAttachmentReference depthRef{};
        if ( key.Depth )
            depthRef = describe( *key.Depth, key.HasStencil, samples );
        if ( !key.Resolves.empty() && key.Resolves.size() != key.Colours.size() )
            return Common::MakeFormattedError<VkRenderPass>( "render pass with {} colour(s) and {} resolve(s)",
                                                             key.Colours.size(), key.Resolves.size() );
        for ( const RdgAttachmentKey& resolve : key.Resolves )
        {
            resolveRefs.push_back( resolve.Format == VK_FORMAT_UNDEFINED
                                        ? VkAttachmentReference{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED }
                                        : describe( resolve, false, VK_SAMPLE_COUNT_1_BIT ) );
        }

        // The engine's one single-subpass builder: a pipeline built against a framebuffer's render pass (MSAA
        // with its resolves, or single-sample), or against RdgCompatibleRenderPassKey, is compatible with this one.
        VkRenderPass   renderPass = VK_NULL_HANDLE;
        const VkResult result     = CreateSinglePassRenderPass( device, attachments, colourRefs, resolveRefs,
                                                                key.Depth ? &depthRef : nullptr, false, renderPass );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<VkRenderPass>( "vkCreateRenderPass failed ({})",
                                                             static_cast<int>( result ) );
        return Common::MakeSuccess( renderPass );
    }

    // ── Physical resources ─────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<std::shared_ptr<VulkanRdgTexture>> VulkanRdgTexture::Create( const VulkanRdgDevice&  device,
                                                                                   const RDG::TextureDesc& desc,
                                                                                   uint32_t         accessMask,
                                                                                   std::string_view name )
    {
        using Out                    = std::shared_ptr<VulkanRdgTexture>;
        const VkImageCreateInfo info = RdgImageInfo( device, desc, accessMask );
        if ( info.format == VK_FORMAT_UNDEFINED || info.usage == 0 )
            return Common::MakeFormattedError<Out>(
                 "texture '{}': no Vulkan format or no usage (access mask {:#x})", name, accessMask );
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        Out texture( new VulkanRdgTexture() );
        texture->m_Device     = device.Device;
        texture->m_Allocator  = device.Allocator;
        texture->m_Format     = info.format;
        texture->m_Aspect     = GetImageVulkanAspect( desc.Format );
        texture->m_Desc       = desc;
        texture->m_AccessMask = accessMask;
        const VkResult result = vmaCreateImage( device.Allocator, &info, &allocation, &texture->m_Image,
                                                &texture->m_Allocation, nullptr );
        if ( result != VK_SUCCESS )
        {
            texture->m_Image = VK_NULL_HANDLE;
            return Common::MakeFormattedError<Out>(
                 "texture '{}' ({}x{}, {} mips, {} layers): vmaCreateImage failed ({})", name, desc.Size.Width,
                 desc.Size.Height, desc.Mips, desc.Layers, static_cast<int>( result ) );
        }
        return Common::MakeSuccess( std::move( texture ) );
    }

    std::shared_ptr<VulkanRdgTexture> VulkanRdgTexture::Wrap( VkDevice device, VkImage image, VkFormat format,
                                                              const RDG::TextureDesc& desc )
    {
        std::shared_ptr<VulkanRdgTexture> texture( new VulkanRdgTexture() );
        texture->m_Device = device;
        texture->m_Image  = image;
        texture->m_Format = format;
        texture->m_Aspect = GetImageVulkanAspect( desc.Format );
        texture->m_Desc   = desc;
        return texture;
    }

    VulkanRdgTexture::~VulkanRdgTexture()
    {
        for ( const auto& [key, view] : m_Views )
            vkDestroyImageView( m_Device, view, nullptr );
        if ( m_Allocator != nullptr && m_Image != VK_NULL_HANDLE )
            vmaDestroyImage( m_Allocator, m_Image, m_Allocation );
    }

    Common::ResultStr<VkImageView> VulkanRdgTexture::GetView( RDG::SubresourceRange range, bool forAttachment )
    {
        if ( range.MipCount == RDG::kAllRemaining )
            range.MipCount = m_Desc.Mips - range.BaseMip;
        if ( range.LayerCount == RDG::kAllRemaining )
            range.LayerCount = m_Desc.Layers - range.BaseLayer;
        if ( range.MipCount == 0 || range.LayerCount == 0 || range.BaseMip + range.MipCount > m_Desc.Mips ||
             range.BaseLayer + range.LayerCount > m_Desc.Layers )
            return Common::MakeFormattedError<VkImageView>( "view mips [{}, +{}) layers [{}, +{}) outside {} x {}",
                                                            range.BaseMip, range.MipCount, range.BaseLayer,
                                                            range.LayerCount, m_Desc.Mips, m_Desc.Layers );
        const auto key = std::make_tuple( range.BaseMip, range.MipCount, range.BaseLayer,
                                          range.LayerCount | ( forAttachment ? 0x80000000u : 0u ) );
        if ( const auto found = m_Views.find( key ); found != m_Views.end() )
            return Common::MakeSuccess( found->second );

        VkImageViewType type = range.LayerCount == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if ( m_Desc.Dim == RDG::TextureDim::Tex3D )
            type = VK_IMAGE_VIEW_TYPE_3D;
        else if ( !forAttachment && m_Desc.Dim == RDG::TextureDim::Cube && range.LayerCount % 6 == 0 )
            type = range.LayerCount == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;

        VkImageViewCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image            = m_Image;
        info.viewType         = type;
        info.format           = m_Format;
        info.subresourceRange = { m_Aspect, range.BaseMip, range.MipCount, range.BaseLayer, range.LayerCount };
        VkImageView    view   = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView( m_Device, &info, nullptr, &view );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<VkImageView>( "vkCreateImageView failed ({})",
                                                            static_cast<int>( result ) );
        m_Views.emplace( key, view );
        return Common::MakeSuccess( view );
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>> VulkanRdgBuffer::Create( const VulkanRdgDevice& device,
                                                                                 const RDG::BufferDesc& desc,
                                                                                 uint32_t               accessMask,
                                                                                 std::string_view       name )
    {
        using Out                     = std::shared_ptr<VulkanRdgBuffer>;
        const VkBufferCreateInfo info = RdgBufferInfo( desc, accessMask );
        if ( info.size == 0 || info.usage == 0 )
            return Common::MakeFormattedError<Out>( "buffer '{}': {} bytes, usage from access mask {:#x} is empty",
                                                    name, desc.Bytes, accessMask );
        const bool              host = RdgHas( accessMask, RDG::Access::HostRead );
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags =
             host ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT : 0u;

        Out               buffer( new VulkanRdgBuffer() );
        VmaAllocationInfo allocated{};
        buffer->m_Allocator   = device.Allocator;
        buffer->m_Size        = desc.Bytes;
        buffer->m_AccessMask  = accessMask;
        const VkResult result = vmaCreateBuffer( device.Allocator, &info, &allocation, &buffer->m_Buffer,
                                                 &buffer->m_Allocation, &allocated );
        if ( result != VK_SUCCESS )
        {
            buffer->m_Buffer = VK_NULL_HANDLE;
            return Common::MakeFormattedError<Out>( "buffer '{}' ({} bytes): vmaCreateBuffer failed ({})", name,
                                                    desc.Bytes, static_cast<int>( result ) );
        }
        buffer->m_Mapped = host ? allocated.pMappedData : nullptr;
        return Common::MakeSuccess( std::move( buffer ) );
    }

    std::shared_ptr<VulkanRdgBuffer> VulkanRdgBuffer::Wrap( VkBuffer buffer, uint64_t size )
    {
        std::shared_ptr<VulkanRdgBuffer> wrapped( new VulkanRdgBuffer() );
        wrapped->m_Buffer = buffer;
        wrapped->m_Size   = size;
        return wrapped;
    }

    VulkanRdgBuffer::~VulkanRdgBuffer()
    {
        // A wrapped engine buffer has no allocator: its owner destroys it.
        if ( m_Allocator != nullptr && m_Buffer != VK_NULL_HANDLE )
            vmaDestroyBuffer( m_Allocator, m_Buffer, m_Allocation );
    }

    void VulkanRdgBuffer::InvalidateForHost() const
    {
        if ( m_Mapped == nullptr )
            return;
        // A failed invalidate leaves the host view stale; the readback that follows still runs, so the
        // failure is named here with its code rather than dropped.
        const VkResult result = vmaInvalidateAllocation( m_Allocator, m_Allocation, 0, VK_WHOLE_SIZE );
        if ( result != VK_SUCCESS )
        {
            LOG_ERROR( "[RenderGraph] vmaInvalidateAllocation failed on a host-visible graph buffer (VkResult {})",
                       static_cast<int>( result ) );
        }
    }

    // ── Pool ───────────────────────────────────────────────────────────────────────────────────────────

    VulkanRdgPool::VulkanRdgPool( const VulkanRdgDevice& device, uint32_t frameSlots )
         : m_Device( device ), m_Slots( std::max( 1u, frameSlots ) )
    {
    }

    void VulkanRdgPool::BeginFrame( uint32_t slot )
    {
        m_Slot = slot % static_cast<uint32_t>( m_Slots.size() );
        ++m_Frame;
        Slot& current = m_Slots[m_Slot];
        // Kept: whatever this slot's previous frame handed out, and whatever an extraction target still holds.
        auto stale = [&]( const auto& entry )
        { return entry.LastFrame < current.PreviousFrame && entry.Resource.use_count() == 1; };
        std::erase_if( current.Textures, stale );
        std::erase_if( current.Buffers, stale );
        current.PreviousFrame = m_Frame;
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgTexture>>
    VulkanRdgPool::AcquireTexture( const RDG::TextureDesc& desc, uint32_t accessMask, std::string_view name )
    {
        using Out = std::shared_ptr<VulkanRdgTexture>;
        if ( m_Frame == 0 )
            return Common::MakeFormattedError<Out>( "texture '{}': VulkanRdgPool::BeginFrame was never called",
                                                    name );
        Slot& slot = m_Slots[m_Slot];
        for ( Entry<VulkanRdgTexture>& entry : slot.Textures )
        {
            if ( entry.LastFrame != m_Frame && entry.Resource.use_count() == 1 &&
                 entry.Resource->GetAccessMask() == accessMask && RdgSameDesc( entry.Resource->GetDesc(), desc ) )
            {
                entry.LastFrame = m_Frame;
                return Common::MakeSuccess( entry.Resource );
            }
        }
        Common::ResultStr<Out> created = VulkanRdgTexture::Create( m_Device, desc, accessMask, name );
        if ( !created )
            return created;
        slot.Textures.push_back( { created.GetValue(), m_Frame } );
        return created;
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>>
    VulkanRdgPool::AcquireBuffer( const RDG::BufferDesc& desc, uint32_t accessMask, std::string_view name )
    {
        using Out = std::shared_ptr<VulkanRdgBuffer>;
        if ( m_Frame == 0 )
            return Common::MakeFormattedError<Out>( "buffer '{}': VulkanRdgPool::BeginFrame was never called",
                                                    name );
        Slot& slot = m_Slots[m_Slot];
        for ( Entry<VulkanRdgBuffer>& entry : slot.Buffers )
        {
            if ( entry.LastFrame != m_Frame && entry.Resource.use_count() == 1 &&
                 entry.Resource->GetAccessMask() == accessMask && entry.Resource->GetSize() == desc.Bytes )
            {
                entry.LastFrame = m_Frame;
                return Common::MakeSuccess( entry.Resource );
            }
        }
        Common::ResultStr<Out> created = VulkanRdgBuffer::Create( m_Device, desc, accessMask, name );
        if ( !created )
            return created;
        slot.Buffers.push_back( { created.GetValue(), m_Frame } );
        return created;
    }

    size_t VulkanRdgPool::GetTextureCount() const
    {
        size_t count = 0;
        for ( const Slot& slot : m_Slots )
            count += slot.Textures.size();
        return count;
    }

    size_t VulkanRdgPool::GetBufferCount() const
    {
        size_t count = 0;
        for ( const Slot& slot : m_Slots )
            count += slot.Buffers.size();
        return count;
    }

    // ── Memory requirements ────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RDG::MemoryRequirements>
    VulkanRdgMemoryRequirements::GetTextureRequirements( const RDG::TextureDesc& desc, uint32_t accessMask ) const
    {
        const std::string key = fmt::format( "t{}x{}x{}:{}:{}:{}:{}:{:x}", desc.Size.Width, desc.Size.Height,
                                             desc.Size.Depth, static_cast<int>( desc.Format ), desc.Mips,
                                             desc.Layers, static_cast<int>( desc.Dim ), accessMask );
        if ( const auto found = m_Cache.find( key ); found != m_Cache.end() )
            return Common::MakeSuccess( found->second );
        const VkImageCreateInfo info = RdgImageInfo( m_Device, desc, accessMask );
        if ( info.format == VK_FORMAT_UNDEFINED || info.usage == 0 )
            return Common::MakeFormattedError<RDG::MemoryRequirements>(
                 "no Vulkan format or no usage (access mask {:#x})", accessMask );
        VkImage        image  = VK_NULL_HANDLE;
        const VkResult result = vkCreateImage( m_Device.Device, &info, nullptr, &image );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<RDG::MemoryRequirements>( "vkCreateImage failed ({})",
                                                                        static_cast<int>( result ) );
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements( m_Device.Device, image, &requirements );
        vkDestroyImage( m_Device.Device, image, nullptr );
        const RDG::MemoryRequirements out{ requirements.size, requirements.alignment,
                                           requirements.memoryTypeBits };
        m_Cache.emplace( key, out );
        return Common::MakeSuccess( out );
    }

    Common::ResultStr<RDG::MemoryRequirements>
    VulkanRdgMemoryRequirements::GetBufferRequirements( const RDG::BufferDesc& desc, uint32_t accessMask ) const
    {
        const std::string key = fmt::format( "b{}:{:x}", desc.Bytes, accessMask );
        if ( const auto found = m_Cache.find( key ); found != m_Cache.end() )
            return Common::MakeSuccess( found->second );
        const VkBufferCreateInfo info = RdgBufferInfo( desc, accessMask );
        if ( info.size == 0 || info.usage == 0 )
            return Common::MakeFormattedError<RDG::MemoryRequirements>( "{} bytes, no usage (access mask {:#x})",
                                                                        desc.Bytes, accessMask );
        VkBuffer       buffer = VK_NULL_HANDLE;
        const VkResult result = vkCreateBuffer( m_Device.Device, &info, nullptr, &buffer );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<RDG::MemoryRequirements>( "vkCreateBuffer failed ({})",
                                                                        static_cast<int>( result ) );
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements( m_Device.Device, buffer, &requirements );
        vkDestroyBuffer( m_Device.Device, buffer, nullptr );
        const RDG::MemoryRequirements out{ requirements.size, requirements.alignment,
                                           requirements.memoryTypeBits };
        m_Cache.emplace( key, out );
        return Common::MakeSuccess( out );
    }

    // ── Backend ────────────────────────────────────────────────────────────────────────────────────────

    VulkanRdgBackend::VulkanRdgBackend( const VulkanRdgDevice& device, VulkanRdgPool& pool )
         : m_Device( device ), m_Pool( pool ), m_Memory( device )
    {
    }

    VulkanRdgBackend::~VulkanRdgBackend()
    {
        for ( const FramebufferEntry& entry : m_Framebuffers )
            vkDestroyFramebuffer( m_Device.Device, entry.Framebuffer, nullptr );
        for ( const auto& [key, renderPass] : m_RenderPasses )
            vkDestroyRenderPass( m_Device.Device, renderPass, nullptr );
    }

    Common::ResultStr<VkCommandBuffer> VulkanRdgBackend::CommandBufferOf( const RDG::PassContext& context )
    {
        RDG::IBackend& backend = context.GetBackend();
        if ( backend.GetKind() != RDG::BackendKind::Vulkan )
            return Common::MakeFormattedError<VkCommandBuffer>( "pass '{}' is not recorded by the Vulkan backend",
                                                                context.GetPassName() );
        const VkCommandBuffer commandBuffer = static_cast<VulkanRdgBackend&>( backend ).m_CommandBuffer;
        if ( commandBuffer == VK_NULL_HANDLE )
            return Common::MakeFormattedError<VkCommandBuffer>( "pass '{}': the backend has no command buffer",
                                                                context.GetPassName() );
        return Common::MakeSuccess( commandBuffer );
    }

    Common::ResultStr<VulkanRdgTexture*> VulkanRdgBackend::TextureOf( const RDG::TextureBinding& binding )
    {
        if ( binding.Physical == nullptr || binding.Physical->GetBackendKind() != RDG::BackendKind::Vulkan )
            return Common::MakeFormattedError<VulkanRdgTexture*>( "texture '{}' has no Vulkan image",
                                                                  binding.Name );
        return Common::MakeSuccess( static_cast<VulkanRdgTexture*>( binding.Physical ) );
    }

    Common::ResultStr<VulkanRdgBuffer*> VulkanRdgBackend::BufferOf( const RDG::BufferBinding& binding )
    {
        if ( binding.Physical == nullptr || binding.Physical->GetBackendKind() != RDG::BackendKind::Vulkan )
            return Common::MakeFormattedError<VulkanRdgBuffer*>( "buffer '{}' has no Vulkan buffer",
                                                                 binding.Name );
        return Common::MakeSuccess( static_cast<VulkanRdgBuffer*>( binding.Physical ) );
    }

    Common::BoolResultStr VulkanRdgBackend::BeginGraph( const RDG::GraphView& graph )
    {
        if ( m_CommandBuffer == VK_NULL_HANDLE )
            return Common::MakeError( "the Vulkan backend has no command buffer (SetCommandBuffer)" );
        Release();
        // A framebuffer whose images the pool has destroyed is dead: drop it before a new view could reuse
        // the handle it remembers. Called between submissions the caller has waited for (pool contract).
        std::erase_if( m_Framebuffers,
                       [&]( const FramebufferEntry& entry )
                       {
                           const bool dead =
                                std::any_of( entry.Textures.begin(), entry.Textures.end(),
                                             []( const auto& texture ) { return texture.expired(); } );
                           if ( dead )
                               vkDestroyFramebuffer( m_Device.Device, entry.Framebuffer, nullptr );
                           return dead;
                       } );
        m_Textures.resize( graph.Resources.size() );
        m_Buffers.resize( graph.Resources.size() );
        for ( const RDG::ResourceView& view : graph.Resources )
        {
            if ( !view.Used )
                continue;
            if ( view.Kind == RDG::ResourceKind::Texture )
            {
                if ( view.ExternalTex )
                {
                    const std::shared_ptr<RDG::IPhysicalTexture>& physical = view.ExternalTex->Physical;
                    if ( !physical || physical->GetBackendKind() != RDG::BackendKind::Vulkan )
                        return Common::MakeFormattedError( "external texture '{}' carries no Vulkan image",
                                                           view.Name );
                    m_Textures[view.Resource] = std::static_pointer_cast<VulkanRdgTexture>( physical );
                    continue;
                }
                auto acquired = m_Pool.AcquireTexture( *view.Texture, view.AccessMask, view.Name );
                if ( !acquired )
                    return Common::MakeError( acquired.GetError() );
                m_Textures[view.Resource] = acquired.GetValue();
            }
            else
            {
                if ( view.ExternalBuf )
                {
                    const std::shared_ptr<RDG::IPhysicalBuffer>& physical = view.ExternalBuf->Physical;
                    if ( !physical || physical->GetBackendKind() != RDG::BackendKind::Vulkan )
                        return Common::MakeFormattedError( "external buffer '{}' carries no Vulkan buffer",
                                                           view.Name );
                    m_Buffers[view.Resource] = std::static_pointer_cast<VulkanRdgBuffer>( physical );
                    continue;
                }
                auto acquired = m_Pool.AcquireBuffer( *view.Buffer, view.AccessMask, view.Name );
                if ( !acquired )
                    return Common::MakeError( acquired.GetError() );
                m_Buffers[view.Resource] = acquired.GetValue();
            }
        }
        return Common::MakeSuccess( true );
    }

    // THE one place a pass is labelled and timed: every graph pass gets both, and no pass records its own.
    void VulkanRdgBackend::BeginPass( const RDG::CompiledPass& pass )
    {
        if ( m_Device.CmdBeginLabel != nullptr )
        {
            VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
            label.pLabelName = pass.Name.c_str();
            m_Device.CmdBeginLabel( m_CommandBuffer, &label );
        }
#if DESERT_DEV_INSTRUMENTS
        m_CpuScopes.push_back( std::make_unique<Common::Profiling::ScopedTimer>( pass.Name.c_str() ) );
#endif
        m_ProfilerScopes.push_back(
             m_Device.Profiler != nullptr ? m_Device.Profiler->BeginScope( pass.Name.c_str() ) : -1 );
    }

    void VulkanRdgBackend::EndPass( const RDG::CompiledPass& )
    {
        if ( !m_ProfilerScopes.empty() )
        {
            if ( m_Device.Profiler != nullptr )
                m_Device.Profiler->EndScope( m_ProfilerScopes.back() );
            m_ProfilerScopes.pop_back();
#if DESERT_DEV_INSTRUMENTS
            if ( !m_CpuScopes.empty() )
                m_CpuScopes.pop_back();
#endif
        }
        if ( m_Device.CmdEndLabel != nullptr )
            m_Device.CmdEndLabel( m_CommandBuffer );
    }

    void VulkanRdgBackend::RecordBarriers( std::span<const RDG::Barrier> barriers )
    {
        // ONE vkCmdPipelineBarrier: the stage masks are the union over the batch, each resource keeps its
        // own access masks and layouts.
        VkPipelineStageFlags               srcStages = 0;
        VkPipelineStageFlags               dstStages = 0;
        std::vector<VkImageMemoryBarrier>  images;
        std::vector<VkBufferMemoryBarrier> buffers;
        for ( const RDG::Barrier& barrier : barriers )
        {
            srcStages |= RdgVulkanStages( barrier.Before.Stages );
            dstStages |= RdgVulkanStages( barrier.After.Stages );
            // A write-free source has nothing to make available: its stages only order the work.
            const VkAccessFlags srcAccess = RdgVulkanAccess( barrier.Before.Memory & RDG::kWriteAccessMask );
            const VkAccessFlags dstAccess = RdgVulkanAccess( barrier.After.Memory );
            if ( barrier.Kind == RDG::ResourceKind::Texture )
            {
                const VulkanRdgTexture& texture = *m_Textures[barrier.Resource];
                VkImageMemoryBarrier    image{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                image.srcAccessMask       = srcAccess;
                image.dstAccessMask       = dstAccess;
                image.oldLayout           = barrier.DiscardContents ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                    : RdgVulkanLayout( barrier.Before.Layout );
                image.newLayout           = RdgVulkanLayout( barrier.After.Layout );
                image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image.image               = texture.GetImage();
                image.subresourceRange    = { texture.GetAspect(), barrier.Range.BaseMip, barrier.Range.MipCount,
                                              barrier.Range.BaseLayer, barrier.Range.LayerCount };
                images.push_back( image );
            }
            else
            {
                VkBufferMemoryBarrier buffer{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                buffer.srcAccessMask       = srcAccess;
                buffer.dstAccessMask       = dstAccess;
                buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer.buffer              = m_Buffers[barrier.Resource]->GetBuffer();
                buffer.offset              = 0;
                buffer.size                = VK_WHOLE_SIZE;
                buffers.push_back( buffer );
            }
        }
        // v1 has no NONE stage: "nothing before" is TOP_OF_PIPE, "nothing after" (Present) BOTTOM_OF_PIPE.
        if ( srcStages == 0 )
            srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if ( dstStages == 0 )
            dstStages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        vkCmdPipelineBarrier( m_CommandBuffer, srcStages, dstStages, 0, 0, nullptr,
                              static_cast<uint32_t>( buffers.size() ), buffers.data(),
                              static_cast<uint32_t>( images.size() ), images.data() );
    }

    Common::ResultStr<VkRenderPass> VulkanRdgBackend::GetRenderPass( const RdgRenderPassKey& key )
    {
        if ( const auto found = m_RenderPasses.find( key ); found != m_RenderPasses.end() )
            return Common::MakeSuccess( found->second );
        Common::ResultStr<VkRenderPass> created = CreateRdgRenderPass( m_Device.Device, key );
        if ( created )
            m_RenderPasses.emplace( key, created.GetValue() );
        return created;
    }

    Common::BoolResultStr VulkanRdgBackend::BeginRenderPass( const RDG::CompiledPass& pass )
    {
        uint32_t colourCount = 0;
        for ( const RDG::AttachmentDecision& attachment : pass.Attachments )
        {
            if ( !attachment.IsDepth )
                colourCount = std::max( colourCount, attachment.Slot + 1 );
        }
        const bool resolves = std::any_of( pass.Attachments.begin(), pass.Attachments.end(),
                                           []( const RDG::AttachmentDecision& a ) { return a.IsResolve; } );
        // Attachment order in the render pass: colour slots first (unused slots have no attachment), depth
        // last; views and clear values follow the same order.
        RdgRenderPassKey key;
        key.Colours.resize( colourCount, RdgAttachmentKey{} );
        std::vector<std::pair<VkImageView, VkClearValue>> colourViews( colourCount,
                                                                       { VK_NULL_HANDLE, VkClearValue{} } );
        std::pair<VkImageView, VkClearValue>              depthView{ VK_NULL_HANDLE, VkClearValue{} };
        std::vector<VkImageView>                          resolveViews( resolves ? colourCount : 0u, VK_NULL_HANDLE );
        if ( resolves )
            key.Resolves.resize( colourCount, RdgAttachmentKey{} );
        std::vector<std::weak_ptr<VulkanRdgTexture>>      textures;
        VkExtent2D                                        extent{ 0, 0 };
        uint32_t                                          layers = 1;
        for ( const RDG::AttachmentDecision& attachment : pass.Attachments )
        {
            const std::shared_ptr<VulkanRdgTexture>& texture = m_Textures[attachment.Resource];
            auto                                     view =
                 texture->GetView( { attachment.Mip, 1, attachment.BaseLayer, attachment.LayerCount }, true );
            if ( !view )
                return Common::MakeFormattedError( "attachment of '{}': {}", pass.Name, view.GetError() );
            textures.push_back( texture );
            const RDG::TextureDesc& desc = texture->GetDesc();
            extent                       = { std::max( 1u, desc.Size.Width >> attachment.Mip ),
                                             std::max( 1u, desc.Size.Height >> attachment.Mip ) };
            layers                       = attachment.LayerCount;

            const RdgAttachmentKey described{ texture->GetFormat(), RdgLoadOp( attachment.Load ),
                                              attachment.Store == RDG::StoreAction::Store
                                                   ? VK_ATTACHMENT_STORE_OP_STORE
                                                   : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                              RdgVulkanLayout( RDG::GetAccessState( attachment.Usage ).Layout ) };
            VkClearValue           clear{};
            if ( attachment.IsResolve )
            {
                key.Resolves[attachment.Slot]   = described;
                resolveViews[attachment.Slot]   = view.GetValue();
                continue;
            }
            key.Samples = std::max( 1u, desc.Samples );
            if ( attachment.IsDepth )
            {
                clear.depthStencil = { attachment.Clear.Depth, attachment.Clear.Stencil };
                key.Depth          = described;
                key.HasStencil     = ( texture->GetAspect() & VK_IMAGE_ASPECT_STENCIL_BIT ) != 0;
                depthView          = { view.GetValue(), clear };
            }
            else
            {
                std::copy( attachment.Clear.Color.begin(), attachment.Clear.Color.end(), clear.color.float32 );
                key.Colours[attachment.Slot] = described;
                colourViews[attachment.Slot] = { view.GetValue(), clear };
            }
        }
        Common::ResultStr<VkRenderPass> renderPass = GetRenderPass( key );
        if ( !renderPass )
            return Common::MakeFormattedError( "render pass of '{}': {}", pass.Name, renderPass.GetError() );

        std::vector<VkImageView>  views;
        std::vector<VkClearValue> clears;
        for ( const auto& [view, clear] : colourViews )
        {
            if ( view != VK_NULL_HANDLE )
            {
                views.push_back( view );
                clears.push_back( clear );
            }
        }
        if ( depthView.first != VK_NULL_HANDLE )
        {
            views.push_back( depthView.first );
            clears.push_back( depthView.second );
        }
        for ( const VkImageView view : resolveViews )
        {
            if ( view != VK_NULL_HANDLE )
            {
                views.push_back( view );
                clears.push_back( VkClearValue{} );
            }
        }

        // The pool hands out the same images while the graph is unchanged, so the same views come back
        // and the framebuffer is found here instead of being rebuilt every frame.
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        for ( const FramebufferEntry& entry : m_Framebuffers )
        {
            if ( entry.RenderPass == renderPass.GetValue() && entry.Views == views &&
                 entry.Extent.width == extent.width && entry.Extent.height == extent.height &&
                 entry.Layers == layers )
                framebuffer = entry.Framebuffer;
        }
        if ( framebuffer == VK_NULL_HANDLE )
        {
            VkFramebufferCreateInfo info{};
            info.sType            = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass       = renderPass.GetValue();
            info.attachmentCount  = static_cast<uint32_t>( views.size() );
            info.pAttachments     = views.data();
            info.width            = extent.width;
            info.height           = extent.height;
            info.layers           = layers;
            const VkResult result = vkCreateFramebuffer( m_Device.Device, &info, nullptr, &framebuffer );
            if ( result != VK_SUCCESS )
                return Common::MakeFormattedError( "framebuffer of '{}': vkCreateFramebuffer failed ({})",
                                                   pass.Name, static_cast<int>( result ) );
            m_Framebuffers.push_back(
                 { renderPass.GetValue(), views, std::move( textures ), extent, layers, framebuffer } );
        }

        VkRenderPassBeginInfo begin{};
        begin.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass      = renderPass.GetValue();
        begin.framebuffer     = framebuffer;
        begin.renderArea      = { { 0, 0 }, extent };
        begin.clearValueCount = static_cast<uint32_t>( clears.size() );
        begin.pClearValues    = clears.data();
        vkCmdBeginRenderPass( m_CommandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE );
        m_RenderPassOpen = true;

        // The whole target: what nearly every pass wants, set once here instead of in every exec lambda. The
        // engine's convention (VulkanRendererAPI::SetViewportAndScissor): negative height, so +Y is up and the
        // engine's pipelines draw the same picture in a graph-opened render pass as in their own.
        const VkViewport viewport{ 0.0f,
                                   static_cast<float>( extent.height ),
                                   static_cast<float>( extent.width ),
                                   -static_cast<float>( extent.height ),
                                   0.0f,
                                   1.0f };
        const VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport( m_CommandBuffer, 0, 1, &viewport );
        vkCmdSetScissor( m_CommandBuffer, 0, 1, &scissor );
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::EndRenderPass()
    {
        vkCmdEndRenderPass( m_CommandBuffer );
        m_RenderPassOpen = false;
    }

    Common::BoolResultStr VulkanRdgBackend::EndGraph( std::span<const RDG::Barrier> finalBarriers )
    {
        if ( !finalBarriers.empty() )
            RecordBarriers( finalBarriers );
        Release();
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::AbandonGraph()
    {
        if ( m_RenderPassOpen )
            EndRenderPass();
        while ( !m_ProfilerScopes.empty() )
            EndPass( RDG::CompiledPass{} );
        Release();
    }

    std::shared_ptr<RDG::IPhysicalTexture> VulkanRdgBackend::GetPhysicalTexture( uint32_t resource ) const
    {
        return resource < m_Textures.size() ? m_Textures[resource] : nullptr;
    }

    std::shared_ptr<RDG::IPhysicalBuffer> VulkanRdgBackend::GetPhysicalBuffer( uint32_t resource ) const
    {
        return resource < m_Buffers.size() ? m_Buffers[resource] : nullptr;
    }

    void VulkanRdgBackend::Release()
    {
        m_Textures.clear();
        m_Buffers.clear();
    }
} // namespace Desert::Graphic::API::Vulkan
