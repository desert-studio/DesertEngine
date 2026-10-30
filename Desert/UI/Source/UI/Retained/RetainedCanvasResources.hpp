#pragma once

#include <UI/UIAnimationSource.hpp>
#include <UI/UICanvasResources.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// THE RESOURCES OF A VIEW THAT DRAWS A RETAINED WIDGET TREE. Sprites, fonts, themes and icons are the host's
// (the same services a level's canvas draws with), so this forwards every one of them to @p inner. What it
// does not forward is the animation source: UI clips are authored on the ECS scene (UIAnimComponent) and a
// widget tree built in C++ has none — Slate animates from code — so its view evaluates an empty source
// rather than handing the engine's timeline a tree it refuses.
namespace Desert::UI
{
    // No clip drives any node.
    class NoClipUIAnimationSource final : public IUIAnimationSource
    {
    public:
        [[nodiscard]] std::unique_ptr<IUIAnimationSource> Clone() const override
        {
            return std::make_unique<NoClipUIAnimationSource>();
        }
        void Evaluate( const IUITree& scene, const UIAnimationStep& step ) override
        {
            (void)scene;
            (void)step;
        }
        void Reset() override
        {
        }
        [[nodiscard]] const UIClipSample* Sample( NodeId element ) const override
        {
            (void)element;
            return nullptr;
        }
    };

    class RetainedCanvasResources final : public IUICanvasResources
    {
    public:
        explicit RetainedCanvasResources( IUICanvasResources& inner ) : m_Inner( &inner )
        {
        }

        [[nodiscard]] TextureRef SpriteTexture( const Common::AssetHandle& sprite ) override
        {
            return m_Inner->SpriteTexture( sprite );
        }
        [[nodiscard]] uint64_t ImageAtPath( const std::string& path ) override
        {
            return m_Inner->ImageAtPath( path );
        }
        [[nodiscard]] TextureRef AnimatedFrame( const Common::AssetHandle& sprite ) override
        {
            return m_Inner->AnimatedFrame( sprite );
        }
        [[nodiscard]] TextureRef VideoFrame( uint64_t video, float volume, bool muted ) override
        {
            return m_Inner->VideoFrame( video, volume, muted );
        }
        [[nodiscard]] const UIThemeRuntime* Theme( const Common::AssetHandle& theme ) override
        {
            return m_Inner->Theme( theme );
        }
        [[nodiscard]] uint64_t DefaultFontHandle() override
        {
            return m_Inner->DefaultFontHandle();
        }
        void RequestGlyphs( uint64_t font, const std::vector<uint32_t>& codepoints ) override
        {
            m_Inner->RequestGlyphs( font, codepoints );
        }
        [[nodiscard]] FontFace Font( uint64_t font, float pixelHeight ) override
        {
            return m_Inner->Font( font, pixelHeight );
        }
        [[nodiscard]] IconRef Icon( uint64_t icon ) override
        {
            return m_Inner->Icon( icon );
        }
        [[nodiscard]] IUITextSource& Text() override
        {
            return m_Inner->Text();
        }
        [[nodiscard]] std::unique_ptr<IUIAnimationSource> CreateAnimationSource() override
        {
            return std::make_unique<NoClipUIAnimationSource>();
        }

    private:
        IUICanvasResources* m_Inner;
    };
} // namespace Desert::UI
