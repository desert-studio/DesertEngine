#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: Execute calls a method on it, and RenderCommand.hpp only forward-declares the type.
#include <Engine/Graphic/SceneRenderer.hpp>

#include <utility>

namespace Desert::Graphic::Render
{
    // Carries one water zone of this frame (its quadtree description and the water bodies' footprints inside it)
    // from ECS::WaterZoneECSSystem to the water renderer. One command per zone; none = no water drawn.
    struct WaterZoneCommand : RenderCommand
    {
        System::WaterZoneDraw Zone;

        explicit WaterZoneCommand( System::WaterZoneDraw zone ) : Zone( std::move( zone ) )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitWaterZone( std::move( Zone ) );
        }
    };
} // namespace Desert::Graphic::Render
