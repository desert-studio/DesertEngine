#pragma once

#include "../RenderCommand.hpp"
#include <Engine/Graphic/Systems/Scene/Decals/DecalRenderer.hpp>

namespace Desert::Graphic::Render
{
    // One DecalComponent of this frame (ECS::DecalECSSystem) for "Deferred: Decals" (System::DecalRenderer).
    struct DecalCommand : RenderCommand
    {
        System::DecalProxy Decal;

        explicit DecalCommand( const System::DecalProxy& decal ) : Decal( decal )
        {
        }

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render
