#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{

    // The backend builds and asks (IRhi::CreateImage*): null = it refused and logged why — the image's size,
    // tag and the allocation's error, where they are known.
    std::shared_ptr<Image2D> Image2D::Create( const Core::Formats::Image2DSpecification& spec )
    {
        return Renderer::GetInstance().Rhi().CreateImage2D( spec );
    }

    std::shared_ptr<ImageCube> ImageCube::Create( const Core::Formats::ImageCubeSpecification& spec,
                                                  const std::unique_ptr<MipMapCubeGenerator>&  mipGenerator )
    {
        auto image = Renderer::GetInstance().Rhi().CreateImageCube( spec );
        if ( image && spec.Mips > 1 && mipGenerator )
        {
            mipGenerator->GenerateMips( image );
        }
        return image;
    }

    std::shared_ptr<Image3D> Image3D::Create( const Core::Formats::Image3DSpecification& spec )
    {
        return Renderer::GetInstance().Rhi().CreateImage3D( spec );
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
