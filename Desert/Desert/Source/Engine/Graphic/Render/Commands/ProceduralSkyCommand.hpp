#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Graphic/SunLightFx.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::Render
{
    // Carries the procedural-sky configuration from the ECS (SkyAtmosphereComponent + the atmosphere sun)
    // to the SkyboxRenderer. SunDir is the direction TOWARD the sun, already normalized — the engine's one
    // negation happened in ECS::Rules::AtmosphereSunDirection and must not happen again downstream.
    // Fx is the chosen sun light's render-effect slice (light shafts): it rides the
    // same command because those effects have no meaning apart from the sun that casts them.
    struct ProceduralSkyCommand : RenderCommand
    {
        bool        Enabled;
        glm::vec3   SunDir;
        bool        BakeNow; // one-shot: the editor's Bake button requested an IBL rebuild this frame
        SkySettings Sky;
        SunLightFx  Fx;
        // The second atmosphere light (the moon): scattered by the same sky, its own disc.
        AtmosphereLight1 Light1;

        ProceduralSkyCommand( bool enabled, const glm::vec3& sunDir, bool bakeNow, const SkySettings& sky,
                              const SunLightFx& fx, const AtmosphereLight1& light1 )
             : Enabled( enabled ), SunDir( sunDir ), BakeNow( bakeNow ), Sky( sky ), Fx( fx ), Light1( light1 )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SetProceduralSky( Enabled, SunDir, BakeNow, Sky, Fx, Light1 );
        }
    };
} // namespace Desert::Graphic::Render
