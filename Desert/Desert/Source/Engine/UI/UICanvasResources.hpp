#pragma once

#include <Engine/UI/UIAnimationSource.hpp>
#include <Engine/UI/UITextSource.hpp>
#include <Engine/UI/Style/UIThemeRuntime.hpp>
#include <Engine/Text/BakedFont.hpp>
#include <Engine/Text/IconLayer.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
// flat colour, a missing font or icon draws nothing. RegistryUICanvasResources (UI/Ecs/) is the engine's
// answer; a test passes a mock that answers what the test is about.
//
// EVERY ANSWER IS UI DATA, never an engine type: a texture is an opaque id plus its size (the draw list never
// dereferences the id; the 2D backend binds it), a font is its atlas id plus its baked metrics, an icon is its
// atlas id plus its layers. The walk drew with nothing more than that even when the answers were
// Graphic::Image2D / Runtime::Font / Runtime::Icon -- so the framework no longer includes any of the three.
namespace Desert::UI
{
    // A texture as the walk draws it: the opaque id the draw list carries to the backend (the engine's
    // Image2D*, by which Render2D keys its per-texture executor) and the size 9-slice divides by. Id null =
    // nothing resolved.
    struct TextureRef
    {
        const void* Id     = nullptr;
        uint32_t    Width  = 0;
        uint32_t    Height = 0;

        [[nodiscard]] explicit operator bool() const
        {
            return Id != nullptr;
        }
    };

    // A font at one bake size: its distance atlas (opaque id) and the metrics that lay text out in it. Both
    // owned by the resources; valid for the frame. Baked null = the font cannot be had.
    struct FontFace
    {
        const void*            Atlas = nullptr;
        const Text::BakedFont* Baked = nullptr;
    };

    // An icon: the shared SDF atlas its layers address and the layers themselves, back-to-front. Empty
    // layers or a null atlas = the icon draws nothing.
    struct IconRef
    {
        const void*                      Atlas = nullptr;
        std::span<const Text::IconLayer> Layers;
        float                            Aspect = 1.0f; // source viewBox width / height
    };

    class IUICanvasResources
    {
    public:
        virtual ~IUICanvasResources() = default;

        // A sprite's texture asset as its runtime GPU texture. Empty when the handle names no loaded texture.
        [[nodiscard]] virtual TextureRef SpriteTexture( const Common::AssetHandle& sprite ) = 0;

        // An animated (GIF) sprite's current frame, a pure function of wall-clock time. Empty for a handle
        // that is not an animated image, so an ordinary texture falls through to SpriteTexture.
        [[nodiscard]] virtual TextureRef AnimatedFrame( const Common::AssetHandle& sprite ) = 0;

        // A streamed video's stable frame texture; empty while the stream has no frame. @p volume and @p muted
        // are what the drawing panel asks of the clip's sound (UIPanelData::VideoVolume / VideoMuted).
        [[nodiscard]] virtual TextureRef VideoFrame( uint64_t video, float volume, bool muted ) = 0;

        // The theme a canvas's slot names; nullptr for an empty slot, and every element then draws its own
        // authored colours.
        [[nodiscard]] virtual const UIThemeRuntime* Theme( const Common::AssetHandle& theme ) = 0;

        // The font a text element with no font of its own draws with.
        [[nodiscard]] virtual uint64_t DefaultFontHandle() = 0;

        // Ask for @p codepoints to be in @p font's atlas BEFORE the font is resolved, so a re-bake happens
        // ahead of the draw and non-ASCII text is right on its first frame rather than a frame late.
        virtual void RequestGlyphs( uint64_t font, const std::vector<uint32_t>& codepoints ) = 0;

        // @p font baked at @p pixelHeight; Baked null when it cannot be had, and the text draws nothing.
        [[nodiscard]] virtual FontFace Font( uint64_t font, float pixelHeight ) = 0;

        // An icon's layers in the shared SDF atlas; no layers or no atlas and the icon draws nothing.
        [[nodiscard]] virtual IconRef Icon( uint64_t icon ) = 0;

        // Where every authored string the walk draws is turned into the string on screen (UITextSource.hpp).
        [[nodiscard]] virtual IUITextSource& Text() = 0;

        // A new, empty animation source for ONE view (UIAnimationSource.hpp): each view evaluates the scene's
        // clips into results of its own, so this is a factory and not a shared service. Never null.
        [[nodiscard]] virtual std::unique_ptr<IUIAnimationSource> CreateAnimationSource() = 0;
    };
} // namespace Desert::UI
