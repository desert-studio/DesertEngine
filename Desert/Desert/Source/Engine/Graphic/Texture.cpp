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

        // A CUBE IS NOT A 2D TEXTURE, AND THE CONTAINER CAN NOW SAY SO. Before v3 this was not a check
        // anybody could write: the file had no way to carry a face count, so every `.tex` was one layer
        // by construction. It does now, and the spans below are built from a table indexed by
        // (level, layer) -- handing them to an `Image2DSpecification` would upload face 0 of each level
        // and call the result a texture. Named here rather than surviving into a sampler.
        if ( data.Kind != Assets::Serialization::TextureKind::Texture2D || data.LayerCount != 1 )
        {
            return Common::MakeFormattedError<std::shared_ptr<Texture2D>>(
                 "cooked texture '{}' is kind {} with {} array layers, and Texture2D loads one-layer 2D "
                 "images only. A cube is loaded through its own path.",
                 cookedPath.string(), static_cast<uint32_t>( data.Kind ), data.LayerCount );
        }

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
        // ── WHAT THIS CLASS ACTUALLY DOES, WHICH IS NOT WHAT IT LOOKS LIKE ───────────────────────
        //
        // IT HAS NO CALLERS, AND IT HAS NEVER UPLOADED A PIXEL. `TextureCube::Create` is called from
        // nowhere in this repository; the IBL path builds its cubes through `ComputeImages` and never
        // through here. And until the container grew faces, the pixels this function put into
        // `ImageCubeSpecification::Data` were DISCARDED: `VulkanImageCube::UploadData` was an empty
        // function body that nothing called. Both ends looked right and the link between them threw
        // the image away, which is why a cube's content could only ever be a clear colour.
        //
        // THE UPLOAD EXISTS NOW AND THIS LAYOUT STILL CANNOT USE IT. `LoadTexture` returns a 4x3 CROSS
        // unwrap: the six faces are sub-rectangles of one image, each row of a face separated from the
        // next by the cross's full width. A `VkBufferImageCopy` names a contiguous run per face, so a
        // cross has to be REPACKED into six face-major blocks before any of it can be copied — and
        // `bufferRowLength` cannot express it either, because the faces also differ in their starting
        // row. That repack is real work with no consumer asking for it.
        //
        // SO IT REFUSES, LOUDLY, instead of building an empty cube and returning success — which is
        // what it used to do, on a line that said `return Common::MakeSuccess( true ); // TODO`. A
        // caller that appears gets a sentence naming what is missing rather than a black cubemap.
        return Common::MakeFormattedError<bool>(
             "TextureCube cannot load '{}': the file is a 4x3 cross unwrap and the cube upload path "
             "copies six contiguous faces. Repacking the cross into face-major blocks is unwritten work "
             "with no caller — the engine's cubes are built by ComputeImages, and a BAKED cube is loaded "
             "from its cooked container (Engine/Graphic/Environment/EnvironmentBake.hpp).",
             m_TexturePath.string() );
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