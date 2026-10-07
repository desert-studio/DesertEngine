#pragma once

#include <Editor/Core/CommandPalette.hpp>
#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    // The words of UE's "Add Level Sequence", one spelling for the Assets window's context menu and the
    // palette entry below: two literals would drift, and the palette is also the control channel's
    // vocabulary.
    inline constexpr std::string_view kNewLevelSequenceLabel = "New Level Sequence";

    // "Assets / New Level Sequence". The creation itself is FileExplorerPanel::CreateNewLevelSequence and
    // nothing here: the entry is handed that one route and answers with its outcome, so the palette, the
    // context menu and a control-socket `run` create the same file the same way. No ImGui and no panel
    // here, which is what lets Tests/Editor/PaletteCommands pin the wiring.
    [[nodiscard]] inline std::vector<PaletteCommand>
    ContentCreatePaletteCommands( std::function<Common::BoolResultStr()> newLevelSequence )
    {
        std::vector<PaletteCommand> commands;
        commands.push_back( { "Assets", std::string( kNewLevelSequenceLabel ), std::move( newLevelSequence ) } );
        return commands;
    }
} // namespace Desert::Editor
