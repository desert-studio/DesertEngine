#pragma once

#include "../RenderCommand.hpp"

#include <Engine/Graphic/Fog/HeterogeneousVolumePayload.hpp>

namespace Desert::Graphic::Render
{
    // One heterogeneous volume of this frame, already packed (Graphic::PackHeterogeneousVolume), from the ECS to
    // the renderer. ONE COMMAND PER VOLUME, like the local fog volumes: the command lives in the command buffer's
    // arena, and the renderer's list (reserved once) is emptied with the lights' when the frame ends. Whether the
    // volume is on THIS view's screen is the renderer's question (HeterogeneousVolumeRenderer culls per view).
    struct HeterogeneousVolumeCommand : RenderCommand
    {
        HeterogeneousVolumeInstance Volume;

        explicit HeterogeneousVolumeCommand( const HeterogeneousVolumeInstance& volume ) : Volume( volume )
        {
        }

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render
