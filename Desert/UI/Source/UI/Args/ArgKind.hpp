#pragma once

#include <cstdint>

namespace Desert::UI
{
    // Which authored argument a UI*Data is. Each *Data in UI/Args names its own as `Arg`, so code
    // that is handed "the arguments of a node" can ask for one by kind without knowing who stores them
    // (today the ECS wraps every *Data in a UI*Component; the tree interface asks through this key).
    enum class ArgKind : uint8_t
    {
        Layout,
        LayoutGroup,
        Canvas,
        Panel,
        Text,
        Icon,
        Image,
        RenderTexture,
        Button,
        Toggle,
        Slider,
        InputField,
        Dropdown,
        ScrollView,
        ListView,
        ProgressBar,
        Path,
        Retainer,
        Style,
        Tween,
        Binding,
        Screen,
        ScreenStack,
        PointerEvents,
        Draggable,
        DropTarget,
        Overlay,
        OverlayTrigger,
        Navigation,
        Count
    };
} // namespace Desert::UI
