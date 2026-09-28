#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Graphic
{
    void Texture2D::ReleaseImage()
    {
        if ( m_Service != nullptr )
            m_Service->Unregister( m_Handle );
    }

    void Texture2D::AdoptImage( std::shared_ptr<Image2D>&& image )
    {
        m_Service = Runtime::ResourceRegistry::GetImageService();
        m_Handle  = m_Service->Register( std::move( image ), Runtime::ImageHandle::Type::Image2D );
    }

    Common::ResultStr<std::shared_ptr<Texture2D>>
    Texture2D::CreateFromAsset( const std::filesystem::path& cookedPath )
    {
        auto cooked = ReadCooked( cookedPath );
        if ( !cooked.IsSuccess() )
            return Common::MakeError<std::shared_ptr<Texture2D>>( cooked.GetError() );
        return CreateFromCooked( cooked.ExtractValue() );
    }

    Common::ResultStr<CookedTexture2D> Texture2D::ReadCooked( const std::filesystem::path& cookedPath )
    {
        const auto raw = Assets::LoadTexturePlatformData( cookedPath );
        if ( !raw.IsSuccess() )
            return Common::MakeError<CookedTexture2D>( raw.GetError() );

        auto decoded = Assets::Serialization::DecodeTextureBinary( raw.GetValue(), cookedPath.string() );
        if ( !decoded.IsSuccess() )
            return Common::MakeError<CookedTexture2D>( decoded.GetError() );

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
            return Common::MakeFormattedError<CookedTexture2D>(
                 "cooked texture '{}' is kind {} with {} array layers, and Texture2D loads one-layer 2D "
                 "images only. A cube is loaded through its own path.",
                 cookedPath.string(), static_cast<uint32_t>( data.Kind ), data.LayerCount );
        }

        std::vector<Core::Formats::MipLevelSpan> spans;
        spans.reserve( data.Levels.size() );
        for ( const Assets::Serialization::TextureLevel& level : data.Levels )
            spans.push_back( Core::Formats::MipLevelSpan{ level.ByteOffset, level.ByteSize } );

        CookedTexture2D cooked;
        cooked.Tag    = Common::Utils::FileSystem::GetFileName( cookedPath );
        cooked.Width  = data.Width;
        cooked.Height = data.Height;
        cooked.Format = data.Format;
        cooked.Pixels = std::move( data.Pixels );
        cooked.Levels = std::move( spans );
        return Common::MakeSuccess( std::move( cooked ) );
    }

    Common::ResultStr<std::shared_ptr<Texture2D>> Texture2D::CreateFromCooked( CookedTexture2D&& cooked )
    {
        auto texture      = std::make_shared<Texture2D>();
        texture->m_Width  = cooked.Width;
        texture->m_Height = cooked.Height;

        const std::size_t                         levelCount = cooked.Levels.size();
        const Core::Formats::Image2DSpecification imageSpec  = { .Tag        = cooked.Tag,
                                                                 .Width      = cooked.Width,
                                                                 .Height     = cooked.Height,
                                                                 .Format     = cooked.Format,
                                                                 .Data       = std::move( cooked.Pixels ),
                                                                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                                                                 .Properties = Core::Formats::Sample,
                                                                 .MipLevels  = std::move( cooked.Levels ) };

        auto image = Image2D::Create( imageSpec );
        if ( !image )
        {
            return Common::MakeFormattedError<std::shared_ptr<Texture2D>>(
                 "the GPU image for cooked texture '{}' ({}x{}, {} levels) was not created.", cooked.Tag,
                 cooked.Width, cooked.Height, levelCount );
        }

        texture->AdoptImage( std::move( image ) );
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

        texture->AdoptImage( Image2D::Create( imageSpec ) );
        return Common::MakeSuccess( texture );
    }

} // namespace Desert::Graphic