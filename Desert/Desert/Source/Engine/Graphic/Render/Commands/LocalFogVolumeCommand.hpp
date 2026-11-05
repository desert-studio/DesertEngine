#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>

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

        void Execute( SceneRenderer& renderer ) override
        {
            // Copied, not moved: the same recorded command is replayed into every view's renderer.
            renderer.AddLocalFogVolume( Volume );
        }
    };
} // namespace Desert::Graphic::Render
