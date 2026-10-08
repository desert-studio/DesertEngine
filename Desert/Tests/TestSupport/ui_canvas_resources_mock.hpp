#pragma once

// A MOCK OF THE RESOURCES A CANVAS WALK DRAWS WITH (Engine/UI/UICanvasResources.hpp). It answers nothing --
// no sprite resolves, no font or icon exists, no theme is set -- which is the headless walk every UI suite
// wants: a sprite draws its flat colour, text and icons draw nothing. A suite that is ABOUT a resolved image
// tells the mock which animated-sprite handle answers with which image.

#include <Engine/UI/UICanvasResources.hpp>
#include <Engine/UI/Ecs/LocalizationUITextSource.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace TestSupport
{
    class MockUICanvasResources final : public Desert::UI::IUICanvasResources
    {
    public:
        // @p handle now resolves as an animated sprite to @p texture. The draw list stores the id as opaque and
        // never dereferences it, so any unique address is a complete stand-in for a GPU image.
        void AnswerAnimatedFrame( uint64_t handle, const void* texture )
        {
            m_Animated[handle] = texture;
        }
        void ForgetAnimatedFrames()
        {
            m_Animated.clear();
        }

        [[nodiscard]] Desert::UI::TextureRef SpriteTexture( const ::Common::AssetHandle& ) override
        {
            return {};
        }
        [[nodiscard]] Desert::UI::TextureRef AnimatedFrame( const ::Common::AssetHandle& sprite ) override
        {
            const auto it = m_Animated.find( static_cast<uint64_t>( sprite ) );
            return it == m_Animated.end() ? Desert::UI::TextureRef{} : Desert::UI::TextureRef{ .Id = it->second };
        }
        [[nodiscard]] Desert::UI::TextureRef VideoFrame( uint64_t, float, bool ) override
        {
            return {};
        }
        [[nodiscard]] const Desert::UI::UIThemeRuntime* Theme( const ::Common::AssetHandle& ) override
        {
            return nullptr;
        }
        [[nodiscard]] uint64_t DefaultFontHandle() override
        {
            return 0;
        }
        void RequestGlyphs( uint64_t, const std::vector<uint32_t>& ) override
        {
        }
        [[nodiscard]] Desert::UI::FontFace Font( uint64_t, float ) override
        {
            return {};
        }
        [[nodiscard]] Desert::UI::IconRef Icon( uint64_t ) override
        {
            return {};
        }
        // Text is the engine's own: a suite about a translated label reads the Localization service it set up.
        [[nodiscard]] Desert::UI::IUITextSource& Text() override
        {
            return m_Text;
        }

    private:
        std::unordered_map<uint64_t, const void*> m_Animated;
        Desert::UI::LocalizationUITextSource      m_Text;
    };
} // namespace TestSupport
