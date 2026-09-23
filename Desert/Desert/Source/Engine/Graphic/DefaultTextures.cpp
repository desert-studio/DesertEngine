#include <Engine/Graphic/DefaultTextures.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // The one place a name becomes a colour. Keyed by the enum value so the array above and this
        // switch cannot drift apart by an index, and with no `default:` so a kind added to the enum is a
        // -Wswitch warning here rather than a texture that silently comes out white.
        std::array<unsigned char, 4> PixelOf( Core::Formats::DefaultTextureKind kind )
        {
            switch ( kind )
            {
                case Core::Formats::DefaultTextureKind::White:
                    return { 255, 255, 255, 255 };
                case Core::Formats::DefaultTextureKind::Black:
                    return { 0, 0, 0, 255 };
                case Core::Formats::DefaultTextureKind::Gray:
                    // 128, not 127: the same mid-grey the .png tools write, so a 1x1 default and a
                    // constant-grey texture an artist authors read identically.
                    return { 128, 128, 128, 255 };
                case Core::Formats::DefaultTextureKind::FlatNormal:
                    // (0,0,1) encoded as 0.5*n+0.5. A shader that unpacks with `2*t-1` gets +Z, i.e. the
                    // geometric normal untouched — which is what "no normal map" has to mean. White here
                    // would decode to a normalised (1,1,1), a normal tilted 54 degrees off the surface,
                    // and that is what an unbound slot fell back to before this table existed.
                    return { 128, 128, 255, 255 };
            }
            return { 255, 255, 255, 255 };
        }
    } // namespace

    DefaultTextures& DefaultTextures::Get()
    {
        // Function-local so it cannot be constructed before main; released explicitly by
        // Renderer::Shutdown(), because the destructor of a static runs after the device is gone.
        static DefaultTextures s_Instance;
        return s_Instance;
    }

    const Image2D* DefaultTextures::Resolve( Core::Formats::DefaultTextureKind kind )
    {
        const auto index = static_cast<std::size_t>( kind );
        if ( index >= m_Images.size() )
        {
            LOG_ERROR( "[DefaultTextures] kind {} is outside the table of {} — the enum grew and this "
                       "array did not",
                       index, m_Images.size() );
            return nullptr;
        }

        if ( m_Images[index] )
            return m_Images[index].get();

        const auto pixel = PixelOf( kind );

        Core::Formats::Image2DSpecification spec = {
             .Tag        = std::string( "DefaultTexture-" ) + Core::Formats::DefaultTextureKindName( kind ),
             .Width      = 1,
             .Height     = 1,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Mips       = 1,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::ImageProperties::Sample,
        };
        spec.Data = std::vector<unsigned char>( pixel.begin(), pixel.end() );

        // The device's own 1x1 fallbacks: built from a constant, not from a file, so nothing may release
        // them. See Engine/Graphic/ResourceLedger.hpp.
        const ResourceAttributionScope owned( ResourceOwner::Device );

        auto image = Image2D::Create( spec );
        if ( !image )
        {
            // DC §1.4: the caller gets nullptr and the log says which name failed. Handing back another
            // kind's image would make a broken upload look like an authored colour.
            LOG_ERROR( "[DefaultTextures] could not create the 1x1 image for '{}'; the sampler that asked "
                       "for it keeps whatever the backend fallback holds",
                       Core::Formats::DefaultTextureKindName( kind ) );
            return nullptr;
        }

        m_Images[index] = std::move( image );
        return m_Images[index].get();
    }

    Common::BoolResultStr DefaultTextures::Release()
    {
        // Keep releasing after the first failure and report the first message — the same rule
        // VulkanFallbackTextures::Release() records: stopping early leaks every image after the refusal.
        std::string firstError;
        for ( auto& image : m_Images )
        {
            if ( !image )
                continue;
            const auto released = image->Release();
            if ( !released.IsSuccess() && firstError.empty() )
                firstError = released.GetError();
            image.reset();
        }

        if ( !firstError.empty() )
            return Common::MakeError( firstError );

        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic
