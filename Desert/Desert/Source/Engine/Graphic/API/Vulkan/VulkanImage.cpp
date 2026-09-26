#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/PixelPack.hpp> // the one packer this and the swapchain readback share
#include <Engine/Graphic/RenderConfig.hpp>

#include <Common/Settings/MachineSettings.hpp>
#include <Common/Utilities/String.hpp>

#include <algorithm>
#include <limits>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // The clear value of a UINT/SINT image is read as integers; every other colour format reads floats.
        bool IsIntegerColourFormat( VkFormat format )
        {
            switch ( format )
            {
                case VK_FORMAT_R8_UINT:
                case VK_FORMAT_R8_SINT:
                case VK_FORMAT_R8G8_UINT:
                case VK_FORMAT_R8G8B8A8_UINT:
                case VK_FORMAT_R8G8B8A8_SINT:
                case VK_FORMAT_R16_UINT:
                case VK_FORMAT_R16_SINT:
                case VK_FORMAT_R16G16_UINT:
                case VK_FORMAT_R16G16B16A16_UINT:
                case VK_FORMAT_R32_UINT:
                case VK_FORMAT_R32_SINT:
                case VK_FORMAT_R32G32_UINT:
                case VK_FORMAT_R32G32B32A32_UINT:
                case VK_FORMAT_R32G32B32A32_SINT:
                    return true;
                default:
                    return false;
            }
        }
    } // namespace

    namespace Utils
    {
        static VkImageLayout GetDefaultLayout( Core::Formats::ImageFormat format, Core::Formats::ImageProperties props )
        {
            if ( props & Core::Formats::Storage ) return VK_IMAGE_LAYOUT_GENERAL;
            if ( Graphic::Utils::IsDepthFormat( format ) ) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Where a sampler's filtering comes from.
        enum class SamplerFilterPolicy
        {
            // Follow the global Scene Settings texture filter (RenderConfig) — ordinary textures, whose
            // filtering IS a user quality preference.
            Global,
            // Always LINEAR with REPEAT addressing, whatever the user picked. For volumes whose
            // interpolation is part of the algorithm: with "Nearest" selected for texture quality, a
            // trilinearly-sampled noise volume would render as visible voxels instead of a smooth field.
            AlwaysLinear
        };

        static void CreateSampler( VkDevice device, VkSampler& outSampler, SamplerFilterPolicy policy )
        {
            // The machine's global filter (Common::Settings::MachineSettings, pushed into RenderConfig by
            // SceneRenderer): Nearest | Bilinear (linear, nearest mip) | Trilinear | Anisotropic.
            using FM               = Common::Settings::TextureFilter;
            const bool forceLinear = policy == SamplerFilterPolicy::AlwaysLinear;
            const int  mode        = Graphic::RenderConfig::TextureFilter.load();
            const bool nearest     = !forceLinear && mode == static_cast<int>( FM::Nearest );
            const bool linearMip   = forceLinear || mode == static_cast<int>( FM::Trilinear ) ||
                                   mode == static_cast<int>( FM::Anisotropic );

            const VkFilter            filter  = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            const VkSamplerMipmapMode mipMode = linearMip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

            // Anisotropy: only when requested AND the device supports it (MaxAnisotropy > 1 = supported).
            // Never for a volume: anisotropic filtering of a 3D noise field buys nothing and is not
            // guaranteed for VK_IMAGE_TYPE_3D.
            const float deviceMaxAniso = Graphic::RenderConfig::MaxAnisotropy.load();
            const bool  useAniso =
                 !forceLinear && mode == static_cast<int>( FM::Anisotropic ) && deviceMaxAniso > 1.0f;
            // User-selected level (4/8/16x), clamped to what the device supports.
            const float requestedAniso = static_cast<float>( Graphic::RenderConfig::AnisotropyLevel.load() );
            const float maxAniso =
                 useAniso ? ( requestedAniso < deviceMaxAniso ? requestedAniso : deviceMaxAniso ) : 1.0f;

            VkSamplerCreateInfo info = {
                 .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                 .magFilter        = filter,
                 .minFilter        = filter,
                 .mipmapMode       = mipMode,
                 .addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                 .addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                 .addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                 .mipLodBias       = 0.0f,
                 .anisotropyEnable = useAniso ? VK_TRUE : VK_FALSE,
                 .maxAnisotropy    = maxAniso,
                 .minLod           = 0.0f,
                 .maxLod           = 100.0f,
                 .borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };

            // Guard the promise, not the branch: everything above is derived from several variables, and
            // a later edit that reintroduces the global filter into this path would otherwise be found
            // only by looking at a voxelised sky.
            if ( forceLinear )
            {
                DESERT_VERIFY( info.magFilter == VK_FILTER_LINEAR && info.minFilter == VK_FILTER_LINEAR &&
                                    info.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT &&
                                    info.addressModeV == VK_SAMPLER_ADDRESS_MODE_REPEAT &&
                                    info.addressModeW == VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               "A volume sampler must be LINEAR/REPEAT regardless of the texture filter" );
            }

            VK_CHECK_RESULT( vkCreateSampler( device, &info, nullptr, &outSampler ) );
        }

        static VkImageView CreateView( VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, 
                                       VkImageViewType type, uint32_t layerCount, uint32_t mipCount, uint32_t baseMip = 0 )
        {
            VkImageViewCreateInfo info = {
                 .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                 .image            = image,
                 .viewType         = type,
                 .format           = format,
                 .subresourceRange = { .aspectMask     = aspect,
                                       .baseMipLevel   = baseMip,
                                       .levelCount     = mipCount,
                                       .baseArrayLayer = 0,
                                       .layerCount     = layerCount } };

            VkImageView view;
            VK_CHECK_RESULT( vkCreateImageView( device, &info, nullptr, &view ) );
            return view;
        }

        static const void* GetPixelDataPtr( const Core::Formats::ImagePixelData& data )
        {
            if ( auto uchar = std::get_if<std::vector<unsigned char>>( &data ) ) return uchar->data();
            if ( auto f32   = std::get_if<std::vector<float>>( &data ) )        return f32->data();
            if ( auto b     = std::get_if<std::byte*>( &data ) )                return *b;
            return nullptr;
        }
    }

    VkFormat GetImageVulkanFormat( const Core::Formats::ImageFormat& format )
    {
        // DEPTH24STENCIL8 is the one entry the table cannot answer on its own — it means "whatever packed
        // depth+stencil format this physical device picked". Everything else is fixed, so the device is
        // only consulted when it actually has a say.
        const VkFormat deviceDepthFormat =
             format == Core::Formats::ImageFormat::DEPTH24STENCIL8
                  ? SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                         ->GetPhysicalDevice()
                         ->GetDepthFormat()
                  : VK_FORMAT_UNDEFINED;

        return Utils::GetVulkanFormat( format, deviceDepthFormat );
    }

    VkImageAspectFlags GetImageVulkanAspect( Core::Formats::ImageFormat format )
    {
        const Core::Formats::ImageAspect aspect = Core::Formats::GetImageAspect( format );

        VkImageAspectFlags flags = 0;
        if ( aspect & Core::Formats::ImageAspect_Colour )
            flags |= VK_IMAGE_ASPECT_COLOR_BIT;
        if ( aspect & Core::Formats::ImageAspect_Depth )
            flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ( aspect & Core::Formats::ImageAspect_Stencil )
            flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return flags;
    }

    // --- VulkanImage2D ---

    VulkanImage2D::VulkanImage2D( const Core::Formats::Image2DSpecification& spec ) : m_Specification( spec ) {}
    VulkanImage2D::~VulkanImage2D()
    {
        Release();
    }
    Common::BoolResultStr VulkanImage2D::Invalidate() { return RT_Invalidate(); }
    Common::BoolResultStr VulkanImage2D::RT_Invalidate() { Release(); return CreateResource(); }

    Common::BoolResultStr VulkanImage2D::Release()
    {
        if ( !m_Resource.Image ) return BOOLSUCCESS;
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator().get();
        allocator->RT_DestroyImage( m_Resource.Image, m_Resource.Allocation, m_Resource.ImageView, m_Resource.Sampler, m_MipViews );
        m_Resource = {}; m_MipViews.clear(); m_IsLoaded = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImage2D::SetData( const Core::Formats::ImagePixelData& data )
    {
        // Stream new pixels into the already-allocated image (single-mip, sampled). Same staging-upload +
        // synchronous flush as CreateResource, but the VkImage / view / sampler / descriptor stay put — so
        // callers holding the Image2D* (e.g. Render2D's per-texture executor, video playback) keep working.
        if ( !m_IsLoaded || m_Resource.Image == VK_NULL_HANDLE )
            return Common::MakeError<bool>( "Image2D::SetData on an uninitialised image" );
        if ( !Core::Formats::HasData( data ) )
            return Common::MakeError<bool>( "Image2D::SetData with empty pixel data" );

        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        const uint64_t size = Core::Formats::CalculateImageSize( m_Specification.Width, m_Specification.Height,
                                                                 m_Specification.Format );
        const VkImageLayout finalLayout =
             Utils::GetDefaultLayout( m_Specification.Format, m_Specification.Properties );

        VkBuffer           staging;
        VmaAllocation      stagingAlloc;
        VkBufferCreateInfo bInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size  = size,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        // ASKED, NOT ASSUMED — the same correction as the command buffer below. GetValue() on a failed
        // result hands back a default-constructed VmaAllocation (null), which the map would then refuse
        // and the upload would silently carry no pixels.
        const auto stagingResult =
             allocator->RT_AllocateBuffer( "SetDataStaging", bInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, staging );
        if ( !stagingResult.IsSuccess() )
            return Common::MakeFormattedError<bool>( "Image2D::SetData: {} byte staging buffer failed: {}", size,
                                                     stagingResult.GetError() );
        stagingAlloc = stagingResult.GetValue();

        {
            MappedMemory staged = allocator->MapMemory( stagingAlloc );
            const auto   wrote  = staged.Write( Utils::GetPixelDataPtr( data ), static_cast<size_t>( size ) );
            if ( !wrote.IsSuccess() )
            {
                allocator->RT_DestroyBuffer( staging, stagingAlloc );
                return Common::MakeFormattedError<bool>( "Image2D::SetData: {}", wrote.GetError() );
            }
        }

        // ASKED, NOT ASSUMED. GetValue() on a failed result hands back a default-constructed
        // VkCommandBuffer — VK_NULL_HANDLE — and every vkCmd* below would then be undefined behaviour
        // rather than a no-op. The allocator can now refuse (a lost device), so the question is live.
        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc.IsSuccess() )
        {
            allocator->RT_DestroyBuffer( staging, stagingAlloc );
            return Common::MakeFormattedError<bool>( "SetData: no command buffer: {}", cmdAlloc.GetError() );
        }
        const VkCommandBuffer cmd = cmdAlloc.GetValue();

        // The tracked layout (SHADER_READ_ONLY after the first upload) is the transition source.
        TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
        VkBufferImageCopy copy = {
             .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
             .imageExtent      = { m_Specification.Width, m_Specification.Height, 1 } };
        vkCmdCopyBufferToImage( cmd, staging, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );
        TransitionLayout( cmd, finalLayout );

        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
        allocator->RT_DestroyBuffer( staging, stagingAlloc );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImage2D::CreateResource()
    {
        auto vkDevice = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator().get();

        m_Resource.Format     = GetImageVulkanFormat( m_Specification.Format );
        m_Resource.MipLevels  = m_Specification.Mips;
        m_Resource.LayerCount = 1;
        m_Resource.Layout     = VK_IMAGE_LAYOUT_UNDEFINED;

        const bool zeroOnCreate = m_Specification.InitialContent == Core::Formats::ImageInitialContent::Zero;
        if ( zeroOnCreate && Core::Formats::HasData( m_Specification.Data ) )
            return Common::MakeFormattedError<bool>(
                 "Image2D '{}' supplies pixel data AND asks for a zero initial content; the data is the content.",
                 m_Specification.Tag );
        if ( zeroOnCreate && m_Specification.Samples > 1 )
            return Common::MakeFormattedError<bool>(
                 "Image2D '{}' is multisampled ({} samples) and asks for a zero initial content; a multisampled "
                 "image carries no transfer usage and is only ever read through its resolve.",
                 m_Specification.Tag, m_Specification.Samples );

        // The complete mip chain for these dimensions is floor(log2(max(w,h)))+1 levels.
        uint32_t maxChainDim = std::max( m_Specification.Width, m_Specification.Height );
        uint32_t maxMipLevels = 1;
        while ( maxChainDim > 1 ) { maxChainDim >>= 1; ++maxMipLevels; }

        // A CHAIN SUPPLIED BY THE CALLER IS THE CHAIN. `MipLevels` is the authority when it is
        // non-empty, and `Mips` is then not consulted — see Image2DSpecification. Setting both is
        // refused rather than resolved: a count and a table that disagree can only be a mistake, and
        // the wrong half of it draws.
        const bool suppliedChain = !m_Specification.MipLevels.empty();
        if ( suppliedChain )
        {
            if ( m_Specification.Mips != 1 )
            {
                return Common::MakeFormattedError<bool>(
                     "Image2D '{}' supplies a {}-level mip chain AND asks for Mips={}. The table is the "
                     "chain; Mips must be left at its default when one is supplied.",
                     m_Specification.Tag, m_Specification.MipLevels.size(), m_Specification.Mips );
            }
            if ( !Core::Formats::HasData( m_Specification.Data ) )
            {
                return Common::MakeFormattedError<bool>(
                     "Image2D '{}' supplies a {}-level mip table and no pixel data for it to describe.",
                     m_Specification.Tag, m_Specification.MipLevels.size() );
            }
            m_Resource.MipLevels = static_cast<uint32_t>( m_Specification.MipLevels.size() );
        }

        // Clamp to the chain length regardless of source. An over-specified Mips count (e.g. a small render
        // target asking for more levels than its size allows) is an invalid vkCreateImage
        // (VUID-VkImageCreateInfo-mipLevels-00958) and produces a broken image -> garbage / GPU faults on
        // some drivers. Clamp defensively so the image is always valid.
        if ( m_Resource.MipLevels > maxMipLevels )
            m_Resource.MipLevels = maxMipLevels;
        if ( m_Resource.MipLevels < 1 )
            m_Resource.MipLevels = 1;

        VkImageLayout finalDefaultLayout = Utils::GetDefaultLayout( m_Specification.Format, m_Specification.Properties );

        VkImageCreateInfo info = {
             .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
             .imageType     = VK_IMAGE_TYPE_2D,
             .format        = m_Resource.Format,
             .extent        = { m_Specification.Width, m_Specification.Height, 1 },
             .mipLevels     = m_Resource.MipLevels,
             .arrayLayers   = 1,
             // 2/4/8 map 1:1 onto the VK_SAMPLE_COUNT_*_BIT values.
             .samples       = static_cast<VkSampleCountFlagBits>(
                  m_Specification.Samples > 1 ? m_Specification.Samples : 1 ),
             .tiling        = VK_IMAGE_TILING_OPTIMAL,
             .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

        // A multisampled image is written by the render pass and consumed by its RESOLVE — plain
        // transfer usage does not apply to it (and mip generation is illegal on MS images).
        if ( m_Specification.Samples > 1 )
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

        // A BLOCK FORMAT IS SAMPLED AND TRANSFERRED, AND NOTHING ELSE. `vkGetPhysicalDeviceFormatProperties`
        // reports `sampledImage` and `transferSrc/Dst` for BC7 and BC6H and reports neither
        // `storageImage` nor `colorAttachment` — a shader cannot write a compressed block, and neither
        // can a render pass. Multisampling is out for the same reason a mip blit is (`blitDst = 0`): the
        // resolve would have to write one.
        //
        // NAMED HERE RATHER THAN LEFT TO THE DRIVER, because the driver's answer is the problem: on this
        // machine a BC image is created and used with no validation message even when the feature that
        // makes it legal is switched off (measured — see the TextureCompressionBC suite), so an illegal
        // usage bit is exactly the kind of mistake that works here and fails on the target.
        if ( Core::Formats::IsBlockCompressed( m_Specification.Format ) &&
             ( m_Specification.Usage == Core::Formats::Image2DUsage::Attachment ||
               ( m_Specification.Properties & Core::Formats::Storage ) != 0 || m_Specification.Samples > 1 ) )
        {
            return Common::MakeFormattedError<bool>(
                 "Image2D '{}' asks for format {} as an attachment, a storage image or a {}x multisampled "
                 "image. A block-compressed format can only be sampled and transferred.",
                 m_Specification.Tag, static_cast<uint32_t>( m_Specification.Format ), m_Specification.Samples );
        }

        if ( m_Specification.Usage == Core::Formats::Image2DUsage::Attachment )
            info.usage |= Graphic::Utils::IsDepthFormat( m_Specification.Format ) ?
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        
        if ( m_Specification.Properties & Core::Formats::Storage ) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

        auto allocResult = allocator->RT_AllocateImage( m_Specification.Tag, info, VMA_MEMORY_USAGE_GPU_ONLY, m_Resource.Image );
        if ( !allocResult.IsSuccess() ) return Common::MakeError<bool>( allocResult.GetError() );
        m_Resource.Allocation = allocResult.GetValue();

        // ITS COST ON THE DEVICE, RECORDED WHERE THE ALLOCATION HAPPENS.
        //
        // THIS USED TO LIVE IN `Image2D::Create`, AND TWO FILES WENT AROUND IT. `VulkanFramebuffer.cpp`
        // builds its colour/depth attachments and its MSAA resolves with `std::make_shared<VulkanImage2D>`
        // directly (2 sites), and `VulkanFallbackTextures.cpp` does the same for 2D, cube and volume
        // fallbacks (5 sites) — seven images in all took a ledger row from the `Image` base constructor
        // and then never reported a size. The images that
        // skipped it are the BIGGEST ones the engine owns: the log's own line says "4 cascade(s) at
        // 2048x2048 ... = 320.0 MiB of attachments", and every one of those bytes was invisible to the
        // only memory figure the engine prints. That is why the ledger total was a floor over 36 of 602
        // rows on the world scene, and why `Image.hpp` claimed that every attachment goes through
        // `Image2D::Create` — a comment, not the code.
        //
        // Here it cannot be skipped: an image that did not run this line did not allocate. And the figure
        // is the allocator's, not a width x height x bytes-per-pixel product computed elsewhere — which
        // additionally is the number that stops existing the day a format is block-compressed.
        RecordDeviceBytes( VulkanAllocator::AllocationSize( m_Resource.Allocation ) );

        // The Tag already named the VMA allocation, which only shows up in a VMA dump. Name the VkImage
        // itself as well so a graphics debugger lists "GBuffer_attachment0" instead of "Image 1234" —
        // without this, finding a specific target in a capture means guessing by size and format.
        if ( !m_Specification.Tag.empty() )
            VKUtils::SetDebugUtilsObjectName( vkDevice, VK_OBJECT_TYPE_IMAGE, m_Specification.Tag,
                                              m_Resource.Image );

        VkImageAspectFlags aspect = Graphic::Utils::IsDepthFormat( m_Specification.Format ) ?
            VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        
        m_Resource.ImageView = Utils::CreateView( vkDevice, m_Resource.Image, m_Resource.Format, aspect, VK_IMAGE_VIEW_TYPE_2D, 1, m_Resource.MipLevels );

        if ( m_Specification.Properties & Core::Formats::Sample )
            Utils::CreateSampler( vkDevice, m_Resource.Sampler, Utils::SamplerFilterPolicy::Global );

        for ( uint32_t i = 0; i < m_Resource.MipLevels; ++i )
            m_MipViews.push_back( Utils::CreateView( vkDevice, m_Resource.Image, m_Resource.Format, aspect, VK_IMAGE_VIEW_TYPE_2D, 1, 1, i ) );

        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc.IsSuccess() )
            return Common::MakeFormattedError<bool>( "VulkanImage2D::CreateResource: no command buffer: {}",
                                                     cmdAlloc.GetError() );
        const VkCommandBuffer cmd = cmdAlloc.GetValue();

        if ( Core::Formats::HasData( m_Specification.Data ) )
        {
            // THE WHOLE CHAIN IS ONE STAGING BUFFER AND ONE `memcpy`, then one copy region per level.
            // The alternative — a buffer per level — is what makes a ten-level 2048x2048 texture ten
            // allocations and ten map/unmap pairs on the frame that first touches it.
            uint64_t size =
                 suppliedChain ? Core::Formats::GetPixelDataSize( m_Specification.Data )
                               : Core::Formats::CalculateImageSize( m_Specification.Width, m_Specification.Height,
                                                                    m_Specification.Format );
            VkBuffer staging; VmaAllocation stagingAlloc;
            VkBufferCreateInfo bInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
            const auto stagingResult =
                 allocator->RT_AllocateBuffer( "Staging", bInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, staging );
            if ( !stagingResult.IsSuccess() )
            {
                CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
                return Common::MakeFormattedError<bool>( "Image2D '{}': {} byte staging buffer failed: {}",
                                                         m_Specification.Tag, size, stagingResult.GetError() );
            }
            stagingAlloc = stagingResult.GetValue();

            {
                MappedMemory staged = allocator->MapMemory( stagingAlloc );
                const auto   wrote =
                     staged.Write( Utils::GetPixelDataPtr( m_Specification.Data ), static_cast<size_t>( size ) );
                if ( !wrote.IsSuccess() )
                {
                    allocator->RT_DestroyBuffer( staging, stagingAlloc );
                    CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
                    return Common::MakeFormattedError<bool>( "Image2D '{}': {}", m_Specification.Tag,
                                                             wrote.GetError() );
                }
            }

            // Transition UNDEFINED -> TRANSFER_DST_OPTIMAL -> finalDefaultLayout
            TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

            if ( suppliedChain )
            {
                // One region per level, all out of the one buffer. `bufferOffset` is legal without
                // padding because every level's byte size is a whole multiple of the texel size, so a
                // tightly packed chain lands every level on a texel-size boundary — the container
                // writes it that way for exactly this reason (TextureBinary.hpp).
                std::vector<VkBufferImageCopy> regions;
                regions.reserve( m_Resource.MipLevels );
                uint32_t levelW = m_Specification.Width, levelH = m_Specification.Height;
                for ( uint32_t level = 0; level < m_Resource.MipLevels; ++level )
                {
                    regions.push_back(
                         VkBufferImageCopy{ .bufferOffset     = m_Specification.MipLevels[level].ByteOffset,
                                            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                  .mipLevel   = level,
                                                                  .layerCount = 1 },
                                            .imageExtent      = { levelW, levelH, 1 } } );
                    levelW = levelW > 1 ? levelW / 2 : 1;
                    levelH = levelH > 1 ? levelH / 2 : 1;
                }
                vkCmdCopyBufferToImage( cmd, staging, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        static_cast<uint32_t>( regions.size() ), regions.data() );
            }
            else
            {
                VkBufferImageCopy copy = {
                     .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                     .imageExtent      = { m_Specification.Width, m_Specification.Height, 1 } };
                vkCmdCopyBufferToImage( cmd, staging, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                        &copy );
            }

            TransitionLayout( cmd, finalDefaultLayout );

            allocator->RT_DestroyBuffer( staging, stagingAlloc );
        }
        else
        {
            // DESERT_POISON_NEW_MEMORY (VulkanAllocator.hpp): stand in for a driver that does not zero new
            // memory, so a read before the first write shows up as NaN here instead of only on Windows.
            // Multisampled images carry no transfer usage and are only ever read through their resolve.
            // An image that declared a zero initial content (Image2DSpecification::InitialContent) is cleared
            // to zero instead, poison or not: it is read in frames that do not write it, by contract.
            if ( zeroOnCreate )
            {
                TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
                const VkImageSubresourceRange range{ aspect, 0, m_Resource.MipLevels, 0, 1 };
                if ( aspect == VK_IMAGE_ASPECT_DEPTH_BIT )
                {
                    const VkClearDepthStencilValue zero{ 0.0f, 0 };
                    vkCmdClearDepthStencilImage( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 &zero, 1, &range );
                }
                else
                {
                    const VkClearColorValue zero{};
                    vkCmdClearColorImage( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                                          &range );
                }
            }
            else if ( VulkanAllocator::PoisonNewMemory() && ( info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT ) != 0 )
            {
                TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
                const VkImageSubresourceRange range{ aspect, 0, m_Resource.MipLevels, 0, 1 };
                if ( aspect == VK_IMAGE_ASPECT_DEPTH_BIT )
                {
                    // Depth must stay in [0, 1]; mid-range is the value least likely to look cleared.
                    const VkClearDepthStencilValue depth{ 0.5f, 0 };
                    vkCmdClearDepthStencilImage( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 &depth, 1, &range );
                }
                else
                {
                    VkClearColorValue garbage{};
                    if ( IsIntegerColourFormat( m_Resource.Format ) )
                        std::fill( std::begin( garbage.uint32 ), std::end( garbage.uint32 ), 0xCDCDCDCDu );
                    else
                        std::fill( std::begin( garbage.float32 ), std::end( garbage.float32 ),
                                   std::numeric_limits<float>::quiet_NaN() );
                    vkCmdClearColorImage( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &garbage, 1,
                                          &range );
                }
            }
            TransitionLayout( cmd, finalDefaultLayout );
        }

        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );

        m_IsLoaded = true;
        return Common::MakeSuccess( true );
    }

    namespace
    {
        // Single-mip layout barrier with explicit stage/access masks (the blit chain needs precise
        // per-mip synchronization, which the whole-image TransitionLayout can't express).
        void MipBarrier( VkCommandBuffer cmd, VkImage image, uint32_t mip, VkImageLayout oldLayout,
                         VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage )
        {
            VkImageMemoryBarrier b = {
                 .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                 .srcAccessMask       = srcAccess,
                 .dstAccessMask       = dstAccess,
                 .oldLayout           = oldLayout,
                 .newLayout           = newLayout,
                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                 .image               = image,
                 .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 } };
            vkCmdPipelineBarrier( cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b );
        }
    } // namespace

    void VulkanImage2D::GenerateMips( VkCommandBuffer cmd, VkImageLayout finalLayout )
    {
        // The whole image arrives in TRANSFER_DST_OPTIMAL with mip 0 populated. Walk down the chain:
        // transition the source mip to TRANSFER_SRC, blit (linear) into the next mip, then park the
        // source mip in finalLayout. The last mip is transitioned at the end.
        int32_t mipW = static_cast<int32_t>( m_Specification.Width );
        int32_t mipH = static_cast<int32_t>( m_Specification.Height );

        for ( uint32_t i = 1; i < m_Resource.MipLevels; ++i )
        {
            MipBarrier( cmd, m_Resource.Image, i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

            const int32_t nextW = mipW > 1 ? mipW / 2 : 1;
            const int32_t nextH = mipH > 1 ? mipH / 2 : 1;

            VkImageBlit blit = {
                 .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 },
                 .srcOffsets     = { { 0, 0, 0 }, { mipW, mipH, 1 } },
                 .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 },
                 .dstOffsets     = { { 0, 0, 0 }, { nextW, nextH, 1 } } };
            vkCmdBlitImage( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Resource.Image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR );

            // Source mip done -> park it in the final sampled layout.
            MipBarrier( cmd, m_Resource.Image, i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, finalLayout,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );

            mipW = nextW;
            mipH = nextH;
        }

        // The last mip never served as a blit source; move it from TRANSFER_DST to the final layout.
        MipBarrier( cmd, m_Resource.Image, m_Resource.MipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    finalLayout, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );

        m_Resource.Layout = finalLayout;
    }

    void VulkanImage2D::UploadData( VkCommandBuffer cmd, VkBuffer staging )
    {
        // This is only used for runtime updates, assuming current layout is already SHADER_READ_ONLY_OPTIMAL or similar
        VkImageLayout originalLayout = m_Resource.Layout;
        TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
        
        VkBufferImageCopy copy = { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                                   .imageExtent = { m_Specification.Width, m_Specification.Height, 1 } };
        vkCmdCopyBufferToImage( cmd, staging, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );
        
        TransitionLayout( cmd, originalLayout );
    }

    Common::ResultStr<std::vector<uint8_t>> VulkanImage2D::ReadPixelsRGBA8()
    {
        // EVERY ARM OF THIS FUNCTION USED TO RETURN `{}` AND MEAN SOMETHING DIFFERENT BY IT. An
        // uninitialised image, a format the packer does not know, a staging allocation that failed, a
        // command buffer that could not be taken and a readback that refused all produced one empty
        // vector — which is also exactly what a legitimately empty picture would produce. The two
        // callers that cache thumbnails cannot tell "not ready, ask again" from "there is nothing here",
        // so they either cached a blank PNG for good or retried forever. Contract §1.4.
        const uint32_t w = m_Specification.Width;
        const uint32_t h = m_Specification.Height;
        if ( w == 0 || h == 0 || m_Resource.Image == VK_NULL_HANDLE )
            return Common::MakeFormattedError<std::vector<uint8_t>>(
                 "ReadPixelsRGBA8: the image is {}x{} and its VkImage is {}", w, h,
                 m_Resource.Image == VK_NULL_HANDLE ? "null" : "valid" );

        // The engine's format vocabulary, mapped to the pack's own. The MAPPING belongs here, where the
        // ImageFormat enum is; the PACK is shared with the swapchain readback next door (PixelPack.hpp),
        // because two copies of the channel swizzle is how one capture ends up with red and blue exchanged
        // and gets reported as a rendering defect.
        const auto                 fmt = m_Specification.Format;
        Graphic::PackedPixelSource source{};
        switch ( fmt )
        {
            case Core::Formats::ImageFormat::RGBA8F:
                source = Graphic::PackedPixelSource::RGBA8;
                break;
            case Core::Formats::ImageFormat::BGRA8F:
                source = Graphic::PackedPixelSource::BGRA8;
                break;
            case Core::Formats::ImageFormat::RGBA32F:
                source = Graphic::PackedPixelSource::RGBA32F;
                break;
            default:
                return Common::MakeFormattedError<std::vector<uint8_t>>(
                     "ReadPixelsRGBA8: image format {} has no RGBA8 packing",
                     static_cast<int>( fmt ) ); // only colour formats the pack knows
        }

        auto allocator =
             SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator().get();

        const uint64_t     srcSize = Core::Formats::CalculateImageSize( w, h, fmt ); // GPU bytes
        VkBuffer           staging = VK_NULL_HANDLE;
        VkBufferCreateInfo bInfo   = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size  = srcSize,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        auto allocRes = allocator->RT_AllocateBuffer( "ThumbReadback", bInfo, VMA_MEMORY_USAGE_GPU_TO_CPU, staging );
        if ( !allocRes.IsSuccess() )
            return Common::MakeFormattedError<std::vector<uint8_t>>(
                 "ReadPixelsRGBA8: {} byte readback buffer failed: {}", srcSize, allocRes.GetError() );
        VmaAllocation stagingAlloc = allocRes.GetValue();

        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc.IsSuccess() )
        {
            allocator->RT_DestroyBuffer( staging, stagingAlloc );
            return Common::MakeFormattedError<std::vector<uint8_t>>( "ReadPixelsRGBA8: no command buffer: {}",
                                                                     cmdAlloc.GetError() );
        }
        const VkCommandBuffer cmd      = cmdAlloc.GetValue();
        const VkImageLayout   original = m_Resource.Layout;
        TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
        VkBufferImageCopy copy = { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                                   .imageExtent      = { w, h, 1 } };
        vkCmdCopyImageToBuffer( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy );
        TransitionLayout( cmd, original == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                     : original );
        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );

        std::vector<uint8_t> raw( static_cast<size_t>( srcSize ) );
        {
            // The READBACK direction, and the one this class of defect is nastiest in: unchecked, the
            // null was the memcpy's SOURCE, so the crash landed inside libc with no frame of ours on it.
            MappedMemory readback = allocator->MapMemory( stagingAlloc );
            const auto   read     = readback.ReadInto( raw.data(), static_cast<size_t>( srcSize ) );
            if ( !read.IsSuccess() )
            {
                const std::string reason = read.GetError();
                readback.Unmap();
                allocator->RT_DestroyBuffer( staging, stagingAlloc );
                return Common::MakeFormattedError<std::vector<uint8_t>>( "ReadPixelsRGBA8: {}", reason );
            }
        }
        allocator->RT_DestroyBuffer( staging, stagingAlloc );

        return Common::MakeSuccess(
             Graphic::PackToRGBA8( raw.data(), raw.size(), static_cast<size_t>( w ) * h, source ) );
    }

    void VulkanImage2D::TransitionLayout( VkCommandBuffer cmd, VkImageLayout newLayout, uint32_t mip )
    {
        Graphic::API::Vulkan::Utils::InsertImageMemoryBarrier( cmd, m_Resource.Image, m_Resource.Format,
                                                               m_Resource.Layout, newLayout, 1, mip == 0 ? m_Resource.MipLevels : 1 );
        m_Resource.Layout = newLayout;
    }

    void VulkanImage2D::TransitionLayout( VkCommandBuffer cmd, VkImageLayout newLayout,
                                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                          VkAccessFlags srcAccess, VkAccessFlags dstAccess )
    {
        // The aspect comes from the FORMAT. This used to be a hardcoded COLOR bit, which is why the scene
        // depth attachment could not be moved to SHADER_READ_ONLY for a compute read: naming COLOR on a
        // D24S8 image is a VUID-VkImageMemoryBarrier-image-03319 violation, and naming DEPTH alone on a
        // packed depth+stencil image is another.
        VkImageSubresourceRange range{ .aspectMask     = GetImageVulkanAspect( m_Specification.Format ),
                                       .baseMipLevel   = 0,
                                       .levelCount     = m_Resource.MipLevels,
                                       .baseArrayLayer = 0,
                                       .layerCount     = 1 };
        Graphic::API::Vulkan::Utils::InsertImageMemoryBarrier( cmd, m_Resource.Image, srcAccess, dstAccess,
                                                               m_Resource.Layout, newLayout, srcStage,
                                                               dstStage, range );
        m_Resource.Layout = newLayout;
    }

    VkImageLayout VulkanImage2D::GetDefaultLayout() const
    {
        return Utils::GetDefaultLayout( m_Specification.Format, m_Specification.Properties );
    }

    VkImageView VulkanImage2D::GetMipView( uint32_t level ) const
    {
        return m_MipViews[level];
    }

    // --- VulkanImageCube ---

    VulkanImageCube::VulkanImageCube( const Core::Formats::ImageCubeSpecification& spec ) : m_Specification( spec ) {}
    VulkanImageCube::~VulkanImageCube()
    {
        Release();
    }
    Common::BoolResultStr VulkanImageCube::Invalidate() { return RT_Invalidate(); }
    Common::BoolResultStr VulkanImageCube::RT_Invalidate() { Release(); return CreateResource(); }

    Common::BoolResultStr VulkanImageCube::Release()
    {
        if ( !m_Resource.Image ) return BOOLSUCCESS;
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator().get();
        allocator->RT_DestroyImage( m_Resource.Image, m_Resource.Allocation, m_Resource.ImageView, m_Resource.Sampler, m_MipViews );
        m_Resource = {};
        m_MipViews.clear();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImageCube::CreateResource()
    {
        auto vkDevice = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )->GetVulkanAllocator().get();

        m_Resource.Format     = GetImageVulkanFormat( m_Specification.Format );
        m_Resource.MipLevels  = m_Specification.Mips;
        m_Resource.LayerCount = 6;
        m_Resource.Layout     = VK_IMAGE_LAYOUT_UNDEFINED;

        const uint32_t faceSize = m_Specification.FaceSize;

        // The specification names the FACE, so there is no division here for a caller to forget — but a
        // mip count past the face's chain is still an invalid vkCreateImage (VUID-...-00958) that surfaces
        // as a GPU fault / VK_ERROR_DEVICE_LOST far from the call site. Refuse it loudly with the numbers
        // instead: silently clamping would desync a caller that then walks mip views it asked for.
        const uint32_t maxMips = Core::Formats::MipChainLength( faceSize );
        if ( m_Specification.Mips > maxMips )
            return Common::MakeFormattedError<bool>(
                 "ImageCube '{}': {} mips requested but a {}-texel face supports at most {} "
                 "(VUID-VkImageCreateInfo-mipLevels-00958)",
                 m_Specification.Tag, m_Specification.Mips, faceSize, maxMips );

        VkImageLayout finalDefaultLayout = Utils::GetDefaultLayout( m_Specification.Format, m_Specification.Properties );

        VkImageCreateInfo info = {
             .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
             .flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
             .imageType     = VK_IMAGE_TYPE_2D,
             .format        = m_Resource.Format,
             .extent        = { faceSize, faceSize, 1 },
             .mipLevels     = m_Resource.MipLevels,
             .arrayLayers   = 6,
             .samples       = VK_SAMPLE_COUNT_1_BIT,
             .tiling        = VK_IMAGE_TILING_OPTIMAL,
             .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

        // SAME RULE AS THE 2D PATH, AND THE CASE IT WAS WRITTEN FOR. A baked environment cube is created
        // with `Storage | Sample` because the three compute passes WRITE it; a cube read back off the
        // cache is BC6H and is only ever sampled, so `EnvironmentBake.cpp` drops the storage bit for it.
        // This is the line that says what happens if it ever stops doing so.
        if ( Core::Formats::IsBlockCompressed( m_Specification.Format ) &&
             ( m_Specification.Properties & Core::Formats::Storage ) != 0 )
        {
            return Common::MakeFormattedError<bool>(
                 "ImageCube '{}' asks for format {} as a storage image. A block-compressed format can only "
                 "be sampled and transferred: a shader cannot write a compressed block.",
                 m_Specification.Tag, static_cast<uint32_t>( m_Specification.Format ) );
        }

        if ( m_Specification.Properties & Core::Formats::Storage ) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

        auto allocResult = allocator->RT_AllocateImage( m_Specification.Tag, info, VMA_MEMORY_USAGE_GPU_ONLY, m_Resource.Image );
        if ( !allocResult.IsSuccess() ) return Common::MakeError<bool>( allocResult.GetError() );
        m_Resource.Allocation = allocResult.GetValue();

        // ITS COST ON THE DEVICE, RECORDED WHERE THE ALLOCATION HAPPENS.
        //
        // THIS USED TO LIVE IN `Image2D::Create`, AND TWO FILES WENT AROUND IT. `VulkanFramebuffer.cpp`
        // builds its colour/depth attachments and its MSAA resolves with `std::make_shared<VulkanImage2D>`
        // directly (2 sites), and `VulkanFallbackTextures.cpp` does the same for 2D, cube and volume
        // fallbacks (5 sites) — seven images in all took a ledger row from the `Image` base constructor
        // and then never reported a size. The images that
        // skipped it are the BIGGEST ones the engine owns: the log's own line says "4 cascade(s) at
        // 2048x2048 ... = 320.0 MiB of attachments", and every one of those bytes was invisible to the
        // only memory figure the engine prints. That is why the ledger total was a floor over 36 of 602
        // rows on the world scene, and why `Image.hpp` claimed that every attachment goes through
        // `Image2D::Create` — a comment, not the code.
        //
        // Here it cannot be skipped: an image that did not run this line did not allocate. And the figure
        // is the allocator's, not a width x height x bytes-per-pixel product computed elsewhere — which
        // additionally is the number that stops existing the day a format is block-compressed.
        RecordDeviceBytes( VulkanAllocator::AllocationSize( m_Resource.Allocation ) );

        m_Resource.ImageView = Utils::CreateView( vkDevice, m_Resource.Image, m_Resource.Format, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, 6, m_Resource.MipLevels );
        Utils::CreateSampler( vkDevice, m_Resource.Sampler, Utils::SamplerFilterPolicy::Global );

        for ( uint32_t i = 0; i < m_Resource.MipLevels; ++i )
            m_MipViews.push_back( Utils::CreateView( vkDevice, m_Resource.Image, m_Resource.Format, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, 6, 1, i ) );

        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc.IsSuccess() )
            return Common::MakeFormattedError<bool>( "VulkanImageCube::CreateResource: no command buffer: {}",
                                                     cmdAlloc.GetError() );
        const VkCommandBuffer cmd = cmdAlloc.GetValue();

        // ── THE PIXELS, IF THE CALLER BROUGHT ANY ────────────────────────────────────────────────
        //
        // A CUBE USED TO DROP THEM HERE, SILENTLY. `UploadData` was an empty body and nothing called it,
        // so `ImageCubeSpecification::Data` was a field the backend read and discarded — both ends of the
        // chain looked right and the link between them lost everything. The census that exists because of
        // it is the `Levels` table: a cube's level is six images, and the ONLY legal way to name them is
        // a span each.
        const bool hasPixels = Core::Formats::HasData( m_Specification.Data ) && !m_Specification.Levels.empty();
        if ( Core::Formats::HasData( m_Specification.Data ) && m_Specification.Levels.empty() )
        {
            // NAMED, NOT IGNORED. This is the exact shape that used to be legal and silent.
            CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
            return Common::MakeFormattedError<bool>(
                 "ImageCube '{}' was given {} bytes of pixel data and no level table. A cube's level is six "
                 "images and there is no order to assume: fill ImageCubeSpecification::Levels, indexed "
                 "level * 6 + face.",
                 m_Specification.Tag, Core::Formats::GetPixelDataSize( m_Specification.Data ) );
        }

        VkBuffer      staging      = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = nullptr;
        if ( hasPixels )
        {
            const size_t expected =
                 static_cast<size_t>( m_Resource.MipLevels ) * Core::Formats::kImageCubeLayerCount;
            if ( m_Specification.Levels.size() != expected )
            {
                CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
                return Common::MakeFormattedError<bool>(
                     "ImageCube '{}': {} mips x {} faces is {} spans and the table has {}.", m_Specification.Tag,
                     m_Resource.MipLevels, Core::Formats::kImageCubeLayerCount, expected,
                     m_Specification.Levels.size() );
            }

            const uint64_t     size  = Core::Formats::GetPixelDataSize( m_Specification.Data );
            VkBufferCreateInfo bInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size  = size,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
            const auto         stagingResult =
                 allocator->RT_AllocateBuffer( "CubeStaging", bInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, staging );
            if ( !stagingResult.IsSuccess() )
            {
                CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
                return Common::MakeFormattedError<bool>( "ImageCube '{}': {} byte staging buffer failed: {}",
                                                         m_Specification.Tag, size, stagingResult.GetError() );
            }
            stagingAlloc = stagingResult.GetValue();
            {
                MappedMemory staged = allocator->MapMemory( stagingAlloc );
                const auto   wrote =
                     staged.Write( Utils::GetPixelDataPtr( m_Specification.Data ), static_cast<size_t>( size ) );
                if ( !wrote.IsSuccess() )
                {
                    allocator->RT_DestroyBuffer( staging, stagingAlloc );
                    CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
                    return Common::MakeFormattedError<bool>( "ImageCube '{}': {}", m_Specification.Tag,
                                                             wrote.GetError() );
                }
            }

            TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
            UploadData( cmd, staging );
        }

        TransitionLayout( cmd, finalDefaultLayout );
        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
        if ( staging != VK_NULL_HANDLE )
            allocator->RT_DestroyBuffer( staging, stagingAlloc );

        return Common::MakeSuccess( true );
    }

    void VulkanImageCube::UploadData( VkCommandBuffer cmd, VkBuffer stagingBuffer )
    {
        // ONE REGION PER (LEVEL, FACE), ALL OUT OF ONE BUFFER — the same shape `VulkanImage2D` uses for a
        // supplied chain, with the array layer as the second axis. `baseArrayLayer` indexes the face in
        // the order `VkImageCreateInfo` defines it (+X -X +Y -Y +Z -Z), which is the order the container
        // stores and therefore the order nobody permutes on the way through.
        std::vector<VkBufferImageCopy> regions;
        regions.reserve( m_Specification.Levels.size() );

        uint32_t side = m_Specification.FaceSize;
        for ( uint32_t level = 0; level < m_Resource.MipLevels; ++level )
        {
            for ( uint32_t face = 0; face < Core::Formats::kImageCubeLayerCount; ++face )
            {
                const size_t row = static_cast<size_t>( level ) * Core::Formats::kImageCubeLayerCount + face;
                regions.push_back(
                     VkBufferImageCopy{ .bufferOffset     = m_Specification.Levels[row].ByteOffset,
                                        .imageSubresource = { .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                                              .mipLevel       = level,
                                                              .baseArrayLayer = face,
                                                              .layerCount     = 1 },
                                        .imageExtent      = { side, side, 1 } } );
            }
            side = side > 1u ? side / 2u : 1u;
        }

        vkCmdCopyBufferToImage( cmd, stagingBuffer, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                static_cast<uint32_t>( regions.size() ), regions.data() );
    }

    Common::ResultStr<std::vector<unsigned char>> VulkanImageCube::RT_ReadAllLevels()
    {
        // THE OTHER DIRECTION, AND THE ONE THE BAKE NEEDS. `Image2D::ReadPixelsRGBA8` is deliberately the
        // only readback in the engine and it CONVERTS to RGBA8 for a thumbnail; that is the wrong answer
        // for an environment cube, whose whole content is radiance outside [0,1]. This one converts
        // nothing: it hands back the image's own bytes, in the image's own format, laid out TIGHTLY and
        // in TABLE ORDER — level 0's six faces, then level 1's — which is exactly what
        // `Serialization::BuildLevelTable` takes.
        if ( m_Resource.Image == VK_NULL_HANDLE )
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "ImageCube '{}': RT_ReadAllLevels before the image exists", m_Specification.Tag );

        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        std::vector<VkBufferImageCopy> regions;
        regions.reserve( static_cast<size_t>( m_Resource.MipLevels ) * Core::Formats::kImageCubeLayerCount );

        uint64_t at   = 0;
        uint32_t side = m_Specification.FaceSize;
        for ( uint32_t level = 0; level < m_Resource.MipLevels; ++level )
        {
            const uint64_t one = Core::Formats::CalculateImageSize( side, side, m_Specification.Format );
            for ( uint32_t face = 0; face < Core::Formats::kImageCubeLayerCount; ++face )
            {
                regions.push_back(
                     VkBufferImageCopy{ .bufferOffset     = at,
                                        .imageSubresource = { .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                                              .mipLevel       = level,
                                                              .baseArrayLayer = face,
                                                              .layerCount     = 1 },
                                        .imageExtent      = { side, side, 1 } } );
                at += one;
            }
            side = side > 1u ? side / 2u : 1u;
        }

        VkBuffer           staging = VK_NULL_HANDLE;
        VkBufferCreateInfo bInfo   = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size  = at,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        const auto         allocRes =
             allocator->RT_AllocateBuffer( "CubeReadback", bInfo, VMA_MEMORY_USAGE_GPU_TO_CPU, staging );
        if ( !allocRes.IsSuccess() )
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "ImageCube '{}': {} byte readback buffer failed: {}", m_Specification.Tag, at,
                 allocRes.GetError() );
        const VmaAllocation stagingAlloc = allocRes.GetValue();

        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc.IsSuccess() )
        {
            allocator->RT_DestroyBuffer( staging, stagingAlloc );
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "ImageCube '{}': RT_ReadAllLevels has no command buffer: {}", m_Specification.Tag,
                 cmdAlloc.GetError() );
        }
        const VkCommandBuffer cmd     = cmdAlloc.GetValue();
        const VkImageLayout   restore = m_Resource.Layout;
        TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
        vkCmdCopyImageToBuffer( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging,
                                static_cast<uint32_t>( regions.size() ), regions.data() );
        TransitionLayout( cmd, restore );
        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );

        std::vector<unsigned char> out( static_cast<size_t>( at ) );
        {
            MappedMemory mapped = allocator->MapMemory( stagingAlloc );
            const auto   read   = mapped.ReadInto( out.data(), out.size() );
            if ( !read.IsSuccess() )
            {
                const std::string reason = read.GetError();
                mapped.Unmap();
                allocator->RT_DestroyBuffer( staging, stagingAlloc );
                return Common::MakeFormattedError<std::vector<unsigned char>>( "ImageCube '{}': {}",
                                                                               m_Specification.Tag, reason );
            }
        }
        allocator->RT_DestroyBuffer( staging, stagingAlloc );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr VulkanImageCube::RT_ClearToColor( float r, float g, float b, float a )
    {
        if ( m_Resource.Image == VK_NULL_HANDLE )
            return Common::MakeFormattedError<bool>( "ImageCube '{}': RT_ClearToColor before the image exists",
                                                     m_Specification.Tag );

        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc.IsSuccess() )
            return Common::MakeFormattedError<bool>( "ImageCube '{}': RT_ClearToColor has no command buffer: {}",
                                                     m_Specification.Tag, cmdAlloc.GetError() );

        const VkCommandBuffer cmd = cmdAlloc.GetValue();

        // The layout the image is meant to live in, restored afterwards: a clear is a transfer and the
        // sampler wants SHADER_READ_ONLY back. Leaving it in TRANSFER_DST would be a descriptor pointing at
        // a layout no shader may read, which validation catches and a release build does not.
        const VkImageLayout restore = m_Resource.Layout;
        TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

        const VkClearColorValue       colour = { { r, g, b, a } };
        const VkImageSubresourceRange range  = { .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                                 .baseMipLevel   = 0,
                                                 .levelCount     = m_Resource.MipLevels,
                                                 .baseArrayLayer = 0,
                                                 .layerCount     = 6 };
        vkCmdClearColorImage( cmd, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range );

        TransitionLayout( cmd, restore );
        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );

        return Common::MakeSuccess( true );
    }

    void VulkanImageCube::TransitionLayout( VkCommandBuffer cmd, VkImageLayout newLayout, uint32_t mip )
    {
        Graphic::API::Vulkan::Utils::InsertImageMemoryBarrier( cmd, m_Resource.Image, m_Resource.Format,
                                                               m_Resource.Layout, newLayout, 6,
                                                               mip == 0 ? m_Resource.MipLevels : 1 );
        m_Resource.Layout = newLayout;
    }

    void VulkanImageCube::TransitionLayout( VkCommandBuffer cmd, VkImageLayout newLayout,
                                            VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                            VkAccessFlags srcAccess, VkAccessFlags dstAccess )
    {
        VkImageSubresourceRange range{ .aspectMask     = GetImageVulkanAspect( m_Specification.Format ),
                                       .baseMipLevel   = 0,
                                       .levelCount     = m_Resource.MipLevels,
                                       .baseArrayLayer = 0,
                                       .layerCount     = 6 };
        Graphic::API::Vulkan::Utils::InsertImageMemoryBarrier( cmd, m_Resource.Image, srcAccess, dstAccess,
                                                               m_Resource.Layout, newLayout, srcStage, dstStage,
                                                               range );
        m_Resource.Layout = newLayout;
    }

    VkImageLayout VulkanImageCube::GetDefaultLayout() const
    {
        return Utils::GetDefaultLayout( m_Specification.Format, m_Specification.Properties );
    }

    VkImageView VulkanImageCube::GetMipView( uint32_t level ) const
    {
        return m_MipViews[level];
    }

    // --- VulkanImage3D ---

    VulkanImage3D::VulkanImage3D( const Core::Formats::Image3DSpecification& spec ) : m_Specification( spec )
    {
    }
    VulkanImage3D::~VulkanImage3D()
    {
        Release();
    }
    Common::BoolResultStr VulkanImage3D::Invalidate()
    {
        return RT_Invalidate();
    }
    Common::BoolResultStr VulkanImage3D::RT_Invalidate()
    {
        Release();
        return CreateResource();
    }

    Common::BoolResultStr VulkanImage3D::Release()
    {
        if ( !m_Resource.Image )
            return BOOLSUCCESS;
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();
        // Deferred by frame index inside the allocator, so a volume released while a frame that still
        // references it is in flight is destroyed only once the GPU is done with it.
        allocator->RT_DestroyImage( m_Resource.Image, m_Resource.Allocation, m_Resource.ImageView,
                                    m_Resource.Sampler, m_MipViews );
        m_Resource = {};
        m_MipViews.clear();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanImage3D::CreateResource()
    {
        if ( m_Specification.Width == 0 || m_Specification.Height == 0 || m_Specification.Depth == 0 )
            return Common::MakeFormattedError<bool>( "Image3D '{}': zero extent {}x{}x{}", m_Specification.Tag,
                                                     m_Specification.Width, m_Specification.Height,
                                                     m_Specification.Depth );

        // A volume is a sampled/storage texture, never an attachment: Vulkan cannot render into a 3D image
        // without a layered framebuffer, which the engine does not create. Refuse loudly rather than
        // produce an image whose usage flags silently do not match how it is bound.
        if ( !( m_Specification.Properties & ( Core::Formats::Storage | Core::Formats::Sample ) ) )
            return Common::MakeFormattedError<bool>(
                 "Image3D '{}': Properties must request Storage and/or Sample (got {})", m_Specification.Tag,
                 static_cast<uint32_t>( m_Specification.Properties ) );

        auto vkDevice =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        m_Resource.Format     = GetImageVulkanFormat( m_Specification.Format );
        m_Resource.MipLevels  = 1;
        m_Resource.LayerCount = 1;
        m_Resource.Layout     = VK_IMAGE_LAYOUT_UNDEFINED;

        const VkImageLayout finalDefaultLayout =
             Utils::GetDefaultLayout( m_Specification.Format, m_Specification.Properties );

        VkImageCreateInfo info = {
             .sType     = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
             .imageType = VK_IMAGE_TYPE_3D,
             .format    = m_Resource.Format,
             .extent    = { m_Specification.Width, m_Specification.Height, m_Specification.Depth },
             .mipLevels = 1,
             // A 3D image has exactly one layer: its slices are the depth extent, not array elements.
             .arrayLayers = 1,
             .samples     = VK_SAMPLE_COUNT_1_BIT,
             .tiling      = VK_IMAGE_TILING_OPTIMAL,
             .usage =
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

        if ( m_Specification.Properties & Core::Formats::Storage )
            info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

        auto allocResult =
             allocator->RT_AllocateImage( m_Specification.Tag, info, VMA_MEMORY_USAGE_GPU_ONLY, m_Resource.Image );
        if ( !allocResult.IsSuccess() )
            return Common::MakeError<bool>( allocResult.GetError() );
        m_Resource.Allocation = allocResult.GetValue();

        // ITS COST ON THE DEVICE, RECORDED WHERE THE ALLOCATION HAPPENS.
        //
        // THIS USED TO LIVE IN `Image2D::Create`, AND TWO FILES WENT AROUND IT. `VulkanFramebuffer.cpp`
        // builds its colour/depth attachments and its MSAA resolves with `std::make_shared<VulkanImage2D>`
        // directly (2 sites), and `VulkanFallbackTextures.cpp` does the same for 2D, cube and volume
        // fallbacks (5 sites) — seven images in all took a ledger row from the `Image` base constructor
        // and then never reported a size. The images that
        // skipped it are the BIGGEST ones the engine owns: the log's own line says "4 cascade(s) at
        // 2048x2048 ... = 320.0 MiB of attachments", and every one of those bytes was invisible to the
        // only memory figure the engine prints. That is why the ledger total was a floor over 36 of 602
        // rows on the world scene, and why `Image.hpp` claimed that every attachment goes through
        // `Image2D::Create` — a comment, not the code.
        //
        // Here it cannot be skipped: an image that did not run this line did not allocate. And the figure
        // is the allocator's, not a width x height x bytes-per-pixel product computed elsewhere — which
        // additionally is the number that stops existing the day a format is block-compressed.
        RecordDeviceBytes( VulkanAllocator::AllocationSize( m_Resource.Allocation ) );

        // Name the VkImage as well as the VMA allocation, so a capture lists "SkyTransmittanceLut" rather
        // than a nameless 128x128x128 volume.
        if ( !m_Specification.Tag.empty() )
            VKUtils::SetDebugUtilsObjectName( vkDevice, VK_OBJECT_TYPE_IMAGE, m_Specification.Tag,
                                              m_Resource.Image );

        const VkImageAspectFlags aspect = GetImageVulkanAspect( m_Specification.Format );

        m_Resource.ImageView = Utils::CreateView( vkDevice, m_Resource.Image, m_Resource.Format, aspect,
                                                  VK_IMAGE_VIEW_TYPE_3D, 1, 1 );
        // ComputePipeline::SetOutput binds GetMipView( mip ); a volume has one level, so the mip view IS
        // the whole-image view. Kept in the vector so Release() hands it to the same deletion queue.
        m_MipViews.push_back( Utils::CreateView( vkDevice, m_Resource.Image, m_Resource.Format, aspect,
                                                 VK_IMAGE_VIEW_TYPE_3D, 1, 1 ) );

        if ( m_Specification.Properties & Core::Formats::Sample )
            Utils::CreateSampler( vkDevice, m_Resource.Sampler, Utils::SamplerFilterPolicy::AlwaysLinear );

        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc.IsSuccess() )
            return Common::MakeFormattedError<bool>( "VulkanImage3D::CreateResource: no command buffer: {}",
                                                     cmdAlloc.GetError() );
        const VkCommandBuffer cmd = cmdAlloc.GetValue();

        if ( Core::Formats::HasData( m_Specification.Data ) )
        {
            const uint64_t size = Core::Formats::CalculateImageSize(
                 m_Specification.Width, m_Specification.Height, m_Specification.Depth, m_Specification.Format );

            VkBuffer           staging;
            VmaAllocation      stagingAlloc;
            VkBufferCreateInfo bInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size  = size,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
            auto               stagingResult =
                 allocator->RT_AllocateBuffer( "VolumeStaging", bInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, staging );
            if ( !stagingResult.IsSuccess() )
            {
                CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
                return Common::MakeFormattedError<bool>( "Image3D '{}': {} byte staging buffer failed: {}",
                                                         m_Specification.Tag, size, stagingResult.GetError() );
            }
            stagingAlloc = stagingResult.GetValue();

            {
                MappedMemory staged = allocator->MapMemory( stagingAlloc );
                const auto   wrote =
                     staged.Write( Utils::GetPixelDataPtr( m_Specification.Data ), static_cast<size_t>( size ) );
                if ( !wrote.IsSuccess() )
                {
                    allocator->RT_DestroyBuffer( staging, stagingAlloc );
                    CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );
                    return Common::MakeFormattedError<bool>( "Image3D '{}': {}", m_Specification.Tag,
                                                             wrote.GetError() );
                }
            }

            TransitionLayout( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

            // One copy for the whole volume: bufferRowLength/bufferImageHeight stay 0, meaning the source
            // is tightly packed at the image's own extent — which is what CalculateImageSize sized.
            VkBufferImageCopy copy = {
                 .imageSubresource = { .aspectMask = aspect, .layerCount = 1 },
                 .imageExtent      = { m_Specification.Width, m_Specification.Height, m_Specification.Depth } };
            vkCmdCopyBufferToImage( cmd, staging, m_Resource.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                    &copy );

            TransitionLayout( cmd, finalDefaultLayout );

            allocator->RT_DestroyBuffer( staging, stagingAlloc );
        }
        else
        {
            TransitionLayout( cmd, finalDefaultLayout );
        }

        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( cmd );

        return Common::MakeSuccess( true );
    }

    void VulkanImage3D::TransitionLayout( VkCommandBuffer cmd, VkImageLayout newLayout, uint32_t mip )
    {
        // `mip` selects a sub-range on the 2D and cube paths; a volume has exactly one level, so the only
        // meaningful value is 0. Refuse anything else instead of transitioning the wrong subresource and
        // then tracking a layout the image is not actually in.
        DESERT_VERIFY( mip == 0, "Image3D has exactly one mip level" );

        Graphic::API::Vulkan::Utils::InsertImageMemoryBarrier( cmd, m_Resource.Image, m_Resource.Format,
                                                               m_Resource.Layout, newLayout, 1, 1 );
        m_Resource.Layout = newLayout;
    }

    void VulkanImage3D::TransitionLayout( VkCommandBuffer cmd, VkImageLayout newLayout,
                                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                          VkAccessFlags srcAccess, VkAccessFlags dstAccess )
    {
        VkImageSubresourceRange range{ .aspectMask     = GetImageVulkanAspect( m_Specification.Format ),
                                       .baseMipLevel   = 0,
                                       .levelCount     = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount     = 1 };
        Graphic::API::Vulkan::Utils::InsertImageMemoryBarrier( cmd, m_Resource.Image, srcAccess, dstAccess,
                                                               m_Resource.Layout, newLayout, srcStage, dstStage,
                                                               range );
        m_Resource.Layout = newLayout;
    }

    VkImageLayout VulkanImage3D::GetDefaultLayout() const
    {
        return Utils::GetDefaultLayout( m_Specification.Format, m_Specification.Properties );
    }

    VkImageView VulkanImage3D::GetMipView( uint32_t level ) const
    {
        // Single-level by construction; anything else is a caller bug worth naming rather than an
        // out-of-range read of m_MipViews.
        DESERT_VERIFY( level == 0, "Image3D has exactly one mip level" );
        return m_MipViews[0];
    }

    // --- Live sampler recreation (texture-filter setting change) ---

    static void RecreateSamplerImpl( VulkanImageResource& res, Utils::SamplerFilterPolicy policy )
    {
        auto vkDevice =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        if ( res.Sampler )
            vkDestroySampler( vkDevice, res.Sampler, nullptr );
        res.Sampler = VK_NULL_HANDLE;
        Utils::CreateSampler( vkDevice, res.Sampler, policy );
    }

    void VulkanImage2D::RecreateSampler()
    {
        RecreateSamplerImpl( m_Resource, Utils::SamplerFilterPolicy::Global );
    }
    void VulkanImageCube::RecreateSampler()
    {
        RecreateSamplerImpl( m_Resource, Utils::SamplerFilterPolicy::Global );
    }
    // A volume keeps LINEAR through a filter change too — this is the call that would otherwise undo it,
    // since Renderer::RecreateImageSamplers walks EVERY registered image when the setting moves.
    void VulkanImage3D::RecreateSampler()
    {
        if ( m_Resource.Sampler )
            RecreateSamplerImpl( m_Resource, Utils::SamplerFilterPolicy::AlwaysLinear );
    }

} // namespace Desert::Graphic::API::Vulkan
