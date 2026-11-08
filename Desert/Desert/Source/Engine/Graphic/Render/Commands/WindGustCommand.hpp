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
    // along for a view pass's previous position (CameraUB::PrevWindGust). It also carries the directional wind's
    // horizontal speed (the local sources' reference speed) and opens the step's local source list.
    struct WindGustCommand : RenderCommand
    {
        glm::vec4 Gust;
        glm::vec4 PrevGust;
        float     DirectionalSpeed;

        WindGustCommand( const glm::vec4& gust, const glm::vec4& prevGust, const float directionalSpeed )
             : Gust( gust ), PrevGust( prevGust ), DirectionalSpeed( directionalSpeed )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SetWindGust( Gust, PrevGust, DirectionalSpeed );
        }
    };

    // One point (local) wind source of this gameplay step, sent after the step's WindGustCommand by the same
    // system (MeshECSSystem): a candidate for the camera block's local sources (Graphic::SelectLocalWind).
    struct LocalWindSourceCommand : RenderCommand
    {
        LocalWindCandidate Source;

        explicit LocalWindSourceCommand( const LocalWindCandidate& source ) : Source( source )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.AddLocalWindSource( Source );
        }
    };
} // namespace Desert::Graphic::Render
