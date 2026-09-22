#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/IO/ImageReader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Graphic
{
    struct ImageBaseSpec
    {
        std::string                    Tag;
        uint32_t                       Width;
        uint32_t                       Height;
        Core::Formats::ImageFormat     Format;
        uint32_t                       Mips = 1;
        Core::Formats::ImagePixelData  Data;
        Core::Formats::ImageProperties Properties;
    };

    static ImageBaseSpec LoadTexture( const std::filesystem::path& path, bool alpha, bool /*isCube*/,
                                      const TextureSpecification& /*specification*/ )
    {

        bool isHDR = Core::IO::ImageReader::IsHDR( path );

        ImageBaseSpec imageSpecification;
        imageSpecification.Tag = Common::Utils::FileSystem::GetFileName( path );

        if ( isHDR )
        {
            const auto& imageData     = Core::IO::ImageReader::ReadHDR( path );
            imageSpecification.Width  = imageData.Width;
            imageSpecification.Height = imageData.Height;
            imageSpecification.Format = Core::Formats::ImageFormat::RGBA32F;
            imageSpecification.Data   = imageData.Data;
        }
        else
        {
            const auto& imageData     = Core::IO::ImageReader::Read( path, alpha );
            imageSpecification.Width  = imageData.Width;
            imageSpecification.Height = imageData.Height;
            imageSpecification.Format = Core::Formats::ImageFormat::RGBA8F;
            imageSpecification.Data   = imageData.Data;
        }

        imageSpecification.Properties = Core::Formats::Sample;
        imageSpecification.Mips = Utils::CalculateMipCount( imageSpecification.Width, imageSpecification.Height );

        LOG_INFO( "Loading texture {}, alpha channel = {}, HDR = {}",
                  Common::Utils::FileSystem::GetFileName( path ), alpha, isHDR );

        return imageSpecification;
    }

    Texture2D::Texture2D( const TextureSpecification& specification, const std::filesystem::path& path )
         : m_TexturePath( path ), m_Specification( specification )
    {
    }

    Common::BoolResultStr Texture2D::Invalidate()
    {
        if ( !Common::Utils::FileSystem::Exists( m_TexturePath ) )
        {
            return Common::MakeError( "File does not exists" );
        }
        const ImageBaseSpec imageBaseSpec = LoadTexture( m_TexturePath, true, false, m_Specification );

        const Core::Formats::Image2DSpecification imageSpec = { .Tag        = imageBaseSpec.Tag,
                                                                .Width      = imageBaseSpec.Width,
                                                                .Height     = imageBaseSpec.Height,
                                                                .Format     = imageBaseSpec.Format,
                                                                .Mips       = imageBaseSpec.Mips,
                                                                .Data       = imageBaseSpec.Data,
                                                                .Usage      = Core::Formats::Image2DUsage::Image2D,
                                                                .Properties = imageBaseSpec.Properties };
        const auto                                mipGenerator =
             m_Specification.GenerateMips ? MipMap2DGenerator::Create( MipGenStrategy::TransferOps ) : nullptr;
        m_Handle = Runtime::ResourceRegistry::GetImageService()->Register(
             Image2D::Create( imageSpec, mipGenerator ), Runtime::ImageHandle::Type::Image2D );
        return Common::MakeSuccess( true ); // TODO
        // return std::static_pointer_cast<Graphic::API::Vulkan::VulkanImage2D>( m_Image2D )->RT_Invalidate();
    }

    Common::ResultStr<std::shared_ptr<Texture2D>> Texture2D::Create( const TextureSpecification&  specification,
                                                                     const std::filesystem::path& path )
    {
        auto texture   = std::make_shared<Texture2D>( specification, path );
        auto invResult = texture->Invalidate();
        if ( !invResult.IsSuccess() )
        {
            return Common::MakeError<std::shared_ptr<Texture2D>>( invResult.GetError() );
        }
        return Common::MakeSuccess( texture );
    }

    Common::ResultStr<std::shared_ptr<Texture2D>>
    Texture2D::CreateFromCooked( const std::filesystem::path& cookedPath )
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( cookedPath );
        if ( !raw.IsSuccess() )
            return Common::MakeError<std::shared_ptr<Texture2D>>( raw.GetError() );

        auto decoded = Assets::Serialization::DecodeTextureBinary( raw.GetValue(), cookedPath.string() );
        if ( !decoded.IsSuccess() )
            return Common::MakeError<std::shared_ptr<Texture2D>>( decoded.GetError() );

        // MOVED OUT, NOT COPIED. The payload is 21.3 MB for a 2048x2048 texture, and every copy of it
        // between the file and the staging buffer is that many bytes of pure memcpy on the frame that
        // first touches the texture. `ImagePixelData` carries exactly this vector type, so the move
        // below is the whole handover.
        auto data = decoded.ExtractValue();

        std::vector<Core::Formats::MipLevelSpan> spans;
        spans.reserve( data.Levels.size() );
        for ( const Assets::Serialization::TextureLevel& level : data.Levels )
            spans.push_back( Core::Formats::MipLevelSpan{ level.ByteOffset, level.ByteSize } );

        auto texture      = std::make_shared<Texture2D>( TextureSpecification{ false }, cookedPath );
        texture->m_Width  = data.Width;
        texture->m_Height = data.Height;

        const Core::Formats::Image2DSpecification imageSpec = {
             .Tag        = Common::Utils::FileSystem::GetFileName( cookedPath ),
             .Width      = data.Width,
             .Height     = data.Height,
             .Format     = data.Format,
             .Data       = std::move( data.Pixels ),
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Sample,
             .MipLevels  = std::move( spans ) };

        auto image = Image2D::Create( imageSpec, nullptr );
        if ( !image )
        {
            return Common::MakeFormattedError<std::shared_ptr<Texture2D>>(
                 "the GPU image for cooked texture '{}' ({}x{}, {} levels) was not created.", cookedPath.string(),
                 data.Width, data.Height, data.Levels.size() );
        }

        texture->m_Handle = Runtime::ResourceRegistry::GetImageService()->Register(
             std::move( image ), Runtime::ImageHandle::Type::Image2D );
        return Common::MakeSuccess( texture );
    }

    Common::ResultStr<std::shared_ptr<Texture2D>>
    Texture2D::Create( const TextureSpecification& specification, const std::string& tag, uint32_t width,
                       uint32_t height, Core::Formats::ImageFormat format,
                       Core::Formats::ImagePixelData&& data )
    {
        auto texture      = std::make_shared<Texture2D>( specification, std::filesystem::path( tag ) );
        texture->m_Width  = width;
        texture->m_Height = height;

        const Core::Formats::Image2DSpecification imageSpec = {
            .Tag        = tag,
            .Width      = width,
            .Height     = height,
            .Format     = format,
            .Mips       = 1, // procedural data: single level (callers wanting mips can extend later)
            .Data       = std::move( data ),
            .Usage      = Core::Formats::Image2DUsage::Image2D,
            .Properties = Core::Formats::Sample };

        texture->m_Handle = Runtime::ResourceRegistry::GetImageService()->Register(
             Image2D::Create( imageSpec, nullptr ), Runtime::ImageHandle::Type::Image2D );
        return Common::MakeSuccess( texture );
    }

    // ***************************************************************************************************************//

    TextureCube::TextureCube( const TextureSpecification& specification, const std::filesystem::path& path )
         : m_TexturePath( path ), m_Specification( specification )
    {
    }

    Common::BoolResultStr TextureCube::Invalidate()
    {
        const ImageBaseSpec imageBaseSpec = LoadTexture( m_TexturePath, true, false, m_Specification );

        // The loaded file is a 4x3 cross unwrap, so the face is a quarter of the image's width. This is
        // the ONE place that arithmetic belongs — at the boundary where source-pixel layout meets the
        // cube — not inside every consumer of the spec.
        const Core::Formats::ImageCubeSpecification imageSpec = { .Tag        = imageBaseSpec.Tag,
                                                                  .FaceSize   = imageBaseSpec.Width / 4u,
                                                                  .Format     = imageBaseSpec.Format,
                                                                  .Mips       = 1u,
                                                                  .Data       = imageBaseSpec.Data,
                                                                  .Properties = imageBaseSpec.Properties };

        const auto mipGenerator =
             m_Specification.GenerateMips ? MipMapCubeGenerator::Create( MipGenStrategy::TransferOps ) : nullptr;
        m_Handle = Runtime::ResourceRegistry::GetImageService()->Register(
             ImageCube::Create( imageSpec, mipGenerator ), Runtime::ImageHandle::Type::ImageCube );
        return Common::MakeSuccess( true ); // TODO
        // return std::static_pointer_cast<Graphic::API::Vulkan::VulkanImage2D>( m_Image2D )->RT_Invalidate();
    }

    Common::ResultStr<std::shared_ptr<TextureCube>> TextureCube::Create( const TextureSpecification& specification,
                                                                         const std::filesystem::path& path )
    {
        auto texture =
             std::make_shared<TextureCube>( specification, path ); // path is FULL — no directory gluing here
        auto invResult = texture->Invalidate();
        if ( !invResult.IsSuccess() )
        {
            return Common::MakeError<std::shared_ptr<TextureCube>>( invResult.GetError() );
        }
        return Common::MakeSuccess( texture );
    }

} // namespace Desert::Graphic