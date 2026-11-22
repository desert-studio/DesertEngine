#pragma once

#include "../RenderCommand.hpp"
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/LocalShadows.hpp>
#include <Engine/Graphic/Systems/Scene/Lights/LightFunctionProxy.hpp>

namespace Desert::Graphic::Render
{
    struct SpotLightCommand : RenderCommand
    {
        ShaderProtocols::SpotLightPayload Light;
        LocalLightShadowSettings          Shadow;
        System::LightFunctionProxy        Function;

        SpotLightCommand( const ShaderProtocols::SpotLightPayload& light, const LocalLightShadowSettings& shadow,
                          const System::LightFunctionProxy& function )
             : Light( light ), Shadow( shadow ), Function( function )
        {
        }

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render
