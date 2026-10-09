#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <string>
#include <glm/glm.hpp>

// Leaf visuals: a vector icon, a sprite, and a live scene rendered into the rect.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // A vector icon. The artwork is an ASSET — an .svg imported once into a signed distance field
    // (Runtime::IconService) — so icons are added by dropping a file in, never by touching C++, and they
    // stay crisp at any size because the same SDF shader that draws text reconstructs the edge.
    // Place as a child of a button/panel, or standalone for status glyphs.
    struct UIIconData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Icon;

        PROPERTY( DisplayName( "Icon" ), Category( "UI Icon" ), Asset<IconAsset> )
        Assets::AssetHandle Icon; // .svg vector icon — drag one from the Content Browser or pick a built-in

        PROPERTY( DisplayName( "Color" ), Category( "UI Icon" ), Color )
        glm::vec3 Color = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Scale" ), Category( "UI Icon" ), Range( 0.2f, 1.0f ) )
        float Scale = 0.7f; // icon size as a fraction of the element's shorter side
    };

    // A plain image block: draws a sprite (any PNG/JPG/TGA — or an animated GIF) filling the element rect,
    // tinted by Tint*Opacity. The full-colour, raster alternative to the monochrome vector UIIcon —
    // drag any texture onto Sprite and position it freely with the element's anchors. 9-slice supported.
    struct UIImageData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Image;

        PROPERTY( DisplayName( "Sprite" ), Category( "UI Image" ), Asset<TextureAsset> )
        Assets::AssetHandle Sprite; // drag a texture (PNG/JPG/TGA/GIF) from the Content Browser

        PROPERTY( DisplayName( "Tint" ), Category( "UI Image" ), Color )
        glm::vec3 Tint = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Image" ), Range( 0.0f, 1.0f ) )
        float Opacity = 1.0f;

        PROPERTY( DisplayName( "Sprite Border L/T/R/B" ), Category( "UI Image" ) )
        glm::vec4 SpriteBorder = glm::vec4( 0.0f ); // 9-slice: source-px borders kept unstretched (0 = stretch)
    };

    // A LIVE WORLD INSIDE A UI ELEMENT (Ю16). The element's rect is filled with another scene — named by
    // its .desce path — rendered offscreen every frame through that scene's own camera, then sampled as a
    // texture. UE calls the family "render target": a character portrait in a menu, an inventory item you
    // can turn, a security-camera feed, a minimap.
    //
    // WHY A SCENE FILE AND NOT A MESH SLOT. A mesh slot would need this component to carry a camera, a
    // light rig and a background as well, and every one of those is a thing the scene editor already
    // authors better than a Details page can. The author builds the little world as a normal scene, puts
    // its camera where the shot should be, saves, and names it here — so what the element shows is
    // WYSIWYG in the tool that already exists, and this component stays four fields.
    //
    // WHY IT IS NOT FREE, said here because the price is the design. Each of these elements that is
    // actually on screen owns a Graphic::SceneRenderer, and therefore a view's worth of GPU memory
    // (Engine/Core/ViewBudget.hpp). One past the budget is REFUSED, by name and with numbers, and draws the
    // magenta error fill rather than nothing — see Engine/UI/UIRenderTextureSource.hpp for who decides
    // and Engine/Graphic/Render2D/UIRenderTextureCache.hpp for the accounting. An element the walk did
    // not draw this frame — scrolled away, or not Visible — is not on screen, and its slot goes back.
    struct UIRenderTextureData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::RenderTexture;

        // The scene to render, as a path a host can open ("Resources/Assets/Scenes/UI_Portrait.desce").
        // A PATH at runtime and not an AssetHandle because there is no SceneAsset type and no service that
        // hands one out: the host opens the file. The SCENE FILE names it by the `.desce` header GUID
        // (SCNE 31, `"Scene": {Guid, Path}`, ComponentRegistry.cpp), so a moved scene is still found.
        PROPERTY( DisplayName( "Scene" ), Category( "UI Render Texture" ),
                  Tooltip( "Path to a .desce rendered live into this element, e.g. "
                           "Content/Scenes/UI_Portrait.desce" ) )
        std::string ScenePath;

        PROPERTY( DisplayName( "Tint" ), Category( "UI Render Texture" ), Color )
        glm::vec3 Tint = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Render Texture" ), Range( 0.0f, 1.0f ) )
        float Opacity = 1.0f;

        // Offscreen pixels per element pixel. 1 renders the world at the size it is shown; below that it
        // is cheaper and softer, above it supersamples. It is a knob on the PICTURE and on the cost at
        // once, which is why it is the only performance control here — the rest of the world's quality is
        // the scene's own business.
        PROPERTY( DisplayName( "Resolution Scale" ), Category( "UI Render Texture" ), Range( 0.25f, 2.0f ) )
        float ResolutionScale = 1.0f;
    };
} // namespace Desert::UI
