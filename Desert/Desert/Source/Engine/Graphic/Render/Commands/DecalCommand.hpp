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

    // This frame's live impact decals (World::DecalImpactPool via ECS::DecalECSSystem): one command for the whole
    // pool, not one per record. Replayed into every view, so it is copied, never moved from.
    struct DecalImpactsCommand : RenderCommand
    {
        std::vector<System::DecalProxy> Decals;

        explicit DecalImpactsCommand( std::vector<System::DecalProxy> decals ) : Decals( std::move( decals ) )
        {
        }

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render
