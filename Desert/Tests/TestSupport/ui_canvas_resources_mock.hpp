#pragma once

// A MOCK OF THE RESOURCES A CANVAS WALK DRAWS WITH (Engine/UI/UICanvasResources.hpp). It answers nothing --
// no sprite resolves, no font or icon exists, no theme is set -- which is the headless walk every UI suite
// wants: a sprite draws its flat colour, text and icons draw nothing. A suite that is ABOUT a resolved image
// tells the mock which animated-sprite handle answers with which image.

#include <Engine/UI/UICanvasResources.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace TestSupport
{
    class MockUICanvasResources final : public Desert::UI::IUICanvasResources
    {
    public:
        // @p handle now resolves as an animated sprite to @p image. The draw list stores the pointer as an
        // opaque id and never dereferences it, so any unique address is a complete stand-in for a GPU image.
        void AnswerAnimatedFrame( uint64_t handle, Desert::Graphic::Image2D* image )
        {
            m_Animated[handle] = image;
        }
        void ForgetAnimatedFrames()
        {
            m_Animated.clear();
        }

        [[nodiscard]] Desert::Graphic::Image2D* SpriteImage( const Desert::Assets::AssetHandle& ) override
        {
            return nullptr;
        }
        [[nodiscard]] Desert::Graphic::Image2D* AnimatedFrame( const Desert::Assets::AssetHandle& sprite ) override
        {
            const auto it = m_Animated.find( static_cast<uint64_t>( sprite ) );
            return it == m_Animated.end() ? nullptr : it->second;
        }
        [[nodiscard]] Desert::Graphic::Image2D* VideoFrame( uint64_t ) override
        {
            return nullptr;
        }
        [[nodiscard]] const Desert::Assets::UIThemeRuntime* Theme( const Desert::Assets::AssetHandle& ) override
        {
            return nullptr;
        }
        [[nodiscard]] uint64_t DefaultFontHandle() override
        {
            return 0;
        }
        void RequestGlyphs( uint64_t, const std::vector<uint32_t>& ) override {}
        [[nodiscard]] Desert::Runtime::Font* Font( uint64_t, float ) override
        {
            return nullptr;
        }
        [[nodiscard]] Desert::Runtime::Icon* Icon( uint64_t ) override
        {
            return nullptr;
        }
        [[nodiscard]] const Desert::Graphic::Image2D* IconAtlas() override
        {
            return nullptr;
        }

    private:
        std::unordered_map<uint64_t, Desert::Graphic::Image2D*> m_Animated;
    };
} // namespace TestSupport
