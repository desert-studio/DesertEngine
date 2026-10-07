#include <Engine/UI/UICanvasResources.hpp>

#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::UI
{
    Graphic::Image2D* RegistryUICanvasResources::SpriteImage( const Assets::AssetHandle& sprite )
    {
        const Graphic::Texture2D* texture = Runtime::ResourceRegistry::GetTextureService()->Get( sprite );
        if ( texture == nullptr )
            return nullptr;
        return static_cast<Graphic::Image2D*>(
             Runtime::ResourceRegistry::GetImageService()->Resolve( texture->GetImageHandle() ) );
    }

    Graphic::Image2D* RegistryUICanvasResources::AnimatedFrame( const Assets::AssetHandle& sprite )
    {
        return Runtime::ResourceRegistry::GetAnimatedImageService()->Resolve( sprite );
    }

    Graphic::Image2D* RegistryUICanvasResources::VideoFrame( uint64_t video, float volume, bool muted )
    {
        return Runtime::ResourceRegistry::GetVideoService()->Resolve( video, { .Volume = volume, .Muted = muted } );
    }

    const Assets::UIThemeRuntime* RegistryUICanvasResources::Theme( const Assets::AssetHandle& theme )
    {
        return Runtime::ResourceRegistry::GetUIThemeService()->Get( theme );
    }

    uint64_t RegistryUICanvasResources::DefaultFontHandle()
    {
        return Runtime::ResourceRegistry::GetFontService()->DefaultFontHandle();
    }

    void RegistryUICanvasResources::RequestGlyphs( uint64_t font, const std::vector<uint32_t>& codepoints )
    {
        // The answer (did the atlas re-bake) is the font service's business; the walk resolves the font next
        // either way.
        (void)Runtime::ResourceRegistry::GetFontService()->RequestGlyphs( font, codepoints );
    }

    Runtime::Font* RegistryUICanvasResources::Font( uint64_t font, float pixelHeight )
    {
        return Runtime::ResourceRegistry::GetFontService()->Get( font, pixelHeight );
    }

    Runtime::Icon* RegistryUICanvasResources::Icon( uint64_t icon )
    {
        return Runtime::ResourceRegistry::GetIconService()->Get( icon );
    }

    const Graphic::Image2D* RegistryUICanvasResources::IconAtlas()
    {
        return Runtime::ResourceRegistry::GetIconService()->Atlas().get();
    }
} // namespace Desert::UI
