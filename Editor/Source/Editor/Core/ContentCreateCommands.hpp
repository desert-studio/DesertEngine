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
    // UE's "Niagara Data Channel" under the FX category; the same one-spelling rule.
    inline constexpr std::string_view kNewVFXDataChannelLabel = "New VFX Data Channel";
    // UE's Add > Input > "Input Action" / "Input Mapping Context", the same one-spelling rule.
    inline constexpr std::string_view kNewInputActionLabel         = "New Input Action";
    inline constexpr std::string_view kNewInputMappingContextLabel = "New Input Mapping Context";

    // "Assets / New Level Sequence". The creation itself is FileExplorerPanel::CreateNewLevelSequence and
    // nothing here: the entry is handed that one route and answers with its outcome, so the palette, the
    // context menu and a control-socket `run` create the same file the same way. No ImGui and no panel
    // here, which is what lets Tests/Editor/PaletteCommands pin the wiring.
    [[nodiscard]] inline std::vector<PaletteCommand>
    ContentCreatePaletteCommands( std::function<Common::BoolResultStr()> newLevelSequence,
                                  std::function<Common::BoolResultStr()> newVFXDataChannel,
                                  std::function<Common::BoolResultStr()> newInputAction,
                                  std::function<Common::BoolResultStr()> newInputMappingContext )
    {
        std::vector<PaletteCommand> commands;
        commands.push_back( { "Assets", std::string( kNewLevelSequenceLabel ), std::move( newLevelSequence ) } );
        commands.push_back( { "Assets", std::string( kNewVFXDataChannelLabel ), std::move( newVFXDataChannel ) } );
        commands.push_back( { "Assets", std::string( kNewInputActionLabel ), std::move( newInputAction ) } );
        commands.push_back(
             { "Assets", std::string( kNewInputMappingContextLabel ), std::move( newInputMappingContext ) } );
        return commands;
    }
} // namespace Desert::Editor
