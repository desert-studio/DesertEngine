#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>

namespace Desert::Graphic::Render
{
    struct SkyboxCommand : RenderCommand
    {
        std::shared_ptr<MaterialSkybox> Skybox;
        // The whole authored look of this scene's HDR sky (rotation, tint, intensity), carried as one
        // value. It used to be the brightness alone, and the brightness alone was all that ever reached
        // the renderer — see Engine/Graphic/Environment/SkyLook.hpp.
        SkyLook Look{};

        SkyboxCommand( const std::shared_ptr<MaterialSkybox>& skybox, const SkyLook& look )
             : Skybox( skybox ), Look( look )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SetEnvironment( Skybox, Look );
        }
    };
} // namespace Desert::Graphic::Render