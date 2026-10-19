#include <Engine/Graphic/API/Vulkan/VulkanFallbackTextures.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

#include <Engine/Core/Formats/ImageFormat.hpp>

namespace Desert::Graphic::API::Vulkan
{

    VulkanFallbackTextures::VulkanFallbackTextures()
    {
        const std::vector<Core::Formats::ImageFormat> formats = { Core::Formats::ImageFormat::RGBA8F,
                                                                  Core::Formats::ImageFormat::RGBA32F };

        for ( auto format : formats )
        {
            CreateFallbackTexture2D( format );
            CreateFallbackTextureCube( format, false );
            CreateFallbackTextureCube( format, true );
            CreateFallbackStorageImage2D( format );
            CreateFallbackTexture3D( format );
            CreateFallbackStorageImage3D( format );
        }
        // The integer sampled fallback (usampler2D; RDG System.ZeroUint): a float image in an integer binding is a
        // format mismatch. Sampled 2D only - no integer cube, volume or storage image is read anywhere.
        CreateFallbackTexture2D( Core::Formats::ImageFormat::RG32_UINT );
    }

    const std::shared_ptr<Image2D>&
    VulkanFallbackTextures::GetFallbackTexture2D( Core::Formats::ImageFormat format ) const
    {
        return m_FallbackTextures2D.at( format );
    }

    const std::shared_ptr<Image2D>&
    VulkanFallbackTextures::GetFallbackStorageImage2D( Core::Formats::ImageFormat format ) const
    {
        return m_FallbackStorageImages2D.at( format );
    }

    const std::shared_ptr<ImageCube>&
    VulkanFallbackTextures::GetFallbackTextureCube( Core::Formats::ImageFormat format ) const
    {
        return m_FallbackTexturesCube.at( format );
    }

    const std::shared_ptr<ImageCube>&
    VulkanFallbackTextures::GetFallbackTextureCubeArray( Core::Formats::ImageFormat format ) const
    {
        return m_FallbackTexturesCubeArray.at( format );
    }

    const std::shared_ptr<Image3D>&
    VulkanFallbackTextures::GetFallbackTexture3D( Core::Formats::ImageFormat format ) const
    {
        return m_FallbackTextures3D.at( format );
    }

    const std::shared_ptr<Image3D>&
    VulkanFallbackTextures::GetFallbackStorageImage3D( Core::Formats::ImageFormat format ) const
    {
        return m_FallbackStorageImages3D.at( format );
    }

    void VulkanFallbackTextures::CreateFallbackTexture2D( Core::Formats::ImageFormat format )
    {
        Core::Formats::Image2DSpecification spec = {
             .Tag        = "VulkanFallbackTextures-2D",
             .Width      = 1,
             .Height     = 1,
             .Format     = format,
             .Mips       = 1,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::ImageProperties::Sample,
        };

        switch ( format )
        {
            case Core::Formats::ImageFormat::RGBA8F:
            case Core::Formats::ImageFormat::BGRA8F:
                spec.Data = std::vector<unsigned char>{ 255, 255, 255, 255 };
                break;
            case Core::Formats::ImageFormat::RGBA32F:
                spec.Data = std::vector<float>{ 1.0f, 1.0f, 1.0f, 1.0f };
                break;
            case Core::Formats::ImageFormat::RG32_UINT:
                spec.Data = std::vector<unsigned char>( 8u, 0u ); // two zero uint32 channels
                break;
            default:
                return;
        }

        auto texture = std::make_shared<VulkanImage2D>( spec );
        if ( texture->RT_Invalidate().IsSuccess() )
        {
            m_FallbackTextures2D[format] = texture;
        }
    }

    void VulkanFallbackTextures::CreateFallbackTextureCube( Core::Formats::ImageFormat format, bool array )
    {
        // @p array: the same black cube viewed as a cube array of one cube (GetFallbackTextureCubeArray).
        Core::Formats::ImageCubeSpecification spec = {
             .Tag        = array ? "VulkanFallbackTextures-CubeArray" : "VulkanFallbackTextures-Cube",
             .FaceSize   = 4,
             .Format     = format,
             .Mips       = 1,
             .Properties = Core::Formats::ImageProperties::Sample,
             .CubeArray  = array,
        };

        // NO `spec.Data`, AND THAT IS THE CORRECTION. It used to carry six white faces, which never
        // reached the device — VulkanImageCube::UploadData is a no-op — so the comment that stood here
        // said the pixels do not matter "because nothing that binds it is ever allowed to be read". That
        // was never true (an unbound `samplerCube` is sampled by the shader like any other) and Г14 made
        // it load-bearing: this image is now what a cube binding is pointed AT when the scene states it
        // has no environment, so its texels are the value of that statement. It is cleared below instead.
        switch ( format )
        {
            case Core::Formats::ImageFormat::RGBA8F:
            case Core::Formats::ImageFormat::BGRA8F:
            case Core::Formats::ImageFormat::RGBA32F:
                break;
            default:
                return;
        }

        auto texture = std::make_shared<VulkanImageCube>( spec );
        if ( !texture->RT_Invalidate().IsSuccess() )
            return;

        // BLACK, and the 2D fallback beside it is WHITE for a reason that is not inconsistency. A 2D
        // fallback stands in for a map that MULTIPLIES (albedo, roughness, a mask), whose identity is 1.
        // A cube stands in for an environment that is ADDED as light, whose identity is 0. White here
        // would mean "a uniform sky at full radiance in every direction" — every static surface in a
        // scene with no sky lit by an environment nobody authored.
        //
        // A FAILED CLEAR STILL PUBLISHES THE IMAGE, loudly. Every consumer reaches this table through
        // `.at( format )`, so withholding the entry turns a colour problem into an out_of_range throw far
        // from here; the honest outcome is a defined-shaped descriptor whose CONTENT is announced as
        // unknown, which is a thing a reader can act on.
        if ( const auto cleared = texture->RT_ClearToColor( 0.0f, 0.0f, 0.0f, 1.0f ); !cleared.IsSuccess() )
            LOG_ERROR( "[VulkanFallbackTextures] the cube fallback for ImageFormat {} could not be cleared, "
                       "so a cube binding that nothing has bound samples UNDEFINED texels: {}",
                       static_cast<uint32_t>( format ), cleared.GetError() );

        ( array ? m_FallbackTexturesCubeArray : m_FallbackTexturesCube )[format] = texture;
    }

    void VulkanFallbackTextures::CreateFallbackStorageImage2D( Core::Formats::ImageFormat format )
    {
        Core::Formats::Image2DSpecification spec = {
             .Tag        = "VulkanFallbackStorageImage-2D",
             .Width      = 1,
             .Height     = 1,
             .Format     = format,
             .Mips       = 1,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::ImageProperties::Storage,
        };

        switch ( format )
        {
            case Core::Formats::ImageFormat::RGBA8F:
            case Core::Formats::ImageFormat::BGRA8F:
                spec.Data = std::vector<unsigned char>{ 0, 0, 0, 255 };
                break;
            case Core::Formats::ImageFormat::RGBA32F:
                spec.Data = std::vector<float>{ 0.0f, 0.0f, 0.0f, 1.0f };
                break;
            default:
                return;
        }

        auto texture = std::make_shared<VulkanImage2D>( spec );
        if ( texture->RT_Invalidate().IsSuccess() )
        {
            m_FallbackStorageImages2D[format] = texture;
        }
    }

    // A 1x1x1 volume. Its only job is to be a DEFINED descriptor for a `sampler3D` binding that nothing
    // has bound yet — a binding left unwritten is undefined memory that reads as garbage, and the 2D
    // fallbacks are not substitutable here: a 2D view in a 3D binding samples nonsense without failing.
    // White, matching the 2D sampled fallback, so a missing volume reads as "no attenuation" rather than
    // as a black hole in the middle of an effect.
    void VulkanFallbackTextures::CreateFallbackTexture3D( Core::Formats::ImageFormat format )
    {
        Core::Formats::Image3DSpecification spec = {
             .Tag        = "VulkanFallbackTextures-3D",
             .Width      = 1,
             .Height     = 1,
             .Depth      = 1,
             .Format     = format,
             .Properties = Core::Formats::ImageProperties::Sample,
        };

        switch ( format )
        {
            case Core::Formats::ImageFormat::RGBA8F:
            case Core::Formats::ImageFormat::BGRA8F:
                spec.Data = std::vector<unsigned char>{ 255, 255, 255, 255 };
                break;
            case Core::Formats::ImageFormat::RGBA32F:
                spec.Data = std::vector<float>{ 1.0f, 1.0f, 1.0f, 1.0f };
                break;
            default:
                // Only the formats a fallback is actually requested for are built. Anything else would be
                // a lookup that throws out of .at() later, with no clue where it came from — so say it now.
                LOG_ERROR( "VulkanFallbackTextures: no 3D fallback defined for ImageFormat value {}",
                           static_cast<uint32_t>( format ) );
                return;
        }

        auto texture = std::make_shared<VulkanImage3D>( spec );
        if ( texture->RT_Invalidate().IsSuccess() )
        {
            m_FallbackTextures3D[format] = texture;
        }
    }

    void VulkanFallbackTextures::CreateFallbackStorageImage3D( Core::Formats::ImageFormat format )
    {
        Core::Formats::Image3DSpecification spec = {
             .Tag        = "VulkanFallbackStorageImage-3D",
             .Width      = 1,
             .Height     = 1,
             .Depth      = 1,
             .Format     = format,
             .Properties = Core::Formats::ImageProperties::Storage,
        };

        switch ( format )
        {
            case Core::Formats::ImageFormat::RGBA8F:
            case Core::Formats::ImageFormat::BGRA8F:
                spec.Data = std::vector<unsigned char>{ 0, 0, 0, 255 };
                break;
            case Core::Formats::ImageFormat::RGBA32F:
                spec.Data = std::vector<float>{ 0.0f, 0.0f, 0.0f, 1.0f };
                break;
            default:
                LOG_ERROR( "VulkanFallbackTextures: no 3D storage fallback defined for ImageFormat value {}",
                           static_cast<uint32_t>( format ) );
                return;
        }

        auto texture = std::make_shared<VulkanImage3D>( spec );
        if ( texture->RT_Invalidate().IsSuccess() )
        {
            m_FallbackStorageImages3D[format] = texture;
        }
    }

    Common::BoolResultStr VulkanFallbackTextures::Release()
    {
        // KEEP RELEASING AFTER THE FIRST FAILURE, and report the first message. This runs once at
        // teardown over six independent caches; stopping at the first refusal would leak every image
        // after it, and returning success unconditionally — which is what these loops did by
        // dropping every `Release()` result — makes the `[[nodiscard]]` on the caller's side a lie.
        std::string firstError;
        const auto  release = [&firstError]( auto& texture )
        {
            const auto released = texture->Release();
            if ( !released.IsSuccess() && firstError.empty() )
                firstError = released.GetError();
            texture.reset();
        };

        for ( auto& texture2D : m_FallbackTextures2D )
            release( texture2D.second );

        for ( auto& textureCube : m_FallbackTexturesCube )
            release( textureCube.second );

        for ( auto& textureCubeArray : m_FallbackTexturesCubeArray )
            release( textureCubeArray.second );

        for ( auto& storageImage : m_FallbackStorageImages2D )
            release( storageImage.second );

        for ( auto& texture3D : m_FallbackTextures3D )
            release( texture3D.second );

        for ( auto& storageImage : m_FallbackStorageImages3D )
            release( storageImage.second );

        if ( !firstError.empty() )
            return Common::MakeError( firstError );

        return BOOLSUCCESS;
    }

} // namespace Desert::Graphic::API::Vulkan