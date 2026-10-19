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
        // The empty environment: the engine's black cube (UE GBlackTextureCube), sampled where a scene has no
        // baked sky - the split-sum ambient reads zero.
        TextureRef BlackCube;
        // 1x1 RG32_UINT zero (UE GSystemTextures.ZeroUIntDummy): what an integer sampler (usampler2D) is handed
        // where its producer did not run - a float image there is a format mismatch, not a neutral input.
        // Registered by the frame setup (FrameTextures) from FallbackTextures; invalid in a graph without it.
        TextureRef ZeroUint;
        // 1x1x1 RGBA8 all zero (UE GSystemTextures.VolumetricBlackDummy): what a sampler3D of light is handed
        // where its producer did not run (FrameTransients::MegaLightsVolume off). Registered by the frame setup
        // from FallbackTextures::GetBlackTexture3D; invalid in a graph without it.
        TextureRef Black3D;
    };

    // Registers @p black, @p white and @p blackCube (externals already holding the engine images and their
    // recorded layouts) in @p graph under the names "System.Black" / "System.White" / "System.BlackCube".
    inline SystemTextures RegisterSystemTextures( Builder& graph, ExternalTexture& black, ExternalTexture& white,
                                                  ExternalTexture& blackCube )
    {
        const SystemTextures system{ graph.RegisterExternal( black, "System.Black" ),
                                     graph.RegisterExternal( white, "System.White" ),
                                     graph.RegisterExternal( blackCube, "System.BlackCube" ) };
        // RDG-FAULT1: the images a FaultDefault names, so every graph with system textures can honour one.
        graph.GetFaultDefaults().SetSources( system.Black, system.White, system.BlackCube );
        return system;
    }
} // namespace Desert::Graphic::RDG
