#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>

namespace Desert::Graphic::Render
{
    struct PointLightCommand : RenderCommand
    {
        ShaderProtocols::PointLightPayload Light;

        PointLightCommand( const ShaderProtocols::PointLightPayload& light ) : Light( light )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.AddPointLight( std::move( Light ) );
        }
    };
} // namespace Desert::Graphic::Render