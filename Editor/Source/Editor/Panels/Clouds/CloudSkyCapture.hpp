#pragma once

// "CAPTURE CURRENT SKY" (CLOUD-OFFSET-BAKE, owner 10-19): fold the drift game time has given a cloud layer
// into its authored Wind Offset, so the sky being looked at is the one the scene opens on.
//
// ONE FUNCTION, TWO DOORS: the Details button on the layer and the palette's Clouds entry both call it, so
// a client on the control channel captures exactly what the button captures.
//
// IN PLAY THE CAPTURE IS KEPT PAST STOP (PlayKeptChanges, UE's "Keep Simulation Changes"): the drift only
// exists while game time runs, and Stop's restore would otherwise throw the captured value away with every
// other play-time edit.

#include <Common/Core/ResultStr.hpp>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::ECS
{
    class Entity;
}

namespace Desert::Editor
{
    /// Wind Offset += the layer's drift, and the drift back to zero, as one undoable edit — the drawn sky
    /// does not move. Refused, with the entity named, when `layer` has no Volumetric Cloud component or
    /// when there is no drift to capture (the editor's sky stands still outside Play).
    [[nodiscard]] Common::BoolResultStr CaptureCurrentSky( const ::Desert::Core::Scene& scene,
                                                           ::Desert::ECS::Entity&       layer );
} // namespace Desert::Editor
