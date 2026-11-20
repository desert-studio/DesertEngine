#pragma once

#include "../RenderCommand.hpp"

// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
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

        void Execute( SceneRenderer& renderer ) override
        {
            // Copied, not moved: the same recorded command is replayed into every view's renderer.
            renderer.AddHeterogeneousVolume( Volume );
        }
    };
} // namespace Desert::Graphic::Render
