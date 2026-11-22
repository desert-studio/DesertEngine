#pragma once

#include "../RenderCommand.hpp"
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>

namespace Desert::Graphic
{
    class MaterialSkybox;
} // namespace Desert::Graphic

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

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render