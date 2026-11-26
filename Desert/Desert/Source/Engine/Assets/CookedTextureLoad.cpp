#include <Engine/Assets/CookedTextureLoad.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Desert::Assets
{
    Common::ResultStr<std::shared_ptr<Graphic::Texture2D>>
    CreateTextureFromAsset( const std::filesystem::path& asset, const Graphic::TextureBackend& backend )
    {
        auto cooked = ReadCookedTexture( asset );
        if ( !cooked.IsSuccess() )
            return Common::MakeError<std::shared_ptr<Graphic::Texture2D>>( cooked.GetError() );
        return Graphic::Texture2D::CreateFromCooked( cooked.ExtractValue(), backend );
    }

    Common::ResultStr<Graphic::CookedTexture2D> ReadCookedTexture( const std::filesystem::path& cookedPath )
    {
        using Graphic::CookedTexture2D;

        const auto raw = LoadTexturePlatformData( cookedPath );
        if ( !raw.IsSuccess() )
            return Common::MakeError<CookedTexture2D>( raw.GetError() );

        auto decoded = Serialization::DecodeTextureBinary( raw.GetValue(), cookedPath.string() );
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
        if ( data.Kind != Serialization::TextureKind::Texture2D || data.LayerCount != 1 )
        {
            return Common::MakeFormattedError<CookedTexture2D>(
                 "cooked texture '{}' is kind {} with {} array layers, and Texture2D loads one-layer 2D "
                 "images only. A cube is loaded through its own path.",
                 cookedPath.string(), static_cast<uint32_t>( data.Kind ), data.LayerCount );
        }

        std::vector<Core::Formats::MipLevelSpan> spans;
        spans.reserve( data.Levels.size() );
        for ( const Serialization::TextureLevel& level : data.Levels )
            spans.push_back( Core::Formats::MipLevelSpan{ level.ByteOffset, level.ByteSize } );

        CookedTexture2D cooked;
        cooked.Tag        = Common::Utils::FileSystem::GetFileName( cookedPath );
        cooked.Width      = data.Width;
        cooked.Height     = data.Height;
        cooked.Format     = data.Format;
        cooked.ColorSpace = data.ColorSpace;
        cooked.Pixels     = std::move( data.Pixels );
        cooked.Levels     = std::move( spans );
        cooked.FullWidth  = cooked.Width;
        cooked.FullHeight = cooked.Height;
        cooked.FullLevelBytes.reserve( cooked.Levels.size() );
        for ( const Core::Formats::MipLevelSpan& level : cooked.Levels )
            cooked.FullLevelBytes.push_back( level.ByteSize );
        return Common::MakeSuccess( std::move( cooked ) );
    }

    Common::ResultStr<Graphic::CookedTexture2D> ReadCookedTextureMips( const std::filesystem::path& cookedPath,
                                                                       const uint32_t               firstMip,
                                                                       const uint32_t               tailSize )
    {
        auto cooked = ReadCookedTexture( cookedPath );
        if ( !cooked.IsSuccess() )
            return cooked;
        Graphic::CookedTexture2D texture = cooked.ExtractValue();
        uint32_t                 first   = firstMip;
        if ( firstMip == Graphic::Texture2D::kStreamingTail )
        {
            first = 0;
            while ( std::max( texture.FullWidth >> first, texture.FullHeight >> first ) > tailSize &&
                    first + 1 < texture.Levels.size() )
                ++first;
        }
        texture.KeepFromMip( first );
        return Common::MakeSuccess( std::move( texture ) );
    }

    std::shared_ptr<Graphic::Texture2D> CreateTexture2D( const std::shared_ptr<TextureAsset>& asset )
    {
        if ( !asset )
        {
            LOG_ERROR(
                 "[CookedTextureLoad] CreateTexture2D was handed a null TextureAsset; no texture is built." );
            return nullptr;
        }

        const auto textureResult = CreateTextureFromAsset( asset->GetMetadata().Filepath );
        if ( !textureResult.IsSuccess() )
        {
            LOG_ERROR( "[CookedTextureLoad] Building the GPU texture for cooked asset '{0}' failed: {1}",
                       asset->GetMetadata().Filepath.string(), textureResult.GetError() );
            return nullptr;
        }

        return textureResult.GetValue();
    }
} // namespace Desert::Assets
