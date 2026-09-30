#pragma once

#include <Editor/Core/UICommandInfo.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Desert::Editor
{
    /**
     * @brief UE's FContentBrowserCommands / Asset Actions: what the Content Browser does to its SELECTION.
     *        The item context menu draws these (FileExplorerPanel::CommandMenuItem) and the palette offers
     *        them (group "Content Browser"); both call FileExplorerPanel::RunCommand. A right click selects
     *        the clicked entry first (UE), so the menu and the palette act on the same thing.
     */
    enum class ContentBrowserCommand : std::uint8_t
    {
        Open,
        ShowInExplorer,
        OpenContainingFolder,
        Reimport,
        ReimportWithNewFile,
        CaptureThumbnail,
        EditThumbnail,
        ClearSelection,
    };

    inline constexpr std::string_view kContentBrowserContext = "Content Browser";

    // Indexed by ContentBrowserCommand; kContentBrowserCommandOrder is the same order, checked below.
    inline constexpr std::array<UICommandInfo, 8> kContentBrowserCommandInfos{ {
         { kContentBrowserContext, "Open", "" },
         { kContentBrowserContext, "Show in Explorer", "" },
         { kContentBrowserContext, "Open Containing Folder", "" },
         { kContentBrowserContext, "Reimport", "" },
         { kContentBrowserContext, "Reimport with New File...", "" },
         { kContentBrowserContext, "Capture Thumbnail (from viewport)", "" },
         { kContentBrowserContext, "Edit Thumbnail", "drag / wheel, Esc" },
         { kContentBrowserContext, "Clear Selection", "" },
    } };

    inline constexpr std::array<ContentBrowserCommand, 8> kContentBrowserCommandOrder{
         ContentBrowserCommand::Open,
         ContentBrowserCommand::ShowInExplorer,
         ContentBrowserCommand::OpenContainingFolder,
         ContentBrowserCommand::Reimport,
         ContentBrowserCommand::ReimportWithNewFile,
         ContentBrowserCommand::CaptureThumbnail,
         ContentBrowserCommand::EditThumbnail,
         ContentBrowserCommand::ClearSelection,
    };

    [[nodiscard]] constexpr const UICommandInfo& CommandInfo( ContentBrowserCommand command )
    {
        return kContentBrowserCommandInfos[static_cast<std::size_t>( command )];
    }

    static_assert(
         []
         {
             for ( std::size_t i = 0; i < kContentBrowserCommandOrder.size(); ++i )
                 if ( static_cast<std::size_t>( kContentBrowserCommandOrder[i] ) != i )
                     return false;
             return true;
         }(),
         "kContentBrowserCommandOrder must list every command in enum order (it indexes the infos)" );
} // namespace Desert::Editor
