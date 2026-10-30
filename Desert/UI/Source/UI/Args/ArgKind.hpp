#pragma once

#include <cstddef>
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
        Tabs,
        TileView,
        TreeView,
        Count
    };

    // The number of argument kinds — the size of any table indexed by one.
    inline constexpr std::size_t kArgKindCount = static_cast<std::size_t>( ArgKind::Count );

    // The kinds a control writes its value back into, and no others (IUITree::FindState). One statement of
    // the set for every tree: the ECS adapter and the retained widget tree both answer FindState from it,
    // so a kind cannot be writable through one store and read-only through the other.
    [[nodiscard]] constexpr bool IsStateKind( ArgKind kind )
    {
        switch ( kind )
        {
            case ArgKind::Toggle:
            case ArgKind::Slider:
            case ArgKind::InputField:
            case ArgKind::Dropdown:
            case ArgKind::ScrollView:
            case ArgKind::ListView:
                return true;
            default:
                return false;
        }
    }
} // namespace Desert::UI
