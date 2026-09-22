#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

namespace Desert::Graphic::Render
{
    struct SpotLightCommand : RenderCommand
    {
        ShaderProtocols::SpotLightPayload Light;

        SpotLightCommand( const ShaderProtocols::SpotLightPayload& light ) : Light( light )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            // COPIED, NOT MOVED, and that is a correctness fix rather than a style choice. A scene with
            // several views replays the SAME recorded command into each view's renderer (Scene::OnUpdate),
            // so a command that hands its payload away serves the first view and gives the second one a
            // moved-from value. For a light that is a light which exists in one viewport and not in the
            // other, with nothing in the log.
            renderer.AddSpotLight( Light );
        }
    };
} // namespace Desert::Graphic::Render
