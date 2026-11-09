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
    // horizontal velocity (cm/s; its length is the local sources' reference speed, and the particle simulation
    // reads it as the air, VFX_SceneWind) and opens the step's local source list.
    struct WindGustCommand : RenderCommand
    {
        glm::vec4 Gust;
        glm::vec4 PrevGust;
        glm::vec2 DirectionalVelocityXZ;

        WindGustCommand( const glm::vec4& gust, const glm::vec4& prevGust, const glm::vec2& directionalVelocityXZ )
             : Gust( gust ), PrevGust( prevGust ), DirectionalVelocityXZ( directionalVelocityXZ )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SetWindGust( Gust, PrevGust, DirectionalVelocityXZ );
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
