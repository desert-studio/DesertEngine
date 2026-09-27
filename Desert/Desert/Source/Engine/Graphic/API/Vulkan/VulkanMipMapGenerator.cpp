#include <Engine/Graphic/API/Vulkan/VulkanMipMapGenerator.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuBatch.hpp>

#include <vulkan/vulkan.h>

namespace Desert::Graphic::API::Vulkan
{
    // Blits each mip from the one above it, leaving every level in TRANSFER_SRC_OPTIMAL. When
    // @p transitionToShaderRead is true the whole image is then moved to SHADER_READ_ONLY_OPTIMAL with a
    // RAW barrier the wrapper's layout tracking does not see — callers whose tracked layout would go
    // stale (the cube path, whose images the compute dispatcher later re-transitions from the TRACKED
    // layout) pass false and do both boundary transitions through the image's own TransitionLayout.
    static void GenerateMipmapsTO( VkCommandBuffer commandBuffer, VkImage image, VkFormat /*imageFormat*/,
                                   uint32_t width, uint32_t height, uint32_t mipLevels,
                                   uint32_t baseArrayLayer = 0, uint32_t layerCount = 1,
                                   bool transitionToShaderRead = true )
    {
        for ( uint32_t layer = baseArrayLayer; layer < baseArrayLayer + layerCount; layer++ )
        {
            for ( uint32_t i = 1; i < mipLevels; i++ )
            {
                VkImageBlit imageBlit{};

                imageBlit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                imageBlit.srcSubresource.baseArrayLayer = layer;
                imageBlit.srcSubresource.layerCount     = 1;
                imageBlit.srcSubresource.mipLevel       = i - 1;
                imageBlit.srcOffsets[0]                 = { 0, 0, 0 };
                imageBlit.srcOffsets[1].x               = int32_t( std::max( width >> ( i - 1 ), 1u ) );
                imageBlit.srcOffsets[1].y               = int32_t( std::max( height >> ( i - 1 ), 1u ) );
                imageBlit.srcOffsets[1].z               = 1;

                imageBlit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                imageBlit.dstSubresource.baseArrayLayer = layer;
                imageBlit.dstSubresource.layerCount     = 1;
                imageBlit.dstSubresource.mipLevel       = i;
                imageBlit.dstOffsets[0]                 = { 0, 0, 0 };
                imageBlit.dstOffsets[1].x               = int32_t( std::max( width >> i, 1u ) );
                imageBlit.dstOffsets[1].y               = int32_t( std::max( height >> i, 1u ) );
                imageBlit.dstOffsets[1].z               = 1;

                VkImageSubresourceRange mipSubRange = {};
                mipSubRange.aspectMask              = VK_IMAGE_ASPECT_COLOR_BIT;
                mipSubRange.baseArrayLayer          = layer;
                mipSubRange.layerCount              = 1;
                mipSubRange.baseMipLevel            = i;
                mipSubRange.levelCount              = 1;

                Utils::InsertImageMemoryBarrier( commandBuffer, image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                 mipSubRange );

                vkCmdBlitImage( commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageBlit, VK_FILTER_LINEAR );

                Utils::InsertImageMemoryBarrier(
                     commandBuffer, image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, mipSubRange );
            }
        }

        if ( !transitionToShaderRead )
            return;

        VkImageSubresourceRange finalRange = {};
        finalRange.aspectMask              = VK_IMAGE_ASPECT_COLOR_BIT;
        finalRange.baseArrayLayer          = baseArrayLayer;
        finalRange.layerCount              = layerCount;
        finalRange.baseMipLevel            = 0;
        finalRange.levelCount              = mipLevels;

        Utils::InsertImageMemoryBarrier( commandBuffer, image, VK_ACCESS_TRANSFER_READ_BIT,
                                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, finalRange );
    }

    Common::BoolResultStr
    VulkanMipMapCubeGeneratorCS::GenerateMips( const std::shared_ptr<ImageCube>& /*imageCube*/ ) const
    {
        return Common::MakeError( "Not impl" );
    }

    namespace
    {
        // The one recording of a cube's blit chain, shared by the waited path and the batched one. The
        // two transitions around it carry explicit stages and access: inside a single command buffer the
        // mip-0 compute write before it and the shader reads after it are ordered by nothing else.
        void RecordCubeChain( VkCommandBuffer commandBuffer, VulkanImageCube& cube )
        {
            const auto& res = cube.GetResource();
            cube.TransitionLayout( commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_ACCESS_MEMORY_WRITE_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT );

            GenerateMipmapsTO( commandBuffer, res.Image, res.Format, cube.GetWidth(), cube.GetHeight(),
                               cube.GetMipmapLevels(), 0, 6, /*transitionToShaderRead=*/false );

            cube.TransitionLayout( commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                                   VK_ACCESS_MEMORY_READ_BIT );
        }
    } // namespace

    Common::BoolResultStr
    VulkanMipMapCubeGeneratorCS::RecordMips( GpuBatch& /*batch*/,
                                             const std::shared_ptr<ImageCube>& /*imageCube*/ ) const
    {
        return Common::MakeError( "Not impl" );
    }

    Common::BoolResultStr
    VulkanMipMapCubeGeneratorTO::GenerateMips( const std::shared_ptr<ImageCube>& imageCube ) const
    {
        const auto cmdAlloc = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !cmdAlloc ) return Common::MakeError( cmdAlloc.GetError() );

        VkCommandBuffer commandBuffer = cmdAlloc.GetValue();
        RecordCubeChain( commandBuffer, *SP_CAST( VulkanImageCube, imageCube ) );
        CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( commandBuffer );

        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr
    VulkanMipMapCubeGeneratorTO::RecordMips( GpuBatch& batch, const std::shared_ptr<ImageCube>& imageCube ) const
    {
        const VkCommandBuffer commandBuffer = RecordingBuffer( batch );
        if ( commandBuffer == VK_NULL_HANDLE )
            return Common::MakeError( "the GPU batch was already submitted; the mip chain cannot join it." );
        RecordCubeChain( commandBuffer, *SP_CAST( VulkanImageCube, imageCube ) );
        return Common::MakeSuccess( true );
    }

} // namespace Desert::Graphic::API::Vulkan
