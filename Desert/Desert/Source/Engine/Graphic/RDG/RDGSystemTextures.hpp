#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

namespace Desert::Graphic::RDG
{
    // RDG-A2. The engine's constant textures as graph resources of ONE graph (UE: FRDGSystemTextures). The frame
    // setup registers them once per graph, before any pass is added, and hands the refs to every AddFrame*
    // (FrameTextures::System). A pass that needs a neutral input - the black texture where an optional producer
    // did not run this frame - declares a read of the system ref and binds it by shader name through
    // PassBindings like any other graph texture. No pass imports an engine default texture itself, and no
    // backend substitutes one for an unbound slot: the choice is visible in the pass's declarations.
    // The refs die with the graph; the images behind them are the engine's (DefaultTextures), not the graph's.
    struct SystemTextures
    {
        TextureRef Black; // 1x1 opaque black (DefaultTextureKind::Black)
        TextureRef White; // 1x1 white (DefaultTextureKind::White)
    };

    // Registers @p black and @p white (externals already holding the engine images and their recorded layouts)
    // in @p graph under the names "System.Black" / "System.White".
    inline SystemTextures RegisterSystemTextures( Builder& graph, ExternalTexture& black, ExternalTexture& white )
    {
        return { graph.RegisterExternal( black, "System.Black" ),
                 graph.RegisterExternal( white, "System.White" ) };
    }
} // namespace Desert::Graphic::RDG
