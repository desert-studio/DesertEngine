#pragma once

#include "../RenderCommand.hpp"
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>

#include <utility>

namespace Desert::Graphic::Render
{
    // Carries one landscape root's grass (its packed field and the varieties of its layers) from the ECS
    // (LandscapeECSSystem) to the GrassRenderer for this frame. The field is shared, rebuilt only when the
    // landscape's heights or weights change.
    struct DrawLandscapeGrassCommand : RenderCommand
    {
        System::GrassFieldDraw Draw;

        explicit DrawLandscapeGrassCommand( System::GrassFieldDraw draw ) : Draw( std::move( draw ) )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitLandscapeGrass( std::move( Draw ) );
        }
    };
} // namespace Desert::Graphic::Render
