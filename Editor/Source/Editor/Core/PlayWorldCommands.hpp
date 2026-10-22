#pragma once

#include <Editor/Core/UICommandInfo.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Desert::Editor
{
    /**
     * @brief UE's FPlayWorldCommands: the world's play session as commands — Play, Play from Here, Simulate,
     *        Pause, Resume, Frame Skip ("Next Frame") and Stop. The toolbar's playback group, its Play-options
     * menu and the palette / control channel (group "Action", so `desertctl run Action "Pause"`) all call
     *        PlaySession::Run; no surface keeps a second copy of a body.
     *
     *        Pause and Resume are two commands rather than one toggle (UE: PausePlaySession /
     *        ResumePlaySession), so a script that asks to pause is told when nothing was running instead of
     *        resuming a world it meant to stop. The toolbar's single Pause/Resume slot picks one of the two
     *        by the scene state.
     */
    enum class PlayWorldCommand : std::uint8_t
    {
        Play,
        PlayFromHere,
        // UE's Simulate In Editor (FPlayWorldCommands::Simulate): the world ticks - physics, scripts, game
        // rules - but no player pawn is spawned or possessed; the editor camera and the editor's overlays stay.
        // Stop ends it exactly as it ends Play.
        Simulate,
        Pause,
        Resume,
        NextFrame,
        Stop,
    };

    inline constexpr std::string_view kPlayWorldContext = "Action";

    inline constexpr std::array<UICommandInfo, 7> kPlayWorldCommandInfos{ {
         { kPlayWorldContext, "Play", "" },
         { kPlayWorldContext, "Play from Here", "" },
         { kPlayWorldContext, "Simulate", "" },
         { kPlayWorldContext, "Pause", "" },
         { kPlayWorldContext, "Resume", "" },
         { kPlayWorldContext, "Next Frame", "" },
         { kPlayWorldContext, "Stop", "" },
    } };

    inline constexpr std::array<PlayWorldCommand, 7> kPlayWorldCommandOrder{
         PlayWorldCommand::Play,  PlayWorldCommand::PlayFromHere, PlayWorldCommand::Simulate,
         PlayWorldCommand::Pause, PlayWorldCommand::Resume,       PlayWorldCommand::NextFrame,
         PlayWorldCommand::Stop };

    [[nodiscard]] constexpr const UICommandInfo& CommandInfo( PlayWorldCommand command )
    {
        return kPlayWorldCommandInfos[static_cast<std::size_t>( command )];
    }
} // namespace Desert::Editor
