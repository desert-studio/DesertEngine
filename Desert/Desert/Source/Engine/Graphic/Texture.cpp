#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Graphic
{
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

        auto texture      = std::make_shared<Texture2D>();
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

        auto image = Image2D::Create( imageSpec );
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

    Common::ResultStr<std::shared_ptr<Texture2D>> Texture2D::Create( const std::string& tag, uint32_t width,
                                                                     uint32_t                        height,
                                                                     Core::Formats::ImageFormat      format,
                                                                     Core::Formats::ImagePixelData&& data )
    {
        auto texture      = std::make_shared<Texture2D>();
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
             Image2D::Create( imageSpec ), Runtime::ImageHandle::Type::Image2D );
        return Common::MakeSuccess( texture );
    }

} // namespace Desert::Graphic