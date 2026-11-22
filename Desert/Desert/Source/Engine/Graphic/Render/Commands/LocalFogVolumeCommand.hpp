#pragma once

#include "../RenderCommand.hpp"

#include <Engine/Graphic/Fog/VolumetricFogPayload.hpp>

namespace Desert::Graphic::Render
{
    // One local fog volume of this frame, already packed for the froxels (Graphic::PackLocalFogVolume), from
    // the ECS to the renderer. ONE COMMAND PER VOLUME, like the local lights: the command lives in the command
    // buffer's arena, so a frame's volumes cost no heap block of their own, and the renderer's list (reserved
    // once) is emptied with the lights' when the frame ends.
    struct LocalFogVolumeCommand : RenderCommand
    {
        LocalFogVolumeGpu Volume;

        explicit LocalFogVolumeCommand( const LocalFogVolumeGpu& volume ) : Volume( volume )
        {
        }

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render
