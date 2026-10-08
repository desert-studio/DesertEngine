#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <glm/glm.hpp>

// The root of a UI tree: how it maps to the viewport, its draw order and its theme.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // How the canvas maps to the viewport (Unity CanvasScaler-style):
    //  Stretch        - canvas == the WHOLE viewport, 1:1 pixels. Layout is driven by anchors, so a full-screen
    //                   element (anchors 0,0-1,1) fills any resolution and nothing "zooms" when the window
    //                   resizes. The resolution-independent default.
    //  ScaleWithScreen- canvas == the whole viewport too, but the ENTIRE design is scaled from the reference
    //                   resolution (offsets + font sizes multiplied), so a layout authored at 1280x720 keeps
    //                   its proportions on any screen. MatchWidthHeight blends width- vs height-based scaling.
    //  Letterbox      - the reference resolution scaled to FIT inside the viewport and centred (black bars).
    //                   For fixed-aspect, pixel-perfect designs.
    enum class UICanvasScaleMode
    {
        Stretch,
        ScaleWithScreen,
        Letterbox
    };

    // Where the canvas lives. ScreenSpace = a flat overlay (menus/HUD). WorldSpace = billboarded at the
    // canvas entity's 3D position, projected to the screen + distance-scaled each frame (nameplate over an
    // NPC, a floating panel). WorldSpace needs the camera's view-proj (passed by the renderer's caller).
    enum class UICanvasRenderMode
    {
        ScreenSpace,
        WorldSpace
    };

    // Root of a screen-space UI tree. Child entities with a UILayout are laid out against this canvas. Add UI
    // elements as CHILDREN of the canvas entity (the viewport "UI" menu / UI Editor do this for you).
    struct UICanvasData
    {
        REFLECT()

        static constexpr ArgKind Kind = ArgKind::Canvas;

        PROPERTY( DisplayName( "Scale Mode" ), Category( "UI Canvas" ) )
        UICanvasScaleMode ScaleMode = UICanvasScaleMode::Stretch;

        PROPERTY( DisplayName( "Render Mode" ), Category( "UI Canvas" ) )
        UICanvasRenderMode RenderMode = UICanvasRenderMode::ScreenSpace;

        PROPERTY( DisplayName( "World Scale" ), Category( "UI Canvas" ), Range( 1.0f, 4000.0f ) )
        float WorldScale = 400.0f; // WorldSpace: on-screen px per reference-unit at distance 1

        PROPERTY( DisplayName( "Reference Width" ), Category( "UI Canvas" ), Range( 64.0f, 7680.0f ) )
        float ReferenceWidth = 1280.0f;

        PROPERTY( DisplayName( "Reference Height" ), Category( "UI Canvas" ), Range( 64.0f, 4320.0f ) )
        float ReferenceHeight = 720.0f;

        PROPERTY( DisplayName( "Match Width/Height" ), Category( "UI Canvas" ), Range( 0.0f, 1.0f ) )
        float MatchWidthHeight = 0.5f; // ScaleWithScreen only: 0 = match width, 1 = match height

        // Asset<TextureAsset> is what names the asset TYPE to the serializer's resolver. Without it the
        // resolver gets an empty type string, falls through its table to the mesh lookup, and writes the
        // slot out as an EMPTY STRING — so every save cleared it.
        //
        // CORRECTED 2026-09-05: this comment used to say the annotation "is what makes this a HANDLE and
        // not a number", and that the field could not be authored without it. Neither is so.
        // DesertHeaderTool maps `Assets::AssetHandle` onto FieldType::AssetHandle by the TYPE's spelling
        // (main.cpp, MapFieldType), and the Details panel's texture picker is the default arm of that
        // field type — so the slot was always editable in the editor and always discarded on save. The
        // setting was dead in three places, but the third one is "nothing it was set to survived a save",
        // and the cause is the empty asset type, not the field type.
        PROPERTY( DisplayName( "Background Sprite" ), Category( "UI Canvas" ), Asset<TextureAsset> )
        Assets::AssetHandle Sprite; // drag a texture from the Content Browser; unset = transparent

        PROPERTY( DisplayName( "Visible" ), Category( "UI Canvas" ) )
        bool Visible = true;

        // WHICH CANVAS IS ON TOP, authored. A view draws EVERY canvas of its scene (Ю4), so with two of
        // them something has to decide the order — and "whichever entt hands out first" is a property of
        // the component pool, not a decision. Ascending: a higher Sort Order draws later, so it covers the
        // lower ones and takes the pointer from them. Equal values keep the scene file's own order.
        //
        // This is the knob the four overlay features are built on: a tooltip, a context menu, a modal and a
        // toast are each a canvas that must be above the HUD whatever order the level happened to create
        // them in. Read by UI::CanvasesInDrawOrder (UICanvasLayout.cpp).
        PROPERTY( DisplayName( "Sort Order" ), Category( "UI Canvas" ) )
        int SortOrder = 0;

        // Safe area (Phase B): per-edge insets (L/T/R/B, design px) the top-level content stays inside — for
        // mobile notches / rounded corners. On desktop set manually to preview a device; 0 = full canvas.
        PROPERTY( DisplayName( "Safe Area L/T/R/B" ), Category( "UI Canvas" ) )
        glm::vec4 SafeArea = glm::vec4( 0.0f );

        // --- Theme (Ю13) --------------------------------------------------------------------------------
        //
        // WHY THE THEME HANGS OFF THE CANVAS. A canvas is the root of one UI tree, and Ю4 already keyed
        // every piece of walk state by (canvas x view) precisely because a value owned by the PROCESS
        // cannot express two canvases or two viewports. A theme in a global would bring that back in the
        // place an author notices first: a HUD and a menu overlay in one scene could not have two looks,
        // and the UI Editor's preview could not show a theme the viewport is not on.
        //
        // WHY NOT PER ELEMENT. An element picks a STYLE (UIStyleComponent), which is a role INSIDE a
        // theme. Two themes inside one canvas is two palettes fighting on one screen, and the ancestor
        // walk it would cost is paid per element per frame for a capability a second canvas already
        // expresses — UICanvas has a Sort Order for exactly that.
        //
        // Empty is not an error and is the state of every scene authored before themes existed: each
        // element then draws the colours its author typed into it. Read by UICanvasRenderer2D.cpp.
        PROPERTY( DisplayName( "Theme" ), Category( "UI Theme" ), Asset<UIThemeAsset> )
        Assets::AssetHandle Theme;

        // ACCESSIBILITY, AND IT IS LIVE WITH OR WITHOUT A THEME. Multiplies every font size this canvas
        // draws at — the theme's, the element's own, and the auto-size floor — so raising it is one field
        // rather than an edit of every UIText in the scene. It is here and not in the theme because it is
        // the PLAYER's preference applied on top of whatever look the designer authored; a theme that
        // carried its own type scale would make "larger text" mean "a different theme".
        PROPERTY( DisplayName( "Font Scale" ), Category( "UI Theme" ), Range( 0.5f, 3.0f ) )
        float FontScale = 1.0f;

        // The other half of the accessibility pair: while this is on, a colour token the theme declares a
        // high-contrast value for resolves to that value instead. A theme that declares none is simply
        // unaffected, and a canvas with no theme is unaffected too — this switches a PALETTE, it does not
        // apply an algorithm to arbitrary colours, which is why it can never make a designed screen look
        // like something nobody drew. See Engine/Assets/UIThemeData.hpp for why the override table is
        // sparse rather than a second theme file.
        PROPERTY( DisplayName( "High Contrast" ), Category( "UI Theme" ) )
        bool HighContrast = false;
    };
} // namespace Desert::UI
