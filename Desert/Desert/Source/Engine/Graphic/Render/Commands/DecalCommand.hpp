#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
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

        void Execute( SceneRenderer& renderer ) override
        {
            // COPIED, NOT MOVED: a scene with several views replays the same recorded command into each view's
            // renderer (SpotLightCommand.hpp says why a moved-from payload is a decal missing from one view).
            renderer.AddDecal( Decal );
        }
    };
} // namespace Desert::Graphic::Render
