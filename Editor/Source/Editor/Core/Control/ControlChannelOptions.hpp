#pragma once

#include <string>

namespace Desert::Editor::Control
{
    /**
     * @brief Whether this editor listens, and where. Published before the renderer exists, exactly as
     *        ShotOptions is, so the answer is settled for the very first frame.
     *
     * A SEPARATE HOME FROM ShotOptions on purpose. The two look alike — both are command-line state held
     * process-wide — and they are opposites in the one way that matters: ShotOptions describes a run that
     * nobody is driving, and this describes a run that somebody is. Folding the second into the first
     * would put "is anyone connected" inside the type whose whole job is to answer "is anyone absent".
     *
     * EMPTY IS THE DEFAULT AND MEANS SILENT. An editor that listened without being asked would be a hole:
     * the channel runs every entry the command palette offers, and that includes writing over the scene
     * on disk. It exists only when a person named a socket path on the command line.
     */
    struct ControlChannelOptions
    {
        static ControlChannelOptions& Get();

        /// `--control-socket <path>`. Empty = this editor listens for nothing at all.
        std::string SocketPath;

        [[nodiscard]] bool Requested() const noexcept
        {
            return !SocketPath.empty();
        }
    };
} // namespace Desert::Editor::Control
