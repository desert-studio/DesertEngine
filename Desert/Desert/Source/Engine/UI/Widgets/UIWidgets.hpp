#pragma once

// THE WIDGETS — one function per control, each drawing the ElementFrame DrawElement resolved for it.
// DrawElement is the one dispatch: it asks which component the element carries and calls exactly one of the
// control widgets (Button > Panel > ProgressBar > Path > Toggle > Slider > InputField > Dropdown), then the
// content widgets (Text, Icon, Image, RenderTexture) that compose with any of them, then DrawChildren.

#include <Engine/UI/UIWalkCtx.hpp>

namespace Desert::UI::Walk
{
    void DrawButtonWidget( ElementFrame& frame );        // Widgets/Button.cpp
    void DrawPanelWidget( ElementFrame& frame );         // Widgets/Panel.cpp
    void DrawProgressBarWidget( ElementFrame& frame );   // Widgets/ProgressBar.cpp
    void DrawPathWidget( ElementFrame& frame );          // Widgets/Path.cpp
    void DrawToggleWidget( ElementFrame& frame );        // Widgets/Toggle.cpp
    void DrawSliderWidget( ElementFrame& frame );        // Widgets/Slider.cpp
    void DrawInputFieldWidget( ElementFrame& frame );    // Widgets/InputField.cpp
    void DrawDropdownWidget( ElementFrame& frame );      // Widgets/Dropdown.cpp
    void DrawTextWidget( ElementFrame& frame );          // Widgets/Text.cpp
    void DrawIconWidget( ElementFrame& frame );          // Widgets/Text.cpp
    void DrawImageWidget( ElementFrame& frame );         // Widgets/Image.cpp
    void DrawRenderTextureWidget( ElementFrame& frame ); // Widgets/Image.cpp
    void DrawChildren( ElementFrame& frame );            // Widgets/ScrollList.cpp
} // namespace Desert::UI::Walk
