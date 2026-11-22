#pragma once

#include "../RenderCommand.hpp"
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/LocalShadows.hpp>
#include <Engine/Graphic/Systems/Scene/Lights/LightFunctionProxy.hpp>

namespace Desert::Graphic::Render
{
    struct PointLightCommand : RenderCommand
    {
        ShaderProtocols::PointLightPayload Light;
        LocalLightShadowSettings           Shadow;
        System::LightFunctionProxy         Function;

        PointLightCommand( const ShaderProtocols::PointLightPayload& light,
                           const LocalLightShadowSettings&           shadow,
                           const System::LightFunctionProxy&         function )
             : Light( light ), Shadow( shadow ), Function( function )
        {
        }

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render