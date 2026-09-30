#pragma once

#include <Common/Core/KeyCodes.hpp>
#include <Editor/Core/UICommandInfo.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Desert::Editor
{
    /**
     * @brief UE's FLevelViewportCommands subset the editor had only as keys: frame the selection (F) and
     *        drop it (Esc, the second press). The keys and the palette (group "Level Viewport") both call
     *        ViewportPanel::RunCommand on the viewport the user works in (ViewportPanel::RequestCommand).
     */
    enum class ViewportCommand : std::uint8_t
    {
        FocusSelected,
        SelectNone,
    };

    inline constexpr std::string_view kViewportContext = "Level Viewport";

    inline constexpr std::array<UICommandInfo, 2> kViewportCommandInfos{ {
         { kViewportContext, "Focus Selected", "F" },
         { kViewportContext, "Select None", "Esc" },
    } };

    inline constexpr std::array<ViewportCommand, 2> kViewportCommandOrder{ ViewportCommand::FocusSelected,
                                                                           ViewportCommand::SelectNone };

    [[nodiscard]] constexpr const UICommandInfo& CommandInfo( ViewportCommand command )
    {
        return kViewportCommandInfos[static_cast<std::size_t>( command )];
    }

    struct ViewportCommandKey
    {
        Common::KeyCode Key;
        ViewportCommand Command;
    };

    inline constexpr std::array<ViewportCommandKey, 2> kViewportCommandKeys{ {
         { Common::KeyCode::F, ViewportCommand::FocusSelected },
         { Common::KeyCode::Escape, ViewportCommand::SelectNone },
    } };

    [[nodiscard]] constexpr std::optional<ViewportCommand> ViewportCommandForKey( Common::KeyCode key )
    {
        for ( const ViewportCommandKey& binding : kViewportCommandKeys )
            if ( binding.Key == key )
                return binding.Command;
        return std::nullopt;
    }
} // namespace Desert::Editor
