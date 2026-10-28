#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace Desert::Graphic
{
    void Texture2D::ReleaseImage()
    {
        if ( m_Service != nullptr )
            m_Service->Unregister( m_Handle );
    }

    TextureBackend TextureBackend::Device()
    {
        // Stateless: one instance serves every texture, and it lives as long as the process.
        static const DeviceImageFactory device;
        return TextureBackend{ device, *Runtime::ResourceRegistry::GetImageService() };
    }

    void Texture2D::AdoptImage( std::shared_ptr<Image2D>&& image, Runtime::ImageService& registry )
    {
        m_Service = &registry;
        m_Handle  = m_Service->Register( std::move( image ), Runtime::ImageHandle::Type::Image2D );
    }

    Common::ResultStr<std::shared_ptr<Texture2D>> Texture2D::CreateFromAsset( const std::filesystem::path& asset,
                                                                              const TextureBackend& backend )
    {
        auto cooked = ReadCooked( asset );
        if ( !cooked.IsSuccess() )
            return Common::MakeError<std::shared_ptr<Texture2D>>( cooked.GetError() );
        return CreateFromCooked( cooked.ExtractValue(), backend );
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
        cooked.ColorSpace = data.ColorSpace;
        cooked.Pixels = std::move( data.Pixels );
        cooked.Levels = std::move( spans );
        cooked.FullWidth  = cooked.Width;
        cooked.FullHeight = cooked.Height;
        cooked.FullLevelBytes.reserve( cooked.Levels.size() );
        for ( const Core::Formats::MipLevelSpan& level : cooked.Levels )
            cooked.FullLevelBytes.push_back( level.ByteSize );
        return Common::MakeSuccess( std::move( cooked ) );
    }

    Common::ResultStr<CookedTexture2D> Texture2D::ReadCookedMips( const std::filesystem::path& cookedPath,
                                                                  const uint32_t               firstMip,
                                                                  const uint32_t               tailSize )
    {
        auto cooked = ReadCooked( cookedPath );
        if ( !cooked.IsSuccess() )
            return cooked;
        CookedTexture2D texture = cooked.ExtractValue();
        uint32_t        first   = firstMip;
        if ( firstMip == kStreamingTail )
        {
            first = 0;
            while ( std::max( texture.FullWidth >> first, texture.FullHeight >> first ) > tailSize &&
                    first + 1 < texture.Levels.size() )
                ++first;
        }
        texture.KeepFromMip( first );
        return Common::MakeSuccess( std::move( texture ) );
    }

    void CookedTexture2D::KeepFromMip( const uint32_t firstMip )
    {
        if ( Levels.empty() )
            return;
        const uint32_t first = std::min<uint32_t>( firstMip, static_cast<uint32_t>( Levels.size() ) - 1 );
        if ( first == FirstMip )
            return;
        // Levels are always counted from the asset's level 0 by the caller; this texture may already be trimmed.
        if ( first < FirstMip )
            return;
        const uint32_t drop    = first - FirstMip;
        auto           compact = [&]( auto& blob )
        {
            using Element = typename std::remove_reference_t<decltype( blob )>::value_type;
            std::remove_reference_t<decltype( blob )> kept;
            uint64_t                                  bytes = 0;
            for ( std::size_t level = drop; level < Levels.size(); ++level )
                bytes += Levels[level].ByteSize;
            kept.resize( static_cast<std::size_t>( bytes / sizeof( Element ) ) );
            uint64_t                                 offset = 0;
            std::vector<Core::Formats::MipLevelSpan> spans;
            for ( std::size_t level = drop; level < Levels.size(); ++level )
            {
                std::memcpy( reinterpret_cast<unsigned char*>( kept.data() ) + offset,
                             reinterpret_cast<const unsigned char*>( blob.data() ) + Levels[level].ByteOffset,
                             static_cast<std::size_t>( Levels[level].ByteSize ) );
                spans.push_back( { offset, Levels[level].ByteSize } );
                offset += Levels[level].ByteSize;
            }
            blob   = std::move( kept );
            Levels = std::move( spans );
        };
        if ( auto* u8 = std::get_if<std::vector<unsigned char>>( &Pixels ) )
            compact( *u8 );
        else if ( auto* f32 = std::get_if<std::vector<float>>( &Pixels ) )
            compact( *f32 );
        else
            return; // a bare pointer carries no length to compact from: kept whole
        FirstMip = first;
        Width    = std::max( 1u, FullWidth >> first );
        Height   = std::max( 1u, FullHeight >> first );
    }

    Common::ResultStr<std::shared_ptr<Texture2D>> Texture2D::CreateFromCooked( CookedTexture2D       cooked,
                                                                               const TextureBackend& backend )
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
                                                                 .MipLevels  = std::move( cooked.Levels ),
                                                                 .ColorSpace = cooked.ColorSpace };

        auto image = backend.Images.CreateImage2D( imageSpec );
        if ( !image )
        {
            return Common::MakeFormattedError<std::shared_ptr<Texture2D>>(
                 "the GPU image for cooked texture '{}' ({}x{}, {} levels) was not created.", cooked.Tag,
                 cooked.Width, cooked.Height, levelCount );
        }

        texture->AdoptImage( std::move( image ), backend.Registry );
        return Common::MakeSuccess( texture );
    }

    Common::BoolResultStr Texture2D::ReplaceLevels( CookedTexture2D cooked, const TextureBackend& backend )
    {
        auto* current = m_Service != nullptr ? dynamic_cast<Image2D*>( m_Service->Resolve( m_Handle ) ) : nullptr;
        if ( current == nullptr )
            return Common::MakeFormattedError<bool>( "texture '{}' has no resident image to replace", cooked.Tag );
        const uint32_t width = cooked.Width, height = cooked.Height;
        const std::size_t                         levelCount = cooked.Levels.size();
        const Core::Formats::Image2DSpecification imageSpec  = { .Tag        = cooked.Tag,
                                                                 .Width      = cooked.Width,
                                                                 .Height     = cooked.Height,
                                                                 .Format     = cooked.Format,
                                                                 .Data       = std::move( cooked.Pixels ),
                                                                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                                                                 .Properties = Core::Formats::Sample,
                                                                 .MipLevels  = std::move( cooked.Levels ),
                                                                 .ColorSpace = cooked.ColorSpace };
        const std::shared_ptr<Image2D> staged = backend.Images.CreateImage2D( imageSpec );
        if ( !staged )
            return Common::MakeFormattedError<bool>( "the GPU image for texture '{}' ({}x{}, {} levels) was not created",
                                                     imageSpec.Tag, width, height, levelCount );
        // The ledger row moves with the pixels: the staged image is the asset's until it dies holding the old ones.
        staged->ClaimOwnership( current->GetResourceOwner(), current->GetOwningAsset() );
        if ( auto taken = current->TakeDeviceImageFrom( *staged ); !taken.IsSuccess() )
            return taken;
        m_Width  = width;
        m_Height = height;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::shared_ptr<Texture2D>>
    Texture2D::Create( const std::string& tag, uint32_t width, uint32_t height, Core::Formats::ImageFormat format,
                       Core::Formats::ImagePixelData&& data, const TextureBackend& backend )
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

        texture->AdoptImage( backend.Images.CreateImage2D( imageSpec ), backend.Registry );
        return Common::MakeSuccess( texture );
    }

} // namespace Desert::Graphic