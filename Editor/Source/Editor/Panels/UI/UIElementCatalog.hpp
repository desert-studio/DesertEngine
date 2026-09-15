#pragma once

#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <cstddef>

// THE list of UI element types the editor can place on a canvas.
//
// It exists because there were two of them. The UI Editor panel's toolbar and the viewport's "UI" create
// menu were written independently and drifted: the viewport offered UI Image and the panel did not, so
// which elements a scene could contain depended on which window the author happened to be in. Both are
// generated from this macro now, so the two menus cannot disagree again — adding an element is one line
// here and it appears in both.
//
// WHAT BELONGS IN THE LIST. An entry is an ELEMENT: a type the renderer draws as a child entity of the
// canvas in its own right — a visual (Panel, Text, Button, Image, Icon and the controls) or a container that
// positions its children (Layout Group, Scroll View). Image and Icon show nothing until an asset is bound to
// them, and that is authoring, not a defect: the entity exists, is selectable, and Details has the slot.
// Deliberately NOT entries:
//
//   * UICanvasComponent      - the root itself, created by its own "Create UI Canvas" action, and a second
//                              canvas would not be drawn at all (the renderer takes the first one).
//   * UILayoutComponent      - the rect; every element gets one automatically (see UIElementFactory).
//   * UIScreen/UIScreenStack - the screen machine. A screen with no NAME is skipped by the renderer's own
//                              seeding loop, and a stack belongs on the canvas rather than on a child, so a
//                              menu entry for either would produce something that cannot be drawn at all —
//                              which is a different thing from an element still waiting for its asset.
//   * UIAnim, UIBinding, UITween, UIDraggable, UIDropTarget, UIPointerEvents
//                            - MODIFIERS. They are added in Details to an element that already exists and
//                              draw nothing by themselves, ever.
//
// That partition is not a comment that can rot: the `UIElementCensus` suite enumerates the renderer's own
// dispatch and requires every UI component it handles to be either an entry below or one of the exclusions
// above, with the reason. A twenty-third component fails that test until somebody decides which it is.
//
// X( ComponentType, EntityName, Icon, MenuLabel )
//   ComponentType - the ECS component, in namespace Desert::ECS, added alongside a UILayoutComponent.
//   EntityName    - the scene-graph name the new entity gets.
//   Icon          - the toolbar/menu glyph.
//   MenuLabel     - the human label. The panel's toolbar prefixes it with "+ ".
#define DESERT_UI_ELEMENT_LIST( X )                                                                               \
    X( UIPanelComponent, "UI Panel", ICON_MDI_CARD_OUTLINE, "Panel" )                                             \
    X( UITextComponent2D, "UI Text", ICON_MDI_FORMAT_TEXT, "Text" )                                               \
    X( UIButtonComponent, "UI Button", ICON_MDI_BUTTON_POINTER, "Button" )                                        \
    X( UIImageComponent, "UI Image", ICON_MDI_IMAGE, "Image" )                                                    \
    X( UIIconComponent, "UI Icon", ICON_MDI_STAR_OUTLINE, "Icon" )                                                \
    X( UIRenderTextureComponent, "UI Render Texture", ICON_MDI_VIDEO_BOX, "Render Texture" )                      \
    X( UILayoutGroupComponent, "UI Layout Group", ICON_MDI_VIEW_GRID, "Layout Group" )                            \
    X( UIProgressBarComponent, "UI Progress Bar", ICON_MDI_PROGRESS_HELPER, "Progress Bar" )                      \
    X( UIToggleComponent, "UI Toggle", ICON_MDI_CHECKBOX_MARKED_OUTLINE, "Toggle" )                               \
    X( UISliderComponent, "UI Slider", ICON_MDI_TUNE_VARIANT, "Slider" )                                          \
    X( UIScrollViewComponent, "UI Scroll View", ICON_MDI_VIEW_LIST, "Scroll View" )                               \
    X( UIListViewComponent, "UI List View", ICON_MDI_FORMAT_LIST_BULLETED, "List View" )                          \
    X( UIInputFieldComponent, "UI Input Field", ICON_MDI_FORM_TEXTBOX, "Input Field" )                            \
    X( UIDropdownComponent, "UI Dropdown", ICON_MDI_MENU_DOWN, "Dropdown" )

namespace Desert::Editor
{
    // One row of the list above, as data. Deliberately free of ECS and of the renderer so the census test
    // can include this header on its own (EditorLayer.cpp and the panels are compiled by no suite).
    struct UIElementEntry
    {
        const char* ComponentType; // "UIPanelComponent" — matched against the renderer's own dispatch
        const char* EntityName;    // "UI Panel"
        const char* Icon;          // ICON_MDI_*
        const char* Label;         // "Panel"
    };

#define DESERT_UI_ELEMENT_ENTRY( Type, EntityName, Icon, Label ) UIElementEntry{ #Type, EntityName, Icon, Label },

    inline constexpr UIElementEntry kUIElements[] = { DESERT_UI_ELEMENT_LIST( DESERT_UI_ELEMENT_ENTRY ) };

#undef DESERT_UI_ELEMENT_ENTRY

    inline constexpr std::size_t kUIElementCount = sizeof( kUIElements ) / sizeof( kUIElements[0] );
} // namespace Desert::Editor
