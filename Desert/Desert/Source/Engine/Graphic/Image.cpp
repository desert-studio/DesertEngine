#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

namespace Desert::Graphic
{

    std::shared_ptr<Image2D> Image2D::Create( const Core::Formats::Image2DSpecification& spec )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {

                const auto& image  = std::make_shared<API::Vulkan::VulkanImage2D>( spec );
                const auto  result = image->RT_Invalidate();
                if ( !result.IsSuccess() )
                {
                    // ASKED, NOT ASSUMED — the treatment Image3D::Create below already had, and the
                    // reason it has it applies here word for word: handing back a half-built image
                    // pushes the failure into the first pass that samples it, with no connection to the
                    // allocation that actually failed. `nullptr` is inside this function's declared
                    // contract, not a new outcome: the RendererAPIType::None arm above returns it.
                    LOG_ERROR( "Image2D::Create: image {}x{} failed: {}", spec.Width, spec.Height,
                               result.GetError() );
                    return nullptr;
                }

                // THE SIZE IS RECORDED BY THE BACKEND'S ALLOCATION, NOT HERE. It used to be computed from
                // the specification at this line, and "this is the one place every 2D image passes
                // through" — the reason given — was false: seven sites (2 in VulkanFramebuffer.cpp, 5 in
                // VulkanFallbackTextures.cpp) construct the backend image directly, which is how 320 MiB
                // of shadow cascades reported zero bytes. See VulkanImage.cpp, beside the allocation.
                return image;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
    }

    std::shared_ptr<ImageCube> ImageCube::Create( const Core::Formats::ImageCubeSpecification& spec,
                                                  const std::unique_ptr<MipMapCubeGenerator>&  mipGenerator )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {

                const auto& image  = std::make_shared<API::Vulkan::VulkanImageCube>( spec );
                const auto  result = image->RT_Invalidate();
                if ( !result.IsSuccess() )
                {
                    LOG_ERROR( "ImageCube::Create: cube '{}' with a {}-texel face failed: {}", spec.Tag,
                               spec.FaceSize, result.GetError() );
                    return nullptr;
                }

                if ( spec.Mips > 1 && mipGenerator )
                {
                    mipGenerator->GenerateMips( image );
                }

                // Size recorded by the allocation — see VulkanImage.cpp and the note on the 2D arm above.
                return image;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
    }

    std::shared_ptr<Image3D> Image3D::Create( const Core::Formats::Image3DSpecification& spec )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                const auto& image  = std::make_shared<API::Vulkan::VulkanImage3D>( spec );
                const auto  result = image->RT_Invalidate();
                if ( !result.IsSuccess() )
                {
                    // Handing back a half-built volume would push the failure into the first dispatch
                    // that binds it, with no connection to the allocation that actually failed.
                    LOG_ERROR( "Image3D::Create: volume '{}' {}x{}x{} failed: {}", spec.Tag, spec.Width,
                               spec.Height, spec.Depth, result.GetError() );
                    return nullptr;
                }

                // Size recorded by the allocation — see VulkanImage.cpp and the note on the 2D arm above.
                return image;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
    }

    // `ImageCube::Copy` STOOD HERE AND WAS AN ALIAS, NOT A COPY. It read
    //
    //     std::make_shared<VulkanImageCube>( *SP_CAST( VulkanImageCube, targetImageCube ) )
    //
    // with a one-word unfinished-work marker above it, which is how long it had been unfinished.
    //
    // — a copy CONSTRUCTION of the backend object, which memberwise-copies `VkImage`, `VkImageView`,
    // `VkSampler` and the `VmaAllocation` out of the original. The result is not a second cubemap; it is a
    // second owner of the first one's device memory, and when either shared_ptr dies the survivor holds
    // freed handles. Exactly М9's finding about `MaterialProperty::Clone`, whose four implementations were
    // all aliases of the original's GPU object, one layer further down.
    //
    // DELETED RATHER THAN FIXED, for М9's reason and one more of its own: it had no caller anywhere in the
    // engine or the editor, so nothing is losing a capability, and a real cubemap copy is not a
    // constructor — it is an allocation plus a `vkCmdCopyImage` on a command buffer with two layout
    // transitions, i.e. a function that needs a device and a queue and belongs beside the mip generator
    // rather than beside `Create`. Anybody who needs one should write THAT and not restore this.
    //
    // It is also now UNWRITABLE in this shape: `Image` holds a move-only `ResourceOwnership` row
    // (ResourceLedger.hpp), so copy-constructing any image is a compile error rather than a double free.

    namespace Utils
    {
        bool IsDepthFormat( Core::Formats::ImageFormat format )
        {
            if ( format == Core::Formats::ImageFormat::DEPTH32F )
                return true;
            if ( format == Core::Formats::ImageFormat::DEPTH24STENCIL8 )
                return true;
            return false;
        }

        bool HasStencilComponent( Core::Formats::ImageFormat format )
        {
            switch ( format )
            {
                case Core::Formats::ImageFormat::DEPTH24STENCIL8:
                    return true;
                default:
                    return false;
            }
        }
    } // namespace Utils

} // namespace Desert::Graphic
