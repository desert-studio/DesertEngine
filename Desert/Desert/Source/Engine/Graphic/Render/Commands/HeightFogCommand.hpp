#pragma once

#include "../RenderCommand.hpp"

#include <Engine/ECS/ExponentialHeightFogComponent.hpp>

namespace Desert::Graphic::Render
{
    // Carries this frame's height-fog settings from the ECS to the fog renderer.
    //
    // `Present` is said explicitly instead of being implied by the command's absence: the renderer
    // keeps its settings across frames, so a scene whose fog component was deleted would otherwise go
    // on fogging the last medium it was told about.
    //
    // `FogHeightY` rides here because the fog floor is the fog ENTITY's transform Y (UE takes it from
    // the component transform the same way); the component itself deliberately has no height field.
    struct HeightFogCommand : RenderCommand
    {
        bool                          Present;
        ECS::ExponentialHeightFogData Data;
        float                         FogHeightY;

        HeightFogCommand( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY )
             : Present( present ), Data( data ), FogHeightY( fogHeightY )
        {
        }

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render
