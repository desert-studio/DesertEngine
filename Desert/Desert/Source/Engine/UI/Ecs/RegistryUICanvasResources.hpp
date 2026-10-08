#pragma once

#include <Engine/UI/UICanvasResources.hpp>
#include <Engine/UI/Ecs/LocalizationUITextSource.hpp>

namespace Desert::UI
{
    // The engine's resources (UICanvasResources.hpp): ResourceRegistry's texture, image, animated-image, video,
    // theme, font and icon services, and the Localization service for text. Stateless -- every answer is the
    // registry's at the moment of asking -- so a host keeps one beside its view and the two die together.
    class RegistryUICanvasResources final : public IUICanvasResources
    {
    public:
        [[nodiscard]] TextureRef            SpriteTexture( const Common::AssetHandle& sprite ) override;
        [[nodiscard]] TextureRef            AnimatedFrame( const Common::AssetHandle& sprite ) override;
        [[nodiscard]] TextureRef            VideoFrame( uint64_t video, float volume, bool muted ) override;
        [[nodiscard]] const UIThemeRuntime* Theme( const Common::AssetHandle& theme ) override;
        [[nodiscard]] uint64_t              DefaultFontHandle() override;
        void RequestGlyphs( uint64_t font, const std::vector<uint32_t>& codepoints ) override;
        [[nodiscard]] FontFace       Font( uint64_t font, float pixelHeight ) override;
        [[nodiscard]] IconRef        Icon( uint64_t icon ) override;
        [[nodiscard]] IUITextSource& Text() override
        {
            return m_Text;
        }

    private:
        LocalizationUITextSource m_Text;
    };
} // namespace Desert::UI
