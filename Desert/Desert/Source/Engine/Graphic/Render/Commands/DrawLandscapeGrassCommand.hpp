#pragma once

#include "../RenderCommand.hpp"
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>

#include <memory>
#include <utility>

namespace Desert::Graphic::Render
{
    // Carries one landscape root's grass (its packed field and the varieties of its layers) from the ECS
    // (LandscapeECSSystem) to the GrassRenderer for this frame. Shared, never copied: the ECS builds the draw
    // only when the landscape, a grass type or a material changes, and writes this frame's wind into it in
    // place only while no command holds it (GRASS-EMIT copy on write), so the draw a command holds is frozen.
    struct DrawLandscapeGrassCommand : RenderCommand
    {
        std::shared_ptr<const System::GrassFieldDraw> Draw; // never null

        explicit DrawLandscapeGrassCommand( std::shared_ptr<const System::GrassFieldDraw> draw )
            : Draw( std::move( draw ) )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitLandscapeGrass( *Draw ); // by reference: read until EndScene; the command outlives it
        }
    };
} // namespace Desert::Graphic::Render
