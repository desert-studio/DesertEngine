#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanFormat.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgTransient.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderPassDependencies.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <atomic>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <cstdio>
#include <cstdlib>
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
            VkImageCreateInfo info{};
            info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.flags         = desc.Dim == RDG::TextureDim::Cube || desc.Dim == RDG::TextureDim::CubeArray
                                      ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
                                      : 0u;
            info.imageType     = desc.Dim == RDG::TextureDim::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            info.format        = format;
            info.extent        = { desc.Size.Width, desc.Size.Height,
                            desc.Dim == RDG::TextureDim::Tex3D ? desc.Size.Depth : 1u };
            info.mipLevels     = desc.Mips;
            info.arrayLayers   = desc.Layers;
            info.samples       = static_cast<VkSampleCountFlagBits>( std::max( 1u, desc.Samples ) );
            info.tiling        = VK_IMAGE_TILING_OPTIMAL;
            info.usage         = RdgImageUsage( accessMask );
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            // The device's sharing rule (VulkanResourceSharing.hpp); the info points into @p device.Sharing.
            device.Sharing.ApplyTo( info );
            return info;
        }

        VkBufferCreateInfo RdgBufferInfo( const VulkanRdgDevice& device, const RDG::BufferDesc& desc,
                                          uint32_t accessMask )
        {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size  = desc.Bytes;
            info.usage = RdgBufferUsage( accessMask );
            // The device's sharing rule (VulkanResourceSharing.hpp); the info points into @p device.Sharing.
            device.Sharing.ApplyTo( info );
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

    VkImageUsageFlags RdgImageUsage( uint32_t accessMask )
    {
        using RDG::Access;
        VkImageUsageFlags usage = 0;
        if ( RdgHas( accessMask, Access::SampledGraphics ) || RdgHas( accessMask, Access::SampledCompute ) )
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if ( RdgHas( accessMask, Access::StorageRead ) || RdgHas( accessMask, Access::StorageWrite ) )
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if ( RdgHas( accessMask, Access::ExternalWrite ) )
            usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ( RdgHas( accessMask, Access::ColorTarget ) )
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if ( RdgHas( accessMask, Access::DepthWrite ) || RdgHas( accessMask, Access::DepthRead ) )
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ( RdgHas( accessMask, Access::CopySrc ) )
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ( RdgHas( accessMask, Access::CopyDst ) )
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
        // A build's scratch is written as AccelStructBuildWrite too, and Vulkan requires scratch memory to be a
        // storage buffer (VkAccelerationStructureBuildGeometryInfoKHR::scratchData VUID-03674).
        if ( RdgHas( accessMask, Access::AccelStructBuildWrite ) )
            usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        return usage;
    }

    // Vulkan 1.0 barriers (owner decision 2026-09-27: no synchronization2). The graph's stages are finer
    // than v1 in one place: Copy becomes TRANSFER.
    namespace
    {
        // A device fact, written once at device creation (SetRdgRayTracingShaderStages) before any graph runs.
        std::atomic<bool> g_RdgRayTracingShaderStages{ false };
    } // namespace

    void SetRdgRayTracingShaderStages( const bool enabled )
    {
        g_RdgRayTracingShaderStages.store( enabled, std::memory_order_relaxed );
    }

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
        if ( ( stages & RDG::PipelineStage_ComputeShader ) != 0 &&
             g_RdgRayTracingShaderStages.load( std::memory_order_relaxed ) )
            out |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
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

    RdgRenderPassKey RdgCompatibilityKey( const RdgRenderPassKey& key )
    {
        std::vector<VkFormat> colourFormats;
        colourFormats.reserve( key.Colours.size() );
        for ( const RdgAttachmentKey& colour : key.Colours )
            colourFormats.push_back( colour.Format );
        return RdgCompatibleRenderPassKey( colourFormats, key.Depth ? key.Depth->Format : VK_FORMAT_UNDEFINED,
                                           key.Depth && key.HasStencil, std::max( 1u, key.Samples ) );
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
        resolveRefs.reserve( key.Resolves.size() );
        for ( const RdgAttachmentKey& resolve : key.Resolves )
        {
            resolveRefs.push_back( resolve.Format == VK_FORMAT_UNDEFINED
                                        ? VkAttachmentReference{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED }
                                        : describe( resolve, false, VK_SAMPLE_COUNT_1_BIT ) );
        }

        // The engine's one single-subpass builder: a pipeline built against a framebuffer's render pass (MSAA
        // with its resolves, or single-sample), or against RdgCompatibleRenderPassKey, is compatible with this
        // one.
        VkRenderPass   renderPass = VK_NULL_HANDLE;
        const VkResult result     = CreateSinglePassRenderPass( device, attachments, colourRefs, resolveRefs,
                                                            key.Depth ? &depthRef : nullptr, false, renderPass );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<VkRenderPass>( "vkCreateRenderPass failed ({})",
                                                             static_cast<int>( result ) );
        return Common::MakeSuccess( renderPass );
    }

    // ── Physical resources ─────────────────────────────────────────────────────────────────────────────

    namespace
    {
        // The ledger kind of a graph image, by its dimension.
        Graphic::ResourceKind RdgLedgerKind( const RDG::TextureDim dim )
        {
            switch ( dim )
            {
                case RDG::TextureDim::Tex3D:
                    return Graphic::ResourceKind::Image3D;
                case RDG::TextureDim::Cube:
                case RDG::TextureDim::CubeArray:
                    return Graphic::ResourceKind::ImageCube;
                case RDG::TextureDim::Tex2D:
                    break;
            }
            return Graphic::ResourceKind::Image2D;
        }

        // THE LEASE (VulkanRdgPool::AcquireTexture / AcquireBuffer). The pool keeps one reference to each
        // resource; a hand-out is a pointer that shares ownership with a Lease holding a second one. While any
        // copy of the hand-out lives the pool sees use_count() == 2 and does not hand the resource out again
        // (unchanged rule); the last copy dropped destroys the Lease, which returns the ledger row to the device —
        // the pool holds the memory from then on, whoever extracted it. The extractor never has to say it let go.
        template <class T>
        std::shared_ptr<T> LeaseTo( const std::shared_ptr<T>& resource, const ResourceAttribution& holder )
        {
            struct Lease
            {
                explicit Lease( std::shared_ptr<T> leased ) : Resource( std::move( leased ) )
                {
                }
                Lease( const Lease& )            = delete;
                Lease& operator=( const Lease& ) = delete;
                Lease( Lease&& )                 = delete;
                Lease& operator=( Lease&& )      = delete;
                ~Lease()
                {
                    Resource->ClaimLedger( ResourceAttribution{ .Owner = ResourceOwner::Device } );
                }
                std::shared_ptr<T> Resource;
            };
            resource->ClaimLedger( holder );
            const std::shared_ptr<Lease> lease = std::make_shared<Lease>( resource );
            return std::shared_ptr<T>( lease, lease->Resource.get() );
        }
    } // namespace

    namespace
    {
        AllocationLedger& MutableRdgDedicatedLedger()
        {
            static AllocationLedger ledger;
            return ledger;
        }
        uint64_t RdgLedgerHandle( VmaAllocation allocation )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            return reinterpret_cast<uint64_t>( allocation );
        }
    } // namespace

    const AllocationLedger& RdgDedicatedLedger()
    {
        return MutableRdgDedicatedLedger();
    }

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
        VmaAllocationInfo allocated{};
        const VkResult    result = vmaCreateImage( device.Allocator, &info, &allocation, &texture->m_Image,
                                                   &texture->m_Allocation, &allocated );
        if ( result != VK_SUCCESS )
        {
            texture->m_Image = VK_NULL_HANDLE;
            return Common::MakeFormattedError<Out>(
                 "texture '{}' ({}x{}, {} mips, {} layers): vmaCreateImage failed ({})", name, desc.Size.Width,
                 desc.Size.Height, desc.Mips, desc.Layers, static_cast<int>( result ) );
        }
        // A dedicated allocation is a ledger row of its own (the pool claims it for its holder on every hand-out).
        texture->m_Ledger =
             ResourceOwnership::Take( RdgLedgerKind( desc.Dim ), static_cast<std::size_t>( allocated.size ) );
        vmaSetAllocationName( device.Allocator, texture->m_Allocation, std::format( "RDG {}", name ).c_str() );
        MutableRdgDedicatedLedger().Record( RdgLedgerHandle( texture->m_Allocation ), std::string( name ),
                                            static_cast<std::size_t>( allocated.size ) );
        return Common::MakeSuccess( std::move( texture ) );
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgTexture>>
    VulkanRdgTexture::CreatePlaced( const VulkanRdgDevice& device, const RDG::TextureDesc& desc,
                                    uint32_t accessMask, VmaAllocation memory, uint64_t offset,
                                    std::string_view name )
    {
        using Out                    = std::shared_ptr<VulkanRdgTexture>;
        const VkImageCreateInfo info = RdgImageInfo( device, desc, accessMask );
        if ( info.format == VK_FORMAT_UNDEFINED || info.usage == 0 )
            return Common::MakeFormattedError<Out>(
                 "texture '{}': no Vulkan format or no usage (access mask {:#x})", name, accessMask );
        Out texture( new VulkanRdgTexture() );
        texture->m_Device     = device.Device;
        texture->m_Allocator  = device.Allocator; // m_Allocation stays null: the heap's memory is not ours
        texture->m_Format     = info.format;
        texture->m_Aspect     = GetImageVulkanAspect( desc.Format );
        texture->m_Desc       = desc;
        texture->m_AccessMask = accessMask;
        VkResult result       = vkCreateImage( device.Device, &info, nullptr, &texture->m_Image );
        if ( result != VK_SUCCESS )
        {
            texture->m_Image = VK_NULL_HANDLE;
            return Common::MakeFormattedError<Out>( "texture '{}': vkCreateImage failed ({})", name,
                                                    static_cast<int>( result ) );
        }
        // The plan measured this create info (VulkanRdgMemoryRequirements); the heap must still hold it here.
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements( device.Device, texture->m_Image, &requirements );
        VmaAllocationInfo heap{};
        vmaGetAllocationInfo( device.Allocator, memory, &heap );
        if ( offset % requirements.alignment != 0 || offset + requirements.size > heap.size ||
             ( requirements.memoryTypeBits & ( 1u << heap.memoryType ) ) == 0 )
            return Common::MakeFormattedError<Out>( "texture '{}': {} bytes aligned {} (types {:#x}) do not fit "
                                                    "at offset {} of a {}-byte heap of type {}",
                                                    name, requirements.size, requirements.alignment,
                                                    requirements.memoryTypeBits, offset, heap.size,
                                                    heap.memoryType );
        result = vmaBindImageMemory2( device.Allocator, memory, offset, texture->m_Image, nullptr );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<Out>( "texture '{}': vmaBindImageMemory2 at {} failed ({})", name,
                                                    offset, static_cast<int>( result ) );
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

    bool VulkanRdgTexture::Matches( const RDG::TextureDesc& desc, uint32_t accessMask ) const
    {
        return m_AccessMask == accessMask && RdgSameDesc( m_Desc, desc );
    }

    VulkanRdgTexture::~VulkanRdgTexture()
    {
        for ( const auto& [key, view] : m_Views )
            vkDestroyImageView( m_Device, view, nullptr );
        if ( m_Allocation != nullptr )
            MutableRdgDedicatedLedger().Release( RdgLedgerHandle( m_Allocation ) );
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
        {
            const std::shared_lock<std::shared_mutex> lookup( m_ViewsLock );
            if ( const auto found = m_Views.find( key ); found != m_Views.end() )
                return Common::MakeSuccess( found->second );
        }

        VkImageViewType type = range.LayerCount == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if ( m_Desc.Dim == RDG::TextureDim::Tex3D )
            type = VK_IMAGE_VIEW_TYPE_3D;
        else if ( !forAttachment && m_Desc.Dim == RDG::TextureDim::Cube && range.LayerCount % 6 == 0 )
            type = range.LayerCount == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        else if ( !forAttachment && m_Desc.Dim == RDG::TextureDim::CubeArray && range.LayerCount % 6 == 0 )
            type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        else if ( !forAttachment && m_Desc.Dim == RDG::TextureDim::Tex2DArray )
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

        VkImageViewCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image            = m_Image;
        info.viewType         = type;
        info.format           = m_Format;
        info.subresourceRange = { m_Aspect, range.BaseMip, range.MipCount, range.BaseLayer, range.LayerCount };
        // Created under the write lock: two threads asking for the same new view get one view.
        const std::unique_lock<std::shared_mutex> create( m_ViewsLock );
        if ( const auto found = m_Views.find( key ); found != m_Views.end() )
            return Common::MakeSuccess( found->second );
        VkImageView    view   = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView( m_Device, &info, nullptr, &view );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<VkImageView>( "vkCreateImageView failed ({})",
                                                            static_cast<int>( result ) );
        m_Views.emplace( key, view );
        return Common::MakeSuccess( view );
    }

    Common::ResultStr<VkImageView> SampledSubresourceView( const RDG::SubresourceRange& range, VkImageView whole,
                                                           const std::function<VkImageView( uint32_t )>& mipView,
                                                           VulkanRdgTexture* graphTexture )
    {
        if ( range == RDG::SubresourceRange::All() )
        {
            if ( whole == VK_NULL_HANDLE )
                return Common::MakeFormattedError<VkImageView>( "the image has no view" );
            return Common::MakeSuccess( whole );
        }
        if ( range.MipCount == 1 && range.BaseLayer == 0 && range.LayerCount == RDG::kAllRemaining )
        {
            VkImageView view = mipView( range.BaseMip );
            if ( view == VK_NULL_HANDLE )
                return Common::MakeFormattedError<VkImageView>( "no view of mip {}", range.BaseMip );
            return Common::MakeSuccess( view );
        }
        if ( graphTexture == nullptr )
            return Common::MakeFormattedError<VkImageView>(
                 "mips [{}, +{}) layers [{}, +{}) name a layer range of an image the graph did not import",
                 range.BaseMip, range.MipCount, range.BaseLayer, range.LayerCount );
        constexpr VkImageAspectFlags kDepthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if ( ( graphTexture->GetAspect() & kDepthStencil ) == kDepthStencil )
            return Common::MakeFormattedError<VkImageView>(
                 "a layer of a packed depth-stencil image has no single sampleable aspect" );
        return graphTexture->GetView( range );
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>> VulkanRdgBuffer::Create( const VulkanRdgDevice& device,
                                                                                 const RDG::BufferDesc& desc,
                                                                                 uint32_t               accessMask,
                                                                                 std::string_view       name )
    {
        using Out                     = std::shared_ptr<VulkanRdgBuffer>;
        const VkBufferCreateInfo info = RdgBufferInfo( device, desc, accessMask );
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
        // A dedicated allocation is a ledger row of its own, as VulkanRdgTexture::Create's.
        buffer->m_Ledger = ResourceOwnership::Take( Graphic::ResourceKind::StorageBuffer,
                                                    static_cast<std::size_t>( allocated.size ) );
        vmaSetAllocationName( device.Allocator, buffer->m_Allocation, std::format( "RDG {}", name ).c_str() );
        MutableRdgDedicatedLedger().Record( RdgLedgerHandle( buffer->m_Allocation ), std::string( name ),
                                            static_cast<std::size_t>( allocated.size ) );
        return Common::MakeSuccess( std::move( buffer ) );
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>>
    VulkanRdgBuffer::CreatePlaced( const VulkanRdgDevice& device, const RDG::BufferDesc& desc, uint32_t accessMask,
                                   VmaAllocation memory, uint64_t offset, std::string_view name )
    {
        using Out                     = std::shared_ptr<VulkanRdgBuffer>;
        const VkBufferCreateInfo info = RdgBufferInfo( device, desc, accessMask );
        if ( info.size == 0 || info.usage == 0 )
            return Common::MakeFormattedError<Out>( "buffer '{}': {} bytes, usage from access mask {:#x} is empty",
                                                    name, desc.Bytes, accessMask );
        // The host reads a buffer after its graph; a transient that dies with the graph cannot be read there.
        if ( RdgHas( accessMask, RDG::Access::HostRead ) )
            return Common::MakeFormattedError<Out>( "buffer '{}': a HostRead buffer must be extracted, not placed",
                                                    name );
        Out buffer( new VulkanRdgBuffer() );
        buffer->m_Allocator  = device.Allocator; // m_Allocation stays null: the heap's memory is not ours
        buffer->m_Size       = desc.Bytes;
        buffer->m_AccessMask = accessMask;
        VkResult result      = vkCreateBuffer( device.Device, &info, nullptr, &buffer->m_Buffer );
        if ( result != VK_SUCCESS )
        {
            buffer->m_Buffer = VK_NULL_HANDLE;
            return Common::MakeFormattedError<Out>( "buffer '{}': vkCreateBuffer failed ({})", name,
                                                    static_cast<int>( result ) );
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements( device.Device, buffer->m_Buffer, &requirements );
        VmaAllocationInfo heap{};
        vmaGetAllocationInfo( device.Allocator, memory, &heap );
        if ( offset % requirements.alignment != 0 || offset + requirements.size > heap.size ||
             ( requirements.memoryTypeBits & ( 1u << heap.memoryType ) ) == 0 )
            return Common::MakeFormattedError<Out>( "buffer '{}': {} bytes aligned {} (types {:#x}) do not fit at "
                                                    "offset {} of a {}-byte heap of type {}",
                                                    name, requirements.size, requirements.alignment,
                                                    requirements.memoryTypeBits, offset, heap.size,
                                                    heap.memoryType );
        result = vmaBindBufferMemory2( device.Allocator, memory, offset, buffer->m_Buffer, nullptr );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<Out>( "buffer '{}': vmaBindBufferMemory2 at {} failed ({})", name,
                                                    offset, static_cast<int>( result ) );
        return Common::MakeSuccess( std::move( buffer ) );
    }

    std::shared_ptr<VulkanRdgBuffer> VulkanRdgBuffer::Wrap( VkBuffer buffer, uint64_t size, uint64_t offset )
    {
        std::shared_ptr<VulkanRdgBuffer> wrapped( new VulkanRdgBuffer() );
        wrapped->m_Buffer = buffer;
        wrapped->m_Size   = size;
        wrapped->m_Offset = offset;
        return wrapped;
    }

    std::shared_ptr<VulkanRdgBuffer>
    VulkanRdgBuffer::WrapAccelerationStructure( VkBuffer buffer, uint64_t size,
                                                VkAccelerationStructureKHR accelerationStructure )
    {
        std::shared_ptr<VulkanRdgBuffer> wrapped = Wrap( buffer, size );
        wrapped->m_AccelerationStructure         = accelerationStructure;
        return wrapped;
    }

    VulkanRdgBuffer::~VulkanRdgBuffer()
    {
        // A wrapped engine buffer has no allocator: its owner destroys it.
        if ( m_Allocation != nullptr )
            MutableRdgDedicatedLedger().Release( RdgLedgerHandle( m_Allocation ) );
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

    std::span<const std::byte> VulkanRdgBuffer::ReadForHost() const
    {
        if ( m_Mapped == nullptr )
            return {};
        InvalidateForHost();
        return { static_cast<const std::byte*>( m_Mapped ), static_cast<std::size_t>( m_Size ) };
    }

    // ── Pool ───────────────────────────────────────────────────────────────────────────────────────────

    VulkanRdgPool::VulkanRdgPool( const VulkanRdgDevice& device, uint32_t frameSlots )
         : m_Device( device ), m_FrameSlots( std::max( 1u, frameSlots ) )
    {
    }

    void VulkanRdgPool::BeginFrame()
    {
        const uint64_t frame = m_Frame.fetch_add( 1 ) + 1;
        // Every frame up to frame - m_FrameSlots has completed: what was retired on one of them is released.
        // Taken under the lock and released outside it, so a view's destructor never runs with it held.
        std::vector<RetiredTexture> released;
        {
            const std::lock_guard lock( m_RetiredMutex );
            const auto            keptEnd =
                 std::partition( m_Retired.begin(), m_Retired.end(),
                                 [this]( const RetiredTexture& retired ) { return !Fenced( retired.Frame ); } );
            released.assign( std::make_move_iterator( keptEnd ), std::make_move_iterator( m_Retired.end() ) );
            m_Retired.erase( keptEnd, m_Retired.end() );
        }
        released.clear();
        // Kept: whatever a frame not yet waited for was handed, and whatever an extraction target still holds.
        auto stale = [&]( const auto& entry )
        { return entry.LastFrame + m_FrameSlots < frame && entry.Resource.use_count() == 1; };
        std::erase_if( m_Textures, stale );
        std::erase_if( m_Buffers, stale );
    }

    void VulkanRdgPool::Retire( std::shared_ptr<RDG::IPhysicalTexture> texture )
    {
        if ( !texture )
            return;
        const uint64_t        frame = m_Frame.load();
        const std::lock_guard lock( m_RetiredMutex );
        m_Retired.push_back( { std::move( texture ), frame } );
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgTexture>>
    VulkanRdgPool::AcquireTexture( const RDG::TextureDesc& desc, uint32_t accessMask, std::string_view name,
                                   const ResourceAttribution& holder )
    {
        using Out = std::shared_ptr<VulkanRdgTexture>;
        if ( m_Frame == 0 )
            return Common::MakeFormattedError<Out>( "texture '{}': VulkanRdgPool::BeginFrame was never called",
                                                    name );
        for ( Entry<VulkanRdgTexture>& entry : m_Textures )
        {
            if ( Fenced( entry.LastFrame ) && entry.Resource.use_count() == 1 &&
                 entry.Resource->Matches( desc, accessMask ) )
            {
                entry.LastFrame = m_Frame;
                return Common::MakeSuccess( LeaseTo( entry.Resource, holder ) );
            }
        }
        Common::ResultStr<Out> created = VulkanRdgTexture::Create( m_Device, desc, accessMask, name );
        if ( !created )
            return created;
        m_Textures.push_back( { created.GetValue(), m_Frame } );
        return Common::MakeSuccess( LeaseTo( m_Textures.back().Resource, holder ) );
    }

    Common::ResultStr<std::shared_ptr<VulkanRdgBuffer>>
    VulkanRdgPool::AcquireBuffer( const RDG::BufferDesc& desc, uint32_t accessMask, std::string_view name,
                                  const ResourceAttribution& holder )
    {
        using Out = std::shared_ptr<VulkanRdgBuffer>;
        if ( m_Frame == 0 )
            return Common::MakeFormattedError<Out>( "buffer '{}': VulkanRdgPool::BeginFrame was never called",
                                                    name );
        for ( Entry<VulkanRdgBuffer>& entry : m_Buffers )
        {
            if ( Fenced( entry.LastFrame ) && entry.Resource.use_count() == 1 &&
                 entry.Resource->GetAccessMask() == accessMask && entry.Resource->GetSize() == desc.Bytes )
            {
                entry.LastFrame = m_Frame;
                return Common::MakeSuccess( LeaseTo( entry.Resource, holder ) );
            }
        }
        Common::ResultStr<Out> created = VulkanRdgBuffer::Create( m_Device, desc, accessMask, name );
        if ( !created )
            return created;
        m_Buffers.push_back( { created.GetValue(), m_Frame } );
        return Common::MakeSuccess( LeaseTo( m_Buffers.back().Resource, holder ) );
    }

    size_t VulkanRdgPool::GetTextureCount() const
    {
        return m_Textures.size();
    }

    size_t VulkanRdgPool::GetBufferCount() const
    {
        return m_Buffers.size();
    }

    // ── Memory requirements ────────────────────────────────────────────────────────────────────────────

    namespace
    {
        // boost::hash_combine's mix: every field moves the whole seed.
        size_t RdgMemoryKeyMix( const size_t seed, const uint64_t value )
        {
            return seed ^ ( std::hash<uint64_t>{}( value ) + 0x9e3779b97f4a7c15ULL + ( seed << 6 ) + ( seed >> 2 ) );
        }
    } // namespace

    size_t VulkanRdgMemoryRequirements::KeyHash::operator()( const TextureKey& key ) const noexcept
    {
        size_t seed = 0;
        for ( const uint32_t field : { key.Width, key.Height, key.Depth, key.Format, key.Mips, key.Layers, key.Dim,
                                       key.Samples, key.AccessMask } )
            seed = RdgMemoryKeyMix( seed, field );
        return seed;
    }

    size_t VulkanRdgMemoryRequirements::KeyHash::operator()( const BufferKey& key ) const noexcept
    {
        return RdgMemoryKeyMix( RdgMemoryKeyMix( 0, key.Bytes ), key.AccessMask );
    }

    Common::ResultStr<RDG::MemoryRequirements>
    VulkanRdgMemoryRequirements::GetTextureRequirements( const RDG::TextureDesc& desc, uint32_t accessMask ) const
    {
        TextureKey key;
        key.Width      = desc.Size.Width;
        key.Height     = desc.Size.Height;
        key.Depth      = desc.Size.Depth;
        key.Format     = static_cast<uint32_t>( desc.Format );
        key.Mips       = desc.Mips;
        key.Layers     = desc.Layers;
        key.Dim        = static_cast<uint32_t>( desc.Dim );
        key.Samples    = desc.Samples;
        key.AccessMask = accessMask;
        if ( const auto found = m_TextureCache.find( key ); found != m_TextureCache.end() )
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
        m_TextureCache.emplace( key, out );
        return Common::MakeSuccess( out );
    }

    Common::ResultStr<RDG::MemoryRequirements>
    VulkanRdgMemoryRequirements::GetBufferRequirements( const RDG::BufferDesc& desc, uint32_t accessMask ) const
    {
        const BufferKey key{ desc.Bytes, accessMask };
        if ( const auto found = m_BufferCache.find( key ); found != m_BufferCache.end() )
            return Common::MakeSuccess( found->second );
        const VkBufferCreateInfo info = RdgBufferInfo( m_Device, desc, accessMask );
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
        m_BufferCache.emplace( key, out );
        return Common::MakeSuccess( out );
    }

    // ── Pass descriptors ────────────────────────────────────────────────────────────────────────────

    VulkanRdgPassDescriptors::VulkanRdgPassDescriptors( VkDevice device, VulkanSamplerCache& samplers,
                                                        uint32_t frameSlots, bool accelerationStructures )
         : m_Device( device ), m_Slots( std::max( 1u, frameSlots ), std::vector<SlotPools>( 1 ) ),
           m_SamplerCache( samplers ), m_AccelerationStructures( accelerationStructures )
    {
    }

    VulkanRdgPassDescriptors::~VulkanRdgPassDescriptors()
    {
        for ( const std::vector<SlotPools>& slot : m_Slots )
        {
            for ( const SlotPools& arena : slot )
            {
                for ( VkDescriptorPool pool : arena.Pools )
                    vkDestroyDescriptorPool( m_Device, pool, nullptr );
            }
        }
        // Destroying a pool frees every set still allocated from it (cached and retired alike).
        for ( VkDescriptorPool pool : m_CachedPools )
            vkDestroyDescriptorPool( m_Device, pool, nullptr );
    }

    uint64_t NextRdgPhysicalIdentity() noexcept
    {
        static std::atomic<uint64_t> next{ 1 };
        return next.fetch_add( 1, std::memory_order_relaxed );
    }

    namespace
    {
        VkFilter RdgVulkanFilter( RDG::SamplerFilter filter )
        {
            return filter == RDG::SamplerFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        }

        VkSamplerAddressMode RdgVulkanAddress( RDG::SamplerAddress address )
        {
            switch ( address )
            {
                case RDG::SamplerAddress::ClampToEdge:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case RDG::SamplerAddress::Repeat:
                    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case RDG::SamplerAddress::MirroredRepeat:
                    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            }
            std::abort(); // unreachable: the switch names every enumerator
        }
    } // namespace

    VkSamplerCreateInfo RdgSamplerCreateInfo( const RDG::SamplerDesc& desc )
    {
        VkSamplerCreateInfo info{};
        info.sType             = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter         = RdgVulkanFilter( desc.MagFilter );
        info.minFilter         = RdgVulkanFilter( desc.MinFilter );
        info.mipmapMode        = desc.MipMode == RDG::SamplerMipMode::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU      = RdgVulkanAddress( desc.AddressU );
        info.addressModeV      = RdgVulkanAddress( desc.AddressV );
        info.addressModeW      = RdgVulkanAddress( desc.AddressW );
        info.minLod            = 0.0f;
        info.maxLod            = VK_LOD_CLAMP_NONE;
        info.borderColor       = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        return info;
    }

    Common::ResultStr<VkSampler> VulkanRdgPassDescriptors::GetSampler( const RDG::SamplerDesc& desc )
    {
        return m_SamplerCache.Acquire( RdgSamplerCreateInfo( desc ) );
    }

    Common::ResultStr<VkDescriptorPool> VulkanRdgPassDescriptors::AddPool( SlotPools& slot )
    {
        // A full pool is followed by another, never enlarged in place.
        const Common::ResultStr<VkDescriptorPool> pool = CreatePool( 0 );
        if ( !pool )
            return pool;
        slot.Pools.push_back( pool.GetValue() );
        return pool;
    }

    Common::ResultStr<VkDescriptorPool> VulkanRdgPassDescriptors::CreatePool( VkDescriptorPoolCreateFlags flags )
    {
        // One pool shape for every pass.
        constexpr uint32_t                        kSets = 256;
        const std::array<VkDescriptorPoolSize, 6> sizes = {
             VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets * 4 },
             VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets * 4 },
             VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_SAMPLER, kSets },
             VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets * 2 },
             VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets * 2 },
             VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kSets } };
        VkDescriptorPoolCreateInfo info{};
        info.sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags   = flags;
        info.maxSets = kSets;
        // The acceleration-structure row is last: a device without the extension leaves just that row out.
        info.poolSizeCount      = static_cast<uint32_t>( sizes.size() ) - ( m_AccelerationStructures ? 0u : 1u );
        info.pPoolSizes         = sizes.data();
        VkDescriptorPool pool   = VK_NULL_HANDLE;
        const VkResult   result = vkCreateDescriptorPool( m_Device, &info, nullptr, &pool );
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<VkDescriptorPool>( "vkCreateDescriptorPool failed ({})",
                                                                 static_cast<int>( result ) );
        return Common::MakeSuccess( pool );
    }

    void VulkanRdgPassDescriptors::BeginFrameSlot( uint32_t slot )
    {
        m_Slot = slot % static_cast<uint32_t>( m_Slots.size() );
        for ( SlotPools& arena : m_Slots[m_Slot] )
        {
            for ( VkDescriptorPool pool : arena.Pools )
                vkResetDescriptorPool( m_Device, pool, 0 );
            arena.Current = 0;
        }
        EvictCached();
    }

    namespace
    {
        // A cached set's key hash (splitmix64 per word, folded): the shard and the bucket of the key.
        uint64_t HashCachedSetKey( std::span<const uint64_t> key )
        {
            uint64_t hash = 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>( key.size() );
            for ( const uint64_t word : key )
            {
                uint64_t mixed = word + 0x9E3779B97F4A7C15ULL;
                mixed          = ( mixed ^ ( mixed >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
                mixed          = ( mixed ^ ( mixed >> 27 ) ) * 0x94D049BB133111EBULL;
                mixed          = mixed ^ ( mixed >> 31 );
                hash           = ( hash ^ mixed ) * 0x100000001B3ULL;
            }
            return hash ^ ( hash >> 29 );
        }

        bool SameCachedSetKey( std::span<const uint64_t> stored, std::span<const uint64_t> key )
        {
            return std::ranges::equal( stored, key );
        }
    } // namespace

    Common::ResultStr<VulkanRdgPassDescriptors::CachedSet>
    VulkanRdgPassDescriptors::AcquireCached( VkDescriptorSetLayout layout, std::span<const uint64_t> key )
    {
        const uint64_t  hash  = HashCachedSetKey( key );
        CachedSetShard& shard = m_CachedShards[hash % kCachedSetShards];
        {
            const std::scoped_lock lock( shard.Lock );
            const auto             bucket = shard.Buckets.find( hash );
            if ( bucket != shard.Buckets.end() )
            {
                for ( CachedSetEntry& entry : bucket->second )
                {
                    if ( !SameCachedSetKey( entry.Key, key ) )
                        continue;
                    entry.LastUsed = m_CacheFrame.load( std::memory_order_relaxed );
                    return Common::MakeSuccess( CachedSet{ .Set = entry.Set, .Pool = entry.Pool, .Hit = true } );
                }
            }
        }

        // A miss: a fresh set from the cache's own pools (externally synchronized: under m_CachedPoolLock). A
        // full or fragmented pool hands over to the next one; when every pool refuses, one more is made.
        const std::scoped_lock lock( m_CachedPoolLock );
        const auto             allocate = [this, layout]( VkDescriptorPool pool, VkDescriptorSet& set )
        {
            VkDescriptorSetAllocateInfo info{};
            info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            info.descriptorPool     = pool;
            info.descriptorSetCount = 1;
            info.pSetLayouts        = &layout;
            return vkAllocateDescriptorSets( m_Device, &info, &set );
        };
        for ( size_t tried = 0; tried < m_CachedPools.size(); ++tried )
        {
            const size_t    index  = ( m_CachedPoolCurrent + tried ) % m_CachedPools.size();
            VkDescriptorSet set    = VK_NULL_HANDLE;
            const VkResult  result = allocate( m_CachedPools[index], set );
            if ( result == VK_SUCCESS )
            {
                m_CachedPoolCurrent = index;
                return Common::MakeSuccess( CachedSet{ .Set = set, .Pool = m_CachedPools[index], .Hit = false } );
            }
            if ( result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL )
                return Common::MakeFormattedError<CachedSet>( "vkAllocateDescriptorSets (cached) failed ({})",
                                                              static_cast<int>( result ) );
        }
        const Common::ResultStr<VkDescriptorPool> pool =
             CreatePool( VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT );
        if ( !pool )
            return Common::MakeFormattedError<CachedSet>( "{}", pool.GetError() );
        m_CachedPools.push_back( pool.GetValue() );
        m_CachedPoolCurrent    = m_CachedPools.size() - 1;
        VkDescriptorSet set    = VK_NULL_HANDLE;
        const VkResult  result = allocate( pool.GetValue(), set );
        // A layout that does not fit a fresh pool never will.
        if ( result != VK_SUCCESS )
            return Common::MakeFormattedError<CachedSet>( "vkAllocateDescriptorSets (cached) failed ({})",
                                                          static_cast<int>( result ) );
        return Common::MakeSuccess( CachedSet{ .Set = set, .Pool = pool.GetValue(), .Hit = false } );
    }

    void VulkanRdgPassDescriptors::PublishCached( std::span<const uint64_t> key, const CachedSet& fresh )
    {
        const uint64_t  hash  = HashCachedSetKey( key );
        CachedSetShard& shard = m_CachedShards[hash % kCachedSetShards];
        {
            const std::scoped_lock       lock( shard.Lock );
            std::vector<CachedSetEntry>& bucket = shard.Buckets[hash];
            const bool                   taken  = std::ranges::any_of(
                 bucket, [key]( const CachedSetEntry& entry ) { return SameCachedSetKey( entry.Key, key ); } );
            if ( !taken )
            {
                CachedSetEntry entry;
                entry.Key.assign( key.begin(), key.end() );
                entry.Set      = fresh.Set;
                entry.Pool     = fresh.Pool;
                entry.LastUsed = m_CacheFrame.load( std::memory_order_relaxed );
                bucket.push_back( std::move( entry ) );
                return;
            }
        }
        // An equal set another thread published first stays; this one is bound by this exec, so it is freed only
        // after the frames in flight.
        DiscardCached( fresh );
    }

    void VulkanRdgPassDescriptors::DiscardCached( const CachedSet& fresh )
    {
        const std::scoped_lock lock( m_CachedPoolLock );
        m_RetiredCached.push_back( RetiredCachedSet{ .Set   = fresh.Set,
                                                     .Pool  = fresh.Pool,
                                                     .Frame = m_CacheFrame.load( std::memory_order_relaxed ) } );
    }

    void VulkanRdgPassDescriptors::EvictCached()
    {
        // BeginFrameSlot of frame F runs after the fence of frame F - slots, the last frame that shared its slot:
        // a set last bindable in a frame before that one is bound by no command buffer that can still execute.
        const uint64_t frame    = m_CacheFrame.fetch_add( 1, std::memory_order_relaxed ) + 1;
        const uint64_t inFlight = static_cast<uint64_t>( m_Slots.size() );
        const auto     expired  = [frame, inFlight]( uint64_t used ) { return used + inFlight < frame; };
        const std::scoped_lock poolLock( m_CachedPoolLock );
        const auto             release = [this]( VkDescriptorPool pool, VkDescriptorSet set )
        { vkFreeDescriptorSets( m_Device, pool, 1, &set ); };
        for ( CachedSetShard& shard : m_CachedShards )
        {
            const std::scoped_lock lock( shard.Lock );
            for ( auto bucket = shard.Buckets.begin(); bucket != shard.Buckets.end(); )
            {
                std::erase_if( bucket->second,
                               [&expired, &release]( const CachedSetEntry& entry )
                               {
                                   if ( !expired( entry.LastUsed ) )
                                       return false;
                                   release( entry.Pool, entry.Set );
                                   return true;
                               } );
                if ( bucket->second.empty() )
                    bucket = shard.Buckets.erase( bucket );
                else
                    ++bucket;
            }
        }
        std::erase_if( m_RetiredCached,
                       [&expired, &release]( const RetiredCachedSet& retired )
                       {
                           if ( !expired( retired.Frame ) )
                               return false;
                           release( retired.Pool, retired.Set );
                           return true;
                       } );
    }

    void VulkanRdgPassDescriptors::ReserveArenas( uint32_t arenas )
    {
        std::vector<SlotPools>& slot = m_Slots[m_Slot];
        if ( slot.size() < arenas )
            slot.resize( arenas );
    }

    Common::ResultStr<VkDescriptorSet> VulkanRdgPassDescriptors::Allocate( VkDescriptorSetLayout layout )
    {
        std::vector<SlotPools>& slot = m_Slots[m_Slot];
        if ( t_Arena >= slot.size() )
            return Common::MakeFormattedError<VkDescriptorSet>( "descriptor arena {} was not reserved (ReserveArenas)",
                                                                t_Arena );
        SlotPools& pools = slot[t_Arena];
        for ( ;; )
        {
            bool fresh = false;
            if ( pools.Current == pools.Pools.size() )
            {
                if ( auto added = AddPool( pools ); !added )
                    return Common::MakeFormattedError<VkDescriptorSet>( "{}", added.GetError() );
                fresh = true;
            }
            VkDescriptorSetAllocateInfo info{};
            info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            info.descriptorPool     = pools.Pools[pools.Current];
            info.descriptorSetCount = 1;
            info.pSetLayouts        = &layout;
            VkDescriptorSet set     = VK_NULL_HANDLE;
            const VkResult  result  = vkAllocateDescriptorSets( m_Device, &info, &set );
            if ( result == VK_SUCCESS )
                return Common::MakeSuccess( set );
            // Only a full pool moves on to the next; a layout that does not fit a fresh pool never will.
            const bool full = result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL;
            if ( !full || fresh )
                return Common::MakeFormattedError<VkDescriptorSet>( "vkAllocateDescriptorSets failed ({})",
                                                                    static_cast<int>( result ) );
            ++pools.Current;
        }
    }

    namespace
    {
        // The descriptors queued on this thread (VulkanRdgPassDescriptors::WriteBatch). The infos live in their
        // own arrays and are pointed at when the batch flushes, so the arrays may grow while writes are queued.
        struct PendingDescriptorWrites
        {
            std::vector<VkWriteDescriptorSet>                         Writes;
            std::vector<VkDescriptorImageInfo>                        Images;
            std::vector<VkDescriptorBufferInfo>                       Buffers;
            std::vector<VkAccelerationStructureKHR>                   Structures;
            std::vector<VkWriteDescriptorSetAccelerationStructureKHR> StructureWrites;
            uint32_t                                                  Depth = 0;
        };
        thread_local PendingDescriptorWrites t_PendingWrites;
    } // namespace

    VulkanRdgPassDescriptors::WriteBatch::WriteBatch( VulkanRdgPassDescriptors& descriptors )
         : m_Descriptors( descriptors )
    {
        ++t_PendingWrites.Depth;
    }

    VulkanRdgPassDescriptors::WriteBatch::~WriteBatch()
    {
        --t_PendingWrites.Depth;
        if ( t_PendingWrites.Depth == 0 )
            m_Descriptors.Flush();
    }

    void VulkanRdgPassDescriptors::Queue( const VkWriteDescriptorSet& write, const VkDescriptorImageInfo* image,
                                          const VkDescriptorBufferInfo* buffer, VkAccelerationStructureKHR structure )
    {
        PendingDescriptorWrites& pending = t_PendingWrites;
        pending.Writes.push_back( write );
        if ( image != nullptr )
            pending.Images.push_back( *image );
        if ( buffer != nullptr )
            pending.Buffers.push_back( *buffer );
        if ( write.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR )
            pending.Structures.push_back( structure );
        if ( pending.Depth == 0 )
            Flush();
    }

    void VulkanRdgPassDescriptors::Flush()
    {
        PendingDescriptorWrites& pending = t_PendingWrites;
        if ( pending.Writes.empty() )
            return;
        // Each write takes the next info of its kind, in the order the writes were queued.
        pending.StructureWrites.resize( pending.Structures.size() );
        size_t imageAt     = 0;
        size_t bufferAt    = 0;
        size_t structureAt = 0;
        for ( VkWriteDescriptorSet& write : pending.Writes )
        {
            if ( write.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR )
            {
                VkWriteDescriptorSetAccelerationStructureKHR& chained = pending.StructureWrites[structureAt];
                chained                            = VkWriteDescriptorSetAccelerationStructureKHR{};
                chained.sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                chained.accelerationStructureCount = 1;
                chained.pAccelerationStructures    = &pending.Structures[structureAt];
                write.pNext                        = &chained;
                ++structureAt;
            }
            else if ( write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                      write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER )
            {
                write.pBufferInfo = &pending.Buffers[bufferAt];
                ++bufferAt;
            }
            else
            {
                write.pImageInfo = &pending.Images[imageAt];
                ++imageAt;
            }
        }
        vkUpdateDescriptorSets( m_Device, static_cast<uint32_t>( pending.Writes.size() ), pending.Writes.data(), 0,
                                nullptr );
        pending.Writes.clear();
        pending.Images.clear();
        pending.Buffers.clear();
        pending.Structures.clear();
        pending.StructureWrites.clear();
    }

    Common::BoolResultStr VulkanRdgPassDescriptors::WriteTexture( VkDescriptorSet set, uint32_t binding,
                                                                  VkDescriptorType           type,
                                                                  const RDG::TextureBinding& texture,
                                                                  RDG::SubresourceRange      range,
                                                                  VkImageLayout layout, uint32_t element )
    {
        if ( type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE )
            return Common::MakeFormattedError(
                 "texture '{}': descriptor type {} is not a separate or storage image "
                 "type (a pass binds no combined sampler)",
                 texture.Name, static_cast<int>( type ) );
        const Common::ResultStr<VulkanRdgTexture*> physical = VulkanRdgBackend::TextureOf( texture );
        if ( !physical )
            return Common::MakeError( physical.GetError() );
        const Common::ResultStr<VkImageView> view = physical.GetValue()->GetView( range, false );
        if ( !view )
            return Common::MakeFormattedError( "texture '{}': {}", texture.Name, view.GetError() );
        const VkDescriptorImageInfo image{ VK_NULL_HANDLE, view.GetValue(), layout };
        VkWriteDescriptorSet        write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = binding;
        write.dstArrayElement = element;
        write.descriptorCount = 1;
        write.descriptorType  = type;
        Queue( write, &image, nullptr, VK_NULL_HANDLE );
        return Common::MakeSuccess( true );
    }

    void VulkanRdgPassDescriptors::WriteSampler( VkDescriptorSet set, uint32_t binding, VkSampler sampler )
    {
        const VkDescriptorImageInfo image{ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet        write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = binding;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        Queue( write, &image, nullptr, VK_NULL_HANDLE );
    }

    Common::BoolResultStr VulkanRdgPassDescriptors::WriteBuffer( VkDescriptorSet set, uint32_t binding,
                                                                 VkDescriptorType          type,
                                                                 const RDG::BufferBinding& buffer )
    {
        if ( type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER )
            return Common::MakeFormattedError( "buffer '{}': descriptor type {} is not a buffer type", buffer.Name,
                                               static_cast<int>( type ) );
        const Common::ResultStr<VulkanRdgBuffer*> physical = VulkanRdgBackend::BufferOf( buffer );
        if ( !physical )
            return Common::MakeError( physical.GetError() );
        const VkDescriptorBufferInfo info{ physical.GetValue()->GetBuffer(), physical.GetValue()->GetOffset(),
                                           physical.GetValue()->GetSize() };
        VkWriteDescriptorSet         write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = binding;
        write.descriptorCount = 1;
        write.descriptorType  = type;
        Queue( write, nullptr, &info, VK_NULL_HANDLE );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRdgPassDescriptors::WriteAccelerationStructure( VkDescriptorSet           set,
                                                                                uint32_t                  binding,
                                                                                VkDescriptorType          type,
                                                                                const RDG::BufferBinding& tlas )
    {
        if ( type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR )
            return Common::MakeFormattedError( "'{}': descriptor type {} is not an acceleration-structure type",
                                               tlas.Name, static_cast<int>( type ) );
        if ( !m_AccelerationStructures )
            return Common::MakeFormattedError( "'{}': the device has no acceleration structures", tlas.Name );
        const Common::ResultStr<VulkanRdgBuffer*> physical = VulkanRdgBackend::BufferOf( tlas );
        if ( !physical )
            return Common::MakeError( physical.GetError() );
        const VkAccelerationStructureKHR handle = physical.GetValue()->GetAccelerationStructure();
        if ( handle == VK_NULL_HANDLE )
            return Common::MakeFormattedError( "'{}' stores no acceleration structure", tlas.Name );
        // Its VkWriteDescriptorSetAccelerationStructureKHR is chained when the batch flushes (Flush).
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = binding;
        write.descriptorCount = 1;
        write.descriptorType  = type;
        Queue( write, nullptr, nullptr, handle );
        return Common::MakeSuccess( true );
    }

    // ── Backend ────────────────────────────────────────────────────────────────────────────────────────

    VulkanRdgBackend::VulkanRdgBackend( const VulkanRdgDevice& device, VulkanRdgPool& pool )
         : m_Device( device ), m_Pool( pool ), m_Memory( device ),
           m_FallbackLog( []( std::string_view line ) { LOG_WARN( "{}", line ); } ),
           m_FaultReporter(
                []( RDG::PassFaultReporter::Severity severity, std::string_view line )
                {
                    if ( severity == RDG::PassFaultReporter::Severity::Error )
                    {
                        LOG_ERROR( "[RDG] {}", line );
                    }
                    else
                    {
                        LOG_INFO( "[RDG] {}", line );
                    }
                } ),
           m_CompileCache(
                []( bool warning, std::string_view line )
                {
                    if ( warning )
                    {
                        LOG_WARN( "[RDG] {}", line );
                    }
                    else
                    {
                        LOG_INFO( "[RDG] {}", line );
                    }
                } )
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
        // GetKind() is Vulkan (checked above), and VulkanRdgBackend is the one backend that answers so.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        VkCommandBuffer commandBuffer = static_cast<VulkanRdgBackend&>( backend ).RecordingBuffer();
        if ( commandBuffer == VK_NULL_HANDLE )
            return Common::MakeFormattedError<VkCommandBuffer>( "pass '{}': the backend has no command buffer",
                                                                context.GetPassName() );
        return Common::MakeSuccess( commandBuffer );
    }

    Common::ResultStr<VulkanRdgPassSetMemo*> VulkanRdgBackend::SetMemoOf( const RDG::PassContext& context )
    {
        RDG::IBackend& backend = context.GetBackend();
        if ( backend.GetKind() != RDG::BackendKind::Vulkan )
            return Common::MakeFormattedError<VulkanRdgPassSetMemo*>(
                 "pass '{}' is not recorded by the Vulkan backend", context.GetPassName() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): GetKind() == Vulkan names this exact
        // type
        return Common::MakeSuccess( &static_cast<VulkanRdgBackend&>( backend ).PassSetsState() );
    }

    bool VulkanRdgPassSetMemo::Matches( const Entry& entry, const Key& key )
    {
        return entry.Block.Pass == key.Block.Pass && entry.Block.Block == key.Block.Block &&
               entry.Shader == key.Shader && entry.Layout == key.Layout && entry.Material == key.Material &&
               entry.MaterialSet == key.MaterialSet && entry.MaterialPush == key.MaterialPush &&
               entry.BlockPushBytes == key.BlockPushBytes &&
               std::equal( entry.MaterialBindings.begin(), entry.MaterialBindings.end(), key.MaterialBindings.begin(),
                           key.MaterialBindings.end() );
    }

    const std::vector<VkDescriptorSet>* VulkanRdgPassSetMemo::Find( const Key& key ) const
    {
        if ( m_LastHit < m_Entries.size() && Matches( m_Entries[m_LastHit], key ) )
            return &m_Entries[m_LastHit].Sets;
        for ( size_t index = 0; index < m_Entries.size(); ++index )
        {
            if ( Matches( m_Entries[index], key ) )
            {
                m_LastHit = index;
                return &m_Entries[index].Sets;
            }
        }
        return nullptr;
    }

    void VulkanRdgPassSetMemo::Keep( const Key& key, std::vector<VkDescriptorSet> sets )
    {
        m_LastHit = m_Entries.size();
        Entry entry{ .Block          = key.Block,
                     .Shader         = key.Shader,
                     .Layout         = key.Layout,
                     .Material       = key.Material,
                     .MaterialSet    = key.MaterialSet,
                     .MaterialPush   = key.MaterialPush,
                     .BlockPushBytes = key.BlockPushBytes,
                     .Sets           = std::move( sets ) };
        entry.MaterialBindings.assign( key.MaterialBindings.begin(), key.MaterialBindings.end() );
        m_Entries.push_back( std::move( entry ) );
    }

    void VulkanRdgPassSetMemo::Clear()
    {
        m_Entries.clear();
        m_LastHit = 0;
    }

    Common::BoolResultStr VulkanRdgBackend::BeginFrame( uint32_t slot, const VulkanRdgQueueSet& queues,
                                                        VulkanRdgTransientAllocator& transients,
                                                        VulkanRdgPassDescriptors&    descriptors )
    {
        if ( queues.GraphicsQueue == VK_NULL_HANDLE )
            return Common::MakeError( "BeginFrame: the queue set has no graphics queue" );
        m_CompileCache.CountFrame();
        m_FrameSlot   = slot;
        m_Queues      = &queues;
        m_Transients  = &transients;
        m_Descriptors = &descriptors;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<VulkanRdgPassDescriptors*> VulkanRdgBackend::DescriptorsOf( const RDG::PassContext& context )
    {
        RDG::IBackend& backend = context.GetBackend();
        if ( backend.GetKind() != RDG::BackendKind::Vulkan )
            return Common::MakeFormattedError<VulkanRdgPassDescriptors*>(
                 "pass '{}' is not recorded by the Vulkan backend", context.GetPassName() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): GetKind() == Vulkan names this exact
        // type
        VulkanRdgPassDescriptors* descriptors = static_cast<VulkanRdgBackend&>( backend ).m_Descriptors;
        if ( descriptors == nullptr )
            return Common::MakeFormattedError<VulkanRdgPassDescriptors*>(
                 "pass '{}': the backend has no descriptors (BeginFrame)", context.GetPassName() );
        return Common::MakeSuccess( descriptors );
    }

    RDG::ITransientAllocator& VulkanRdgBackend::GetTransientAllocator()
    {
        if ( m_Transients == nullptr )
        {
            std::fputs( "VulkanRdgBackend::GetTransientAllocator before BeginFrame\n", stderr );
            std::abort();
        }
        return *m_Transients;
    }

    Common::ResultStr<VulkanRdgTexture*> VulkanRdgBackend::TextureOf( const RDG::TextureBinding& binding )
    {
        if ( binding.Physical == nullptr || binding.Physical->GetBackendKind() != RDG::BackendKind::Vulkan )
            return Common::MakeFormattedError<VulkanRdgTexture*>( "texture '{}' has no Vulkan image",
                                                                  binding.Name );
        // GetBackendKind() is Vulkan (checked above), and only VulkanRdgTexture answers so.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return Common::MakeSuccess( static_cast<VulkanRdgTexture*>( binding.Physical ) );
    }

    Common::ResultStr<VulkanRdgBuffer*> VulkanRdgBackend::BufferOf( const RDG::BufferBinding& binding )
    {
        if ( binding.Physical == nullptr || binding.Physical->GetBackendKind() != RDG::BackendKind::Vulkan )
            return Common::MakeFormattedError<VulkanRdgBuffer*>( "buffer '{}' has no Vulkan buffer",
                                                                 binding.Name );
        // GetBackendKind() is Vulkan (checked above), and only VulkanRdgBuffer answers so.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        return Common::MakeSuccess( static_cast<VulkanRdgBuffer*>( binding.Physical ) );
    }

    Common::BoolResultStr VulkanRdgBackend::BeginGraph( const RDG::GraphView& graph )
    {
        if ( m_Queues == nullptr )
            return Common::MakeFormattedError( "graph '{}': the Vulkan backend has no queues (BeginFrame)",
                                               graph.Name );
        // BEGINGRAPH-STALL-1: the steps of BeginGraph, one CPU row each under the builder's "RDG: BeginGraph", so
        // a stall inside it names its step (UE: RDG's per-phase CPU events).
        DESERT_PROFILE_PHASES( phases );
        DESERT_PROFILE_PHASE( phases, "RDG BeginGraph: Release + framebuffers" );
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
        if ( m_Transients == nullptr )
            return Common::MakeFormattedError(
                 "graph '{}': the Vulkan backend has no transient allocator (BeginFrame)", graph.Name );
        if ( graph.Result == nullptr )
            return Common::MakeFormattedError( "graph '{}': no compile result to place transients from",
                                               graph.Name );
        // THE CROSSING RULE'S PRECONDITION (RecordBarriers): a graph with an AsyncCompute segment crosses queue
        // families, and its release / acquire pairs name no family - valid only when every resource is CONCURRENT
        // across both pipes' families (VulkanResourceSharing.hpp). A device set up otherwise is refused by name,
        // never recorded with ownership the GPU does not have.
        const bool crossesQueues = std::any_of( graph.Result->Segments.begin(), graph.Result->Segments.end(),
                                                []( const RDG::PipeSegment& segment )
                                                { return segment.OnPipe == RDG::Pipe::AsyncCompute; } );
        if ( crossesQueues && ( !m_Device.Sharing.Contains( m_Queues->FamilyOf( RDG::Pipe::Graphics ) ) ||
                                !m_Device.Sharing.Contains( m_Queues->FamilyOf( RDG::Pipe::AsyncCompute ) ) ||
                                !m_Device.Sharing.IsConcurrent() ) )
            return Common::MakeFormattedError( "graph '{}': it has an AsyncCompute segment, but the device's resources "
                                               "are not CONCURRENT across the graphics and compute families "
                                               "(VulkanRdgDevice::Sharing)",
                                               graph.Name );
        // THE ASYNC SEGMENTS, NAMED (FPS-CPU-6): which passes this graph runs on the compute queue, logged when the
        // set changes, so a log shows what overlapped the graphics queue (and an empty async frame shows nothing).
        m_Result = graph.Result;
        if ( crossesQueues )
        {
            DESERT_PROFILE_PHASE( phases, "RDG BeginGraph: Async segments" );
            // The set is its STRUCTURE (positions), never the pass names: a name carries this frame's counts
            // ("SurfaceCache: Direct Lighting (196 pages)"), so a line keyed by names changed nearly every frame
            // and BeginGraph built and wrote a log line per frame on the render thread (BEGINGRAPH-STALL-1).
            std::vector<uint32_t> structure;
            for ( const RDG::PipeSegment& segment : graph.Result->Segments )
            {
                if ( segment.OnPipe != RDG::Pipe::AsyncCompute )
                    continue;
                structure.push_back( segment.FirstPosition );
                structure.push_back( segment.LastPosition );
                for ( const RDG::CrossPipeSync& sync : graph.Result->Syncs )
                {
                    if ( !sync.IsFork && sync.SignalPosition == segment.LastPosition )
                        structure.push_back( sync.WaitPosition );
                }
                for ( const uint32_t s : segment.WaitSyncs )
                {
                    if ( graph.Result->Syncs[s].IsFork )
                        structure.push_back( graph.Result->Syncs[s].SignalPosition );
                }
            }
            auto logged = m_AsyncSegmentsLogged.find( graph.Name );
            if ( logged == m_AsyncSegmentsLogged.end() )
                logged = m_AsyncSegmentsLogged.emplace( std::string( graph.Name ), std::vector<uint32_t>{} ).first;
            if ( logged->second != structure )
            {
                std::string line;
                for ( const RDG::PipeSegment& segment : graph.Result->Segments )
                {
                    if ( segment.OnPipe != RDG::Pipe::AsyncCompute )
                        continue;
                    line += " [" + graph.Result->Passes[segment.FirstPosition].Name + " .. " +
                            graph.Result->Passes[segment.LastPosition].Name + ": " +
                            std::to_string( segment.LastPosition - segment.FirstPosition + 1 ) + " passes";
                    // The graphics pass it waits for (its fork, RDGCompile CutRunsAtConsumers / CreateAsyncSyncs) and
                    // why; a segment without one follows the one before it on the compute queue.
                    bool forked = false;
                    for ( const uint32_t s : segment.WaitSyncs )
                    {
                        const RDG::CrossPipeSync& forkSync = graph.Result->Syncs[s];
                        if ( !forkSync.IsFork )
                            continue;
                        forked = true;
                        if ( forkSync.SignalPosition == RDG::CrossPipeSync::kForkAtGraphStart )
                            line += ", forked at the graph start";
                        else
                            line += ", forked after '" + graph.Result->Passes[forkSync.SignalPosition].Name + "'";
                        if ( !forkSync.Cause.empty() )
                            line += " by " + forkSync.Cause;
                    }
                    if ( !forked )
                        line += ", no fork of its own (after the segment before it on the queue)";
                    // The graphics pass that waits for this segment (its join, RDGCompile CutRunsAtConsumers).
                    for ( const RDG::CrossPipeSync& sync : graph.Result->Syncs )
                    {
                        if ( sync.IsFork || sync.SignalPosition != segment.LastPosition )
                            continue;
                        if ( sync.WaitPosition == RDG::CrossPipeSync::kJoinAtGraphEnd )
                            line += ", joined at the graph end";
                        else
                        {
                            line += ", joined before '" + graph.Result->Passes[sync.WaitPosition].Name + "'";
                            // What made it wait (RDGCompile CutRunsAtConsumers): the resource and the async pass.
                            if ( !sync.Cause.empty() )
                                line += " by " + sync.Cause;
                        }
                    }
                    line += "]";
                }
                LOG_INFO( "[RDG] graph '{}' async compute segments:{}", graph.Name, line );
                logged->second = std::move( structure );
            }
        }
        DESERT_PROFILE_PHASE( phases, "RDG BeginGraph: Segments" );
        // B(3): what records before the first segment (a graph with no segment: the tail for its final barriers).
        const Common::ResultStr<VkCommandBuffer> first = m_Segments.BeginGraph( *graph.Result, *m_Queues );
        if ( !first )
            return Common::MakeFormattedError( "graph '{}': {}", graph.Name, first.GetError() );
        // A graph refused below is not abandoned by Builder::Execute, and its compile result dies with that call:
        // the recorder drops it (and what this graph queued) on every refusal.
        struct AbandonUnlessBegun
        {
            VulkanRdgSegmentRecorder& Segments;
            bool                      Begun = false;
            ~AbandonUnlessBegun()
            {
                if ( !Begun )
                    Segments.AbandonGraph();
            }
        } segments{ m_Segments };
        SetRecording( first.GetValue() );
        m_Textures.resize( graph.Resources.size() );
        m_Buffers.resize( graph.Resources.size() );

        // Every used, non-extracted transient is PLACED at its planned heap offset, in plan order. Two that share
        // bytes are two resources over the same memory; the AliasAcquire barrier alone orders them.
        DESERT_PROFILE_PHASE( phases, "RDG BeginGraph: Transient heaps" );
        const RDG::AliasingPlan& plan = graph.Result->Aliasing;
        // A graph only on the graphics queue holds the slot's shared heaps in turn with the slot's other graphs
        // (BISTRO-MEM-2 p5); one with an async-compute segment keeps its own (a compute submission is ordered
        // after the earlier graphs only by the semaphores it waits on).
        const bool graphicsOnly = !crossesQueues;
        const Common::ResultStr<bool> reserved =
             m_Transients->ReserveGraphHeaps( graph.Name, plan.Heaps, graphicsOnly );
        if ( !reserved )
            return Common::MakeFormattedError( "{}", reserved.GetError() );
        // An earlier graph used the bytes (this frame or one in flight; same queue, earlier in submission order):
        // its last accesses complete before this graph's first one, recorded into the first command buffer the
        // graph opens (BeginGraph returns one only for a graph without segments).
        m_SharedHeapBarrierPending = reserved.GetValue();
        m_GraphName                = graph.Name; // before the barrier: its GPU row is named after the graph
        if ( first.GetValue() != VK_NULL_HANDLE )
            RecordSharedHeapBarrier( first.GetValue() );
        DESERT_PROFILE_PHASE( phases, "RDG BeginGraph: Place transients" );
        for ( const RDG::Allocation& allocation : plan.Allocations )
        {
            if ( allocation.Resource >= graph.Resources.size() )
                return Common::MakeFormattedError( "graph '{}': allocation of resource {} of {}", graph.Name,
                                                   allocation.Resource, graph.Resources.size() );
            const RDG::ResourceView& view = graph.Resources[allocation.Resource];
            if ( !view.Used )
                continue;
            if ( view.Extracted || view.ExternalTex != nullptr || view.ExternalBuf != nullptr )
                return Common::MakeFormattedError(
                     "graph '{}': '{}' is external or extracted but has an allocation", graph.Name, view.Name );
            if ( view.Kind == RDG::ResourceKind::Texture )
            {
                auto placed = m_Transients->PlaceTexture( allocation, *view.Texture, view.AccessMask, view.Name );
                if ( !placed )
                    return Common::MakeError( placed.GetError() );
                m_Textures[view.Resource] = std::static_pointer_cast<VulkanRdgTexture>( placed.GetValue() );
            }
            else
            {
                auto placed = m_Transients->PlaceBuffer( allocation, *view.Buffer, view.AccessMask, view.Name );
                if ( !placed )
                    return Common::MakeError( placed.GetError() );
                m_Buffers[view.Resource] = std::static_pointer_cast<VulkanRdgBuffer>( placed.GetValue() );
            }
        }

        // Externals are bound; an extracted transient gets a dedicated resource from the pool (it outlives the
        // graph, so it may not live in a heap the next frame reuses).
        DESERT_PROFILE_PHASE( phases, "RDG BeginGraph: Externals + extracted" );
        for ( const RDG::ResourceView& view : graph.Resources )
        {
            if ( !view.Used )
                continue;
            if ( view.Kind == RDG::ResourceKind::Texture )
            {
                if ( view.ExternalTex != nullptr )
                {
                    const std::shared_ptr<RDG::IPhysicalTexture>& physical = view.ExternalTex->Physical;
                    if ( !physical || physical->GetBackendKind() != RDG::BackendKind::Vulkan )
                        return Common::MakeFormattedError( "external texture '{}' carries no Vulkan image",
                                                           view.Name );
                    m_Textures[view.Resource] = std::static_pointer_cast<VulkanRdgTexture>( physical );
                    continue;
                }
                if ( m_Textures[view.Resource] )
                    continue; // placed above
                if ( !view.Extracted )
                    return Common::MakeFormattedError( "graph '{}': transient '{}' has no allocation in the plan",
                                                       graph.Name, view.Name );
                auto acquired =
                     m_Pool.AcquireTexture( *view.Texture, view.AccessMask, view.Name, view.ExtractOwner );
                if ( !acquired )
                    return Common::MakeError( acquired.GetError() );
                m_Textures[view.Resource] = acquired.GetValue();
            }
            else
            {
                if ( view.ExternalBuf != nullptr )
                {
                    const std::shared_ptr<RDG::IPhysicalBuffer>& physical = view.ExternalBuf->Physical;
                    if ( !physical || physical->GetBackendKind() != RDG::BackendKind::Vulkan )
                        return Common::MakeFormattedError( "external buffer '{}' carries no Vulkan buffer",
                                                           view.Name );
                    m_Buffers[view.Resource] = std::static_pointer_cast<VulkanRdgBuffer>( physical );
                    continue;
                }
                if ( m_Buffers[view.Resource] )
                    continue; // placed above
                if ( !view.Extracted )
                    return Common::MakeFormattedError( "graph '{}': transient '{}' has no allocation in the plan",
                                                       graph.Name, view.Name );
                auto acquired =
                     m_Pool.AcquireBuffer( *view.Buffer, view.AccessMask, view.Name, view.ExtractOwner );
                if ( !acquired )
                    return Common::MakeError( acquired.GetError() );
                m_Buffers[view.Resource] = acquired.GetValue();
            }
        }
        segments.Begun = true;
        return Common::MakeSuccess( true );
    }

    // The graph's own commands outside every pass - the heap-reuse barrier, the fork releases at the graph start,
    // the final barriers - each get a GPU row of their own ("RDG Barriers: <graph> | <what>"), so the graph
    // records nothing the profiler cannot name (UE: RDG's prologue / epilogue barrier batches sit under the
    // graph's event scope, not outside "stat gpu").
    int32_t VulkanRdgBackend::BeginGraphCommandsScope( std::string_view what )
    {
        ResumeSegment();
        if ( m_Device.Profiler == nullptr )
            return -1;
        const std::string name = std::format( "RDG Barriers: {} | {}", m_GraphName, what );
        return m_Device.Profiler->BeginScope( name.c_str() );
    }

    void VulkanRdgBackend::EndGraphCommandsScope( int32_t handle )
    {
        if ( m_Device.Profiler != nullptr )
            m_Device.Profiler->EndScope( handle );
    }

    // THE one place a pass is labelled and timed: every graph pass gets both, and no pass records its own.
    void VulkanRdgBackend::BeginPass( const RDG::CompiledPass& pass )
    {
        ResumeSegment();
        Chunk* const          chunk         = BoundChunk();
        const VkCommandBuffer commandBuffer = RecordingBuffer();
        PassSetsState().Clear();
        if ( m_PassBoundaryListener )
            m_PassBoundaryListener();
        if ( m_Device.CmdBeginLabel != nullptr && commandBuffer != VK_NULL_HANDLE )
        {
            VkDebugUtilsLabelEXT label{};
            label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label.pLabelName = pass.Name.c_str();
            m_Device.CmdBeginLabel( commandBuffer, &label );
            if ( chunk != nullptr )
                chunk->Labels.push_back( pass.Name.c_str() );
        }
#if DESERT_DEV_INSTRUMENTS
        // The plan's interned row (CompiledPass::PassRow): no name lookup and no allocation per pass.
        ( chunk != nullptr ? chunk->CpuScope : m_CpuScope ).emplace( pass.PassRow );
#endif
        ProfilerScopesState().push_back(
             m_Device.Profiler != nullptr ? m_Device.Profiler->BeginScope( pass.Name.c_str() ) : -1 );
    }

    void VulkanRdgBackend::EndPass( const RDG::CompiledPass& )
    {
        Chunk* const          chunk         = BoundChunk();
        const VkCommandBuffer commandBuffer = RecordingBuffer();
        PassSetsState().Clear();
        if ( m_PassBoundaryListener )
            m_PassBoundaryListener();
        std::vector<int32_t>& scopes = ProfilerScopesState();
        if ( !scopes.empty() )
        {
            if ( m_Device.Profiler != nullptr )
                m_Device.Profiler->EndScope( scopes.back() );
            scopes.pop_back();
#if DESERT_DEV_INSTRUMENTS
            ( chunk != nullptr ? chunk->CpuScope : m_CpuScope ).reset();
#endif
        }
        if ( m_Device.CmdEndLabel != nullptr && commandBuffer != VK_NULL_HANDLE )
        {
            if ( chunk == nullptr )
                m_Device.CmdEndLabel( commandBuffer );
            else if ( !chunk->Labels.empty() )
            {
                m_Device.CmdEndLabel( commandBuffer );
                chunk->Labels.pop_back();
            }
        }
    }

    void VulkanRdgBackend::RecordBarriers( std::span<const RDG::Barrier> barriers )
    {
        ResumeSegment();
        // At most TWO vkCmdPipelineBarrier: the halves of the graph's cross-pipe pairs first, then everything
        // else. A pair half may not share a call with a HOST-stage barrier (VUID-vkCmdPipelineBarrier-srcStageMask-
        // 09633/09634), and a resource that returns to Graphics before a plain barrier hands it on (an async
        // result read back by the host) needs the two in that order. Within a call the stage masks are the
        // union over the batch, each resource keeps its own access masks and layouts.
        //
        // THE CROSSING RULE (graphics -> async compute -> graphics). RDGCompile emits a release / acquire pair at
        // every crossing of a resource it sees (TrackUse crossPipes, CollectFinalBarriers), the semaphore between
        // the queues orders them. Every resource is CONCURRENT across the device's families whenever there is an
        // async pipe (VulkanResourceSharing.hpp; refused otherwise in Execute), so the pair names no families:
        //   release, on the source queue: the whole transition - the source's writes made available, the layout
        //            changed - ending at BOTTOM_OF_PIPE; the semaphore signal after it covers it.
        //   acquire, on the destination queue: chained to the semaphore wait through ALL_COMMANDS, it makes the
        //            contents visible to the destination's stages in the layout the release left (old == new).
        // A family-indexed transfer would be invalid on a CONCURRENT resource (VUID-VkImageMemoryBarrier-image-
        // 04071), and an EXCLUSIVE one could not cover a read the graph never saw (a bindless upload).
        for ( const bool ownershipCall : { true, false } )
        {
            // CPU-FRAME-ARENA (MEM-7): the batch lives in the recording thread's scratch, not on the heap.
            Common::Memory::FrameArena&                        scratch = ScratchArena();
            const Common::Memory::FrameArena::ScopedMark       scratchMark( scratch );
            const Common::Memory::ArenaAllocator<std::byte>    scratchAlloc( scratch );
            VkPipelineStageFlags                               srcStages = 0;
            VkPipelineStageFlags                               dstStages = 0;
            Common::Memory::ArenaVector<VkImageMemoryBarrier>  images( scratchAlloc );
            Common::Memory::ArenaVector<VkBufferMemoryBarrier> buffers( scratchAlloc );
            images.reserve( barriers.size() );
            buffers.reserve( barriers.size() );
            for ( const RDG::Barrier& barrier : barriers )
            {
                const bool release = barrier.BarrierType == RDG::BarrierKind::OwnershipRelease;
                const bool acquire = barrier.BarrierType == RDG::BarrierKind::OwnershipAcquire;
                if ( ( release || acquire ) != ownershipCall )
                    continue;
                srcStages |= acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : RdgVulkanStages( barrier.Before.Stages );
                // A release's second scope is on the other queue: on this one it only ends the barrier.
                dstStages |=
                     release ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : RdgVulkanStages( barrier.After.Stages );
                // A write-free source has nothing to make available: its stages only order the work. An acquire
                // makes nothing available (its release did); a release makes nothing visible (its acquire does).
                const VkAccessFlags srcAccess =
                     acquire ? 0 : RdgVulkanAccess( barrier.Before.Memory & RDG::kWriteAccessMask );
                const VkAccessFlags dstAccess = release ? 0 : RdgVulkanAccess( barrier.After.Memory );
                if ( barrier.Kind == RDG::ResourceKind::Texture )
                {
                    const VulkanRdgTexture& texture = *m_Textures[barrier.Resource];
                    VkImageMemoryBarrier    image{};
                    image.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    image.srcAccessMask       = srcAccess;
                    image.dstAccessMask       = dstAccess;
                    // The acquire's layout is already the release's newLayout: it transitions nothing.
                    image.oldLayout           = acquire                   ? RdgVulkanLayout( barrier.After.Layout )
                                                : barrier.DiscardContents ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                          : RdgVulkanLayout( barrier.Before.Layout );
                    image.newLayout           = RdgVulkanLayout( barrier.After.Layout );
                    image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    image.image               = texture.GetImage();
                    image.subresourceRange = { texture.GetAspect(), barrier.Range.BaseMip, barrier.Range.MipCount,
                                               barrier.Range.BaseLayer, barrier.Range.LayerCount };
                    images.push_back( image );
                }
                else
                {
                    VkBufferMemoryBarrier buffer{};
                    buffer.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    buffer.srcAccessMask       = srcAccess;
                    buffer.dstAccessMask       = dstAccess;
                    buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    const VulkanRdgBuffer& physical = *m_Buffers[barrier.Resource];
                    buffer.buffer                   = physical.GetBuffer();
                    buffer.offset                   = physical.GetOffset();
                    buffer.size = physical.GetOffset() == 0 ? VK_WHOLE_SIZE : physical.GetSize();
                    buffers.push_back( buffer );
                }
            }
            if ( images.empty() && buffers.empty() )
                continue;
            // v1 has no NONE stage: "nothing before" is TOP_OF_PIPE, "nothing after" (Present) BOTTOM_OF_PIPE.
            if ( srcStages == 0 )
                srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            if ( dstStages == 0 )
                dstStages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            const VkCommandBuffer commandBuffer = RecordingBuffer();
            if ( commandBuffer == VK_NULL_HANDLE )
                return; // a chunk whose buffer could not be begun: ExecuteChunk reports it
            vkCmdPipelineBarrier( commandBuffer, srcStages, dstStages, 0, 0, nullptr,
                                  static_cast<uint32_t>( buffers.size() ), buffers.data(),
                                  static_cast<uint32_t>( images.size() ), images.data() );
        }
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

    void VulkanRdgBackend::PrepareRecording( std::span<const RDG::CompiledPass> passes )
    {
        // CPU-FRAME-ARENA (MEM-7): the entries are kept across graphs, so each one's clear values keep their
        // capacity; an entry with no Source is not prepared for this graph (BeginRenderPass resolves its pass).
        uint32_t count = 0;
        for ( const RDG::CompiledPass& pass : passes )
            count = std::max( count, pass.Pass + 1 );
        if ( m_PreparedRenderPasses.size() < count )
            m_PreparedRenderPasses.resize( count );
        for ( PreparedRenderPass& entry : m_PreparedRenderPasses )
            entry.Source = nullptr;
        // Every pass that opens a render pass (a merged pass continues the one its first pass opened; a pass a
        // fault re-opens resolves in BeginRenderPass). Sequential, before any pass records: no lock.
        for ( const RDG::CompiledPass& pass : passes )
        {
            const bool opens = RDG::HasFlag( pass.Flags, RDG::PassFlags::Raster ) &&
                               ( !pass.Attachments.empty() || pass.AreaWidth != 0 ) && !pass.ContinuesRenderPass;
            if ( !opens )
                continue;
            PreparedRenderPass& prepared = m_PreparedRenderPasses[pass.Pass];
            if ( ResolveRenderPass( pass, prepared ) )
                prepared.Source = &pass;
            else
                prepared = PreparedRenderPass{};
        }
    }

    Common::BoolResultStr VulkanRdgBackend::BeginRenderPass( const RDG::CompiledPass& pass )
    {
        ResumeSegment();
        // The planned pass PrepareRecording resolved: only looked up. A fault-adjusted copy (Load -> Clear) or a
        // merged pass re-opening after a fault closed its render pass resolves here, under the lock (the render
        // pass map and the framebuffer list are shared by every recording thread).
        if ( RecordingBuffer() == VK_NULL_HANDLE )
            return Common::MakeFormattedError( "graph '{}' pass '{}': the backend has no command buffer", m_GraphName,
                                               pass.Name );
        const PreparedRenderPass* prepared = nullptr;
        PreparedRenderPass        resolved;
        if ( pass.Pass < m_PreparedRenderPasses.size() && m_PreparedRenderPasses[pass.Pass].Source == &pass )
            prepared = &m_PreparedRenderPasses[pass.Pass];
        else
        {
            std::lock_guard<std::mutex> lock( m_RenderPassLock );
            Common::BoolResultStr       made = ResolveRenderPass( pass, resolved );
            if ( !made )
                return made;
            prepared = &resolved;
        }
        // RDG-PARALLEL-10: a chunk begins the render pass inline in its own primary, as the calling thread does.
        RecordBeginRenderPass( *prepared );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRdgBackend::ResolveRenderPass( const RDG::CompiledPass& pass,
                                                               PreparedRenderPass&      out )
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
        // CPU-FRAME-ARENA (MEM-7): the per-pass lists live in the recording thread's scratch, not on the heap.
        Common::Memory::FrameArena&                     scratch = ScratchArena();
        const Common::Memory::FrameArena::ScopedMark    scratchMark( scratch );
        const Common::Memory::ArenaAllocator<std::byte> scratchAlloc( scratch );
        using ColourView = std::pair<VkImageView, VkClearValue>;
        RdgRenderPassKey key;
        key.Colours.resize( colourCount, RdgAttachmentKey{} );
        const ColourView                        noView{ VK_NULL_HANDLE, VkClearValue{} };
        Common::Memory::ArenaVector<ColourView> colourViews( colourCount, noView, scratchAlloc );
        ColourView                              depthView    = noView;
        uint32_t                                resolveCount = 0;
        if ( resolves )
            resolveCount = colourCount;
        const VkImageView                        noImageView = VK_NULL_HANDLE;
        Common::Memory::ArenaVector<VkImageView> resolveViews( resolveCount, noImageView, scratchAlloc );
        if ( resolves )
            key.Resolves.resize( colourCount, RdgAttachmentKey{} );
        VkExtent2D                                   extent{ 0, 0 };
        uint32_t                                     layers = 1;
        for ( const RDG::AttachmentDecision& attachment : pass.Attachments )
        {
            const std::shared_ptr<VulkanRdgTexture>& texture = m_Textures[attachment.Resource];
            auto                                     view =
                 texture->GetView( { attachment.Mip, 1, attachment.BaseLayer, attachment.LayerCount }, true );
            if ( !view )
                return Common::MakeFormattedError( "attachment of '{}': {}", pass.Name, view.GetError() );
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
                key.Resolves[attachment.Slot] = described;
                resolveViews[attachment.Slot] = view.GetValue();
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
        if ( pass.AreaWidth != 0 )
        {
            // PassBuilder::RenderArea: no attachment, so the framebuffer has none and its size is the area. The
            // device limits are the only bound (CapabilityCatalog refuses a renderer that needs more).
            const VkPhysicalDeviceLimits& limits = m_Device.Limits;
            if ( pass.AreaWidth > limits.maxFramebufferWidth || pass.AreaHeight > limits.maxFramebufferHeight ||
                 pass.AreaWidth > limits.maxViewportDimensions[0] ||
                 pass.AreaHeight > limits.maxViewportDimensions[1] )
                return Common::MakeFormattedError(
                     "render area of '{}': {}x{} exceeds the device's framebuffer {}x{} / viewport {}x{}",
                     pass.Name, pass.AreaWidth, pass.AreaHeight, limits.maxFramebufferWidth,
                     limits.maxFramebufferHeight, limits.maxViewportDimensions[0],
                     limits.maxViewportDimensions[1] );
            extent = { pass.AreaWidth, pass.AreaHeight };
        }
        const Common::ResultStr<VkRenderPass> renderPass = GetRenderPass( key );
        if ( !renderPass )
            return Common::MakeFormattedError( "render pass of '{}': {}", pass.Name, renderPass.GetError() );

        // The clear values go straight into @p out (a kept entry keeps their capacity); a failure below leaves
        // @p out without a Source, which every caller treats as not prepared.
        Common::Memory::ArenaVector<VkImageView> views( scratchAlloc );
        std::vector<VkClearValue>&               clears = out.Clears;
        views.reserve( pass.Attachments.size() );
        clears.clear();
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
        for ( VkImageView view : resolveViews )
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
            if ( entry.RenderPass == renderPass.GetValue() &&
                 std::equal( entry.Views.begin(), entry.Views.end(), views.begin(), views.end() ) &&
                 entry.Extent.width == extent.width && entry.Extent.height == extent.height &&
                 entry.Layers == layers )
            {
                // Unique: an entry is added only when none matches, and BeginGraph drops the dead ones.
                framebuffer = entry.Framebuffer;
                break;
            }
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
            // The attachments' textures, in attachment order: only a new entry keeps them (a found one had them).
            std::vector<std::weak_ptr<VulkanRdgTexture>> textures;
            textures.reserve( pass.Attachments.size() );
            for ( const RDG::AttachmentDecision& attachment : pass.Attachments )
                textures.push_back( m_Textures[attachment.Resource] );
            std::vector<VkImageView> kept( views.begin(), views.end() );
            m_Framebuffers.push_back( { renderPass.GetValue(), std::move( kept ), std::move( textures ), extent,
                                        layers, framebuffer } );
        }

        out.RenderPass    = renderPass.GetValue();
        out.Framebuffer   = framebuffer;
        out.Extent        = extent;
        out.Compatibility = RdgCompatibilityKey( key );
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::RecordBeginRenderPass( const PreparedRenderPass& prepared )
    {
        const VkExtent2D      extent = prepared.Extent;
        VkRenderPassBeginInfo begin{};
        begin.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass      = prepared.RenderPass;
        begin.framebuffer     = prepared.Framebuffer;
        begin.renderArea      = { { 0, 0 }, extent };
        begin.clearValueCount = static_cast<uint32_t>( prepared.Clears.size() );
        begin.pClearValues    = prepared.Clears.data();
        const VkCommandBuffer commandBuffer = RecordingBuffer(); // the chunk's primary on a worker
        vkCmdBeginRenderPass( commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE );
        OpenRenderPassState() = prepared.Compatibility;

        // The whole target: what nearly every pass wants, set once here instead of in every exec lambda. The
        // engine's convention (VulkanRendererAPI::SetViewportAndScissor): negative height, so +Y is up and the
        // engine's pipelines draw the same picture in a graph-opened render pass as in their own.
        const VkViewport viewport{ 0.0f,
                                   static_cast<float>( extent.height ),
                                   static_cast<float>( extent.width ),
                                   -static_cast<float>( extent.height ),
                                   0.0f,
                                   1.0f };
        const VkRect2D   scissor{ { 0, 0 }, extent };
        vkCmdSetViewport( commandBuffer, 0, 1, &viewport );
        vkCmdSetScissor( commandBuffer, 0, 1, &scissor );
        // RDG-PARALLEL-4: every graphics pipeline enables VK_DYNAMIC_STATE_LINE_WIDTH (VulkanPipeline). Set at each
        // render pass begin (on the calling thread and in every chunk), so no draw relies on a width an earlier
        // pass left - a chunk's freshly begun primary has none. Lines that want another width set it at their draw.
        vkCmdSetLineWidth( commandBuffer, 1.0f );
    }

    Common::BoolResultStr VulkanRdgBackend::UploadBuffer( uint32_t resource, std::span<const std::byte> bytes )
    {
        ResumeSegment();
        const VkCommandBuffer commandBuffer = RecordingBuffer();
        if ( commandBuffer == VK_NULL_HANDLE )
            return Common::MakeFormattedError( "graph '{}': buffer upload outside a recording pass", m_GraphName );
        if ( OpenRenderPassState().has_value() )
            return Common::MakeFormattedError( "graph '{}': buffer upload inside a render pass", m_GraphName );
        const std::shared_ptr<VulkanRdgBuffer>& target =
             resource < m_Buffers.size() ? m_Buffers[resource] : std::shared_ptr<VulkanRdgBuffer>{};
        if ( target == nullptr )
            return Common::MakeFormattedError(
                 "graph '{}': buffer upload into resource {}, which has no Vulkan buffer", m_GraphName, resource );
        constexpr size_t kUpdateLimit = 65536; // vkCmdUpdateBuffer: dataSize <= 65536, a multiple of 4
        for ( size_t offset = 0; offset < bytes.size(); offset += kUpdateLimit )
        {
            const size_t size = std::min( kUpdateLimit, bytes.size() - offset );
            vkCmdUpdateBuffer( commandBuffer, target->GetBuffer(), target->GetOffset() + offset, size,
                               bytes.data() + offset );
        }
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::EndRenderPass()
    {
        const VkCommandBuffer commandBuffer = RecordingBuffer(); // the chunk's primary on a worker
        if ( commandBuffer != VK_NULL_HANDLE )
            vkCmdEndRenderPass( commandBuffer );
        OpenRenderPassState().reset();
    }

    Common::BoolResultStr VulkanRdgBackend::BeginChunks( std::span<const RDG::RecordChunkDesc> chunks )
    {
        if ( m_Queues == nullptr || m_Queues->Objects == nullptr || m_Descriptors == nullptr )
            return Common::MakeFormattedError( "graph '{}': chunks before BeginFrame", m_GraphName );
        const auto count = static_cast<uint32_t>( chunks.size() );
        while ( m_Chunks.size() < count )
            m_Chunks.push_back( std::make_unique<Chunk>() );
        std::array<uint32_t, 2> perPipe{};
        for ( uint32_t index = 0; index < count; ++index )
        {
            Chunk& chunk = *m_Chunks[index];
            chunk.Owner  = this;
            chunk.Desc   = chunks[index];
            chunk.Arena  = index;
            chunk.Record.ForgetBoundState();
            chunk.Record.CommandBuffer = VK_NULL_HANDLE;
            chunk.Record.GpuScopes.clear();
            chunk.Record.GpuScopesMade.clear();
            chunk.Record.GpuOrder     = 0; // re-stamped at replay (StampChunkOrder)
            chunk.Record.GpuOrderNext = 0;
            chunk.OpenRenderPass.reset();
            chunk.PassSets.Clear();
            chunk.ProfilerScopes.clear();
#if DESERT_DEV_INSTRUMENTS
            chunk.CpuScope.reset();
#endif
            chunk.Labels.clear();
            chunk.CommandBuffer = VK_NULL_HANDLE;
            chunk.Failure.clear();
            chunk.Scratch.Reset(); // CPU-FRAME-ARENA: no mark is live between graphs; folds its blocks into one
            const size_t pipe = chunk.Desc.OnPipe == RDG::Pipe::Graphics ? 0 : 1;
            perPipe[pipe]     = index + 1;
            // RDG-PARALLEL-1-1: the chunk records while this thread walks the graph (its scope stack and its
            // profiler queue change under it), so where the chunk's scopes hang is taken now, on its own pipe.
            Common::Profiling::GpuQueue queue = Common::Profiling::GpuQueue::Graphics;
            if ( pipe != 0 )
                queue = Common::Profiling::GpuQueue::AsyncCompute;
            chunk.Record.GpuRoot = {};
#if DESERT_DEV_INSTRUMENTS // the GPU profiler is a dev instrument: Shipping declares no VulkanGpuProfiler
            if ( m_Device.ChunkOrder != nullptr )
                chunk.Record.GpuRoot = m_Device.ChunkOrder->ChunkRoot( queue );
#endif
        }
        for ( size_t pipe = 0; pipe < perPipe.size(); ++pipe )
        {
            if ( perPipe[pipe] == 0 )
                continue;
            RDG::Pipe onPipe = RDG::Pipe::Graphics;
            if ( pipe != 0 )
                onPipe = RDG::Pipe::AsyncCompute;
            Common::BoolResultStr reserved = m_Queues->Objects->ReserveChunkArenas( onPipe, perPipe[pipe] );
            if ( !reserved )
                return reserved;
        }
        m_Descriptors->ReserveArenas( count + 1 );
        m_ChunkCount = count;
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::BeginChunk( uint32_t index )
    {
        Chunk& chunk          = *m_Chunks[index];
        chunk.PreviousChunk   = t_Chunk;
        chunk.PreviousContext = VulkanRecordContext::Bind( &chunk.Record );
        chunk.PreviousArena   = VulkanRdgPassDescriptors::BindArena( chunk.Arena + 1 );
        t_Chunk               = &chunk;
        chunk.Record.ForgetBoundState(); // a freshly begun primary holds no binding
        Common::ResultStr<VkCommandBuffer> begun =
             m_Queues->Objects->BeginChunkCommandBuffer( chunk.Desc.OnPipe, chunk.Arena );
        if ( !begun )
        {
            chunk.Failure              = begun.GetError();
            chunk.Record.CommandBuffer = VK_NULL_HANDLE;
            return;
        }
        chunk.CommandBuffer        = begun.GetValue();
        chunk.Record.CommandBuffer = chunk.CommandBuffer;
    }

    void VulkanRdgBackend::EndChunk( uint32_t index )
    {
        Chunk& chunk = *m_Chunks[index];
        // A pass that failed mid-way leaves its bracket open: it is closed here, as AbandonGraph does inline.
        if ( chunk.OpenRenderPass )
            EndRenderPass();
        while ( !chunk.ProfilerScopes.empty() )
            EndPass( RDG::CompiledPass{} );
        chunk.Labels.clear();
        if ( chunk.CommandBuffer != VK_NULL_HANDLE && vkEndCommandBuffer( chunk.CommandBuffer ) != VK_SUCCESS &&
             chunk.Failure.empty() )
            chunk.Failure = "vkEndCommandBuffer of a chunk failed";
        chunk.Record.CommandBuffer = VK_NULL_HANDLE;
        t_Chunk = chunk.PreviousChunk;
        VulkanRecordContext::Bind( chunk.PreviousContext );
        VulkanRdgPassDescriptors::BindArena( chunk.PreviousArena );
    }

    Common::BoolResultStr VulkanRdgBackend::ExecuteChunk( uint32_t index )
    {
        if ( index >= m_ChunkCount )
            return Common::MakeFormattedError( "graph '{}': chunk {} was not begun", m_GraphName, index );
        Chunk& chunk = *m_Chunks[index];
        if ( !chunk.Failure.empty() )
            return Common::MakeFormattedError( "{}", chunk.Failure );
        if ( m_CommandBuffer == VK_NULL_HANDLE && !m_ContinuationPending )
            return Common::MakeFormattedError( "graph '{}': chunk {} replayed outside a segment", m_GraphName,
                                               index );
        if ( m_Queues == nullptr )
            return Common::MakeFormattedError( "graph '{}': chunk {} replayed without queues", m_GraphName, index );
        // RDG-PARALLEL-10 (UE: the parallel command lists' primary payloads, submitted in order): what the segment
        // recorded so far ends, the chunk's primary follows it in the same submission - no secondary, no render
        // pass re-begun here, no state re-emitted per piece. CPU-RDG-RECORD (F6): the continuation is begun only
        // when the calling thread records again (ResumeSegment), so adjacent chunks cost one End, not one pair.
        const Common::BoolResultStr spliced = m_Segments.SpliceChunk( chunk.CommandBuffer );
        SetRecording( VK_NULL_HANDLE ); // the ended buffer takes nothing more
        if ( !spliced )
            return Common::MakeFormattedError( "graph '{}' chunk {}: {}", m_GraphName, index, spliced.GetError() );
        chunk.CommandBuffer   = VK_NULL_HANDLE; // the segment's submission holds it now
        m_ContinuationPending = true;
#if DESERT_DEV_INSTRUMENTS
        if ( m_Device.ChunkOrder != nullptr )
            m_Device.ChunkOrder->StampChunkOrder( chunk.Record.GpuScopesMade );
#endif
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::ResumeSegment()
    {
        if ( !m_ContinuationPending || BoundChunk() != nullptr )
            return;
        m_ContinuationPending = false;
        const Common::ResultStr<VkCommandBuffer> continued = m_Segments.Continue( *m_Queues );
        if ( !continued )
        {
            m_ResumeFailure = continued.GetError();
            return;
        }
        SetRecording( continued.GetValue() );
        // The frame recorder's bind cache does not describe the continuation (a freshly begun buffer).
        if ( m_PassBoundaryListener )
            m_PassBoundaryListener();
    }

    void VulkanRdgBackend::DiscardChunk( uint32_t index )
    {
        if ( index >= m_ChunkCount )
            return;
        // Its primary is never submitted (the arena resets with the slot), so the queries of its scopes are
        // never written. RDG-PARALLEL-5: its passes are recorded again inline, each with a scope of its own, so
        // the chunk's scopes are retracted: [GpuOrder] holds every pass once, no duplicate row read untimed.
        Chunk& chunk = *m_Chunks[index];
#if DESERT_DEV_INSTRUMENTS
        if ( m_Device.ChunkOrder != nullptr )
            m_Device.ChunkOrder->RetractChunkScopes( chunk.Record.GpuScopesMade );
#endif
        chunk.Record.GpuScopesMade.clear();
        chunk.CommandBuffer = VK_NULL_HANDLE; // ended, never submitted: reset with the slot's arena
    }

    void VulkanRdgBackend::EndChunks()
    {
        m_ChunkCount = 0;
    }

    Common::BoolResultStr VulkanRdgBackend::EndGraph( std::span<const RDG::Barrier> finalBarriers )
    {
        if ( !finalBarriers.empty() )
        {
            const int32_t scope = BeginGraphCommandsScope( "final barriers" );
            RecordBarriers( finalBarriers );
            EndGraphCommandsScope( scope );
        }
        CloseQueueBoundaryScope();
        Release();
        m_Segments.EndGraph();
        m_Result = nullptr;
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::AbandonGraph()
    {
        // The dispatch has returned, so every chunk secondary is ended; they are simply never executed.
        m_ChunkCount          = 0;
        m_ContinuationPending = false;
        m_ResumeFailure.clear();
        if ( m_OpenRenderPass )
            EndRenderPass();
        while ( !m_ProfilerScopes.empty() )
            EndPass( RDG::CompiledPass{} );
        if ( m_SegmentScope >= 0 && m_Device.Profiler != nullptr )
            m_Device.Profiler->EndScope( m_SegmentScope );
        m_SegmentScope = -1;
        if ( m_PrologueScope >= 0 )
            EndGraphCommandsScope( m_PrologueScope );
        m_PrologueScope = -1;
        CloseQueueBoundaryScope();
        m_Result = nullptr;
        Release();
        m_Segments.AbandonGraph();
        SetRecording( VK_NULL_HANDLE );
    }

    // ── RDG-CONTRACTS B: pipes, segments, submissions ────────────────────────────────────────────────────

    RDG::PipeCapabilities VulkanRdgBackend::GetPipeCapabilities() const
    {
        // Before BeginFrame there are no queues; BeginGraph refuses that graph by name.
        return m_Queues != nullptr ? m_Queues->GetCapabilities() : RDG::PipeCapabilities{};
    }

    RDG::AsyncComputeFallbackLog& VulkanRdgBackend::GetAsyncComputeFallbackLog()
    {
        return m_FallbackLog;
    }

    RDG::PassFaultReporter& VulkanRdgBackend::GetPassFaultReporter()
    {
        return m_FaultReporter;
    }

    RDG::CompileCache* VulkanRdgBackend::GetCompileCache()
    {
        if ( !::Desert::Graphic::RenderConfig::RdgCompileCache.load( std::memory_order_relaxed ) )
            return nullptr;
        return &m_CompileCache;
    }

    Common::BoolResultStr VulkanRdgBackend::BeginPipeSegment( const RDG::PipeSegment& segment )
    {
        if ( m_Queues == nullptr )
            return Common::MakeError( "the Vulkan backend has no queues (BeginFrame)" );
        const Common::ResultStr<VkCommandBuffer> begun = m_Segments.BeginSegment( segment, *m_Queues );
        if ( !begun )
            return Common::MakeError( begun.GetError() );
        SetRecording( begun.GetValue() );
        // The graphics queue's boundary row ends at the head of its next command buffer (OpenQueueBoundaryScope).
        if ( segment.OnPipe == RDG::Pipe::Graphics )
            CloseQueueBoundaryScope();
        // Every scope from here to EndPipeSegment is on this segment's queue: the profiler keeps one timeline
        // per queue, and an async pass tiled against the graphics bracket would hide the graphics remainder.
        if ( m_Device.Profiler != nullptr )
            m_Device.Profiler->SetQueue( segment.OnPipe == RDG::Pipe::AsyncCompute
                                              ? Common::Profiling::GpuQueue::AsyncCompute
                                              : Common::Profiling::GpuQueue::Graphics );
        if ( begun.GetValue() != VK_NULL_HANDLE )
            RecordSharedHeapBarrier( begun.GetValue() );
        // The async segment's own GPU row. Only a segment that waits on a fork: the frame's query reset is
        // recorded at the head of the graphics queue, and only a fork orders this queue after it.
        // The prologue segment has no passes, only the releases of a fork from the graph start (RDGBuilder
        // Execute).
        if ( segment.FirstPosition == RDG::CrossPipeSync::kForkAtGraphStart )
            m_PrologueScope = BeginGraphCommandsScope( "fork releases at the graph start" );
        // A later piece of a run cut at a join (RDGCompile CutRunsAtConsumers), or a run an earlier run's fork
        // already orders (RDGCompile CreateAsyncSyncs, the newest fork), waits on nothing itself, but it follows
        // that earlier async segment on the compute queue: its fork orders this one too.
        bool forked = segment.OnPipe == RDG::Pipe::AsyncCompute && !segment.WaitSyncs.empty();
        if ( segment.OnPipe == RDG::Pipe::AsyncCompute && !forked && m_Result != nullptr &&
             segment.FirstPosition != RDG::CrossPipeSync::kForkAtGraphStart )
        {
            for ( const RDG::PipeSegment& earlier : m_Result->Segments )
            {
                if ( earlier.OnPipe == RDG::Pipe::AsyncCompute &&
                     earlier.FirstPosition != RDG::CrossPipeSync::kForkAtGraphStart &&
                     earlier.FirstPosition < segment.FirstPosition && !earlier.WaitSyncs.empty() )
                    forked = true;
            }
        }
        if ( forked && m_Device.Profiler != nullptr && m_Result != nullptr &&
             segment.FirstPosition != RDG::CrossPipeSync::kForkAtGraphStart )
        {
            m_SegmentScopeName = std::string( Common::Profiling::kGpuAsyncSegmentScopePrefix ) +
                                 m_Result->Passes[segment.FirstPosition].Name + " .. " +
                                 m_Result->Passes[segment.LastPosition].Name + " (" +
                                 std::to_string( segment.LastPosition - segment.FirstPosition + 1 ) + " passes)";
            m_SegmentScope     = m_Device.Profiler->BeginScope( m_SegmentScopeName.c_str() );
        }
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::OpenQueueBoundaryScope( const RDG::PipeSegment& ending )
    {
        CloseQueueBoundaryScope();
        if ( m_Device.Profiler == nullptr || m_Result == nullptr )
            return;
        const std::vector<RDG::PipeSegment>& segments = m_Result->Segments;
        size_t                               at       = segments.size();
        for ( size_t i = 0; i < segments.size(); ++i )
            if ( segments[i].OnPipe == ending.OnPipe && segments[i].FirstPosition == ending.FirstPosition &&
                 segments[i].LastPosition == ending.LastPosition )
                at = i;
        if ( at + 1 >= segments.size() )
            return; // the graph's last segment: the final barriers record into it, no boundary follows
        const RDG::PipeSegment* nextGraphics = nullptr;
        for ( size_t i = at + 1; i < segments.size() && nextGraphics == nullptr; ++i )
            if ( segments[i].OnPipe == RDG::Pipe::Graphics )
                nextGraphics = &segments[i];
        const auto passName = [&]( uint32_t position ) -> std::string_view
        {
            return position < m_Result->Passes.size() ? std::string_view( m_Result->Passes[position].Name )
                                                      : std::string_view( "graph start" );
        };
        if ( nextGraphics == nullptr ) // only async segments follow: the tail waits on their graph-end joins
            m_QueueBoundaryScopeName = "RDG Wait: graphics queue on async compute at the graph end";
        else if ( !nextGraphics->WaitSyncs.empty() )
            m_QueueBoundaryScopeName = std::format( "RDG Wait: graphics queue on async compute before '{}'",
                                                    passName( nextGraphics->FirstPosition ) );
        else
            m_QueueBoundaryScopeName =
                 std::format( "RDG Resubmit: graphics queue after '{}'", passName( ending.LastPosition ) );
        m_QueueBoundaryScope = m_Device.Profiler->BeginScope( m_QueueBoundaryScopeName.c_str() );
    }

    void VulkanRdgBackend::CloseQueueBoundaryScope()
    {
        if ( m_QueueBoundaryScope >= 0 && m_Device.Profiler != nullptr )
            m_Device.Profiler->EndScope( m_QueueBoundaryScope );
        m_QueueBoundaryScope = -1;
    }

    void VulkanRdgBackend::RecordSharedHeapBarrier( VkCommandBuffer commandBuffer )
    {
        if ( !m_SharedHeapBarrierPending )
            return;
        m_SharedHeapBarrierPending = false;
        const int32_t   scope      = BeginGraphCommandsScope( "shared heap reuse" );
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr );
        EndGraphCommandsScope( scope );
    }

    Common::BoolResultStr VulkanRdgBackend::EndPipeSegment( const RDG::PipeSegment& segment )
    {
        // The segment's own buffer records its closing scopes and barriers: a segment that ended on a chunk
        // continues once here (one Begin after the segment's run of spliced chunks).
        ResumeSegment();
        if ( !m_ResumeFailure.empty() )
            return Common::MakeError( std::exchange( m_ResumeFailure, {} ) );
        if ( m_OpenRenderPass )
            return Common::MakeError( "a render pass is open across a segment boundary" );
        if ( m_SegmentScope >= 0 )
        {
            m_Device.Profiler->EndScope( m_SegmentScope );
            m_SegmentScope = -1;
        }
        if ( m_PrologueScope >= 0 )
        {
            EndGraphCommandsScope( m_PrologueScope );
            m_PrologueScope = -1;
        }
        // Whatever records next (the next segment, or the frame's buffer after the graph) is graphics until a
        // segment says otherwise.
        if ( m_Device.Profiler != nullptr )
            m_Device.Profiler->SetQueue( Common::Profiling::GpuQueue::Graphics );
        // Still recording into this graphics segment's buffer: the boundary row opens at its end.
        if ( segment.OnPipe == RDG::Pipe::Graphics )
            OpenQueueBoundaryScope( segment );
        const Common::ResultStr<VkCommandBuffer> next = m_Segments.EndSegment( segment, *m_Queues );
        SetRecording( next ? next.GetValue() : VK_NULL_HANDLE );
        // An async segment after which the recorder opened the graph's graphics tail: the tail's head ends it.
        if ( segment.OnPipe != RDG::Pipe::Graphics && next && next.GetValue() != VK_NULL_HANDLE )
            CloseQueueBoundaryScope();
        if ( !next )
            return Common::MakeError( next.GetError() );
        return Common::MakeSuccess( true );
    }

    void VulkanRdgBackend::RecordEpilogueBarriers( std::span<const RDG::Barrier> barriers )
    {
        // Releases (RecordBarriers ends each at BOTTOM_OF_PIPE with dstAccess empty: the acquire on the other queue
        // makes it visible) and, on Graphics, the opening Transition of a held read group (a full barrier).
        RecordBarriers( barriers );
    }

    void VulkanRdgBackend::SetRecording( VkCommandBuffer commandBuffer )
    {
        m_CommandBuffer = commandBuffer;
        if ( m_RecordingListener )
            m_RecordingListener( commandBuffer );
    }

    std::vector<VulkanRdgSubmission> VulkanRdgBackend::TakeSubmissions()
    {
        SetRecording( VK_NULL_HANDLE );
        return m_Segments.Take();
    }

    std::shared_ptr<RDG::IPhysicalTexture> VulkanRdgBackend::GetPhysicalTexture( uint32_t resource ) const
    {
        return resource < m_Textures.size() ? m_Textures[resource] : nullptr;
    }

    std::shared_ptr<RDG::IPhysicalBuffer> VulkanRdgBackend::GetPhysicalBuffer( uint32_t resource ) const
    {
        return resource < m_Buffers.size() ? m_Buffers[resource] : nullptr;
    }

    RDG::IPhysicalTexture* VulkanRdgBackend::FindPhysicalTexture( uint32_t resource ) const
    {
        return resource < m_Textures.size() ? m_Textures[resource].get() : nullptr;
    }

    RDG::IPhysicalBuffer* VulkanRdgBackend::FindPhysicalBuffer( uint32_t resource ) const
    {
        return resource < m_Buffers.size() ? m_Buffers[resource].get() : nullptr;
    }

    void VulkanRdgBackend::Release()
    {
        // CPU-FRAME-ARENA (MEM-7): kept with their capacity; no Source points into the ended graph.
        for ( PreparedRenderPass& entry : m_PreparedRenderPasses )
            entry.Source = nullptr;
        m_Textures.clear();
        m_Buffers.clear();
        // The allocator's open graph ends with the backend's hold (EndGraph, AbandonGraph, a new BeginGraph).
        if ( m_Transients != nullptr )
            m_Transients->EndGraph( m_GraphName );
        m_GraphName.clear();
    }
} // namespace Desert::Graphic::API::Vulkan
