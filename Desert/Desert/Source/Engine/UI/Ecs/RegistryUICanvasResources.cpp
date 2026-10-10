#include <Engine/UI/Ecs/RegistryUICanvasResources.hpp>

#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Runtime/Services/Icon/IconService.hpp>

namespace Desert::UI
{
    namespace
    {
        // The id is the Image2D* itself (converted from Image2D*, not from a base, so it is the very address
        // Render2D keys its per-texture executor by).
        TextureRef RefOf( const Graphic::Image2D* image )
        {
            if ( image == nullptr )
                return {};
            return TextureRef{ .Id = image, .Width = image->GetWidth(), .Height = image->GetHeight() };
        }
    } // namespace

    TextureRef RegistryUICanvasResources::SpriteTexture( const Common::AssetHandle& sprite )
    {
        const Graphic::Texture2D* texture = Runtime::ResourceRegistry::GetTextureService()->Get( sprite );
        if ( texture == nullptr )
            return {};
        return RefOf( dynamic_cast<Graphic::Image2D*>(
             Runtime::ResourceRegistry::GetImageService()->Resolve( texture->GetImageHandle() ) ) );
    }

    TextureRef RegistryUICanvasResources::AnimatedFrame( const Common::AssetHandle& sprite )
    {
        return RefOf( Runtime::ResourceRegistry::GetAnimatedImageService()->Resolve( sprite ) );
    }

    TextureRef RegistryUICanvasResources::VideoFrame( uint64_t video, float volume, bool muted )
    {
        return RefOf( Runtime::ResourceRegistry::GetVideoService()->Resolve(
             video, { .Volume = volume, .Muted = muted } ) );
    }

    const UIThemeRuntime* RegistryUICanvasResources::Theme( const Common::AssetHandle& theme )
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

    FontFace RegistryUICanvasResources::Font( uint64_t font, float pixelHeight )
    {
        const Runtime::Font* baked = Runtime::ResourceRegistry::GetFontService()->Get( font, pixelHeight );
        if ( baked == nullptr )
            return {};
        return FontFace{ .Atlas = baked->Atlas.get(), .Baked = &baked->Baked };
    }

    IconRef RegistryUICanvasResources::Icon( uint64_t icon )
    {
        Runtime::IconService* icons = Runtime::ResourceRegistry::GetIconService();
        const Runtime::Icon*  baked = icons->Get( icon );
        if ( baked == nullptr )
            return {};
        return IconRef{ .Atlas = icons->Atlas().get(), .Layers = baked->Layers, .Aspect = baked->Aspect };
    }
} // namespace Desert::UI
