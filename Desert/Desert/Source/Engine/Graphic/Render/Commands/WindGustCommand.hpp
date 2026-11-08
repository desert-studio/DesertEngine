#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: Execute calls a method on it, and RenderCommand.hpp only forward-declares the type.
#include <Engine/Graphic/SceneRenderer.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::Render
{
    // Carries this gameplay step's gust fronts from the ECS to the renderer (WIND-FIELD-GPU; Graphic::PackWindGust,
    // Common/WindGust.glslh). The fronts' offset is accumulated by MeshECSSystem, which owns the foliage wind's
    // clock and the timestep, so the gusts and the sway advance by the same step; the previous step's fronts ride
    // along for a view pass's previous position (CameraUB::PrevWindGust).
    struct WindGustCommand : RenderCommand
    {
        glm::vec4 Gust;
        glm::vec4 PrevGust;

        WindGustCommand( const glm::vec4& gust, const glm::vec4& prevGust ) : Gust( gust ), PrevGust( prevGust )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SetWindGust( Gust, PrevGust );
        }
    };
} // namespace Desert::Graphic::Render
