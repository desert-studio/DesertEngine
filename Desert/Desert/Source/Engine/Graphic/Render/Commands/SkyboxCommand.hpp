#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>

namespace Desert::Graphic::Render
{
    struct SkyboxCommand : RenderCommand
    {
        std::shared_ptr<MaterialSkybox> Skybox;
        float                           Intensity = 1.0f; // HDR skybox brightness (SkyboxComponent::Intensity)

        SkyboxCommand( const std::shared_ptr<MaterialSkybox>& skybox, float intensity )
             : Skybox( skybox ), Intensity( intensity )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SetEnvironment( Skybox, Intensity );
        }
    };
} // namespace Desert::Graphic::Render