#pragma once

#include <Engine/Assets/Common.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Assets
{
    struct UIThemeRuntime;
} // namespace Desert::Assets

namespace Desert::Graphic
{
    class Image2D;
} // namespace Desert::Graphic

namespace Desert::Runtime
{
    struct Font;
    struct Icon;
} // namespace Desert::Runtime

// WHAT THE CANVAS WALK READS FROM THE ENGINE'S RESOURCES, stated as the questions it asks and nothing more.
//
// The walk (UICanvasRenderer2D) resolves sprites, GIF and video frames, themes, fonts and icons. It used to
// reach for each through ResourceRegistry's process-wide accessors, so a suite that wanted to assert what the
// walk draws had to REDEFINE those accessors at link time -- and once the suites shared one test runner with
// the engine's own definitions, that was a duplicate symbol, and a suite could not say "this one handle
// resolves to this image" any other way. It also gave every helper an "is the service there?" branch the
// engine never takes, because ResourceRegistry never answers null.
//
// So the view is HANDED its resources (UIViewContext's constructor), the walk asks them, and every answer
// that can be "nothing" is a pointer the walk already copes with: an unset or unresolvable sprite draws its
// flat colour, a missing font or icon draws nothing. RegistryUICanvasResources is the engine's answer; a test
// passes a mock that answers what the test is about.
namespace Desert::UI
{
    class IUICanvasResources
    {
    public:
        virtual ~IUICanvasResources() = default;

        // A sprite's texture asset as its runtime GPU image (non-owning; Render2D keys its per-texture
        // executor by the raw pointer). nullptr when the handle names no loaded texture.
        [[nodiscard]] virtual Graphic::Image2D* SpriteImage( const Assets::AssetHandle& sprite ) = 0;

        // An animated (GIF) sprite's current frame, a pure function of wall-clock time. nullptr for a handle
        // that is not an animated image, so an ordinary texture falls through to SpriteImage.
        [[nodiscard]] virtual Graphic::Image2D* AnimatedFrame( const Assets::AssetHandle& sprite ) = 0;

        // A streamed video's stable frame texture; nullptr while the stream has no frame.
        [[nodiscard]] virtual Graphic::Image2D* VideoFrame( uint64_t video ) = 0;

        // The theme a canvas's slot names; nullptr for an empty slot, and every element then draws its own
        // authored colours.
        [[nodiscard]] virtual const Assets::UIThemeRuntime* Theme( const Assets::AssetHandle& theme ) = 0;

        // The font a text element with no font of its own draws with.
        [[nodiscard]] virtual uint64_t DefaultFontHandle() = 0;

        // Ask for @p codepoints to be in @p font's atlas BEFORE the font is resolved, so a re-bake happens
        // ahead of the draw and non-ASCII text is right on its first frame rather than a frame late.
        virtual void RequestGlyphs( uint64_t font, const std::vector<uint32_t>& codepoints ) = 0;

        // @p font baked at @p pixelHeight; nullptr when it cannot be had, and the text draws nothing.
        [[nodiscard]] virtual Runtime::Font* Font( uint64_t font, float pixelHeight ) = 0;

        // An icon's layers in the shared SDF atlas, and that atlas; either null and the icon draws nothing.
        [[nodiscard]] virtual Runtime::Icon*          Icon( uint64_t icon )  = 0;
        [[nodiscard]] virtual const Graphic::Image2D* IconAtlas()            = 0;
    };

    // The engine's resources: ResourceRegistry's texture, image, animated-image, video, theme, font and icon
    // services. Stateless -- every answer is the registry's at the moment of asking -- so a host keeps one
    // beside its view and the two die together.
    class RegistryUICanvasResources final : public IUICanvasResources
    {
    public:
        [[nodiscard]] Graphic::Image2D*             SpriteImage( const Assets::AssetHandle& sprite ) override;
        [[nodiscard]] Graphic::Image2D*             AnimatedFrame( const Assets::AssetHandle& sprite ) override;
        [[nodiscard]] Graphic::Image2D*             VideoFrame( uint64_t video ) override;
        [[nodiscard]] const Assets::UIThemeRuntime* Theme( const Assets::AssetHandle& theme ) override;
        [[nodiscard]] uint64_t                      DefaultFontHandle() override;
        void                                        RequestGlyphs( uint64_t                     font,
                                                                   const std::vector<uint32_t>& codepoints ) override;
        [[nodiscard]] Runtime::Font*                Font( uint64_t font, float pixelHeight ) override;
        [[nodiscard]] Runtime::Icon*                Icon( uint64_t icon ) override;
        [[nodiscard]] const Graphic::Image2D*       IconAtlas() override;
    };
} // namespace Desert::UI
